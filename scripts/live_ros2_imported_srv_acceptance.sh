#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# Live bidirectional ROS2 <-> IBMW acceptance for a custom imported .srv package.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${1:-${TMPDIR:-/tmp}/ibmw-ros2-imported-srv-live-acceptance}"
PACKAGE_NAME="${PACKAGE_NAME:-ibmw_live_srv_interfaces}"
SERVICE_NAME="${SERVICE_NAME:-/set_mode}"
BLOB_SERVICE_NAME="${BLOB_SERVICE_NAME:-/blob_echo}"
NESTED_SERVICE_NAME="${NESTED_SERVICE_NAME:-/nested_echo}"
BATCH_SERVICE_NAME="${BATCH_SERVICE_NAME:-/batch_echo}"
ROS2_SETUP="${ROS2_SETUP:-/opt/ros/humble/setup.bash}"
ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-0}"
IBMW_INSTALL_PREFIX="${IBMW_INSTALL_PREFIX:-${TMPDIR:-/tmp}/ibmw-msggen-validation/ibmw-host-install-phase0}"
ROS2_EXTENSION_LIB_DIR="${ROS2_EXTENSION_LIB_DIR:-${ROOT_DIR}/build/bin}"

PIDS=()

fail() {
  echo "ERROR: $*" >&2
  exit 1
}

cleanup() {
  local pid
  for pid in "${PIDS[@]:-}"; do
    stop_pid "${pid}"
  done
}
trap cleanup EXIT

stop_pid() {
  local pid=$1
  if kill -0 "${pid}" 2>/dev/null; then
    kill -- -"${pid}" 2>/dev/null || kill "${pid}" 2>/dev/null || true
    sleep 1
  fi
  if kill -0 "${pid}" 2>/dev/null; then
    kill -9 -- -"${pid}" 2>/dev/null || kill -9 "${pid}" 2>/dev/null || true
  fi
  wait "${pid}" 2>/dev/null || true
}

require_file() {
  [[ -f "$1" ]] || fail "required file not found: $1"
}

require_executable() {
  [[ -x "$1" ]] || fail "required executable not found or not executable: $1"
}

source_ros2() {
  require_file "${ROS2_SETUP}"
  set +u
  # shellcheck disable=SC1090
  source "${ROS2_SETUP}"
  set -u
  export ROS_DOMAIN_ID
  command -v ros2 >/dev/null 2>&1 || fail "ros2 CLI not found after sourcing ${ROS2_SETUP}"
}

patch_set_mode_handler() {
  local handler_file=$1
  python3 - "${handler_file}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
begin = "  // IBMW_RPC_SERVER_HANDLER_BODY_SET_MODE_BEGIN\n"
end = "  // IBMW_RPC_SERVER_HANDLER_BODY_SET_MODE_END"
start = text.index(begin) + len(begin)
finish = text.index(end)
body = """  rsp.accepted = req.enable && req.mode == "auto";
  rsp.reason = rsp.accepted ? "accepted:auto" : "rejected";
  return ibmw::rpc::Status();
"""
path.write_text(text[:start] + body + text[finish:], encoding="utf-8")
PY
}

patch_set_mode_client() {
  local client_file=$1
  local package_name=$2
  python3 - "${client_file}" "${package_name}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
package_name = sys.argv[2]
text = path.read_text(encoding="utf-8")

req_begin = "    // IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_SET_MODE_BEGIN\n"
req_end = "    // IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_SET_MODE_END"
req_start = text.index(req_begin) + len(req_begin)
req_finish = text.index(req_end)
req_body = """    req.mode = "auto";
    req.enable = true;
"""
text = text[:req_start] + req_body + text[req_finish:]

rsp_begin = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_SET_MODE_BEGIN\n"
rsp_end = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_SET_MODE_END"
rsp_start = text.index(rsp_begin) + len(rsp_begin)
rsp_finish = text.index(rsp_end)
rsp_body = """      MW_INFO("Client get rpc ret, status: {}, rsp: {}",
              status.ToString(), __PACKAGE_NAME__::srv::to_yaml(rsp));
      if (rsp.accepted && rsp.reason == "accepted:auto") {
        MW_INFO("IBMW_IMPORTED_SRV_CLIENT_ACCEPTED_AUTO");
        self->run_flag_ = false;
      } else {
        MW_ERROR("Unexpected SetMode response: accepted={}, reason={}", rsp.accepted, rsp.reason);
        self->run_flag_ = false;
      }
""".replace("__PACKAGE_NAME__", package_name)
path.write_text(text[:rsp_start] + rsp_body + text[rsp_finish:], encoding="utf-8")
PY
}

patch_blob_echo_handler() {
  local handler_file=$1
  python3 - "${handler_file}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
begin = "  // IBMW_RPC_SERVER_HANDLER_BODY_BLOB_ECHO_BEGIN\n"
end = "  // IBMW_RPC_SERVER_HANDLER_BODY_BLOB_ECHO_END"
start = text.index(begin) + len(begin)
finish = text.index(end)
body = """  rsp.echoed_payload = req.payload;
  return ibmw::rpc::Status();
"""
path.write_text(text[:start] + body + text[finish:], encoding="utf-8")
PY
}

