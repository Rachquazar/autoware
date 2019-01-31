#include <deque>
#include <iostream>
#include <vector>
#include <chrono>

#include <geometry_msgs/PoseArray.h>
#include <geometry_msgs/PoseStamped.h>
#include <geometry_msgs/PoseWithCovarianceStamped.h>
#include <geometry_msgs/TwistStamped.h>
#include <ros/ros.h>
#include <sensor_msgs/Imu.h>
#include <std_msgs/Float64.h>
#include <std_msgs/Float64MultiArray.h>
#include <tf/transform_datatypes.h>
#include <tf2/utils.h>

#include <autoware_msgs/VehicleStatus.h>

#include "kalman_filter/kalman_filter.h"

#define PRINT_MAT(X) std::cout << #X << ":\n" << X << std::endl << std::endl

#define EKF_DEBUG_VERBOSE
#ifdef EKF_DEBUG_VERBOSE
#define EKF_INFO(...) {if (show_debug_info_) { printf(__VA_ARGS__); } }
#else
#define EKF_INFO(...)
#endif



class KalmanFilterNode {
public:
  KalmanFilterNode();
  ~KalmanFilterNode();

private:
  ros::NodeHandle nh_, pnh_;
  ros::Publisher pub_pose_, pub_pose_array_, pub_debug_, pub_ndt_pose_;
  ros::Subscriber sub_ndt_pose_, sub_vehicle_status_, sub_imu_;
  ros::Timer timer_control_;
  bool show_debug_info_;

  KalmanFilter kf_;
  bool initial_pose_received_;
  bool initial_twist_received_;

  /* parameters */
  double predict_frequency_;
  double predict_dt_;
  double wheelbase_;
  unsigned int dim_x_;
  unsigned int dim_x_ex_;
  int extend_state_step_;


  /* process noise standard deviation for continuous model*/
  double stddev_proc_x_c_;
  double stddev_proc_y_c_;
  double stddev_proc_yaw_c_;
  double stddev_proc_wz_bias_c_;

  /* process noise variance for discrete model */
  double cov_proc_x_d_;
  double cov_proc_y_d_;
  double cov_proc_yaw_d_;
  double cov_proc_wz_bias_d_;

  /* measurement noise for time uncertainty */
  double measure_time_uncertainty_;

  ros::Time last_ndt_received_time_;


  /* for model prediction */
  autoware_msgs::VehicleStatus current_vehicle_status_;
  geometry_msgs::TwistStamped current_twist_;
  geometry_msgs::PoseStamped current_ndt_pose_;
  sensor_msgs::Imu current_imu_;

  void timerCallback(const ros::TimerEvent &e);
  void callbackNDTPose(const geometry_msgs::PoseStamped::ConstPtr &msg);
  void callbackVehicleStatus(const autoware_msgs::VehicleStatus &msg);
  void callbackIMU(const sensor_msgs::Imu &msg);
  void callbackTwist(const geometry_msgs::TwistStamped &msg);

  void initKalmanFilter();
  void predictKinematicsModel();
  void measurementUpdateNDTPose(const geometry_msgs::PoseStamped &ndt_pose);
  void measurementUpdateIMU(const sensor_msgs::Imu &msg);

  void publishEstimatedPose();
};

