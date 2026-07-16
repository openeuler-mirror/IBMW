# ROS2 RPC Examples


## ROS2 RPC Sync


An RPC example using the ROS2 srv protocol with synchronous interfaces and a local backend. This demo covers:
- How to use the ROS2 srv protocol as the RPC service protocol;
- How to use Executor, synchronous RPC client, and server interfaces in Module mode;
- How to use a local RPC backend;
- How to integrate Modules in Pkg mode and launch them;


Core code:
- [example_ros2/srv/RosTestRpc.srv](../../../schema/ros2/example_ros2/srv/RosTestRpc.srv)
- [normal_rpc_sync_client_module.cc](./managed/normal_rpc_sync_client_module/normal_rpc_sync_client_module.cc)
- [normal_rpc_sync_server_module.cc](./managed/normal_rpc_sync_server_module/normal_rpc_sync_server_module.cc)
- [service.cc](./managed/normal_rpc_sync_server_module/service.cc)
- [ros2_rpc_client_pkg/pkg_main.cc](./package/ros2_rpc_client_pkg/pkg_main.cc)
- [ros2_rpc_server_pkg/pkg_main.cc](./package/ros2_rpc_server_pkg/pkg_main.cc)


Configuration file:
- [examples_cpp_ros2_rpc_sync_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_rpc_sync_cfg.yaml)



How to run (Linux):
- Build IBMW with `IBMW_BUILD_EXAMPLES` and `IBMW_BUILD_WITH_ROS2` enabled;
- Run the `start_examples_cpp_ros2_rpc_sync.sh` script in the build directory to start the process;
- Press `ctrl-c` to stop the process;


Notes:
- This example creates the following two modules:
  - `NormalRpcSyncClientModule`: Uses the `work_thread_pool` executor to send RPC requests to `RosTestRpc` at the configured frequency via the synchronous Client interface;
  - `NormalRpcSyncServerModule`: Registers the `RosTestRpc` service and provides echo functionality via the synchronous Server interface;
- This example integrates `NormalRpcSyncClientModule` and `NormalRpcSyncServerModule` into two separate packages `ros2_rpc_client_pkg` and `ros2_rpc_server_pkg`, and loads both into a single IBMW process via the configuration file;
- This example uses a local RPC backend for communication and configures a `timeout_handle` executor as the timeout executor;

Expected output (all sub-demos produce similar patterns):
```
[INFO] RPC call succeeded, response: <echo of request>
[INFO] RPC call succeeded, response: <echo of request>
...
```
The server logs each incoming request, and the client logs each successful response. The exact log format varies by sub-demo (Sync/Async/Future/Co) but all demonstrate successful RPC round-trips.



## ROS2 RPC Async


An RPC example using the ROS2 srv protocol with asynchronous interfaces and a local backend. This demo covers:
- How to use the ROS2 srv protocol as the RPC service protocol;
- How to use Executor, asynchronous RPC client, and server interfaces in Module mode;
- How to use a local RPC backend;
- How to integrate Modules in Pkg mode and launch them;



Core code:
- [example_ros2/srv/RosTestRpc.srv](../../../schema/ros2/example_ros2/srv/RosTestRpc.srv)
- [normal_rpc_async_client_module.cc](./managed/normal_rpc_async_client_module/normal_rpc_async_client_module.cc)
- [normal_rpc_async_server_module.cc](./managed/normal_rpc_async_server_module/normal_rpc_async_server_module.cc)
- [service.cc](./managed/normal_rpc_async_server_module/service.cc)
- [ros2_rpc_client_pkg/pkg_main.cc](./package/ros2_rpc_client_pkg/pkg_main.cc)
- [ros2_rpc_server_pkg/pkg_main.cc](./package/ros2_rpc_server_pkg/pkg_main.cc)


Configuration file:
- [examples_cpp_ros2_rpc_async_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_rpc_async_cfg.yaml)



How to run (Linux):
- Build IBMW with `IBMW_BUILD_EXAMPLES` and `IBMW_BUILD_WITH_ROS2` enabled;
- Run the `start_examples_cpp_ros2_rpc_async.sh` script in the build directory to start the process;
- Press `ctrl-c` to stop the process;


Notes:
- This example creates the following two modules:
  - `NormalRpcAsyncClientModule`: Uses the `work_thread_pool` executor to send RPC requests to `RosTestRpc` at the configured frequency via the asynchronous Client interface;
  - `NormalRpcAsyncServerModule`: Registers the `RosTestRpc` service and provides echo functionality via the asynchronous Server interface;
