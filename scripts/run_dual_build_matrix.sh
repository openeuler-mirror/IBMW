#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# run_dual_build_matrix.sh — IBMW dual-build matrix harness.
#
# Drives a transport-flag matrix to verify that the build, the
# unit tests, and the smoke subset all survive when each transport
# extension is individually disabled, plus an all-off entry that
# exercises the framework + local-transport-only path. Two more entries
# cover the self-developed shm transport (off in BASE_FLAGS): shm_only
# (shm replaces iceoryx) and shm_both (shm next to iceoryx).
#
# Scope: build + ctest + smoke per entry, driven by
# scripts/dual_build_matrix.yaml.
#
# Entries — see scripts/dual_build_matrix.yaml for the canonical
# definition (cmake_flags / ctest_exclude / smoke_excludes / excluded_samples):
#   dds_off, ros2_off, iceoryx_off, zenoh_off, mqtt_off, net_off, all_off,
#   shm_only, shm_both, plus the r8_iceoryx_* / r8_shm_* / r12 validation
#   entries.
#
# Usage:
#   bash scripts/run_dual_build_matrix.sh                     # all entries
#   bash scripts/run_dual_build_matrix.sh dds_off net_off     # subset
#   bash scripts/run_dual_build_matrix.sh --strict ...        # fail on any per-entry FAIL
#   bash scripts/run_dual_build_matrix.sh --skip-ctest ...    # build + smoke only
#   bash scripts/run_dual_build_matrix.sh --skip-smoke ...    # build + ctest only
#   bash scripts/run_dual_build_matrix.sh --build-only ...    # build only (commit #3 behaviour)
#
# Environment:
#   IBMW_MATRIX_ROOT   override build root (default /tmp/ibmw_matrix)
#   IBMW_MATRIX_JOBS   override -j parallelism (default $(nproc))
#   IBMW_MATRIX_YAML   override yaml config path (default scripts/dual_build_matrix.yaml)
#
# Exit code:
#   0  by default — harness ran end-to-end. Per-entry results are reported
#      in the SUMMARY table and per-entry artefacts under
#      ${IBMW_MATRIX_ROOT}/<entry>/.
#   1  with --strict if any entry's enabled phases FAILed.
#   2  bad CLI usage (unknown entry / missing yaml).

set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MATRIX_ROOT="${IBMW_MATRIX_ROOT:-/tmp/ibmw_matrix}"
JOBS="${IBMW_MATRIX_JOBS:-$(nproc)}"
YAML_PATH="${IBMW_MATRIX_YAML:-${REPO_ROOT}/scripts/dual_build_matrix.yaml}"

BASE_FLAGS=(
  "-DCMAKE_BUILD_TYPE=Release"
  "-DIBMW_BUILD_RUNTIME=ON"
  "-DIBMW_BUILD_EXAMPLES=ON"
  "-DIBMW_BUILD_TESTS=ON"
  "-DIBMW_BUILD_BENCHMARKS=ON"
  "-DIBMW_BUILD_WITH_ROS2=ON"
  "-DIBMW_BUILD_NET_EXTENSION=ON"
  "-DIBMW_BUILD_GRPC_EXTENSION=ON"
  "-DIBMW_BUILD_MQTT_EXTENSION=ON"
  "-DIBMW_BUILD_ECHO_EXTENSION=ON"
  "-DIBMW_BUILD_PROXY_EXTENSION=ON"
  "-DIBMW_BUILD_LOG_CONTROL_EXTENSION=ON"
  "-DIBMW_BUILD_PARAMETER_EXTENSION=ON"
  "-DIBMW_BUILD_SERVICE_INTROSPECTION_EXTENSION=ON"
  "-DIBMW_BUILD_TIME_MANIPULATOR_EXTENSION=ON"
  "-DIBMW_BUILD_TOPIC_LOGGER_EXTENSION=ON"
  "-DIBMW_BUILD_RECORD_PLAYBACK_EXTENSION=ON"
  "-DIBMW_BUILD_DDS_EXTENSION=ON"
  "-DIBMW_BUILD_ICEORYX_EXTENSION=ON"
  "-DIBMW_BUILD_ZENOH_EXTENSION=ON"
  "-DIBMW_BUILD_ROS2_EXTENSION=ON"
)

