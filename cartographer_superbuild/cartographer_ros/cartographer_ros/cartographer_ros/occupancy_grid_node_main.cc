/*
 * Copyright 2016 The Cartographer Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cmath>
#include <string>
#include <vector>

#include "Eigen/Core"
#include "Eigen/Geometry"
#include "cairo/cairo.h"
#include "cartographer/common/mutex.h"
#include "cartographer/common/port.h"
#include "cartographer/io/image.h"
#include "cartographer/mapping/id.h"
#include "cartographer/transform/rigid_transform.h"
#include "cartographer_ros/msg_conversion.h"
#include "cartographer_ros/node_constants.h"
#include "cartographer_ros/ros_log_sink.h"
#include "cartographer_ros/submap.h"
#include "cartographer_ros_msgs/SubmapList.h"
#include "cartographer_ros_msgs/SubmapQuery.h"
#include "gflags/gflags.h"
#include "nav_msgs/OccupancyGrid.h"
#include "ros/ros.h"

DEFINE_double(resolution, 0.05,
              "Resolution of a grid cell in the published occupancy grid.");

namespace cartographer_ros {
namespace {

using ::cartographer::mapping::SubmapId;

constexpr cairo_format_t kCairoFormat = CAIRO_FORMAT_ARGB32;

Eigen::Affine3d ToEigen(const ::cartographer::transform::Rigid3d& rigid3) {
  return Eigen::Translation3d(rigid3.translation()) * rigid3.rotation();
}

struct SubmapState {
  SubmapState()
      : surface(::cartographer::io::MakeUniqueCairoSurfacePtr(nullptr)) {}

  // Texture data.
  int width;
  int height;
  int version;
  double resolution;
  ::cartographer::transform::Rigid3d slice_pose;
  ::cartographer::io::UniqueCairoSurfacePtr surface;
  // Pixel data used by 'surface'. Must outlive 'surface'.
  std::vector<uint32_t> cairo_data;

  // Metadata.
  ::cartographer::transform::Rigid3d pose;
  int metadata_version = -1;
};

void CairoDrawEachSubmap(
    const double scale, std::map<SubmapId, SubmapState>* submaps, cairo_t* cr,
    std::function<void(const SubmapState&)> draw_callback) {
  cairo_scale(cr, scale, scale);

  for (auto& pair : *submaps) {
    auto& submap_state = pair.second;
    if (submap_state.surface == nullptr) {
      return;
    }
    const Eigen::Matrix4d homo =
        ToEigen(submap_state.pose * submap_state.slice_pose).matrix();

    cairo_save(cr);
    cairo_matrix_t matrix;
    cairo_matrix_init(&matrix, homo(1, 0), homo(0, 0), -homo(1, 1), -homo(0, 1),
                      homo(0, 3), -homo(1, 3));
    cairo_transform(cr, &matrix);

    const double submap_resolution = submap_state.resolution;
    cairo_scale(cr, submap_resolution, submap_resolution);
    draw_callback(submap_state);
    cairo_restore(cr);
  }
}

class Node {
 public:
  explicit Node(double resolution);
  ~Node() {}

  Node(const Node&) = delete;
  Node& operator=(const Node&) = delete;

 private:
  void HandleSubmapList(const cartographer_ros_msgs::SubmapList::ConstPtr& msg);
  void DrawAndPublish(const string& frame_id, const ros::Time& time);
  void PublishOccupancyGrid(const string& frame_id, const ros::Time& time,
                            const Eigen::Array2f& origin,
                            const Eigen::Array2i& size,
                            cairo_surface_t* surface);

  ::ros::NodeHandle node_handle_;
  const double resolution_;

  ::cartographer::common::Mutex mutex_;
  ::ros::ServiceClient client_ GUARDED_BY(mutex_);
  ::ros::Subscriber submap_list_subscriber_ GUARDED_BY(mutex_);
  ::ros::Publisher occupancy_grid_publisher_ GUARDED_BY(mutex_);
  std::map<SubmapId, SubmapState> submaps_ GUARDED_BY(mutex_);
};

Node::Node(const double resolution)
    : resolution_(resolution),
      client_(node_handle_.serviceClient<::cartographer_ros_msgs::SubmapQuery>(
          kSubmapQueryServiceName)),
      submap_list_subscriber_(node_handle_.subscribe(
          kSubmapListTopic, kLatestOnlyPublisherQueueSize,
          boost::function<void(
              const cartographer_ros_msgs::SubmapList::ConstPtr&)>(
              [this](const cartographer_ros_msgs::SubmapList::ConstPtr& msg) {
                HandleSubmapList(msg);
              }))),
      occupancy_grid_publisher_(
          node_handle_.advertise<::nav_msgs::OccupancyGrid>(
              kOccupancyGridTopic, kLatestOnlyPublisherQueueSize,
              true /* latched */))

{}

