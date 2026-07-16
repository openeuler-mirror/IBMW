# IBMW ROS2 Application Migration

Use this playbook to move ROS2 topic/service applications into one or both of
these ecosystem roles:

- **DDS/ROS2 application:** preserves DDS wire interoperability with ROS2 peers
  while keeping IBMW runtime business code free of rcl/rclcpp.
- **JSON/local application:** preserves business behavior with IBMW-owned JSON
  types over local pub/sub or RPC. This is a local application workflow, not
  evidence for DDS, ROS2, network, or target deployment.

For OHOS delivery, host smoke is insufficient. Run the cross-build and target
artifact audit described in `tasks/08-route-b-ohos-cross.md`.

## Migration contract

1. Inventory topics, services, actions, parameters, timers, QoS, and business
   callbacks.
2. Extract business operations into ordinary C++ adapters with stable inputs,
   outputs, validation, units, errors, and logging.
3. Generate IBMW modules that call those adapters; do not duplicate business
   logic in generated files.
4. Keep native ROS2 code as an interoperability peer or test harness, not in the
   DDS/ROS2 application runtime.
5. Validate the largest representative serialized DDS sample against the
   application type capacity and receive it with the intended peer.
6. Inventory ROS2 Actions, but do not claim automatic Action semantic migration.
   Redesign explicitly as topics/services or use the scoped experimental adapter.

## Workflow

- Start with `00-decision-tree.md` and `tasks/01-inventory.md`.
- DDS/ROS2 role: `tasks/02-dds-ros2-pubsub.md` and
  `tasks/03-dds-ros2-service.md`.
- JSON/local role: `tasks/04-json-local-pubsub.md` and
  `tasks/05-json-local-service.md`.
- Finish with build, runtime, and target tasks 06-08.
- Use templates and checklists with matching ecosystem-role names.

## Suggested output tree

```text
migration_practice/
  native/{pubsub,service,action_optional}/
  adapters/
  dds_ros2_application/{pubsub,service}/
  json_local_application/{pubsub,service}/
  route_b_ohos/
  reports/runtime_logs/
```

Claims must follow `reference/backend-status.md` and the public
[support matrix](../../docs/reference/support-matrix.md).
