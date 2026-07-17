Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# SDK Layout and Prefix Roles

Expected IBMW SDK install prefix:

```text
<sdk>/
  bin/                    # ibmw runner and installed generator helper scripts
  cmake/                  # IbmwHelpers.cmake, toolchain/helper modules
  include/                # public headers
  lib/                    # target libraries
  lib/cmake/ibmw/         # ibmw-config.cmake and ibmw-targets.cmake
  share/                  # optional package metadata when installed by profile
```

Prefix roles:

| Prefix | Role |
|--------|------|
| IBMW SDK prefix | `find_package(ibmw)` and IBMW runtime headers/libs |
| Target third-party prefix | target FastDDS/Fast-CDR/asio/yaml-cpp/TBB/etc. if not co-located |
| Host tools prefix | tools that run on build host/container, not target runtime |
| ROS2 target prefix | rosidl/types/FastDDS provider for Route B when ROS2 target deps are used |

Rule: customer `CMAKE_PREFIX_PATH` must include every target dependency prefix
needed by `ibmw-config.cmake` `find_dependency()` calls.