void Node::HandleSubmapList(
    const cartographer_ros_msgs::SubmapList::ConstPtr& msg) {
  ::cartographer::common::MutexLocker locker(&mutex_);

  // We do not do any work if nobody listens.
  if (occupancy_grid_publisher_.getNumSubscribers() == 0) {
    return;
  }
  for (const auto& submap_msg : msg->submap) {
    const SubmapId id{submap_msg.trajectory_id, submap_msg.submap_index};
    SubmapState& submap_state = submaps_[id];
    submap_state.pose = ToRigid3d(submap_msg.pose);
    submap_state.metadata_version = submap_msg.submap_version;
    if (submap_state.surface != nullptr &&
        submap_state.version == submap_msg.submap_version) {
      continue;
    }

    auto fetched_texture = ::cartographer_ros::FetchSubmapTexture(id, &client_);
    if (fetched_texture == nullptr) {
      continue;
    }
    submap_state.width = fetched_texture->width;
    submap_state.height = fetched_texture->height;
    submap_state.version = fetched_texture->version;
    submap_state.slice_pose = fetched_texture->slice_pose;
    submap_state.resolution = fetched_texture->resolution;

    // Properly dealing with a non-common stride would make this code much more
    // complicated. Let's check that it is not needed.
    const int expected_stride = 4 * submap_state.width;
    CHECK_EQ(expected_stride,
             cairo_format_stride_for_width(kCairoFormat, submap_state.width));
    submap_state.cairo_data.clear();
    for (size_t i = 0; i < fetched_texture->intensity.size(); ++i) {
      // We use the red channel to track intensity information. The green
      // channel we use to track if a cell was ever observed.
      const uint8_t intensity = fetched_texture->intensity.at(i);
      const uint8_t alpha = fetched_texture->alpha.at(i);
      const uint8_t observed = (intensity == 0 && alpha == 0) ? 0 : 255;
      submap_state.cairo_data.push_back((alpha << 24) | (intensity << 16) |
                                        (observed << 8) | 0);
    }

    submap_state.surface = ::cartographer::io::MakeUniqueCairoSurfacePtr(
        cairo_image_surface_create_for_data(
            reinterpret_cast<unsigned char*>(submap_state.cairo_data.data()),
            kCairoFormat, submap_state.width, submap_state.height,
            expected_stride));
    CHECK_EQ(cairo_surface_status(submap_state.surface.get()),
             CAIRO_STATUS_SUCCESS)
        << cairo_status_to_string(
               cairo_surface_status(submap_state.surface.get()));
  }
  DrawAndPublish(msg->header.frame_id, msg->header.stamp);
}

