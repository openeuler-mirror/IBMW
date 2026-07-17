# Parameter 示例

演示 IBMW 的 parameter API：用于跨 module 通信和动态配置的 runtime 参数 set/get。

## 运行

```bash
cd build
bash start_examples_cpp_parameter.sh
```

## 期望输出

```
Set parameter 'key1' = 'value1'
Get parameter 'key1' = 'value1'
Set parameter 'key2' = 'value2'
Get parameter 'key2' = 'value2'
...
```

## 配置

| 配置文件 | 说明 |
|----------|------|
| `examples_cpp_parameter_cfg.yaml` | 基础 parameter module |

## 涉及概念

- `core_.GetParameterHandle()` —— 获取 parameter handle
- `SetParameter(key, value)` —— 设置参数
- `GetParameter(key)` —— 读取参数值
- 用于运行期配置的 parameter get/set 周期

---

## ibmw-gen 方式

本 Demo 也可由 `ibmw-gen` 从
[`config_param_demo.ibmw.yaml`](./config_param_demo.ibmw.yaml) 定义生成。

### ibmw-gen 生成 vs 你自己写

| 自动生成 | 你来写 |
|----------|--------|
| `CMakeLists.txt` —— 构建系统 | `parameter_module.cc` —— parameter get/set 逻辑 |
| `cfg/config_param_app.yaml` —— 含 executor 的 App 配置 | `parameter_module.h` —— module 头文件 |
| `cfg/parameter_module_pkg.yaml` —— Pkg 模式配置 | |
| `src/config_param_app/main.cc`、`src/parameter_module_pkg/pkg_main.cc` | |

### 关键 YAML 字段

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

这是一个最小 ibmw-gen 工程，含一个 `bare` 类型 module 和一个工程级 executor。
parameter API（`SetParameter` / `GetParameter`）在用户编写的 module 代码内调用 ——
ibmw-gen 生成的是带生命周期 hook 的脚手架，你在里面填入 parameter 逻辑。
