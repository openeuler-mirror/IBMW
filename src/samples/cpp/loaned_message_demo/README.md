# LoanedMessage Demo

End-to-end verification demo for the IBMW LoanedMessage API with zero-copy message publishing.

## Features

- LoanedMessage publishing of `geometry_msgs::msg::Point` on the ROS2-compatible
  DDS lane. ROS2 message types are registered as non-plain, so AUTO uses the
  heap fallback (the sample is copied by `DataWriter::write`). For a true
  zero-copy `loan_sample` lane with a native plain DDS type, see `dds_loaned/`
  in the [transport benchmark](../transport_benchmark/README.md).
- Automatic fallback verification for non-Plain types (`std_msgs::msg::String`)
- Three configuration mode tests (AUTO / STRICT / COMPAT)
- Zero-copy statistics output
- ROS2 interoperability support

## Build

```bash
cd <IBMW_ROOT>
source /opt/ros/humble/setup.bash

cmake -B build \
  -DIBMW_BUILD_WITH_ROS2=ON \
  -DIBMW_BUILD_DDS_EXTENSION=ON \
  -DIBMW_BUILD_EXAMPLES=ON

# Single-threaded build to avoid log loss
cmake --build build --target ibmw_dds_extension -- -j1
cmake --build build --target loaned_message_app -- -j1
cmake --build build --target config_test_app -- -j1
cmake --build build --target plain_pub_only_app -- -j1
cmake --build build --target nonplain_pub_only_app -- -j1
cmake --build build --target sub_only_app -- -j1
```

## Run

```bash
cd build
source /opt/ros/humble/setup.bash

# Full end-to-end test (Ctrl+C to exit)
./loaned_message_app ../src/samples/cpp/loaned_message_demo/config/cfg/loaned_message_cfg.yaml

# Configuration mode test (completes automatically)
./config_test_app ../src/samples/cpp/loaned_message_demo/config/cfg/config_test_cfg.yaml

# Individual tests
./plain_pub_only_app ../src/samples/cpp/loaned_message_demo/config/cfg/plain_pub_only_cfg.yaml
./nonplain_pub_only_app ../src/samples/cpp/loaned_message_demo/config/cfg/nonplain_pub_only_cfg.yaml
./sub_only_app ../src/samples/cpp/loaned_message_demo/config/cfg/sub_only_cfg.yaml
```

## Applications

| App | Description |
|-----|-------------|
| loaned_message_app | Full test: Plain + NonPlain publish + subscribe |
| config_test_app | 6 configuration mode test scenarios |
| plain_pub_only_app | Point publish (ROS2 type, AUTO heap fallback) |
| nonplain_pub_only_app | Non-Plain type fallback verification |
| sub_only_app | Subscriber |

## Configuration Modes

| Mode | Plain Type | Non-Plain Type |
|------|-----------|--------------|
| AUTO | Zero-copy (if SHM enabled) | Automatic fallback to heap memory |
| STRICT | Zero-copy (returns invalid on failure) | Returns invalid object |
| COMPAT | Forces heap memory | Heap memory |

## Configuration Files

| File | Description |
|------|-------------|
| loaned_message_cfg.yaml | Full test (AUTO mode) |
| strict_mode_cfg.yaml | STRICT mode |
| compat_mode_cfg.yaml | COMPAT mode |
| config_test_cfg.yaml | ConfigTestModule dedicated |
| plain_pub_only_cfg.yaml | Plain publish |
| nonplain_pub_only_cfg.yaml | Non-Plain publish |
| sub_only_cfg.yaml | Subscribe |

## Expected Output

Point (ROS2 type, AUTO heap fallback):
```
[SEND] Point[0] x=1.00, y=2.00, z=3.00, is_zero_copy=false, is_plain=false, ptr=0x7f...
[RECV] Point[1] x=1.10, y=2.20, z=3.30, valid=YES, ptr=0x7f...
```

Non-Plain type (fallback):
```
[SEND] String[0] borrow ok: is_zero_copy=false, is_plain=false, is_valid=true
[RECV] String[1] data="Hello LoanedMessage #0", valid=YES
```

Configuration test:
```
Config Test Summary: 6/6 passed
All tests passed!
```

## ROS2 Interoperability

```bash
# Terminal 1: IBMW publish
./plain_pub_only_app ../src/samples/cpp/loaned_message_demo/config/cfg/plain_pub_only_cfg.yaml

# Terminal 2: ROS2 subscribe
ros2 topic echo /rt/point_topic geometry_msgs/msg/Point
```

## Note on ibmw-gen

This demo uses **DDS extension internal APIs** (`DdsTransportDriver`,
`Ros2StylePublisher`, `BorrowLoanedMessage`, `ZeroCopyConfig`) for zero-copy
message publishing. These are specialized low-level APIs not covered by the
`ibmw-gen` code generator. To learn about ibmw-gen for standard pub/sub and RPC
projects, see the [ibmw-gen README](../../../tools/ibmw_gen/README.md).

## Troubleshooting

See the [public troubleshooting guide](../../../../docs/guides/troubleshooting.md).

## Architecture boundary

This sample intentionally exercises low-level DDS extension APIs. Its direct
driver includes are marked with the repository's R1 exemption anchors. Treat it
as an explicit transport-specific escape hatch; managed applications should use
the public loan facade and transport-neutral generated modules instead.
