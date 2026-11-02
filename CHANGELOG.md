# Changelog

All notable public changes to IBMW are documented here. The project follows
semantic versioning for published version numbers where practical; compatibility
details remain governed by the documentation for each release.

## [Unreleased]

### Changed

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

### Added

- Recorder `storage_policy` disk limits `min_free_space_m`, `max_folder_num`
  and `max_total_size_m`; all default to 0 (disabled).
- CMake option `IBMW_ENABLE_HARDENING` (default `OFF`) adds stack protector,
  `_FORTIFY_SOURCE=2`, full RELRO, noexecstack and PIE executables.

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
