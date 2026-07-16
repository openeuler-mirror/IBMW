# multi_driver_demo

End-to-end exerciser for the variadic `IBMW_USE_DRIVERS` macro.

A single user-region module declares typed-slot bindings against **two**
independent transport drivers (DDS + Iceoryx) within the same translation
unit, proving that the macro composes correctly across extensions.

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

## Build

This sample is gated by `IBMW_BUILD_DDS_EXTENSION AND IBMW_BUILD_ICEORYX_EXTENSION`
in `src/samples/cpp/CMakeLists.txt` and is automatically included by the
`full` preset.

## Run (App mode, no extensions required)

```bash
cd build
./multi_driver_demo_app ./cfg/multi_driver_demo_app_mode_cfg.yaml
```

The app-mode config does not load any transport extensions, so the demo
runs without RouDi or a DDS domain. Slot binding is deliberately not
exercised at runtime; the value of this sample is the compile-time
proof that `IBMW_USE_DRIVERS` accepts heterogeneous driver types.

## Run (Pkg mode)

```bash
cd build
./bin/ibmw run ./cfg/multi_driver_demo_cfg.yaml
```
