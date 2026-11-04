#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#
# Smoke-test staging helper. Idempotent: removes the stage first.
#
# Inputs : $REPO_ROOT (auto-resolved), $BUILD ($REPO_ROOT/build), $STAGE (default /tmp/ibmw_regress/stage)
# Output : staged binaries+libs+cfg+cli.yaml+run_all_tests.sh under $STAGE
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

RECORD_SMOKE_REQUIRED=0
if [ -f "$BUILD/record_smoke_required" ]; then
  RECORD_SMOKE_REQUIRED=1
fi
INFO_SMOKE_REQUIRED=0
if [ -f "$BUILD/info_smoke_required" ]; then
  INFO_SMOKE_REQUIRED=1
fi
CONVERT_SMOKE_REQUIRED=0
if [ -f "$BUILD/convert_smoke_required" ]; then
  CONVERT_SMOKE_REQUIRED=1
fi
PLAY_SMOKE_REQUIRED=0
if [ -f "$BUILD/play_smoke_required" ]; then
  PLAY_SMOKE_REQUIRED=1
fi

# Shared-memory transports in this build. iceoryx-only artifacts (iox-roudi,
# libibmw_iceoryx_transport.so, iceoryx record/play configs) are required only
# when iceoryx is built; the self-developed shm plugin when shm is built.
# Without a CMakeCache.txt iceoryx is assumed built (the previous behaviour).
build_option_on() {
  grep -qiE "^$1:BOOL=(ON|YES|Y|TRUE|1)$" "$BUILD/CMakeCache.txt" 2>/dev/null
}
ICEORYX_BUILT=0
if [ ! -f "$BUILD/CMakeCache.txt" ]; then
  echo "stage: $BUILD/CMakeCache.txt not found; assuming iceoryx is built" >&2
  ICEORYX_BUILT=1
elif build_option_on IBMW_BUILD_ICEORYX_EXTENSION; then
  ICEORYX_BUILT=1
fi
SHM_BUILT=0
if build_option_on IBMW_BUILD_SHM_EXTENSION; then
  SHM_BUILT=1
fi

copy_required() {
  src="$1"
  dst="$2"
  [ -e "$src" ] || { echo "stage: required smoke artifact missing: $src" >&2; exit 1; }
  cp -a "$src" "$dst"
}

# 1) ibmw + iox-roudi binary (iox-roudi only when iceoryx is built)
cp "$BUILD/ibmw" "$STAGE/bin/" 2>/dev/null || true
cp "$BUILD/ibmw" "$STAGE/" 2>/dev/null || true
chmod +x "$STAGE/ibmw" "$STAGE/bin/ibmw" 2>/dev/null || true
if [ "$ICEORYX_BUILT" -eq 1 ]; then
  find "$BUILD" -name "iox-roudi" -executable -type f 2>/dev/null | head -1 | xargs -I{} cp {} "$STAGE/bin/" || true
  find "$BUILD" -name "iox-roudi" -executable -type f 2>/dev/null | head -1 | xargs -I{} cp {} "$STAGE/" || true
  chmod +x "$STAGE/iox-roudi" 2>/dev/null || true
fi

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
     -o -name "r8_validation_direct_shm_publisher_ok" \
     -o -name "r8_validation_direct_shm_subscriber_ok" \
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

if [ "$SHM_BUILT" -eq 1 ]; then
  [ -f "$STAGE/lib/libibmw_shm_transport.so" ] || {
    echo "stage: required shm transport library missing: libibmw_shm_transport.so" >&2
    exit 1
  }
fi

# Use the manifest generated for this layout, with paths relative to $STAGE.
copy_required "$BUILD/cli-stage.yaml" "$STAGE/cli.yaml"

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
if [ "$RECORD_SMOKE_REQUIRED" -eq 1 ]; then
  copy_required "$BUILD/validate_record_smoke.sh" "$STAGE/validate_record_smoke.sh"
  copy_required "$BUILD/record_smoke_ros2_runtime" "$STAGE/record_smoke_ros2_runtime"
  copy_required "$BUILD/record_smoke_required" "$STAGE/record_smoke_required"
  chmod +x "$STAGE/validate_record_smoke.sh"