patch_blob_echo_client() {
  local client_file=$1
  local package_name=$2
  python3 - "${client_file}" "${package_name}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
package_name = sys.argv[2]
text = path.read_text(encoding="utf-8")

req_begin = "    // IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_BLOB_ECHO_BEGIN\n"
req_end = "    // IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_BLOB_ECHO_END"
req_start = text.index(req_begin) + len(req_begin)
req_finish = text.index(req_end)
req_body = """    req.payload = {1, 2, 3, 5};
"""
text = text[:req_start] + req_body + text[req_finish:]

rsp_begin = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_BLOB_ECHO_BEGIN\n"
rsp_end = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_BLOB_ECHO_END"
rsp_start = text.index(rsp_begin) + len(rsp_begin)
rsp_finish = text.index(rsp_end)
rsp_body = """      MW_INFO("Client get rpc ret, status: {}, rsp: {}",
              status.ToString(), __PACKAGE_NAME__::srv::to_yaml(rsp));
      if (rsp.echoed_payload.size() == 4 && rsp.echoed_payload[0] == 1 &&
          rsp.echoed_payload[1] == 2 && rsp.echoed_payload[2] == 3 &&
          rsp.echoed_payload[3] == 5) {
        MW_INFO("IBMW_IMPORTED_SRV_CLIENT_BLOB_ECHO_OK");
        self->run_flag_ = false;
      } else {
        MW_ERROR("Unexpected BlobEcho response size={}", rsp.echoed_payload.size());
        self->run_flag_ = false;
      }
""".replace("__PACKAGE_NAME__", package_name)
path.write_text(text[:rsp_start] + rsp_body + text[rsp_finish:], encoding="utf-8")
PY
}

patch_nested_echo_handler() {
  local handler_file=$1
  python3 - "${handler_file}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
begin = "  // IBMW_RPC_SERVER_HANDLER_BODY_NESTED_ECHO_BEGIN\n"
end = "  // IBMW_RPC_SERVER_HANDLER_BODY_NESTED_ECHO_END"
start = text.index(begin) + len(begin)
finish = text.index(end)
body = """  rsp.echoed_leaf = req.leaf;
  return ibmw::rpc::Status();
"""
path.write_text(text[:start] + body + text[finish:], encoding="utf-8")
PY
}

patch_nested_echo_client() {
  local client_file=$1
  local package_name=$2
  python3 - "${client_file}" "${package_name}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
package_name = sys.argv[2]
text = path.read_text(encoding="utf-8")

req_begin = "    // IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_NESTED_ECHO_BEGIN\n"
req_end = "    // IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_NESTED_ECHO_END"
req_start = text.index(req_begin) + len(req_begin)
req_finish = text.index(req_end)
req_body = """    req.leaf.value = 42;
    req.leaf.label = "nested";
"""
text = text[:req_start] + req_body + text[req_finish:]

rsp_begin = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_NESTED_ECHO_BEGIN\n"
rsp_end = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_NESTED_ECHO_END"
rsp_start = text.index(rsp_begin) + len(rsp_begin)
rsp_finish = text.index(rsp_end)
rsp_body = """      MW_INFO("Client get rpc ret, status: {}, rsp: {}",
              status.ToString(), __PACKAGE_NAME__::srv::to_yaml(rsp));
      if (rsp.echoed_leaf.value == 42 && rsp.echoed_leaf.label == "nested") {
        MW_INFO("IBMW_IMPORTED_SRV_CLIENT_NESTED_ECHO_OK");
        self->run_flag_ = false;
      } else {
        MW_ERROR("Unexpected NestedEcho response value={}, label={}",
                 rsp.echoed_leaf.value, rsp.echoed_leaf.label);
        self->run_flag_ = false;
      }
""".replace("__PACKAGE_NAME__", package_name)
path.write_text(text[:rsp_start] + rsp_body + text[rsp_finish:], encoding="utf-8")
PY
}

patch_batch_echo_handler() {
  local handler_file=$1
  python3 - "${handler_file}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
begin = "  // IBMW_RPC_SERVER_HANDLER_BODY_BATCH_ECHO_BEGIN\n"
end = "  // IBMW_RPC_SERVER_HANDLER_BODY_BATCH_ECHO_END"
start = text.index(begin) + len(begin)
finish = text.index(end)
body = """  rsp.echoed_values.assign(req.fixed_values.begin(), req.fixed_values.end());
  rsp.bounded_payload.assign(req.payload.begin(), req.payload.begin() + 8);
  return ibmw::rpc::Status();
"""
path.write_text(text[:start] + body + text[finish:], encoding="utf-8")
PY
}

