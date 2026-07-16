Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Executor Conformance Test

Self-checking standalone sample that validates the core executor contracts used
by IBMW runtime code.

The app creates temporary YAML configuration files under `${TMPDIR:-/tmp}` (falling
back to `/tmp`) and runs positive plus fail-fast cases in-process. It does not
require DDS, ROS 2, or privileged real-time permissions.

It covers the five built-in executor types (`thread_pool`, `strand`, `dedicated`,
`parallel`, `timer`). When `libibmw_time_manipulator.so` is available, it also
checks the extension-registered `time_manipulator` executor type.

```bash
./executor_conformance_app
```

The process exits non-zero if any assertion fails.
