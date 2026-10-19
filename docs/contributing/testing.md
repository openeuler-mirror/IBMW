# Testing and Verification

Choose checks according to the boundary changed. Record commands actually run,
their result, and any intentional omission.

## Minimum workflow

1. Configure a clean or compatible build directory.
2. Build the affected target.
3. Run focused unit or integration tests.
4. Run applicable architecture/red-line checks.
5. For runtime behavior, capture a bounded smoke run and verify a meaningful
   success condition rather than accepting a timeout alone.

## Change-specific checks

| Change | Required verification |
|---|---|
| Public API or package ABI | Compile consumers; inspect exported package entry when relevant; test compatibility paths. |
| Generator/schema/template | Run generator tests, regenerate maintained examples, and require byte-equal output unless fixtures intentionally change. |
| Transport or zero-copy path | Build supported feature combinations; test unsupported combinations fail clearly; verify ownership/copy semantics. |
| Extension | Build and load the extension with representative configuration; run focused RPC or messaging smoke. |
| Cross compilation | Validate the target prefix, configure with host paths isolated, build target artifacts, and inspect architecture/dependencies. |
| Performance-sensitive path | Use the public benchmark methodology with pinned environment metadata and repeated runs; compare like-for-like modes. |

## Test quality rules

- Add a regression test with a non-trivial bug fix.
- Do not use skip/only markers, delete assertions, or broaden accepted exit codes
  to conceal a failure.
- Keep fixtures deterministic and review generated changes as code.
- A functional receive marker does not establish latency or zero-copy behavior.
- Historical test counts and measurements are context, not acceptance criteria
  for a new checkout.

## Common commands

```bash
cmake --preset minimal
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
python3 -m pytest src/tools/ibmw_gen/tests
bash scripts/check_red_lines.sh
```

Not every build exposes every command or target. Use the preset and focused test
target appropriate to the feature, then report exactly what ran. For benchmark
work, follow [Transport Benchmark](../guides/transport-benchmark.md).

## TypeSupport consumers

These standalone regressions create their build and install trees under `/tmp`:

```bash
# Default non-ROS SDK export, build-tree consumer, and relocated installed consumer.
bash scripts/tests/test_json_sdk_export.sh
# Also exercise the upstream JSON export and propagated diagnostic settings.
bash scripts/tests/test_json_sdk_export.sh -DJSON_Install=ON -DJSON_Diagnostics=ON
# DDS inheritance/name collisions; optionally include an imported ROS2 service.
bash scripts/tests/test_text_typesupport_consumers.sh -DCMAKE_PREFIX_PATH=<installed-sdk>
bash scripts/tests/test_text_typesupport_consumers.sh -DCMAKE_PREFIX_PATH=<installed-sdk> -DIBMW_TEST_ROS2_SERVICE=ON
```

The SDK export script accepts additional CMake arguments for local dependency
sources. The codec consumer requires Fast-DDS-Gen and the SDK's matching DDS
libraries; the optional service case also requires ROS2 `example_interfaces`.
Discovery exception tests run with the Core tests. DDS ownership reconnection
and Zenoh auto-record schema tests join CTest when their transports are enabled
(the latter also requires recording). They assert received samples and stored
schema metadata, respectively.
