Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Task 01 — Inventory

Create `reports/inventory.md` with one row per ROS entity:

| kind | name | type | file | callback/business function | QoS/depth | evidence |
|---|---|---|---|---|---|---|

Capture publishers, subscribers, services, clients, timers, parameters,
shutdown conditions, and action clients/servers.

Required commands:

```bash
rg -n 'create_(publisher|subscription|service|client|wall_timer)|declare_parameter|Action|action' native
rg -n 'RCLCPP_|get_logger|spin\(|shutdown\(' native
```

Minimal-change extraction:

- Move callback/business logic to `adapters/` as pure functions or adapter
  methods.
- Keep ROS message conversion at the edge.
- Record adapter path in `reports/inventory.md`.
