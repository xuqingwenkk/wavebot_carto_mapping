-- Copyright 2016 The Cartographer Authors
--
-- Licensed under the Apache License, Version 2.0 (the "License");
-- you may not use this file except in compliance with the License.
-- You may obtain a copy of the License at
--
--      http://www.apache.org/licenses/LICENSE-2.0
--
-- Unless required by applicable law or agreed to in writing, software
-- distributed under the License is distributed on an "AS IS" BASIS,
-- WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
-- See the License for the specific language governing permissions and
-- limitations under the License.

include "map_builder.lua"
include "trajectory_builder.lua"

options = {
  map_builder = MAP_BUILDER,
  trajectory_builder = TRAJECTORY_BUILDER,
  map_frame = "map",
  tracking_frame = "base_link",
  published_frame = "odom",
  odom_frame = "odom",
  provide_odom_frame = false,
  use_odometry = true,
  num_laser_scans = 1,
  num_multi_echo_laser_scans = 0,
  num_subdivisions_per_laser_scan = 1,
  num_point_clouds = 0,
  lookup_transform_timeout_sec = 0.2,
  submap_publish_period_sec = 0.3,
  pose_publish_period_sec = 5e-3,
  trajectory_publish_period_sec = 30e-3,
}

MAP_BUILDER.use_trajectory_builder_2d = true


-- Trust the constant velocity model more (this is putting more weight on odometry)
TRAJECTORY_BUILDER_2D.ceres_scan_matcher.translation_weight = 70
TRAJECTORY_BUILDER_2D.ceres_scan_matcher.rotation_weight = 300

TRAJECTORY_BUILDER_2D.min_range = 0.3
TRAJECTORY_BUILDER_2D.max_range = 8.

-- Made submaps bigger since the laser is not so good made resolution bigger -> less features.
--TRAJECTORY_BUILDER_2D.submaps.num_range_data = 35
--TRAJECTORY_BUILDER_2D.submaps.resolution = 0.02        

-- TRAJECTORY_BUILDER_2D.missing_data_ray_length = 1.
TRAJECTORY_BUILDER_2D.use_imu_data = false
TRAJECTORY_BUILDER_2D.use_online_correlative_scan_matching = false
--TRAJECTORY_BUILDER_2D.real_time_correlative_scan_matcher.linear_search_window = 0.1
--TRAJECTORY_BUILDER_2D.real_time_correlative_scan_matcher.translation_delta_cost_weight = 10.
--TRAJECTORY_BUILDER_2D.real_time_correlative_scan_matcher.rotation_delta_cost_weight = 1e-1

SPARSE_POSE_GRAPH.optimization_problem.huber_scale = 1e1
SPARSE_POSE_GRAPH.optimize_every_n_scans = 35
SPARSE_POSE_GRAPH.constraint_builder.min_score = 1.0
--SPARSE_POSE_GRAPH.constraint_builder.fast_correlative_scan_matcher.linear_search_window = 5.
--SPARSE_POSE_GRAPH.constraint_builder.ceres_scan_matcher.covariance_scale = 1e-2

SPARSE_POSE_GRAPH.constraint_builder.ceres_scan_matcher = TRAJECTORY_BUILDER_2D.ceres_scan_matcher
SPARSE_POSE_GRAPH.constraint_builder.global_localization_min_score = 1.0

return options
