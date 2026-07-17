# DDS Image Demo

Demonstrates high-throughput DDS messaging with large payloads: 640x480 RGB8
images at 10Hz (~1MB/frame). Tests DDS serialization performance and FastCDR
handling of large binary data.

## Run

```bash
cd build
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:$(pwd)/lib:$(pwd):$(pwd)/bin:${LD_LIBRARY_PATH}"

bash start_dds_image_demo.sh
# or: ./dds_image_app ./cfg/dds_image_cfg.yaml
```

Press Ctrl+C after a few frames to shut down.

## Expected Output

```
DDS Image Demo initialized.
Published image: 640x480, size=921600 bytes, frame=0
Received image: 640x480, size=921600 bytes, frame=0, latency=0.8ms
Published image: 640x480, size=921600 bytes, frame=1
Received image: 640x480, size=921600 bytes, frame=1, latency=0.9ms
...
```

## Configuration

| Config | Description |
|--------|-------------|
| `dds_image_cfg.yaml` | DDS extension, image pub/sub at 10Hz |

## Concepts Demonstrated

- Large message serialization with FastCDR
- High-throughput DDS communication (~10MB/s sustained)
- Sub-millisecond latency for ~1MB payloads
- `ImageMsg` type with binary pixel data

---

## ibmw-gen Way

This demo can also be generated using `ibmw-gen` from the
[`dds_image_demo.ibmw.yaml`](./dds_image_demo.ibmw.yaml) definition.

### What ibmw-gen generates vs what you write

| Generated (auto) | You write |
|-------------------|-----------|
| `CMakeLists.txt` -- build system with FastCDR linking | `image_publisher.cc` -- image generation + publish |
| `protocol/image_msg.h` -- ImageMsg struct + ImageEncoding enum + inline CDR | `image_subscriber.cc` -- receive + latency calc |
| `cfg/dds_image_app.yaml` -- App-mode config | Module `.h` headers |
| `cfg/dds_image_app.yaml` -- App-mode config | |
| `cfg/dds_image_pkg.yaml` -- Pkg-mode config | |
| `src/dds_image_app/main.cc`, `src/*_pkg/pkg_main.cc` | |

### Key YAML features

```yaml
custom_dds_types:
  namespace: "ibmw::examples::dds_image_demo"
  constants:
    - { name: kImageBufferSize, type: uint64, value: 1048576 }
  enums:
    - name: ImageEncoding
      underlying_type: uint32
      values:
        - { name: RGB8, value: 0 }
        - { name: GRAY8, value: 2 }
  messages:
    - name: ImageMsg
      fields:
        - { name: encoding, type: ImageEncoding, default: "ImageEncoding::RGB8" }
        - { name: data, type: "array<uint8, kImageBufferSize>" }  # 1MB pixel buffer
```

This demonstrates ibmw-gen's `custom_dds_types` with constants, enums, and
fixed-size array fields. The generated CDR serialization handles the 1MB
`array<uint8, kImageBufferSize>` field efficiently via FastCDR bulk copy.
