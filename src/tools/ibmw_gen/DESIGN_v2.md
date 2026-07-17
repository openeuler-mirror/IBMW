# ibmw-gen v2 Comprehensive Design

> Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

## 1. Executive Summary

ibmw-gen v1 supports 5 module types (`bare`, `publisher`, `subscriber`,
`rpc_server`, `rpc_client`) across 3 transports (`dds`, `json`, `ros2`).
v2 extends the tool to cover **all** IBMW demo scenarios audited in Phase 5,
with particular emphasis on:

- **DDS zero-copy / loaned messages** (user-critical requirement)
- **ROS2 interop mode** (three-argument RegisterSendType API)
- **Context API modules** (alternative module programming model)
- **Logging extensions** (rotate_file, console patterns, executor)
- **Extensions section** (DDS, echo, net, grpc, mqtt, etc.)
- **DDS QoS configuration** (per-topic publisher/subscriber QoS)
- **Executor extensions** (strand, dedicated, timer, parallel, RT scheduling)
- **Timer and coroutine module types**
- **Module custom config** (YAML root-level per-module sections)
- **Auto-generated CDR types** (custom DDS types from YAML field definitions,
  with automatic plain-type detection for zero-copy support)

### Design Principles

1. **Additive, not breaking** -- existing `.ibmw.yaml` files remain valid for
   generator input compatibility; generated package code targets the current
   single-entry typed-slot package ABI.
2. **Explicit over magic** -- new features require opt-in YAML keys.
3. **Config fidelity** -- generated YAML configs match real demo configs exactly.
4. **CMake correctness** -- whole-archive linking, feature guards, find_package.
5. **Module code quality** -- generated code compiles and runs without manual edits
   for standard scenarios; complex scenarios get well-marked TODO stubs.

---

## 2. New YAML Schema Additions

### 2.1 `project` Section (unchanged)

```yaml
project:
  name: my_project
  namespace: my_project
```

No changes.

### 2.2 `logging` Section (NEW)

Controls logging backends in generated config YAML. Defaults to console-only
(v1 behavior) when omitted.

```yaml
logging:
  core_level: INFO                    # default: INFO
  backends:
    - type: console
      options:                        # optional
        pattern: "[%c.%f][%A][%l][%t][%n][%G] %v"
        log_executor_name: log_executor   # requires matching executor

    - type: rotate_file
      options:
        path: ./log
        filename: my_app.log
        max_file_size_m: 4
        max_file_num: 10
        enable_sync: true             # optional, default: false
        sync_interval_ms: 5000        # required when enable_sync: true
        sync_executor_name: sync_timer_executor  # required when enable_sync: true
```

**Schema rules:**
- `backends` is a list; each item has `type` (required) and `options` (optional).
- Valid types: `console`, `rotate_file`.
- When `log_executor_name` or `sync_executor_name` is set, the referenced
  executor must exist in `executors` section (validated at schema level).

### 2.3 `extensions` Section (NEW)

Declares IBMW extension plugins to load. Directly maps to
`ibmw.extensions.items[]` in config YAML.

```yaml
extensions:
  - name: dds_extension
    path: ./libibmw_dds_transport.so
    options:
      domain_id: 0

  - name: echo_extension
    path: ./libibmw_echo_driver.so

  - name: grpc_extension
    path: ./libibmw_grpc_transport.so
    options:
      thread_num: 2
      listen_ip: "127.0.0.1"
      listen_port: 50051

  - name: net_extension
    path: ./libibmw_net_transport.so
    options:
      thread_num: 2
      http_options:
        listen_ip: "127.0.0.1"
        listen_port: 50080

  - name: mqtt_extension
    path: ./libibmw_mqtt_transport.so
    options:
      broker_addr: "tcp://127.0.0.1:1883"
      client_id: "my_client"

  # Lifecycle-only extensions (no options needed)
  - name: log_control_extension
    path: ./libibmw_log_control.so
  - name: parameter_extension
    path: ./libibmw_parameter_service.so
  - name: time_manipulator_extension
    path: ./libibmw_time_manipulator.so
  - name: topic_logger_extension
    path: ./libibmw_topic_logger.so
  - name: service_introspection_extension
    path: ./libibmw_service_introspection.so
    options:
      client_info_topic_name: /ibmw/service_introspection/client_info
      server_info_topic_name: /ibmw/service_introspection/server_info
  - name: proxy_extension
    path: ./libibmw_proxy_driver.so
  - name: record_playback_extension
    path: ./libibmw_record_playback.so
```

**Schema rules:**
- `name` and `path` are required. `options` is a free-form dict.
- No validation of option keys (extension-specific).
- `dds_extension` name is special: when present, enables DDS driver in
  messaging config and CMake feature guards.

### 2.4 `executors` Section (NEW)

Directly declares executors for the scheduling section. When omitted, executors
are auto-derived from module requirements (v1 behavior).

```yaml
executors:
  - name: work_executor
    type: thread_pool
    options:
      thread_num: 2

  - name: thread_safe_executor
    type: strand
    options:
      bind_thread_pool_executor_name: work_executor

  - name: time_schedule_executor
    type: thread_pool
    options:
      thread_num: 2

  - name: timer_executor
    type: timer
    options:
      bind_executor: work_thread_pool

  - name: sched_fifo_thread
    type: thread_pool
    options:
      thread_num: 1
      thread_sched_policy: "SCHED_FIFO:80"
      thread_bind_cpu: [0, 1]
      timeout_alarm_threshold_us: 100

  - name: parallel_exec
    type: parallel
    options:
      thread_num: 4
      queue_threshold: 10000

  - name: time_manip
    type: time_manipulator
    options:
      bind_executor: work_thread_pool
```

**Schema rules:**
- `name` and `type` are required.
- Valid types: `thread_pool`, `strand`, `dedicated`, `timer`, `parallel`,
  `time_manipulator`.
- `options` is a free-form dict (type-specific).

### 2.5 `dds_driver` Section (NEW)

Controls `messaging.drivers[type=dds]` configuration. Only relevant when
`dds_extension` is in `extensions`.

```yaml
dds_driver:
  # ROS2 compatibility
  ros2_compatible_mode: true         # optional, default: false
  ros2_topic_prefix: "rt"            # optional, default: ""

  # Zero-copy (for loaned message support)
  zero_copy:
    mode: auto                       # auto | strict | compat
    log_level: info                  # optional
    monitoring: true                 # optional
    monitoring_interval: 5000        # optional, ms

  # QoS defaults (applied to all topics via regex "(.*)")
  publisher_qos:
    history: keep_last
    depth: 10
    reliability: reliable
    durability: volatile
    data_sharing: auto               # optional, for zero-copy

  subscriber_qos:
    history: keep_last
    depth: 10
    reliability: reliable
    durability: volatile

  # Per-topic QoS overrides (optional)
  publisher_qos_overrides:
    - topic: "image_topic"
      qos:
        history: keep_last
        depth: 5
        reliability: best_effort
        data_sharing: auto

  subscriber_qos_overrides:
    - topic: "image_topic"
      qos:
        history: keep_last
        depth: 5
        reliability: best_effort
```

