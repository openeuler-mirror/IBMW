# ROS2 Action Fibonacci DDS-Only Proof

Private host-side proof sample for validating ROS2 Action interactions where the
IBMW server side stays on DDS-only transport boundaries and the ROS2 client
remains a separate validation peer. The original proof uses
`example_interfaces/action/Fibonacci`; the generic path also includes a second
real ROS2 Action type, `control_msgs/action/GripperCommand`, to prove the
`ActionServerNode` layer is not Fibonacci-only.

This sample is intentionally narrow:

- `Fibonacci` is kept where the proof targets the official ROS2 sample type
  `example_interfaces/action/Fibonacci`; generic IBMW layers are named
  `ActionServerNode`, `ServerRuntime`, `ActionTraits`, and `ActionServerBinding`
- `GripperCommand` is kept where the proof targets the installed ROS2 control
  type `control_msgs/action/GripperCommand`
- the server executable stays free of `rcl`, `rclcpp`, `rclcpp_action`, `rcl_action`, and `rmw`
- the validation client may link `rclcpp` and `rclcpp_action`
- the sample is experimental and does **not** claim generic ROS2 Action support;
  a separate experimental ibmw-gen `experimental_actions` lane can reproduce this
  Fibonacci-only proof for compile/link coverage, but it is not a public Action
  generator

## Scope

- DDS-only server proof for the Fibonacci action wire path
- DDS-only generic server proof for the installed `control_msgs/action/GripperCommand` wire path
- ROS2-native client used only as a validation peer
- parent/sample-level CMake gating for DDS extension + ROS2 build-time types,
  with the native ROS2 validation peer gated separately by `IBMW_BUILD_WITH_ROS2`
- documentation of the dependency boundary so the proof remains non-productized
- host-only proof coverage for native ROS2 client success and cancel interop

## Non-goals

- No generic Action API surface in public docs
- No rcl/rclcpp dependency on the server side
- No public `.action` generator claim
- No smoke runner wiring yet
- No change to red-line baselines or core dependency modules
- No board, aarch64 runtime, cross-RMW, or cross-distro claim

## Layout

| Path | Role |
|------|------|
| `CMakeLists.txt` | Sample-level integration and target gating |
| `../ros2_action_fibonacci_common/` | Private sample-local helper layer for Fibonacci endpoint names, QoS, sequence, UUID, capacity, and retention details |
| `ros2_server/fibonacci_action_server.cpp` | Hand-written DDS-only server reference entrypoint |
| `ros2_server/generic_fibonacci_action_server.cpp` | Generic `ActionServerNode<ActionTraits<Fibonacci>, FibonacciHook>` server entrypoint |
| `ros2_server/countdown_fibonacci_wire_action_server.cpp` | Generic `ActionServerNode<ActionTraits<Fibonacci>, CountdownHook>` second Hook/Binding demo entrypoint |
| `ros2_server/generic_gripper_command_action_server.cpp` | Generic `ActionServerNode<ActionTraits<GripperCommand>, GripperCommandHook>` second real Action-type server entrypoint |
| `ros2_client/fibonacci_action_client.cpp` | ROS2 Fibonacci validation client |
| `ros2_client/gripper_command_action_client.cpp` | ROS2 GripperCommand validation client |
| `validate_ros2_client_to_ibmw_server.sh` | Host validation harness that checks the server has no ROS2 runtime linkage |

## Targets

