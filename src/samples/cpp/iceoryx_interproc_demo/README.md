# iceoryx_interproc_demo

Two-process pub/sub demo over the Iceoryx shared-memory transport. Publisher
and subscriber are **separate OS processes** connected through a shared
`iox-roudi` daemon, exercising the cross-process zero-copy SHM path
end-to-end.

## What this demo proves

- A publisher and a subscriber process can communicate over Iceoryx shared
  memory through the IBMW transport facade without copying the payload
  across the process boundary (zero-copy on the hot path).
- The `sensor::CompressedImage` IDL type is reused from
  `ibmw::schema::sensor_idl_gencode` (via `WRAP_IDLS CompressedImage`) — no
  hand-written CDR shim, and the **wire format is byte-equal to**
  `iceoryx_inproc_demo`. Switching between the two demos is a deployment
  decision, not an API change.
- Large crafted payloads exercise the SHM auto-grow path across the
  process boundary.

## Layout

| Path | Role |
|------|------|
| `managed/publisher_module/` | `publisher_module.{h,cc}` |
| `managed/subscriber_module/` | `subscriber_module.{h,cc}` |
| `managed/compressed_image_dds_types.h` | Type alias header |
| `standalone/publisher_app/main.cc` | Publisher process entry |
| `standalone/subscriber_app/main.cc` | Subscriber process entry |
| `config/linux/bin/cfg/publisher.yaml` | Publisher process config |
| `config/linux/bin/cfg/subscriber.yaml` | Subscriber process config |
| `config/linux/bin/start_iceoryx_interproc.sh` | RouDi + 2-process launcher |

## Build

Gated by `IBMW_BUILD_ICEORYX_EXTENSION`:

```bash
cmake --preset full -DIBMW_BUILD_ICEORYX_EXTENSION=ON
cmake --build build -j$(nproc)
```

## Run

```bash
cd build
bash start_iceoryx_interproc.sh
```

The helper script starts `iox-roudi`, then `iceoryx_interproc_subscriber_app`,
then `iceoryx_interproc_publisher_app`, lets them run for ~5s, and tears
all three down using the documented sample process cleanup sequence (never
`pkill`).

Manual two-terminal run:

```bash
# Terminal 1 (RouDi)
iox-roudi &

# Terminal 2 (subscriber first, so discovery is hot)
cd build
./iceoryx_interproc_subscriber_app cfg/subscriber.yaml

# Terminal 3 (publisher)
cd build
./iceoryx_interproc_publisher_app cfg/publisher.yaml
```

## Expected behavior

The subscriber prints one log line per received frame. With Iceoryx the
payload is read directly out of the shared-memory chunk produced by the
publisher — no per-frame `memcpy` across the process boundary, in line with
the project-wide zero-copy invariant.

## Related samples

- `iceoryx_inproc_demo` — same modules, single process.
- `transport_benchmark/iox_loaned_pod` and `iox_loan_v2` — quantitative
  latency benchmarks over this same code path with crafted 1KiB-1MiB
  payload sweeps.
