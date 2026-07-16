Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# DDS/ROS2 Application Checklist

- [ ] Inventory exists: `test -f reports/inventory.md`
- [ ] DDS/ROS2 YAML exists: `ls dds_ros2_application/**/*.ibmw.yaml`
- [ ] Adapter exists: `ls adapters/*`
- [ ] User-region calls adapter: `rg -n 'adapter|Adapter|handle_|process_' dds_ros2_application adapters`
- [ ] Runtime rcl-free audit clean:
  `rg -n 'rclcpp|rcl_|RCLCPP_|create_(publisher|subscription|service|client|wall_timer)|spin\(' dds_ros2_application adapters`
- [ ] Pub/sub PASS if in scope:
  `rg -n 'DDS_ROS2_PUBSUB_PASS' reports/runtime_logs/*.log`
- [ ] Service PASS if in scope:
  `rg -n 'DDS_ROS2_SERVICE_PASS' reports/runtime_logs/*.log`
- [ ] Native ROS2 code, if any, is only harness/peer; evidence path recorded in
  `reports/migration_report.md`.
- [ ] Each dynamic/large topic has a recorded payload budget: measured actual
  serialized bytes, conservative maximum, margin, and the distinct serializer
  validation maximum where one exists; evidence is in `reports/inventory.md`.
- [ ] The registered DDS serialized type ceiling is recorded and is at least the
  payload budget; any increase is an app-local type-support override rather than
  an unexplained global default change.
- [ ] An intended external ROS2/DDS observer received and validated a
  representative maximum-size payload; the receive log path is recorded in
  `reports/migration_report.md`, and discovery alone is not accepted.
