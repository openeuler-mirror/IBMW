Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Decision Tree

1. Is the source a ROS 2 user service/app with topics or services?
   - Yes: run `tasks/01-inventory.md`.
   - No: this manual is not the right tool.
2. Need ROS2 wire compatibility with native peers?
   - Yes: DDS/ROS2 application route; run tasks 02 and/or 03.
   - No: continue.
3. Need local JSON pub/sub or JSON RPC practice?
    - Yes: JSON/local application route; run tasks 04 and/or 05.
    - No: record unsupported target in `reports/migration_report.md`.
4. Is the target OHOS or Route B delivery?
   - Yes: host smoke is insufficient; run `tasks/08-route-b-ohos-cross.md`.
   - No: record host-only status and do not claim OHOS readiness.
5. Does the source use ROS 2 actions?
   - Inventory only. Do not claim action semantics migrated automatically.
   - Convert explicitly to topics/services or defer.
6. Does the request treat local JSON smoke as proof of another backend?
   - Stop. Validate the requested backend and deployment separately.

Exit with PASS only when the relevant ecosystem-role checklist and completeness
checklist have concrete evidence paths/commands.