| Target | Dependency boundary |
|--------|---------------------|
| `ibmw_fibonacci_action_dds_server` | Hand-written DDS-only Fibonacci action server proof; no `rcl`, `rclcpp`, `rclcpp_action`, `rcl_action`, or `rmw` linkage |
| `ibmw_action_dds_generic_fibonacci_server` | Generic `ActionServerNode` Fibonacci server proof using `ActionTraits`, `FibonacciHook`, and `ActionServerBinding`; no `rcl`, `rclcpp`, `rclcpp_action`, `rcl_action`, or `rmw` linkage |
| `ibmw_action_dds_countdown_fibonacci_wire_server` | Generic `ActionServerNode` countdown Hook/Binding demo reusing the official Fibonacci wire type; no `rcl`, `rclcpp`, `rclcpp_action`, `rcl_action`, or `rmw` linkage |
| `ibmw_action_dds_generic_gripper_command_server` | Generic `ActionServerNode` proof for the real ROS2 `control_msgs/action/GripperCommand` type; no `rcl`, `rclcpp`, `rclcpp_action`, `rcl_action`, or `rmw` linkage |
| `ibmw_fibonacci_action_ros2_client` | Fibonacci validation peer that may link `rclcpp` and `rclcpp_action` |
| `ibmw_gripper_command_action_ros2_client` | GripperCommand validation peer that may link `rclcpp` and `rclcpp_action` |

## Focused DDS-only server build command

Use this build shape when only the DDS-only server proof target is intended. It
does not enable `IBMW_BUILD_WITH_ROS2`, so the sample-local CMake does not probe
`rclcpp` or `rclcpp_action` for the native validation client peer:

```bash
cmake --preset full \
  -DIBMW_BUILD_EXAMPLES=ON \
  -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_BUILD_ROS2_TYPES=ON \
  -DIBMW_BUILD_WITH_ROS2=OFF \
  -DIBMW_BUILD_ROS2_EXTENSION=OFF
cmake --build build --target ibmw_fibonacci_action_dds_server ibmw_action_dds_generic_fibonacci_server ibmw_action_dds_countdown_fibonacci_wire_server ibmw_action_dds_generic_gripper_command_server -j$(nproc)
```

To build the native ROS2 client validation peer as well, opt in to
`IBMW_BUILD_WITH_ROS2` and build both existing target names:

```bash
cmake --preset full \
  -DIBMW_BUILD_EXAMPLES=ON \
  -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_BUILD_ROS2_TYPES=ON \
  -DIBMW_BUILD_WITH_ROS2=ON \
  -DIBMW_BUILD_ROS2_EXTENSION=OFF
cmake --build build --target ibmw_fibonacci_action_dds_server ibmw_action_dds_generic_fibonacci_server ibmw_action_dds_countdown_fibonacci_wire_server ibmw_action_dds_generic_gripper_command_server ibmw_fibonacci_action_ros2_client ibmw_gripper_command_action_ros2_client -j$(nproc)
```

## Build prerequisites

The DDS-only server directory is gated in the parent sample CMake by:

- `IBMW_BUILD_DDS_EXTENSION`
- `IBMW_BUILD_ROS2_TYPES`
- `IBMW_BUILD_ROS2_ACTION_FIBONACCI_SAMPLES`

The sample-local CMake additionally checks for the required generated ROS2 action
type targets before defining the DDS-only server executable. The native ROS2
client validation peer is intentionally separate: it is defined only when
`IBMW_BUILD_WITH_ROS2=ON` and the `rclcpp`/`rclcpp_action` targets are present.

## Validation flow

The provided shell harness starts the DDS-only server, waits for its existing
startup marker, runs the ROS2 client, checks deterministic client-side markers,
and verifies the server binary does not pick up ROS2 runtime linkage.

Build both `ibmw_fibonacci_action_dds_server` and
`ibmw_fibonacci_action_ros2_client` with `IBMW_BUILD_WITH_ROS2=ON` before running
this host validation harness.

```bash
./validate_ros2_client_to_ibmw_server.sh
```

Environment variables supported by the harness:

- `VALIDATION_ACTION` — `fibonacci` or `gripper_command`, default `fibonacci`
- `BUILD_DIR` — build tree containing the executables
- `SERVER_BIN` — override for the server binary path
- `CLIENT_BIN` — override for the client binary path
- `ACTION_NAME` — action name, default `/fibonacci` or `/gripper_command` based on `VALIDATION_ACTION`
- `CLIENT_MODE` — `success` or `cancel`
- `GOAL_ORDER` — Fibonacci goal order, default `8`
- `GRIPPER_POSITION` — GripperCommand target position, default `0.42`
- `GRIPPER_EFFORT` — GripperCommand max effort, default `3.0`
- `CANCEL_DELAY_MS` — delay before native client sends cancel in cancel mode, default `250`
- `SERVER_READY_MARKER` — override server readiness marker; useful for validating the generic server target
- `SERVER_READY_TIMEOUT_SECS` — wait limit for the DDS-only server startup marker, default `10`
- `CLIENT_SERVER_WAIT_SECS` — ROS2 client wait limit for action server discovery, default `10`
- `CLIENT_RESULT_TIMEOUT_SECS` — ROS2 client wait limit for terminal result, default `20`

