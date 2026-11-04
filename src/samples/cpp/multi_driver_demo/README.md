# multi_driver_demo

End-to-end exerciser for the variadic `IBMW_USE_DRIVERS` macro.

A single user-region module declares typed-slot bindings against **two**
independent transport drivers (DDS + Iceoryx, or DDS + the self-developed
shm transport) within the same translation unit, proving that the macro
composes correctly across extensions.

## What this demo proves

The `IBMW_USE_DRIVER` macro supports a single driver per module class.
`IBMW_USE_DRIVERS` is the variadic counterpart that accepts an arbitrary
number of `IBMW_DRIVER_ENTRY(DriverT, member_ptr)` arguments, generating
one slot-match clause per entry inside a single `SetDriverSlot` override.

Successful compilation of `managed/multi_driver_module/multi_driver_module.h`
is the compile-time witness; the static library, the package `.so`, and the
standalone `_app` binary together provide the link-time and runtime
witnesses that the macro produces a well-formed `Module` subclass.

## Layout

| Path | Role |
|------|------|
| `multi_driver_demo.ibmw.yaml` | ibmw-gen project file (regen byte-equality artifact only) |
| `managed/multi_driver_module/` | Hand-written module using `IBMW_USE_DRIVERS` |
| `package/multi_driver_pkg/` | Pkg ABI shared library wrapping the module |
| `standalone/multi_driver_demo_app/` | App-mode binary embedding the module |
| `config/linux/bin/cfg/` | Pkg-mode and app-mode runtime configs |

## Variants

The module sources are built once per enabled second driver; the
`IBMW_MULTI_DRIVER_DEMO_WITH_SHM` define (PUBLIC on the shm module target)
selects `shm_extension::ShmTransportDriver` instead of
`iceoryx_extension::IceoryxTransportDriver`.

| Variant | Built when | Module lib / pkg / app | Pkg-mode config |
|---------|------------|------------------------|-----------------|
| DDS + Iceoryx | `IBMW_BUILD_ICEORYX_EXTENSION` | `multi_driver_module` / `libmulti_driver_pkg.so` / `multi_driver_demo_app` | `cfg/multi_driver_demo_cfg.yaml` |
| DDS + shm | `IBMW_BUILD_SHM_EXTENSION` | `multi_driver_module_shm` / `libmulti_driver_shm_pkg.so` / `multi_driver_shm_demo_app` | `cfg/multi_driver_demo_shm_cfg.yaml`, `cfg/multi_driver_demo_shm_bind_cfg.yaml` |

## Build

This sample is gated by `IBMW_BUILD_DDS_EXTENSION AND
(IBMW_BUILD_ICEORYX_EXTENSION OR IBMW_BUILD_SHM_EXTENSION)` in
`src/samples/cpp/CMakeLists.txt` and is automatically included by the
`full` preset. With iceoryx OFF only the shm variant is built.

## Run (App mode, no extensions required)

```bash
cd build
./multi_driver_demo_app ./cfg/multi_driver_demo_app_mode_cfg.yaml
```

The app-mode config does not load any transport extensions, so the demo
runs without RouDi or a DDS domain. Slot binding is deliberately not
exercised at runtime; the value of this sample is the compile-time
proof that `IBMW_USE_DRIVERS` accepts heterogeneous driver types.

The shm variant runs the same way:
`./multi_driver_shm_demo_app ./cfg/multi_driver_demo_app_mode_cfg.yaml`.

## Run (Pkg mode)

```bash
cd build
./ibmw run ./cfg/multi_driver_demo_cfg.yaml           # DDS + Iceoryx
./ibmw run ./cfg/multi_driver_demo_shm_cfg.yaml       # DDS + shm
```

## Run (Pkg mode, slot binding exercised, DDS + shm)

```bash
cd build
./ibmw run ./cfg/multi_driver_demo_shm_bind_cfg.yaml
```

This config loads the DDS and shm extensions and puts one topic on both
drivers, so the engine binds both typed slots after `Setup()`; `Run()` then
logs `Run: DDS driver slot bound.` and `Run: Shm driver slot bound.`. No
RouDi daemon is needed.
