# DDS RPC Demo

Demonstrates the IBMW pure-DDS RPC service path with ROS2-compatible service
wire naming. The demo uses the DDS service driver (`type: dds`) and does not
depend on `rcl` or `rclcpp` at runtime.

The source of truth is [`dds_rpc_demo.ibmw.yaml`](./dds_rpc_demo.ibmw.yaml). It
models the ROS2 `example_interfaces/srv/AddTwoInts` service through generated
DDS IDL and RPC wrapper code.

## What Gets Generated

From `dds_rpc_demo.ibmw.yaml`, `ibmw-gen` produces:

- DDS IDL type definitions in `schema/AddTwoInts.idl`
- RPC service metadata in `schema/AddTwoIntsService.yaml`
- RPC gencode target `add_two_ints_service_rpc_gencode`
- CDR glue target `add_two_ints_service_cdr`
- `RpcServerModule`, a sync RPC server for method `AddTwoInts`
- `RpcClientModule`, a periodic sync RPC client for method `AddTwoInts`
- App-mode binaries `dds_rpc_server_app` and `dds_rpc_client_app`
- Pkg-mode configs `rpc_server_module_pkg.yaml` and
  `rpc_client_module_pkg.yaml`
- DDS extension config with `ros2_compatible_mode: true`

The generated service metadata derives the ROS2-compatible DDS type names from
the `.srv` path:

```yaml
services:
  - name: AddTwoIntsService
    ros2_service_name: "/add_two_ints"
    methods:
      - name: AddTwoInts
        request: AddTwoInts_Request
        response: AddTwoInts_Response
        req_dds_type_name: "example_interfaces::srv::dds_::AddTwoInts_Request_"
        rsp_dds_type_name: "example_interfaces::srv::dds_::AddTwoInts_Response_"
```

## Generate

```bash
cd src/samples/cpp/dds_rpc_demo
python3 ../../../tools/ibmw_gen/ibmw_gen.py dds_rpc_demo.ibmw.yaml -o dds_rpc_demo_gen
```

## Build

```bash
cd dds_rpc_demo_gen
mkdir -p build && cd build
cmake .. -DCMAKE_PREFIX_PATH=<ibmw-build-or-install-dir>
make -j$(nproc)
```

## Run App Mode

Use two terminals from `dds_rpc_demo_gen/build`:

```bash
# Terminal 1: server
./dds_rpc_server_app cfg/dds_rpc_server_app.yaml

# Terminal 2: client
./dds_rpc_client_app cfg/dds_rpc_client_app.yaml
```

## Run Pkg Mode

Use two terminals from `dds_rpc_demo_gen/build`:

```bash
# Terminal 1: server package
ibmw run cfg/rpc_server_module_pkg.yaml

# Terminal 2: client package
ibmw run cfg/rpc_client_module_pkg.yaml
```

## Configuration Files

| Config | Description |
|--------|-------------|
| `dds_rpc_server_app.yaml` | App-mode server using DDS service driver |
| `dds_rpc_client_app.yaml` | App-mode client using DDS service driver |
| `rpc_server_module_pkg.yaml` | Pkg-mode server package |
| `rpc_client_module_pkg.yaml` | Pkg-mode client package |

## Concepts Demonstrated

- Pure-DDS service driver without `rcl` / `rclcpp` runtime dependency
- Native `.srv`-style generator input through `srv: example_interfaces/srv/AddTwoInts`
- Automatic ROS2 service name derivation (`/add_two_ints`)
- Automatic ROS2 DDS request/response type-name derivation
- Generated DDS IDL + generated DDS RPC CDR glue
- App-mode and Pkg-mode service deployment from one `.ibmw.yaml`

## ROS2 Interop Scope

With `ros2_compatible_mode: true`, the generated service uses ROS2-compatible
DDS service naming and type names for `example_interfaces/srv/AddTwoInts`.
Runtime interop still requires a ROS2 peer using the same service type and a
FastDDS-compatible RMW configuration. This sample documents the generated IBMW
side; it does not introduce an `rcl` / `rclcpp` dependency into the IBMW
server/client binaries.
