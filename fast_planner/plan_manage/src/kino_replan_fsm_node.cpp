/**
* This file is part of Fast-Planner-ROS2.
*
* Copyright 2024 Guanrui Li, Aerial-robot Control and Perception Lab, WPI
* Developed by Guanrui Li <lguanrui.github@gmail.com>
* for more information see <https://github.com/lguanrui/Fast-Planner-ROS2.git>.
* If you use this code, please cite the respective publications as
* listed on the above website.
*
* Fast-Planner is free software: you can redistribute it and/or modify
* it under the terms of the GNU Lesser General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* Fast-Planner is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU Lesser General Public License
* along with Fast-Planner. If not, see <http://www.gnu.org/licenses/>.
*/

#include <Eigen/Eigen>
#include <algorithm>
#include <iostream>
#include <nav_msgs/msg/path.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <rclcpp/rclcpp.hpp>
#include <std_msgs/msg/empty.hpp>
#include <vector>
#include <visualization_msgs/msg/marker.hpp>
#include <message_filters/subscriber.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <message_filters/time_synchronizer.h>

#include <bspline_opt/bspline_optimizer.h>
#include <path_searching/kinodynamic_astar.h>
#include <plan_env/edt_environment.h>
#include <plan_env/obj_predictor.h>
#include <plan_env/sdf_map.h>
#include <plan_manage/Bspline.h>
#include <plan_manage/planner_manager.h>
#include <traj_utils/planning_visualization.h>

namespace fast_planner {

class KinoReplanFSM : public rclcpp::Node {
public:
  KinoReplanFSM(const rclcpp::NodeOptions &options)
      : Node("kino_replan_fsm", options),
        trigger_(false),
        have_target_(false),
        have_odom_(false),
        exec_state_(INIT),
        current_wp_(0) {
    init();
  }

  // Need this since we have Eigen members
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW;

private:
  /* ---------- flag ---------- */
  enum FSM_EXEC_STATE { INIT, WAIT_TARGET, GEN_NEW_TRAJ, REPLAN_TRAJ, EXEC_TRAJ, REPLAN_NEW };
  enum TARGET_TYPE { MANUAL_TARGET = 1, PRESET_TARGET = 2, REFENCE_PATH = 3 };

  /* planning utils */
  FastPlannerManager::Ptr planner_manager_;
  PlanningVisualization::Ptr visualization_;

  /* parameters */
  int target_type_;  // 1 mannual select, 2 hard code
  double no_replan_thresh_, replan_thresh_;
  double waypoints_[50][3];
  int waypoint_num_;

  /* planning data */
  bool trigger_, have_target_, have_odom_;
  FSM_EXEC_STATE exec_state_;

  Eigen::Vector3d odom_pos_, odom_vel_;  // odometry state
  Eigen::Quaterniond odom_orient_;

  Eigen::Vector3d start_pt_, start_vel_, start_acc_, start_yaw_;  // start state
  Eigen::Vector3d end_pt_, end_vel_;                              // target state
  int current_wp_;

  /* ROS2 utils */
  typedef message_filters::sync_policies::ApproximateTime<sensor_msgs::msg::Image, nav_msgs::msg::Odometry>
      SyncPolicyImageOdom;
  //typedef std::shared_ptr<message_filters::Synchronizer<SyncPolicyImageOdom>> SynchronizerImageOdom;

  rclcpp::TimerBase::SharedPtr occ_timer_, esdf_timer_, vis_timer_;
  rclcpp::TimerBase::SharedPtr exec_timer_, safety_timer_;

  rclcpp::Subscription<sensor_msgs::msg::PointCloud2>::SharedPtr indep_cloud_sub_;
  rclcpp::Subscription<nav_msgs::msg::Path>::SharedPtr waypoint_sub_;
  rclcpp::Subscription<nav_msgs::msg::Odometry>::SharedPtr odom_sub_;

  rclcpp::Publisher<std_msgs::msg::Empty>::SharedPtr replan_pub_;
  rclcpp::Publisher<visualization_msgs::msg::Marker>::SharedPtr bspline_pub_;
  rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr map_pub_, esdf_pub_, map_inf_pub_, update_range_pub_;
  rclcpp::Publisher<sensor_msgs::msg::PointCloud2>::SharedPtr unknown_pub_, depth_pub_;