void Node::DrawAndPublish(const string& frame_id, const ros::Time& time) {
  if (submaps_.empty()) {
    return;
  }

  Eigen::AlignedBox2f bounding_box;
  {
    auto surface = ::cartographer::io::MakeUniqueCairoSurfacePtr(
        cairo_image_surface_create(kCairoFormat, 1, 1));
    auto cr =
        ::cartographer::io::MakeUniqueCairoPtr(cairo_create(surface.get()));
    const auto update_bounding_box = [&bounding_box, &cr](double x, double y) {
      cairo_user_to_device(cr.get(), &x, &y);
      bounding_box.extend(Eigen::Vector2f(x, y));
    };

    CairoDrawEachSubmap(
        1. / resolution_, &submaps_, cr.get(),
        [&update_bounding_box, &bounding_box](const SubmapState& submap_state) {
          update_bounding_box(0, 0);
          update_bounding_box(submap_state.width, 0);
          update_bounding_box(0, submap_state.height);
          update_bounding_box(submap_state.width, submap_state.height);
        });
  }

  const int kPaddingPixel = 5;
  const Eigen::Array2i size(
      std::ceil(bounding_box.sizes().x()) + 2 * kPaddingPixel,
      std::ceil(bounding_box.sizes().y()) + 2 * kPaddingPixel);
  const Eigen::Array2f origin(-bounding_box.min().x() + kPaddingPixel,
                              -bounding_box.min().y() + kPaddingPixel);

  {
    auto surface = ::cartographer::io::MakeUniqueCairoSurfacePtr(
        cairo_image_surface_create(kCairoFormat, size.x(), size.y()));
    auto cr =
        ::cartographer::io::MakeUniqueCairoPtr(cairo_create(surface.get()));
    cairo_set_source_rgba(cr.get(), 0.5, 0.0, 0.0, 1.);
    cairo_paint(cr.get());
    cairo_translate(cr.get(), origin.x(), origin.y());
    CairoDrawEachSubmap(1. / resolution_, &submaps_, cr.get(),
                        [&cr](const SubmapState& submap_state) {
                          cairo_set_source_surface(
                              cr.get(), submap_state.surface.get(), 0., 0.);
                          cairo_paint(cr.get());
                        });
    cairo_surface_flush(surface.get());
    PublishOccupancyGrid(frame_id, time, origin, size, surface.get());
  }
}

    void MedianFilterOccupancyGrid(::nav_msgs::OccupancyGrid& occupancy_grid,
                      float occupancy_threshold){
        // do nothing for border
        for(size_t y = 1; y < occupancy_grid.info.height - 1; ++y){
            for(size_t x = 1; x < occupancy_grid.info.width - 1; ++x){
                auto& cell_value = occupancy_grid.data[y*occupancy_grid.info.width + x];
                if(cell_value < 0 || cell_value > occupancy_threshold) {
                    for(size_t i = y-1; i <= y+1; i++){
                        for(size_t j = x-1; j <= x+1; j++){
                            cell_value += occupancy_grid.data[i*occupancy_grid.info.width + j];
                        }
                    }
                    cell_value /= 10;
                    if(cell_value > (occupancy_threshold)*0.1)
                        cell_value = 100;
                    else
                        cell_value = cell_value > 1 ? 0 : -1;
                }else{
                    cell_value = 0;
                }
            }
        }
    }

    void FilterOccupancyGrid(::nav_msgs::OccupancyGrid& occupancy_grid,
                             float occupancy_threshold) {
        for(size_t y = 0; y < occupancy_grid.info.height; ++y) {
            for(size_t x = 0; x < occupancy_grid.info.width; ++x) {
                auto& cell_value = occupancy_grid.data[y*occupancy_grid.info.width + x];
                size_t count = 0, no_occ = 0;
                if(cell_value < 0 || cell_value > occupancy_threshold) {
                    for(size_t i = (y>0?y-2:0);
                        i <= (y+2<occupancy_grid.info.height?y+2:occupancy_grid.info.height);
                        ++i) {
                        for(size_t j = (x>0?x-2:0);
                            j <= (x+2<occupancy_grid.info.width?x+2:occupancy_grid.info.width);
                            ++j) {
                            auto value = occupancy_grid.data[i*occupancy_grid.info.width + j];
                            ++count;
                            if(value > 0 && value < occupancy_threshold) {
                                ++no_occ;
                            }
                        }
                    }
                    if(no_occ > count/2){
                        cell_value = 0;
                    }else{
                        cell_value = cell_value > 0 ? 100 : -1;
                    }
                }else{
                    cell_value = 0;
                }
            }
        }
    }

void Node::PublishOccupancyGrid(const string& frame_id, const ros::Time& time,
                                const Eigen::Array2f& origin,
                                const Eigen::Array2i& size,
                                cairo_surface_t* surface) {
  nav_msgs::OccupancyGrid occupancy_grid;
  occupancy_grid.header.stamp = time;
  occupancy_grid.header.frame_id = frame_id;
  occupancy_grid.info.map_load_time = time;
  occupancy_grid.info.resolution = resolution_;
  occupancy_grid.info.width = size.x();
  occupancy_grid.info.height = size.y();
  occupancy_grid.info.origin.position.x = -origin.x() * resolution_;
  occupancy_grid.info.origin.position.y =
      (-size.y() + origin.y()) * resolution_;
  occupancy_grid.info.origin.position.z = 0.;
  occupancy_grid.info.origin.orientation.w = 1.;
  occupancy_grid.info.origin.orientation.x = 0.;
  occupancy_grid.info.origin.orientation.y = 0.;
  occupancy_grid.info.origin.orientation.z = 0.;

  const uint32* pixel_data =
      reinterpret_cast<uint32*>(cairo_image_surface_get_data(surface));
  occupancy_grid.data.reserve(size.x() * size.y());
  for (int y = size.y() - 1; y >= 0; --y) {
    for (int x = 0; x < size.x(); ++x) {
      const uint32 packed = pixel_data[y * size.x() + x];
      const unsigned char color = packed >> 16;
      const unsigned char observed = packed >> 8;
      const int value =
          observed == 0
              ? -1
              : ::cartographer::common::RoundToInt((1. - color / 255.) * 100.);
      CHECK_LE(-1, value);
      CHECK_GE(100, value);
      int cell_value = -1;
      if(value > 50)
          cell_value = 100;
      else
          cell_value = value != -1 ? 0 : -1;
      occupancy_grid.data.push_back(cell_value);
    }
  }

  //FilterOccupancyGrid(occupancy_grid, 50);
  occupancy_grid_publisher_.publish(occupancy_grid);
}

}  // namespace
}  // namespace cartographer_ros

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  google::ParseCommandLineFlags(&argc, &argv, true);

  ::ros::init(argc, argv, "cartographer_occupancy_grid_node");
  ::ros::start();

  cartographer_ros::ScopedRosLogSink ros_log_sink;
  ::cartographer_ros::Node node(FLAGS_resolution);

  ::ros::spin();
  ::ros::shutdown();
}