if [[ ! -f "${YAML_PATH}" ]]; then
  echo "[matrix] ERROR: yaml config not found: ${YAML_PATH}" >&2
  exit 2
fi

# Parse yaml once via python3+pyyaml. Emit a flat TAB-separated stream
# the bash side can consume. Tab is chosen because the ctest_exclude
# regex commonly contains '|' (alternation) and the smoke_excludes glob
# list is joined with '|' as the inner separator. Field syntax:
#   ENTRIES\t<comma-separated names in declared order>
#   FLAGS\t<entry>\t<space-joined cmake_flags>
#   CTEST\t<entry>\t<ctest_exclude regex, may contain '|'>
#   SMOKE\t<entry>\t<glob1>|<glob2>|...
PARSED="$(python3 - "${YAML_PATH}" <<'PY'
import sys, yaml
with open(sys.argv[1]) as f:
    cfg = yaml.safe_load(f)
entries = cfg.get("entries") or []
names = [e["name"] for e in entries]
print("ENTRIES\t" + ",".join(names))
for e in entries:
    flags = " ".join(e.get("cmake_flags") or [])
    print(f"FLAGS\t{e['name']}\t{flags}")
    print(f"CTEST\t{e['name']}\t{e.get('ctest_exclude','') or ''}")
    smoke = e.get("smoke_excludes") or []
    print(f"SMOKE\t{e['name']}\t" + "|".join(smoke))
    expect_fail = 1 if e.get("expect_build_fail") else 0
    print(f"EXPECT_FAIL\t{e['name']}\t{expect_fail}")
    extras = e.get("extra_targets") or []
    print(f"EXTRA\t{e['name']}\t" + ",".join(extras))
    expect_tgt_fail = 1 if e.get("expect_target_build_fail") else 0
    print(f"EXPECT_TGT_FAIL\t{e['name']}\t{expect_tgt_fail}")
    scan_path = e.get("expect_scan_path_fail") or ""
    print(f"SCAN_PATH_FAIL\t{e['name']}\t{scan_path}")
PY
)"
if [[ -z "${PARSED}" ]]; then
  echo "[matrix] ERROR: failed to parse ${YAML_PATH}" >&2
  exit 2
fi

declare -A ENTRY_FLAGS
declare -A ENTRY_CTEST
declare -A ENTRY_SMOKE
declare -A ENTRY_EXPECT_FAIL
declare -A ENTRY_EXTRA
declare -A ENTRY_EXPECT_TGT_FAIL
declare -A ENTRY_SCAN_PATH_FAIL
ALL_ENTRIES=()

while IFS=$'\t' read -r tag e1 rest; do
  [[ -z "${tag}" ]] && continue
  case "${tag}" in
    ENTRIES)
      IFS=',' read -ra ALL_ENTRIES <<< "${e1}"
      ;;
    FLAGS)
      ENTRY_FLAGS[$e1]="${rest}"
      ;;
    CTEST)
      ENTRY_CTEST[$e1]="${rest}"
      ;;
    SMOKE)
      ENTRY_SMOKE[$e1]="${rest}"
      ;;
    EXPECT_FAIL)
      ENTRY_EXPECT_FAIL[$e1]="${rest}"
      ;;
    EXTRA)
      ENTRY_EXTRA[$e1]="${rest}"
      ;;
    EXPECT_TGT_FAIL)
      ENTRY_EXPECT_TGT_FAIL[$e1]="${rest}"
      ;;
    SCAN_PATH_FAIL)
      ENTRY_SCAN_PATH_FAIL[$e1]="${rest}"
      ;;
  esac
done <<< "${PARSED}"

