# Hello World 示例

最小 IBMW module，演示 module 生命周期：Setup / Run / Stop。同时展示通过 YAML
加载配置以及三种执行模式。

## 运行

```bash
cd build

# Pkg 模式 —— module 作为 .so 由 ibmw 加载
bash start_helloworld_pkg.sh
# 或：./ibmw run ./cfg/examples_cpp_helloworld_cfg.yaml

# App 模式 —— 独立可执行
bash start_helloworld_app.sh
# 或：./helloworld_app ./cfg/examples_cpp_helloworld_app_mode_cfg.yaml

# Registration 模式 —— 在代码里注册 module
./helloworld_app_registration_mode
```

## 期望输出

```
Setup succeeded.
Run succeeded.
(Ctrl+C)
Stop succeeded.
```

## 文件清单

| 文件 | 说明 |
|------|------|
| `managed/helloworld_module/helloworld_module.cc` | Module 实现 |
| `standalone/helloworld_app.cc` | App 模式入口 |
| `package/helloworld_pkg.cc` | Pkg 模式 package loader |
| `config/linux/bin/cfg/examples_cpp_helloworld_cfg.yaml` | Pkg 模式配置 |
| `config/linux/bin/cfg/examples_cpp_helloworld_app_mode_cfg.yaml` | App 模式配置 |

## 涉及概念

- `ibmw::CoreRef` —— 框架主 handle
- `core_.GetConfigurator().GetConfigFilePath()` —— 单 module 的配置访问
- `IBMW_INFO()` —— 日志宏
- Module 生命周期：`Setup(Runtime)` -> `Run()` -> `Stop()`

---

## ibmw-gen 方式

本 Demo 也可由 `ibmw-gen` 从
[`quickstart.ibmw.yaml`](./quickstart.ibmw.yaml) 定义生成。

### ibmw-gen 生成 vs 你自己写

| 自动生成 | 你来写 |
|----------|--------|
| `CMakeLists.txt` —— 构建系统 | `hello_world_module.cc` —— module 业务逻辑 |
| `cfg/helloworld_app.yaml` —— App 模式配置 | `hello_world_module.h` —— module 头文件 |
| `cfg/hello_world_module_pkg.yaml` —— Pkg 模式配置 | |
| `src/helloworld_app/main.cc` —— App 入口 | |
| `src/hello_world_module_pkg/pkg_main.cc` —— Pkg loader | |

module 的 `.h` / `.cc` 只在首次生成、带 TODO 标记。后续再跑时 ibmw-gen 会
**跳过**它们以保留你的修改。

### 生成、构建、运行

```bash
# 生成
python3 <ibmw-gen>/ibmw_gen.py quickstart.ibmw.yaml -o quickstart_gen

# 构建
mkdir -p quickstart_gen/build && cd quickstart_gen/build
cmake .. -DCMAKE_PREFIX_PATH=<ibmw_build_dir>
make -j$(nproc)

# 运行（App 模式）
./helloworld_app cfg/helloworld_app.yaml

# 运行（Pkg 模式）
ibmw run cfg/hello_world_module_pkg.yaml
```

### YAML 定义

```yaml
project:
  name: quickstart
  namespace: ibmw::examples::cpp::helloworld

modules:
  - name: HelloWorldModule
    type: bare                     # 不带 transport，仅生命周期

apps:
  - name: helloworld_app
    type: bare
    modules: [HelloWorldModule]
```

这是最简的 ibmw-gen 工程：一个 `bare` 类型 module + 三个生命周期方法
（`Setup` / `Run` / `Stop`）。
