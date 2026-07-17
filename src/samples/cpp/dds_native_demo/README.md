# DDS Native Demo

Demonstrates direct DDS publish/subscribe using IBMW's DDS transport interface
with a custom `HelloMsg` type. Both publisher and subscriber run in a single
app-mode process.

## Run

```bash
cd build
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$(pwd)/lib:$(pwd):$(pwd)/bin:${LD_LIBRARY_PATH}"

bash start_dds_native_demo.sh
# or: ./dds_native_app ./cfg/dds_native_cfg.yaml
```

Press Ctrl+C after a few messages to shut down.

## Expected Output

```
DDS Native Demo initialized.
Published HelloMsg: id=0, data="Hello DDS"
Received HelloMsg: id=0, data="Hello DDS"
Published HelloMsg: id=1, data="Hello DDS"
Received HelloMsg: id=1, data="Hello DDS"
...
```

## Configuration

| Config | Description |
|--------|-------------|
| `dds_native_cfg.yaml` | DDS extension with domain_id=0 |

## Concepts Demonstrated

- Custom DDS type (`HelloMsg`) with `IBMW_DDS_TYPE_NAME()` macro
- `ibmw::transport::RegisterPublishType<T>()` -- DDS type registration
- `GenericDdsPubSubType<T>` -- automatic FastCDR-based TopicDataType adapter
- Single-process DDS loopback (publisher + subscriber in same DDS participant)

---

## ibmw-gen Way

This demo can also be generated using `ibmw-gen` from the
[`dds_native_demo.ibmw.yaml`](./dds_native_demo.ibmw.yaml) definition.

### What ibmw-gen generates vs what you write

| Generated (auto) | You write |
|-------------------|-----------|
| `CMakeLists.txt` -- build system with FastCDR linking | `publisher_module.cc` -- publish logic |
| `protocol/hello_msg.h` -- C++ struct + inline CDR serialization | `subscriber_module.cc` -- receive callback |
| `cfg/dds_native_app.yaml` -- App-mode config with DDS driver | Module `.h` headers |
| `cfg/dds_native_pkg.yaml` -- Pkg-mode config | |
| `src/dds_native_app/main.cc`, `src/*_pkg/pkg_main.cc` | |

### Generate, build, and run

```bash
# Generate
python3 <ibmw-gen>/ibmw_gen.py dds_native_demo.ibmw.yaml -o dds_native_demo_gen

# Build
mkdir -p dds_native_demo_gen/build && cd dds_native_demo_gen/build
cmake .. -DCMAKE_PREFIX_PATH=<ibmw_build_dir>
make -j$(nproc)

# Run (App mode)
./dds_native_app cfg/dds_native_app.yaml

# Run (Pkg mode)
ibmw run cfg/dds_native_pkg.yaml
```

### Key YAML features

```yaml
custom_dds_types:
  namespace: "ibmw::examples::dds_native_demo"
  messages:
    - name: HelloMsg
      constructor_args: true            # Generate constructor with field args
      fields:
        - { name: id, type: uint64, default: "0" }
        - { name: message, type: string }
        - { name: timestamp, type: int64, default: "0" }
```

ibmw-gen generates the complete `HelloMsg` C++ struct with `IBMW_DDS_TYPE_NAME`
macro, FastCDR `serialize`/`deserialize` methods, and `GenericDdsPubSubType`
adapter -- eliminating the need to write IDL or manual CDR code. The module
scaffolds provide typed `SenderAccess<HelloMsg>` / `ReceiverAccess<HelloMsg>`
wrappers.