KalmanFilterNode::KalmanFilterNode()
    : nh_(""), pnh_("~"), initial_pose_received_(false),
      initial_twist_received_(false) {

  pnh_.param("show_debug_info", show_debug_info_, bool(false));
  pnh_.param("predict_frequency", predict_frequency_, double(50.0));
  pnh_.param("wheelbase", wheelbase_, double(2.79));
  pnh_.param("extend_state_step", extend_state_step_, int(40));

  pnh_.param("stddev_proc_x_c", stddev_proc_x_c_, double(1.0));
  pnh_.param("stddev_proc_y_c", stddev_proc_y_c_, double(1.0));
  pnh_.param("stddev_proc_yaw_c", stddev_proc_yaw_c_, double(0.5));
  pnh_.param("stddev_proc_wz_bias_c", stddev_proc_wz_bias_c_, double(0.2));

  pnh_.param("measure_time_uncertainty", measure_time_uncertainty_, double(0.03));

  predict_dt_ = 1.0 / std::max(predict_frequency_, 0.1);
  cov_proc_x_d_ = stddev_proc_x_c_ * stddev_proc_x_c_ * predict_dt_ * predict_dt_;
  cov_proc_y_d_ = stddev_proc_y_c_ * stddev_proc_y_c_ * predict_dt_ * predict_dt_;
  cov_proc_yaw_d_ = stddev_proc_yaw_c_ * stddev_proc_yaw_c_ * predict_dt_ * predict_dt_;
  cov_proc_wz_bias_d_ = stddev_proc_wz_bias_c_ * stddev_proc_wz_bias_c_ * predict_dt_ * predict_dt_;

  timer_control_ = nh_.createTimer(ros::Duration(predict_dt_), &KalmanFilterNode::timerCallback, this);
  pub_pose_ = nh_.advertise<geometry_msgs::PoseWithCovarianceStamped>("/kf_estimated_pose", 1);
  pub_ndt_pose_ = pnh_.advertise<geometry_msgs::PoseStamped>("/my_ndt_pose", 1);
  pub_pose_array_ = nh_.advertise<geometry_msgs::PoseArray>("/kalman_filter_pose_array", 1);
  pub_debug_ = pnh_.advertise<std_msgs::Float64MultiArray>("debug", 1);
  sub_ndt_pose_ = nh_.subscribe("/ndt_pose", 1, &KalmanFilterNode::callbackNDTPose, this);
  sub_vehicle_status_ = nh_.subscribe("/vehicle_status", 1, &KalmanFilterNode::callbackVehicleStatus, this);
  sub_imu_ = nh_.subscribe("/imu_raw", 1, &KalmanFilterNode::callbackIMU, this);


  dim_x_ = 4;
  dim_x_ex_ = dim_x_ * extend_state_step_;
  last_ndt_received_time_ = ros::Time::now();

  initKalmanFilter();
};

KalmanFilterNode::~KalmanFilterNode(){};

/*
 * timerCallback
 */
void KalmanFilterNode::timerCallback(const ros::TimerEvent &e) {

  /* check flags */
  if (!initial_pose_received_ || !initial_twist_received_) {
    ROS_WARN("initial info is not received. pose = %d, odom = %d",
             initial_pose_received_, initial_twist_received_);
    return;
  }

  /* predict model in kalman filter */
  predictKinematicsModel();

  publishEstimatedPose();

}

/*
 * callbackNDTPose
 */
void KalmanFilterNode::callbackNDTPose(
    const geometry_msgs::PoseStamped::ConstPtr &msg) {
  current_ndt_pose_ = *msg;
  initial_pose_received_ = true;

  measurementUpdateNDTPose(current_ndt_pose_);
};

/*
 * callbackVehicleStatus
 */
void KalmanFilterNode::callbackVehicleStatus(
    const autoware_msgs::VehicleStatus &msg) {
  current_vehicle_status_ = msg;
  // current_twist_.linear.x = msg->speed;
  // current_twist_.angular.z = msg->speed * tan(msg->angle) / wheelbase_;

  // TEMP
  current_twist_.twist.linear.x = msg.speed / 3.6;
  current_twist_.twist.angular.z =
      (msg.speed / 3.6) * tan(msg.angle * 3.1415 / 180.0 / 17.85) / 2.95;
  initial_twist_received_ = true;
};

/*
 * callbackTwist
 */
void KalmanFilterNode::callbackTwist(const geometry_msgs::TwistStamped &msg) {
  current_twist_ = msg;
  initial_twist_received_ = true;
};

/*
 * callbackIMU
 */
void KalmanFilterNode::callbackIMU(const sensor_msgs::Imu &msg) {
  current_imu_ = msg;
  measurementUpdateIMU(current_imu_);
}

/*
 * initKalmanFilter
 */
void KalmanFilterNode::initKalmanFilter() {
  Eigen::MatrixXf X = Eigen::MatrixXf::Zero(dim_x_ex_, 1);
  Eigen::MatrixXf P = Eigen::MatrixXf::Identity(dim_x_ex_, dim_x_ex_) * 1.0E9;
  for (int i = 0; i + 3 < dim_x_ex_; i += dim_x_) {
    P(i + 3, i + 3) = 0.1; // for omega bias
  }
  kf_.init(X, P);
}

/*
 * predictKinematicsModel
 */