**Schema rules:**
- Entire section is optional. Defaults are auto-derived when dds_extension is present.
- `zero_copy.mode`: enum `auto`, `strict`, `compat`.
- QoS fields: `history` (keep_last/keep_all), `depth`, `reliability` (reliable/best_effort),
  `durability` (volatile/transient_local), `data_sharing` (auto/off).

### 2.6 `messaging_driver_options` Section (NEW)

For non-DDS messaging drivers (e.g., `local` driver with specific options).

```yaml
messaging_driver_options:
  local:
    subscriber_use_inline_executor: false
    subscriber_executor: work_executor
  http: {}    # net_extension creates this driver type
```

### 2.7 `modules` Section Extensions

#### 2.7.1 New Module Types

Add to `VALID_MODULE_TYPES`:

| Type | Description |
|------|-------------|
| `context_publisher` | Uses `ibmw::context::Context` API for publishing |
| `context_subscriber` | Uses Context API for subscribing (inline or on-executor) |
| `context_rpc_server` | Uses Context API for RPC server |
| `context_rpc_client` | Uses Context API for RPC client |
| `timer` | Timer-based periodic execution |
| `coroutine` | Coroutine-based module with AsyncScope |
| `coroutine_loop` | Minimal coroutine loop module |
| `executor_demo` | Executor dispatch/TryDispatch patterns |
| `loaned_publisher` | DDS zero-copy publisher using LoanedMessage API |
| `ros2_interop_publisher` | ROS2 interop publisher (3-arg RegisterSendType) |
| `ros2_interop_subscriber` | ROS2 interop subscriber (ContextRef callback) |
| `real_time` | Real-time scheduler introspection module |
| `logger` | Logging demonstration module |

#### 2.7.2 New Module-Level Fields

```yaml
modules:
  # Loaned message publisher (zero-copy)
  - name: PlainPublisherModule
    type: loaned_publisher
    transport: dds
    topic: point_topic
    message_type: "geometry_msgs::msg::Point"
    ros2_package: geometry_msgs
    ros2_msg_include: "geometry_msgs/msg/point.hpp"
    is_plain_type: true                  # NEW: type is trivially copyable + standard layout
    zero_copy_config: auto               # NEW: auto | strict | compat (per-module override)
    dds_type_name: "geometry_msgs::msg::dds_::Point_"  # NEW: explicit DDS type name string

  # ROS2 interop publisher (three-argument API)
  - name: Ros2InteropPublisher
    type: ros2_interop_publisher
    transport: dds                       # NOTE: uses DDS transport, not ros2!
    topic: chatter
    message_type: "std_msgs::msg::String"
    ros2_package: std_msgs               # for find_package and include
    ros2_msg_include: "std_msgs/msg/string.hpp"
    ros2_type_name: "std_msgs::msg::dds_::String_"  # NEW: ROS2 DDS type name string

  # ROS2 interop subscriber
  - name: Ros2InteropSubscriber
    type: ros2_interop_subscriber
    transport: dds
    topic: chatter
    message_type: "std_msgs::msg::String"
    ros2_package: std_msgs
    ros2_msg_include: "std_msgs/msg/string.hpp"
    ros2_type_name: "std_msgs::msg::dds_::String_"

  # Context publisher
  - name: ContextPublisher
    type: context_publisher
    transport: local                     # local | dds
    topic: test_topic
    message_type: "ExampleEventMsg"      # DDS type or custom
    custom_config:                       # NEW: module custom config keys
      topic: test_topic
      transport_frq: 1

  # Context subscriber (inline)
  - name: ContextSubscriberInline
    type: context_subscriber
    transport: local
    topic: test_topic
    message_type: "ExampleEventMsg"
    subscriber_mode: inline              # NEW: inline | on_executor
    custom_config:
      topic: test_topic

  # Context RPC server
  - name: ContextRpcServer
    type: context_rpc_server
    transport: dds
    dds_service: ExampleService
    server_mode: inline                  # inline | on_executor

  # Context RPC client
  - name: ContextRpcClient
    type: context_rpc_client
    transport: dds
    dds_service: ExampleService
    custom_config:
      rpc_frq: 0.5

  # Timer module
  - name: TimerModule
    type: timer
    executor_name: timer_executor        # NEW: which executor to bind the timer to

  # Coroutine module
  - name: CoModule
    type: coroutine
    executors:                           # NEW: named executors this module uses
      - work_executor
      - time_schedule_executor

  # Executor demo module
  - name: ExecutorModule
    type: executor_demo
    executors:
      - work_executor
      - thread_safe_executor
      - time_schedule_executor

  # Logger module
  - name: LoggerModule
    type: logger
    log_level: TRACE                     # NEW: module-level log_level override

  # Real-time module
  - name: RealTimeModule
    type: real_time
    executors:
      - sched_fifo_thread
      - sched_other_thread
      - sched_rr_thread
```

### 2.8 `custom_dds_types` Section (NEW — Auto-Generated CDR Types)

Defines custom DDS message types that ibmw-gen **auto-generates** as complete
C++ header files with FastCDR serialization. This replaces the hand-written
pattern seen in `dds_native_demo/protocol/hello_msg.h` and
`dds_image_demo/protocol/image_msg.h`.

For each type, ibmw-gen generates:
1. A struct definition with fields and default constructor
2. Member `serialize()` / `deserialize()` methods (FastCDR v1)
3. Three `eprosima::fastcdr` template specializations:
   `calculate_serialized_size<T>`, `serialize<T>`, `deserialize<T>`
4. `IBMW_DDS_TYPE_NAME(FullyQualified::Type, "dds_type_name")` macro call
5. A `static constexpr bool kIsPlainType = true/false;` (auto-detected)

#### 2.8.1 Type Mapping

ibmw-gen maps YAML type strings to C++ types:

| YAML type | C++ type | Plain? | CDR alignment |
|-----------|----------|--------|---------------|
| `bool` | `bool` | yes | 1 |
| `int8` | `int8_t` | yes | 1 |
| `uint8` | `uint8_t` | yes | 1 |
| `int16` | `int16_t` | yes | 2 |
| `uint16` | `uint16_t` | yes | 2 |
| `int32` | `int32_t` | yes | 4 |
| `uint32` | `uint32_t` | yes | 4 |
| `int64` | `int64_t` | yes | 8 |
| `uint64` | `uint64_t` | yes | 8 |
| `float32` | `float` | yes | 4 |
| `float64` | `double` | yes | 8 |
| `string` | `std::string` | **NO** | 4 (length prefix) |
| `array<T, N>` | `std::array<T, N>` | if T is plain | same as T |
| `vector<T>` | `std::vector<T>` | **NO** | 4 (length prefix) |
| `<EnumName>` | user-defined enum class | yes (uint32_t underlying) | 4 |

