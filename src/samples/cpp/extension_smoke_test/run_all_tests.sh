#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# IBMW Comprehensive Integration / Smoke Test Script
# Runs all pkg-mode configs, standalone apps, and extension smoke tests.
#
# Usage:
#   ./run_all_tests.sh              # Run all tests
#   ./run_all_tests.sh --quick      # Skip DDS/ROS2 tests (faster)
#   ./run_all_tests.sh --ext-only   # Only extension smoke tests
#
# Exit codes 0/124/130/137 from timeout are treated as PASS.
# Demos don't self-terminate; timeout -s KILL is used.

set -eo pipefail

###############################################################################
# Configuration
###############################################################################
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}"
TIMEOUT_SEC=8
KILL_SIGNAL="KILL"

# Source ROS2 if available (disable nounset temporarily — ROS2 scripts use unset vars)
set +u
if [ -f /opt/ros/humble/setup.bash ]; then
    source /opt/ros/humble/setup.bash
fi
set -u
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:/opt/ros/humble/lib:${LD_LIBRARY_PATH:-}"

###############################################################################
# Counters
###############################################################################
PASS=0
FAIL=0
SKIP=0
TOTAL=0
FAILURES=()

###############################################################################
# Helpers
###############################################################################
COLOR_GREEN="\033[0;32m"
COLOR_RED="\033[0;31m"
COLOR_YELLOW="\033[1;33m"
COLOR_RESET="\033[0m"

run_test() {
    local name="$1"
    shift
    local cmd=("$@")
    TOTAL=$((TOTAL + 1))

    printf "  [%3d] %-60s " "$TOTAL" "$name"

    local exit_code=0
    timeout -s "$KILL_SIGNAL" "$TIMEOUT_SEC" "${cmd[@]}" >/dev/null 2>&1 || exit_code=$?

    # 0=clean exit, 124=timeout(TERM), 130=SIGINT, 137=SIGKILL
    if [[ "$exit_code" == 0 || "$exit_code" == 124 || "$exit_code" == 130 || "$exit_code" == 137 ]]; then
        printf "${COLOR_GREEN}PASS${COLOR_RESET} (exit %d)\n" "$exit_code"
        PASS=$((PASS + 1))
    else
        printf "${COLOR_RED}FAIL${COLOR_RESET} (exit %d)\n" "$exit_code"
        FAIL=$((FAIL + 1))
        FAILURES+=("$name (exit $exit_code)")
    fi
}

run_strict_test() {
    local name="$1"
    local timeout_sec="$2"
    shift 2
    local cmd=("$@")
    TOTAL=$((TOTAL + 1))

    printf "  [%3d] %-60s " "$TOTAL" "$name"

    local exit_code=0
    local log_dir="${SCRIPT_DIR}/strict-test-logs"
    mkdir -p "$log_dir"
    local log_file="${log_dir}/$(printf '%s' "$name" | tr ' /:' '___').log"
    timeout -s "$KILL_SIGNAL" "$timeout_sec" "${cmd[@]}" >"$log_file" 2>&1 || exit_code=$?

    if [[ "$exit_code" == 0 ]]; then
        printf "${COLOR_GREEN}PASS${COLOR_RESET} (exit %d)\n" "$exit_code"
        PASS=$((PASS + 1))
    else
        printf "${COLOR_RED}FAIL${COLOR_RESET} (exit %d, log %s)\n" "$exit_code" "$log_file"
        FAIL=$((FAIL + 1))
        FAILURES+=("$name (exit $exit_code)")
    fi
}

skip_test() {
    local name="$1"
    TOTAL=$((TOTAL + 1))
    SKIP=$((SKIP + 1))
    printf "  [%3d] %-60s ${COLOR_YELLOW}SKIP${COLOR_RESET}\n" "$TOTAL" "$name"
}

section() {
    echo ""
    echo "========================================================================"
    echo "  $1"
    echo "========================================================================"
}

###############################################################################
# Parse arguments
###############################################################################
QUICK=false
EXT_ONLY=false
for arg in "$@"; do
    case "$arg" in
        --quick)    QUICK=true ;;
        --ext-only) EXT_ONLY=true ;;
        --help|-h)
            echo "Usage: $0 [--quick] [--ext-only]"
            echo "  --quick     Skip DDS/ROS2 tests"
            echo "  --ext-only  Only run extension smoke tests"
            exit 0
            ;;
    esac
done

cd "$BUILD_DIR"

echo "IBMW Comprehensive Test Suite"
echo "Build dir:  $BUILD_DIR"
echo "Timeout:    ${TIMEOUT_SEC}s"
echo "Quick mode: $QUICK"
echo "Ext-only:   $EXT_ONLY"
echo "Started at: $(date)"

###############################################################################
# Section 1: Extension Smoke Tests (NEW)
###############################################################################
section "Extension Smoke Tests"

EXT_CONFIGS=(
    ext_echo_driver_cfg.yaml
    ext_net_transport_cfg.yaml
    ext_net_http_pubsub_cfg.yaml
    ext_net_udp_pubsub_cfg.yaml
    ext_grpc_transport_cfg.yaml
    ext_grpc_rpc_cfg.yaml
    ext_mqtt_transport_cfg.yaml
    ext_zenoh_transport_cfg.yaml
    ext_log_control_cfg.yaml
    ext_parameter_service_cfg.yaml
    ext_time_manipulator_cfg.yaml
    ext_topic_logger_cfg.yaml
    ext_service_introspection_cfg.yaml
    ext_record_playback_cfg.yaml
    ext_proxy_driver_cfg.yaml
    ext_parallel_executor_cfg.yaml
)

for cfg in "${EXT_CONFIGS[@]}"; do
    if [ -f "cfg/$cfg" ]; then
        run_test "ext: $cfg" ./bin/ibmw run "cfg/$cfg"
    else
        skip_test "ext: $cfg (missing)"
    fi
done

if [ -f "./record_smoke_required" ] && $QUICK; then
    skip_test "ext: bag_record_transport_and_cli_matrix (--quick)"
elif [ -f "./record_smoke_required" ] && [ ! -x "./validate_record_smoke.sh" ]; then
    run_strict_test "ext: bag_record_transport_and_cli_matrix (required artifact missing)" 1 false
elif ! $QUICK && [ -x "./validate_record_smoke.sh" ]; then
    run_strict_test "ext: bag_record_transport_and_cli_matrix" 300 \
        env BUILD_DIR="$BUILD_DIR" CFG_DIR="$BUILD_DIR/cfg" TMP_DIR="$BUILD_DIR/strict-test-logs" \
        ./validate_record_smoke.sh
else
    skip_test "ext: bag_record_transport_and_cli_matrix (missing or --quick)"
fi

if [ -f "./info_smoke_required" ] && $QUICK; then
    skip_test "ext: bag_info_cli_and_metadata_matrix (--quick)"
elif [ -f "./info_smoke_required" ] && [ ! -x "./validate_info_smoke.sh" ]; then
    run_strict_test "ext: bag_info_cli_and_metadata_matrix (required artifact missing)" 1 false
elif ! $QUICK && [ -x "./validate_info_smoke.sh" ]; then
    run_strict_test "ext: bag_info_cli_and_metadata_matrix" 120 \
        env BUILD_DIR="$BUILD_DIR" BAGS_DIR="$BUILD_DIR/info_smoke_bags" \
        ./validate_info_smoke.sh
else
    skip_test "ext: bag_info_cli_and_metadata_matrix (missing or --quick)"
fi

if [ -f "./convert_smoke_required" ] && $QUICK; then
    skip_test "ext: bag_convert_cli_and_conversion_matrix (--quick)"
elif [ -f "./convert_smoke_required" ] && [ ! -x "./validate_convert_smoke.sh" ]; then
    run_strict_test "ext: bag_convert_cli_and_conversion_matrix (required artifact missing)" 1 false