void KalmanFilterNode::predictKinematicsModel() {

/*  == Nonlinear model ==
 *
 * x_{k+1} = x_k + vx * cos(yaw_k) * dt
 * y_{k+1} = y_k + vx * sin(yaw_k) * dt
 * yaw_{k+1} = yaw_k + (wz + wz_bias_k) * dt
 * wz_bias_{k+1} = wz_bias_k
 */

 /*  == Linearized model ==
  *
  * A = [ 1, 0, -vx*sin(yaw)*dt, 0 ]
  *     [ 0, 1, vx*cos(yaw)*dt, 0 ]
  *     [ 0, 0, 1,              dt ]
  *     [ 0, 0, 0,               1 ]
  */

  Eigen::MatrixXf X_curr(dim_x_ex_, 1); // curent state
  Eigen::MatrixXf X_next(dim_x_ex_, 1); // predicted state
  kf_.getX(X_curr);

  const double vx = current_twist_.twist.linear.x;
  const double wz = current_twist_.twist.angular.z;
  const double yaw = X_curr(2);
  const double wz_bias = X_curr(3);
  const double dt = predict_dt_;
  EKF_INFO("X_curr = %f, %f, %f, %f\n", X_curr(0), X_curr(1), X_curr(2), X_curr(3));

  /* update for latest state */
  X_next(0) = X_curr(0) + vx * cos(yaw) * dt;  // dx = v * cos(yaw)
  X_next(1) = X_curr(1) + vx * sin(yaw) * dt;  // dy = v * sin(yaw)
  X_next(2) = X_curr(2) + (wz + wz_bias) * dt; // dyaw = omega + omega_bias
  X_next(3) = X_curr(3); // d_omega_bias = 0;

  /* slide states in the time direction */
  for (int i = dim_x_; i < dim_x_ex_; ++i) {
    X_next(i) = X_curr(i - dim_x_);
  }

  EKF_INFO("X_next = %f, %f, %f, %f\n", X_next(0), X_next(1), X_next(2), X_next(3));

  /* set A matrix for latest state */
  Eigen::MatrixXf A_ex = Eigen::MatrixXf::Zero(dim_x_ex_, dim_x_ex_);
  A_ex.block(0, 0, dim_x_, dim_x_) = Eigen::MatrixXf::Identity(dim_x_, dim_x_);
  A_ex(0, 2) = -vx * sin(yaw) * dt;
  A_ex(1, 2) = vx * cos(yaw) * dt;
  A_ex(2, 3) = dt;

  /* set A matrix for other states (x_k = x_k) */
  const int dim_d = dim_x_ex_ - dim_x_;
  A_ex.block(dim_x_, 0, dim_d, dim_d) = Eigen::MatrixXf::Identity(dim_d, dim_d);

  /* set covariance matrix Q for process noise */
  Eigen::MatrixXf Q_ex = Eigen::MatrixXf::Zero(dim_x_ex_, dim_x_ex_);
  Q_ex(0, 0) = cov_proc_x_d_;
  Q_ex(1, 1) = cov_proc_y_d_;
  Q_ex(2, 2) = cov_proc_yaw_d_;
  Q_ex(3, 3) = cov_proc_wz_bias_d_;

  auto start = std::chrono::system_clock::now();
  kf_.predictEKF(X_next, A_ex, Q_ex);
  auto now = std::chrono::system_clock::now();
  double elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count();
  EKF_INFO("[time] calc predictEKF time = %f [ms]\n", elapsed * 1.0e-6);
}

/*
 * measurementUpdateNDTPose
 */
void KalmanFilterNode::measurementUpdateNDTPose(const geometry_msgs::PoseStamped &ndt_pose) {

  const int dim_y = 3; // pos_x, pos_y, yaw, depending on NDT output
  const ros::Time t_curr = ros::Time::now();

  /* calculate delay step */
  double delay_time = (t_curr - ndt_pose.header.stamp).toSec();
  if (delay_time < 0) {
    delay_time = 0;
    ROS_WARN("time stamp is inappropriate. delay = %f, now() = %f, ndt.stamp = %f",
             delay_time, t_curr.toSec(), ndt_pose.header.stamp.toSec());
  };

  int delay_step = std::roundf(delay_time / predict_dt_);
  if (delay_step > extend_state_step_ - 1) {
    delay_step = extend_state_step_ - 1;
    ROS_WARN("NDT pose delay time: %f[s] exceeds the allowable limit: extend_state_step * predict_dt_ = %f [s]",
             delay_time, extend_state_step_ * predict_dt_);
  }

  EKF_INFO("predict_dt_ = %f, delay_time = %f, delay_step = %d, extend_state_step_ = %d\n",
           predict_dt_, delay_time, delay_step, extend_state_step_);

  /* set measurement matrix */
  Eigen::MatrixXf C_ex = Eigen::MatrixXf::Zero(dim_y, dim_x_ex_);
  C_ex(0, dim_x_ * delay_step) = 1.0;     // for pos x
  C_ex(1, dim_x_ * delay_step + 1) = 1.0; // for pos y
  C_ex(2, dim_x_ * delay_step + 2) = 1.0; // for yaw

  /* set measurement noise covariancs */
  const double dt = (t_curr - last_ndt_received_time_).toSec();
  const double vx = current_twist_.twist.linear.x;
  const double wz = current_twist_.twist.angular.z;
  const double cov_pos_x = measure_time_uncertainty_ * measure_time_uncertainty_ * vx * vx;
  const double cov_pos_y = measure_time_uncertainty_ * measure_time_uncertainty_ * vx * vx;
  const double cov_yaw = measure_time_uncertainty_ * measure_time_uncertainty_ * wz * wz;
  Eigen::MatrixXf R = Eigen::MatrixXf::Zero(dim_y, dim_y);
  R(0, 0) = std::pow(0.05, 2) + cov_pos_x*0.01; // pos_x
  R(1, 1) = std::pow(0.05, 2) + cov_pos_y*0.01; // pos_y
  R(2, 2) = std::pow(0.05, 2) + cov_yaw*0.01;   // yaw

  /* measurement update */
  Eigen::MatrixXf y(dim_y, 1);
  y << ndt_pose.pose.position.x, ndt_pose.pose.position.y,
      tf::getYaw(ndt_pose.pose.orientation);

  auto start = std::chrono::system_clock::now();
  kf_.update(y, C_ex, R);
  auto end = std::chrono::system_clock::now();
  double elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  EKF_INFO("[time] calc measurement updateEKF time = %f [ms]\n", elapsed * 1.0e-6);

  last_ndt_received_time_ = t_curr;
}

