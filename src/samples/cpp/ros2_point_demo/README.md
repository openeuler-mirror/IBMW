# ROS2 Point Demo

Demonstrates zero-copy communication with ROS2 Plain types (`geometry_msgs::msg::Point`) using the IBMW DDS plugin.

## Dependencies

- ROS2 Humble (`source /opt/ros/humble/setup.bash`)
- geometry_msgs message package

## Build

```bash
cd <IBMW_ROOT>
source /opt/ros/humble/setup.bash

cmake -B build \
  -DIBMW_BUILD_EXAMPLES=ON \
  -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_BUILD_WITH_ROS2=ON

cmake --build build --target ros2_point_app
```

## Run

```bash
source /opt/ros/humble/setup.bash
cd build
./ros2_point_app ../src/samples/cpp/ros2_point_demo/config/cfg/ros2_point_cfg.yaml
```

## Zero-Copy Details

`geometry_msgs::msg::Point` is a Plain type (`is_trivially_copyable` + `is_standard_layout`),
and the plugin automatically detects and enables zero-copy optimization:

- Publisher: `loan_sample` + `memcpy` (1-copy)
- Subscriber: `TakeLoan` (0-copy)

## ROS2 Interoperability

```bash
# Terminal 1: IBMW publish
./ros2_point_app ../src/samples/cpp/ros2_point_demo/config/cfg/ros2_point_cfg.yaml

# Terminal 2: ROS2 subscribe
ros2 topic echo /rt/point_topic geometry_msgs/msg/Point
```

## Configuration

See `config/cfg/ros2_point_cfg.yaml`.

---

## ibmw-gen Way

This demo can also be generated using `ibmw-gen` from the
[`ros2_point_demo.ibmw.yaml`](./ros2_point_demo.ibmw.yaml) definition.

### What ibmw-gen generates vs what you write

| Generated (auto) | You write |
|-------------------|-----------|
| `CMakeLists.txt` -- with geometry_msgs + ros2_types_impl linking | `point_publisher_module.cc` -- Point publish logic |
| `cfg/ros2_point_app.yaml` -- App config with DDS ROS2 compat | `point_subscriber_module.cc` -- Point subscribe callback |
| `cfg/ros2_point_pkg.yaml` -- Pkg-mode config | Module `.h` headers |
| `src/ros2_point_app/main.cc`, `src/*_pkg/pkg_main.cc` | |

### Key YAML features

```yaml
dds_driver:
  domain_id: 0
  ros2_compatible_mode: true
  ros2_topic_prefix: "rt"

modules:
  - name: PointPublisherModule
    type: ros2_interop_publisher
    topic: "point_topic"
    message_type: "geometry_msgs::msg::Point"
    ros2_msg_include: "geometry_msgs/msg/point.hpp"
    ros2_type_name: "geometry_msgs/msg/Point"
    ros2_packages: [geometry_msgs]       # Additional ROS2 package dependency
```

The `ros2_packages` field tells ibmw-gen to add `find_package(geometry_msgs)` and
link `${geometry_msgs_TARGETS}` in the generated CMakeLists.txt. `std_msgs` is
already a transitive dependency of `ibmw::extensions::ros2_types` and does not
need to be listed.
