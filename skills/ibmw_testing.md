# IBMW Testing Skill

Use verification proportional to the changed contract. Do not reuse historical
counts or measurements as proof for a new checkout.

## Core sequence

1. Configure a compatible clean build directory.
2. Build affected targets.
3. Run focused unit/integration tests.
4. Run architecture/red-line checks for public, generator, ABI, or transport
   changes.
5. Run a bounded functional smoke with a meaningful success assertion.
6. Record commands, environment, results, and omissions.

## Required by change type

- **Generator or schema:** run pytest, regenerate maintained examples, and check
  byte equality unless fixture changes are intentional.
- **Package ABI/export:** compile a consumer and inspect the exported
  `IbmwPkgEntry` where applicable.
- **Transport/loan path:** cover enabled and disabled feature combinations;
  verify unsupported loans fail and supported loans preserve ownership.
- **Extension:** load representative YAML and exercise its messaging/RPC path.
- **Cross build:** validate target prefixes, isolate host paths, inspect ELF
  architecture/dependencies, and build a clean installed consumer.
- **Performance:** use repeated, like-for-like runs with recorded hardware,
  software, affinity, payload, duration, and mode. Functional PASS is not a
  performance result.

## Test integrity

- Add a regression test for a non-trivial bug fix.
- Never use skip/only markers or weaken assertions to hide a failure.
- Treat generated fixtures as reviewed source artifacts.
- Do not accept a timeout alone when the test can assert a receive, response, or
  state transition.
- Report external dependency or environment blockers explicitly.

Canonical public guidance: [Testing and Verification](../docs/contributing/testing.md).