/*
 * measurementUpdateIMU
 */
void KalmanFilterNode::measurementUpdateIMU(const sensor_msgs::Imu &msg) {}

/*
 * publishEstimatedPose
 */
void KalmanFilterNode::publishEstimatedPose() {
  Eigen::MatrixXf X(dim_x_ex_, 1);
  Eigen::MatrixXf P(dim_x_ex_, dim_x_ex_);
  kf_.getX(X);
  kf_.getP(P);
  // PRINT_MAT(P);

  /* publish latest pose */
  geometry_msgs::PoseWithCovarianceStamped pose_curr;
  pose_curr.header.stamp = ros::Time::now();
  pose_curr.header.frame_id = current_ndt_pose_.header.frame_id;
  pose_curr.pose.pose.position.x = X(0);
  pose_curr.pose.pose.position.y = X(1);
  pose_curr.pose.pose.position.z = current_ndt_pose_.pose.position.z;
  pose_curr.pose.pose.orientation = tf::createQuaternionMsgFromYaw(X(2, 0));
  for (int i = 0; i < 36; ++i) {
    pose_curr.pose.covariance[i] = 0.0;
  }
  pose_curr.pose.covariance[0] = P(0, 0);
  pose_curr.pose.covariance[1] = P(0, 1);
  pose_curr.pose.covariance[5] = P(0, 2);
  pose_curr.pose.covariance[6] = P(1, 0);
  pose_curr.pose.covariance[7] = P(1, 1);
  pose_curr.pose.covariance[11] = P(1, 2);
  pose_curr.pose.covariance[30] = P(2, 1);
  pose_curr.pose.covariance[31] = P(2, 1);
  pose_curr.pose.covariance[35] = P(2, 2);
  pub_pose_.publish(pose_curr);


  /* publish pose array */
  // geometry_msgs::PoseArray pose_array;
  // geometry_msgs::Pose pose;
  // for (unsigned int i = 0; i < dim_x_ex_; i += dim_x_) {
  //   pose.position.x = X(i, 0);
  //   pose.position.y = X(i + 1, 0);
  //   pose.position.z = current_ndt_pose_.pose.position.z;
  //   pose.orientation = tf::createQuaternionMsgFromYaw(X(i + 2, 0));
  //   pose_array.poses.push_back(pose);
  // }
  // pose_array.header.stamp = ros::Time::now();
  // pose_array.header.frame_id = current_ndt_pose_.header.frame_id;
  // pub_pose_array_.publish(pose_array);


  /* debug my ndt */
  // geometry_msgs::PoseStamped p;
  // p = current_ndt_pose_;
  // p.header.stamp = ros::Time::now();
  // pub_ndt_pose_.publish(p);

  /* debug publish */
  std_msgs::Float64MultiArray msg;
  msg.data.push_back(current_twist_.twist.linear.x);
  msg.data.push_back(current_twist_.twist.angular.z);
  msg.data.push_back(X(2) * 180.0 / 3.1415); // yaw angle
  msg.data.push_back(tf::getYaw(current_ndt_pose_.pose.orientation) * 180.0 / 3.1415); // NDT yaw angle
  msg.data.push_back(X(3) * 180.0 / 3.1415); // omega bias
  pub_debug_.publish(msg);
}

int main(int argc, char **argv) {

  ros::init(argc, argv, "kalman_filter_node");
  KalmanFilterNode obj;

  ros::spin();

  return 0;
};