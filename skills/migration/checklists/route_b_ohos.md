Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Route B / OHOS Checklist

- [ ] OHOS target is in scope and recorded:
  `rg -n 'OHOS|Route B|route_b' reports/migration_report.md`
- [ ] Execution location is recorded:
  `rg -n 'inside_route_b_container|outside_route_b_container' reports/migration_report.md reports/route_b_ohos.md`
- [ ] If running inside Route B container, container-visible mounts are recorded:
  `rg -n 'container|image|mount|workspace|SDK|sysroot|prefix' reports/route_b_ohos.md`
- [ ] Target prefixes are explicit, not host ROS:
  `rg -n 'IBMW_OHOS_ROS2_PREFIX_LIST|IBMW_OHOS_SDK_NATIVE_ROOT' reports/route_b_ohos.md`
- [ ] Final build directory is fresh/unique. If a cache is reused,
  `CMAKE_HOME_DIRECTORY` exactly matches the IBMW repo root and the recorded
  `CMAKE_GENERATOR` is unchanged; no generator switch was attempted.
- [ ] Configure/build environment clears `CMAKE_PREFIX_PATH`,
  `COLCON_PREFIX_PATH`, and `ROS_PACKAGE_PATH`, while explicit target
  `AMENT_PREFIX_PATH` and `PYTHONPATH` are recorded.
- [ ] FastDDS/Fast-CDR provider is explicit and comes from target ROS2 when available:
  `rg -n 'FastDDS|Fast-CDR|fastcdr|fastrtps|ROS2 target prefix|IBMW_OHOS_ROS2_PREFIX_LIST' reports/route_b_ohos.md`
- [ ] For each dynamic/large ROS2 topic, the target FastDDS lane's registered
  serialized type ceiling is recorded and covers the application's payload
  budget; host capacity is not assumed to prove target capacity.
- [ ] Any required capacity increase is implemented in app-local type support
  for the affected type and verified in the target artifact/configuration; the
  generic global ceiling was not raised for one application.
- [ ] A representative maximum-size target sample was received and validated by
  the intended external ROS2/DDS observer; the target writer and observer
  receive log paths are recorded in `reports/route_b_ohos.md`, and endpoint
  discovery alone is not accepted as data-plane evidence.
- [ ] No unreviewed bundled/newer Fast-CDR override is used. If `_deps`,
  `FetchContent`, or `bundled` appears, the report must also say `reviewed exception`:
  `rg -n '_deps|FetchContent|bundled|high-version|newer Fast|reviewed exception' reports/route_b_ohos.md`
- [ ] Route B command log exists:
  `test -f reports/route_b_ohos.md`
- [ ] Target artifact audit JSON exists:
  `test -f <route_b_build>/logs/route_b_target_artifact_audit.json`
- [ ] Audit JSON parses:
  `python3 -m json.tool <route_b_build>/logs/route_b_target_artifact_audit.json >/dev/null`
- [ ] Audit status and arch are PASS/aarch64:

  ```bash
  python3 - <<'PY'
  import json
  p = '<route_b_build>/logs/route_b_target_artifact_audit.json'
  d = json.load(open(p))
  assert d.get('status') == 'pass'
  assert d.get('expected_arch') == 'aarch64'
  assert all(a.get('arch') == 'aarch64' for a in d.get('artifacts', []))
  PY
  ```

- [ ] Audited target ELFs have no absolute `DT_NEEDED` path:
  `bash scripts/check_ohos_route_b_link_boundary.sh <route_b_build>`
- [ ] Target `libfastrtps` and `libfastcdr` realpaths and SONAMEs are recorded,
  and `readelf -d <libfastrtps-realpath>` shows its FastCDR `DT_NEEDED` exactly
  matches the selected `libfastcdr` SONAME.
- [ ] Install prefix was clean before install, and a separate clean external
  consumer configured and built with `find_package(ibmw CONFIG REQUIRED)`.
- [ ] Installed `ibmw-config.cmake` resolves target ROS dependencies without any
  Route B build-tree path; consumer link metadata has no absolute `DT_NEEDED`.
- [ ] Report does not claim board runtime unless separately proven:
  `rg -n 'board runtime|device runtime' reports/migration_report.md || true`
- [ ] If board runtime is claimed, `/data/<case>` logs are copied into evidence
  and include the case-specific PASS marker.
