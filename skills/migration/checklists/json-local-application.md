Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# JSON/Local Application Checklist

- [ ] JSON/local YAML exists: `ls json_local_application/**/*.ibmw.yaml`
- [ ] JSON pub/sub PASS if in scope:
  `rg -n 'JSON_LOCAL_PUBSUB_PASS' reports/runtime_logs/*.log`
- [ ] JSON RPC PASS if in scope:
  `rg -n 'JSON_LOCAL_SERVICE_PASS' reports/runtime_logs/*.log`
- [ ] PASS markers visible:
  `rg -n 'JSON_LOCAL_(PUBSUB|SERVICE)_PASS' reports/runtime_logs/*.log`
- [ ] Report states boundary: JSON local/RPC practice passed but
  backend global status remains degraded. Evidence: `rg -n 'degraded' reports/migration_report.md`
- [ ] No DDS, ROS2, network, shared-memory, or target-runtime claim is inferred from local JSON smoke. Evidence:
  `rg -n 'dds_native|iceoryx' reports/migration_report.md`