**Plain type rule**: A struct is plain (supports DDS zero-copy) if and only
if ALL its fields are plain types. This means:
- `std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>` is true
- No `string`, no `vector`, no nested non-plain structs
- `std::array<PlainT, N>` is plain; `std::array<NonPlainT, N>` is not
- Enum classes with fixed underlying type are plain

ibmw-gen **automatically determines** whether a type is plain and emits:
```cpp
// In generated header, inside the struct:
static constexpr bool kIsPlainType = true;  // or false
```

#### 2.8.2 YAML Schema

```yaml
custom_dds_types:
  # Optional: C++ namespace for all types in this section.
  # Defaults to project.namespace if omitted.
  namespace: "ibmw::examples::dds_native_demo"

  # Optional: enum definitions (referenced by message fields)
  enums:
    - name: ImageEncoding
      underlying_type: uint32   # optional, default: uint32
      values:
        - { name: RGB8, value: 0 }
        - { name: RGBA8, value: 1 }
        - { name: GRAY8, value: 2 }
        - { name: GRAY16, value: 3 }

  # Optional: constants (emitted as static constexpr in namespace)
  constants:
    - { name: kImageBufferSize, type: uint64, value: 1048576 }

  # Required: message type definitions
  messages:
    - name: HelloMsg
      # Optional DDS type name string for IBMW_DDS_TYPE_NAME macro.
      # Default: "{namespace}::{name}" (e.g., "ibmw::examples::dds_native_demo::HelloMsg")
      dds_type_name: "ibmw::examples::dds_native_demo::HelloMsg"
      # Optional: override the generated header filename.
      # Default: snake_case(name).h (e.g., "hello_msg.h")
      header_filename: hello_msg.h
      # Optional: provide a custom constructor with args.
      # If omitted, only default constructor is generated.
      constructor_args: true
      fields:
        - { name: id, type: uint64, default: "0" }
        - { name: message, type: string }
        - { name: timestamp, type: int64, default: "0" }

    - name: ImageMsg
      dds_type_name: "ibmw::examples::dds_image_demo::ImageMsg"
      fields:
        - { name: frame_id, type: uint64, default: "0" }
        - { name: timestamp, type: int64, default: "0" }
        - { name: width, type: uint32, default: "0" }
        - { name: height, type: uint32, default: "0" }
        - { name: encoding, type: ImageEncoding, default: "ImageEncoding::RGB8" }
        - { name: data, type: "array<uint8, 1048576>" }
        - { name: data_size, type: uint32, default: "0" }
```

#### 2.8.3 Generated Output

For the `HelloMsg` example above, ibmw-gen generates
`protocol/hello_msg.h`:

```cpp
// Auto-generated by ibmw-gen. DO NOT EDIT.
// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

#pragma once

#include <cstdint>
#include <string>

#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>
#include "dds_types/util/fastcdr_v1_compat.h"
#include "dds_types/util/dds_type_support.h"

namespace ibmw::examples::dds_native_demo {

struct HelloMsg {
  static constexpr bool kIsPlainType = false;  // contains std::string

  uint64_t id = 0;
  std::string message;
  int64_t timestamp = 0;

  HelloMsg() = default;
  HelloMsg(uint64_t id_, std::string message_, int64_t timestamp_)
      : id(id_), message(std::move(message_)), timestamp(timestamp_) {}

  void serialize(eprosima::fastcdr::Cdr& cdr) const {
    cdr << id << message << timestamp;
  }

  void deserialize(eprosima::fastcdr::Cdr& cdr) {
    cdr >> id >> message >> timestamp;
  }
};

}  // namespace ibmw::examples::dds_native_demo

namespace eprosima {
namespace fastcdr {

template <>
inline size_t calculate_serialized_size(
    CdrSizeCalculator& calculator,
    const ibmw::examples::dds_native_demo::HelloMsg& data,
    size_t& current_alignment) {
  size_t total = 0;
  total += calculator.calculate_member_serialized_size(
      MemberId(0), data.id, current_alignment);
  total += calculator.calculate_member_serialized_size(
      MemberId(1), data.message, current_alignment);
  total += calculator.calculate_member_serialized_size(
      MemberId(2), data.timestamp, current_alignment);
  return total;
}

template <>
inline void serialize(
    Cdr& scdr,
    const ibmw::examples::dds_native_demo::HelloMsg& data) {
  scdr << data.id
       << data.message
       << data.timestamp;
}

template <>
inline void deserialize(
    Cdr& cdr,
    ibmw::examples::dds_native_demo::HelloMsg& data) {
  cdr >> data.id
      >> data.message
      >> data.timestamp;
}

}  // namespace fastcdr
}  // namespace eprosima

IBMW_DDS_TYPE_NAME(ibmw::examples::dds_native_demo::HelloMsg,
                   "ibmw::examples::dds_native_demo::HelloMsg")
```

For `ImageMsg` with enum, ibmw-gen generates `protocol/image_msg.h`:

```cpp
// Auto-generated by ibmw-gen. DO NOT EDIT.
#pragma once

#include <array>
#include <cstdint>
#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>
#include "dds_types/util/fastcdr_v1_compat.h"
#include "dds_types/util/dds_type_support.h"

namespace ibmw::examples::dds_image_demo {

static constexpr uint64_t kImageBufferSize = 1048576;

enum class ImageEncoding : uint32_t {
  RGB8 = 0,
  RGBA8 = 1,
  GRAY8 = 2,
  GRAY16 = 3
};

struct ImageMsg {
  static constexpr bool kIsPlainType = true;  // all fields are plain

  uint64_t frame_id = 0;
  int64_t timestamp = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  ImageEncoding encoding = ImageEncoding::RGB8;
  std::array<uint8_t, kImageBufferSize> data{};
  uint32_t data_size = 0;

  ImageMsg() = default;

  void serialize(eprosima::fastcdr::Cdr& cdr) const {
    cdr << frame_id << timestamp << width << height
        << static_cast<uint32_t>(encoding) << data << data_size;
  }

  void deserialize(eprosima::fastcdr::Cdr& cdr) {
    cdr >> frame_id >> timestamp >> width >> height;
    uint32_t enc_val;
    cdr >> enc_val;
    encoding = static_cast<ImageEncoding>(enc_val);
    cdr >> data >> data_size;
  }
};

}  // namespace ibmw::examples::dds_image_demo

namespace eprosima {
namespace fastcdr {

template <>
inline size_t calculate_serialized_size(
    CdrSizeCalculator& calculator,
    const ibmw::examples::dds_image_demo::ImageMsg& data,
    size_t& current_alignment) {
  size_t total = 0;
  total += calculator.calculate_member_serialized_size(
      MemberId(0), data.frame_id, current_alignment);
  // ... (all fields)
  total += calculator.calculate_member_serialized_size(
      MemberId(4), static_cast<uint32_t>(data.encoding), current_alignment);
  // ...
  return total;
}

template <>
inline void serialize(Cdr& scdr,
    const ibmw::examples::dds_image_demo::ImageMsg& data) {
  scdr << data.frame_id << data.timestamp << data.width << data.height
       << static_cast<uint32_t>(data.encoding) << data.data << data.data_size;
}

template <>
inline void deserialize(Cdr& cdr,
    ibmw::examples::dds_image_demo::ImageMsg& data) {
  cdr >> data.frame_id >> data.timestamp >> data.width >> data.height;
  uint32_t enc_val;
  cdr >> enc_val;
  data.encoding = static_cast<ibmw::examples::dds_image_demo::ImageEncoding>(enc_val);
  cdr >> data.data >> data.data_size;
}

}  // namespace fastcdr
}  // namespace eprosima

IBMW_DDS_TYPE_NAME(ibmw::examples::dds_image_demo::ImageMsg,
                   "ibmw::examples::dds_image_demo::ImageMsg")
```

