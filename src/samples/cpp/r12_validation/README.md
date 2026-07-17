# r12_validation

Scanner validation sample for the **R12 red-line rule**. It is a deliberate
fail-sample: its sole purpose is to keep `scripts/check_red_lines.sh`'s R12
detector honest.

> This sample is a reverse-proof for the project's red-line scanner. It is
> not a user-facing demo — do not copy it into production code.

## R12 in one paragraph

The macros `IBMW_HAS_DDS`, `IBMW_HAS_ICEORYX`, `IBMW_HAS_ROS2`, … (declared
in `src/api/cpp/features.h`) are **compile-time selectors**. They exist so
the linker can elide entire transport branches in builds that disable the
corresponding extension. Using them as runtime expressions
(`if (IBMW_HAS_DDS) { … }`) defeats that elision and quietly turns a
compile-time invariant into a runtime branch — silently, with no compiler
warning. **R12** forbids this pattern.

## What this directory contains

```
r12_validation/
└── r12_runtime_use_fail/
    └── standalone/
        └── main.cc         # ~39 lines, deliberately violates R12
```

There is **no** `CMakeLists.txt`. The file is intentionally not added to
any build target — it only needs to exist on disk for
`scripts/check_red_lines.sh --scan-path` to pick it up.

## What `main.cc` does

```cpp
#include "cpp/features.h"

int main() {
  if (IBMW_HAS_DDS) {        // <-- R12 violation: runtime use of compile-time macro
    return 0;
  }
  return 1;
}
```

That single `if (IBMW_HAS_DDS)` is the entire R12 violation surface. The
scanner reports +1 R12 on this file every time, by design.

## Why this directory exists

The same reasoning as `r8_validation/`: a red-line rule that only lives in
documentation rots silently. By committing a tiny fail-sample whose sole
job is to make the detector fire, "R12 still works" becomes a build-time
invariant rather than tribal knowledge.

## Related rules

- **R8 / R8.2 / R1** — covered by `r8_validation/`.
- **R11** — covered by a separate scan over the generator tree.
