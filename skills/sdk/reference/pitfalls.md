Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# SDK Pitfalls

- Using the IBMW build tree as the customer SDK. Fix: install to a clean prefix.
- Using a host SDK for OHOS cross builds. Fix: cross-compile IBMW and install the
  target SDK prefix first.
- Dual install-tree include paths can expose the same public header as both
  `c/...` and `api/c/...`. Required fix: de-duplicate the install layout so
  `<sdk>/include/ibmw/api` is absent, unless a reviewed compatibility exception
  is documented. Path-independent include guards are only a safety net.
- Treating the minimal OHOS SDK as a DDS/ROS2 SDK. Fix: use the profile matrix;
  minimal OHOS has no DDS/ROS2 transport `.so` and supports only minimal/local
  generated modules.
- Treating a full-flag SDK as automatically suitable for every app. A full
  development SDK is only correct when CMake exports are componentized and each
  consumer links/deploys only requested components. Until that is proven, build
  IBMW/SDK per capability set and audit the app's actual `DT_NEEDED` closure.
- Shipping one universal SDK before OHOS cross profiles are separated. Fix:
  build the OHOS SDK with the matching target sysroot, target ROS2/DDS provider,
  and feature flags required by that product slice.
- Forgetting target third-party prefixes. Fix: co-locate them or add them to
  customer `CMAKE_PREFIX_PATH`.
- Letting customer builds fetch `nlohmann_json` or other deps from the network in
  Route B/offline containers. Fix: vendor or preinstall dependencies.
- Mixing ROS2 Fast-CDR/FastDDS with bundled/newer Fast-CDR. Fix: prefer the
  Route B target ROS2 provider and record reviewed exceptions.
- Treating cross configure/build as board runtime validation. Fix: label board
  runtime separately and require device logs.
- Running inside a container and losing path provenance. Fix: record container
  image/name, mounts, SDK path, sysroot, target ROS2 prefix, and build dir.