#### 2.8.4 ROS2 Interop Naming Convention

When a custom DDS type needs to interoperate with ROS2 nodes, the DDS type
name must follow the ROS2 convention: `<pkg>::msg::dds_::<Type>_`.

This is handled at the **module level** via `ros2_type_name`, not in the
`custom_dds_types` section itself. The `dds_type_name` in `custom_dds_types`
is the native IBMW DDS type name used for non-ROS2 scenarios.

For ROS2 interop with custom types, the user would:
1. Define the type in `custom_dds_types` with a native `dds_type_name`
2. In the module, specify `ros2_type_name` to override the DDS type name
   used for ROS2 registration

#### 2.8.5 Schema Rules

- `messages` is required; at least one message must be defined.
- `namespace` defaults to `project.namespace` if omitted.
- `enums` is optional; enum names can be referenced in field types.
- `constants` is optional; constant names can be referenced in field defaults
  and array sizes.
- Each message must have `name` (PascalCase) and `fields` (non-empty list).
- Each field must have `name` and `type`.
- `type` must be a recognized primitive, an enum name from `enums`, or
  `array<T, N>` / `vector<T>` where T is a recognized type.
- `default` is optional; if omitted, primitives get zero-initialization,
  strings/vectors get default construction, arrays get `{}`.
- `dds_type_name` defaults to `"{namespace}::{name}"`.
- `header_filename` defaults to `snake_case(name).h`.
- `constructor_args: true` generates a non-default constructor with all
  fields as parameters (with `std::move` for string/vector/array fields).
- Generated files are placed in `protocol/` subdirectory of the output.

#### 2.8.6 Plain Type Auto-Detection Logic (in schema.py)

```python
# Types that are NOT plain (contain heap allocation)
_NON_PLAIN_TYPES = {"string", "vector"}

def _is_plain_type(field_type: str, enums: dict) -> bool:
    """Determine if a field type is plain (trivially_copyable + standard_layout)."""
    if field_type in _NON_PLAIN_TYPES:
        return False
    if field_type.startswith("vector<"):
        return False
    if field_type.startswith("array<"):
        # array<T, N> is plain if T is plain
        inner = field_type[6:].rsplit(",", 1)[0].strip()
        return _is_plain_type(inner, enums)
    if field_type in enums:
        return True  # enum class with fixed underlying type
    # All primitive numeric types are plain
    return field_type in PLAIN_PRIMITIVE_TYPES

def _is_message_plain(msg: dict, enums: dict) -> bool:
    """A message is plain iff ALL its fields are plain."""
    return all(_is_plain_type(f["type"], enums) for f in msg["fields"])
```

This is computed during `_enrich()` and stored as `msg["_is_plain"]`.

### 2.9 `apps` Section Extensions

#### New app-level fields:

```yaml
apps:
  - name: my_app
    type: pubsub                       # pubsub | rpc | bare | context
    modules: [Pub, Sub]

    # NEW: per-app config variants
    configs:
      - name: my_app                   # default config
      - name: my_app_strict            # variant with different settings
        overrides:
          dds_driver:
            zero_copy:
              mode: strict
      - name: my_app_sub_only          # variant with subset of modules
        modules: [Sub]

    # NEW: per-app extension requirements
    requires_extensions: [dds_extension]  # auto-derived if not set

    # NEW: per-app messaging overrides
    messaging_drivers: [dds]            # override default [local]
```

**Decision: Config variants are OUT OF SCOPE for v2.0**. The multi-config
pattern (loaned_message_demo has 7 configs) is too specialized. For v2.0,
each app gets exactly one app config and one pkg config. Users can manually
duplicate/edit configs for variants.

### 2.10 Summary of Schema Changes

| Section | Status | Affects |
|---------|--------|---------|
| `logging` | NEW | config_gen |
| `extensions` | NEW | config_gen, cmake_gen |
| `executors` | NEW | config_gen |
| `dds_driver` | NEW | config_gen |
| `messaging_driver_options` | NEW | config_gen |
| `custom_dds_types` | NEW | **custom_dds_types_gen (NEW)**, cmake_gen, module_gen |
| modules: 13 new types | EXTENDED | module_gen, cmake_gen |
| modules: new fields | EXTENDED | module_gen, config_gen |
| apps: `messaging_drivers` | EXTENDED | config_gen |

---

## 3. Generator Changes

### 3.1 schema.py Changes

**New validation functions:**

```
_validate_logging(data)       # validate logging section
_validate_extensions(data)    # validate extensions list
_validate_executors(data)     # validate executors list, cross-ref
_validate_dds_driver(data)    # validate dds_driver section
_validate_custom_dds_types(data)  # validate custom_dds_types: field types,
                                  # enum refs, array syntax, constants;
                                  # compute _is_plain per message
```

**Extended `VALID_MODULE_TYPES`:**
```python
VALID_MODULE_TYPES = {
    # v1
    "bare", "publisher", "subscriber", "rpc_server", "rpc_client",
    # v2
    "context_publisher", "context_subscriber",
    "context_rpc_server", "context_rpc_client",
    "loaned_publisher",
    "ros2_interop_publisher", "ros2_interop_subscriber",
    "timer", "coroutine", "coroutine_loop",
    "executor_demo", "real_time", "logger",
}
```

**Extended `_validate_modules`:**
- `loaned_publisher` requires: `transport: dds`, `topic`, `message_type`,
  `ros2_package`, `ros2_msg_include`.
- `ros2_interop_publisher/subscriber` requires: `transport: dds`, `topic`,
  `message_type`, `ros2_package`, `ros2_msg_include`.
- `context_*` types: validate `transport`, cross-ref `dds_service` for RPC types.
- `timer`: validate `executor_name` exists in executors.
- `coroutine`/`executor_demo`/`real_time`: validate `executors` list.

