# Pub/Sub Demo

Demonstrates IBMW transport (publish/subscribe) with local and DDS backends.
Uses IDL-generated `ExampleEventMsg` type with FastCDR serialization.

## Run

```bash
cd build
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$(pwd)/lib:$(pwd):$(pwd)/bin:${LD_LIBRARY_PATH}"

# Single process, local backend (pkg mode)
bash start_pubsub_local.sh

# Single process, DDS backend (app mode)
bash start_pubsub_dds.sh

# Two processes, DDS backend (pkg mode)
# Terminal 1:
bash start_publisher_dds.sh
# Terminal 2:
bash start_subscriber_dds.sh
```

## Expected Output

Publisher:
```
Published: msg='HelloWorld', num=0
Published: msg='HelloWorld', num=1
Published: msg='HelloWorld', num=2
...
```

Subscriber:
```
Received: msg='HelloWorld', num=0
Received: msg='HelloWorld', num=1
Received: msg='HelloWorld', num=2
...
```

## Configurations

| Config | Mode | Backend | Description |
|--------|------|---------|-------------|
| `pubsub_single_process_cfg.yaml` | pkg | local | Both modules in one process |
| `pubsub_dds_cfg.yaml` | app | DDS | Both modules via DDS in one process |
| `publisher_dds_cfg.yaml` | pkg | DDS | Publisher only (separate process) |
| `subscriber_dds_cfg.yaml` | pkg | DDS | Subscriber only (separate process) |
| `publisher_local_cfg.yaml` | pkg | local | Publisher only (local backend) |
| `subscriber_local_cfg.yaml` | pkg | local | Subscriber only (local backend) |

## Concepts Demonstrated

- `ibmw::transport::RegisterPublishType<T>(publisher)` -- register DDS type for publishing
- `ibmw::transport::Subscribe<T>(subscriber, callback)` -- subscribe with typed callback
- `ibmw::transport::Send(publisher, msg)` -- publish a message
- Local vs DDS backend routing via YAML `enable_backends`
- Cross-process DDS communication (~74us latency)

## IDL Type

The `ExampleEventMsg` type is defined in `src/schema/dds_idl/example/ExampleEvent.idl`:

```idl
module pubsub_demo {
  struct ExampleEventMsg {
    string msg;
    long num;
  };
};
```

---

## ibmw-gen Way

This demo can also be generated using `ibmw-gen` from the
[`transport_demo.ibmw.yaml`](./transport_demo.ibmw.yaml) definition.

### What ibmw-gen generates vs what you write

| Generated (auto) | You write |
|-------------------|-----------|
| `CMakeLists.txt` -- build system with DDS linking | `publisher_module.cc` -- publish logic (timer, message construction) |
| `cfg/pubsub_app.yaml` -- App-mode config with DDS driver | `subscriber_module.cc` -- receive callback logic |
| `cfg/*_pkg.yaml` -- Pkg-mode configs | `publisher_module.h`, `subscriber_module.h` -- headers |
| `src/pubsub_app/main.cc` -- App entry point | |
| `src/*_pkg/pkg_main.cc` -- Pkg loaders | |
| DDS type bridge headers (referencing external IDL target) | |

### Generate, build, and run

```bash
# Generate
python3 <ibmw-gen>/ibmw_gen.py transport_demo.ibmw.yaml -o transport_demo_gen

# Build
mkdir -p transport_demo_gen/build && cd transport_demo_gen/build
cmake .. -DCMAKE_PREFIX_PATH=<ibmw_build_dir>
make -j$(nproc)

# Run (App mode)
./pubsub_app cfg/pubsub_app.yaml

# Run (Pkg mode)
ibmw run cfg/pubsub_pkg.yaml
```

### Key YAML features

```yaml
dds_types:
  namespace: ibmw::protocols::example
  groups:
    - name: ExampleEvent
      idl_gencode_target: ibmw::schema::example_idl_gencode  # External IDL target
      messages:
        - name: ExampleEventMsg
          fields:
            - { name: msg, type: string }
            - { name: num, type: int32 }

modules:
  - name: PublisherModule
    type: publisher
    transport: dds              # DDS transport backend
    topic: hello_topic
    message_type: ExampleEventMsg
```

ibmw-gen generates `SenderAccess<ExampleEventMsg>` for the publisher and
`ReceiverAccess<ExampleEventMsg>` for the subscriber, with typed publish/subscribe
wrappers. The DDS type bridges reference the externally-built IDL gencode target.
