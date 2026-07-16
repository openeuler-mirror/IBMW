# LoanedMessage Demo Troubleshooting

## 1. Build Issues

### DDS extension header not found
```
fatal error: dds_ext/ros2_style_api.h: No such file or directory
```
Ensure the following is in CMakeLists.txt:
```cmake
target_include_directories(your_module PRIVATE ${CMAKE_SOURCE_DIR}/src/extensions)
```

### Link error - FastDDS symbols
```
undefined reference to `eprosima::fastdds::dds::DataWriter::loan_sample'
```
Ensure `fastdds::fastdds` is linked.

### CMake configuration error
The following options must be enabled:
```bash
cmake .. -DIBMW_BUILD_WITH_ROS2=ON -DIBMW_BUILD_DDS_EXTENSION=ON -DIBMW_BUILD_EXAMPLES=ON
```

## 2. Runtime Issues

### libibmw_dds_extension.so not found
```bash
export LD_LIBRARY_PATH=build:$LD_LIBRARY_PATH
# or
source /opt/ros/humble/setup.bash
```

### DDS Backend is nullptr
Check the extension section in the configuration file:
```yaml
ibmw:
  extensions:
    items:
      - name: dds_extension
        path: ./libibmw_dds_extension.so
```

### Zero-copy ratio is 0%
Possible causes:
1. SHM transport not enabled -- check whether `/dev/shm` exists
2. DATA_SHARING QoS not configured
3. The type is not a Plain type

How to check:
```bash
ls -la /dev/shm
df -h /dev/shm
```

### borrow_loaned_message() returns invalid
- STRICT mode + non-Plain type: expected behavior
- loan_sample failure: SHM not enabled
- DataWriter is null: check that the topic name matches

### Non-Plain type publish failure
Non-Plain types (e.g., `std_msgs::msg::String`) cannot be published via `DataWriter::write(void*)`.
The traditional `ibmw::ros2::transport::Send<T>()` API must be used instead.

## 3. Configuration Issues

### Configuration not taking effect
- Check that the configuration file path is correct (use absolute paths)
- Validate the YAML format: `python3 -c "import yaml; yaml.safe_load(open('config.yaml'))"`
- Check the configuration loading information in the logs

### Topic name mismatch
In ROS2 compatibility mode, an IBMW topic `point_topic` is converted to DDS topic `rt/point_topic`.
Ensure `ros2_compatible: true` and `ros2_topic_prefix: "rt"` are configured correctly.

## 4. ROS2 Interoperability Issues

### ROS2 cannot see the topic
- Check that Domain IDs match: `export ROS_DOMAIN_ID=0`
- Check the DDS implementation: `echo $RMW_IMPLEMENTATION` (should be `rmw_fastrtps_cpp`)
- Topic names include `rt/` prefix: `ros2 topic echo /rt/point_topic`

## 5. DDS Shutdown Crash
An occasional "double free" during shutdown is a known FastDDS issue and does not affect functionality.

## 6. Debugging Tips

Enable verbose logging:
```yaml
log:
  core_lvl: DEBUG
```

Inspect DDS discovery:
```bash
ros2 topic list
ros2 topic info /rt/point_topic -v
```
