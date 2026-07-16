# Experimental Fibonacci Action Generator Sample

This sample exercises the experimental `ibmw-gen` lane for the DDS-only
ROS2-compatible Fibonacci Action proof. It generates the existing hand-written
Fibonacci Action DDS server/client proof into a self-contained generated project
under `ros2_action_fibonacci_gen/`.

Scope boundaries:

- only `example_interfaces/action/Fibonacci` is accepted
- only `transport: dds` is accepted
- roles are limited to `server` and `client`
- no public `action_server` or `action_client` module type is added
- generated binaries must not link `rcl`, `rclcpp`, `rclcpp_action`,
  `rcl_action`, or `rmw`

Generate it with:

```bash
python3 <ibmw>/src/tools/ibmw_gen/ibmw_gen.py \
  ros2_action_fibonacci_gen.ibmw.yaml -o ros2_action_fibonacci_gen
```

The generated project is a compile/link proof only. Runtime interop validation
continues to live in the hand-written sibling samples:
`ros2_action_fibonacci_dds_server/` and `ros2_action_fibonacci_dds_client/`.
