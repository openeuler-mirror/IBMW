# Changelog

All notable public changes to IBMW are documented here. The project follows
semantic versioning for published version numbers where practical; compatibility
details remain governed by the documentation for each release.

## [Unreleased]

### Changed

- Zenoh: leading and trailing `/` in `limit_domain` are ignored, and an empty
  `limit_domain` adds no key suffix. These values previously produced invalid
  key expressions (`//`, trailing `/`) and failed registration.
- MQTT RPC: servers only answer on topics starting with `ibmw_rpc_rsp/`;
  requests carrying any other answer topic are dropped with a rate-limited
  warning. Clients only accept an answer from the called function's answer
  topic, and request ids start at a random value. The wire format is unchanged.
- Time manipulator: `SetTimeRatio` rejects NaN, infinite and out-of-range ratios
  (new `max_ratio` option, default 1000) and rounds valid ratios to the nearest
  integer step (N or 1/N). Rejected RPC calls return code 2 /
  `INVALID_TIME_RATIO`; an invalid `init_ratio` fails initialization.
- gRPC server: the default `max_connection_num` is now 1024 (was 1000000,
  effectively `SOMAXCONN`); `max_connection_num` and `max_concurrent_streams`
  are configurable.
- TCP transport: `max_send_queue_size` now bounds queued plus in-flight bytes
  per session; previously a slow peer could hold about twice the limit.
- C++ action API: `ActionLog` is `noexcept` and swallows exceptions from the
  default stdout/stderr sink. The sink is per shared library.
- HTTP RPC: CORS responses always carry `Vary: Origin` when the allow list is
  not `*`, including requests without an `Origin` header.