**Extended `_enrich`:**
- Compute `_has_dds_extension`, `_has_ros2_types`, `_needs_whole_archive`,
  `_needs_fastcdr`, `_needs_fastdds`.
- Cross-link modules to their DDS/ROS2 type info.
- For `custom_dds_types`: compute `_is_plain` per message, resolve enum
  references, compute C++ types for each field, determine required includes
  (`<string>`, `<array>`, `<vector>`, `<cstdint>`), compute `_fq_name`
  (fully-qualified C++ name with namespace).

### 3.2 config_gen.py Changes

**Major rewrite** to support all new config sections.

**New helper functions:**

```python
_gen_logging_section(project)        # logging + backends
_gen_extensions_section(project)     # extensions.items
_gen_executors_section(project, app) # scheduling.executors
_gen_messaging_section(project, app) # messaging with DDS driver + QoS
_gen_service_section(project, app)   # service section
_gen_modules_section(app, is_pkg)    # modules section
_gen_custom_config_section(app, module_map)  # root-level module configs
```

**Key changes:**
1. **Logging**: emit `rotate_file` backend with all options, console `pattern`.
2. **Extensions**: emit `ibmw.extensions.items[]` block.
3. **Executors**: emit explicit executor list from `executors` section, or
   auto-derive (v1 behavior) when section is absent.
4. **DDS driver**: emit `messaging.drivers[type=dds]` with QoS, `ros2_compatible_mode`,
   `zero_copy` section.
5. **Module custom config**: emit root-level YAML sections for modules with
   `custom_config` field.

### 3.3 cmake_gen.py Changes

**New CMake patterns to support:**

1. **Feature guards:**
   ```cmake
   if(NOT IBMW_BUILD_DDS_EXTENSION)
     message(STATUS "Skip ... because IBMW_BUILD_DDS_EXTENSION is OFF")
     return()
   endif()
   ```
   Emitted when any module needs DDS extension.

2. **find_package for ROS2 packages:**
   ```cmake
   find_package(ament_cmake REQUIRED)
   find_package(geometry_msgs REQUIRED)
   find_package(std_msgs REQUIRED)
   ```

3. **find_package(fastcdr) for custom DDS types:**
   ```cmake
   find_package(fastcdr REQUIRED)
   ```

4. **find_package(fastdds/fastrtps) for loaned messages:**
   ```cmake
   find_package(fastdds 3 QUIET)
   if(NOT fastdds_FOUND)
     find_package(fastrtps REQUIRED)
     find_package(fastcdr REQUIRED)
     set(fastdds_FOUND TRUE)
     if(NOT TARGET fastdds)
       add_library(fastdds ALIAS fastrtps)
     endif()
   endif()
   ```

5. **Whole-archive linking helper:**
   ```cmake
   # Helper macro for whole-archive linking
   function(ibmw_whole_archive_link target lib)
     if(APPLE)
       target_link_libraries(${target} PUBLIC -Wl,-force_load ${lib})
     elseif(UNIX)
       target_link_libraries(${target} PUBLIC
         -Wl,--whole-archive ${lib} -Wl,--no-whole-archive)
     else()
       target_link_libraries(${target} PUBLIC ${lib})
     endif()
   endfunction()
   ```

6. **Module DEPS extensions:**
   - `loaned_publisher` / `ros2_interop_*`: `ibmw::extensions::ros2_types`,
     whole-archive `ibmw::extensions::ros2_types_impl`, `${ros2_pkg_TARGETS}`,
     `fastdds` (loaned only).
   - `context_*` with DDS: `ibmw::extensions::dds_types`, CDR impl targets.
   - `custom_dds_types`: `fastcdr`, include directories.

### 3.4 module_gen.py Changes

**New generator functions (13 new module type generators):**

Each new type gets `_gen_<type>_header()` and `_gen_<type>_cc()` functions.

#### 3.4.1 `loaned_publisher` Generator

Generates code using the LoanedMessage API:

```cpp
// Header: members
ibmw::Runtime core_;
ibmw::transport::DdsTransportDriver* dds_driver_ = nullptr;
std::unique_ptr<ibmw::ros2::transport::Ros2StylePublisher<MsgType>> ros2_pub_;

// Setup: DDS driver injection + Ros2StylePublisher creation
auto& bus = core_.GetTransportBus();
for (auto& drv : bus.GetUsedTransportDriver()) {
  dds_driver_ = dynamic_cast<ibmw::transport::DdsTransportDriver*>(drv.get());
  if (dds_driver_) break;
}
ros2_pub_ = dds_driver_->create_publisher_with_driver_config<MsgType>(
    dds_driver_, topic, type_name);

// Publish loop:
auto loaned = ros2_pub_->borrow_loaned_message();
if (loaned.is_valid()) {
  loaned->x = 1.0;  // populate
  ros2_pub_->publish(std::move(loaned));
}
```

#### 3.4.2 `ros2_interop_publisher` Generator

Uses three-argument RegisterSendType:

```cpp
// Setup:
auto publisher = core_.GetTransport().GetPublisher(topic);
ibmw::ros2::transport::RegisterSendType<MsgType>(publisher, type_name);

// Publish:
MsgType msg;
ibmw::ros2::transport::Send(publisher, type_name, msg);
```

#### 3.4.3 `ros2_interop_subscriber` Generator

Uses ContextRef callback:

```cpp
// Setup:
auto subscriber = core_.GetTransport().GetSubscriber(topic);
ibmw::ros2::transport::SubscribeType<MsgType>(
    subscriber, type_name,
    [this](ibmw::transport::ContextRef ctx,
           const std::shared_ptr<const MsgType>& msg) {
      MW_INFO("Received message");
    });
```

#### 3.4.4 `context_publisher` Generator

Uses Context API:

```cpp
// Setup:
ctx_ptr_ = std::make_shared<ibmw::context::Context>(core);
ctx_ptr_->LetMe();
auto pub = ctx_ptr_->CreatePublisher<MsgType>(topic);

// PublishLoop:
ctx_ptr_->LetMe();
MsgType msg;
pub.Publish(msg);
```

#### 3.4.5 `context_subscriber` Generator

Two variants based on `subscriber_mode`:

**inline:**
```cpp
ctx_ptr_->CreateSubscriber<MsgType>(topic,
    [this](const std::shared_ptr<const MsgType>& msg) {
      ctx_ptr_->LetMe();
      // handle
    });
```

**on_executor:**
```cpp
auto executor = ctx_ptr_->CreateExecutor("work_executor");
ctx_ptr_->CreateSubscriber<MsgType>(topic, executor,
    [this](const std::shared_ptr<const MsgType>& msg) {
      ctx_ptr_->LetMe();
      // handle
    });
```

#### 3.4.6 `context_rpc_server` Generator

Similar to existing `dds_rpc_server` but uses Context API pattern.
Two variants based on `server_mode`: `inline` (ServeInline) and
`on_executor` (ServeOnExecutor).

