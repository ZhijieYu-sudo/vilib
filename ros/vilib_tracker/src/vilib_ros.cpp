/*
 * Copyright (c) 2019-2021 Philipp Foehn,
 * Robotics and Perception Group, University of Zurich
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "vilib_tracker/vilib_ros.hpp"

#include <ros/ros.h>

#include <unordered_map>

#include "vilib/common/frame.h"
#include "vilib/common/framebundle.h"
#include "vilib/storage/pyramid_pool.h"
#include "vilib_msgs/Features.h"


namespace vilib {


VilibRos::VilibRos(const ros::NodeHandle &nh, const ros::NodeHandle &pnh)
  : nh_(nh), pnh_(pnh), it_(pnh_), params_(pnh) {
  if (!params_.valid()) {
    logger_.error("Could not load valid parameter set!");
    ros::shutdown();
  }

  timer_frame_.nest(timer_copy_);
  timer_frame_.nest(timer_tracking_);
  timer_frame_.nest(timer_publish_);
  timer_frame_.nest(timer_visualize_);

  image_sub_ = it_.subscribe("image", 20, &VilibRos::imageCallback, this);
  if (params_.publish_debug_image)
    image_pub_ = it_.advertise("debug_image", 1, false);
  features_pub_ = nh_.advertise<vilib_msgs::Features>("features", 10);

  PyramidPool::init(1, params_.image_width, params_.image_height, 1,
                    params_.numberOfPyramidLevels(), IMAGE_PYRAMID_MEMORY_TYPE);

  feature_tracker_ = params_.createFeatureTracker();
  feature_detector_ = params_.createFeatureDetector();
  feature_tracker_->setDetectorGPU(feature_detector_, 0);
}

VilibRos::~VilibRos() {
  PyramidPool::deinit();
  if(params_.log_stats < 0) printStats();
}

void VilibRos::imageCallback(const sensor_msgs::ImageConstPtr &msg) {
  timer_frame_.tic();

  timer_copy_.tic();
  const std::shared_ptr<Frame> frame =
    std::make_shared<Frame>(msg, params_.numberOfPyramidLevels());
  const std::shared_ptr<FrameBundle> frame_bundle =
    std::make_shared<FrameBundle>(std::vector<std::shared_ptr<Frame>>({frame}));
  timer_copy_.toc();

  timer_tracking_.tic();
  size_t n_tracked, n_detected;
  feature_tracker_->track(frame_bundle, n_tracked, n_detected);
  stats_tracked_ << (double)n_tracked;
  stats_detected_ << (double)n_detected;
  timer_tracking_.toc();

  timer_publish_.tic();
  publishFeatures(msg->header.stamp, frame);
  timer_publish_.toc();

  if (params_.publish_debug_image) {
    timer_visualize_.tic();
    cv_bridge::CvImagePtr debug_image = cv_bridge::toCvCopy(msg, "bgr8");
    visualize(frame, debug_image->image);
    image_pub_.publish(debug_image->toImageMsg());
    timer_visualize_.toc();
  }

  if(params_.log_stats > 1) printStats();
  timer_frame_.toc();
}

void VilibRos::publishFeatures(const ros::Time &frame_time,
                               const std::shared_ptr<Frame> &frame) const {
  const Eigen::Matrix<double, 2, Eigen::Dynamic> features = frame->px_vec_;
  const Eigen::VectorXi ids = frame->track_id_vec_;

  vilib_msgs::Features features_msg;
  features_msg.header.stamp = frame_time;
  features_msg.features.reserve(frame->num_features_);
  for (int i = 0; i < (int)frame->num_features_; ++i) {
    vilib_msgs::Feature feature;
    feature.id = ids(i);
    feature.x = features(0, i);
    feature.y = features(1, i);
    features_msg.features.push_back(feature);
  }
  features_pub_.publish(features_msg);
}

void VilibRos::visualize(const std::shared_ptr<Frame> &frame,
                         cv::Mat &image) const {
  const int cw = params_.cell_width;
  const int ch = params_.cell_height;
  const int n_rows = (image.rows + ch - 1) / ch;
  const int n_cols = (image.cols + cw - 1) / cw;
  for (int r = 0; r < n_rows; ++r) {
    for (int c = 0; c < n_cols; ++c) {
      cv::rectangle(image, cv::Point(c * cw, r * ch),
                    cv::Point((c + 1) * cw, (r + 1) * ch),
                    cv::Scalar(244, 215, 66),  // B,G,R
                    1, 8, 0);
    }
  }

  static int max_track_id = -1;
  static std::unordered_map<int, cv::Scalar> track_colors;

  const Eigen::Matrix<double, 2, Eigen::Dynamic> features = frame->px_vec_;
  const Eigen::VectorXi ids = frame->track_id_vec_;
  for (int i = 0; i < (int)frame->num_features_; ++i) {
    const int track_id = ids(i);
    if (max_track_id < track_id) {
      max_track_id = track_id;
      track_colors[track_id] =
        cv::Scalar(rand() % 255, rand() % 255, rand() % 255);
    }
    cv::circle(image, cv::Point(features(0, i), features(1, i)), 3,
               track_colors[track_id], 3, 8);
  }
}

void VilibRos::printStats() const {
  logger_ << '\n' << timer_frame_ << stats_tracked_ << stats_detected_;
}

}  // namespace vilib

int main(int argc, char **argv) {
  ros::init(argc, argv, "vilib");
  vilib::VilibRos vilib;

  ros::spin();
  return 0;
}