  /* helper functions */
  void init(); // initialize
  bool callKinodynamicReplan();        // front-end and back-end method
  bool callTopologicalTraj(int step);  // topo path guided gradient-based
                                       // optimization; 1: new, 2: replan
  void changeFSMExecState(FSM_EXEC_STATE new_state, std::string pos_call);
  void printFSMExecState();

  /* ROS2 functions */
  void execFSMCallback();
  void checkCollisionCallback();
  void waypointCallback(const nav_msgs::msg::Path::SharedPtr msg);
  void odometryCallback(const nav_msgs::msg::Odometry::SharedPtr msg);

};

void KinoReplanFSM::init() {

  current_wp_  = 0;
  exec_state_  = FSM_EXEC_STATE::INIT;
  have_target_ = false;
  have_odom_   = false;
  PlanParameters pp;

  /*  fsm param  */
  this->declare_parameter("fsm.flight_type", -1);
  this->declare_parameter("fsm.thresh_replan", -1.0);
  this->declare_parameter("fsm.thresh_no_replan", -1.0);
  this->declare_parameter("fsm.waypoint_num", -1);

  this->get_parameter("fsm.flight_type", target_type_);
  this->get_parameter("fsm.thresh_replan", replan_thresh_);
  this->get_parameter("fsm.thresh_no_replan", no_replan_thresh_);
  this->get_parameter("fsm.waypoint_num", waypoint_num_);

  for (int i = 0; i < waypoint_num_; i++) {
    this->declare_parameter("fsm.waypoint" + std::to_string(i) + "_x", -1.0);
    this->declare_parameter("fsm.waypoint" + std::to_string(i) + "_y", -1.0);
    this->declare_parameter("fsm.waypoint" + std::to_string(i) + "_z", -1.0);

    this->get_parameter("fsm.waypoint" + std::to_string(i) + "_x", waypoints_[i][0]);
    this->get_parameter("fsm.waypoint" + std::to_string(i) + "_y", waypoints_[i][1]);
    this->get_parameter("fsm.waypoint" + std::to_string(i) + "_z", waypoints_[i][2]);
  }

  /* planner manager params */
  this->declare_parameter("manager.max_vel", -1.0);
  this->declare_parameter("manager.max_acc", -1.0);
  this->declare_parameter("manager.max_jerk", -1.0);
  this->declare_parameter("manager.dynamic_environment", -1);
  this->declare_parameter("manager.clearance_threshold", -1.0);
  this->declare_parameter("manager.local_segment_length", -1.0);
  this->declare_parameter("manager.control_points_distance", -1.0);

  this->get_parameter("manager.max_vel", pp.max_vel_);
  this->get_parameter("manager.max_acc", pp.max_acc_);
  this->get_parameter("manager.max_jerk", pp.max_jerk_);
  this->get_parameter("manager.dynamic_environment", pp.dynamic_);
  this->get_parameter("manager.clearance_threshold", pp.clearance_);
  this->get_parameter("manager.local_segment_length", pp.local_traj_len_);
  this->get_parameter("manager.control_points_distance", pp.ctrl_pt_dist);

  bool use_geometric_path, use_kinodynamic_path, use_topo_path, use_optimization, use_active_perception;

  use_geometric_path = false;
  use_kinodynamic_path = true;
  use_topo_path = false;
  use_optimization = true;

  /* get sdf_map params*/
  MappingParameters mp;
  double x_size, y_size, z_size;
  this->declare_parameter("sdf_map/resolution", -1.0);
  this->declare_parameter("sdf_map/map_size_x", -1.0);
  this->declare_parameter("sdf_map/map_size_y", -1.0);
  this->declare_parameter("sdf_map/map_size_z", -1.0);
  this->declare_parameter("sdf_map/local_update_range_x", -1.0);
  this->declare_parameter("sdf_map/local_update_range_y", -1.0);
  this->declare_parameter("sdf_map/local_update_range_z", -1.0);
  this->declare_parameter("sdf_map/obstacles_inflation", -1.0);

  this->declare_parameter("sdf_map/fx", -1.0);
  this->declare_parameter("sdf_map/fy", -1.0);
  this->declare_parameter("sdf_map/cx", -1.0);
  this->declare_parameter("sdf_map/cy", -1.0);

  this->declare_parameter("sdf_map/use_depth_filter", true);
  this->declare_parameter("sdf_map/depth_filter_tolerance", 1.0);
  this->declare_parameter("sdf_map/depth_filter_maxdist", -1.0);
  this->declare_parameter("sdf_map/depth_filter_mindist", -1.0);
  this->declare_parameter("sdf_map/depth_filter_margin", -1);
  this->declare_parameter("sdf_map/k_depth_scaling_factor", -1.0);
  this->declare_parameter("sdf_map/skip_pixel", -1);

  this->declare_parameter("sdf_map/p_hit", 0.70);
  this->declare_parameter("sdf_map/p_miss", 0.35);
  this->declare_parameter("sdf_map/p_min", 0.12);
  this->declare_parameter("sdf_map/p_max", 0.97);
  this->declare_parameter("sdf_map/p_occ", 0.80);
  this->declare_parameter("sdf_map/min_ray_length", -0.1);
  this->declare_parameter("sdf_map/max_ray_length", -0.1);

  this->declare_parameter("sdf_map/esdf_slice_height", -0.1);
  this->declare_parameter("sdf_map/visualization_truncate_height", -0.1);
  this->declare_parameter("sdf_map/virtual_ceil_height", -0.1);

  this->declare_parameter("sdf_map/show_occ_time", false);
  this->declare_parameter("sdf_map/show_esdf_time", false);
  this->declare_parameter("sdf_map/pose_type", 1);

  this->declare_parameter("sdf_map/frame_id", std::string("world"));
  this->declare_parameter("sdf_map/local_bound_inflate", 1.0);
  this->declare_parameter("sdf_map/local_map_margin", 1);
  this->declare_parameter("sdf_map/ground_height", 1.0);

  this->get_parameter("sdf_map/resolution", mp.resolution_);
  this->get_parameter("sdf_map/map_size_x", x_size);
  this->get_parameter("sdf_map/map_size_y", y_size);
  this->get_parameter("sdf_map/map_size_z", z_size);
  this->get_parameter("sdf_map/local_update_range_x", mp.local_update_range_(0));
  this->get_parameter("sdf_map/local_update_range_y", mp.local_update_range_(1));
  this->get_parameter("sdf_map/local_update_range_z", mp.local_update_range_(2));
  this->get_parameter("sdf_map/obstacles_inflation", mp.obstacles_inflation_);

  this->get_parameter("sdf_map/fx", mp.fx_);
  this->get_parameter("sdf_map/fy", mp.fy_);
  this->get_parameter("sdf_map/cx", mp.cx_);
  this->get_parameter("sdf_map/cy", mp.cy_);

  this->get_parameter("sdf_map/use_depth_filter", mp.use_depth_filter_);
  this->get_parameter("sdf_map/depth_filter_tolerance", mp.depth_filter_tolerance_);
  this->get_parameter("sdf_map/depth_filter_maxdist", mp.depth_filter_maxdist_);
  this->get_parameter("sdf_map/depth_filter_mindist", mp.depth_filter_mindist_);
  this->get_parameter("sdf_map/depth_filter_margin", mp.depth_filter_margin_);
  this->get_parameter("sdf_map/k_depth_scaling_factor", mp.k_depth_scaling_factor_);
  this->get_parameter("sdf_map/skip_pixel", mp.skip_pixel_);

  this->get_parameter("sdf_map/p_hit", mp.p_hit_);
  this->get_parameter("sdf_map/p_miss", mp.p_miss_);
  this->get_parameter("sdf_map/p_min", mp.p_min_);
  this->get_parameter("sdf_map/p_max", mp.p_max_);
  this->get_parameter("sdf_map/p_occ", mp.p_occ_);
  this->get_parameter("sdf_map/min_ray_length", mp.min_ray_length_);
  this->get_parameter("sdf_map/max_ray_length", mp.max_ray_length_);

  this->get_parameter("sdf_map/esdf_slice_height", mp.esdf_slice_height_);
  this->get_parameter("sdf_map/visualization_truncate_height", mp.visualization_truncate_height_);
  this->get_parameter("sdf_map/virtual_ceil_height", mp.virtual_ceil_height_);

  this->get_parameter("sdf_map/show_occ_time", mp.show_occ_time_);
  this->get_parameter("sdf_map/show_esdf_time", mp.show_esdf_time_);
  this->get_parameter("sdf_map/pose_type", mp.pose_type_);

  this->get_parameter("sdf_map/frame_id", mp.frame_id_);
  this->get_parameter("sdf_map/local_bound_inflate", mp.local_bound_inflate_);
  this->get_parameter("sdf_map/local_map_margin", mp.local_map_margin_);
  this->get_parameter("sdf_map/ground_height", mp.ground_height_);

  // Create a timer to execute the FSM callback every 100 milliseconds
  exec_timer_ = this->create_wall_timer(
      std::chrono::milliseconds(10),
      std::bind(&KinoReplanFSM::execFSMCallback, this));

  // Create a timer to check for collisions every 100 milliseconds
  safety_timer_ = this->create_wall_timer(
      std::chrono::milliseconds(50),
      std::bind(&KinoReplanFSM::checkCollisionCallback, this));

  // Subscribe to the "waypoints" topic to receive waypoint messages
  waypoint_sub_ = this->create_subscription<nav_msgs::msg::Path>(
      "waypoints", 10, std::bind(&KinoReplanFSM::waypointCallback, this, std::placeholders::_1));

  // Subscribe to the "odom" topic to receive odometry messages
  odom_sub_ = this->create_subscription<nav_msgs::msg::Odometry>(
      "odom", 10, std::bind(&KinoReplanFSM::odometryCallback, this, std::placeholders::_1));

  // Create a publisher for the "replan" topic to signal replanning
  replan_pub_ = this->create_publisher<std_msgs::msg::Empty>("replan", 10);

 /* init callback */
  auto depth_sub_ = std::make_shared<message_filters::Subscriber<sensor_msgs::msg::Image>>(this, "/sdf_map/depth");
  auto sync_odom_sub_ = std::make_shared<message_filters::Subscriber<nav_msgs::msg::Odometry>>(this, "/sdf_map/odom");
  auto depth_odom_sync = std::make_shared<message_filters::Synchronizer<SyncPolicyImageOdom>>(
      SyncPolicyImageOdom(100), *depth_sub_, *sync_odom_sub_);
  depth_odom_sync->registerCallback(std::bind(&KinoReplanFSM::depthOdomCallback, this, std::placeholders::_1, std::placeholders::_2, 0, 1));

// use odometry and point cloud
// indep_cloud_sub_ = node_.subscribe<sensor_msgs::PointCloud2>("/sdf_map/cloud", 10, &SDFMap::cloudCallback, this);

// occ_timer_ = node_.createTimer(ros::Duration(0.05), &SDFMap::updateOccupancyCallback, this);
// esdf_timer_ = node_.createTimer(ros::Duration(0.05), &SDFMap::updateESDFCallback, this);
// vis_timer_ = node_.createTimer(ros::Duration(0.05), &SDFMap::visCallback, this);

// map_pub_ = node_.advertise<sensor_msgs::PointCloud2>("/sdf_map/occupancy", 10);
// map_inf_pub_ = node_.advertise<sensor_msgs::PointCloud2>("/sdf_map/occupancy_inflate", 10);
// esdf_pub_ = node_.advertise<sensor_msgs::PointCloud2>("/sdf_map/esdf", 10);
// update_range_pub_ = node_.advertise<visualization_msgs::Marker>("/sdf_map/update_range", 10);

// unknown_pub_ = node_.advertise<sensor_msgs::PointCloud2>("/sdf_map/unknown", 10);
// depth_pub_ = node_.advertise<sensor_msgs::PointCloud2>("/sdf_map/depth_cloud", 10);

indep_cloud_sub_ = this->create_subscription<sensor_msgs::msg::PointCloud2>(
    "/sdf_map/cloud", 10, std::bind(&KinoReplanFSM::cloudCallback, this, std::placeholders::_1));

occ_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(50), std::bind(&KinoReplanFSM::updateOccupancyCallback, this));

esdf_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(50), std::bind(&KinoReplanFSM::updateESDFCallback, this));