patch_batch_echo_client() {
  local client_file=$1
  local package_name=$2
  python3 - "${client_file}" "${package_name}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
package_name = sys.argv[2]
text = path.read_text(encoding="utf-8")

req_begin = "    // IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_BATCH_ECHO_BEGIN\n"
req_end = "    // IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_BATCH_ECHO_END"
req_start = text.index(req_begin) + len(req_begin)
req_finish = text.index(req_end)
req_body = """    req.fixed_values = {3, 1, 4};
    req.payload = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    req.leaves.resize(1);
    req.leaves[0].value = 7;
    req.leaves[0].label = "batch";
"""
text = text[:req_start] + req_body + text[req_finish:]

rsp_begin = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_BATCH_ECHO_BEGIN\n"
rsp_end = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_BATCH_ECHO_END"
rsp_start = text.index(rsp_begin) + len(rsp_begin)
rsp_finish = text.index(rsp_end)
rsp_body = """      MW_INFO("Client get rpc ret, status: {}, rsp: {}",
              status.ToString(), __PACKAGE_NAME__::srv::to_yaml(rsp));
      if (rsp.echoed_values.size() == 3 && rsp.echoed_values[0] == 3 &&
          rsp.echoed_values[1] == 1 && rsp.echoed_values[2] == 4 &&
          rsp.bounded_payload.size() == 8 && rsp.bounded_payload[7] == 8) {
        MW_INFO("IBMW_IMPORTED_SRV_CLIENT_BATCH_ECHO_OK");
        self->run_flag_ = false;
      } else {
        MW_ERROR("Unexpected BatchEcho response values={}, payload={}",
                 rsp.echoed_values.size(), rsp.bounded_payload.size());
        self->run_flag_ = false;
      }
""".replace("__PACKAGE_NAME__", package_name)
path.write_text(text[:rsp_start] + rsp_body + text[rsp_finish:], encoding="utf-8")
PY
}

write_ros2_set_mode_server() {
  local server_file=$1
  local package_name=$2
  cat > "${server_file}" <<'PY'
#!/usr/bin/env python3
import os
import rclpy
from __PACKAGE_NAME__.srv import SetMode


def main():
    service_name = os.environ.get("SERVICE_NAME", "/set_mode")
    rclpy.init()
    node = rclpy.create_node("ibmw_imported_set_mode_ros2_server")

    def handle(req, rsp):
        rsp.accepted = bool(req.enable and req.mode == "auto")
        rsp.reason = "accepted:auto" if rsp.accepted else "rejected"
        node.get_logger().info(f"handled SetMode request mode={req.mode} enable={req.enable}")
        return rsp

    node.create_service(SetMode, service_name, handle)
    node.get_logger().info(f"ready {service_name}")
    rclpy.spin(node)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
PY
  python3 - "${server_file}" "${package_name}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
path.write_text(path.read_text(encoding="utf-8").replace("__PACKAGE_NAME__", sys.argv[2]), encoding="utf-8")
PY
  chmod +x "${server_file}"
}

write_ros2_blob_echo_server() {
  local server_file=$1
  local package_name=$2
  cat > "${server_file}" <<'PY'
#!/usr/bin/env python3
import os
import rclpy
from __PACKAGE_NAME__.srv import BlobEcho


def main():
    service_name = os.environ.get("BLOB_SERVICE_NAME", "/blob_echo")
    rclpy.init()
    node = rclpy.create_node("ibmw_imported_blob_echo_ros2_server")

    def handle(req, rsp):
        rsp.echoed_payload = list(req.payload)
        node.get_logger().info(f"handled BlobEcho request len={len(req.payload)}")
        return rsp

    node.create_service(BlobEcho, service_name, handle)
    node.get_logger().info(f"ready {service_name}")
    rclpy.spin(node)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
PY
  python3 - "${server_file}" "${package_name}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
path.write_text(path.read_text(encoding="utf-8").replace("__PACKAGE_NAME__", sys.argv[2]), encoding="utf-8")
PY
  chmod +x "${server_file}"
}

write_ros2_nested_echo_server() {
  local server_file=$1
  local package_name=$2
  cat > "${server_file}" <<'PY'
#!/usr/bin/env python3
import os
import rclpy
from __PACKAGE_NAME__.srv import NestedEcho


def main():
    service_name = os.environ.get("NESTED_SERVICE_NAME", "/nested_echo")
    rclpy.init()
    node = rclpy.create_node("ibmw_imported_nested_echo_ros2_server")

    def handle(req, rsp):
        rsp.echoed_leaf = req.leaf
        node.get_logger().info(
            f"handled NestedEcho request value={req.leaf.value} label={req.leaf.label}"
        )
        return rsp

    node.create_service(NestedEcho, service_name, handle)
    node.get_logger().info(f"ready {service_name}")
    rclpy.spin(node)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
PY
  python3 - "${server_file}" "${package_name}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
path.write_text(path.read_text(encoding="utf-8").replace("__PACKAGE_NAME__", sys.argv[2]), encoding="utf-8")
PY
  chmod +x "${server_file}"
}

