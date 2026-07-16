Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Completeness Checklist

- [ ] Decision recorded: `test -f reports/migration_report.md`
- [ ] Inventory complete: `rg -n 'publisher|subscriber|service|client|timer|parameter|action' reports/inventory.md`
- [ ] Minimal-change adapter evidence: `rg -n 'adapters/' reports/migration_report.md`
- [ ] Build/config result recorded: `test -f reports/build_configure.md`
- [ ] Runtime report exists: `test -f reports/runtime_report.md`
- [ ] PASS markers checked:
  `rg -n 'DDS_ROS2_(PUBSUB|SERVICE)_PASS|JSON_LOCAL_(PUBSUB|SERVICE)_PASS' reports/runtime_logs/*.log`
- [ ] rcl-free DDS/ROS2 application audit checked:
  `rg -n 'rclcpp|rcl_|RCLCPP_|create_(publisher|subscription|service|client|wall_timer)|spin\(' dds_ros2_application adapters`
- [ ] Actions are converted manually or deferred with evidence:
  `rg -n 'action|Action|defer|manual' reports/migration_report.md`
- [ ] If OHOS/Route B is in scope, target audit exists and passes:
  `python3 -m json.tool <route_b_build>/logs/route_b_target_artifact_audit.json >/dev/null`
- [ ] If OHOS/Route B is not in scope, report explicitly says host-only:
  `rg -n 'host-only|OHOS not verified|Route B not verified' reports/migration_report.md`