else
  cp "$BUILD/validate_record_smoke.sh" "$STAGE/validate_record_smoke.sh" 2>/dev/null || true
  cp -r "$BUILD/record_smoke_ros2_runtime" "$STAGE/record_smoke_ros2_runtime" 2>/dev/null || true
fi

if [ "$RECORD_SMOKE_REQUIRED" -eq 1 ]; then
  record_iceoryx_cfgs=""
  record_iceoryx_libs=""
  if [ "$ICEORYX_BUILT" -eq 1 ]; then
    record_iceoryx_cfgs="record_smoke_iceoryx.yaml"
    record_iceoryx_libs="libibmw_iceoryx_transport.so"
  fi
  for cfg in \
    record_smoke_local.yaml record_smoke_dds.yaml $record_iceoryx_cfgs \
    record_smoke_mqtt.yaml record_smoke_zenoh.yaml record_smoke_ros2.yaml \
    record_smoke_tcp.yaml record_smoke_udp.yaml record_smoke_http.yaml \
    record_smoke_features.yaml record_smoke_json.yaml \
    record_smoke_sim_time.yaml record_smoke_late.yaml \
    record_smoke_mcap_storage.yaml record_smoke_sqlite_storage.yaml \
    record_smoke_transcode.yaml; do
    [ -f "$STAGE/cfg/$cfg" ] || {
      echo "stage: required record smoke config missing: $cfg" >&2
      exit 1
    }
  done
  for artifact in \
    librecord_smoke_pkg.so librecord_smoke_ts.so libcommon_dds_ts.so \
    libibmw_record_playback.so libibmw_dds_transport.so \
    $record_iceoryx_libs libibmw_mqtt_transport.so \
    libibmw_zenoh_transport.so libibmw_ros2_transport.so \
    libibmw_net_transport.so; do
    [ -f "$STAGE/lib/$artifact" ] || {
      echo "stage: required record smoke library missing: $artifact" >&2
      exit 1
    }
  done
  [ -x "$STAGE/ibmw" ] || {
    echo "stage: required record smoke binary missing: ibmw" >&2
    exit 1
  }
  if [ "$ICEORYX_BUILT" -eq 1 ]; then
    [ -x "$STAGE/iox-roudi" ] || [ -x "$STAGE/bin/iox-roudi" ] || {
      echo "stage: required record smoke binary missing: iox-roudi" >&2
      exit 1
    }
  fi
fi

if [ "$INFO_SMOKE_REQUIRED" -eq 1 ]; then
  copy_required "$BUILD/validate_info_smoke.sh" "$STAGE/validate_info_smoke.sh"
  copy_required "$BUILD/info_smoke_bags" "$STAGE/info_smoke_bags"
  copy_required "$BUILD/info_smoke_required" "$STAGE/info_smoke_required"
  chmod +x "$STAGE/validate_info_smoke.sh"
else
  cp "$BUILD/validate_info_smoke.sh" "$STAGE/validate_info_smoke.sh" 2>/dev/null || true
  cp -r "$BUILD/info_smoke_bags" "$STAGE/info_smoke_bags" 2>/dev/null || true
fi

if [ "$INFO_SMOKE_REQUIRED" -eq 1 ]; then
  [ -x "$STAGE/validate_info_smoke.sh" ] || {
    echo "stage: required info smoke script missing" >&2
    exit 1
  }
  [ -f "$STAGE/info_smoke_bags/mcap/metadata.yaml" ] || {
    echo "stage: required info smoke MCAP fixture missing" >&2
    exit 1
  }
  [ -f "$STAGE/info_smoke_bags/sqlite3/metadata.yaml" ] || {
    echo "stage: required info smoke SQLite3 fixture missing" >&2
    exit 1
  }
  for fixture in multi_file_mcap message_zstd_mcap file_lz4_mcap empty_mcap; do
    [ -f "$STAGE/info_smoke_bags/$fixture/metadata.yaml" ] || {
      echo "stage: required info smoke fixture missing: $fixture" >&2
      exit 1
    }
  done