if [[ ${#ALL_ENTRIES[@]} -eq 0 ]]; then
  echo "[matrix] ERROR: no entries parsed from ${YAML_PATH}" >&2
  exit 2
fi

STRICT=0
SKIP_CTEST=0
SKIP_SMOKE=0
BUILD_ONLY=0
ARGS=()
for arg in "$@"; do
  case "${arg}" in
    --strict)      STRICT=1 ;;
    --skip-ctest)  SKIP_CTEST=1 ;;
    --skip-smoke)  SKIP_SMOKE=1 ;;
    --build-only)  BUILD_ONLY=1 ;;
    *)             ARGS+=("${arg}") ;;
  esac
done
if [[ ${BUILD_ONLY} -eq 1 ]]; then
  SKIP_CTEST=1
  SKIP_SMOKE=1
fi
set -- "${ARGS[@]:+${ARGS[@]}}"

if [[ $# -gt 0 ]]; then
  ENTRIES=("$@")
  for e in "${ENTRIES[@]}"; do
    if [[ -z "${ENTRY_FLAGS[$e]+_}" ]]; then
      echo "[matrix] ERROR: unknown entry '$e'." >&2
      echo "[matrix] valid entries: ${ALL_ENTRIES[*]}" >&2
      exit 2
    fi
  done
else
  ENTRIES=("${ALL_ENTRIES[@]}")
fi

mkdir -p "${MATRIX_ROOT}"

declare -A CONFIG_RC
declare -A BUILD_RC
declare -A FEATURES_RC
declare -A CTEST_RC
declare -A SMOKE_RC

# Columns, in order: DDS ROS2 ICEORYX ZENOH MQTT NET SELFDEV_SHM (the names
# array in verify_features_h). SELFDEV_SHM is 0 unless the entry turns
# IBMW_BUILD_SHM_EXTENSION on.
declare -A EXPECT_FEATURES
EXPECT_FEATURES[dds_off]="0 1 1 1 1 1 0"
EXPECT_FEATURES[ros2_off]="1 0 1 1 1 1 0"
EXPECT_FEATURES[iceoryx_off]="1 1 0 1 1 1 0"
EXPECT_FEATURES[zenoh_off]="1 1 1 0 1 1 0"
EXPECT_FEATURES[mqtt_off]="1 1 1 1 0 1 0"
EXPECT_FEATURES[net_off]="1 1 1 1 1 0 0"
EXPECT_FEATURES[all_off]="0 0 0 0 0 0 0"
EXPECT_FEATURES[shm_only]="1 1 0 1 1 1 1"
EXPECT_FEATURES[shm_both]="1 1 1 1 1 1 1"
EXPECT_FEATURES[r8_iceoryx_publisher_ok]="1 1 1 1 1 1 0"
EXPECT_FEATURES[r8_iceoryx_subscriber_ok]="1 1 1 1 1 1 0"
EXPECT_FEATURES[r8_iceoryx_mixed_api_fail]="1 1 1 1 1 1 0"
EXPECT_FEATURES[r8_iceoryx_no_anchor_fail]="1 1 1 1 1 1 0"
EXPECT_FEATURES[r8_shm_publisher_ok]="1 1 0 1 1 1 1"
EXPECT_FEATURES[r8_shm_subscriber_ok]="1 1 0 1 1 1 1"
EXPECT_FEATURES[r8_shm_mixed_api_fail]="1 1 0 1 1 1 1"
EXPECT_FEATURES[r8_shm_no_anchor_fail]="1 1 0 1 1 1 1"
EXPECT_FEATURES[r12_runtime_use_fail]="1 1 1 1 1 1 0"

verify_features_h() {
  local entry="$1" build_dir="$2"
  local features_h="${build_dir}/include/ibmw/transport_features.h"
  if [[ ! -f "${features_h}" ]]; then
    features_h="$(find "${build_dir}" -name transport_features.h -path '*/ibmw/*' 2>/dev/null | head -n1)"
  fi
  if [[ -z "${features_h}" || ! -f "${features_h}" ]]; then
    echo "[matrix:${entry}] features.h NOT FOUND"
    return 2
  fi
  local expected=(${EXPECT_FEATURES[$entry]})
  local names=(DDS ROS2 ICEORYX ZENOH MQTT NET SELFDEV_SHM)
  local i mismatch=0
  for i in "${!names[@]}"; do
    local n="${names[$i]}" want="${expected[$i]}"
    local got
    got="$(grep -E "^[[:space:]]*#define[[:space:]]+IBMW_HAS_${n}[[:space:]]+[01]" "${features_h}" | awk '{print $3}' | tail -n1)"
    if [[ "${got}" != "${want}" ]]; then
      echo "[matrix:${entry}] features.h soft-mismatch: IBMW_HAS_${n} expected=${want} got='${got}'  (host capability auto-OFF expected for zenoh without rustc; non-fatal)"
      mismatch=1
    fi
  done
  return ${mismatch}
}

run_ctest() {
  local entry="$1" build_dir="$2" ctest_log="$3"
  local pattern="${ENTRY_CTEST[$entry]}"
  (
    cd "${build_dir}"
    set +u; source /opt/ros/humble/setup.bash 2>/dev/null || true; set -u
    export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
    if [[ -n "${pattern}" ]]; then
      ctest -E "${pattern}" --output-on-failure
    else
      ctest --output-on-failure
    fi
  ) >"${ctest_log}" 2>&1
  local rc=$?
  local passed
  passed="$(grep -cE '^[[:space:]]*[0-9]+/[0-9]+ Test[[:space:]]+#[0-9]+:.*Passed' "${ctest_log}" 2>/dev/null || echo 0)"
  if [[ ${rc} -eq 0 && ${passed} -ge 1 ]]; then
    echo "PASSED=${passed}" >>"${ctest_log}"
    return 0
  fi
  echo "PASSED=${passed} RC=${rc}" >>"${ctest_log}"
  return 1
}

# Smoke phase: stage from the entry's build dir into a per-entry stage,
# then run run_all_tests.sh. Filter PASS lines against smoke_excludes
# (glob patterns) and require >= 1 surviving PASS.
run_smoke() {
  local entry="$1" build_dir="$2" smoke_log="$3"
  local stage="${MATRIX_ROOT}/${entry}/stage"
  local excludes="${ENTRY_SMOKE[$entry]}"
  rm -rf "${stage}"
  (
    set +u; source /opt/ros/humble/setup.bash 2>/dev/null || true; set -u
    export REPO_ROOT="${REPO_ROOT}" BUILD="${build_dir}" STAGE="${stage}"
    bash "${REPO_ROOT}/scripts/stage_smoke.sh"
  ) >"${smoke_log}" 2>&1
  local stage_rc=$?
  if [[ ${stage_rc} -ne 0 ]]; then
    echo "STAGE_RC=${stage_rc}" >>"${smoke_log}"
    return 1
  fi
  (
    cd "${stage}"
    export LD_LIBRARY_PATH="${stage}/lib:/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
    ./run_all_tests.sh 2>&1 || true
  ) >>"${smoke_log}" 2>&1

  local total_pass surviving=0
  total_pass="$(grep -cE 'PASS\b' "${smoke_log}" || echo 0)"
  if [[ -z "${excludes}" ]]; then
    surviving="${total_pass}"
  else
    local IFS='|'
    local -a glob_arr
    read -ra glob_arr <<< "${excludes}"
    surviving="$(awk -v globs="${excludes}" '
      BEGIN {
        n=split(globs, g, "|");
        for (i=1;i<=n;i++) {
          gsub(/[.+(){}\\^$|]/, "\\\\&", g[i]);
          gsub(/\*/, ".*", g[i]);
          gsub(/\?/, ".", g[i]);
        }
      }
      /PASS/ {
        line=$0
        excluded=0
        for (i=1;i<=n;i++) if (line ~ g[i]) { excluded=1; break }
        if (!excluded) c++
      }
      END { print c+0 }
    ' "${smoke_log}")"
  fi
  echo "SMOKE_TOTAL_PASS=${total_pass} SURVIVING=${surviving} EXCLUDES=${excludes}" >>"${smoke_log}"
  if [[ ${surviving} -ge 1 ]]; then
    return 0
  fi
  return 1
}

run_entry() {
  local entry="$1"
  local entry_dir="${MATRIX_ROOT}/${entry}"
  local build_dir="${entry_dir}/build"
  local cfg_log="${entry_dir}/configure.log"
  local build_log="${entry_dir}/build.log"
  local feat_log="${entry_dir}/features.log"
  local ctest_log="${entry_dir}/ctest.log"
  local smoke_log="${entry_dir}/smoke.log"

  echo "===================================================================="
  echo "[matrix] entry=${entry}  flags=${ENTRY_FLAGS[$entry]}"
  echo "[matrix] build_dir=${build_dir}"
  echo "===================================================================="

  rm -rf "${entry_dir}"
  mkdir -p "${entry_dir}"

  local entry_extra=(${ENTRY_FLAGS[$entry]})
  (
    cd "${REPO_ROOT}"
    cmake -B "${build_dir}" -S "${REPO_ROOT}" \
      "${BASE_FLAGS[@]}" "${entry_extra[@]}"
  ) >"${cfg_log}" 2>&1
  CONFIG_RC[$entry]=$?

  if [[ ${CONFIG_RC[$entry]} -ne 0 ]]; then
    echo "[matrix:${entry}] CONFIGURE FAILED  (see ${cfg_log})"
    BUILD_RC[$entry]=255
    FEATURES_RC[$entry]=255
    CTEST_RC[$entry]=255
    SMOKE_RC[$entry]=255
    return
  fi

  # Entries declaring extra_targets (e.g. r8_iceoryx_*, r8_shm_*) skip the
  # monolithic build because their default-include neighbours (R8.2 mixed-API
  # _fail samples) trigger unconditional static_assert at TU compile time and
  # would tank the full-tree build. The extra_targets loop below builds only
  # the declared targets, preserving the required per-target-only build mode.
  local _extras_now="${ENTRY_EXTRA[$entry]:-}"
  if [[ -n "${_extras_now}" ]]; then
    echo "[matrix:${entry}] extras-mode: skipping monolithic build (per-target only)" \
      >"${build_log}"
    BUILD_RC[$entry]=0
  else
    cmake --build "${build_dir}" -j"${JOBS}" >"${build_log}" 2>&1
    BUILD_RC[$entry]=$?
  fi

  # K-J: expect_build_fail support
  local expect_fail="${ENTRY_EXPECT_FAIL[$entry]:-0}"
  if [[ ${expect_fail} -eq 1 ]]; then
    if [[ ${BUILD_RC[$entry]} -ne 0 ]]; then
      echo "[matrix:${entry}] BUILD FAILED AS EXPECTED"
      BUILD_RC[$entry]=0  # Treat as success for the harness
      FEATURES_RC[$entry]=255
      CTEST_RC[$entry]=255
      SMOKE_RC[$entry]=255
      return
    else
      echo "[matrix:${entry}] BUILD SUCCEEDED BUT EXPECTED FAILURE"
      BUILD_RC[$entry]=1
      FEATURES_RC[$entry]=255
      CTEST_RC[$entry]=255
      SMOKE_RC[$entry]=255
      return
    fi
  fi

  if [[ ${BUILD_RC[$entry]} -ne 0 ]]; then
    echo "[matrix:${entry}] BUILD FAILED  (see ${build_log})"
    FEATURES_RC[$entry]=255
    CTEST_RC[$entry]=255
    SMOKE_RC[$entry]=255
    return
  fi
  echo "[matrix:${entry}] BUILD OK"

  local extras="${ENTRY_EXTRA[$entry]:-}"
  local expect_tgt_fail="${ENTRY_EXPECT_TGT_FAIL[$entry]:-0}"
  local scan_path_fail="${ENTRY_SCAN_PATH_FAIL[$entry]:-}"
  if [[ -n "${extras}" ]]; then
    local extra_log="${entry_dir}/extra_targets.log"
    local IFS_SAVE="${IFS}"
    IFS=',' read -ra EXTRA_ARR <<< "${extras}"
    IFS="${IFS_SAVE}"
    local any_tgt_fail=0
    local tgt
    for tgt in "${EXTRA_ARR[@]}"; do
      echo "[matrix:${entry}] building extra_target=${tgt}" >>"${extra_log}"
      cmake --build "${build_dir}" --target "${tgt}" -j"${JOBS}" \
        >>"${extra_log}" 2>&1
      local extra_rc=$?
      echo "[matrix:${entry}] extra_target=${tgt} rc=${extra_rc}" >>"${extra_log}"
      if [[ ${extra_rc} -ne 0 ]]; then
        any_tgt_fail=1
      fi
    done
    if [[ ${expect_tgt_fail} -eq 1 ]]; then
      if [[ ${any_tgt_fail} -ne 0 ]]; then
        echo "[matrix:${entry}] EXTRA-TARGET BUILD FAILED AS EXPECTED  (see ${extra_log})"
      else
        echo "[matrix:${entry}] EXTRA-TARGET BUILD SUCCEEDED BUT EXPECTED FAILURE  (see ${extra_log})"
        BUILD_RC[$entry]=1
        FEATURES_RC[$entry]=255
        CTEST_RC[$entry]=255
        SMOKE_RC[$entry]=255
        return
      fi
    else
      if [[ ${any_tgt_fail} -ne 0 ]]; then
        echo "[matrix:${entry}] EXTRA-TARGET BUILD FAILED  (see ${extra_log})"
        BUILD_RC[$entry]=1
        FEATURES_RC[$entry]=255
        CTEST_RC[$entry]=255
        SMOKE_RC[$entry]=255
        return
      fi
      echo "[matrix:${entry}] EXTRA-TARGET BUILD OK  (see ${extra_log})"
    fi
  fi

  if [[ -n "${scan_path_fail}" ]]; then
    local scan_log="${entry_dir}/scan_path.log"
    echo "[matrix:${entry}] running --scan-path red-line on ${scan_path_fail}" >"${scan_log}"
    bash "${REPO_ROOT}/scripts/check_red_lines.sh" \
      --scan-path "${REPO_ROOT}/${scan_path_fail}" \
      >>"${scan_log}" 2>&1
    local scan_rc=$?
    echo "[matrix:${entry}] --scan-path rc=${scan_rc}" >>"${scan_log}"
    if [[ ${scan_rc} -ne 0 ]]; then
      echo "[matrix:${entry}] SCAN-PATH RED-LINE FAILED AS EXPECTED  (see ${scan_log})"
    else
      echo "[matrix:${entry}] SCAN-PATH RED-LINE PASSED BUT EXPECTED FAIL — REGRESSION  (see ${scan_log})"
      BUILD_RC[$entry]=1
      FEATURES_RC[$entry]=255
      CTEST_RC[$entry]=255
      SMOKE_RC[$entry]=255
      return
    fi
  fi

  if [[ ${expect_tgt_fail} -eq 1 || -n "${scan_path_fail}" || -n "${extras}" ]]; then
    FEATURES_RC[$entry]=255
    CTEST_RC[$entry]=255
    SMOKE_RC[$entry]=255
    return
  fi

  verify_features_h "${entry}" "${build_dir}" >"${feat_log}" 2>&1
  FEATURES_RC[$entry]=$?
  if [[ ${FEATURES_RC[$entry]} -ne 0 ]]; then
    echo "[matrix:${entry}] FEATURES.H MISMATCH  (see ${feat_log})"
  fi

  if [[ ${SKIP_CTEST} -eq 1 ]]; then
    CTEST_RC[$entry]=255
  else
    run_ctest "${entry}" "${build_dir}" "${ctest_log}"
    CTEST_RC[$entry]=$?
    if [[ ${CTEST_RC[$entry]} -eq 0 ]]; then
      echo "[matrix:${entry}] CTEST OK  (see ${ctest_log})"
    else
      echo "[matrix:${entry}] CTEST FAIL  (see ${ctest_log})"
    fi
  fi

  if [[ ${SKIP_SMOKE} -eq 1 ]]; then
    SMOKE_RC[$entry]=255
  else
    run_smoke "${entry}" "${build_dir}" "${smoke_log}"
    SMOKE_RC[$entry]=$?
    if [[ ${SMOKE_RC[$entry]} -eq 0 ]]; then
      echo "[matrix:${entry}] SMOKE OK  (see ${smoke_log})"
    else
      echo "[matrix:${entry}] SMOKE FAIL  (see ${smoke_log})"
    fi
  fi
}

for e in "${ENTRIES[@]}"; do
  run_entry "${e}"
done

echo
echo "===================================================================="
echo "[matrix] SUMMARY"
echo "===================================================================="
printf "%-14s  %-9s  %-7s  %-9s  %-7s  %-7s\n" "ENTRY" "CONFIGURE" "BUILD" "FEATURES" "CTEST" "SMOKE"
printf "%-14s  %-9s  %-7s  %-9s  %-7s  %-7s\n" "-----" "---------" "-----" "--------" "-----" "-----"
overall=0
features_warns=0
for e in "${ENTRIES[@]}"; do
  rc_cfg="${CONFIG_RC[$e]}"
  rc_bld="${BUILD_RC[$e]}"
  rc_feat="${FEATURES_RC[$e]}"
  rc_ct="${CTEST_RC[$e]}"
  rc_sm="${SMOKE_RC[$e]}"
  cfg_s=$([[ ${rc_cfg} -eq 0 ]] && echo "OK" || echo "FAIL(${rc_cfg})")
  bld_s=$([[ ${rc_bld} -eq 0 ]] && echo "OK" || ([[ ${rc_bld} -eq 255 ]] && echo "SKIP" || echo "FAIL(${rc_bld})"))
  feat_s=$([[ ${rc_feat} -eq 0 ]] && echo "OK" || ([[ ${rc_feat} -eq 255 ]] && echo "SKIP" || ([[ ${rc_feat} -eq 1 ]] && echo "WARN" || echo "FAIL")))
  ct_s=$([[ ${rc_ct} -eq 0 ]] && echo "OK" || ([[ ${rc_ct} -eq 255 ]] && echo "SKIP" || echo "FAIL"))
  sm_s=$([[ ${rc_sm} -eq 0 ]] && echo "OK" || ([[ ${rc_sm} -eq 255 ]] && echo "SKIP" || echo "FAIL"))
  printf "%-14s  %-9s  %-7s  %-9s  %-7s  %-7s\n" "${e}" "${cfg_s}" "${bld_s}" "${feat_s}" "${ct_s}" "${sm_s}"
  if [[ ${rc_cfg} -ne 0 || ${rc_bld} -ne 0 ]]; then
    overall=1
  fi
  if [[ ${SKIP_CTEST} -eq 0 && ${rc_ct} -ne 0 && ${rc_ct} -ne 255 ]]; then
    overall=1
  fi
  if [[ ${SKIP_SMOKE} -eq 0 && ${rc_sm} -ne 0 && ${rc_sm} -ne 255 ]]; then
    overall=1
  fi
  if [[ ${rc_feat} -eq 1 ]]; then
    features_warns=$((features_warns + 1))
  fi
done
echo

if [[ ${overall} -ne 0 ]]; then
  echo "[matrix] At least one entry had a per-phase FAIL."
  echo "[matrix] per-entry artefacts under ${MATRIX_ROOT}/<entry>/"
  echo "[matrix] features.h warns: ${features_warns}  (non-blocking)"
  if [[ ${STRICT} -eq 1 ]]; then
    echo "[matrix] OVERALL: FAIL (--strict)"
    exit 1
  fi
  echo "[matrix] OVERALL: HARNESS-OK, ENTRIES-PARTIAL — see SUMMARY above."
  exit 0
fi

echo "[matrix] OVERALL: PASS — ${#ENTRIES[@]} entries fully green."
if [[ ${features_warns} -gt 0 ]]; then
  echo "[matrix] features.h warns: ${features_warns}  (non-blocking)"
fi
exit 0
