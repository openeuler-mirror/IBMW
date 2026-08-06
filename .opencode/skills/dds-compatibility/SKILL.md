---
name: dds-compatibility
description: DDS compatibility, minimal SDK, or ROS2_MINIMAL_SDK diagnosis and IBMW configuration for host/OHOS.
---

# DDS compatibility

Use the canonical sibling CLI at `../IBMW_together/ros2_minimal_deps/scripts/ibmw_dds_compat.py`.
Always run `/usr/bin/python3 ../IBMW_together/ros2_minimal_deps/scripts/ibmw_dds_compat.py doctor ...`
first, then use `run` with exactly the same source, target, prefix, `--dds-only` scope, build directory, OHOS SDK, FetchContent base, `--allow-network` policy, and relevant environment resolution.
Default scope requires standard ROS2 messages; `--dds-only` is advanced substrate-only mode.
Offline failures require a complete prepopulated CMake FETCHCONTENT_BASE_DIR tree containing every required source layout, or explicit `--allow-network`. Require the exact cache phrase `CMakeCache prefix/tool/network identity validated`. Restart OpenCode after adding or changing skill files so the skill is reloaded.