fi

if [ "$CONVERT_SMOKE_REQUIRED" -eq 1 ]; then
  copy_required "$BUILD/validate_convert_smoke.sh" "$STAGE/validate_convert_smoke.sh"
  copy_required "$BUILD/convert_smoke_bags" "$STAGE/convert_smoke_bags"
  copy_required "$BUILD/convert_smoke_cfg" "$STAGE/convert_smoke_cfg"
  copy_required "$BUILD/convert_smoke_required" "$STAGE/convert_smoke_required"
  chmod +x "$STAGE/validate_convert_smoke.sh"
else
  cp "$BUILD/validate_convert_smoke.sh" "$STAGE/validate_convert_smoke.sh" 2>/dev/null || true
  cp -r "$BUILD/convert_smoke_bags" "$STAGE/convert_smoke_bags" 2>/dev/null || true
  cp -r "$BUILD/convert_smoke_cfg" "$STAGE/convert_smoke_cfg" 2>/dev/null || true
fi

if [ "$CONVERT_SMOKE_REQUIRED" -eq 1 ]; then
  [ -x "$STAGE/validate_convert_smoke.sh" ] || {
    echo "stage: required convert smoke script missing" >&2
    exit 1
  }
  [ -f "$STAGE/convert_smoke_bags/ibmw_mcap/metadata.yaml" ] || {
    echo "stage: required convert smoke IBMW fixture missing" >&2
    exit 1
  }
  for cfg in \
    convert_smoke_mcap_writer.yaml convert_smoke_mcap_reader.yaml \
    convert_smoke_sqlite_writer.yaml convert_smoke_sqlite_reader.yaml; do
    [ -f "$STAGE/convert_smoke_cfg/$cfg" ] || {
      echo "stage: required convert smoke config missing: $cfg" >&2
      exit 1
    }
  done
  [ -x "$STAGE/ibmw" ] || {
    echo "stage: required convert smoke binary missing: ibmw" >&2
    exit 1
  }
  [ -f "$STAGE/lib/libibmw_record_playback.so" ] || {
    echo "stage: required convert smoke library missing: libibmw_record_playback.so" >&2
    exit 1
  }
fi

if [ "$PLAY_SMOKE_REQUIRED" -eq 1 ]; then
  copy_required "$BUILD/validate_play_smoke.sh" "$STAGE/validate_play_smoke.sh"
  copy_required "$BUILD/play_smoke_bags" "$STAGE/play_smoke_bags"
  copy_required "$BUILD/play_smoke_cfg" "$STAGE/play_smoke_cfg"
  copy_required "$BUILD/play_smoke_ros2_runtime" "$STAGE/play_smoke_ros2_runtime"
  copy_required "$BUILD/play_smoke_required" "$STAGE/play_smoke_required"
  chmod +x "$STAGE/validate_play_smoke.sh"
else
  cp "$BUILD/validate_play_smoke.sh" "$STAGE/validate_play_smoke.sh" 2>/dev/null || true
  cp -r "$BUILD/play_smoke_bags" "$STAGE/play_smoke_bags" 2>/dev/null || true
  cp -r "$BUILD/play_smoke_cfg" "$STAGE/play_smoke_cfg" 2>/dev/null || true
  cp -r "$BUILD/play_smoke_ros2_runtime" "$STAGE/play_smoke_ros2_runtime" 2>/dev/null || true
fi