#### 3.4.7 `context_rpc_client` Generator

Uses Context API + coroutine pattern. Similar to existing `dds_rpc_client`.

#### 3.4.8 `timer` Generator

```cpp
// Setup:
auto executor = core_.GetExecutor().GetExecutor("timer_executor");
timer_ = ibmw::executor::CreateTimer(executor,
    std::chrono::milliseconds(1000),
    [this]() { OnTimer(); });

// Stop:
timer_->Cancel();
```

#### 3.4.9 `coroutine` Generator

```cpp
// Members:
ibmw::co::AsyncScope scope_;
ibmw::executor::ExecutorHandle time_executor_;

// Run:
auto executor = ibmw::co::IbmwExecutor(time_executor_);
scope_.spawn(ibmw::co::On(executor, MainLoop()));

// MainLoop:
ibmw::co::Task<void> MainLoop() {
  while (running_) {
    co_await ibmw::co::ScheduleAfter(executor, 1s);
    // ...
  }
  co_return;
}
```

#### 3.4.10 `executor_demo` Generator

```cpp
// Dispatch patterns:
core_.GetExecutor().Dispatch(executor,
    [this]() { /* work */ });

core_.GetExecutor().TryDispatch(executor, latch,
    [this]() { /* work */ });

core_.GetExecutor().DispatchAfter(executor, 500ms,
    [this]() { /* delayed work */ });
```

#### 3.4.11 `logger` Generator

Uses all 6 MW log macros:

```cpp
MW_TRACE("trace msg");
MW_DEBUG("debug msg");
MW_INFO("info msg");
MW_WARN("warn msg");
MW_ERROR("error msg");
MW_FATAL("fatal msg");
```

#### 3.4.12 `real_time` Generator

POSIX scheduling introspection:

```cpp
// Reads thread scheduling policy + CPU affinity
// Uses sched_getscheduler, sched_getparam, CPU_ISSET
```

#### 3.4.13 `coroutine_loop` Generator

Minimal co_await loop pattern:

```cpp
// Setup:
auto handle = core_.GetExecutor().GetExecutor("time_schedule_executor");
ibmw::co::IbmwExecutor executor(handle);

// Run:
scope_.spawn(ibmw::co::On(executor, Loop()));

ibmw::co::Task<void> Loop() {
  while (running_) {
    co_await ibmw::co::ScheduleAfter(executor, 1s);
    MW_INFO("tick");
  }
  co_return;
}
```

### 3.5 Updated _GENERATORS Dispatch Table

```python
_GENERATORS = {
    # v1 (unchanged)
    ("bare", None):           (_gen_bare_header, _gen_bare_cc),
    ("publisher", "dds"):     (_gen_dds_publisher_header, _gen_dds_publisher_cc),
    ("subscriber", "dds"):    (_gen_dds_subscriber_header, _gen_dds_subscriber_cc),
    ("rpc_server", "dds"):    (_gen_dds_rpc_server_header, _gen_dds_rpc_server_cc),
    ("rpc_client", "dds"):    (_gen_dds_rpc_client_header, _gen_dds_rpc_client_cc),
    ("publisher", "json"):    (_gen_json_publisher_header, _gen_json_publisher_cc),
    ("subscriber", "json"):   (_gen_json_subscriber_header, _gen_json_subscriber_cc),
    ("rpc_server", "json"):   (_gen_json_rpc_server_header, _gen_json_rpc_server_cc),
    ("rpc_client", "json"):   (_gen_json_rpc_client_header, _gen_json_rpc_client_cc),
    ("publisher", "ros2"):    (_gen_ros2_publisher_header, _gen_ros2_publisher_cc),
    ("subscriber", "ros2"):   (_gen_ros2_subscriber_header, _gen_ros2_subscriber_cc),
    ("rpc_server", "ros2"):   (_gen_ros2_rpc_server_header, _gen_ros2_rpc_server_cc),
    ("rpc_client", "ros2"):   (_gen_ros2_rpc_client_header, _gen_ros2_rpc_client_cc),

    # v2 new types
    ("loaned_publisher", "dds"):         (...),
    ("ros2_interop_publisher", "dds"):   (...),
    ("ros2_interop_subscriber", "dds"):  (...),
    ("context_publisher", None):         (...),  # transport-agnostic
    ("context_publisher", "local"):      (...),
    ("context_publisher", "dds"):        (...),
    ("context_subscriber", None):        (...),
    ("context_subscriber", "local"):     (...),
    ("context_rpc_server", "dds"):       (...),
    ("context_rpc_client", "dds"):       (...),
    ("timer", None):                     (...),
    ("coroutine", None):                 (...),
    ("coroutine_loop", None):            (...),
    ("executor_demo", None):             (...),
    ("real_time", None):                 (...),
    ("logger", None):                    (...),
}
```

### 3.6 custom_dds_types_gen.py (NEW)

New generator file: `generators/custom_dds_types_gen.py`.

**Purpose:** Generate C++ header files for custom DDS types defined in the
`custom_dds_types` YAML section. Each message produces one `.h` file in the
`protocol/` subdirectory.

**Public API:**

```python
def generate(project: dict, out_dir: str) -> list[str]:
    """Generate custom DDS type headers.

    Args:
        project: The enriched project data from schema.py
        out_dir: Output directory root

    Returns:
        List of generated file paths (relative to out_dir)
    """
```

**Internal structure:**

