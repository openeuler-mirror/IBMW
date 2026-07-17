# Hello World Example

Minimal IBMW module demonstrating the module lifecycle: Setup, Run, Stop. Shows
configuration loading via YAML and the three execution modes.

## Run

```bash
cd build

# Pkg mode -- module loaded as .so by ibmw
bash start_helloworld_pkg.sh
# or: ./ibmw run ./cfg/examples_cpp_helloworld_cfg.yaml

# App mode -- standalone executable
bash start_helloworld_app.sh
# or: ./helloworld_app ./cfg/examples_cpp_helloworld_app_mode_cfg.yaml

# Registration mode -- modules registered in code
./helloworld_app_registration_mode
```

## Expected Output

```
Setup succeeded.
Run succeeded.
(Ctrl+C)
Stop succeeded.
```

## Files

| File | Description |
|------|-------------|
| `managed/helloworld_module/helloworld_module.cc` | Module implementation |
| `standalone/helloworld_app.cc` | App-mode entry point |
| `package/helloworld_pkg.cc` | Pkg-mode package loader |
| `config/linux/bin/cfg/examples_cpp_helloworld_cfg.yaml` | Pkg-mode config |
| `config/linux/bin/cfg/examples_cpp_helloworld_app_mode_cfg.yaml` | App-mode config |

## Concepts Demonstrated

- `ibmw::CoreRef` -- main framework handle
- `core_.GetConfigurator().GetConfigFilePath()` -- per-module config access
- `IBMW_INFO()` -- logging macro
- Module lifecycle: `Setup(Runtime)` -> `Run()` -> `Stop()`

---

## ibmw-gen Way

This demo can also be generated using `ibmw-gen` from the
[`quickstart.ibmw.yaml`](./quickstart.ibmw.yaml) definition.

### What ibmw-gen generates vs what you write

| Generated (auto) | You write |
|-------------------|-----------|
| `CMakeLists.txt` -- build system | `hello_world_module.cc` -- module business logic |
| `cfg/helloworld_app.yaml` -- App-mode config | `hello_world_module.h` -- module header |
| `cfg/hello_world_module_pkg.yaml` -- Pkg-mode config | |
| `src/helloworld_app/main.cc` -- App entry point | |
| `src/hello_world_module_pkg/pkg_main.cc` -- Pkg loader | |

The module `.h`/`.cc` files are generated once with TODO markers. On subsequent
runs, ibmw-gen **skips** them to preserve your edits.

### Generate, build, and run

```bash
# Generate
python3 <ibmw-gen>/ibmw_gen.py quickstart.ibmw.yaml -o quickstart_gen

# Build
mkdir -p quickstart_gen/build && cd quickstart_gen/build
cmake .. -DCMAKE_PREFIX_PATH=<ibmw_build_dir>
make -j$(nproc)

# Run (App mode)
./helloworld_app cfg/helloworld_app.yaml

# Run (Pkg mode)
ibmw run cfg/hello_world_module_pkg.yaml
```

### YAML definition

```yaml
project:
  name: quickstart
  namespace: ibmw::examples::cpp::helloworld

modules:
  - name: HelloWorldModule
    type: bare                     # No transport, pure lifecycle

apps:
  - name: helloworld_app
    type: bare
    modules: [HelloWorldModule]
```

This is the simplest possible ibmw-gen project: one `bare` module with the
three lifecycle methods (`Setup`, `Run`, `Stop`).
