# ROS2 Interop Demo

Demonstrates IBMW publishing and subscribing ROS2 message types over the DDS
extension in `ros2_compatible_mode`, without using `rcl`, `rclcpp`, `rmw`, or
ROS2 client-library runtime APIs in the generated IBMW app.

The source of truth is
[`ros2_interop_demo.ibmw.yaml`](./ros2_interop_demo.ibmw.yaml). It generates a
low-level DDS/ROS2-compatible `std_msgs::msg::String` publisher/subscriber pair
for topic `chatter`.

## Architecture

```text
IBMW generated app
  ├─ PublisherModule   (direct_ros2_publisher, std_msgs::msg::String)
  ├─ SubscriberModule  (direct_ros2_subscriber, std_msgs::msg::String)
  └─ dds messaging driver with ros2_compatible: true
       ↓
     FastDDS wire format and ROS2-compatible topic/type naming
       ↓
     ROS2 peers using rmw_fastrtps-compatible DDS discovery
```

IBMW uses its own runtime and DDS extension. ROS2 peers may use their normal
ROS2 stack; the generated IBMW binaries stay on the pure-DDS side.

## What Gets Generated

From `ros2_interop_demo.ibmw.yaml`, `ibmw-gen` produces:

- `PublisherModule` with low-level `ibmw::ros2::transport::RegisterSendType`
  and `ibmw::ros2::transport::Send`
- `SubscriberModule` with low-level `ibmw::ros2::transport::OnMessage`
- App-mode binaries:
  - `ros2_interop_app` for combined pub/sub
  - `ros2_pub_only_app` for publisher-only runs
  - `ros2_sub_only_app` for subscriber-only runs
- Pkg-mode configs:
  - `ros2_interop_pkg.yaml`
  - `publisher_module_pkg.yaml`
  - `subscriber_module_pkg.yaml`
- DDS messaging driver config with:
  - `ros2_compatible: true`
  - `ros2_topic_prefix: "rt"`
- CMake targets linked against `ibmw::extensions::ros2_types` with
  whole-archive registration for `ibmw_extensions_ros2_types_impl`

## Key YAML Shape

```yaml
messages:
  std_msgs::msg::String:
    wire:
      ros2:
        include: "std_msgs/msg/string.hpp"
        type: "std_msgs/msg/String"

dds_driver:
  domain_id: 0
  ros2_compatible_mode: true
  ros2_topic_prefix: "rt"

modules:
  - name: PublisherModule
    type: direct_ros2_publisher
    topic: "chatter"
    message: "std_msgs::msg::String"
  - name: SubscriberModule
    type: direct_ros2_subscriber
    topic: "chatter"
    message: "std_msgs::msg::String"
```

`direct_ros2_publisher` and `direct_ros2_subscriber` are the current canonical
module types. The older `ros2_interop_publisher` / `ros2_interop_subscriber`
names remain deprecated aliases in `ibmw-gen` documentation, but this sample no
longer uses them.

## Generate

```bash
cd src/samples/cpp/ros2_interop_demo
python3 ../../../tools/ibmw_gen/ibmw_gen.py ros2_interop_demo.ibmw.yaml -o ros2_interop_demo_gen
```

## Build

Native builds may source ROS2 Humble for message headers and generator metadata.
For aarch64 cross builds, use the source-built dependency-prefix workflow from
`docs/cross-compilation.md` instead of the host ROS2 install.

```bash
cd ros2_interop_demo_gen
mkdir -p build && cd build
cmake .. -DCMAKE_PREFIX_PATH=<ibmw-build-or-install-dir>
make -j$(nproc)
```

## Run App Mode

From `ros2_interop_demo_gen/build`:

```bash
# Combined publisher + subscriber
./ros2_interop_app cfg/ros2_interop_app.yaml

# Publisher-only
./ros2_pub_only_app cfg/ros2_pub_only_app.yaml

# Subscriber-only
./ros2_sub_only_app cfg/ros2_sub_only_app.yaml
```

## Run Pkg Mode

From `ros2_interop_demo_gen/build`:

```bash
# Combined publisher + subscriber package
ibmw run cfg/ros2_interop_pkg.yaml

# Publisher-only package
ibmw run cfg/publisher_module_pkg.yaml

# Subscriber-only package
ibmw run cfg/subscriber_module_pkg.yaml
```

## ROS2 CLI Smoke Checks

When a compatible ROS2 environment is present, the DDS-compatible topic should
appear as `/chatter` to ROS2 tools:

```bash
ros2 topic list
ros2 topic info /chatter
ros2 topic echo /chatter
```

Typical topic info should report:

```text
Type: std_msgs/msg/String
```

Discovery must use the same domain id. This demo defaults to `domain_id: 0`.

## Generated Config Highlights

The generated App-mode configs load `libibmw_dds_transport.so` and preserve the
ROS2-compatible DDS options from the YAML:

```yaml
ibmw:
  extensions:
    items:
      - name: dds_extension
        path: libibmw_dds_transport.so
        options:
          domain_id: 0
          ros2_compatible_mode: true
          ros2_topic_prefix: "rt"
```

## Generated Code Highlights

The publisher registers and sends the ROS2 wire type directly:

```cpp
bool success = ibmw::ros2::transport::RegisterSendType<std_msgs::msg::String>(
    publisher_, "std_msgs/msg/String");

ibmw::ros2::transport::Send(
    publisher_, "std_msgs/msg/String", msg);
```

The subscriber registers a typed callback directly on the DDS subscriber:

```cpp
bool success = ibmw::ros2::transport::OnMessage<std_msgs::msg::String>(
    subscriber,
    "std_msgs/msg/String",
    [this](ibmw::transport::ContextRef ctx,
           const std::shared_ptr<const std_msgs::msg::String>& msg) {
      OnMessage(ctx, msg);
    });
```

## Scope and Limitations

- This sample covers ROS2-compatible MSG pub/sub over DDS.
- It does not use `rcl`, `rclcpp`, `rmw`, or ROS2 executor APIs in the IBMW
  generated app.
- It validates the generated IBMW side; ROS2 CLI interop depends on a matching
  ROS2 peer environment and compatible FastDDS/RMW discovery settings.
- Zero-copy plain-type flows are demonstrated separately by
  `ros2_point_demo` / `loaned_message_demo`; this sample intentionally uses
  `std_msgs::msg::String` for the basic compatibility path.
