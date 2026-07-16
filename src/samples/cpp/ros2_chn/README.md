# ROS2 Transport Examples

## ROS2 Transport

A basic transport example using the ROS2 msg protocol with a local backend. This demo covers:
- How to use the ROS2 msg protocol as the transport protocol;
- How to use Executor, Transport publish, and subscribe features in Module mode;
- How to use a local transport backend;
- How to integrate Modules in Pkg mode and launch them;


Core code:
- [example_ros2/msg/RosTestMsg.msg](../../../schema/ros2/example_ros2/msg/RosTestMsg.msg)
- [normal_publisher_module.cc](./managed/normal_publisher_module/normal_publisher_module.cc)
- [normal_subscriber_module.cc](./managed/normal_subscriber_module/normal_subscriber_module.cc)
- [ros2_chn_pub_pkg/pkg_main.cc](./package/ros2_chn_pub_pkg/pkg_main.cc)
- [ros2_chn_sub_pkg/pkg_main.cc](./package/ros2_chn_sub_pkg/pkg_main.cc)


Configuration file:
- [examples_cpp_ros2_chn_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_chn_cfg.yaml)


How to run (Linux):
- Build IBMW with `IBMW_BUILD_EXAMPLES` and `IBMW_BUILD_WITH_ROS2` enabled;
- Run the `start_examples_cpp_ros2_chn.sh` script in the build directory to start the process;
- Press `ctrl-c` to stop the process;


Notes:
- This example creates the following two modules:
  - `NormalPublisherModule`: Uses the `work_thread_pool` executor to publish `RosTestMsg` messages to the configured topic at the configured frequency;
  - `NormalSubscriberModule`: Subscribes to `RosTestMsg` messages on the configured topic;
- This example integrates `NormalPublisherModule` and `NormalSubscriberModule` into two separate packages `ros2_chn_pub_pkg` and `ros2_chn_sub_pkg`, and loads both into a single IBMW process via the configuration file;
- This example uses a local transport backend for communication;



## ROS2 Transport Single Pkg


A basic transport example using the ROS2 msg protocol with a local backend. This demo covers:
- How to use the ROS2 msg protocol as the transport protocol;
- How to use Executor, Transport publish, and subscribe features in Module mode;
- How to use a local transport backend;
- How to integrate Modules in Pkg mode and launch them;


Core code:
- [example_ros2/msg/RosTestMsg.msg](../../../schema/ros2/example_ros2/msg/RosTestMsg.msg)
- [normal_publisher_module.cc](./managed/normal_publisher_module/normal_publisher_module.cc)
- [normal_subscriber_module.cc](./managed/normal_subscriber_module/normal_subscriber_module.cc)
- [ros2_chn_pkg/pkg_main.cc](./package/ros2_chn_pkg/pkg_main.cc)


Configuration file:
- [examples_cpp_ros2_chn_single_pkg_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_chn_single_pkg_cfg.yaml)


How to run (Linux):
- Build IBMW with `IBMW_BUILD_EXAMPLES` and `IBMW_BUILD_WITH_ROS2` enabled;
- Run the `start_examples_cpp_ros2_chn_single_pkg.sh` script in the build directory to start the process;
- Press `ctrl-c` to stop the process;


Notes:
- This example is essentially the same as the **ROS2 Transport** example, with the only difference being that `NormalPublisherModule` and `NormalSubscriberModule` are integrated into a single package `ros2_chn_pkg`;

## ROS2 Transport Publisher App


A transport example using the ROS2 msg protocol with a local backend. This demo covers:
- How to use the ROS2 msg protocol as the transport protocol;
- How to use Transport publish functionality by creating a module in App mode;
- How to use Transport subscribe functionality in Module mode;
- How to use a local transport backend;
- How to launch in App mode;


Core code:
- [RosTestMsg.msg](../../../schema/ros2/example_ros2/msg/RosTestMsg.msg)
- [normal_ros2_chn_publisher_app/main.cc](./standalone/normal_ros2_chn_publisher_app/main.cc)
- [normal_subscriber_module.cc](./managed/normal_subscriber_module/normal_subscriber_module.cc)
- [ros2_chn_pub_pkg/pkg_main.cc](./package/ros2_chn_pub_pkg/pkg_main.cc)