- This example integrates `NormalRpcAsyncClientModule` and `NormalRpcAsyncServerModule` into two separate packages `ros2_rpc_client_pkg` and `ros2_rpc_server_pkg`, and loads both into a single IBMW process via the configuration file;
- This example uses a local RPC backend for communication and configures a `timeout_handle` executor as the timeout executor;



## ROS2 RPC Future

An RPC example using the ROS2 srv protocol with future-based interfaces and a local backend. This demo covers:
- How to use the ROS2 srv protocol as the RPC service protocol;
- How to use Executor, future-based RPC client interface, and synchronous RPC server interface in Module mode;
- How to use a local RPC backend;
- How to integrate Modules in Pkg mode and launch them;



Core code:
- [example_ros2/srv/RosTestRpc.srv](../../../schema/ros2/example_ros2/srv/RosTestRpc.srv)
- [normal_rpc_future_client_module.cc](./managed/normal_rpc_future_client_module/normal_rpc_future_client_module.cc)
- [normal_rpc_sync_server_module.cc](./managed/normal_rpc_sync_server_module/normal_rpc_sync_server_module.cc)
- [service.cc](./managed/normal_rpc_sync_server_module/service.cc)
- [ros2_rpc_client_pkg/pkg_main.cc](./package/ros2_rpc_client_pkg/pkg_main.cc)
- [ros2_rpc_server_pkg/pkg_main.cc](./package/ros2_rpc_server_pkg/pkg_main.cc)


Configuration file:
- [examples_cpp_ros2_rpc_future_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_rpc_future_cfg.yaml)



How to run (Linux):
- Build IBMW with `IBMW_BUILD_EXAMPLES` and `IBMW_BUILD_WITH_ROS2` enabled;
- Run the `start_examples_cpp_ros2_rpc_future.sh` script in the build directory to start the process;
- Press `ctrl-c` to stop the process;


Notes:
- This example creates the following two modules:
  - `NormalRpcFutureClientModule`: Uses the `work_thread_pool` executor to send RPC requests to `RosTestRpc` at the configured frequency via the future-based Client interface;
  - `NormalRpcSyncServerModule`: Registers the `RosTestRpc` service and provides echo functionality via the synchronous Server interface;
- This example integrates `NormalRpcFutureClientModule` and `NormalRpcSyncServerModule` into two separate packages `ros2_rpc_client_pkg` and `ros2_rpc_server_pkg`, and loads both into a single IBMW process via the configuration file;
- This example uses a local RPC backend for communication and configures a `timeout_handle` executor as the timeout executor;



## ROS2 RPC Co (Coroutine)


An RPC example using the ROS2 srv protocol with coroutine-based interfaces and a local backend. This demo covers:
- How to use the ROS2 srv protocol as the RPC service protocol;
- How to use Executor, coroutine-based RPC client, and server interfaces in Module mode;
- How to use coroutine-based RPC filter functionality;
- How to use a local RPC backend;
- How to integrate Modules in Pkg mode and launch them;



Core code:
- [example_ros2/srv/RosTestRpc.srv](../../../schema/ros2/example_ros2/srv/RosTestRpc.srv)
- [normal_rpc_co_client_module.cc](./managed/normal_rpc_co_client_module/normal_rpc_co_client_module.cc)
- [normal_rpc_co_server_module.cc](./managed/normal_rpc_co_server_module/normal_rpc_co_server_module.cc)
- [service.cc](./managed/normal_rpc_co_server_module/service.cc)
- [ros2_rpc_client_pkg/pkg_main.cc](./package/ros2_rpc_client_pkg/pkg_main.cc)
- [ros2_rpc_server_pkg/pkg_main.cc](./package/ros2_rpc_server_pkg/pkg_main.cc)


Configuration file:
- [examples_cpp_ros2_rpc_co_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_rpc_co_cfg.yaml)



How to run (Linux):
- Build IBMW with `IBMW_BUILD_EXAMPLES` and `IBMW_BUILD_WITH_ROS2` enabled;
- Run the `start_examples_cpp_ros2_rpc_co.sh` script in the build directory to start the process;
- Press `ctrl-c` to stop the process;