vis_timer_ = this->create_wall_timer(
    std::chrono::milliseconds(50), std::bind(&KinoReplanFSM::visCallback, this));

map_pub_ = this->create_publisher<sensor_msgs::msg::PointCloud2>("/sdf_map/occupancy", 10);
map_inf_pub_ = this->create_publisher<sensor_msgs::msg::PointCloud2>("/sdf_map/occupancy_inflate", 10);
esdf_pub_ = this->create_publisher<sensor_msgs::msg::PointCloud2>("/sdf_map/esdf", 10);
update_range_pub_ = this->create_publisher<visualization_msgs::msg::Marker>("/sdf_map/update_range", 10);

unknown_pub_ = this->create_publisher<sensor_msgs::msg::PointCloud2>("/sdf_map/unknown", 10);
depth_pub_ = this->create_publisher<sensor_msgs::msg::PointCloud2>("/sdf_map/depth_cloud", 10);

  // Create a publisher for the "new" topic to signal new events

  /* initialize main modules */
  planner_manager_.reset(new FastPlannerManager);
  planner_manager_->initPlanModules(pp, mp, use_geometric_path, use_kinodynamic_path, use_topo_path,
                                    use_optimization);
  visualization_.reset(new PlanningVisualization(nh));

}

void KinoReplanFSM::depthOdomCallback(const sensor_msgs::msg::Image::SharedPtr depth_msg, const nav_msgs::msg::Odometry::SharedPtr odom_msg, int param1, int param2) {

planner_manager_->sdf_map_->depthOdomCallback(depth_msg, odom_msg);

}