Configuration file:
- [examples_cpp_ros2_chn_publisher_app_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_chn_publisher_app_cfg.yaml)


How to run (Linux):
- Build IBMW with `IBMW_BUILD_EXAMPLES` and `IBMW_BUILD_WITH_ROS2` enabled;
- Run the `start_examples_cpp_ros2_chn_publisher_app.sh` script in the build directory to start the process;
- Press `ctrl-c` to stop the process;


Notes:
- In App mode, this example directly creates a `NormalPublisherModule`, obtains a CoreRef handle, and publishes `RosTestMsg` messages to the configured topic at the configured frequency;
- This example uses a local transport backend for communication;
- This example launches in App mode;


## ROS2 Transport Subscriber App


A transport example using the ROS2 msg protocol with a local backend. This demo covers:
- How to use the ROS2 msg protocol as the transport protocol;
- How to use Transport subscribe functionality by creating a module in App mode;
- How to use a local transport backend;
- How to launch in App mode;


Core code:
- [RosTestMsg.msg](../../../schema/ros2/example_ros2/msg/RosTestMsg.msg)
- [normal_ros2_chn_subscriber_app/main.cc](./standalone/normal_ros2_chn_subscriber_app/main.cc)
- [normal_publisher_module.cc](./managed/normal_publisher_module/normal_publisher_module.cc)
- [ros2_chn_pub_pkg/pkg_main.cc](./package/ros2_chn_pub_pkg/pkg_main.cc)



Configuration file:
- [examples_cpp_ros2_chn_subscriber_app_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_chn_subscriber_app_cfg.yaml)


How to run (Linux):
- Build IBMW with `IBMW_BUILD_EXAMPLES` and `IBMW_BUILD_WITH_ROS2` enabled;
- Run the `start_examples_cpp_ros2_chn_subscriber_app.sh` script in the build directory to start the process;
- Press `ctrl-c` to stop the process;


Notes:
- In App mode, this example directly creates a `NormalSubscriberModule`, obtains a CoreRef handle, and subscribes to `RosTestMsg` messages on the configured topic;
- This example uses a local transport backend for communication;
- This example launches in App mode;

---

## ibmw-gen Way

This demo can also be generated using `ibmw-gen` from the
[`ros2_chn.ibmw.yaml`](./ros2_chn.ibmw.yaml) definition.

### What ibmw-gen generates vs what you write

| Generated (auto) | You write |
|-------------------|-----------|
| `CMakeLists.txt` -- build system with ROS2 package linking | `normal_publisher_module.cc` -- publish logic |
| `cfg/ros2_chn_app.yaml` -- App-mode config | `normal_subscriber_module.cc` -- subscribe callback |
| `cfg/ros2_chn_pkg.yaml` -- Pkg-mode config | Module `.h` headers |
| `src/ros2_chn_app/main.cc` -- App entry point | |
| `src/*_pkg/pkg_main.cc` -- Pkg loaders | |

### Generate, build, and run

```bash
# Generate
python3 <ibmw-gen>/ibmw_gen.py ros2_chn.ibmw.yaml -o ros2_chn_gen

# Build
source /opt/ros/humble/setup.bash
mkdir -p ros2_chn_gen/build && cd ros2_chn_gen/build
cmake .. -DCMAKE_PREFIX_PATH=<ibmw_build_dir>
make -j$(nproc)

# Run (App mode)
./ros2_chn_app cfg/ros2_chn_app.yaml

# Run (Pkg mode)
ibmw run cfg/ros2_chn_pkg.yaml
```

### Key YAML features

```yaml
modules:
  - name: NormalPublisherModule
    type: publisher
    transport: ros2                                 # ROS2 transport backend
    topic: test_topic
    message_type: "example_ros2::msg::RosTestMsg"
    ros2_msg_include: "example_ros2/msg/ros_test_msg.hpp"  # ROS2 message header
    ros2_package: example_ros2                             # ROS2 package dependency
```

ibmw-gen generates `SenderAccess<example_ros2::msg::RosTestMsg>` wrappers and
CMake `find_package(example_ros2)` integration. The module source files contain
TODO markers for the actual publish timer and subscribe callback logic.

**Note:** The benchmark publisher/subscriber modules are NOT included in the
ibmw-gen version because they require custom config parsing, dynamic executor
pools, and multi-topic benchmarking logic beyond the current generator scope.