Notes:
- This example creates the following two modules:
  - `NormalRpcCoClientModule`: Uses the `work_thread_pool` executor to send RPC requests to `RosTestRpc` at the configured frequency via the coroutine Client interface;
  - `NormalRpcCoServerModule`: Registers the `RosTestRpc` service and provides echo functionality via the coroutine Server interface;
- This example registers two Filters on both the RPC Client and Server sides for logging requests and measuring latency;
- This example integrates `NormalRpcCoClientModule` and `NormalRpcCoServerModule` into two separate packages `ros2_rpc_client_pkg` and `ros2_rpc_server_pkg`, and loads both into a single IBMW process via the configuration file;
- This example uses a local RPC backend for communication and configures a `timeout_handle` executor as the timeout executor;



## ROS2 RPC Single Pkg


An RPC example using the ROS2 srv protocol with coroutine-based interfaces and a local backend. This demo covers:
- How to use the ROS2 srv protocol as the RPC service protocol;
- How to use Executor, coroutine-based RPC client, and server interfaces in Module mode;
- How to use coroutine-based RPC filter functionality;
- How to use a local RPC backend;
- How to integrate Modules in Pkg mode and launch them;



Core code:
- [example_ros2/srv/RosTestRpc.srv](../../../schema/ros2/example_ros2/srv/RosTestRpc.srv)
- [normal_rpc_co_client_module.cc](./managed/normal_rpc_co_client_module/normal_rpc_co_client_module.cc)
- [normal_rpc_co_server_module.cc](./managed/normal_rpc_co_server_module/normal_rpc_co_server_module.cc)
- [service.cc](./managed/normal_rpc_co_server_module/service.cc)
- [ros2_rpc_pkg/pkg_main.cc](./package/ros2_rpc_pkg/pkg_main.cc)



Configuration file:
- [examples_cpp_ros2_rpc_single_pkg_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_rpc_single_pkg_cfg.yaml)



How to run (Linux):
- Build IBMW with `IBMW_BUILD_EXAMPLES` and `IBMW_BUILD_WITH_ROS2` enabled;
- Run the `start_examples_cpp_ros2_rpc_single_pkg.sh` script in the build directory to start the process;
- Press `ctrl-c` to stop the process;


Notes:
- This example is essentially the same as the **ROS2 RPC Co** example, with the only difference being that `NormalRpcCoClientModule` and `NormalRpcCoServerModule` are integrated into a single package `ros2_rpc_pkg`;

## ROS2 RPC Server App


An RPC example using the ROS2 protocol with a local backend. This demo covers:
- How to use the ROS2 protocol as the RPC service protocol;
- How to use RPC functionality by creating a module in App mode;
- How to use a local RPC backend;
- How to launch in App mode;


Core code:
- [example_ros2/srv/RosTestRpc.srv](../../../schema/ros2/example_ros2/srv/RosTestRpc.srv)
- [normal_ros2_rpc_server_app/main.cc](./standalone/normal_ros2_rpc_server_app/main.cc)
- [normal_rpc_sync_client_module.cc](./managed/normal_rpc_sync_client_module/normal_rpc_sync_client_module.cc)
- [ros2_rpc_server_pkg/pkg_main.cc](./package/ros2_rpc_server_pkg/pkg_main.cc)


Configuration file:
- [examples_cpp_ros2_rpc_server_app_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_rpc_server_app_cfg.yaml)


How to run (Linux):
- Build IBMW with `IBMW_BUILD_EXAMPLES` and `IBMW_BUILD_WITH_ROS2` enabled;
- Run the `start_examples_cpp_ros2_rpc_server_app.sh` script in the build directory to start the process;
- Press `ctrl-c` to stop the process;


Notes:
- In App mode, this example directly creates a `NormalRpcSyncServerModule`, obtains a CoreRef handle, registers the `RosTestRpc` service, and provides echo functionality via the synchronous Server interface;
- This example uses a local RPC backend for communication;
- This example launches in App mode;


## ROS2 RPC Client App


An RPC example using the ROS2 protocol with a local backend. This demo covers:
- How to use the ROS2 protocol as the RPC service protocol;
- How to use RPC functionality by creating a module in App mode;
- How to use a local RPC backend;
- How to launch in App mode;