- HTTP RPC: failed calls are still answered with HTTP 500 and now carry the
  RPC status in an `ibmw-rpc_status` header. Clients report that status (for
  example the service's own code or `TIMEOUT`) instead of always
  `SVR_NOT_FOUND`; without the header the HTTP status is mapped (404 not found,
  408/504 timeout, 500 handle failed, 501 not implemented, 503 not ready).
- Transport and RPC contexts: the arrays returned by `get_meta_keys` and
  `get_meta_key_vals` are owned by the context and stay valid until its meta
  data changes; reading no longer rewrites them, so concurrent readers are
  safe and one call no longer invalidates the other's result. Entries are in
  insertion order. The C ABI is unchanged.
- ROS 2 JSON deserialization: resizable arrays of one message may claim at
  most 256 MiB of decoded storage (`kMaxTextArrayBytes`); larger input is
  rejected instead of allocated.
- C++ `ibmw::Runtime`: an incompatible or malformed core now throws
  `std::invalid_argument` instead of aborting the process. `ibmw::Module` reports
  it as a failed setup; code constructing `Runtime` directly must handle it.
- Discovery: a plugin found through `IBMW_DISCOVERY_PLUGIN_DIR` is only loaded
  when its directory and library belong to the current user or root and are
  not world writable; setuid processes ignore the variable.
- Timer executor: with `use_system_clock: false` (the default) ticks still
  follow the steady clock, but `Now()` and `DispatchAt()` now use system time
  anchored at start, like the thread pool executor. Previously `Now()` returned
  a steady clock value, so targets computed from `system_clock::now()` never ran.
- gRPC: a request or response above 4 GiB - 1 is refused (client
  `CLI_SERIALIZATION_FAILED`, server `grpc-status` 13) instead of being sent
  with a truncated length prefix.
- Recorder: a signal record now also ends at its stop time while its topics are
  silent, checked every `msg_write_interval_time` (default 1 s). Previously only
  the next message ended it.
- Recorder: `storage_policy.max_bag_size_m: 0` disables bag splitting;
  previously it started a new file for every record. When a bag file can not
  be opened, records are dropped and the open is retried every second, like a
  full disk; previously every record logged a failed write until the next
  split.
- Playback: a signal playback can start again after `StopSignalPlayback` or
  after its end (previously every later start reported "Playback is already
  running"). A stop now also drops the records already queued for publishing;
  previously up to two batches (about 1 s) were still published.
- Bag file compression (`compression_mode: file`): the compressed file is
  written to a new exclusive temporary file and synced, with its directory,
  before the uncompressed file is removed. A link planted at the old fixed
  `<file>.tmp` name is no longer followed.
- `ibmw bag record`: a topic whose Schema conflicts with the one already
  recorded for its type and serialization (for example during a rolling
  upgrade) is skipped and its messages are counted as dropped, with a warning
  at the end. Previously the writer stopped and no topic was recorded any more.
- `ibmw bag record --snapshot`: SIGUSR1 waits at most 5 s for the snapshot and
  then reports "Snapshot is still being written."; the snapshot is still
  completed later. Previously a stuck storage kept the command from stopping.
  C++ `RecorderCoordinator::TriggerSnapshot` takes a timeout and returns false
  when it expires.
- Runtime playback server: at most 64 stream sessions may be active (not yet
  ended) at once; further `StartSession` requests fail. Ended sessions no
  longer count and their threads are released.
- HTTP server: when a registered handler throws, the 500 response body reads
  `An error occurred: 'internal error'`; the exception text is only logged.
  Previously it was sent to the client.
- Net extension: `listen_ip` of the HTTP, TCP and UDP listeners may be an IPv6
  address, and the UDP client reaches IPv6 servers (previously only IPv4 was
  accepted and an IPv6 `listen_ip` failed startup).
- TCP/UDP client pools and the gRPC client pool identify a client by its full
  address. Previously two addresses could share a client, and messages for one
  server went to the other.
- TCP server: a zero-length frame is taken as the client's heartbeat and no
  longer reaches the message handler. Invalid frames and undecodable messages
  from a peer log at most once per second for each kind of problem; an invalid
  head or an oversized message is now a warning instead of an error.
- Documentation: `listen_port` of the net extension's HTTP, TCP and UDP sections
  is required when the section is set; it was wrongly listed with default `0`.
  Missing `thread_num` or `listen_port` now names the option in the error.
- DDS RPC client: a reply is matched to its call by the full 64-bit request id;
  a reply whose high word is not 0 is ignored. Previously only the low 32 bits
  were compared.
- MQTT transport: a message without a context is published (previously the
  send failed with "Reference is null"). `max_pkg_size_k` above 2097151
  (2 GiB - 1) fails initialization instead of wrapping the limit; its
  documented default is corrected to 1024. Failed unsubscribes are logged.
- UDP transport: a send type registered after `Start` is refused, like on TCP;
  previously it changed the publisher map while messages were being sent.
- In-process RPC across packages: a request the service can not deserialize
  now fails with `SVR_DESERIALIZATION_FAILED` and a response the client can
  not deserialize with `CLI_DESERIALIZATION_FAILED`, like the other drivers;
  the two codes were swapped.
- Thread scheduling options (`thread_sched_policy`,
  `listener_thread_sched_policy` and the like): the priority after
  `SCHED_FIFO:` or `SCHED_RR:` must be a plain decimal number; values such as
  `5x`, `+5` or ` 5` are rejected instead of being read as far as they look
  numeric.
- Local transport driver: `subscriber_use_inline_executor` and
  `subscriber_executor` are optional, as documented; previously a missing key
  failed initialization. The documented default of
  `subscriber_use_inline_executor` is corrected to `true`. Topic commands
  reading `--config-file` skip extension and driver items without `name` or
  `type` instead of failing.
- RPC client calls that a driver drops without a result now complete with
  `IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR` when the call is released, and
  the call's frame is freed. Previously the frame kept itself alive and the
  caller waited forever (COMM-014).
- A publisher that any transport driver refuses is now disabled on all drivers
  and gets no metrics. `RegisterSendType` still returns false, and `Send` on
  that publisher warns and sends nothing. Previously the drivers that accepted
  it kept publishing (COMM-022).
- `PublishLoaned` and `ReleaseLoaned` now drop a handle whose `source_driver`
  is not registered with the dispatcher, log an error, and do not call that
  driver. Previously the pointer was dereferenced without any check. A driver
  must set `source_driver` to itself (COMM-002).
- Extension ABI 3: extensions export `IbmwExtEntryV3`, and both
  `IBMW_EXT_ENTRY` and `IBMW_EXT_ENTRY_V2` now define it. The loader no longer
  falls back to `IbmwExtEntryV2` or the unversioned `IbmwExtEntry`; a library
  that exports only one of them is rejected with a request to rebuild, and its
  create function is never called. **All extensions must be rebuilt.**
- Core ABI version is now 3 (API-024). `ibmw_core_base_t` gains `struct_size`
  after `abi_version`, and every handle table it exposes (configurator,
  executor bus/executor, logger, allocator, service, parameter, transport
  handle/sender/receiver) starts with `size_t struct_size`. `Runtime` rejects a
  core or sub-handle table smaller than the SDK's layout; `GetPublisher`,
  `GetSubscriber` and `GetExecutor` return an empty handle for an undersized
  table. Checks run only when a handle is obtained. **All modules and
  extensions must be rebuilt.**
- Module packages now export `IbmwPkgEntryV2`, returning
  `ibmw_pkg_entry_v2_t` that starts with `abi_version`
  (`IBMW_PKG_ABI_VERSION_V2`) and `struct_size` (CORE-021). The loader rejects
  another version or a short entry before calling any callback, and a library
  exporting only the old `IbmwPkgEntry` is rejected with a request to rebuild.
  `IBMW_PKG_MAIN` emits the new entry. **All module packages must be rebuilt.**
- `ibmw_module_base_t` gains an optional `set_driver_slot(ctx, slot_id,
  driver_ptr)` before `ctx`, filled by `ibmw::Module` (CORE-024). Directly
  registered modules now receive driver slots only through it; the engine no
  longer casts their `ctx` to `ibmw::Module`, so a C module without the
  callback is skipped instead of crashing.
- Transport and RPC context ops tables gain a required `try_set_used`, and
  `Context` keeps its used flag in a `std::atomic<bool>` (COMM-020). Publish,
  async publish and RPC call claim a caller's context with one atomic
  exchange, so two threads can no longer both send with the same context. A
  context table without `try_set_used` is rejected as already used. Publishing
  with a caller's context costs about +3.5 ns (measured, ~2%); publishing
  without one is unchanged.
- Type-support package ABI is now 2 (`IBMW_TYPE_SUPPORT_PACKAGE_ABI_VERSION_V2`)
  and `ibmw_type_support_base_t` gains `msg_min_size` before `ctx`. Loaders
  reject version-1 packages with "older ABI ... rebuild"; every type-support
  package must be rebuilt. The cached loaned capture now drops (and releases)
  a loaned payload that is null, smaller than `msg_min_size`, or whose type
  reports `msg_min_size == 0`, logging one warning per subscription. The
  built-in POD, JSON, ROS 2 and DDS type supports and `ibmw_gen` output fill
  it with `sizeof(T)`; hand-written tables must set it to keep loaned capture.
- `ibmw::rpc::Status::operator bool` is now `explicit`: `if (status)` still
  works, but implicit use as an integer or in `bool b = status;` no longer
  compiles.
- `TransportRef::GetPublisher`/`GetSubscriber` return an empty reference when
  the provider's table lacks an entry the C++ wrappers call unconditionally
  (sender: `register_send_type`, `send`, `get_topic`,
  `merge_subscribe_context_to_publish_context`, `loan`, `publish_loaned`,
  `release_loaned`; receiver: `on_message`, `get_topic`).
- Sender C ABI entries now reject a null `ctx`, and exceptions they swallow are counted
  (`SenderAccess::AbiExceptionCount`) and logged as errors at most once per second per sender
  (COMM-039).
- The console logger's `Flush()` now waits (up to 1 s) for the entries already queued on its
  log executor, and the engine no longer sleeps a fixed 50 ms after flushing at shutdown (CORE-015).
- Opening a SQLite bag counts messages with one `GROUP BY` pass when the file
  has more than three channels instead of one full table scan per channel, an
  invalid `qos_json` now fails with an `IbmwException` naming the channel, and
  the reader configuration accepts `integrity_check: false` to skip
  `PRAGMA quick_check` on trusted files (default stays `true`) (EXT-036).

### Added
- `AsyncAccessBase`, `FutureAccessBase` and the generated `AsyncAccess` /
  `FutureAccess` classes (ROS2 `.srv` and DDS IDL generators) accept
  `std::shared_ptr` request and response objects and keep them alive until the
  call completes. The reference overloads are unchanged (API-012).

- Recorder `storage_policy` disk limits `min_free_space_m`, `max_folder_num`
  and `max_total_size_m`; all default to 0 (disabled).
- Recorder option `max_preparation_cache_m` (default 0, disabled) bounds the
  memory of records cached before a signal record starts; the oldest are
  dropped first.
- CMake option `IBMW_ENABLE_HARDENING` (default `OFF`) adds stack protector,
  `_FORTIFY_SOURCE=2`, full RELRO, noexecstack and PIE executables.
- CMake options `IBMW_ENABLE_ASAN`, `IBMW_ENABLE_TSAN` and `IBMW_ENABLE_UBSAN`
  (default `OFF`) build with the sanitizer; ASan and TSan can not be combined.
  ASan/TSan builds turn off `IBMW_ENABLE_DLOPEN_DEEPBIND`, and all sanitizer
  builds skip coverage. Presets `asan` (ASan + UBSan) and `tsan` use
  `build-asan` and `build-tsan`.

### Documentation

- Reorganized public documentation into guides, reference, contributing,
  ecosystem, experimental, and release sections.
- Added public security, support, conduct, changelog, and release documents.
- Added human-facing indexes for the existing migration and SDK skill assets.

## [0.1.0]

Initial public version identified by the repository `VERSION` file. The public
surface includes the modular runtime, C and C++ APIs, extension and package
interfaces, examples, `ibmw-template`, `ibmw-gen`, ROS2/DDS interoperability
guidance, and scoped cross-compilation documentation.

See [the 0.1.0 release notes](docs/releases/0.1.0.md) for usage and support
boundaries.
