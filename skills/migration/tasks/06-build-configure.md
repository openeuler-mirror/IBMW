Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Task 06 — Build / Configure

No heavy builds are required by this manual. Use the lightest project-specific
configure/build needed to prove generated code compiles.

Record in `reports/build_configure.md`:

- install/prefix used
- generator command
- configure command
- build command
- result and log path

Suggested evidence commands:

```bash
python3 src/tools/ibmw_gen/ibmw_gen.py <app.ibmw.yaml> \
  --output-dir <generated_project> --force

set +u
source /opt/ros/humble/setup.bash
set -u

cmake -S <generated_project> -B <generated_project>/build \
  -DCMAKE_PREFIX_PATH=<ibmw_install>\;/opt/ros/humble
cmake --build <generated_project>/build --target <app_target> -j2
```

If skipped, write the reason and exact next command to run.