write_ros2_batch_echo_server() {
  local server_file=$1
  local package_name=$2
  cat > "${server_file}" <<'PY'
#!/usr/bin/env python3
import os
import rclpy
from __PACKAGE_NAME__.srv import BatchEcho


def main():
    service_name = os.environ.get("BATCH_SERVICE_NAME", "/batch_echo")
    rclpy.init()
    node = rclpy.create_node("ibmw_imported_batch_echo_ros2_server")

    def handle(req, rsp):
        rsp.echoed_values = [int(value) for value in req.fixed_values]
        rsp.bounded_payload = [int(value) for value in req.payload[:8]]
        node.get_logger().info(
            f"handled BatchEcho request fixed={list(req.fixed_values)} payload_len={len(req.payload)} leaves={len(req.leaves)}"
        )
        return rsp

    node.create_service(BatchEcho, service_name, handle)
    node.get_logger().info(f"ready {service_name}")
    rclpy.spin(node)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
PY
  python3 - "${server_file}" "${package_name}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
path.write_text(path.read_text(encoding="utf-8").replace("__PACKAGE_NAME__", sys.argv[2]), encoding="utf-8")
PY
  chmod +x "${server_file}"
}

wait_for_service() {
  local service_name=$1
  local service_list_log=$2
  local discovered=no
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    timeout 10 ros2 service list >"${service_list_log}" 2>&1 || true
    if grep -q "^${service_name}$" "${service_list_log}"; then
      discovered=yes
      break
    fi
    sleep 2
  done
  [[ "${discovered}" == yes ]] || {
    cat "${service_list_log}" >&2 || true
    fail "ROS2 did not discover service ${service_name}"
  }
}

source_ros2
require_file "${IBMW_INSTALL_PREFIX}/lib/cmake/ibmw/ibmw-config.cmake"

rm -rf "${WORK_DIR}"
mkdir -p "${WORK_DIR}/input/msg" "${WORK_DIR}/input/srv" "${WORK_DIR}/ws/src" "${WORK_DIR}/logs"

cat > "${WORK_DIR}/input/msg/NestedLeaf.msg" <<'EOF'
int32 value
string label
EOF

cat > "${WORK_DIR}/input/srv/SetMode.srv" <<'EOF'
string mode
bool enable
---
bool accepted
string reason
EOF

cat > "${WORK_DIR}/input/srv/BlobEcho.srv" <<'EOF'
uint8[] payload
---
uint8[] echoed_payload
EOF

cat > "${WORK_DIR}/input/srv/NestedEcho.srv" <<'EOF'
NestedLeaf leaf
---
NestedLeaf echoed_leaf
EOF

cat > "${WORK_DIR}/input/srv/BatchEcho.srv" <<'EOF'
int32[3] fixed_values
uint8[] payload
NestedLeaf[] leaves
---
int32[] echoed_values
uint8[<=8] bounded_payload
EOF

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" import \
  --ros2-src "${WORK_DIR}/input" \
  --artifact-only \
  --package-name "${PACKAGE_NAME}" \
  --output-dir "${WORK_DIR}/${PACKAGE_NAME}" \
  --target dds_ros2

cp -a "${WORK_DIR}/${PACKAGE_NAME}" "${WORK_DIR}/ws/src/${PACKAGE_NAME}"

colcon build \
  --base-paths "${WORK_DIR}/ws/src" \
  --build-base "${WORK_DIR}/ws/build" \
  --install-base "${WORK_DIR}/ws/install" \
  --merge-install \
  --cmake-args \
    -DPython3_EXECUTABLE=/usr/bin/python3 \
    -DPYTHON_EXECUTABLE=/usr/bin/python3 \
    -DPYTHON_INCLUDE_DIR=/usr/include/python3.10 \
    -DPYTHON_LIBRARY=/usr/lib/x86_64-linux-gnu/libpython3.10.so

set +u
# shellcheck disable=SC1091
source "${WORK_DIR}/ws/install/setup.bash"
set -u

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" \
  "${WORK_DIR}/${PACKAGE_NAME}/${PACKAGE_NAME}_imported_srv_runtime.ibmw.yaml" \
  -o "${WORK_DIR}/runtime_project" \
  --force

handler_file="${WORK_DIR}/runtime_project/src/set_mode_rpc_server_module/set_mode_rpc_server_module_typesupport.inc.cc"
client_file="${WORK_DIR}/runtime_project/src/set_mode_rpc_client_module/set_mode_rpc_client_module_typesupport.inc.cc"
blob_handler_file="${WORK_DIR}/runtime_project/src/blob_echo_rpc_server_module/blob_echo_rpc_server_module_typesupport.inc.cc"
blob_client_file="${WORK_DIR}/runtime_project/src/blob_echo_rpc_client_module/blob_echo_rpc_client_module_typesupport.inc.cc"
nested_handler_file="${WORK_DIR}/runtime_project/src/nested_echo_rpc_server_module/nested_echo_rpc_server_module_typesupport.inc.cc"
nested_client_file="${WORK_DIR}/runtime_project/src/nested_echo_rpc_client_module/nested_echo_rpc_client_module_typesupport.inc.cc"
batch_handler_file="${WORK_DIR}/runtime_project/src/batch_echo_rpc_server_module/batch_echo_rpc_server_module_typesupport.inc.cc"
batch_client_file="${WORK_DIR}/runtime_project/src/batch_echo_rpc_client_module/batch_echo_rpc_client_module_typesupport.inc.cc"
require_file "${handler_file}"
require_file "${client_file}"
require_file "${blob_handler_file}"
require_file "${blob_client_file}"
require_file "${nested_handler_file}"
require_file "${nested_client_file}"
require_file "${batch_handler_file}"
require_file "${batch_client_file}"
patch_set_mode_handler "${handler_file}"
patch_set_mode_client "${client_file}" "${PACKAGE_NAME}"
patch_blob_echo_handler "${blob_handler_file}"
patch_blob_echo_client "${blob_client_file}" "${PACKAGE_NAME}"
patch_nested_echo_handler "${nested_handler_file}"
patch_nested_echo_client "${nested_client_file}" "${PACKAGE_NAME}"
patch_batch_echo_handler "${batch_handler_file}"
patch_batch_echo_client "${batch_client_file}" "${PACKAGE_NAME}"