Validation modes:

- `VALIDATION_ACTION=fibonacci CLIENT_MODE=success` checks accepted goal,
  feedback, exact Fibonacci result sequence, and native client success
  completion.
- `VALIDATION_ACTION=gripper_command CLIENT_MODE=success` checks accepted goal,
  feedback, deterministic GripperCommand result fields, and native client
  success completion.
- `CLIENT_MODE=cancel` checks accepted goal, accepted cancel response, and native
  ROS2 client terminal cancel interop against the DDS-only server proof.

For convenience, the harness also accepts the validation mode as its first
positional argument:

```bash
./validate_ros2_client_to_ibmw_server.sh success
./validate_ros2_client_to_ibmw_server.sh cancel

SERVER_TARGET_NAME=ibmw_action_dds_generic_fibonacci_server \
SERVER_BIN=$BUILD_DIR/ibmw_action_dds_generic_fibonacci_server \
./validate_ros2_client_to_ibmw_server.sh success

SERVER_TARGET_NAME=ibmw_action_dds_generic_fibonacci_server \
SERVER_BIN=$BUILD_DIR/ibmw_action_dds_generic_fibonacci_server \
CANCEL_DELAY_MS=0 \
./validate_ros2_client_to_ibmw_server.sh cancel

VALIDATION_ACTION=gripper_command ./validate_ros2_client_to_ibmw_server.sh success

VALIDATION_ACTION=gripper_command CANCEL_DELAY_MS=0 \
./validate_ros2_client_to_ibmw_server.sh cancel
```

## Notes

- The Fibonacci client source lives at `ros2_client/fibonacci_action_client.cpp`.
- The GripperCommand client source lives at `ros2_client/gripper_command_action_client.cpp`.
- The hand-written Fibonacci server source lives at `ros2_server/fibonacci_action_server.cpp`.
- The generic GripperCommand server source lives at `ros2_server/generic_gripper_command_action_server.cpp`.
- Public scope and verification requirements are documented in the
  [DDS-only Action adapter guide](../../../../docs/experimental/ros2-action-dds-only-adapter-guide.md)
  and [support matrix](../../../../docs/reference/support-matrix.md).
- The server boundary is the important part of the proof: the harness rejects any
  server binary that links `librcl`, `librclcpp`, `librclcpp_action`, `librcl_action`,
  or `librmw`.
- The harness is host-only and uses the DDS-only server's existing startup log as
  its readiness marker; it does not add board validation or broaden runtime/API
  support claims.
- Default success order is 8. The proof fixtures accept orders 1..47 to keep
  `int32_t` Fibonacci values bounded and avoid sample-level resource abuse.
- ROS2 `GetResult` wire behavior is preserved for the normal proof path:
  unknown goals respond with `STATUS_UNKNOWN`, known non-terminal goals defer,
  and terminal goals respond with the terminal status plus result. If the sample
  proof server's per-goal pending-result waiter cap is reached, it returns an
  immediate `STATUS_UNKNOWN` empty result for the rejected newest waiter so that
  resource pressure does not create a requester hang. That queue-full behavior is
  a sample robustness guard, not a public Action API contract.
- Shared proof helpers live in `../ros2_action_fibonacci_common/`. They are
  sample infrastructure only and do not define public Action API or
  generic ibmw-gen Action support. The Fibonacci-only
  `experimental_actions` lane copies this proof for compile/link coverage only.
- This sample documents an experimental host-only path. It does **not** claim a
  generic IBMW ROS2 Action runtime, public API, public generator support, board
  validation, or cross-RMW/cross-distro interoperability.
