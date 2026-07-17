Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Migration Report

## Scope

- Source path:
- DDS/ROS2 application lanes:
- JSON/local application lanes:
- Deferred actions / unsupported targets:

## Minimal-change evidence

- Adapter files:
- Generated user-region files calling adapters:
- Business semantics preserved:

## DDS/ROS2 application evidence

- YAML paths:
- rcl-free audit command/result:
- PASS markers:

## JSON/local application evidence

- YAML paths:
- Runtime command/result:
- PASS markers:
- Boundary: JSON evidence covers local pub/sub or RPC only; other backends and targets require separate validation.

## OHOS / Route B evidence

- In scope: yes/no
- Execution location: `outside_route_b_container` / `inside_route_b_container`
- Container image/name and mounts, if inside container:
- FastDDS/Fast-CDR provider: ROS2 target prefix / reviewed exception
- Route B build dir:
- Command log: `reports/route_b_ohos.md`
- Target audit JSON: `<route_b_build>/logs/route_b_target_artifact_audit.json`
- Result: `OHOS Route B: PASS` / `OHOS Route B: NOT VERIFIED`
- Boundary: Route B compile/audit is not board runtime validation.

## Completeness

- Checklist paths:
- Known gaps:
