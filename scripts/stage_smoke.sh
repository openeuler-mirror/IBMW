#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# Smoke-test staging helper. Idempotent: removes the stage first.
#
# Inputs : $REPO_ROOT (auto-resolved), $BUILD ($REPO_ROOT/build), $STAGE (default /tmp/ibmw_regress/stage)
# Output : staged binaries+libs+cfg+run_all_tests.sh under $STAGE
# Run    : after a successful `cmake --build build`. Then:
#            cd $STAGE && LD_LIBRARY_PATH=$PWD/lib:/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH ./run_all_tests.sh
set -e
set +u; source /opt/ros/humble/setup.bash; set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"
BUILD="${BUILD:-$REPO_ROOT/build}"
STAGE="${STAGE:-/tmp/ibmw_regress/stage}"

rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$STAGE/cfg" "$STAGE/lib"

# 1) ibmw + iox-roudi binary
cp "$BUILD/ibmw" "$STAGE/bin/" 2>/dev/null || true
cp "$BUILD/ibmw" "$STAGE/" 2>/dev/null || true
chmod +x "$STAGE/ibmw" "$STAGE/bin/ibmw" 2>/dev/null || true
find "$BUILD" -name "iox-roudi" -executable -type f 2>/dev/null | head -1 | xargs -I{} cp {} "$STAGE/bin/" || true
find "$BUILD" -name "iox-roudi" -executable -type f 2>/dev/null | head -1 | xargs -I{} cp {} "$STAGE/" || true
chmod +x "$STAGE/iox-roudi" 2>/dev/null || true

# 2) all *_app executables
find "$BUILD" -maxdepth 4 -name "*_app" -executable -type f | while read f; do
  cp "$f" "$STAGE/" 2>/dev/null || true
done

# 2b) r8_validation _ok samples (compile-only assertion samples
#     that don't follow the *_app naming convention; the _fail siblings are
#     intentionally NOT staged -- they are matrix-only).
find "$BUILD" -maxdepth 6 \
  \( -name "r8_validation_direct_iceoryx_publisher_ok" \
     -o -name "r8_validation_direct_iceoryx_subscriber_ok" \
     -o -name "ibmw_fibonacci_action_ros2_server" \
     -o -name "ibmw_fibonacci_action_dds_client" \
     -o -name "ibmw_fibonacci_action_dds_server" \
     -o -name "ibmw_fibonacci_action_ros2_client" \) \
  -executable -type f | while read f; do
  cp "$f" "$STAGE/" 2>/dev/null || true
done

# 3) all cfg yamls
find "$REPO_ROOT/src/samples" -path "*/config/linux/bin/cfg/*.yaml" | while read f; do
  cp "$f" "$STAGE/cfg/" 2>/dev/null || true
done
find "$REPO_ROOT/src/samples/cpp/extension_smoke_test/cfg" -name "*.yaml" -type f 2>/dev/null | while read f; do
  cp "$f" "$STAGE/cfg/" 2>/dev/null || true
done
cp "$BUILD/dds_image_cfg.yaml" "$STAGE/cfg/" 2>/dev/null || true
cp "$BUILD/dds_native_cfg.yaml" "$STAGE/cfg/" 2>/dev/null || true

# 4) all .so* into stage/lib + symlink at stage root (cfg writes ./libxxx.so)
find "$BUILD" -maxdepth 6 -name "*.so*" -type f 2>/dev/null | while read f; do
  cp -f "$f" "$STAGE/lib/" 2>/dev/null || true
done
# Prefer the build-root libraries over stale compatibility copies in nested
# build/bin directories. Several apps load extensions through ./libxxx.so while
# direct linkage may resolve against the build-root RPATH during focused runs;
# staging must therefore make $STAGE/lib match $BUILD for the root .so files.
find "$BUILD" -maxdepth 1 -name "*.so*" -type f 2>/dev/null | while read f; do
  cp -f "$f" "$STAGE/lib/" 2>/dev/null || true
