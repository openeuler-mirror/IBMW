Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Runtime Report

| lane | command | log path | PASS grep token | result |
|---|---|---|---|---|
| DDS/ROS2 pub/sub |  | reports/runtime_logs/dds_ros2_pubsub.log | DDS_ROS2_PUBSUB_PASS |  |
| DDS/ROS2 service |  | reports/runtime_logs/dds_ros2_service_client.log | DDS_ROS2_SERVICE_PASS |  |
| JSON/local pub/sub |  | reports/runtime_logs/json_local_pubsub.log | JSON_LOCAL_PUBSUB_PASS |  |
| JSON/local service |  | reports/runtime_logs/json_local_service.log | JSON_LOCAL_SERVICE_PASS |  |

PASS grep:

```bash
rg -n 'DDS_ROS2_(PUBSUB|SERVICE)_PASS|JSON_LOCAL_(PUBSUB|SERVICE)_PASS' reports/runtime_logs/*.log
```