void KinoReplanFSM::cloudCallback(const sensor_msgs::msg::PointCloud2::SharedPtr cloud_msg) {
  planner_manager_->sdf_map_->cloudCallback(cloud_msg);
}

void KinoReplanFSM::updateOccupancyCallback() {
  planner_manager_->sdf_map_->updateOccupancyCallback();
}

void KinoReplanFSM::updateESDFCallback() {
  planner_manager_->sdf_map_->updateESDFCallback();
}

void KinoReplanFSM::visCallback() {
  // Placeholder for visualization callback
}

void KinoReplanFSM::waypointCallback(const nav_msgs::msg::Path::SharedPtr msg) {
  if (msg->poses[0].pose.position.z < -0.1) return;

  std::cout << "Triggered!" << std::endl;
  trigger_ = true;

  if (target_type_ == TARGET_TYPE::MANUAL_TARGET) {
    end_pt_ << msg->poses[0].pose.position.x, msg->poses[0].pose.position.y, 1.0;

  } else if (target_type_ == TARGET_TYPE::PRESET_TARGET) {
    end_pt_(0)  = waypoints_[current_wp_][0];
    end_pt_(1)  = waypoints_[current_wp_][1];
    end_pt_(2)  = waypoints_[current_wp_][2];
    current_wp_ = (current_wp_ + 1) % waypoint_num_;
  }

  visualization_->drawGoal(end_pt_, 0.3, Eigen::Vector4d(1, 0, 0, 1.0));
  end_vel_.setZero();
  have_target_ = true;

  if (exec_state_ == WAIT_TARGET)
    changeFSMExecState(GEN_NEW_TRAJ, "TRIG");
  else if (exec_state_ == EXEC_TRAJ)
    changeFSMExecState(REPLAN_TRAJ, "TRIG");
}


