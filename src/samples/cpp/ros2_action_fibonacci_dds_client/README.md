# ROS2 Action Fibonacci DDS-Only Reverse-Path Proof

Experimental host-only sample for the reverse validation direction where an IBMW
DDS-only client talks to a native ROS2 Fibonacci Action server.

This sample validates one narrow path:

- ROS2 Humble
- `rmw_fastrtps_cpp`
- FastDDS
- `example_interfaces/action/Fibonacci`
- IBMW DDS-only client to native ROS2 Action server

It is not a public ROS2 Action runtime, API, or generic generator feature. A
separate experimental ibmw-gen `experimental_actions` lane can reproduce this
Fibonacci-only proof, but it is not a public Action generator.

## Scope

- sample-scoped host-side reverse-path interoperability check
- native ROS2 Action server fixture with deterministic success and cancel
  markers
- DDS-only IBMW Action client fixture for `example_interfaces/action/Fibonacci`
- validation harness that runs the server, runs the client, checks deterministic
  logs, and enforces a client linkage guard
- focused evidence for success and cancel paths only

## Non-goals

- No public ROS2 Action runtime support claim
- No public ROS2 Action API support claim
- No public or generic ibmw-gen Action generator support claim
- No generic Action type support claim beyond this Fibonacci proof
- No board or aarch64 runtime claim
- No cross-RMW or cross-distro claim
- No productized runtime support claim

## Layout

| Path | Role |
|------|------|
| `CMakeLists.txt` | Sample-local CMake gating for the server and DDS-only client targets |
| `../ros2_action_fibonacci_common/` | Sample-local helper layer for Fibonacci endpoint names, QoS, sequence, UUID, capacity, and retention details |
| `ros2_server/fibonacci_action_server.cpp` | Native ROS2 Fibonacci Action server fixture |
| `dds_client/fibonacci_action_client.cpp` | IBMW DDS-only Fibonacci Action client fixture |
| `validate_ibmw_client_to_ros2_server.sh` | Success and cancel validation harness with DDS-only linkage guard |

## Targets

| Target | Role |
|--------|------|
| `ibmw_fibonacci_action_ros2_server` | Native ROS2 Fibonacci Action server fixture |
| `ibmw_fibonacci_action_dds_client` | IBMW DDS-only Fibonacci Action client fixture |

## Focused DDS-only client build command

Use this build shape when only the DDS-only client proof target is intended. It
does not enable `IBMW_BUILD_WITH_ROS2`, so the sample-local CMake does not probe
`rclcpp` or `rclcpp_action` for the native ROS2 server fixture:

```bash
cmake --preset full \
  -DIBMW_BUILD_EXAMPLES=ON \
  -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_BUILD_ROS2_TYPES=ON \
  -DIBMW_BUILD_WITH_ROS2=OFF \
  -DIBMW_BUILD_ROS2_EXTENSION=OFF
cmake --build build --target ibmw_fibonacci_action_dds_client -j$(nproc)
```

To build the native ROS2 server validation fixture as well, opt in to
`IBMW_BUILD_WITH_ROS2` and build both existing target names:

```bash
cmake --preset full \
  -DIBMW_BUILD_EXAMPLES=ON \
  -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_BUILD_ROS2_TYPES=ON \
  -DIBMW_BUILD_WITH_ROS2=ON \
  -DIBMW_BUILD_ROS2_EXTENSION=OFF
cmake --build build --target ibmw_fibonacci_action_ros2_server ibmw_fibonacci_action_dds_client -j$(nproc)
```

## Build prerequisites

The reverse DDS-only client directory is included by the parent sample CMake when
all of the following are enabled and the needed ROS2 generated type targets are
present:

- `IBMW_BUILD_DDS_EXTENSION`
- `IBMW_BUILD_ROS2_TYPES`
- `IBMW_BUILD_ROS2_ACTION_FIBONACCI_SAMPLES`

The sample-local CMake additionally checks for the required generated ROS2 action
type targets before defining the DDS-only client executable. The native ROS2
server validation fixture is intentionally separate: it is defined only when
`IBMW_BUILD_WITH_ROS2=ON` and the `rclcpp`/`rclcpp_action` targets are present.
The DDS-only client target links IBMW DDS/type support and generated ROS2 type
support, but the harness verifies that it does not link ROS2 runtime libraries;
do not add rcl/rclcpp linkage to `ibmw_fibonacci_action_dds_client`.

## Validation harness

The harness:

- sources `/opt/ros/humble/setup.bash` if needed
- forces `RMW_IMPLEMENTATION=rmw_fastrtps_cpp`
- prepends `/usr/lib/x86_64-linux-gnu` to `LD_LIBRARY_PATH` so ROS2 runtime
  libraries avoid the active conda `libstdc++` mismatch
- checks the DDS-only client with `ldd`
- fails if the client links `librcl`, `librclcpp`, `librclcpp_action`,
  `librcl_action`, or `librmw`
- starts the native ROS2 server fixture
- waits for `SERVER_READY`
- runs the DDS-only IBMW client in success or cancel mode
- checks deterministic client and server markers before reporting `[PASS]`

Success validation:

```bash
./validate_ibmw_client_to_ros2_server.sh
```

Cancel validation:

```bash
CLIENT_MODE=cancel ./validate_ibmw_client_to_ros2_server.sh
# Equivalent positional shortcut:
./validate_ibmw_client_to_ros2_server.sh cancel
```

## Verification expectations

For a current checkout, build the focused targets, run success and cancel modes,
and retain both logs. Confirm the DDS-only client linkage guard rejects `librcl`,
`librclcpp`, `librclcpp_action`, `librcl_action`, and `librmw`. Record results for
the exact ROS2/FastDDS environment used.

## Notes

- Default action name is `/fibonacci`.
- Default success order is 8. The proof fixtures accept orders 1..47 to keep
  `int32_t` Fibonacci values bounded and avoid sample-level resource abuse.
- ROS2 `GetResult` wire behavior is preserved for the normal proof path:
  unknown goals respond with `STATUS_UNKNOWN`, known non-terminal goals defer,
  and terminal goals respond with the terminal status plus result. If the sample
  proof server's per-goal pending-result waiter cap is reached, it returns an
  immediate `STATUS_UNKNOWN` empty result for the rejected newest waiter so that
  resource pressure does not create a requester hang. That queue-full behavior is
  a sample robustness guard, not a public Action API contract.
- Shared proof helpers live in `../ros2_action_fibonacci_common/`. They remain
  sample infrastructure and are not public Action API or generic
  ibmw-gen Action support. The Fibonacci-only `experimental_actions`
  lane copies this proof for compile/link coverage only.
- This is an experimental host-only sample for ROS2 Humble with `rmw_fastrtps_cpp` and
  FastDDS. Keep broader ROS2 Action support claims out of this sample until the
  runtime, API, generator, and product scope are designed and validated.