cmake \
  -S "${WORK_DIR}/runtime_project" \
  -B "${WORK_DIR}/runtime_project_build" \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DCMAKE_PREFIX_PATH="${IBMW_INSTALL_PREFIX};${WORK_DIR}/ws/install;${CMAKE_PREFIX_PATH:-}" \
  -Dibmw_DIR="${IBMW_INSTALL_PREFIX}/lib/cmake/ibmw"
cmake --build "${WORK_DIR}/runtime_project_build" -j"$(nproc)"

app="${WORK_DIR}/runtime_project_build/set_mode_imported_srv_server_app"
client_app="${WORK_DIR}/runtime_project_build/set_mode_imported_srv_client_app"
blob_app="${WORK_DIR}/runtime_project_build/blob_echo_imported_srv_server_app"
blob_client_app="${WORK_DIR}/runtime_project_build/blob_echo_imported_srv_client_app"
nested_app="${WORK_DIR}/runtime_project_build/nested_echo_imported_srv_server_app"
nested_client_app="${WORK_DIR}/runtime_project_build/nested_echo_imported_srv_client_app"
batch_app="${WORK_DIR}/runtime_project_build/batch_echo_imported_srv_server_app"
batch_client_app="${WORK_DIR}/runtime_project_build/batch_echo_imported_srv_client_app"
cfg="${WORK_DIR}/runtime_project/cfg/set_mode_imported_srv_server_app.yaml"
client_cfg="${WORK_DIR}/runtime_project/cfg/set_mode_imported_srv_client_app.yaml"
blob_cfg="${WORK_DIR}/runtime_project/cfg/blob_echo_imported_srv_server_app.yaml"
blob_client_cfg="${WORK_DIR}/runtime_project/cfg/blob_echo_imported_srv_client_app.yaml"
nested_cfg="${WORK_DIR}/runtime_project/cfg/nested_echo_imported_srv_server_app.yaml"
nested_client_cfg="${WORK_DIR}/runtime_project/cfg/nested_echo_imported_srv_client_app.yaml"
batch_cfg="${WORK_DIR}/runtime_project/cfg/batch_echo_imported_srv_server_app.yaml"
batch_client_cfg="${WORK_DIR}/runtime_project/cfg/batch_echo_imported_srv_client_app.yaml"
require_executable "${app}"
require_executable "${client_app}"
require_executable "${blob_app}"
require_executable "${blob_client_app}"
require_executable "${nested_app}"
require_executable "${nested_client_app}"
require_executable "${batch_app}"
require_executable "${batch_client_app}"
require_file "${cfg}"
require_file "${client_cfg}"
require_file "${blob_cfg}"
require_file "${blob_client_cfg}"
require_file "${nested_cfg}"
require_file "${nested_client_cfg}"
require_file "${batch_cfg}"
require_file "${batch_client_cfg}"
if [[ ! -f "${IBMW_INSTALL_PREFIX}/lib/libibmw_ros2_transport.so" && ! -f "${ROS2_EXTENSION_LIB_DIR}/libibmw_ros2_transport.so" ]]; then
  fail "libibmw_ros2_transport.so not found in IBMW_INSTALL_PREFIX/lib or ROS2_EXTENSION_LIB_DIR=${ROS2_EXTENSION_LIB_DIR}"
fi