```python
# ── Type mapping ──

YAML_TO_CPP = {
    "bool": "bool",
    "int8": "int8_t", "uint8": "uint8_t",
    "int16": "int16_t", "uint16": "uint16_t",
    "int32": "int32_t", "uint32": "uint32_t",
    "int64": "int64_t", "uint64": "uint64_t",
    "float32": "float", "float64": "double",
    "string": "std::string",
}

# Types that break plainness (heap allocation)
NON_PLAIN_TYPES = {"string", "vector"}

PLAIN_PRIMITIVE_TYPES = {
    "bool", "int8", "uint8", "int16", "uint16",
    "int32", "uint32", "int64", "uint64",
    "float32", "float64",
}

def _resolve_cpp_type(yaml_type: str, enums: dict, ns: str) -> str:
    """Convert YAML type to C++ type string.

    - Primitives: YAML_TO_CPP lookup
    - Enums: namespace-qualified enum name
    - array<T, N>: std::array<cpp_type(T), N>
    - vector<T>: std::vector<cpp_type(T)>
    """

def _is_plain(yaml_type: str, enums: dict) -> bool:
    """Check if a YAML type is plain (trivially_copyable + standard_layout)."""

def _is_enum_field(yaml_type: str, enums: dict) -> bool:
    """Check if a yaml_type references a defined enum."""

def _required_includes(msg: dict, enums: dict) -> set[str]:
    """Determine which C++ includes are needed for a message.

    Returns set of headers like "<string>", "<array>", "<vector>", "<cstdint>".
    """

# ── Code generation ──

def _gen_enum(enum_def: dict, ns_cpp: str) -> str:
    """Generate enum class definition."""

def _gen_constants(constants: list, ns_cpp: str) -> str:
    """Generate static constexpr constant definitions."""

def _gen_struct(msg: dict, enums: dict, constants: dict, ns_cpp: str) -> str:
    """Generate struct definition with serialize/deserialize members."""

def _gen_constructor(msg: dict, enums: dict, ns_cpp: str) -> str:
    """Generate non-default constructor if constructor_args: true."""

def _gen_serialize_member(fields: list, enums: dict) -> str:
    """Generate the member serialize() function body."""

def _gen_deserialize_member(fields: list, enums: dict, ns_cpp: str) -> str:
    """Generate the member deserialize() function body.

    Enum fields need special handling:
      uint32_t enc_val;
      cdr >> enc_val;
      field = static_cast<EnumType>(enc_val);
    """

def _gen_calculate_serialized_size(msg: dict, enums: dict,
                                    fq_name: str) -> str:
    """Generate the calculate_serialized_size template specialization.

    Enum fields use: static_cast<uint32_t>(data.field)
    All other fields use: data.field directly
    """

def _gen_serialize_free(msg: dict, enums: dict, fq_name: str) -> str:
    """Generate the serialize<T> template specialization."""

def _gen_deserialize_free(msg: dict, enums: dict,
                           fq_name: str, ns_cpp: str) -> str:
    """Generate the deserialize<T> template specialization.

    Enum fields need the same cast-via-temp pattern as member deserialize.
    """

def _gen_header_file(msg: dict, enums: list, constants: list,
                      ns_cpp: str, project: dict) -> str:
    """Assemble the complete header file content for one message type."""
```

**Integration with ibmw_gen.py main:**

The main generator loop calls `custom_dds_types_gen.generate(project, out_dir)`
alongside the existing generators. This produces `protocol/*.h` files that
modules can include via `#include "protocol/hello_msg.h"`.

**CMake integration (cmake_gen.py):**

When `custom_dds_types` is present:
1. Add `find_package(fastcdr REQUIRED)` to the CMakeLists.txt
2. Add the protocol directory to include paths:
   `target_include_directories(target PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})`
3. Link `fastcdr` to targets that use custom DDS types

---

## 4. Implementation Plan

### Phase A: Schema + Config + Custom DDS Types Generator (Critical Path)

**Priority: HIGH** -- blocks all demo migrations.

1. **schema.py**: Add new sections, types, validation, enrichment.
   - Estimated: +250 lines (554 total).
2. **config_gen.py**: Rewrite to support all config sections.
   - Estimated: +300 lines (513 total).
3. **custom_dds_types_gen.py** (NEW): Generate CDR type headers.
   - Estimated: ~350 lines (new file).

### Phase B: CMake Generator

**Priority: HIGH** -- blocks compilation.

4. **cmake_gen.py**: Add feature guards, find_package, whole-archive,
   custom DDS type header include directories.
   - Estimated: +150 lines (438 total).

### Phase C: Module Generators

**Priority: HIGH for loaned/interop/context; MEDIUM for others.**

5. **module_gen.py** -- add generators in priority order:
   a. `loaned_publisher` (critical, user-requested)
   b. `ros2_interop_publisher` + `ros2_interop_subscriber`
   c. `context_publisher` + `context_subscriber`
   d. `context_rpc_server` + `context_rpc_client`
   e. `logger`
   f. `timer`
   g. `coroutine` + `coroutine_loop`
   h. `executor_demo`
   i. `real_time`
   - Estimated: +1500 lines (3094 total).

### Phase D: Demo Migrations (Wave 2-3)

After ibmw-gen v2 is complete, migrate demos in this order:

**Wave 2 (simple extensions, 4 demos):**
1. `logging_demo` -- tests logging section
2. `config_param_demo` -- tests bare + parameter API
3. `dds_native_demo` -- tests custom_dds_types + DDS extension + QoS
4. `dds_image_demo` -- tests custom_dds_types + data_sharing QoS

**Wave 3 (advanced, 5 demos):**
5. `ros2_interop_demo` -- tests ros2_interop_publisher/subscriber
6. `ros2_point_demo` -- tests ros2_interop with geometry_msgs
7. `executor_demo` -- tests executor_demo + timer + coroutine + real_time
8. `context_demo` -- tests all context_* types
9. `loaned_message_demo` -- tests loaned_publisher (CRITICAL)

**Deferred (Wave 4 / manual):**
10. `extension_smoke_test` -- pure YAML/bash test, no module codegen needed
11. `engine_smoke_test` -- pure engine lifecycle test, no module codegen needed

---

## 5. Example: loaned_message_demo .ibmw.yaml

The following shows what the fully migrated loaned_message_demo would look
like as an ibmw-gen v2 YAML:

```yaml
project:
  name: loaned_message_demo
  namespace: loaned_message

extensions:
  - name: dds_extension
    path: ./libibmw_dds_transport.so
    options:
      domain_id: 0

executors:
  - name: simple_thread_executor
    type: dedicated
    options:
      thread_num: 4

logging:
  core_level: INFO
  backends:
    - type: console

dds_driver:
  ros2_compatible_mode: true
  ros2_topic_prefix: "rt"
  publisher_qos:
    history: keep_last
    depth: 10
    reliability: reliable
    durability: volatile
  subscriber_qos:
    history: keep_last
    depth: 10
    reliability: reliable
    durability: volatile

modules:
  - name: PlainPublisherModule
    type: loaned_publisher
    transport: dds
    topic: point_topic
    message_type: "geometry_msgs::msg::Point"
    ros2_package: geometry_msgs
    ros2_msg_include: "geometry_msgs/msg/point.hpp"
    is_plain_type: true
    ros2_type_name: "geometry_msgs::msg::dds_::Point_"

  - name: NonPlainPublisherModule
    type: loaned_publisher
    transport: dds
    topic: string_topic
    message_type: "std_msgs::msg::String"
    ros2_package: std_msgs
    ros2_msg_include: "std_msgs/msg/string.hpp"
    is_plain_type: false
    ros2_type_name: "std_msgs::msg::dds_::String_"

  - name: SubscriberModule
    type: ros2_interop_subscriber
    transport: dds
    topic: point_topic
    message_type: "geometry_msgs::msg::Point"
    ros2_package: geometry_msgs
    ros2_msg_include: "geometry_msgs/msg/point.hpp"
    ros2_type_name: "geometry_msgs::msg::dds_::Point_"
    # Also subscribes to string_topic -- for simplicity, one module handles both

apps:
  - name: loaned_message_app
    type: pubsub
    modules: [PlainPublisherModule, NonPlainPublisherModule, SubscriberModule]
    messaging_drivers: [dds]
```

---

## 6. Example: ros2_interop_demo .ibmw.yaml