Core code:
- [example_ros2/srv/RosTestRpc.srv](../../../schema/ros2/example_ros2/srv/RosTestRpc.srv)
- [normal_ros2_rpc_client_app/main.cc](./standalone/normal_ros2_rpc_client_app/main.cc)
- [normal_rpc_sync_server_module.cc](./managed/normal_rpc_sync_server_module/normal_rpc_sync_server_module.cc)
- [ros2_rpc_client_pkg/pkg_main.cc](./package/ros2_rpc_client_pkg/pkg_main.cc)



Configuration file:
- [examples_cpp_ros2_rpc_client_app_cfg.yaml](./config/linux/bin/cfg/examples_cpp_ros2_rpc_client_app_cfg.yaml)


How to run (Linux):
- Build IBMW with `IBMW_BUILD_EXAMPLES` and `IBMW_BUILD_WITH_ROS2` enabled;
- Run the `start_examples_cpp_ros2_rpc_client_app.sh` script in the build directory to start the process;
- Press `ctrl-c` to stop the process;


Notes:
- In App mode, this example directly creates a `NormalRpcSyncClientModule`, obtains a CoreRef handle, and sends RPC requests to `RosTestRpc` at the configured frequency via the synchronous Client interface;
- This example uses a local RPC backend for communication;
- This example launches in App mode;

---

## ibmw-gen Way

This demo can also be generated using `ibmw-gen` from the
[`ros2_rpc.ibmw.yaml`](./ros2_rpc.ibmw.yaml) definition.

When `ros2_service` is specified in the YAML, ibmw-gen produces **real,
compilable** ROS2 RPC code — not just skeleton stubs. The generated server
registers the service, and the generated client sends periodic RPC calls in a
loop. You only need to fill in the service handler logic (the `// TODO` in the
server `.cc`).

### What ibmw-gen generates vs what you write

| Generated (auto) | You write |
|-------------------|-----------|
| `CMakeLists.txt` -- build system with ROS2 RPC gencode linking | Service handler logic in `rpc_server_module.cc` |
| `cfg/ros2_rpc_app.yaml` -- config with `ros2_extension` + `type: ros2` driver | Client request population in `rpc_client_module.cc` |
| `cfg/ros2_rpc_pkg.yaml` -- Pkg-mode config | |
| `src/ros2_rpc_app/main.cc` -- App entry point | |
| `src/*_pkg/pkg_main.cc` -- Pkg loaders | |
| `src/rpc_server_module/` -- server with service impl class | |
| `src/rpc_client_module/` -- client with RPC loop + config reading | |

### Generate, build, and run

```bash
# Generate
python3 <ibmw-gen>/ibmw_gen.py ros2_rpc.ibmw.yaml -o ros2_rpc_gen

# Build
source /opt/ros/humble/setup.bash
mkdir -p ros2_rpc_gen/build && cd ros2_rpc_gen/build
cmake .. -DCMAKE_PREFIX_PATH=<ibmw_build_dir>
make -j$(nproc)

# Run (App mode -- needs ros2_extension .so on LD_LIBRARY_PATH)
export LD_LIBRARY_PATH="<ibmw_build_dir>/lib:<ibmw_build_dir>:$LD_LIBRARY_PATH"
./ros2_rpc_app cfg/ros2_rpc_app.yaml

# Run (Pkg mode)
ibmw run cfg/ros2_rpc_pkg.yaml
```

### Key YAML features

```yaml
modules:
  - name: RpcServerModule
    type: rpc_server
    transport: ros2
    ros2_package: example_ros2
    ros2_service: RosTestRpc               # ← triggers real code generation
    ros2_rpc_gencode_target: "ibmw::schema::example_ros2_ibmw_rpc_gencode"

  - name: RpcClientModule
    type: rpc_client
    transport: ros2
    ros2_package: example_ros2
    ros2_service: RosTestRpc               # ← triggers real code generation
    ros2_rpc_gencode_target: "ibmw::schema::example_ros2_ibmw_rpc_gencode"
    custom_config:
      rpc_frq: 1.0                         # ← custom per-module config
```

The `ros2_service` field tells ibmw-gen to generate code that includes the
gencode header (`RosTestRpc.ibmw_rpc.srv.h`) and uses the typed
`SyncService`/`SyncAccess` classes from it. Without `ros2_service`, ibmw-gen
falls back to skeleton stubs with TODO markers.

The generated config automatically includes:
- `ros2_extension` with `node_name` derived from the project namespace
- `type: ros2` service driver alongside `type: local`
- `enable_drivers: [ros2, local]` for all client/server func_name patterns