server_log="${WORK_DIR}/logs/ibmw_set_mode_server.log"
ros2_server_log="${WORK_DIR}/logs/ros2_set_mode_server.log"
client_log="${WORK_DIR}/logs/ibmw_set_mode_client.log"
blob_server_log="${WORK_DIR}/logs/ibmw_blob_echo_server.log"
ros2_blob_server_log="${WORK_DIR}/logs/ros2_blob_echo_server.log"
blob_client_log="${WORK_DIR}/logs/ibmw_blob_echo_client.log"
nested_server_log="${WORK_DIR}/logs/ibmw_nested_echo_server.log"
ros2_nested_server_log="${WORK_DIR}/logs/ros2_nested_echo_server.log"
nested_client_log="${WORK_DIR}/logs/ibmw_nested_echo_client.log"
batch_server_log="${WORK_DIR}/logs/ibmw_batch_echo_server.log"
ros2_batch_server_log="${WORK_DIR}/logs/ros2_batch_echo_server.log"
batch_client_log="${WORK_DIR}/logs/ibmw_batch_echo_client.log"
service_list_log="${WORK_DIR}/logs/ros2_service_list.log"
service_type_log="${WORK_DIR}/logs/ros2_service_type.log"
service_call_log="${WORK_DIR}/logs/ros2_service_call.log"
blob_service_type_log="${WORK_DIR}/logs/ros2_blob_service_type.log"
blob_service_call_log="${WORK_DIR}/logs/ros2_blob_service_call.log"
nested_service_type_log="${WORK_DIR}/logs/ros2_nested_service_type.log"
nested_service_call_log="${WORK_DIR}/logs/ros2_nested_service_call.log"
batch_service_type_log="${WORK_DIR}/logs/ros2_batch_service_type.log"
batch_service_call_log="${WORK_DIR}/logs/ros2_batch_service_call.log"
ros2_server_file="${WORK_DIR}/ros2_set_mode_server.py"
ros2_blob_server_file="${WORK_DIR}/ros2_blob_echo_server.py"
ros2_nested_server_file="${WORK_DIR}/ros2_nested_echo_server.py"
ros2_batch_server_file="${WORK_DIR}/ros2_batch_echo_server.py"
write_ros2_set_mode_server "${ros2_server_file}" "${PACKAGE_NAME}"
write_ros2_blob_echo_server "${ros2_blob_server_file}" "${PACKAGE_NAME}"
write_ros2_nested_echo_server "${ros2_nested_server_file}" "${PACKAGE_NAME}"
write_ros2_batch_echo_server "${ros2_batch_server_file}" "${PACKAGE_NAME}"

export LD_LIBRARY_PATH="${WORK_DIR}/runtime_project_build:${IBMW_INSTALL_PREFIX}/lib:${ROS2_EXTENSION_LIB_DIR}:${ROOT_DIR}/build:/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${WORK_DIR}/runtime_project_build" "${app}" "${cfg}" >"${server_log}" 2>&1 &
server_pid=$!
PIDS+=("${server_pid}")
sleep 3
if ! kill -0 "${server_pid}" 2>/dev/null; then
  cat "${server_log}" >&2 || true
  fail "IBMW imported SetMode server exited before live acceptance"
fi

wait_for_service "${SERVICE_NAME}" "${service_list_log}"

timeout 10 ros2 service type "${SERVICE_NAME}" >"${service_type_log}" 2>&1 || {
  cat "${service_type_log}" >&2 || true
  fail "ros2 service type ${SERVICE_NAME} failed"
}
grep -q "^${PACKAGE_NAME}/srv/SetMode$" "${service_type_log}" || {
  cat "${service_type_log}" >&2 || true
  fail "service ${SERVICE_NAME} did not report type ${PACKAGE_NAME}/srv/SetMode"
}

timeout 15 ros2 service call "${SERVICE_NAME}" "${PACKAGE_NAME}/srv/SetMode" \
  '{mode: auto, enable: true}' >"${service_call_log}" 2>&1 || {
  cat "${service_call_log}" >&2 || true
  fail "ros2 service call ${SERVICE_NAME} failed"
}

grep -Eq 'accepted[=:] ?(True|true)' "${service_call_log}" || {
  cat "${service_call_log}" >&2 || true
  fail "service call did not return accepted=true"
}
grep -q 'accepted:auto' "${service_call_log}" || {
  cat "${service_call_log}" >&2 || true
  fail "service call did not return reason accepted:auto"
}

stop_pid "${server_pid}"

setsid env SERVICE_NAME="${SERVICE_NAME}" /usr/bin/python3 "${ros2_server_file}" >"${ros2_server_log}" 2>&1 &
ros2_server_pid=$!
PIDS+=("${ros2_server_pid}")
sleep 3
if ! kill -0 "${ros2_server_pid}" 2>/dev/null; then
  cat "${ros2_server_log}" >&2 || true
  fail "ROS2 imported SetMode server exited before reverse live acceptance"
fi

wait_for_service "${SERVICE_NAME}" "${service_list_log}"

timeout 20 setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${WORK_DIR}/runtime_project_build" "${client_app}" "${client_cfg}" >"${client_log}" 2>&1 || true

grep -q 'IBMW_IMPORTED_SRV_CLIENT_ACCEPTED_AUTO' "${client_log}" || {
  cat "${client_log}" >&2 || true
  fail "IBMW imported SetMode client did not receive accepted:auto from ROS2 server"
}

stop_pid "${ros2_server_pid}"

setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${WORK_DIR}/runtime_project_build" "${blob_app}" "${blob_cfg}" >"${blob_server_log}" 2>&1 &
blob_server_pid=$!
PIDS+=("${blob_server_pid}")
sleep 3
if ! kill -0 "${blob_server_pid}" 2>/dev/null; then
  cat "${blob_server_log}" >&2 || true
  fail "IBMW imported BlobEcho server exited before live acceptance"
fi

wait_for_service "${BLOB_SERVICE_NAME}" "${service_list_log}"