void KinoReplanFSM::odometryCallback(const nav_msgs::msg::Odometry::SharedPtr msg) {
  odom_pos_(0) = msg->pose.pose.position.x;
  odom_pos_(1) = msg->pose.pose.position.y;
  odom_pos_(2) = msg->pose.pose.position.z;

  odom_vel_(0) = msg->twist.twist.linear.x;
  odom_vel_(1) = msg->twist.twist.linear.y;
  odom_vel_(2) = msg->twist.twist.linear.z;

  odom_orient_.w() = msg->pose.pose.orientation.w;
  odom_orient_.x() = msg->pose.pose.orientation.x;
  odom_orient_.y() = msg->pose.pose.orientation.y;
  odom_orient_.z() = msg->pose.pose.orientation.z;

  have_odom_ = true;
}

void KinoReplanFSM::changeFSMExecState(FSM_EXEC_STATE new_state, string pos_call) {
  string state_str[5] = { "INIT", "WAIT_TARGET", "GEN_NEW_TRAJ", "REPLAN_TRAJ", "EXEC_TRAJ" };
  int    pre_s        = int(exec_state_);
  exec_state_         = new_state;
  cout << "[" + pos_call + "]: from " + state_str[pre_s] + " to " + state_str[int(new_state)] << endl;
}

void KinoReplanFSM::printFSMExecState() {
  string state_str[5] = { "INIT", "WAIT_TARGET", "GEN_NEW_TRAJ", "REPLAN_TRAJ", "EXEC_TRAJ" };

  cout << "[FSM]: state: " + state_str[int(exec_state_)] << endl;
}