```yaml
project:
  name: ros2_interop_demo
  namespace: ros2_interop

extensions:
  - name: dds_extension
    path: ./libibmw_dds_transport.so
    options:
      domain_id: 0

executors:
  - name: main_executor
    type: thread_pool
    options:
      thread_num: 4

logging:
  core_level: INFO
  backends:
    - type: console

dds_driver:
  ros2_compatible_mode: true
  ros2_topic_prefix: "rt"
  publisher_qos:
    history: keep_last
    depth: 10
    reliability: reliable
    durability: volatile
  subscriber_qos:
    history: keep_last
    depth: 10
    reliability: reliable
    durability: volatile

modules:
  - name: PublisherModule
    type: ros2_interop_publisher
    transport: dds
    topic: chatter
    message_type: "std_msgs::msg::String"
    ros2_package: std_msgs
    ros2_msg_include: "std_msgs/msg/string.hpp"
    ros2_type_name: "std_msgs::msg::dds_::String_"

  - name: SubscriberModule
    type: ros2_interop_subscriber
    transport: dds
    topic: chatter
    message_type: "std_msgs::msg::String"
    ros2_package: std_msgs
    ros2_msg_include: "std_msgs/msg/string.hpp"
    ros2_type_name: "std_msgs::msg::dds_::String_"

apps:
  - name: ros2_interop_app
    type: pubsub
    modules: [PublisherModule, SubscriberModule]
    messaging_drivers: [dds]
```

---

## 7. Example: dds_native_demo .ibmw.yaml (auto-generated custom DDS types)

This demonstrates the **auto-generated CDR types** feature. The `custom_dds_types`
section defines message types that ibmw-gen generates as complete `.h` files
with FastCDR serialization, replacing hand-written protocol headers.

```yaml
project:
  name: dds_native_demo
  namespace: dds_native_demo

custom_dds_types:
  namespace: "ibmw::examples::dds_native_demo"
  messages:
    - name: HelloMsg
      constructor_args: true
      fields:
        - { name: id, type: uint64, default: "0" }
        - { name: message, type: string }
        - { name: timestamp, type: int64, default: "0" }

extensions:
  - name: dds_extension
    path: ./libibmw_dds_transport.so
    options:
      domain_id: 0

executors:
  - name: work_executor
    type: thread_pool
    options:
      thread_num: 2

logging:
  core_level: INFO
  backends:
    - type: console

dds_driver:
  publisher_qos:
    history: keep_last
    depth: 10
    reliability: reliable
    durability: volatile
  subscriber_qos:
    history: keep_last
    depth: 10
    reliability: reliable
    durability: volatile

modules:
  - name: PublisherModule
    type: publisher
    transport: dds
    topic: hello_topic
    message_type: HelloMsg
    custom_dds_type: true             # indicates message_type is from custom_dds_types

  - name: SubscriberModule
    type: subscriber
    transport: dds
    topic: hello_topic
    message_type: HelloMsg
    custom_dds_type: true

apps:
  - name: dds_native_app
    type: pubsub
    modules: [PublisherModule, SubscriberModule]
    messaging_drivers: [dds]
```

ibmw-gen generates:
- `protocol/hello_msg.h` — struct + CDR serialization (auto-detected: **non-plain**
  due to `std::string` field)
- Module code that `#include "protocol/hello_msg.h"` and uses
  `ibmw::examples::dds_native_demo::HelloMsg`
- CMakeLists.txt with `find_package(fastcdr REQUIRED)` and proper include paths

---

## 8. Example: dds_image_demo .ibmw.yaml (enums + arrays + zero-copy)

This demonstrates auto-generated types with enums, constants, and fixed-size
arrays — producing a **plain type** suitable for DDS zero-copy.

```yaml
project:
  name: dds_image_demo
  namespace: dds_image_demo

custom_dds_types:
  namespace: "ibmw::examples::dds_image_demo"

  constants:
    - { name: kImageBufferSize, type: uint64, value: 1048576 }

  enums:
    - name: ImageEncoding
      underlying_type: uint32
      values:
        - { name: RGB8, value: 0 }
        - { name: RGBA8, value: 1 }
        - { name: GRAY8, value: 2 }
        - { name: GRAY16, value: 3 }

  messages:
    - name: ImageMsg
      fields:
        - { name: frame_id, type: uint64, default: "0" }
        - { name: timestamp, type: int64, default: "0" }
        - { name: width, type: uint32, default: "0" }
        - { name: height, type: uint32, default: "0" }
        - { name: encoding, type: ImageEncoding, default: "ImageEncoding::RGB8" }
        - { name: data, type: "array<uint8, 1048576>" }
        - { name: data_size, type: uint32, default: "0" }

extensions:
  - name: dds_extension
    path: ./libibmw_dds_transport.so
    options:
      domain_id: 0

dds_driver:
  publisher_qos:
    history: keep_last
    depth: 5
    reliability: best_effort
    durability: volatile
    data_sharing: auto
  subscriber_qos:
    history: keep_last
    depth: 5
    reliability: best_effort
    durability: volatile

modules:
  - name: ImagePublisher
    type: publisher
    transport: dds
    topic: image_topic
    message_type: ImageMsg
    custom_dds_type: true

  - name: ImageSubscriber
    type: subscriber
    transport: dds
    topic: image_topic
    message_type: ImageMsg
    custom_dds_type: true

apps:
  - name: dds_image_app
    type: pubsub
    modules: [ImagePublisher, ImageSubscriber]
    messaging_drivers: [dds]
```

ibmw-gen generates:
- `protocol/image_msg.h` — enum class + constants + struct + CDR serialization
  (auto-detected: **plain type** — all fields are fixed-size, suitable for zero-copy)
- `ImageMsg::kIsPlainType = true`

---

## 9. Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| loaned_message API internal headers | Medium | High | Use public API only; verify against installed headers |
| Whole-archive linking breaks on non-Linux | Low | Medium | Platform-specific CMake function |
| Context API changes between IBMW versions | Low | Medium | Pin to current API, document version |
| Generated code doesn't compile | Medium | High | E2E test each module type immediately |
| Schema backward compat break | Low | High | All new sections optional; v1 YAML unchanged |

---

## 10. Acceptance Criteria

For ibmw-gen v2 to be considered complete:

1. All v1 tests still pass (quickstart, transport_demo, ros2_chn, ros2_rpc).
2. Each new module type has at least one demo migration that:
   - Generates from `.ibmw.yaml`
   - Builds with CMake
   - Runs in App mode (exit 137/124/130/0)
   - Runs in Pkg mode (exit 137/124/130/0)
3. `loaned_message_demo` migration specifically demonstrates:
   - PlainPublisherModule uses LoanedMessage API
   - NonPlainPublisherModule shows fallback behavior
   - SubscriberModule receives messages from both publishers
4. All generated YAML configs match the structure of the original demo configs.