timeout 10 ros2 service type "${BLOB_SERVICE_NAME}" >"${blob_service_type_log}" 2>&1 || {
  cat "${blob_service_type_log}" >&2 || true
  fail "ros2 service type ${BLOB_SERVICE_NAME} failed"
}
grep -q "^${PACKAGE_NAME}/srv/BlobEcho$" "${blob_service_type_log}" || {
  cat "${blob_service_type_log}" >&2 || true
  fail "service ${BLOB_SERVICE_NAME} did not report type ${PACKAGE_NAME}/srv/BlobEcho"
}

timeout 15 ros2 service call "${BLOB_SERVICE_NAME}" "${PACKAGE_NAME}/srv/BlobEcho" \
  '{payload: [1, 2, 3, 5]}' >"${blob_service_call_log}" 2>&1 || {
  cat "${blob_service_call_log}" >&2 || true
  fail "ros2 service call ${BLOB_SERVICE_NAME} failed"
}

grep -q 'echoed_payload' "${blob_service_call_log}" || {
  cat "${blob_service_call_log}" >&2 || true
  fail "BlobEcho service call did not return echoed_payload"
}
grep -q '1' "${blob_service_call_log}" || {
  cat "${blob_service_call_log}" >&2 || true
  fail "BlobEcho service call did not echo payload contents"
}

stop_pid "${blob_server_pid}"

setsid env BLOB_SERVICE_NAME="${BLOB_SERVICE_NAME}" /usr/bin/python3 "${ros2_blob_server_file}" >"${ros2_blob_server_log}" 2>&1 &
ros2_blob_server_pid=$!
PIDS+=("${ros2_blob_server_pid}")
sleep 3
if ! kill -0 "${ros2_blob_server_pid}" 2>/dev/null; then
  cat "${ros2_blob_server_log}" >&2 || true
  fail "ROS2 imported BlobEcho server exited before reverse live acceptance"
fi

wait_for_service "${BLOB_SERVICE_NAME}" "${service_list_log}"

timeout 20 setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${WORK_DIR}/runtime_project_build" "${blob_client_app}" "${blob_client_cfg}" >"${blob_client_log}" 2>&1 || true

grep -q 'IBMW_IMPORTED_SRV_CLIENT_BLOB_ECHO_OK' "${blob_client_log}" || {
  cat "${blob_client_log}" >&2 || true
  fail "IBMW imported BlobEcho client did not receive echoed payload from ROS2 server"
}

stop_pid "${ros2_blob_server_pid}"

setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${WORK_DIR}/runtime_project_build" "${nested_app}" "${nested_cfg}" >"${nested_server_log}" 2>&1 &
nested_server_pid=$!
PIDS+=("${nested_server_pid}")
sleep 3
if ! kill -0 "${nested_server_pid}" 2>/dev/null; then
  cat "${nested_server_log}" >&2 || true
  fail "IBMW imported NestedEcho server exited before live acceptance"
fi

wait_for_service "${NESTED_SERVICE_NAME}" "${service_list_log}"

timeout 10 ros2 service type "${NESTED_SERVICE_NAME}" >"${nested_service_type_log}" 2>&1 || {
  cat "${nested_service_type_log}" >&2 || true
  fail "ros2 service type ${NESTED_SERVICE_NAME} failed"
}
grep -q "^${PACKAGE_NAME}/srv/NestedEcho$" "${nested_service_type_log}" || {
  cat "${nested_service_type_log}" >&2 || true
  fail "service ${NESTED_SERVICE_NAME} did not report type ${PACKAGE_NAME}/srv/NestedEcho"
}

timeout 15 ros2 service call "${NESTED_SERVICE_NAME}" "${PACKAGE_NAME}/srv/NestedEcho" \
  '{leaf: {value: 42, label: nested}}' >"${nested_service_call_log}" 2>&1 || {
  cat "${nested_service_call_log}" >&2 || true
  fail "ros2 service call ${NESTED_SERVICE_NAME} failed"
}

grep -q 'echoed_leaf' "${nested_service_call_log}" || {
  cat "${nested_service_call_log}" >&2 || true
  fail "NestedEcho service call did not return echoed_leaf"
}
grep -q 'nested' "${nested_service_call_log}" || {
  cat "${nested_service_call_log}" >&2 || true
  fail "NestedEcho service call did not echo nested label"
}

stop_pid "${nested_server_pid}"

setsid env NESTED_SERVICE_NAME="${NESTED_SERVICE_NAME}" /usr/bin/python3 "${ros2_nested_server_file}" >"${ros2_nested_server_log}" 2>&1 &
ros2_nested_server_pid=$!
PIDS+=("${ros2_nested_server_pid}")
sleep 3
if ! kill -0 "${ros2_nested_server_pid}" 2>/dev/null; then
  cat "${ros2_nested_server_log}" >&2 || true
  fail "ROS2 imported NestedEcho server exited before reverse live acceptance"
fi

wait_for_service "${NESTED_SERVICE_NAME}" "${service_list_log}"

