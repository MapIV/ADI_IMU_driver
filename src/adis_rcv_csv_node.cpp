// Copyright (c) 2017, Analog Devices Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// * Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright
//   notice, this list of conditions and the following disclaimer in
//   the documentation and/or other materials provided with the
//   distribution.
//
// * Neither the name of the copyright holder nor the names of its
//   contributors may be used to endorse or promote products derived
//   from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
// OF THE POSSIBILITY OF SUCH DAMAGE.

#include <string>
#include <ros/ros.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/Temperature.h>
#include <tf/transform_broadcaster.h>
#include <diagnostic_updater/diagnostic_updater.h>

#include "adis_rcv_csv.h"

class ImuNodeRcvCsv
{
public:
  AdisRcvCsv imu_;
  ros::NodeHandle node_handle_;
  ros::Publisher imu_data_pub_;
  diagnostic_updater::Updater updater_;

  std::string device_;
  std::string frame_id_;
  std::string parent_id_;
  double rate_;
  int cant_rcv_cnt_;
   
  explicit ImuNodeRcvCsv(ros::NodeHandle nh)
    : node_handle_(nh)
  {
    // Read parameters
    node_handle_.param("device", device_, std::string("/dev/ttyACM0"));
    node_handle_.param("frame_id", frame_id_, std::string("imu"));
    node_handle_.param("parent_id", parent_id_, std::string("base_link"));
    node_handle_.param("rate", rate_, 100.0);

    ROS_INFO("device: %s", device_.c_str());
    ROS_INFO("frame_id: %s", frame_id_.c_str());
    ROS_INFO("rate: %.1f [Hz]", rate_);

    cant_rcv_cnt_ = 0;
    
    // Data publisher
    imu_data_pub_ = node_handle_.advertise<sensor_msgs::Imu>("data_raw", 100);
    updater_.add("imu", this, &ImuNodeRcvCsv::Diagnostic);
  }

  ~ImuNodeRcvCsv()
  {
    imu_.ClosePort();
  }

  void Diagnostic(diagnostic_updater::DiagnosticStatusWrapper &stat) {
    if (cant_rcv_cnt_ >= 1) 
    {
      stat.summaryf(diagnostic_msgs::DiagnosticStatus::ERROR,
                    "Data cannot be received for more than 1 second.");
    }
    else 
    {
      stat.summaryf(diagnostic_msgs::DiagnosticStatus::OK, "OK");
    }
  }

  /**
   * @brief Check if the device is opened
   */
  bool IsOpened(void)
  {
    return (imu_.fd_ >= 0);
  }
  /**
   * @brief Open IMU device file
   */
  bool Open(void)
  {
    // Open device file
    if (imu_.OpenPort(device_) < 0)
    {
      ROS_ERROR("Failed to open device %s", device_.c_str());
      return false;
    }
    return true;
  }

  std::string SendCmd(const std::string& cmd, bool is_print = true) 
  {
    std::string ret_str = ""; 
    ret_str = imu_.SendAndRetCmd(cmd);
    if (is_print) 
    {
      ROS_INFO("%s = %s", cmd.c_str(), ret_str.c_str());
    }
    return ret_str;
  }

  bool CheckFormat() 
  {
    auto ret_str = SendCmd("GET_FORMAT"); 
    if (ret_str == "X_GYRO_HEX,Y_GYRO_HEX,Z_GYRO_HEX,X_ACC_HEX,Y_ACC_HEX,Z_ACC_HEX,CSUM") 
    {
      imu_.md_ = AdisRcvCsv::Mode::Register;
    }
    else if (ret_str == "YAW[deg],PITCH[deg],ROLL[deg]")
    {
      imu_.md_ = AdisRcvCsv::Mode::YPR;
    }
    else
    {
      imu_.md_ = AdisRcvCsv::Mode::Unknown;
      ROS_WARN("Invalid data format!");
      return false;
    }
    return true;
  }

  void GetProductId()
  {
    auto ret_str = SendCmd("GET_PROD_ID");
    updater_.setHardwareID(ret_str);
  }

  void PrintFirmVersion()
  {
    auto ret_str = SendCmd("GET_VERSION"); 
  }

  bool CheckStatus()
  {
    auto ret_str = SendCmd("GET_STATUS"); 
    if (ret_str == "Running") 
    {
      ROS_WARN("Imu state is Running. Send stop command.");
      SendCmd("stop", /* is_print */false);
      return false;
    } 
    else if (ret_str != "Ready") 
    {
      ROS_WARN("Invalid imu state.");
      return false;
    }
    return true;
  }