void KinoReplanFSM::execFSMCallback() {
  static int fsm_num = 0;
  fsm_num++;
  if (fsm_num == 100) {
    printFSMExecState();
    if (!have_odom_) std::cout << "no odom." << std::endl;
    if (!trigger_) std::cout << "wait for goal." << std::endl;
    fsm_num = 0;
  }

  switch (exec_state_) {
    case INIT: {
      if (!have_odom_) {
        return;
      }
      if (!trigger_) {
        return;
      }
      changeFSMExecState(WAIT_TARGET, "FSM");
      break;
    }

    case WAIT_TARGET: {
      if (!have_target_)
        return;
      else {
        changeFSMExecState(GEN_NEW_TRAJ, "FSM");
      }
      break;
    }

    case GEN_NEW_TRAJ: {
      start_pt_  = odom_pos_;
      start_vel_ = odom_vel_;
      start_acc_.setZero();

      Eigen::Vector3d rot_x = odom_orient_.toRotationMatrix().block(0, 0, 3, 1);
      start_yaw_(0)         = atan2(rot_x(1), rot_x(0));
      start_yaw_(1) = start_yaw_(2) = 0.0;

      bool success = callKinodynamicReplan();
      if (success) {
        changeFSMExecState(EXEC_TRAJ, "FSM");
      } else {
        // have_target_ = false;
        // changeFSMExecState(WAIT_TARGET, "FSM");
        changeFSMExecState(GEN_NEW_TRAJ, "FSM");
      }
      break;
    }

    case EXEC_TRAJ: {
      /* determine if need to replan */
      LocalTrajData* info     = &planner_manager_->local_data_;
      // ros::Time      time_now = ros::Time::now();
      rclcpp::Time curr_time = this->now();
      double         t_cur    = (curr_time - info->start_time_).seconds();
      t_cur                   = std::min(info->duration_, t_cur);

      Eigen::Vector3d pos = info->position_traj_.evaluateDeBoorT(t_cur);

      /* && (end_pt_ - pos).norm() < 0.5 */
      if (t_cur > info->duration_ - 1e-2) {
        have_target_ = false;
        changeFSMExecState(WAIT_TARGET, "FSM");
        return;

      } else if ((end_pt_ - pos).norm() < no_replan_thresh_) {
        // std::cout << "near end" << std::endl;
        return;

      } else if ((info->start_pos_ - pos).norm() < replan_thresh_) {
        // std::cout << "near start" << std::endl;
        return;

      } else {
        changeFSMExecState(REPLAN_TRAJ, "FSM");
      }
      break;
    }

    case REPLAN_TRAJ: {
      LocalTrajData* info     = &planner_manager_->local_data_;
      // ros::Time      time_now = ros::Time::now();
      rclcpp::Time curr_time = this->now();
      double         t_cur    = (curr_time - info->start_time_).seconds();

      start_pt_  = info->position_traj_.evaluateDeBoorT(t_cur);
      start_vel_ = info->velocity_traj_.evaluateDeBoorT(t_cur);
      start_acc_ = info->acceleration_traj_.evaluateDeBoorT(t_cur);

      start_yaw_(0) = info->yaw_traj_.evaluateDeBoorT(t_cur)[0];
      start_yaw_(1) = info->yawdot_traj_.evaluateDeBoorT(t_cur)[0];
      start_yaw_(2) = info->yawdotdot_traj_.evaluateDeBoorT(t_cur)[0];

      std_msgs::msg::Empty replan_msg;
      replan_pub_->publish(replan_msg);

      bool success = callKinodynamicReplan();
      if (success) {
        changeFSMExecState(EXEC_TRAJ, "FSM");
      } else {
        changeFSMExecState(GEN_NEW_TRAJ, "FSM");
      }
      break;
    }
  }
}

