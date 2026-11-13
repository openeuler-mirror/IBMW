# shm_inproc_demo

Single-process pub/sub demo on the self-developed shared-memory transport
extension (`libibmw_shm_transport.so`, `src/extensions/transport/shm_ext`).
It is `iceoryx_inproc_demo` with the transport swapped: the publisher and
subscriber modules and the app entry are compiled from the
`iceoryx_inproc_demo` sources, only the YAML differs. There is **no RouDi
daemon** and the binaries do not link iceoryx, so the demo builds and runs
with `IBMW_BUILD_SHM_EXTENSION=ON` and `IBMW_BUILD_ICEORYX_EXTENSION=OFF`.

## What this demo proves

- The shm transport extension is loaded as a regular IBMW transport and
  serves **two managed modules in the same process**.
- Migrating `iceoryx_inproc_demo` needs no code change: same modules, same
  `sensor::CompressedImage` IDL type (`ibmw::schema::sensor_idl_gencode`),
  same topic, only the extension item and the driver name change.
- `shm_init_size: 256` with 1 KB / 16 KB / 256 KB / 1 MB frames exercises
  the shm loan grow path; the subscriber checks the byte pattern of every
  frame (`integrity=OK`).

## Layout

| Path | Role |
|------|------|
| `../iceoryx_inproc_demo/managed/` | Publisher / subscriber modules (shared, not copied) |
| `../iceoryx_inproc_demo/standalone/pubsub_app/main.cc` | App entry (shared); the banner is overridden through `IBMW_DEMO_TITLE` / `IBMW_DEMO_TRANSPORT_NOTE` |
| `config/linux/bin/cfg/shm_inproc_cfg.yaml` | Loads `shm_extension`, wires both modules to the same topic over driver `shm` |
| `config/linux/bin/start_shm_inproc.sh` | Convenience launcher (no RouDi) |
| `CMakeLists.txt` | Two `STATIC` module libs + one `shm_inproc_app` executable; `DEPENDS ibmw_extensions_shm_ext` |

## Build

Gated by `IBMW_BUILD_SHM_EXTENSION`:

```bash
cmake -S . -B build -DIBMW_BUILD_EXAMPLES=ON -DIBMW_BUILD_SHM_EXTENSION=ON
cmake --build build -j$(nproc)
```

Skipped automatically (with a `Skip shm_inproc_demo because
IBMW_BUILD_SHM_EXTENSION is OFF` status) otherwise.

## Run

```bash
cd build
DURATION=6 bash start_shm_inproc.sh      # omit DURATION to run until Ctrl-C
```

Manual run:

```bash
cd build
./shm_inproc_app cfg/shm_inproc_cfg.yaml
```

Shared-memory segments are created under `$IBMW_SHM_ROOT` when set, else
`/dev/shm`, else `/tmp` (`root_dir` / `domain` extension options override
this). Point `IBMW_SHM_ROOT` at a private directory to keep test runs apart.

## Expected behavior

The subscriber logs one `Received compressed_image ... payload_size=...
integrity=OK` line per published frame, cycling through 1 KB, 16 KB,
256 KB and 1 MB payloads.

## Related samples

- `shm_interproc_demo` — same modules, two processes.
- `iceoryx_inproc_demo` — the same demo on the iceoryx transport.
- `transport_benchmark/shm_send` — latency benchmark of the same Send path.