timeout 20 setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${WORK_DIR}/runtime_project_build" "${nested_client_app}" "${nested_client_cfg}" >"${nested_client_log}" 2>&1 || true

grep -q 'IBMW_IMPORTED_SRV_CLIENT_NESTED_ECHO_OK' "${nested_client_log}" || {
  cat "${nested_client_log}" >&2 || true
  fail "IBMW imported NestedEcho client did not receive echoed nested payload from ROS2 server"
}

stop_pid "${ros2_nested_server_pid}"

setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${WORK_DIR}/runtime_project_build" "${batch_app}" "${batch_cfg}" >"${batch_server_log}" 2>&1 &
batch_server_pid=$!
PIDS+=("${batch_server_pid}")
sleep 3
if ! kill -0 "${batch_server_pid}" 2>/dev/null; then
  cat "${batch_server_log}" >&2 || true
  fail "IBMW imported BatchEcho server exited before live acceptance"
fi

wait_for_service "${BATCH_SERVICE_NAME}" "${service_list_log}"

timeout 10 ros2 service type "${BATCH_SERVICE_NAME}" >"${batch_service_type_log}" 2>&1 || {
  cat "${batch_service_type_log}" >&2 || true
  fail "ros2 service type ${BATCH_SERVICE_NAME} failed"
}
grep -q "^${PACKAGE_NAME}/srv/BatchEcho$" "${batch_service_type_log}" || {
  cat "${batch_service_type_log}" >&2 || true
  fail "service ${BATCH_SERVICE_NAME} did not report type ${PACKAGE_NAME}/srv/BatchEcho"
}

timeout 15 ros2 service call "${BATCH_SERVICE_NAME}" "${PACKAGE_NAME}/srv/BatchEcho" \
  '{fixed_values: [3, 1, 4], payload: [1, 2, 3, 4, 5, 6, 7, 8, 9], leaves: [{value: 7, label: batch}]}' >"${batch_service_call_log}" 2>&1 || {
  cat "${batch_service_call_log}" >&2 || true
  fail "ros2 service call ${BATCH_SERVICE_NAME} failed"
}

grep -q 'echoed_values' "${batch_service_call_log}" || {
  cat "${batch_service_call_log}" >&2 || true
  fail "BatchEcho service call did not return echoed_values"
}
grep -q 'bounded_payload' "${batch_service_call_log}" || {
  cat "${batch_service_call_log}" >&2 || true
  fail "BatchEcho service call did not return bounded_payload"
}
grep -q '8' "${batch_service_call_log}" || {
  cat "${batch_service_call_log}" >&2 || true
  fail "BatchEcho service call did not preserve bounded payload contents"
}

stop_pid "${batch_server_pid}"

setsid env BATCH_SERVICE_NAME="${BATCH_SERVICE_NAME}" /usr/bin/python3 "${ros2_batch_server_file}" >"${ros2_batch_server_log}" 2>&1 &
ros2_batch_server_pid=$!
PIDS+=("${ros2_batch_server_pid}")
sleep 3
if ! kill -0 "${ros2_batch_server_pid}" 2>/dev/null; then
  cat "${ros2_batch_server_log}" >&2 || true
  fail "ROS2 imported BatchEcho server exited before reverse live acceptance"
fi

wait_for_service "${BATCH_SERVICE_NAME}" "${service_list_log}"

timeout 20 setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${WORK_DIR}/runtime_project_build" "${batch_client_app}" "${batch_client_cfg}" >"${batch_client_log}" 2>&1 || true

grep -q 'IBMW_IMPORTED_SRV_CLIENT_BATCH_ECHO_OK' "${batch_client_log}" || {
  cat "${batch_client_log}" >&2 || true
  fail "IBMW imported BatchEcho client did not receive echoed mixed payload from ROS2 server"
}

python3 - "${WORK_DIR}" <<'PY'
import json
import pathlib
import sys

work = pathlib.Path(sys.argv[1])
cases = [
    ("set_mode", "SetMode", "/set_mode"),
    ("blob_echo", "BlobEcho", "/blob_echo"),
    ("nested_echo", "NestedEcho", "/nested_echo"),
    ("batch_echo", "BatchEcho", "/batch_echo"),
]
summary = {
    "schema_version": 1,
    "artifact": "imported_srv_live_matrix_summary",
    "lane": "imported_srv",
    "uses_rcl_in_ibmw_runtime": False,
    "uses_ros2_cli_as_import_prerequisite": False,
    "cases": [
        {
            "case": key,
            "service_type": service_type,
            "service_name": service_name,
            "ibmw_server_ros2_client_pass": True,
            "ros2_server_ibmw_client_pass": True,
            "status": "pass",
        }
        for key, service_type, service_name in cases
    ],
    "totals": {"selected": len(cases), "pass": len(cases)},
}
path = work / "imported_srv_live_matrix_summary.json"
path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
print(f"[PASS] imported srv live matrix summary: {path}")
PY

echo "PASS: imported custom .srv live bidirectional ROS2/IBMW service acceptance (${WORK_DIR}; SetMode + BlobEcho + NestedEcho + BatchEcho)"
