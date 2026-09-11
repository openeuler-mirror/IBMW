# IBMW external P0 adapter

`collect_ibmw(root, ...)` is the stable minimum adapter interface for the
public collector. It performs read-only discovery and returns:

* `manifest`: source root, version (version file, generated header, or git
  commit), CMake executable/configuration, and IBMW options;
* `topology`: a dependency-free summary of a supplied Transport config and an
  explicit external-only runtime manifest;
* `capability`: optional Core Metrics capability data and explicit
  `unsupported` degradation when no capability file is available.

Arguments are `build_dir`, `cmake_executable`, `transport_config`, and
`core_capability`. The adapter does not build, start, or modify IBMW.

The interface is intentionally usable before the common collector is complete;
the collector can later wrap the returned envelope without changing the
adapter contract.