  bool CheckSensitivity()
  {
    std::string ret_str = "";
    ret_str = SendCmd("GET_SENSI"); 
    if (ret_str == "") 
    {
      ROS_WARN("Could not get sensitivities!");
      return false;
    } 
    else
    {
     if (!imu_.SetSensi(ret_str)) 
     {
      ROS_WARN("Insufficient number of sensitivities.");
      return false;
      }
    }
    return true;
  }

  bool IsPrepared(void)
  {
    return (imu_.st_ == AdisRcvCsv::State::Running);
  }

  bool Prepare(void)
  {
    PrintFirmVersion();
    GetProductId();
    // check imu state
    if (!CheckStatus())
    {
      return false;
    }

    imu_.st_ = AdisRcvCsv::State::Ready;

    // check data format
    if (!CheckFormat())
    {
      return false;
    }

    // check sensitivity of gyro and acc
    if (imu_.md_ == AdisRcvCsv::Mode::Register)
    {
      if (!CheckSensitivity()) 
      {
        return false;
      }
    }

    auto ret_str = SendCmd("start", /* is_print */false);
    if (ret_str != "start") 
    {
      ROS_WARN("Send start cmd. but imu was not started.");
      return false;
    }

    ROS_INFO("Start imu!");
    imu_.st_ = AdisRcvCsv::State::Running;

    return true;
  }

  int PubImuData()
  {
    sensor_msgs::Imu data;
    data.header.frame_id = frame_id_;
    data.header.stamp = ros::Time::now();

    // Linear acceleration
    data.linear_acceleration.x = imu_.accl_[0];
    data.linear_acceleration.y = imu_.accl_[1];
    data.linear_acceleration.z = imu_.accl_[2];

    // Angular velocity
    data.angular_velocity.x = imu_.gyro_[0];
    data.angular_velocity.y = imu_.gyro_[1];
    data.angular_velocity.z = imu_.gyro_[2];

    // Orientation (not provided)
    data.orientation.x = 0;
    data.orientation.y = 0;
    data.orientation.z = 0;
    data.orientation.w = 1;

    imu_data_pub_.publish(data);
  }

  void BroadcastImuPose()
  {
    static tf::TransformBroadcaster br;
    tf::Transform transform;
    transform.setOrigin(tf::Vector3(0, 0, 0));
    tf::Quaternion q;
    q.setRPY(imu_.ypr_[2]*DEG2RAD, imu_.ypr_[1]*DEG2RAD, imu_.ypr_[0]*DEG2RAD);
    transform.setRotation(q);
    br.sendTransform(tf::StampedTransform(transform, ros::Time::now(), parent_id_, frame_id_));
  }

  void UpdateAndPubRegMode() 
  {
    int res = imu_.UpdateRegMode();
    if (res == IMU_OK)
    {
      PubImuData();
      cant_rcv_cnt_ = 0;
    }
    else    
    {
      if (res = IMU_ERR_CANT_RCV_DATA) cant_rcv_cnt_++;

      ROS_ERROR("Cannot update on reg mode");
    }
  }

  void UpdateAndPubYprMode() 
  {
    int res = imu_.UpdateYprMode();
    if (res == IMU_OK)
    {
      BroadcastImuPose();
      cant_rcv_cnt_ = 0;
    }
    else
    {
      if (res = IMU_ERR_CANT_RCV_DATA) cant_rcv_cnt_++;

      ROS_ERROR("Cannot update on ypr mode");
    }
  }

  bool Spin()
  {
    ros::Rate loop_rate(rate_);
    while (ros::ok())
    {
      ros::spinOnce();
      switch (imu_.md_)
      {
        case AdisRcvCsv::Mode::Register:
          UpdateAndPubRegMode();
          break;
        case AdisRcvCsv::Mode::YPR:
          UpdateAndPubYprMode();
          break;
        default:
          ROS_WARN("Unknown imu mode");
          break;
      }
      updater_.update();
      loop_rate.sleep();
    }

    imu_.st_ = AdisRcvCsv::State::Ready;
    imu_.md_ = AdisRcvCsv::Mode::Unknown;
    imu_.SendCmd("stop");
    return true;
  }
};

int main(int argc, char** argv)
{
  ros::init(argc, argv, "imu");
  ros::NodeHandle nh("~");

  ImuNodeRcvCsv node(nh);

  node.Open();
  while (ros::ok() && !node.IsOpened())
  {
    ROS_WARN("Keep trying to open the device in 1 second period...");
    ros::Duration(1).sleep();
    node.Open();
  }

  node.Prepare();  
  while (ros::ok() && !node.IsPrepared())
  {
    ROS_WARN("Keep trying to prepare the device in 1 second period...");
    ros::Duration(1).sleep();
    node.Prepare();
  }

  node.Spin();
  return(0);
}