void KinoReplanFSM::checkCollisionCallback() {
  LocalTrajData* info = &planner_manager_->local_data_;

  if (have_target_) {
    auto edt_env = planner_manager_->edt_environment_;

    double dist = planner_manager_->pp_.dynamic_ ?
        edt_env->evaluateCoarseEDT(end_pt_, /* time to program start + */ info->duration_) :
        edt_env->evaluateCoarseEDT(end_pt_, -1.0);

    if (dist <= 0.3) {
      /* try to find a max distance goal around */
      bool            new_goal = false;
      const double    dr = 0.5, dtheta = 30, dz = 0.3;
      double          new_x, new_y, new_z, max_dist = -1.0;
      Eigen::Vector3d goal;

      for (double r = dr; r <= 5 * dr + 1e-3; r += dr) {
        for (double theta = -90; theta <= 270; theta += dtheta) {
          for (double nz = 1 * dz; nz >= -1 * dz; nz -= dz) {

            new_x = end_pt_(0) + r * cos(theta / 57.3);
            new_y = end_pt_(1) + r * sin(theta / 57.3);
            new_z = end_pt_(2) + nz;

            Eigen::Vector3d new_pt(new_x, new_y, new_z);
            dist = planner_manager_->pp_.dynamic_ ?
                edt_env->evaluateCoarseEDT(new_pt, /* time to program start+ */ info->duration_) :
                edt_env->evaluateCoarseEDT(new_pt, -1.0);

            if (dist > max_dist) {
              /* reset end_pt_ */
              goal(0)  = new_x;
              goal(1)  = new_y;
              goal(2)  = new_z;
              max_dist = dist;
            }
          }
        }
      }

      if (max_dist > 0.3) {
        cout << "change goal, replan." << endl;
        end_pt_      = goal;
        have_target_ = true;
        end_vel_.setZero();

        if (exec_state_ == EXEC_TRAJ) {
          changeFSMExecState(REPLAN_TRAJ, "SAFETY");
        }

        visualization_->drawGoal(end_pt_, 0.3, Eigen::Vector4d(1, 0, 0, 1.0));
      } else {
        // have_target_ = false;
        // cout << "Goal near collision, stop." << endl;
        // changeFSMExecState(WAIT_TARGET, "SAFETY");
        cout << "goal near collision, keep retry" << endl;
        changeFSMExecState(REPLAN_TRAJ, "FSM");

        std_msgs::msg::Empty emt;
        replan_pub_->publish(emt);
      }
    }
  }

  /* ---------- check trajectory ---------- */
  if (exec_state_ == FSM_EXEC_STATE::EXEC_TRAJ) {
    double dist;
    bool   safe = planner_manager_->checkTrajCollision(dist);

    if (!safe) {
      // cout << "current traj in collision." << endl;
      RCLCPP_WARN(this->get_logger(), "current traj in collision.");
      changeFSMExecState(REPLAN_TRAJ, "SAFETY");
    }
  }
}

bool KinoReplanFSM::callKinodynamicReplan() {
  bool plan_success =
      planner_manager_->kinodynamicReplan(start_pt_, start_vel_, start_acc_, end_pt_, end_vel_);

  if (plan_success) {

    planner_manager_->planYaw(start_yaw_);

    auto info = &planner_manager_->local_data_;

    /* publish traj */
    plan_manage::Bspline bspline;
    bspline.order      = 3;
    bspline.start_time = info->start_time_;
    bspline.traj_id    = info->traj_id_;

    Eigen::MatrixXd pos_pts = info->position_traj_.getControlPoint();

    for (int i = 0; i < pos_pts.rows(); ++i) {
      geometry_msgs::Point pt;
      pt.x = pos_pts(i, 0);
      pt.y = pos_pts(i, 1);
      pt.z = pos_pts(i, 2);
      bspline.pos_pts.push_back(pt);
    }

    Eigen::VectorXd knots = info->position_traj_.getKnot();
    for (int i = 0; i < knots.rows(); ++i) {
      bspline.knots.push_back(knots(i));
    }

    Eigen::MatrixXd yaw_pts = info->yaw_traj_.getControlPoint();
    for (int i = 0; i < yaw_pts.rows(); ++i) {
      double yaw = yaw_pts(i, 0);
      bspline.yaw_pts.push_back(yaw);
    }
    bspline.yaw_dt = info->yaw_traj_.getInterval();

    bspline_pub_.publish(bspline);

    /* visulization */
    auto plan_data = &planner_manager_->plan_data_;
    visualization_->drawGeometricPath(plan_data->kino_path_, 0.075, Eigen::Vector4d(1, 1, 0, 0.4));
    visualization_->drawBspline(info->position_traj_, 0.1, Eigen::Vector4d(1.0, 0, 0.0, 1), true, 0.2,
                                Eigen::Vector4d(1, 0, 0, 1));

    return true;

  } else {
    cout << "generate new traj fail." << endl;
    return false;
  }
}

}  // namespace fast_planner

#include "rclcpp_components/register_node_macro.hpp"
RCLCPP_COMPONENTS_REGISTER_NODE(fast_planner::KinoReplanFSM)