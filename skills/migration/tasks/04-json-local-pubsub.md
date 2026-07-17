Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Task 04 — JSON/Local Application Pub/Sub

Goal: create JSON-backed local pub/sub practice target.

Steps:

1. Copy `templates/json_local_pubsub.ibmw.yaml` to
   `json_local_application/pubsub/<app>_json_local_pubsub.ibmw.yaml`.
2. Fill JSON message fields from inventory.
3. Generate modules.
4. User-region code calls the same adapter used by the DDS/ROS2 role where possible.
5. Verify local JSON smoke only; do not claim global backend health.

Evidence:

- YAML: `json_local_application/pubsub/*_json_local_pubsub.ibmw.yaml`
- Runtime pass marker: `JSON_LOCAL_PUBSUB_PASS` inside
  `reports/runtime_logs/json_local_pubsub.log` or equivalent.
- Boundary note in final report: JSON/local application smoke passed; backend
  global status remains degraded.