done
for so in "$STAGE/lib/"*.so*; do
  [ -e "$so" ] && ln -sf "lib/$(basename "$so")" "$STAGE/$(basename "$so")" 2>/dev/null || true
done

# 5) run_all_tests.sh
cp "$REPO_ROOT/src/samples/cpp/extension_smoke_test/run_all_tests.sh" "$STAGE/"
chmod +x "$STAGE/run_all_tests.sh"

# 6) strict transport-benchmark validation scripts. These are generated into
#    the build root by the transport_benchmark CMake target and are consumed by
#    run_all_tests.sh as stage-local apps.
cp "$BUILD/validate_transport_bench_matrix.sh" "$STAGE/" 2>/dev/null || true
cp "$BUILD/validate_dds_loaned_e2e.sh" "$STAGE/" 2>/dev/null || true
cp "$BUILD/validate_dds_send_shm_executor_e2e.sh" "$STAGE/" 2>/dev/null || true
cp "$BUILD/validate_dds_send_tcp_executor_e2e.sh" "$STAGE/" 2>/dev/null || true
cp "$BUILD/validate_loaned_message_multi_process.sh" "$STAGE/" 2>/dev/null || true
cp "$REPO_ROOT/src/samples/cpp/ros2_action_fibonacci_dds_client/validate_ibmw_client_to_ros2_server.sh" \
  "$STAGE/validate_ibmw_client_to_ros2_server.sh" 2>/dev/null || true
cp "$REPO_ROOT/src/samples/cpp/ros2_action_fibonacci_dds_server/validate_ros2_client_to_ibmw_server.sh" \
  "$STAGE/validate_ros2_client_to_ibmw_server.sh" 2>/dev/null || true
cp "$REPO_ROOT/src/samples/cpp/extension_smoke_test/validate_local_loopback_matrix.sh" \
  "$STAGE/validate_local_loopback_matrix.sh" 2>/dev/null || true
cp "$REPO_ROOT/src/samples/cpp/extension_smoke_test/validate_dds_cdr_executor_matrix.sh" \
  "$STAGE/validate_dds_cdr_executor_matrix.sh" 2>/dev/null || true
chmod +x "$STAGE/validate_transport_bench_matrix.sh" \
  "$STAGE/validate_dds_loaned_e2e.sh" \
  "$STAGE/validate_dds_send_shm_executor_e2e.sh" \
  "$STAGE/validate_dds_send_tcp_executor_e2e.sh" \
  "$STAGE/validate_loaned_message_multi_process.sh" \
  "$STAGE/validate_ibmw_client_to_ros2_server.sh" \
  "$STAGE/validate_ros2_client_to_ibmw_server.sh" \
  "$STAGE/validate_local_loopback_matrix.sh" \
  "$STAGE/validate_dds_cdr_executor_matrix.sh" 2>/dev/null || true

for bench_sample in \
  iox_loaned_pod \
  iox_loaned_ctx \
  iox_send \
  dds_loaned \
  dds_send_shm \
  dds_send_tcp \
  net_send_tcp; do
  cp "$BUILD/start_${bench_sample}.sh" "$STAGE/" 2>/dev/null || true
  chmod +x "$STAGE/start_${bench_sample}.sh" 2>/dev/null || true
  if [ -d "$BUILD/cfg/${bench_sample}" ]; then
    mkdir -p "$STAGE/cfg/${bench_sample}"
    cp "$BUILD/cfg/${bench_sample}/"*.yaml "$STAGE/cfg/${bench_sample}/" 2>/dev/null || true
  fi
done

for loaned_cfg in plain_pub_only_cfg.yaml sub_only_cfg.yaml; do
  cp "$REPO_ROOT/src/samples/cpp/loaned_message_demo/config/cfg/$loaned_cfg" \
    "$STAGE/cfg/" 2>/dev/null || true
done

echo "=== stage done @ $STAGE ==="
ls "$STAGE/" | head -30
echo "--- cfg count: $(ls "$STAGE/cfg/" | wc -l) ---"
echo "--- lib count: $(ls "$STAGE/lib/" | wc -l) ---"
