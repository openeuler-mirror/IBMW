# shm_interproc_demo

Two-process pub/sub demo on the self-developed shared-memory transport
(`libibmw_shm_transport.so`, `src/extensions/transport/shm_ext`). It is
`iceoryx_interproc_demo` with the transport swapped: the modules and the two
process entries are compiled from the `iceoryx_interproc_demo` sources, only
the YAML differs. Publisher and subscriber are **separate OS processes** that
meet in the same shm root directory and domain; there is **no RouDi daemon**
and the binaries do not link iceoryx.

## What this demo proves

- A publisher and a subscriber process communicate over the shm transport
  through the IBMW transport facade, without any broker process.
- Migrating `iceoryx_interproc_demo` needs no code change: same modules,
  same `sensor::CompressedImage` IDL type, same topic; only the extension
  item and the driver name change.
- 1 KB / 16 KB / 256 KB / 1 MB frames with `shm_init_size: 256` exercise
  the loan grow path across the process boundary; the subscriber checks the
  byte pattern of every frame (`integrity=OK`).

## Layout

| Path | Role |
|------|------|
| `../iceoryx_interproc_demo/managed/` | Publisher / subscriber modules (shared, not copied) |
| `../iceoryx_interproc_demo/standalone/{publisher_app,subscriber_app}/main.cc` | Process entries (shared); banner overridden through `IBMW_DEMO_TITLE` / `IBMW_DEMO_TRANSPORT_NOTE` |
| `config/linux/bin/cfg/shm_interproc_publisher.yaml` | Publisher process config |
| `config/linux/bin/cfg/shm_interproc_subscriber.yaml` | Subscriber process config |
| `config/linux/bin/start_shm_interproc.sh` | 2-process launcher (no RouDi) |

## Build

Gated by `IBMW_BUILD_SHM_EXTENSION`:

```bash
cmake -S . -B build -DIBMW_BUILD_EXAMPLES=ON -DIBMW_BUILD_SHM_EXTENSION=ON
cmake --build build -j$(nproc)
```

## Run

```bash
cd build
DURATION=8 bash start_shm_interproc.sh   # omit DURATION to run until Ctrl-C
```

The helper script starts `shm_interproc_subscriber_app`, then
`shm_interproc_publisher_app`, and stops both with `SIGINT` on exit (only
the PIDs it started). Logs go to `shm_interproc_subscriber.log` and
`shm_interproc_publisher.log` next to the script.

Manual two-terminal run:

```bash
# Terminal 1 (subscriber first)
cd build
./shm_interproc_subscriber_app cfg/shm_interproc_subscriber.yaml

# Terminal 2 (publisher)
cd build
./shm_interproc_publisher_app cfg/shm_interproc_publisher.yaml
```

Both processes must resolve the same shm root and domain: `$IBMW_SHM_ROOT`
(else `/dev/shm`, else `/tmp`) and `$IBMW_SHM_DOMAIN` (else `default`), or
the `root_dir` / `domain` extension options.

## Expected behavior

The subscriber prints one `Received compressed_image ... integrity=OK` line
per frame published by the other process.

## Related samples

- `shm_inproc_demo` — same modules, single process.
- `iceoryx_interproc_demo` — the same demo on the iceoryx transport.
- `transport_benchmark/shm_send`, `shm_loan_v2`, `shm_loaned_pod` — latency
  benchmarks on the shm transport.
