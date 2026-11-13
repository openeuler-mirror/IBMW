# Contributing to IBMW

Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

Start with `docs/README.md`, then read the user, reference, or contributor guide
for the area you plan to change. Record verification in the change description
or pull request; do not copy historical results into a new change.

Canonical contributor references:

- [Architecture overview](docs/architecture/overview.md)
- [Architecture rules](docs/contributing/architecture-rules.md)
- [Testing and verification](docs/contributing/testing.md)
- [Support matrix](docs/reference/support-matrix.md)

## 1. Red lines R1-R12

Any new violation relative to `scripts/baseline_red_lines.txt` is a stop-the-line
event. Fix the source; do not move the baseline to hide the violation.

| ID | Rule |
|---|---|
| R1 | Managed user code does not include transport implementation headers. |
| R2 | Managed user code does not call non-public transport-driver members. |
| R3 | Managed user code does not construct transport drivers directly. |
| R4 | `src/api/` and `src/core/` do not include extension implementations. |
| R5 | Core/public API does not statically link extension implementations. |
| R6 | Managed messaging goes through Module/Context public APIs. |
| R7 | Package injection uses the single `IbmwPkgEntryV2` typed-slot ABI, not driver-name strings or dual public entries. |
| R8 | Loaned/direct send combinations are protected by compile-time transport capability guards and fail fast when unsupported. |
| R9 | Framework-generated typesupport/transport initialization does not leak into editable user regions. |
| R10 | Intentional low-level APIs use explicit `direct_<transport>_*` escape-hatch modules and their documented anchors. |
| R11 | Publisher/subscriber and loaned generator templates remain unified rather than split by transport. |
| R12 | `IBMW_HAS_<TRANSPORT>` and related feature macros are compile-time selectors only: preprocessor checks and `static_assert` are valid; runtime expressions such as `if (IBMW_HAS_DDS)` are forbidden. |

Durable definitions are in `skills/transport_decoupling.md`; executable scanner
behavior is defined by `scripts/check_red_lines.sh`.

## 2. Change verification

Run verification proportional to the affected boundary:

1. Run red-lines before and after user-surface, generator, ABI, or transport work.
2. Run focused unit tests for the changed component.
3. For generator changes, run pytest and byte-equal in-tree regeneration; commit
   intentional fixture changes with the generator.
4. Build changed targets, then run applicable ctest and strict functional smoke.
5. For package ABI/export changes, inspect `IbmwPkgEntryV2` with `nm -D`.
6. For feature/configuration changes, run the affected dual-build cells.
7. For hot-path changes, run the current pinned performance protocol and compare
   p99 with the accepted baseline. Functional PASS is not a performance seal.

The applicable public guide defines the supported boundary. Historical counts
or results from another checkout are not acceptance targets for a later change.

Hooks may be installed with:

```bash
bash scripts/install_git_hooks.sh
```

Hook output supplements review; it does not replace the required commands.

Before asking for a merge, run the local gate. It builds, runs the unit tests,
checks release hygiene and docs governance on the committed tree, and validates
every commit message on the branch, one step after another:

```bash
bash scripts/ci_check.sh --base <target-branch>   # add --skip-build for policy checks only
```

Build and test parallelism defaults to 10 and 8 jobs (`IBMW_CI_JOBS`,
`IBMW_CI_TEST_JOBS`). `.editorconfig` and `.clang-format` (clang-format 18)
describe the existing style; apply them to the lines you change, for example
with `git clang-format`, not to whole files. Files listed in
`scripts/clang_format_files.txt` must stay fully formatted; `ci_check.sh`
checks them with clang-format 18 (set `CLANG_FORMAT` if it is not on `PATH`).

## 3. Commit and history policy

Use focused commits. Types should match actual repository history; common types
include `feat`, `fix`, `refactor`, `docs`, `test`, `perf`, `diag`, `bench`, `ci`,
`build`, and `chore`.

```text
<type>(<scope>): <imperative subject>

Why:
<reason and user/engineering impact>

What:
<material changes>

Compatibility:
<ABI/API/config/runtime boundary, or "No change">

Verification:
<commands actually run and results; state omissions explicitly>

Refs:
<issue, design, evidence section, or milestone>
```

Do not claim checks that were not run. Do not combine unrelated fixes merely to
reduce commit count.

Published history must not be rewritten. Do not force-push, amend already pushed
commits, or rebase shared commits. Milestones use annotated tags so the tag has a
message, author, date, and auditable target. Tag only after the milestone's stated
verification has been run and recorded.

## 4. Compatibility and generated artifacts

- Preserve the typed-slot ABI and public/internal header split described in
  `docs/reference/compatibility.md`.
- Preserve zero-copy loan ownership and avoid hidden fallback/copy behavior.
- Keep ROS2/rcl compatibility, DDS-only ROS2 interop, Action, and OHOS claims
  within their documented scope.
- Do not add build products or raw local progress evidence. Public benchmark
  summaries must state their command, environment, and interpretation.

## 5. Tests and comments

- Add a regression test before or with a non-trivial bug fix.
- Never use skip/only markers or delete a test to conceal a failure.
- Comments should document public contracts, difficult rationale, performance or
  security constraints, or durable SOP anchors. Do not narrate edits or retain
  commented-out code.
- A failure after repeated attempts should be reduced and recorded with exact
  evidence; consult `docs/guides/troubleshooting.md` before changing architecture.
