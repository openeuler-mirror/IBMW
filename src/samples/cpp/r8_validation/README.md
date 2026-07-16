# r8_validation

Compile-time and link-time validation samples for the **R8 / R8.2 red-line
rules**. They pin the public `transport_factory.h` API surface against the
legacy `iceoryx_ext` direct-include surface and **prove at the build level**
that mixing the two APIs or skipping the documented escape-hatch anchor is
detected.

> These samples are reverse-proofs for the project's red-line scanner. They
> are not user-facing demos — do not copy them into production code.

## R8 / R8.2 in one paragraph

- **R8** — User-region code must reach iceoryx through the public
  `cpp/transport/transport_factory.h` facade (`publisher_for<T>()` /
  `subscriber_for<T>()`). The legacy direct-include surface
  (`extensions/transport/iceoryx/...`) is reserved for first-party
  extensions and benchmarks.
- **R8.2** — A single translation unit may not mix the public facade API
  with the legacy direct-include API. The `IbmwR82SlotMixCheck<>` template
  static-asserts on the conflict.
- **R1 escape-hatch** — A first-party extension or benchmark that
  *deliberately* includes a transport-private header must wrap the
  `#include` in `IBMW_R1_EXEMPT_BEGIN/END` comment anchors. Without the
  anchor `scripts/check_red_lines.sh` reports an R1 violation.

## The four sub-samples

| Subdir | Default build? | Result | Validates |
|--------|----------------|--------|-----------|
| `direct_iceoryx_publisher_ok/` | yes | Compiles + links | `publisher_for<T>()` works without any private include or symbol |
| `direct_iceoryx_subscriber_ok/` | yes | Compiles + links | `subscriber_for<T>()` works the same way |
| `direct_iceoryx_mixed_api_fail/` | gated by `IBMW_BUILD_R8_FAIL_SAMPLES` | **Fails to compile** with `IbmwR82SlotMixCheck<>` static_assert | R8.2 detector trips when both APIs appear in the same TU |
| `direct_iceoryx_no_anchor_fail/` | gated by `IBMW_BUILD_R8_FAIL_SAMPLES` | Compiles, but `check_red_lines.sh` reports +1 R1 hit | R1 escape-hatch convention is enforced by the scanner, not by the compiler |

The two `_fail` samples are *expected fails* — they exist to keep the
detectors honest. They are **not** wired into the default build, so a
developer running `cmake --preset full` never sees a build break from them.

## Build

```bash
# OK samples only (default)
cmake --preset full
cmake --build build -j$(nproc)

# Include the two fail samples
cmake -B build_r8_fail \
  -DIBMW_BUILD_ICEORYX_EXTENSION=ON \
  -DIBMW_BUILD_R8_FAIL_SAMPLES=ON
cmake --build build_r8_fail -j$(nproc)   # mixed_api_fail breaks here as designed
```

## Why this directory exists

A red-line rule that is only documented in prose would silently rot the
moment someone refactored the detector or the public facade. By committing
**fail-samples** that the build/scan asserts must keep failing, R8 / R8.2 / R1
detection becomes self-checking: a regression in the detector is caught the
same week it lands, not in production six months later.

## Related rules

- **R11** — covered by a separate scan over the generator tree.
- **R12** — covered by `r12_validation/`.