elif ! $QUICK && [ -x "./validate_convert_smoke.sh" ]; then
    run_strict_test "ext: bag_convert_cli_and_conversion_matrix" 180 \
        env BUILD_DIR="$BUILD_DIR" \
        BAGS_DIR="$BUILD_DIR/convert_smoke_bags" \
        CFG_DIR="$BUILD_DIR/convert_smoke_cfg" \
        TMP_DIR="$BUILD_DIR/strict-test-logs" \
        ./validate_convert_smoke.sh
else
    skip_test "ext: bag_convert_cli_and_conversion_matrix (missing or --quick)"
fi

if [ -f "./play_smoke_required" ] && $QUICK; then
    skip_test "ext: bag_play_transport_and_cli_matrix (--quick)"
elif [ -f "./play_smoke_required" ] && [ ! -x "./validate_play_smoke.sh" ]; then
    run_strict_test "ext: bag_play_transport_and_cli_matrix (required artifact missing)" 1 false
elif ! $QUICK && [ -x "./validate_play_smoke.sh" ]; then
    run_strict_test "ext: bag_play_transport_and_cli_matrix" 300 \
        env BUILD_DIR="$BUILD_DIR" \
        BAGS_DIR="$BUILD_DIR/play_smoke_bags" \
        CFG_DIR="$BUILD_DIR/play_smoke_cfg" \
        TMP_DIR="$BUILD_DIR/strict-test-logs" \
        ./validate_play_smoke.sh
else
    skip_test "ext: bag_play_transport_and_cli_matrix (missing or --quick)"
fi