if [ "$PLAY_SMOKE_REQUIRED" -eq 1 ]; then
  [ -x "$STAGE/validate_play_smoke.sh" ] || {
    echo "stage: required play smoke script missing" >&2
    exit 1
  }
  for fixture in cdr_mcap cdr_mcap_reader cdr_sqlite3 ros2_mcap; do
    [ -f "$STAGE/play_smoke_bags/$fixture/metadata.yaml" ] || {
      echo "stage: required play smoke fixture missing: $fixture" >&2
      exit 1
    }
  done
  play_iceoryx_cfgs=""
  play_iceoryx_libs=""
  if [ "$ICEORYX_BUILT" -eq 1 ]; then
    play_iceoryx_cfgs="play_smoke_iceoryx_subscriber.yaml play_smoke_iceoryx_player.yaml"
    play_iceoryx_libs="libibmw_iceoryx_transport.so"
  fi
  for cfg in \
    play_smoke_local_subscriber.yaml play_smoke_local_player.yaml \
    play_smoke_local_player_alt.yaml play_smoke_json_subscriber.yaml \
    play_smoke_json_player.yaml play_smoke_mcap_reader.yaml \
    play_smoke_sqlite_reader.yaml \
    play_smoke_dds_subscriber.yaml play_smoke_dds_player.yaml \
    $play_iceoryx_cfgs \
    play_smoke_mqtt_subscriber.yaml play_smoke_mqtt_player.yaml \
    play_smoke_zenoh_subscriber.yaml play_smoke_zenoh_player.yaml \
    play_smoke_ros2_subscriber.yaml play_smoke_ros2_player.yaml \
    play_smoke_tcp_subscriber.yaml play_smoke_tcp_player.yaml \
    play_smoke_udp_subscriber.yaml play_smoke_udp_player.yaml \
    play_smoke_http_subscriber.yaml play_smoke_http_player.yaml; do
    [ -f "$STAGE/play_smoke_cfg/$cfg" ] || {
      echo "stage: required play smoke config missing: $cfg" >&2
      exit 1
    }
  done
  for artifact in \
    libplay_smoke_pkg.so libplay_smoke_ts.so \
    libibmw_record_playback.so libibmw_dds_transport.so \
    $play_iceoryx_libs libibmw_mqtt_transport.so \
    libibmw_zenoh_transport.so libibmw_ros2_transport.so \
    libibmw_net_transport.so; do
    [ -f "$STAGE/lib/$artifact" ] || {
      echo "stage: required play smoke library missing: $artifact" >&2
      exit 1
    }
  done
  [ -x "$STAGE/ibmw" ] || {
    echo "stage: required play smoke binary missing: ibmw" >&2
    exit 1
  }
  if [ "$ICEORYX_BUILT" -eq 1 ]; then
    [ -x "$STAGE/iox-roudi" ] || [ -x "$STAGE/bin/iox-roudi" ] || {
      echo "stage: required play smoke binary missing: iox-roudi" >&2
      exit 1
    }
  fi
fi
chmod +x "$STAGE/validate_transport_bench_matrix.sh" \
  "$STAGE/validate_dds_loaned_e2e.sh" \
  "$STAGE/validate_dds_send_shm_executor_e2e.sh" \
  "$STAGE/validate_dds_send_tcp_executor_e2e.sh" \
  "$STAGE/validate_loaned_message_multi_process.sh" \
  "$STAGE/validate_ibmw_client_to_ros2_server.sh" \
  "$STAGE/validate_ros2_client_to_ibmw_server.sh" \
  "$STAGE/validate_local_loopback_matrix.sh" \
  "$STAGE/validate_dds_cdr_executor_matrix.sh" 2>/dev/null || true

# Lanes that are not built (e.g. iox_* without iceoryx, shm_* without shm)
# have no start script and are skipped.
for bench_sample in \
  iox_loaned_pod \
  iox_loaned_ctx \
  iox_send \
  shm_loaned_pod \
  shm_loaned_ctx \
  shm_send \
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
