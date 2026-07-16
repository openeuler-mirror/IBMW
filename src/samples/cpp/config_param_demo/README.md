# Parameter Example

Demonstrates the IBMW parameter API: setting and getting runtime parameters
for inter-module communication and dynamic configuration.

## Run

```bash
cd build
bash start_examples_cpp_parameter.sh
```

## Expected Output

```
Set parameter 'key1' = 'value1'
Get parameter 'key1' = 'value1'
Set parameter 'key2' = 'value2'
Get parameter 'key2' = 'value2'
...
```

## Configuration

| Config | Description |
|--------|-------------|
| `examples_cpp_parameter_cfg.yaml` | Basic parameter module |

## Concepts Demonstrated

- `core_.GetParameterHandle()` -- get parameter handle
- `SetParameter(key, value)` -- set a parameter
- `GetParameter(key)` -- retrieve a parameter value
- Parameter get/set cycles for runtime configuration

---

## ibmw-gen Way

This demo can also be generated using `ibmw-gen` from the
[`config_param_demo.ibmw.yaml`](./config_param_demo.ibmw.yaml) definition.

### What ibmw-gen generates vs what you write

| Generated (auto) | You write |
|-------------------|-----------|
| `CMakeLists.txt` -- build system | `parameter_module.cc` -- parameter get/set logic |
| `cfg/config_param_app.yaml` -- App config with executor | `parameter_module.h` -- module header |
| `cfg/parameter_module_pkg.yaml` -- Pkg-mode config | |
| `src/config_param_app/main.cc`, `src/parameter_module_pkg/pkg_main.cc` | |

### Key YAML features

```yaml
executors:
  - name: work_thread_pool
    type: thread_pool
    options:
      thread_num: 2

modules:
  - name: ParameterModule
    type: bare
    log_level: INFO
```

This is a minimal ibmw-gen project with a `bare` module and a project-level
executor. The parameter API (`SetParameter`/`GetParameter`) is called from within
the user-written module code -- ibmw-gen generates the scaffold with lifecycle
hooks where you add the parameter logic.