The hand-written demos above show all 4 calling patterns (Sync, Async, Future,
Coroutine). The ibmw-gen version generates the Sync pattern as the recommended
starting point.

**Note:** The ROS2 RPC service types (`RosTestRpc`) are generated externally by
the main IBMW build via `ros2_gen_ibmw_cpp_rpc.py`. ibmw-gen references the
pre-built `ros2_rpc_gencode_target` CMake target for linking.

### Architecture note: ROS2 backend requirement

ROS2 RPC interop requires the `ros2_extension` (which runs an `rclcpp::Node`
inside the IBMW process). This is because ROS2 services use a custom
request/reply protocol built on top of rcl, not standard DDS request/reply.
See the [ROS2 Backend Dependency Analysis](#ros2-backend-dependency) section
below for details.

<a id="ros2-backend-dependency"></a>

## ROS2 Backend Dependency Analysis

### Which IBMW features require the ROS2 backend?

The "ROS2 backend" means loading `ros2_extension` (`libibmw_ros2_transport.so`),
which creates an `rclcpp::Node` and runs an `rclcpp::Executor` spin thread.
This effectively makes the IBMW process a ROS2 node running inside the rcl
runtime.

| Feature | Extension needed | Requires rcl? | Notes |
|---------|-----------------|---------------|-------|
| Pub/sub (IBMW types, DDS wire) | `dds_extension` | **No** | Pure FastDDS |
| Pub/sub (ROS2 interop via DDS) | `dds_extension` | **No** | Uses `ros2_compatible: true`, DDS topic/type naming |
| Pub/sub (ROS2 types via rcl) | `ros2_extension` | Yes | Uses `rcl_publish`/rcl subscriptions |
| Zero-copy / loaned message | `dds_extension` | **No** | FastDDS data sharing |
| **ROS2 RPC / Services** | **`ros2_extension`** | **Yes** | `rcl_service_init`, `rcl_send_request/response` |
| DDS RPC / Services | `dds_extension` | **No** | `type: dds` service driver, pure FastDDS. See `dds_rpc_demo`. |

**Key insight:** Pub/sub ROS2 interop is fully achievable via pure DDS (the
`dds_extension` with `ros2_compatible: true` handles topic name prefixing
(`rt/`) and DDS type name conversion). **ROS2 services can now also work
without rcl** using the `type: dds` service driver (see `dds_rpc_demo`).
The `type: ros2` driver still requires rcl for full ROS2 service compatibility.

### Why does ROS2 RPC require rcl?

ROS2 services do **not** use standard DDS request/reply (DDS-RPC). Instead,
they use a custom protocol layered on DDS, managed entirely by the rcl/rmw
layer:

1. **Custom topic naming:** Two DDS topics per service:
   - `rq/<node_name>/<service_name>Request`
   - `rr/<node_name>/<service_name>Reply`

2. **Custom correlation:** Uses `rmw_request_id_t` (writer GUID + sequence
   number) for request-response matching, not DDS correlation IDs.

3. **Service type support:** Requires `rosidl_service_type_support_t` which
   bundles request/response type support with service-specific metadata.

4. **IBMW's rcl API usage:**
   - Server: `rcl_service_init()`, `rcl_send_response()`, inherits
     `rclcpp::ServiceBase`, registers via `node->get_node_services_interface()`
   - Client: `rcl_client_init()`, `rcl_send_request()`, inherits
     `rclcpp::ClientBase`, registers via `node->get_node_services_interface()`
   - Executor: `rclcpp::Executor::spin()` dispatches incoming requests/responses

### Demos by backend requirement

**Pure DDS (no rcl, no ros2_extension):**
- `dds_rpc_demo` — DDS RPC with `type: dds` service driver
- `ros2_interop_demo` — ROS2 pub/sub interop via DDS (`ros2_compatible: true`)
- `ros2_point_demo` — ROS2 geometry_msgs via DDS
- `transport_demo` — DDS pub/sub
- `dds_native_demo`, `dds_image_demo` — native DDS types
- `loaned_message_demo` — zero-copy via FastDDS

**Requires ros2_extension (rcl runtime):**
- `ros2_rpc` — all sub-demos (Sync, Async, Future, Co, SinglePkg, App)
- `ros2_chn` — ROS2 transport pub/sub via rcl (though this could also work
  via DDS with `ros2_compatible: true`)