if $EXT_ONLY; then
    # Jump to summary
    section "Summary"
    echo ""
    echo "Total: $TOTAL  Pass: $PASS  Fail: $FAIL  Skip: $SKIP"
    if [ ${#FAILURES[@]} -gt 0 ]; then
        echo ""
        echo "Failures:"
        for f in "${FAILURES[@]}"; do echo "  - $f"; done
    fi
    exit $FAIL
fi

###############################################################################
# Section 2: Non-DDS/ROS2 Pkg-Mode Configs
###############################################################################
section "Non-DDS/ROS2 Pkg-Mode Configs"

PKG_CONFIGS_BASIC=(
    examples_cpp_helloworld_cfg.yaml
    examples_cpp_executor_cfg.yaml
    examples_cpp_executor_co_cfg.yaml
    examples_cpp_executor_co_loop_cfg.yaml
    examples_cpp_executor_real_time_cfg.yaml
    examples_cpp_executor_timer_cfg.yaml
    examples_cpp_logger_cfg.yaml
    examples_cpp_logger_bench_cfg.yaml
    examples_cpp_logger_format_cfg.yaml
    examples_cpp_logger_rotate_file_cfg.yaml
    examples_cpp_logger_rotate_file_with_sync_cfg.yaml
    examples_cpp_logger_specify_executor_cfg.yaml
    examples_cpp_parameter_cfg.yaml
    pubsub_single_process_cfg.yaml
    publisher_local_cfg.yaml
    subscriber_local_cfg.yaml
    compat_mode_cfg.yaml
    strict_mode_cfg.yaml
)

for cfg in "${PKG_CONFIGS_BASIC[@]}"; do
    if [ -f "cfg/$cfg" ]; then
        run_test "pkg: $cfg" ./bin/ibmw run "cfg/$cfg"
    else
        skip_test "pkg: $cfg (missing)"
    fi
done

###############################################################################
# Section 3: Context Pub/Sub Configs
###############################################################################
section "Context Pub/Sub Configs"

CONTEXT_CONFIGS=(
    examples_cpp_context_executor_cfg.yaml
    examples_cpp_context_logger_cfg.yaml
    examples_cpp_context_publisher_cfg.yaml
    examples_cpp_context_rpc_client_cfg.yaml
    examples_cpp_context_rpc_server_inline_cfg.yaml
    examples_cpp_context_rpc_server_on_executor_cfg.yaml
    examples_cpp_context_subscriber_inline_cfg.yaml
    examples_cpp_context_subscriber_on_executor_cfg.yaml
)

for cfg in "${CONTEXT_CONFIGS[@]}"; do
    if [ -f "cfg/$cfg" ]; then
        run_test "ctx: $cfg" ./bin/ibmw run "cfg/$cfg"
    else
        skip_test "ctx: $cfg (missing)"
    fi
done

###############################################################################
# Section 4: ROS2 Pkg-Mode Configs
###############################################################################
if $QUICK; then
    section "ROS2 Pkg-Mode Configs (SKIPPED --quick)"
else
    section "ROS2 Pkg-Mode Configs"

    ROS2_CONFIGS=(
        examples_cpp_ros2_chn_cfg.yaml
        examples_cpp_ros2_chn_single_pkg_cfg.yaml
        examples_cpp_ros2_chn_publisher_app_cfg.yaml
        examples_cpp_ros2_chn_subscriber_app_cfg.yaml
        examples_cpp_ros2_rpc_sync_cfg.yaml
        examples_cpp_ros2_rpc_async_cfg.yaml
        examples_cpp_ros2_rpc_co_cfg.yaml
        examples_cpp_ros2_rpc_future_cfg.yaml
        examples_cpp_ros2_rpc_single_pkg_cfg.yaml
        examples_cpp_ros2_rpc_client_app_cfg.yaml
        examples_cpp_ros2_rpc_server_app_cfg.yaml
    )

    for cfg in "${ROS2_CONFIGS[@]}"; do
        if [ -f "cfg/$cfg" ]; then
            run_test "ros2: $cfg" ./bin/ibmw run "cfg/$cfg"
        else
            skip_test "ros2: $cfg (missing)"
        fi
    done
fi

###############################################################################
# Section 5: DDS Pkg-Mode Configs
###############################################################################
if $QUICK; then
    section "DDS Pkg-Mode Configs (SKIPPED --quick)"
else
    section "DDS Pkg-Mode Configs"

    DDS_CONFIGS=(
        pubsub_dds_cfg.yaml
        publisher_dds_cfg.yaml
        subscriber_dds_cfg.yaml
    )

    for cfg in "${DDS_CONFIGS[@]}"; do
        if [ -f "cfg/$cfg" ]; then
            run_test "dds: $cfg" ./bin/ibmw run "cfg/$cfg"
        else
            skip_test "dds: $cfg (missing)"
        fi
    done
fi

###############################################################################
# Section 6: Non-DDS/ROS2 Standalone Apps
###############################################################################
section "Non-DDS/ROS2 Standalone Apps"

declare -A STANDALONE_APPS=(
    ["helloworld_app"]="cfg/examples_cpp_helloworld_app_mode_cfg.yaml"
    ["pubsub_app"]="cfg/pubsub_app_mode_cfg.yaml"
    ["publisher_app"]="cfg/publisher_local_cfg.yaml"
    ["subscriber_app"]="cfg/subscriber_local_cfg.yaml"
    ["sub_only_app"]="cfg/sub_only_cfg.yaml"
    ["config_test_app"]="cfg/config_test_cfg.yaml"
    ["loaned_message_app"]="cfg/loaned_message_cfg.yaml"
    ["nonplain_pub_only_app"]="cfg/nonplain_pub_only_cfg.yaml"
    ["plain_pub_only_app"]="cfg/plain_pub_only_cfg.yaml"
    ["multi_driver_demo_app"]="cfg/multi_driver_demo_app_mode_cfg.yaml"
)

for app in "${!STANDALONE_APPS[@]}"; do
    cfg="${STANDALONE_APPS[$app]}"
    if [ -x "./$app" ] && [ -f "$cfg" ]; then
        run_test "app: $app" "./$app" "$cfg"
    else
        skip_test "app: $app (missing binary or cfg)"
    fi
done

# Engine smoke test (no config needed)
if [ -x "./engine_smoke_test_app" ]; then
    run_test "app: engine_smoke_test_app" ./engine_smoke_test_app
else
    skip_test "app: engine_smoke_test_app (missing)"
fi

# Executor conformance test (no config needed; creates temporary configs itself)
if [ -x "./executor_conformance_app" ]; then
    run_strict_test "app: executor_conformance_app" 30 ./executor_conformance_app
else
    skip_test "app: executor_conformance_app (missing)"
fi

# Strict DDS CDR/copy executor lane: proves a non-loaned DDS subscriber can bind
# callbacks to a configured executor and still receive real native DDS messages.
if ! $QUICK && [ -x "./validate_dds_cdr_executor_matrix.sh" ]; then
    run_strict_test "app: dds_cdr_executor_matrix" 30 \
        env BUILD_DIR="$BUILD_DIR" CFG_DIR="$BUILD_DIR/cfg" TMP_DIR="$BUILD_DIR/strict-test-logs" RUN_TIMEOUT_SEC=12 \
        ./validate_dds_cdr_executor_matrix.sh
else
    skip_test "app: dds_cdr_executor_matrix (missing or --quick)"
fi

# Strict transport benchmark matrix: proves that the seven benchmark transport
# lanes deliver real samples instead of only surviving the legacy timeout smoke.
if ! $QUICK && [ -x "./validate_transport_bench_matrix.sh" ]; then
    run_strict_test "app: transport_bench_strict_matrix" 180 \
        env BUILD_DIR="$BUILD_DIR" PAYLOAD_BYTES=4096 DURATION_SEC=2 RUN_TIMEOUT_SEC=20 \
        ./validate_transport_bench_matrix.sh
else
    skip_test "app: transport_bench_strict_matrix (missing or --quick)"
fi

# Strict loaned-message multi-process lane: proves that the standalone
# publisher/subscriber pair exchanges real DDS samples instead of only surviving
# the legacy app timeout smoke.
if ! $QUICK && [ -x "./validate_loaned_message_multi_process.sh" ]; then
    run_strict_test "app: loaned_message_multi_process" 45 \
        env BUILD_DIR="$BUILD_DIR" CFG_DIR="$BUILD_DIR/cfg" RUN_SECONDS=10 \
        ./validate_loaned_message_multi_process.sh
else
    skip_test "app: loaned_message_multi_process (missing or --quick)"
fi

# Strict ROS2 action interoperability lanes. These prove DDS-only action
# endpoints interoperate with native ROS2 action peers in both directions while
# also checking that the DDS-only endpoint does not link against rcl/rmw.
if ! $QUICK && [ -x "./validate_ibmw_client_to_ros2_server.sh" ]; then
    run_strict_test "ros2-action: ibmw_dds_client_to_ros2_server" 60 \
        env BUILD_DIR="$BUILD_DIR" LOG_DIR="$BUILD_DIR/strict-test-logs/ros2_action_client_to_server" \
        ./validate_ibmw_client_to_ros2_server.sh success
else
    skip_test "ros2-action: ibmw_dds_client_to_ros2_server (missing or --quick)"
fi

if ! $QUICK && [ -x "./validate_ros2_client_to_ibmw_server.sh" ]; then
    run_strict_test "ros2-action: ros2_client_to_ibmw_dds_server" 60 \
        env BUILD_DIR="$BUILD_DIR" LOG_DIR="$BUILD_DIR/strict-test-logs/ros2_action_server_to_client" \
        ./validate_ros2_client_to_ibmw_server.sh success
else
    skip_test "ros2-action: ros2_client_to_ibmw_dds_server (missing or --quick)"
fi

# Strict local loopback matrix: upgrades local pub/sub and context demos from
# timeout-pass to real log evidence. This includes the package-mode pub/sub
# paths that previously exposed default-context Send() crashes.
if [ -x "./validate_local_loopback_matrix.sh" ]; then
    run_strict_test "app: local_loopback_strict_matrix" 180 \
        env BUILD_DIR="$BUILD_DIR" CFG_DIR="$BUILD_DIR/cfg" TMP_DIR="$BUILD_DIR/strict-test-logs" RUN_TIMEOUT_SEC="$TIMEOUT_SEC" \
        ./validate_local_loopback_matrix.sh
else
    skip_test "app: local_loopback_strict_matrix (missing)"
fi

# R8 validation _ok samples (no config; standalone main returns 0).
# These compile-only assertion samples prove that publisher_for<T>() and
# subscriber_for<T>() are reachable from a user TU through the public
# cpp/transport/transport_factory.h facade alone (no R1/R2 escapes).
# The _fail samples are gated behind -DIBMW_BUILD_R8_FAIL_SAMPLES=ON and
# are intentionally NOT registered here -- they are exercised only by the
# Gate 9 dual-build matrix (plan §5 #18 / #19, BLK4 K-4').
# The direct_iceoryx_* pair is built with iceoryx, the direct_shm_* pair with
# the self-developed shm transport.
for r8_sample in \
    r8_validation_direct_iceoryx_publisher_ok \
    r8_validation_direct_iceoryx_subscriber_ok \
    r8_validation_direct_shm_publisher_ok \
    r8_validation_direct_shm_subscriber_ok; do
    if [ -x "./$r8_sample" ]; then
        run_test "app: $r8_sample" "./$r8_sample"
    else
        skip_test "app: $r8_sample (missing)"
    fi
done

###############################################################################
# Section 7: DDS Standalone Apps
###############################################################################
if $QUICK; then
    section "DDS Standalone Apps (SKIPPED --quick)"
else
    section "DDS Standalone Apps"

    declare -A DDS_APPS=(
        ["dds_native_app"]="cfg/dds_native_cfg.yaml"
        ["dds_image_app"]="cfg/dds_image_cfg.yaml"
    )

    for app in "${!DDS_APPS[@]}"; do
        cfg="${DDS_APPS[$app]}"
        if [ -x "./$app" ] && [ -f "$cfg" ]; then
            run_test "dds-app: $app" "./$app" "$cfg"
        else
            skip_test "dds-app: $app (missing)"
        fi
    done
fi

###############################################################################
# Section 8: ROS2 Standalone Apps
###############################################################################
if $QUICK; then
    section "ROS2 Standalone Apps (SKIPPED --quick)"
else
    section "ROS2 Standalone Apps"

    declare -A ROS2_APPS=(
        ["normal_ros2_chn_publisher_app"]="cfg/examples_cpp_ros2_chn_publisher_app_cfg.yaml"
        ["normal_ros2_chn_subscriber_app"]="cfg/examples_cpp_ros2_chn_subscriber_app_cfg.yaml"
        ["normal_ros2_rpc_client_app"]="cfg/examples_cpp_ros2_rpc_client_app_cfg.yaml"
        ["normal_ros2_rpc_server_app"]="cfg/examples_cpp_ros2_rpc_server_app_cfg.yaml"
        ["ros2_interop_app"]="cfg/ros2_interop_cfg.yaml"
        ["ros2_point_app"]="cfg/ros2_point_cfg.yaml"
        ["ros2_pub_only_app"]="cfg/ros2_pub_only_cfg.yaml"
        ["ros2_sub_only_app"]="cfg/ros2_sub_only_cfg.yaml"
    )

    for app in "${!ROS2_APPS[@]}"; do
        cfg="${ROS2_APPS[$app]}"
        if [ -x "./$app" ] && [ -f "$cfg" ]; then
            run_test "ros2-app: $app" "./$app" "$cfg"
        else
            skip_test "ros2-app: $app (missing)"
        fi
    done
fi

###############################################################################
# Summary
###############################################################################
section "Summary"
echo ""
echo "Finished at: $(date)"
echo ""
printf "Total: %d   ${COLOR_GREEN}Pass: %d${COLOR_RESET}   ${COLOR_RED}Fail: %d${COLOR_RESET}   ${COLOR_YELLOW}Skip: %d${COLOR_RESET}\n" "$TOTAL" "$PASS" "$FAIL" "$SKIP"

if [ ${#FAILURES[@]} -gt 0 ]; then
    echo ""
    echo "Failed tests:"
    for f in "${FAILURES[@]}"; do
        echo "  - $f"
    done
fi

echo ""
if [ "$FAIL" -eq 0 ]; then
    printf "${COLOR_GREEN}ALL TESTS PASSED${COLOR_RESET}\n"
else
    printf "${COLOR_RED}%d TEST(S) FAILED${COLOR_RESET}\n" "$FAIL"
fi

exit "$FAIL"
