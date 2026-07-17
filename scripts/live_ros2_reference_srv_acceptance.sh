#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# Live bidirectional ROS2 <-> IBMW acceptance for a reference_existing .srv package.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${1:-${TMPDIR:-/tmp}/ibmw-ros2-reference-srv-live-acceptance}"
PACKAGE_NAME="${PACKAGE_NAME:-example_interfaces}"
ROS2_TYPE="${ROS2_TYPE:-srv/AddTwoInts}"
SERVICE_NAME="${SERVICE_NAME:-/add_two_ints}"
SET_BOOL_SERVICE_NAME="${SET_BOOL_SERVICE_NAME:-/set_bool}"
TRIGGER_SERVICE_NAME="${TRIGGER_SERVICE_NAME:-/trigger}"
EMPTY_SERVICE_NAME="${EMPTY_SERVICE_NAME:-/empty}"
LIFECYCLE_GET_STATE_SERVICE_NAME="${LIFECYCLE_GET_STATE_SERVICE_NAME:-/get_state}"
LIFECYCLE_CHANGE_STATE_SERVICE_NAME="${LIFECYCLE_CHANGE_STATE_SERVICE_NAME:-/change_state}"
LIFECYCLE_GET_AVAILABLE_STATES_SERVICE_NAME="${LIFECYCLE_GET_AVAILABLE_STATES_SERVICE_NAME:-/get_available_states}"
TF2_FRAME_GRAPH_SERVICE_NAME="${TF2_FRAME_GRAPH_SERVICE_NAME:-/frame_graph}"
DIAGNOSTIC_SELF_TEST_SERVICE_NAME="${DIAGNOSTIC_SELF_TEST_SERVICE_NAME:-/self_test}"
SENSOR_SET_CAMERA_INFO_SERVICE_NAME="${SENSOR_SET_CAMERA_INFO_SERVICE_NAME:-/set_camera_info}"
ROS2_SETUP="${ROS2_SETUP:-/opt/ros/humble/setup.bash}"
ROS2_PREFIX="${ROS2_PREFIX:-/opt/ros/humble}"
ROS_DOMAIN_ID="${ROS_DOMAIN_ID:-0}"
IBMW_INSTALL_PREFIX="${IBMW_INSTALL_PREFIX:-${TMPDIR:-/tmp}/ibmw-msggen-validation/ibmw-host-install-phase0}"
ROS2_EXTENSION_LIB_DIR="${ROS2_EXTENSION_LIB_DIR:-${ROOT_DIR}/build/bin}"

PIDS=()

fail() {
  echo "ERROR: $*" >&2
  exit 1
}

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

cleanup() {
  local pid
  for pid in "${PIDS[@]:-}"; do
    stop_pid "${pid}"
  done
}
trap cleanup EXIT

require_file() {
  [[ -f "$1" ]] || fail "required file not found: $1"
}

require_executable() {
  [[ -x "$1" ]] || fail "required executable not found or not executable: $1"
}

assert_host_live_audit_fails_on_ros2_prefix() {
  local package_name=$1
  local ros2_type=$2
  local reference_dir=$3
  local audit_log=$4
  shift 4
  local artifacts=("$@")
  local cmd=(
    python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" import
    --ros2-prefix "${ROS2_PREFIX}"
    --ros2-package "${package_name}"
    --ros2-type "${ros2_type}"
    --artifact-only
    --output-dir "${reference_dir}"
    --format json
  )
  local artifact
  for artifact in "${artifacts[@]}"; do
    cmd+=(--target-artifact "${artifact}")
  done
  "${cmd[@]}" >"${audit_log}"

  python3 - "${audit_log}" "${ROS2_PREFIX}" <<'PY'
import json
import pathlib
import sys

report = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
ros2_prefix = sys.argv[2]
audit = report["target_artifact_audit"]
if audit["status"] != "fail":
    raise SystemExit(f"expected host-live audit to fail, got {audit['status']}")
if ros2_prefix not in audit["forbidden_prefixes"]:
    raise SystemExit(f"expected forbidden host prefix {ros2_prefix} in {audit['forbidden_prefixes']}")
if not any(ros2_prefix in reason for reason in audit["reasons"]):
    raise SystemExit(f"expected audit reasons to mention {ros2_prefix}: {audit['reasons']}")
PY
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
  sleep 2
}

patch_add_two_ints_handler() {
  local handler_file=$1
  python3 - "${handler_file}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
begin = "  // IBMW_RPC_SERVER_HANDLER_BODY_ADD_TWO_INTS_BEGIN\n"
end = "  // IBMW_RPC_SERVER_HANDLER_BODY_ADD_TWO_INTS_END"
start = text.index(begin) + len(begin)
finish = text.index(end)
body = """  rsp.sum = req.a + req.b;
  return ibmw::rpc::Status();
"""
path.write_text(text[:start] + body + text[finish:], encoding="utf-8")
PY
}

patch_add_two_ints_client() {
  local client_file=$1
  local package_name=$2
  python3 - "${client_file}" "${package_name}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
package_name = sys.argv[2]
text = path.read_text(encoding="utf-8")

req_begin = "    // IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_ADD_TWO_INTS_BEGIN\n"
req_end = "    // IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_ADD_TWO_INTS_END"
req_start = text.index(req_begin) + len(req_begin)
req_finish = text.index(req_end)
req_body = """    req.a = 2;
    req.b = 3;
"""
text = text[:req_start] + req_body + text[req_finish:]

rsp_begin = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_ADD_TWO_INTS_BEGIN\n"
rsp_end = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_ADD_TWO_INTS_END"
rsp_start = text.index(rsp_begin) + len(rsp_begin)
rsp_finish = text.index(rsp_end)
rsp_body = """      MW_INFO("Client get rpc ret, status: {}, rsp: {}",
              status.ToString(), __PACKAGE_NAME__::srv::to_yaml(rsp));
      if (rsp.sum == 5) {
        MW_INFO("IBMW_REFERENCE_SRV_CLIENT_ADD_TWO_INTS_OK");
        self->run_flag_ = false;
      } else {
        MW_ERROR("Unexpected AddTwoInts response sum={}", rsp.sum);
        self->run_flag_ = false;
      }
""".replace("__PACKAGE_NAME__", package_name)
path.write_text(text[:rsp_start] + rsp_body + text[rsp_finish:], encoding="utf-8")
PY
}

write_ros2_add_two_ints_server() {
  local server_file=$1
  cat > "${server_file}" <<'PY'
#!/usr/bin/env python3
import os
import rclpy
from example_interfaces.srv import AddTwoInts


def main():
    service_name = os.environ.get("SERVICE_NAME", "/add_two_ints")
    rclpy.init()
    node = rclpy.create_node("ibmw_reference_add_two_ints_ros2_server")

    def handle(req, rsp):
        rsp.sum = req.a + req.b
        node.get_logger().info(f"handled AddTwoInts request a={req.a} b={req.b} sum={rsp.sum}")
        return rsp

    node.create_service(AddTwoInts, service_name, handle)
    node.get_logger().info(f"ready {service_name}")
    rclpy.spin(node)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
PY
  chmod +x "${server_file}"
}

patch_set_bool_handler() {
  local handler_file=$1
  python3 - "${handler_file}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
begin = "  // IBMW_RPC_SERVER_HANDLER_BODY_SET_BOOL_BEGIN\n"
end = "  // IBMW_RPC_SERVER_HANDLER_BODY_SET_BOOL_END"
start = text.index(begin) + len(begin)
finish = text.index(end)
body = """  rsp.success = req.data;
  rsp.message = req.data ? "accepted:true" : "rejected:false";
  return ibmw::rpc::Status();
"""
path.write_text(text[:start] + body + text[finish:], encoding="utf-8")
PY
}

patch_set_bool_client() {
  local client_file=$1
  local package_name=$2
  python3 - "${client_file}" "${package_name}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
package_name = sys.argv[2]
text = path.read_text(encoding="utf-8")

req_begin = "    // IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_SET_BOOL_BEGIN\n"
req_end = "    // IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_SET_BOOL_END"
req_start = text.index(req_begin) + len(req_begin)
req_finish = text.index(req_end)
req_body = """    req.data = true;
"""
text = text[:req_start] + req_body + text[req_finish:]

rsp_begin = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_SET_BOOL_BEGIN\n"
rsp_end = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_SET_BOOL_END"
rsp_start = text.index(rsp_begin) + len(rsp_begin)
rsp_finish = text.index(rsp_end)
rsp_body = """      MW_INFO("Client get rpc ret, status: {}, rsp: {}",
              status.ToString(), __PACKAGE_NAME__::srv::to_yaml(rsp));
      if (rsp.success && rsp.message == "accepted:true") {
        MW_INFO("IBMW_REFERENCE_SRV_CLIENT_SET_BOOL_OK");
        self->run_flag_ = false;
      } else {
        MW_ERROR("Unexpected SetBool response success={}, message={}", rsp.success, rsp.message);
        self->run_flag_ = false;
      }
""".replace("__PACKAGE_NAME__", package_name)
path.write_text(text[:rsp_start] + rsp_body + text[rsp_finish:], encoding="utf-8")
PY
}

write_ros2_set_bool_server() {
  local server_file=$1
  cat > "${server_file}" <<'PY'
#!/usr/bin/env python3
import os
import rclpy
from std_srvs.srv import SetBool


def main():
    service_name = os.environ.get("SET_BOOL_SERVICE_NAME", "/set_bool")
    rclpy.init()
    node = rclpy.create_node("ibmw_reference_set_bool_ros2_server")

    def handle(req, rsp):
        rsp.success = bool(req.data)
        rsp.message = "accepted:true" if rsp.success else "rejected:false"
        node.get_logger().info(f"handled SetBool request data={req.data} success={rsp.success}")
        return rsp

    node.create_service(SetBool, service_name, handle)
    node.get_logger().info(f"ready {service_name}")
    rclpy.spin(node)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
PY
  chmod +x "${server_file}"
}

patch_trigger_handler() {
  local handler_file=$1
  python3 - "${handler_file}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
begin = "  // IBMW_RPC_SERVER_HANDLER_BODY_TRIGGER_BEGIN\n"
end = "  // IBMW_RPC_SERVER_HANDLER_BODY_TRIGGER_END"
start = text.index(begin) + len(begin)
finish = text.index(end)
body = """  rsp.success = true;
  rsp.message = "triggered:ok";
  return ibmw::rpc::Status();
"""
path.write_text(text[:start] + body + text[finish:], encoding="utf-8")
PY
}

patch_trigger_client() {
  local client_file=$1
  local package_name=$2
  python3 - "${client_file}" "${package_name}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
package_name = sys.argv[2]
text = path.read_text(encoding="utf-8")

rsp_begin = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_TRIGGER_BEGIN\n"
rsp_end = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_TRIGGER_END"
rsp_start = text.index(rsp_begin) + len(rsp_begin)
rsp_finish = text.index(rsp_end)
rsp_body = """      MW_INFO("Client get rpc ret, status: {}, rsp: {}",
               status.ToString(), __PACKAGE_NAME__::srv::to_yaml(rsp));
      if (rsp.success && rsp.message == "triggered:ok") {
        MW_INFO("IBMW_REFERENCE_SRV_CLIENT_TRIGGER_OK");
        self->run_flag_ = false;
      } else {
        MW_ERROR("Unexpected Trigger response success={}, message={}", rsp.success, rsp.message);
        self->run_flag_ = false;
      }
""".replace("__PACKAGE_NAME__", package_name)
path.write_text(text[:rsp_start] + rsp_body + text[rsp_finish:], encoding="utf-8")
PY
}

write_ros2_trigger_server() {
  local server_file=$1
  cat > "${server_file}" <<'PY'
#!/usr/bin/env python3
import os
import rclpy
from std_srvs.srv import Trigger


def main():
    service_name = os.environ.get("TRIGGER_SERVICE_NAME", "/trigger")
    rclpy.init()
    node = rclpy.create_node("ibmw_reference_trigger_ros2_server")

    def handle(req, rsp):
        rsp.success = True
        rsp.message = "triggered:ok"
        node.get_logger().info("handled Trigger request")
        return rsp

    node.create_service(Trigger, service_name, handle)
    node.get_logger().info(f"ready {service_name}")
    rclpy.spin(node)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
PY
  chmod +x "${server_file}"
}

patch_empty_handler() {
  local handler_file=$1
  python3 - "${handler_file}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
begin = "  // IBMW_RPC_SERVER_HANDLER_BODY_EMPTY_BEGIN\n"
end = "  // IBMW_RPC_SERVER_HANDLER_BODY_EMPTY_END"
start = text.index(begin) + len(begin)
finish = text.index(end)
body = """  return ibmw::rpc::Status();
"""
path.write_text(text[:start] + body + text[finish:], encoding="utf-8")
PY
}

patch_empty_client() {
  local client_file=$1
  local package_name=$2
  python3 - "${client_file}" "${package_name}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
package_name = sys.argv[2]
text = path.read_text(encoding="utf-8")

rsp_begin = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_EMPTY_BEGIN\n"
rsp_end = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_EMPTY_END"
rsp_start = text.index(rsp_begin) + len(rsp_begin)
rsp_finish = text.index(rsp_end)
rsp_body = """      MW_INFO("Client get rpc ret, status: {}, rsp: {}",
               status.ToString(), __PACKAGE_NAME__::srv::to_yaml(rsp));
      MW_INFO("IBMW_REFERENCE_SRV_CLIENT_EMPTY_OK");
      self->run_flag_ = false;
""".replace("__PACKAGE_NAME__", package_name)
path.write_text(text[:rsp_start] + rsp_body + text[rsp_finish:], encoding="utf-8")
PY
}

write_ros2_empty_server() {
  local server_file=$1
  cat > "${server_file}" <<'PY'
#!/usr/bin/env python3
import os
import rclpy
from std_srvs.srv import Empty


def main():
    service_name = os.environ.get("EMPTY_SERVICE_NAME", "/empty")
    rclpy.init()
    node = rclpy.create_node("ibmw_reference_empty_ros2_server")

    def handle(req, rsp):
        node.get_logger().info("handled Empty request")
        return rsp

    node.create_service(Empty, service_name, handle)
    node.get_logger().info(f"ready {service_name}")
    rclpy.spin(node)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
PY
  chmod +x "${server_file}"
}

patch_get_state_handler() {
  local handler_file=$1
  python3 - "${handler_file}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
begin = "  // IBMW_RPC_SERVER_HANDLER_BODY_GET_STATE_BEGIN\n"
end = "  // IBMW_RPC_SERVER_HANDLER_BODY_GET_STATE_END"
start = text.index(begin) + len(begin)
finish = text.index(end)
body = """  rsp.current_state.id = 3;
  rsp.current_state.label = "active";
  return ibmw::rpc::Status();
"""
path.write_text(text[:start] + body + text[finish:], encoding="utf-8")
PY
}

patch_get_state_client() {
  local client_file=$1
  local package_name=$2
  python3 - "${client_file}" "${package_name}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
package_name = sys.argv[2]
text = path.read_text(encoding="utf-8")

rsp_begin = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_GET_STATE_BEGIN\n"
rsp_end = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_GET_STATE_END"
rsp_start = text.index(rsp_begin) + len(rsp_begin)
rsp_finish = text.index(rsp_end)
rsp_body = """      MW_INFO("Client get rpc ret, status: {}, rsp: {}",
               status.ToString(), __PACKAGE_NAME__::srv::to_yaml(rsp));
      if (rsp.current_state.id == 3 && rsp.current_state.label == "active") {
        MW_INFO("IBMW_REFERENCE_SRV_CLIENT_GET_STATE_OK");
        self->run_flag_ = false;
      } else {
        MW_ERROR("Unexpected GetState response id={}, label={}",
                 rsp.current_state.id, rsp.current_state.label);
        self->run_flag_ = false;
      }
""".replace("__PACKAGE_NAME__", package_name)
path.write_text(text[:rsp_start] + rsp_body + text[rsp_finish:], encoding="utf-8")
PY
}

write_ros2_get_state_server() {
  local server_file=$1
  cat > "${server_file}" <<'PY'
#!/usr/bin/env python3
import os
import rclpy
from lifecycle_msgs.srv import GetState


def main():
    service_name = os.environ.get("LIFECYCLE_GET_STATE_SERVICE_NAME", "/get_state")
    rclpy.init()
    node = rclpy.create_node("ibmw_reference_lifecycle_get_state_ros2_server")

    def handle(req, rsp):
        rsp.current_state.id = 3
        rsp.current_state.label = "active"
        node.get_logger().info("handled GetState request")
        return rsp

    node.create_service(GetState, service_name, handle)
    node.get_logger().info(f"ready {service_name}")
    rclpy.spin(node)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
PY
  chmod +x "${server_file}"
}

patch_change_state_handler() {
  local handler_file=$1
  python3 - "${handler_file}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
begin = "  // IBMW_RPC_SERVER_HANDLER_BODY_CHANGE_STATE_BEGIN\n"
end = "  // IBMW_RPC_SERVER_HANDLER_BODY_CHANGE_STATE_END"
start = text.index(begin) + len(begin)
finish = text.index(end)
body = """  rsp.success = req.transition.id == 1 && req.transition.label == "configure";
  return ibmw::rpc::Status();
"""
path.write_text(text[:start] + body + text[finish:], encoding="utf-8")
PY
}

patch_change_state_client() {
  local client_file=$1
  local package_name=$2
  python3 - "${client_file}" "${package_name}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
package_name = sys.argv[2]
text = path.read_text(encoding="utf-8")

req_begin = "    // IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_CHANGE_STATE_BEGIN\n"
req_end = "    // IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_CHANGE_STATE_END"
req_start = text.index(req_begin) + len(req_begin)
req_finish = text.index(req_end)
req_body = """    req.transition.id = 1;
    req.transition.label = "configure";
"""
text = text[:req_start] + req_body + text[req_finish:]

rsp_begin = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_CHANGE_STATE_BEGIN\n"
rsp_end = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_CHANGE_STATE_END"
rsp_start = text.index(rsp_begin) + len(rsp_begin)
rsp_finish = text.index(rsp_end)
rsp_body = """      MW_INFO("Client get rpc ret, status: {}, rsp: {}",
               status.ToString(), __PACKAGE_NAME__::srv::to_yaml(rsp));
      if (rsp.success) {
        MW_INFO("IBMW_REFERENCE_SRV_CLIENT_CHANGE_STATE_OK");
        self->run_flag_ = false;
      } else {
        MW_ERROR("Unexpected ChangeState response success={}", rsp.success);
        self->run_flag_ = false;
      }
""".replace("__PACKAGE_NAME__", package_name)
path.write_text(text[:rsp_start] + rsp_body + text[rsp_finish:], encoding="utf-8")
PY
}

write_ros2_change_state_server() {
  local server_file=$1
  cat > "${server_file}" <<'PY'
#!/usr/bin/env python3
import os
import rclpy
from lifecycle_msgs.srv import ChangeState


def main():
    service_name = os.environ.get("LIFECYCLE_CHANGE_STATE_SERVICE_NAME", "/change_state")
    rclpy.init()
    node = rclpy.create_node("ibmw_reference_lifecycle_change_state_ros2_server")

    def handle(req, rsp):
        rsp.success = req.transition.id == 1 and req.transition.label == "configure"
        node.get_logger().info(
            f"handled ChangeState request id={req.transition.id} label={req.transition.label}"
        )
        return rsp

    node.create_service(ChangeState, service_name, handle)
    node.get_logger().info(f"ready {service_name}")
    rclpy.spin(node)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
PY
  chmod +x "${server_file}"
}

patch_get_available_states_handler() {
  local handler_file=$1
  python3 - "${handler_file}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
begin = "  // IBMW_RPC_SERVER_HANDLER_BODY_GET_AVAILABLE_STATES_BEGIN\n"
end = "  // IBMW_RPC_SERVER_HANDLER_BODY_GET_AVAILABLE_STATES_END"
start = text.index(begin) + len(begin)
finish = text.index(end)
body = """  rsp.available_states.resize(2);
  rsp.available_states[0].id = 1;
  rsp.available_states[0].label = "unconfigured";
  rsp.available_states[1].id = 3;
  rsp.available_states[1].label = "active";
  return ibmw::rpc::Status();
"""
path.write_text(text[:start] + body + text[finish:], encoding="utf-8")
PY
}

patch_get_available_states_client() {
  local client_file=$1
  local package_name=$2
  python3 - "${client_file}" "${package_name}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
package_name = sys.argv[2]
text = path.read_text(encoding="utf-8")

rsp_begin = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_GET_AVAILABLE_STATES_BEGIN\n"
rsp_end = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_GET_AVAILABLE_STATES_END"
rsp_start = text.index(rsp_begin) + len(rsp_begin)
rsp_finish = text.index(rsp_end)
rsp_body = """      MW_INFO("Client get rpc ret, status: {}, rsp: {}",
               status.ToString(), __PACKAGE_NAME__::srv::to_yaml(rsp));
      if (rsp.available_states.size() == 2 &&
          rsp.available_states[0].id == 1 &&
          rsp.available_states[0].label == "unconfigured" &&
          rsp.available_states[1].id == 3 &&
          rsp.available_states[1].label == "active") {
        MW_INFO("IBMW_REFERENCE_SRV_CLIENT_GET_AVAILABLE_STATES_OK");
        self->run_flag_ = false;
      } else {
        MW_ERROR("Unexpected GetAvailableStates response size={}", rsp.available_states.size());
        self->run_flag_ = false;
      }
""".replace("__PACKAGE_NAME__", package_name)
path.write_text(text[:rsp_start] + rsp_body + text[rsp_finish:], encoding="utf-8")
PY
}

write_ros2_get_available_states_server() {
  local server_file=$1
  cat > "${server_file}" <<'PY'
#!/usr/bin/env python3
import os
import rclpy
from lifecycle_msgs.msg import State
from lifecycle_msgs.srv import GetAvailableStates


def main():
    service_name = os.environ.get("LIFECYCLE_GET_AVAILABLE_STATES_SERVICE_NAME", "/get_available_states")
    rclpy.init()
    node = rclpy.create_node("ibmw_reference_lifecycle_get_available_states_ros2_server")

    def handle(req, rsp):
        rsp.available_states = [
            State(id=1, label="unconfigured"),
            State(id=3, label="active"),
        ]
        node.get_logger().info("handled GetAvailableStates request")
        return rsp

    node.create_service(GetAvailableStates, service_name, handle)
    node.get_logger().info(f"ready {service_name}")
    rclpy.spin(node)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
PY
  chmod +x "${server_file}"
}

patch_frame_graph_handler() {
  local handler_file=$1
  python3 - "${handler_file}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
begin = "  // IBMW_RPC_SERVER_HANDLER_BODY_FRAME_GRAPH_BEGIN\n"
end = "  // IBMW_RPC_SERVER_HANDLER_BODY_FRAME_GRAPH_END"
start = text.index(begin) + len(begin)
finish = text.index(end)
body = """  rsp.frame_yaml = "map:\\n  parent: odom\\nbase_link:\\n  parent: map\\n";
  return ibmw::rpc::Status();
"""
path.write_text(text[:start] + body + text[finish:], encoding="utf-8")
PY
}

patch_frame_graph_client() {
  local client_file=$1
  local package_name=$2
  python3 - "${client_file}" "${package_name}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
package_name = sys.argv[2]
text = path.read_text(encoding="utf-8")

rsp_begin = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_FRAME_GRAPH_BEGIN\n"
rsp_end = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_FRAME_GRAPH_END"
rsp_start = text.index(rsp_begin) + len(rsp_begin)
rsp_finish = text.index(rsp_end)
rsp_body = """      MW_INFO("Client get rpc ret, status: {}, rsp: {}",
               status.ToString(), __PACKAGE_NAME__::srv::to_yaml(rsp));
      if (rsp.frame_yaml.find("base_link") != std::string::npos &&
          rsp.frame_yaml.find("parent: map") != std::string::npos) {
        MW_INFO("IBMW_REFERENCE_SRV_CLIENT_FRAME_GRAPH_OK");
        self->run_flag_ = false;
      } else {
        MW_ERROR("Unexpected FrameGraph response yaml={}", rsp.frame_yaml);
        self->run_flag_ = false;
      }
""".replace("__PACKAGE_NAME__", package_name)
path.write_text(text[:rsp_start] + rsp_body + text[rsp_finish:], encoding="utf-8")
PY
}

write_ros2_frame_graph_server() {
  local server_file=$1
  cat > "${server_file}" <<'PY'
#!/usr/bin/env python3
import os
import rclpy
from tf2_msgs.srv import FrameGraph


def main():
    service_name = os.environ.get("TF2_FRAME_GRAPH_SERVICE_NAME", "/frame_graph")
    rclpy.init()
    node = rclpy.create_node("ibmw_reference_tf2_frame_graph_ros2_server")

    def handle(req, rsp):
        rsp.frame_yaml = "map:\n  parent: odom\nbase_link:\n  parent: map\n"
        node.get_logger().info("handled FrameGraph request")
        return rsp

    node.create_service(FrameGraph, service_name, handle)
    node.get_logger().info(f"ready {service_name}")
    rclpy.spin(node)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
PY
  chmod +x "${server_file}"
}

patch_self_test_handler() {
  local handler_file=$1
  python3 - "${handler_file}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
begin = "  // IBMW_RPC_SERVER_HANDLER_BODY_SELF_TEST_BEGIN\n"
end = "  // IBMW_RPC_SERVER_HANDLER_BODY_SELF_TEST_END"
start = text.index(begin) + len(begin)
finish = text.index(end)
body = """  rsp.id = "ibmw-diagnostic-self-test";
  rsp.passed = 1;
  rsp.status.resize(1);
  rsp.status[0].level = 0;
  rsp.status[0].name = "ibmw_reference_self_test";
  rsp.status[0].message = "diagnostics ok";
  rsp.status[0].hardware_id = "ibmw-host";
  rsp.status[0].values.resize(1);
  rsp.status[0].values[0].key = "transport";
  rsp.status[0].values[0].value = "ros2_reference_existing";
  return ibmw::rpc::Status();
"""
path.write_text(text[:start] + body + text[finish:], encoding="utf-8")
PY
}

patch_self_test_client() {
  local client_file=$1
  local package_name=$2
  python3 - "${client_file}" "${package_name}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
package_name = sys.argv[2]
text = path.read_text(encoding="utf-8")

rsp_begin = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_SELF_TEST_BEGIN\n"
rsp_end = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_SELF_TEST_END"
rsp_start = text.index(rsp_begin) + len(rsp_begin)
rsp_finish = text.index(rsp_end)
rsp_body = """      MW_INFO("Client get rpc ret, status: {}, rsp: {}",
               status.ToString(), __PACKAGE_NAME__::srv::to_yaml(rsp));
      if (rsp.passed == 1 &&
          rsp.id == "ibmw-diagnostic-self-test" &&
          rsp.status.size() == 1 &&
          rsp.status[0].name == "ibmw_reference_self_test" &&
          rsp.status[0].values.size() == 1 &&
          rsp.status[0].values[0].key == "transport" &&
          rsp.status[0].values[0].value == "ros2_reference_existing") {
        MW_INFO("IBMW_REFERENCE_SRV_CLIENT_SELF_TEST_OK");
        self->run_flag_ = false;
      } else {
        MW_ERROR("Unexpected SelfTest response id={}, passed={}, status_size={}",
                 rsp.id, rsp.passed, rsp.status.size());
        self->run_flag_ = false;
      }
""".replace("__PACKAGE_NAME__", package_name)
path.write_text(text[:rsp_start] + rsp_body + text[rsp_finish:], encoding="utf-8")
PY
}

write_ros2_self_test_server() {
  local server_file=$1
  cat > "${server_file}" <<'PY'
#!/usr/bin/env python3
import os
import rclpy
from diagnostic_msgs.msg import DiagnosticStatus, KeyValue
from diagnostic_msgs.srv import SelfTest


def main():
    service_name = os.environ.get("DIAGNOSTIC_SELF_TEST_SERVICE_NAME", "/self_test")
    rclpy.init()
    node = rclpy.create_node("ibmw_reference_diagnostic_self_test_ros2_server")

    def handle(req, rsp):
        rsp.id = "ibmw-diagnostic-self-test"
        rsp.passed = b"\x01"
        rsp.status = [
            DiagnosticStatus(
                level=b"\x00",
                name="ibmw_reference_self_test",
                message="diagnostics ok",
                hardware_id="ibmw-host",
                values=[KeyValue(key="transport", value="ros2_reference_existing")],
            )
        ]
        node.get_logger().info("handled SelfTest request")
        return rsp

    node.create_service(SelfTest, service_name, handle)
    node.get_logger().info(f"ready {service_name}")
    rclpy.spin(node)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
PY
  chmod +x "${server_file}"
}

patch_set_camera_info_handler() {
  local handler_file=$1
  python3 - "${handler_file}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
begin = "  // IBMW_RPC_SERVER_HANDLER_BODY_SET_CAMERA_INFO_BEGIN\n"
end = "  // IBMW_RPC_SERVER_HANDLER_BODY_SET_CAMERA_INFO_END"
start = text.index(begin) + len(begin)
finish = text.index(end)
body = """  const bool valid =
      req.camera_info.width == 640 &&
      req.camera_info.height == 480 &&
      req.camera_info.header.frame_id == "camera_optical_frame" &&
      req.camera_info.distortion_model == "plumb_bob" &&
      req.camera_info.d.size() == 5 &&
      req.camera_info.k[0] == 500.0 &&
      req.camera_info.p[10] == 1.0 &&
      req.camera_info.roi.width == 320 &&
      req.camera_info.roi.do_rectify;
  rsp.success = valid;
  rsp.status_message = valid ? "camera_info accepted" : "camera_info rejected";
  return ibmw::rpc::Status();
"""
path.write_text(text[:start] + body + text[finish:], encoding="utf-8")
PY
}

patch_set_camera_info_client() {
  local client_file=$1
  local package_name=$2
  python3 - "${client_file}" "${package_name}" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
package_name = sys.argv[2]
text = path.read_text(encoding="utf-8")

req_begin = "    // IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_SET_CAMERA_INFO_BEGIN\n"
req_end = "    // IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_SET_CAMERA_INFO_END"
req_start = text.index(req_begin) + len(req_begin)
req_finish = text.index(req_end)
req_body = """    req.camera_info.header.stamp.sec = 12;
    req.camera_info.header.stamp.nanosec = 345000000;
    req.camera_info.header.frame_id = "camera_optical_frame";
    req.camera_info.height = 480;
    req.camera_info.width = 640;
    req.camera_info.distortion_model = "plumb_bob";
    req.camera_info.d = {0.1, -0.01, 0.001, 0.002, 0.0};
    req.camera_info.k[0] = 500.0;
    req.camera_info.k[1] = 0.0;
    req.camera_info.k[2] = 320.0;
    req.camera_info.k[3] = 0.0;
    req.camera_info.k[4] = 501.0;
    req.camera_info.k[5] = 240.0;
    req.camera_info.k[6] = 0.0;
    req.camera_info.k[7] = 0.0;
    req.camera_info.k[8] = 1.0;
    req.camera_info.r[0] = 1.0;
    req.camera_info.r[4] = 1.0;
    req.camera_info.r[8] = 1.0;
    req.camera_info.p[0] = 500.0;
    req.camera_info.p[2] = 320.0;
    req.camera_info.p[5] = 501.0;
    req.camera_info.p[6] = 240.0;
    req.camera_info.p[10] = 1.0;
    req.camera_info.binning_x = 1;
    req.camera_info.binning_y = 1;
    req.camera_info.roi.x_offset = 10;
    req.camera_info.roi.y_offset = 20;
    req.camera_info.roi.height = 240;
    req.camera_info.roi.width = 320;
    req.camera_info.roi.do_rectify = true;
"""
text = text[:req_start] + req_body + text[req_finish:]

rsp_begin = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_SET_CAMERA_INFO_BEGIN\n"
rsp_end = "      // IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_SET_CAMERA_INFO_END"
rsp_start = text.index(rsp_begin) + len(rsp_begin)
rsp_finish = text.index(rsp_end)
rsp_body = """      MW_INFO("Client get rpc ret, status: {}, rsp: {}",
               status.ToString(), __PACKAGE_NAME__::srv::to_yaml(rsp));
      if (rsp.success && rsp.status_message == "camera_info accepted") {
        MW_INFO("IBMW_REFERENCE_SRV_CLIENT_SET_CAMERA_INFO_OK");
        self->run_flag_ = false;
      } else {
        MW_ERROR("Unexpected SetCameraInfo response success={}, message={}",
                 rsp.success, rsp.status_message);
        self->run_flag_ = false;
      }
""".replace("__PACKAGE_NAME__", package_name)
path.write_text(text[:rsp_start] + rsp_body + text[rsp_finish:], encoding="utf-8")
PY
}

write_ros2_set_camera_info_server() {
  local server_file=$1
  cat > "${server_file}" <<'PY'
#!/usr/bin/env python3
import os
import rclpy
from sensor_msgs.srv import SetCameraInfo


def main():
    service_name = os.environ.get("SENSOR_SET_CAMERA_INFO_SERVICE_NAME", "/set_camera_info")
    rclpy.init()
    node = rclpy.create_node("ibmw_reference_sensor_set_camera_info_ros2_server")

    def handle(req, rsp):
        info = req.camera_info
        valid = (
            info.width == 640
            and info.height == 480
            and info.header.frame_id == "camera_optical_frame"
            and info.distortion_model == "plumb_bob"
            and len(info.d) == 5
            and info.k[0] == 500.0
            and info.p[10] == 1.0
            and info.roi.width == 320
            and bool(info.roi.do_rectify)
        )
        rsp.success = valid
        rsp.status_message = "camera_info accepted" if valid else "camera_info rejected"
        node.get_logger().info(f"handled SetCameraInfo request valid={valid}")
        return rsp

    node.create_service(SetCameraInfo, service_name, handle)
    node.get_logger().info(f"ready {service_name}")
    rclpy.spin(node)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == "__main__":
    main()
PY
  chmod +x "${server_file}"
}

source_ros2
require_file "${IBMW_INSTALL_PREFIX}/lib/cmake/ibmw/ibmw-config.cmake"
require_file "${ROS2_PREFIX}/share/${PACKAGE_NAME}/package.xml"

rm -rf "${WORK_DIR}"
mkdir -p "${WORK_DIR}/logs"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" import \
  --ros2-prefix "${ROS2_PREFIX}" \
  --ros2-package "${PACKAGE_NAME}" \
  --ros2-type "${ROS2_TYPE}" \
  --artifact-only \
  --output-dir "${WORK_DIR}/reference" \
  --format json >"${WORK_DIR}/logs/reference_import_report.json"

runtime_yaml="${WORK_DIR}/reference/${PACKAGE_NAME}_reference_existing_srv_runtime.ibmw.yaml"
require_file "${runtime_yaml}"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" \
  "${runtime_yaml}" \
  -o "${WORK_DIR}/runtime_project" \
  --force

handler_file="${WORK_DIR}/runtime_project/src/add_two_ints_rpc_server_module/add_two_ints_rpc_server_module_typesupport.inc.cc"
client_file="${WORK_DIR}/runtime_project/src/add_two_ints_rpc_client_module/add_two_ints_rpc_client_module_typesupport.inc.cc"
require_file "${handler_file}"
require_file "${client_file}"
patch_add_two_ints_handler "${handler_file}"
patch_add_two_ints_client "${client_file}" "${PACKAGE_NAME}"

cmake \
  -S "${WORK_DIR}/runtime_project" \
  -B "${WORK_DIR}/runtime_project_build" \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DCMAKE_PREFIX_PATH="${IBMW_INSTALL_PREFIX};${ROS2_PREFIX};${CMAKE_PREFIX_PATH:-}" \
  -Dibmw_DIR="${IBMW_INSTALL_PREFIX}/lib/cmake/ibmw"
cmake --build "${WORK_DIR}/runtime_project_build" -j"$(nproc)"

app="${WORK_DIR}/runtime_project_build/add_two_ints_imported_srv_server_app"
client_app="${WORK_DIR}/runtime_project_build/add_two_ints_imported_srv_client_app"
cfg="${WORK_DIR}/runtime_project/cfg/add_two_ints_imported_srv_server_app.yaml"
client_cfg="${WORK_DIR}/runtime_project/cfg/add_two_ints_imported_srv_client_app.yaml"
require_executable "${app}"
require_executable "${client_app}"
require_file "${cfg}"
require_file "${client_cfg}"
if [[ ! -f "${IBMW_INSTALL_PREFIX}/lib/libibmw_ros2_transport.so" && ! -f "${ROS2_EXTENSION_LIB_DIR}/libibmw_ros2_transport.so" ]]; then
  fail "libibmw_ros2_transport.so not found in IBMW_INSTALL_PREFIX/lib or ROS2_EXTENSION_LIB_DIR=${ROS2_EXTENSION_LIB_DIR}"
fi

export LD_LIBRARY_PATH="${WORK_DIR}/runtime_project_build:${IBMW_INSTALL_PREFIX}/lib:${ROS2_EXTENSION_LIB_DIR}:${ROOT_DIR}/build:/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"

server_log="${WORK_DIR}/logs/ibmw_add_two_ints_server.log"
ros2_server_log="${WORK_DIR}/logs/ros2_add_two_ints_server.log"
client_log="${WORK_DIR}/logs/ibmw_add_two_ints_client.log"
service_list_log="${WORK_DIR}/logs/ros2_service_list.log"
service_type_log="${WORK_DIR}/logs/ros2_service_type.log"
service_call_log="${WORK_DIR}/logs/ros2_service_call.log"
ros2_server_file="${WORK_DIR}/ros2_add_two_ints_server.py"
write_ros2_add_two_ints_server "${ros2_server_file}"

setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${WORK_DIR}/runtime_project_build" "${app}" "${cfg}" >"${server_log}" 2>&1 &
server_pid=$!
PIDS+=("${server_pid}")
sleep 3
if ! kill -0 "${server_pid}" 2>/dev/null; then
  cat "${server_log}" >&2 || true
  fail "IBMW reference AddTwoInts server exited before live acceptance"
fi

wait_for_service "${SERVICE_NAME}" "${service_list_log}"

timeout 10 ros2 service type "${SERVICE_NAME}" >"${service_type_log}" 2>&1 || {
  cat "${service_type_log}" >&2 || true
  fail "ros2 service type ${SERVICE_NAME} failed"
}
grep -q "^${PACKAGE_NAME}/srv/AddTwoInts$" "${service_type_log}" || {
  cat "${service_type_log}" >&2 || true
  fail "service ${SERVICE_NAME} did not report type ${PACKAGE_NAME}/srv/AddTwoInts"
}

timeout 30 ros2 service call "${SERVICE_NAME}" "${PACKAGE_NAME}/srv/AddTwoInts" \
  '{a: 2, b: 3}' >"${service_call_log}" 2>&1 || {
  cat "${service_call_log}" >&2 || true
  fail "ros2 service call ${SERVICE_NAME} failed"
}
grep -q 'sum' "${service_call_log}" || {
  cat "${service_call_log}" >&2 || true
  fail "AddTwoInts service call did not return sum"
}
grep -q '5' "${service_call_log}" || {
  cat "${service_call_log}" >&2 || true
  fail "AddTwoInts service call did not return sum=5"
}

stop_pid "${server_pid}"

setsid env SERVICE_NAME="${SERVICE_NAME}" /usr/bin/python3 "${ros2_server_file}" >"${ros2_server_log}" 2>&1 &
ros2_server_pid=$!
PIDS+=("${ros2_server_pid}")
sleep 3
if ! kill -0 "${ros2_server_pid}" 2>/dev/null; then
  cat "${ros2_server_log}" >&2 || true
  fail "ROS2 reference AddTwoInts server exited before reverse live acceptance"
fi

wait_for_service "${SERVICE_NAME}" "${service_list_log}"

timeout 20 setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${WORK_DIR}/runtime_project_build" "${client_app}" "${client_cfg}" >"${client_log}" 2>&1 || true

grep -q 'IBMW_REFERENCE_SRV_CLIENT_ADD_TWO_INTS_OK' "${client_log}" || {
  cat "${client_log}" >&2 || true
  fail "IBMW reference AddTwoInts client did not receive sum=5 from ROS2 server"
}

stop_pid "${ros2_server_pid}"

set_bool_package="std_srvs"
set_bool_type="srv/SetBool"
set_bool_work_dir="${WORK_DIR}/set_bool"
require_file "${ROS2_PREFIX}/share/${set_bool_package}/package.xml"
mkdir -p "${set_bool_work_dir}/logs"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" import \
  --ros2-prefix "${ROS2_PREFIX}" \
  --ros2-package "${set_bool_package}" \
  --ros2-type "${set_bool_type}" \
  --artifact-only \
  --output-dir "${set_bool_work_dir}/reference" \
  --format json >"${set_bool_work_dir}/logs/reference_import_report.json"

set_bool_runtime_yaml="${set_bool_work_dir}/reference/${set_bool_package}_reference_existing_srv_runtime.ibmw.yaml"
require_file "${set_bool_runtime_yaml}"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" \
  "${set_bool_runtime_yaml}" \
  -o "${set_bool_work_dir}/runtime_project" \
  --force

set_bool_handler_file="${set_bool_work_dir}/runtime_project/src/set_bool_rpc_server_module/set_bool_rpc_server_module_typesupport.inc.cc"
set_bool_client_file="${set_bool_work_dir}/runtime_project/src/set_bool_rpc_client_module/set_bool_rpc_client_module_typesupport.inc.cc"
require_file "${set_bool_handler_file}"
require_file "${set_bool_client_file}"
patch_set_bool_handler "${set_bool_handler_file}"
patch_set_bool_client "${set_bool_client_file}" "${set_bool_package}"

cmake \
  -S "${set_bool_work_dir}/runtime_project" \
  -B "${set_bool_work_dir}/runtime_project_build" \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DCMAKE_PREFIX_PATH="${IBMW_INSTALL_PREFIX};${ROS2_PREFIX};${CMAKE_PREFIX_PATH:-}" \
  -Dibmw_DIR="${IBMW_INSTALL_PREFIX}/lib/cmake/ibmw"
cmake --build "${set_bool_work_dir}/runtime_project_build" -j"$(nproc)"

set_bool_app="${set_bool_work_dir}/runtime_project_build/set_bool_imported_srv_server_app"
set_bool_client_app="${set_bool_work_dir}/runtime_project_build/set_bool_imported_srv_client_app"
set_bool_cfg="${set_bool_work_dir}/runtime_project/cfg/set_bool_imported_srv_server_app.yaml"
set_bool_client_cfg="${set_bool_work_dir}/runtime_project/cfg/set_bool_imported_srv_client_app.yaml"
require_executable "${set_bool_app}"
require_executable "${set_bool_client_app}"
require_file "${set_bool_cfg}"
require_file "${set_bool_client_cfg}"

export LD_LIBRARY_PATH="${set_bool_work_dir}/runtime_project_build:${LD_LIBRARY_PATH}"

set_bool_server_log="${set_bool_work_dir}/logs/ibmw_set_bool_server.log"
ros2_set_bool_server_log="${set_bool_work_dir}/logs/ros2_set_bool_server.log"
set_bool_client_log="${set_bool_work_dir}/logs/ibmw_set_bool_client.log"
set_bool_service_list_log="${set_bool_work_dir}/logs/ros2_service_list.log"
set_bool_service_type_log="${set_bool_work_dir}/logs/ros2_service_type.log"
set_bool_service_call_log="${set_bool_work_dir}/logs/ros2_service_call.log"
ros2_set_bool_server_file="${set_bool_work_dir}/ros2_set_bool_server.py"
write_ros2_set_bool_server "${ros2_set_bool_server_file}"

setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${set_bool_work_dir}/runtime_project_build" "${set_bool_app}" "${set_bool_cfg}" >"${set_bool_server_log}" 2>&1 &
set_bool_server_pid=$!
PIDS+=("${set_bool_server_pid}")
sleep 3
if ! kill -0 "${set_bool_server_pid}" 2>/dev/null; then
  cat "${set_bool_server_log}" >&2 || true
  fail "IBMW reference SetBool server exited before live acceptance"
fi

wait_for_service "${SET_BOOL_SERVICE_NAME}" "${set_bool_service_list_log}"

timeout 10 ros2 service type "${SET_BOOL_SERVICE_NAME}" >"${set_bool_service_type_log}" 2>&1 || {
  cat "${set_bool_service_type_log}" >&2 || true
  fail "ros2 service type ${SET_BOOL_SERVICE_NAME} failed"
}
grep -q "^${set_bool_package}/srv/SetBool$" "${set_bool_service_type_log}" || {
  cat "${set_bool_service_type_log}" >&2 || true
  fail "service ${SET_BOOL_SERVICE_NAME} did not report type ${set_bool_package}/srv/SetBool"
}

timeout 30 ros2 service call "${SET_BOOL_SERVICE_NAME}" "${set_bool_package}/srv/SetBool" \
  '{data: true}' >"${set_bool_service_call_log}" 2>&1 || {
  cat "${set_bool_service_call_log}" >&2 || true
  fail "ros2 service call ${SET_BOOL_SERVICE_NAME} failed"
}
grep -Eq 'success[=:] ?(True|true)' "${set_bool_service_call_log}" || {
  cat "${set_bool_service_call_log}" >&2 || true
  fail "SetBool service call did not return success=true"
}
grep -q 'accepted:true' "${set_bool_service_call_log}" || {
  cat "${set_bool_service_call_log}" >&2 || true
  fail "SetBool service call did not return message accepted:true"
}

stop_pid "${set_bool_server_pid}"

setsid env SET_BOOL_SERVICE_NAME="${SET_BOOL_SERVICE_NAME}" /usr/bin/python3 "${ros2_set_bool_server_file}" >"${ros2_set_bool_server_log}" 2>&1 &
ros2_set_bool_server_pid=$!
PIDS+=("${ros2_set_bool_server_pid}")
sleep 3
if ! kill -0 "${ros2_set_bool_server_pid}" 2>/dev/null; then
  cat "${ros2_set_bool_server_log}" >&2 || true
  fail "ROS2 reference SetBool server exited before reverse live acceptance"
fi

wait_for_service "${SET_BOOL_SERVICE_NAME}" "${set_bool_service_list_log}"

timeout 20 setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${set_bool_work_dir}/runtime_project_build" "${set_bool_client_app}" "${set_bool_client_cfg}" >"${set_bool_client_log}" 2>&1 || true

grep -q 'IBMW_REFERENCE_SRV_CLIENT_SET_BOOL_OK' "${set_bool_client_log}" || {
  cat "${set_bool_client_log}" >&2 || true
  fail "IBMW reference SetBool client did not receive accepted:true from ROS2 server"
}

stop_pid "${ros2_set_bool_server_pid}"

trigger_package="std_srvs"
trigger_type="srv/Trigger"
trigger_work_dir="${WORK_DIR}/trigger"
require_file "${ROS2_PREFIX}/share/${trigger_package}/package.xml"
mkdir -p "${trigger_work_dir}/logs"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" import \
  --ros2-prefix "${ROS2_PREFIX}" \
  --ros2-package "${trigger_package}" \
  --ros2-type "${trigger_type}" \
  --artifact-only \
  --output-dir "${trigger_work_dir}/reference" \
  --format json >"${trigger_work_dir}/logs/reference_import_report.json"

trigger_runtime_yaml="${trigger_work_dir}/reference/${trigger_package}_reference_existing_srv_runtime.ibmw.yaml"
require_file "${trigger_runtime_yaml}"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" \
  "${trigger_runtime_yaml}" \
  -o "${trigger_work_dir}/runtime_project" \
  --force

trigger_handler_file="${trigger_work_dir}/runtime_project/src/trigger_rpc_server_module/trigger_rpc_server_module_typesupport.inc.cc"
trigger_client_file="${trigger_work_dir}/runtime_project/src/trigger_rpc_client_module/trigger_rpc_client_module_typesupport.inc.cc"
require_file "${trigger_handler_file}"
require_file "${trigger_client_file}"
patch_trigger_handler "${trigger_handler_file}"
patch_trigger_client "${trigger_client_file}" "${trigger_package}"

cmake \
  -S "${trigger_work_dir}/runtime_project" \
  -B "${trigger_work_dir}/runtime_project_build" \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DCMAKE_PREFIX_PATH="${IBMW_INSTALL_PREFIX};${ROS2_PREFIX};${CMAKE_PREFIX_PATH:-}" \
  -Dibmw_DIR="${IBMW_INSTALL_PREFIX}/lib/cmake/ibmw"
cmake --build "${trigger_work_dir}/runtime_project_build" -j"$(nproc)"

trigger_app="${trigger_work_dir}/runtime_project_build/trigger_imported_srv_server_app"
trigger_client_app="${trigger_work_dir}/runtime_project_build/trigger_imported_srv_client_app"
trigger_cfg="${trigger_work_dir}/runtime_project/cfg/trigger_imported_srv_server_app.yaml"
trigger_client_cfg="${trigger_work_dir}/runtime_project/cfg/trigger_imported_srv_client_app.yaml"
require_executable "${trigger_app}"
require_executable "${trigger_client_app}"
require_file "${trigger_cfg}"
require_file "${trigger_client_cfg}"

export LD_LIBRARY_PATH="${trigger_work_dir}/runtime_project_build:${LD_LIBRARY_PATH}"

trigger_server_log="${trigger_work_dir}/logs/ibmw_trigger_server.log"
ros2_trigger_server_log="${trigger_work_dir}/logs/ros2_trigger_server.log"
trigger_client_log="${trigger_work_dir}/logs/ibmw_trigger_client.log"
trigger_service_list_log="${trigger_work_dir}/logs/ros2_service_list.log"
trigger_service_type_log="${trigger_work_dir}/logs/ros2_service_type.log"
trigger_service_call_log="${trigger_work_dir}/logs/ros2_service_call.log"
ros2_trigger_server_file="${trigger_work_dir}/ros2_trigger_server.py"
write_ros2_trigger_server "${ros2_trigger_server_file}"

setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${trigger_work_dir}/runtime_project_build" "${trigger_app}" "${trigger_cfg}" >"${trigger_server_log}" 2>&1 &
trigger_server_pid=$!
PIDS+=("${trigger_server_pid}")
sleep 3
if ! kill -0 "${trigger_server_pid}" 2>/dev/null; then
  cat "${trigger_server_log}" >&2 || true
  fail "IBMW reference Trigger server exited before live acceptance"
fi

wait_for_service "${TRIGGER_SERVICE_NAME}" "${trigger_service_list_log}"

timeout 10 ros2 service type "${TRIGGER_SERVICE_NAME}" >"${trigger_service_type_log}" 2>&1 || {
  cat "${trigger_service_type_log}" >&2 || true
  fail "ros2 service type ${TRIGGER_SERVICE_NAME} failed"
}
grep -q "^${trigger_package}/srv/Trigger$" "${trigger_service_type_log}" || {
  cat "${trigger_service_type_log}" >&2 || true
  fail "service ${TRIGGER_SERVICE_NAME} did not report type ${trigger_package}/srv/Trigger"
}

timeout 30 ros2 service call "${TRIGGER_SERVICE_NAME}" "${trigger_package}/srv/Trigger" \
  '{}' >"${trigger_service_call_log}" 2>&1 || {
  cat "${trigger_service_call_log}" >&2 || true
  fail "ros2 service call ${TRIGGER_SERVICE_NAME} failed"
}
grep -Eq 'success[=:] ?(True|true)' "${trigger_service_call_log}" || {
  cat "${trigger_service_call_log}" >&2 || true
  fail "Trigger service call did not return success=true"
}
grep -q 'triggered:ok' "${trigger_service_call_log}" || {
  cat "${trigger_service_call_log}" >&2 || true
  fail "Trigger service call did not return message triggered:ok"
}

stop_pid "${trigger_server_pid}"

setsid env TRIGGER_SERVICE_NAME="${TRIGGER_SERVICE_NAME}" /usr/bin/python3 "${ros2_trigger_server_file}" >"${ros2_trigger_server_log}" 2>&1 &
ros2_trigger_server_pid=$!
PIDS+=("${ros2_trigger_server_pid}")
sleep 3
if ! kill -0 "${ros2_trigger_server_pid}" 2>/dev/null; then
  cat "${ros2_trigger_server_log}" >&2 || true
  fail "ROS2 reference Trigger server exited before reverse live acceptance"
fi

wait_for_service "${TRIGGER_SERVICE_NAME}" "${trigger_service_list_log}"

timeout 20 setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${trigger_work_dir}/runtime_project_build" "${trigger_client_app}" "${trigger_client_cfg}" >"${trigger_client_log}" 2>&1 || true

grep -q 'IBMW_REFERENCE_SRV_CLIENT_TRIGGER_OK' "${trigger_client_log}" || {
  cat "${trigger_client_log}" >&2 || true
  fail "IBMW reference Trigger client did not receive triggered:ok from ROS2 server"
}

stop_pid "${ros2_trigger_server_pid}"

empty_package="std_srvs"
empty_type="srv/Empty"
empty_work_dir="${WORK_DIR}/empty"
require_file "${ROS2_PREFIX}/share/${empty_package}/package.xml"
mkdir -p "${empty_work_dir}/logs"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" import \
  --ros2-prefix "${ROS2_PREFIX}" \
  --ros2-package "${empty_package}" \
  --ros2-type "${empty_type}" \
  --artifact-only \
  --output-dir "${empty_work_dir}/reference" \
  --format json >"${empty_work_dir}/logs/reference_import_report.json"

empty_runtime_yaml="${empty_work_dir}/reference/${empty_package}_reference_existing_srv_runtime.ibmw.yaml"
require_file "${empty_runtime_yaml}"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" \
  "${empty_runtime_yaml}" \
  -o "${empty_work_dir}/runtime_project" \
  --force

empty_handler_file="${empty_work_dir}/runtime_project/src/empty_rpc_server_module/empty_rpc_server_module_typesupport.inc.cc"
empty_client_file="${empty_work_dir}/runtime_project/src/empty_rpc_client_module/empty_rpc_client_module_typesupport.inc.cc"
require_file "${empty_handler_file}"
require_file "${empty_client_file}"
patch_empty_handler "${empty_handler_file}"
patch_empty_client "${empty_client_file}" "${empty_package}"

cmake \
  -S "${empty_work_dir}/runtime_project" \
  -B "${empty_work_dir}/runtime_project_build" \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DCMAKE_PREFIX_PATH="${IBMW_INSTALL_PREFIX};${ROS2_PREFIX};${CMAKE_PREFIX_PATH:-}" \
  -Dibmw_DIR="${IBMW_INSTALL_PREFIX}/lib/cmake/ibmw"
cmake --build "${empty_work_dir}/runtime_project_build" -j"$(nproc)"

empty_app="${empty_work_dir}/runtime_project_build/empty_imported_srv_server_app"
empty_client_app="${empty_work_dir}/runtime_project_build/empty_imported_srv_client_app"
empty_cfg="${empty_work_dir}/runtime_project/cfg/empty_imported_srv_server_app.yaml"
empty_client_cfg="${empty_work_dir}/runtime_project/cfg/empty_imported_srv_client_app.yaml"
require_executable "${empty_app}"
require_executable "${empty_client_app}"
require_file "${empty_cfg}"
require_file "${empty_client_cfg}"

empty_host_live_audit_log="${empty_work_dir}/logs/host_live_target_artifact_audit.json"
assert_host_live_audit_fails_on_ros2_prefix \
  "${empty_package}" \
  "${empty_type}" \
  "${empty_work_dir}/reference" \
  "${empty_host_live_audit_log}" \
  "${empty_app}" \
  "${empty_client_app}" \
  "${empty_work_dir}/runtime_project_build/libempty_rpc_server_module_pkg.so" \
  "${empty_work_dir}/runtime_project_build/libempty_rpc_client_module_pkg.so"

export LD_LIBRARY_PATH="${empty_work_dir}/runtime_project_build:${LD_LIBRARY_PATH}"

empty_server_log="${empty_work_dir}/logs/ibmw_empty_server.log"
ros2_empty_server_log="${empty_work_dir}/logs/ros2_empty_server.log"
empty_client_log="${empty_work_dir}/logs/ibmw_empty_client.log"
empty_service_list_log="${empty_work_dir}/logs/ros2_service_list.log"
empty_service_type_log="${empty_work_dir}/logs/ros2_service_type.log"
empty_service_call_log="${empty_work_dir}/logs/ros2_service_call.log"
ros2_empty_server_file="${empty_work_dir}/ros2_empty_server.py"
write_ros2_empty_server "${ros2_empty_server_file}"

setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${empty_work_dir}/runtime_project_build" "${empty_app}" "${empty_cfg}" >"${empty_server_log}" 2>&1 &
empty_server_pid=$!
PIDS+=("${empty_server_pid}")
sleep 3
if ! kill -0 "${empty_server_pid}" 2>/dev/null; then
  cat "${empty_server_log}" >&2 || true
  fail "IBMW reference Empty server exited before live acceptance"
fi

wait_for_service "${EMPTY_SERVICE_NAME}" "${empty_service_list_log}"

timeout 10 ros2 service type "${EMPTY_SERVICE_NAME}" >"${empty_service_type_log}" 2>&1 || {
  cat "${empty_service_type_log}" >&2 || true
  fail "ros2 service type ${EMPTY_SERVICE_NAME} failed"
}
grep -q "^${empty_package}/srv/Empty$" "${empty_service_type_log}" || {
  cat "${empty_service_type_log}" >&2 || true
  fail "service ${EMPTY_SERVICE_NAME} did not report type ${empty_package}/srv/Empty"
}

timeout 30 ros2 service call "${EMPTY_SERVICE_NAME}" "${empty_package}/srv/Empty" \
  '{}' >"${empty_service_call_log}" 2>&1 || {
  cat "${empty_service_call_log}" >&2 || true
  fail "ros2 service call ${EMPTY_SERVICE_NAME} failed"
}
grep -q 'response:' "${empty_service_call_log}" || {
  cat "${empty_service_call_log}" >&2 || true
  fail "Empty service call did not return a response block"
}

stop_pid "${empty_server_pid}"

setsid env EMPTY_SERVICE_NAME="${EMPTY_SERVICE_NAME}" /usr/bin/python3 "${ros2_empty_server_file}" >"${ros2_empty_server_log}" 2>&1 &
ros2_empty_server_pid=$!
PIDS+=("${ros2_empty_server_pid}")
sleep 3
if ! kill -0 "${ros2_empty_server_pid}" 2>/dev/null; then
  cat "${ros2_empty_server_log}" >&2 || true
  fail "ROS2 reference Empty server exited before reverse live acceptance"
fi

wait_for_service "${EMPTY_SERVICE_NAME}" "${empty_service_list_log}"

timeout 20 setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${empty_work_dir}/runtime_project_build" "${empty_client_app}" "${empty_client_cfg}" >"${empty_client_log}" 2>&1 || true

grep -q 'IBMW_REFERENCE_SRV_CLIENT_EMPTY_OK' "${empty_client_log}" || {
  cat "${empty_client_log}" >&2 || true
  fail "IBMW reference Empty client did not receive an empty response from ROS2 server"
}

stop_pid "${ros2_empty_server_pid}"

get_state_package="lifecycle_msgs"
get_state_type="srv/GetState"
get_state_work_dir="${WORK_DIR}/lifecycle_get_state"
require_file "${ROS2_PREFIX}/share/${get_state_package}/package.xml"
mkdir -p "${get_state_work_dir}/logs"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" import \
  --ros2-prefix "${ROS2_PREFIX}" \
  --ros2-package "${get_state_package}" \
  --ros2-type "${get_state_type}" \
  --artifact-only \
  --output-dir "${get_state_work_dir}/reference" \
  --format json >"${get_state_work_dir}/logs/reference_import_report.json"

get_state_runtime_yaml="${get_state_work_dir}/reference/${get_state_package}_reference_existing_srv_runtime.ibmw.yaml"
require_file "${get_state_runtime_yaml}"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" \
  "${get_state_runtime_yaml}" \
  -o "${get_state_work_dir}/runtime_project" \
  --force

get_state_handler_file="${get_state_work_dir}/runtime_project/src/get_state_rpc_server_module/get_state_rpc_server_module_typesupport.inc.cc"
get_state_client_file="${get_state_work_dir}/runtime_project/src/get_state_rpc_client_module/get_state_rpc_client_module_typesupport.inc.cc"
require_file "${get_state_handler_file}"
require_file "${get_state_client_file}"
patch_get_state_handler "${get_state_handler_file}"
patch_get_state_client "${get_state_client_file}" "${get_state_package}"

cmake \
  -S "${get_state_work_dir}/runtime_project" \
  -B "${get_state_work_dir}/runtime_project_build" \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DCMAKE_PREFIX_PATH="${IBMW_INSTALL_PREFIX};${ROS2_PREFIX};${CMAKE_PREFIX_PATH:-}" \
  -Dibmw_DIR="${IBMW_INSTALL_PREFIX}/lib/cmake/ibmw"
cmake --build "${get_state_work_dir}/runtime_project_build" -j"$(nproc)"

get_state_app="${get_state_work_dir}/runtime_project_build/get_state_imported_srv_server_app"
get_state_client_app="${get_state_work_dir}/runtime_project_build/get_state_imported_srv_client_app"
get_state_cfg="${get_state_work_dir}/runtime_project/cfg/get_state_imported_srv_server_app.yaml"
get_state_client_cfg="${get_state_work_dir}/runtime_project/cfg/get_state_imported_srv_client_app.yaml"
require_executable "${get_state_app}"
require_executable "${get_state_client_app}"
require_file "${get_state_cfg}"
require_file "${get_state_client_cfg}"

export LD_LIBRARY_PATH="${get_state_work_dir}/runtime_project_build:${LD_LIBRARY_PATH}"

get_state_server_log="${get_state_work_dir}/logs/ibmw_get_state_server.log"
ros2_get_state_server_log="${get_state_work_dir}/logs/ros2_get_state_server.log"
get_state_client_log="${get_state_work_dir}/logs/ibmw_get_state_client.log"
get_state_service_list_log="${get_state_work_dir}/logs/ros2_service_list.log"
get_state_service_type_log="${get_state_work_dir}/logs/ros2_service_type.log"
get_state_service_call_log="${get_state_work_dir}/logs/ros2_service_call.log"
ros2_get_state_server_file="${get_state_work_dir}/ros2_get_state_server.py"
write_ros2_get_state_server "${ros2_get_state_server_file}"

setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${get_state_work_dir}/runtime_project_build" "${get_state_app}" "${get_state_cfg}" >"${get_state_server_log}" 2>&1 &
get_state_server_pid=$!
PIDS+=("${get_state_server_pid}")
sleep 3
if ! kill -0 "${get_state_server_pid}" 2>/dev/null; then
  cat "${get_state_server_log}" >&2 || true
  fail "IBMW reference GetState server exited before live acceptance"
fi

wait_for_service "${LIFECYCLE_GET_STATE_SERVICE_NAME}" "${get_state_service_list_log}"

timeout 10 ros2 service type "${LIFECYCLE_GET_STATE_SERVICE_NAME}" >"${get_state_service_type_log}" 2>&1 || {
  cat "${get_state_service_type_log}" >&2 || true
  fail "ros2 service type ${LIFECYCLE_GET_STATE_SERVICE_NAME} failed"
}
grep -q "^${get_state_package}/srv/GetState$" "${get_state_service_type_log}" || {
  cat "${get_state_service_type_log}" >&2 || true
  fail "service ${LIFECYCLE_GET_STATE_SERVICE_NAME} did not report type ${get_state_package}/srv/GetState"
}

timeout 30 ros2 service call "${LIFECYCLE_GET_STATE_SERVICE_NAME}" "${get_state_package}/srv/GetState" \
  '{}' >"${get_state_service_call_log}" 2>&1 || {
  cat "${get_state_service_call_log}" >&2 || true
  fail "ros2 service call ${LIFECYCLE_GET_STATE_SERVICE_NAME} failed"
}
grep -Eq 'id[=:] ?3' "${get_state_service_call_log}" || {
  cat "${get_state_service_call_log}" >&2 || true
  fail "GetState service call did not return id=3"
}
grep -q 'active' "${get_state_service_call_log}" || {
  cat "${get_state_service_call_log}" >&2 || true
  fail "GetState service call did not return label active"
}

stop_pid "${get_state_server_pid}"

setsid env LIFECYCLE_GET_STATE_SERVICE_NAME="${LIFECYCLE_GET_STATE_SERVICE_NAME}" /usr/bin/python3 "${ros2_get_state_server_file}" >"${ros2_get_state_server_log}" 2>&1 &
ros2_get_state_server_pid=$!
PIDS+=("${ros2_get_state_server_pid}")
sleep 3
if ! kill -0 "${ros2_get_state_server_pid}" 2>/dev/null; then
  cat "${ros2_get_state_server_log}" >&2 || true
  fail "ROS2 reference GetState server exited before reverse live acceptance"
fi

wait_for_service "${LIFECYCLE_GET_STATE_SERVICE_NAME}" "${get_state_service_list_log}"

timeout 20 setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${get_state_work_dir}/runtime_project_build" "${get_state_client_app}" "${get_state_client_cfg}" >"${get_state_client_log}" 2>&1 || true

grep -q 'IBMW_REFERENCE_SRV_CLIENT_GET_STATE_OK' "${get_state_client_log}" || {
  cat "${get_state_client_log}" >&2 || true
  fail "IBMW reference GetState client did not receive active state from ROS2 server"
}

stop_pid "${ros2_get_state_server_pid}"

change_state_package="lifecycle_msgs"
change_state_type="srv/ChangeState"
change_state_work_dir="${WORK_DIR}/lifecycle_change_state"
require_file "${ROS2_PREFIX}/share/${change_state_package}/package.xml"
mkdir -p "${change_state_work_dir}/logs"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" import \
  --ros2-prefix "${ROS2_PREFIX}" \
  --ros2-package "${change_state_package}" \
  --ros2-type "${change_state_type}" \
  --artifact-only \
  --output-dir "${change_state_work_dir}/reference" \
  --format json >"${change_state_work_dir}/logs/reference_import_report.json"

change_state_runtime_yaml="${change_state_work_dir}/reference/${change_state_package}_reference_existing_srv_runtime.ibmw.yaml"
require_file "${change_state_runtime_yaml}"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" \
  "${change_state_runtime_yaml}" \
  -o "${change_state_work_dir}/runtime_project" \
  --force

change_state_handler_file="${change_state_work_dir}/runtime_project/src/change_state_rpc_server_module/change_state_rpc_server_module_typesupport.inc.cc"
change_state_client_file="${change_state_work_dir}/runtime_project/src/change_state_rpc_client_module/change_state_rpc_client_module_typesupport.inc.cc"
require_file "${change_state_handler_file}"
require_file "${change_state_client_file}"
patch_change_state_handler "${change_state_handler_file}"
patch_change_state_client "${change_state_client_file}" "${change_state_package}"

cmake \
  -S "${change_state_work_dir}/runtime_project" \
  -B "${change_state_work_dir}/runtime_project_build" \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DCMAKE_PREFIX_PATH="${IBMW_INSTALL_PREFIX};${ROS2_PREFIX};${CMAKE_PREFIX_PATH:-}" \
  -Dibmw_DIR="${IBMW_INSTALL_PREFIX}/lib/cmake/ibmw"
cmake --build "${change_state_work_dir}/runtime_project_build" -j"$(nproc)"

change_state_app="${change_state_work_dir}/runtime_project_build/change_state_imported_srv_server_app"
change_state_client_app="${change_state_work_dir}/runtime_project_build/change_state_imported_srv_client_app"
change_state_cfg="${change_state_work_dir}/runtime_project/cfg/change_state_imported_srv_server_app.yaml"
change_state_client_cfg="${change_state_work_dir}/runtime_project/cfg/change_state_imported_srv_client_app.yaml"
require_executable "${change_state_app}"
require_executable "${change_state_client_app}"
require_file "${change_state_cfg}"
require_file "${change_state_client_cfg}"

export LD_LIBRARY_PATH="${change_state_work_dir}/runtime_project_build:${LD_LIBRARY_PATH}"

change_state_server_log="${change_state_work_dir}/logs/ibmw_change_state_server.log"
ros2_change_state_server_log="${change_state_work_dir}/logs/ros2_change_state_server.log"
change_state_client_log="${change_state_work_dir}/logs/ibmw_change_state_client.log"
change_state_service_list_log="${change_state_work_dir}/logs/ros2_service_list.log"
change_state_service_type_log="${change_state_work_dir}/logs/ros2_service_type.log"
change_state_service_call_log="${change_state_work_dir}/logs/ros2_service_call.log"
ros2_change_state_server_file="${change_state_work_dir}/ros2_change_state_server.py"
write_ros2_change_state_server "${ros2_change_state_server_file}"

setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${change_state_work_dir}/runtime_project_build" "${change_state_app}" "${change_state_cfg}" >"${change_state_server_log}" 2>&1 &
change_state_server_pid=$!
PIDS+=("${change_state_server_pid}")
sleep 3
if ! kill -0 "${change_state_server_pid}" 2>/dev/null; then
  cat "${change_state_server_log}" >&2 || true
  fail "IBMW reference ChangeState server exited before live acceptance"
fi

wait_for_service "${LIFECYCLE_CHANGE_STATE_SERVICE_NAME}" "${change_state_service_list_log}"

timeout 10 ros2 service type "${LIFECYCLE_CHANGE_STATE_SERVICE_NAME}" >"${change_state_service_type_log}" 2>&1 || {
  cat "${change_state_service_type_log}" >&2 || true
  fail "ros2 service type ${LIFECYCLE_CHANGE_STATE_SERVICE_NAME} failed"
}
grep -q "^${change_state_package}/srv/ChangeState$" "${change_state_service_type_log}" || {
  cat "${change_state_service_type_log}" >&2 || true
  fail "service ${LIFECYCLE_CHANGE_STATE_SERVICE_NAME} did not report type ${change_state_package}/srv/ChangeState"
}

timeout 30 ros2 service call "${LIFECYCLE_CHANGE_STATE_SERVICE_NAME}" "${change_state_package}/srv/ChangeState" \
  "{transition: {id: 1, label: 'configure'}}" >"${change_state_service_call_log}" 2>&1 || {
  cat "${change_state_service_call_log}" >&2 || true
  fail "ros2 service call ${LIFECYCLE_CHANGE_STATE_SERVICE_NAME} failed"
}
grep -Eq 'success[=:] ?(True|true)' "${change_state_service_call_log}" || {
  cat "${change_state_service_call_log}" >&2 || true
  fail "ChangeState service call did not return success=true"
}

stop_pid "${change_state_server_pid}"

setsid env LIFECYCLE_CHANGE_STATE_SERVICE_NAME="${LIFECYCLE_CHANGE_STATE_SERVICE_NAME}" /usr/bin/python3 "${ros2_change_state_server_file}" >"${ros2_change_state_server_log}" 2>&1 &
ros2_change_state_server_pid=$!
PIDS+=("${ros2_change_state_server_pid}")
sleep 3
if ! kill -0 "${ros2_change_state_server_pid}" 2>/dev/null; then
  cat "${ros2_change_state_server_log}" >&2 || true
  fail "ROS2 reference ChangeState server exited before reverse live acceptance"
fi

wait_for_service "${LIFECYCLE_CHANGE_STATE_SERVICE_NAME}" "${change_state_service_list_log}"

timeout 20 setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${change_state_work_dir}/runtime_project_build" "${change_state_client_app}" "${change_state_client_cfg}" >"${change_state_client_log}" 2>&1 || true

grep -q 'IBMW_REFERENCE_SRV_CLIENT_CHANGE_STATE_OK' "${change_state_client_log}" || {
  cat "${change_state_client_log}" >&2 || true
  fail "IBMW reference ChangeState client did not receive success=true from ROS2 server"
}

stop_pid "${ros2_change_state_server_pid}"

get_available_states_package="lifecycle_msgs"
get_available_states_type="srv/GetAvailableStates"
get_available_states_work_dir="${WORK_DIR}/lifecycle_get_available_states"
require_file "${ROS2_PREFIX}/share/${get_available_states_package}/package.xml"
mkdir -p "${get_available_states_work_dir}/logs"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" import \
  --ros2-prefix "${ROS2_PREFIX}" \
  --ros2-package "${get_available_states_package}" \
  --ros2-type "${get_available_states_type}" \
  --artifact-only \
  --output-dir "${get_available_states_work_dir}/reference" \
  --format json >"${get_available_states_work_dir}/logs/reference_import_report.json"

get_available_states_runtime_yaml="${get_available_states_work_dir}/reference/${get_available_states_package}_reference_existing_srv_runtime.ibmw.yaml"
require_file "${get_available_states_runtime_yaml}"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" \
  "${get_available_states_runtime_yaml}" \
  -o "${get_available_states_work_dir}/runtime_project" \
  --force

get_available_states_handler_file="${get_available_states_work_dir}/runtime_project/src/get_available_states_rpc_server_module/get_available_states_rpc_server_module_typesupport.inc.cc"
get_available_states_client_file="${get_available_states_work_dir}/runtime_project/src/get_available_states_rpc_client_module/get_available_states_rpc_client_module_typesupport.inc.cc"
require_file "${get_available_states_handler_file}"
require_file "${get_available_states_client_file}"
patch_get_available_states_handler "${get_available_states_handler_file}"
patch_get_available_states_client "${get_available_states_client_file}" "${get_available_states_package}"

cmake \
  -S "${get_available_states_work_dir}/runtime_project" \
  -B "${get_available_states_work_dir}/runtime_project_build" \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DCMAKE_PREFIX_PATH="${IBMW_INSTALL_PREFIX};${ROS2_PREFIX};${CMAKE_PREFIX_PATH:-}" \
  -Dibmw_DIR="${IBMW_INSTALL_PREFIX}/lib/cmake/ibmw"
cmake --build "${get_available_states_work_dir}/runtime_project_build" -j"$(nproc)"

get_available_states_app="${get_available_states_work_dir}/runtime_project_build/get_available_states_imported_srv_server_app"
get_available_states_client_app="${get_available_states_work_dir}/runtime_project_build/get_available_states_imported_srv_client_app"
get_available_states_cfg="${get_available_states_work_dir}/runtime_project/cfg/get_available_states_imported_srv_server_app.yaml"
get_available_states_client_cfg="${get_available_states_work_dir}/runtime_project/cfg/get_available_states_imported_srv_client_app.yaml"
require_executable "${get_available_states_app}"
require_executable "${get_available_states_client_app}"
require_file "${get_available_states_cfg}"
require_file "${get_available_states_client_cfg}"

export LD_LIBRARY_PATH="${get_available_states_work_dir}/runtime_project_build:${LD_LIBRARY_PATH}"

get_available_states_server_log="${get_available_states_work_dir}/logs/ibmw_get_available_states_server.log"
ros2_get_available_states_server_log="${get_available_states_work_dir}/logs/ros2_get_available_states_server.log"
get_available_states_client_log="${get_available_states_work_dir}/logs/ibmw_get_available_states_client.log"
get_available_states_service_list_log="${get_available_states_work_dir}/logs/ros2_service_list.log"
get_available_states_service_type_log="${get_available_states_work_dir}/logs/ros2_service_type.log"
get_available_states_service_call_log="${get_available_states_work_dir}/logs/ros2_service_call.log"
ros2_get_available_states_server_file="${get_available_states_work_dir}/ros2_get_available_states_server.py"
write_ros2_get_available_states_server "${ros2_get_available_states_server_file}"

setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${get_available_states_work_dir}/runtime_project_build" "${get_available_states_app}" "${get_available_states_cfg}" >"${get_available_states_server_log}" 2>&1 &
get_available_states_server_pid=$!
PIDS+=("${get_available_states_server_pid}")
sleep 3
if ! kill -0 "${get_available_states_server_pid}" 2>/dev/null; then
  cat "${get_available_states_server_log}" >&2 || true
  fail "IBMW reference GetAvailableStates server exited before live acceptance"
fi

wait_for_service "${LIFECYCLE_GET_AVAILABLE_STATES_SERVICE_NAME}" "${get_available_states_service_list_log}"

timeout 10 ros2 service type "${LIFECYCLE_GET_AVAILABLE_STATES_SERVICE_NAME}" >"${get_available_states_service_type_log}" 2>&1 || {
  cat "${get_available_states_service_type_log}" >&2 || true
  fail "ros2 service type ${LIFECYCLE_GET_AVAILABLE_STATES_SERVICE_NAME} failed"
}
grep -q "^${get_available_states_package}/srv/GetAvailableStates$" "${get_available_states_service_type_log}" || {
  cat "${get_available_states_service_type_log}" >&2 || true
  fail "service ${LIFECYCLE_GET_AVAILABLE_STATES_SERVICE_NAME} did not report type ${get_available_states_package}/srv/GetAvailableStates"
}

timeout 30 ros2 service call "${LIFECYCLE_GET_AVAILABLE_STATES_SERVICE_NAME}" "${get_available_states_package}/srv/GetAvailableStates" \
  '{}' >"${get_available_states_service_call_log}" 2>&1 || {
  cat "${get_available_states_service_call_log}" >&2 || true
  fail "ros2 service call ${LIFECYCLE_GET_AVAILABLE_STATES_SERVICE_NAME} failed"
}
grep -q 'unconfigured' "${get_available_states_service_call_log}" || {
  cat "${get_available_states_service_call_log}" >&2 || true
  fail "GetAvailableStates service call did not return unconfigured state"
}
grep -q 'active' "${get_available_states_service_call_log}" || {
  cat "${get_available_states_service_call_log}" >&2 || true
  fail "GetAvailableStates service call did not return active state"
}

stop_pid "${get_available_states_server_pid}"

setsid env LIFECYCLE_GET_AVAILABLE_STATES_SERVICE_NAME="${LIFECYCLE_GET_AVAILABLE_STATES_SERVICE_NAME}" /usr/bin/python3 "${ros2_get_available_states_server_file}" >"${ros2_get_available_states_server_log}" 2>&1 &
ros2_get_available_states_server_pid=$!
PIDS+=("${ros2_get_available_states_server_pid}")
sleep 3
if ! kill -0 "${ros2_get_available_states_server_pid}" 2>/dev/null; then
  cat "${ros2_get_available_states_server_log}" >&2 || true
  fail "ROS2 reference GetAvailableStates server exited before reverse live acceptance"
fi

wait_for_service "${LIFECYCLE_GET_AVAILABLE_STATES_SERVICE_NAME}" "${get_available_states_service_list_log}"

timeout 20 setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${get_available_states_work_dir}/runtime_project_build" "${get_available_states_client_app}" "${get_available_states_client_cfg}" >"${get_available_states_client_log}" 2>&1 || true

grep -q 'IBMW_REFERENCE_SRV_CLIENT_GET_AVAILABLE_STATES_OK' "${get_available_states_client_log}" || {
  cat "${get_available_states_client_log}" >&2 || true
  fail "IBMW reference GetAvailableStates client did not receive expected state array from ROS2 server"
}

stop_pid "${ros2_get_available_states_server_pid}"

frame_graph_package="tf2_msgs"
frame_graph_type="srv/FrameGraph"
frame_graph_work_dir="${WORK_DIR}/tf2_frame_graph"
require_file "${ROS2_PREFIX}/share/${frame_graph_package}/package.xml"
mkdir -p "${frame_graph_work_dir}/logs"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" import \
  --ros2-prefix "${ROS2_PREFIX}" \
  --ros2-package "${frame_graph_package}" \
  --ros2-type "${frame_graph_type}" \
  --artifact-only \
  --output-dir "${frame_graph_work_dir}/reference" \
  --format json >"${frame_graph_work_dir}/logs/reference_import_report.json"

frame_graph_runtime_yaml="${frame_graph_work_dir}/reference/${frame_graph_package}_reference_existing_srv_runtime.ibmw.yaml"
require_file "${frame_graph_runtime_yaml}"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" \
  "${frame_graph_runtime_yaml}" \
  -o "${frame_graph_work_dir}/runtime_project" \
  --force

frame_graph_handler_file="${frame_graph_work_dir}/runtime_project/src/frame_graph_rpc_server_module/frame_graph_rpc_server_module_typesupport.inc.cc"
frame_graph_client_file="${frame_graph_work_dir}/runtime_project/src/frame_graph_rpc_client_module/frame_graph_rpc_client_module_typesupport.inc.cc"
require_file "${frame_graph_handler_file}"
require_file "${frame_graph_client_file}"
patch_frame_graph_handler "${frame_graph_handler_file}"
patch_frame_graph_client "${frame_graph_client_file}" "${frame_graph_package}"

cmake \
  -S "${frame_graph_work_dir}/runtime_project" \
  -B "${frame_graph_work_dir}/runtime_project_build" \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DCMAKE_PREFIX_PATH="${IBMW_INSTALL_PREFIX};${ROS2_PREFIX};${CMAKE_PREFIX_PATH:-}" \
  -Dibmw_DIR="${IBMW_INSTALL_PREFIX}/lib/cmake/ibmw"
cmake --build "${frame_graph_work_dir}/runtime_project_build" -j"$(nproc)"

frame_graph_app="${frame_graph_work_dir}/runtime_project_build/frame_graph_imported_srv_server_app"
frame_graph_client_app="${frame_graph_work_dir}/runtime_project_build/frame_graph_imported_srv_client_app"
frame_graph_cfg="${frame_graph_work_dir}/runtime_project/cfg/frame_graph_imported_srv_server_app.yaml"
frame_graph_client_cfg="${frame_graph_work_dir}/runtime_project/cfg/frame_graph_imported_srv_client_app.yaml"
require_executable "${frame_graph_app}"
require_executable "${frame_graph_client_app}"
require_file "${frame_graph_cfg}"
require_file "${frame_graph_client_cfg}"

export LD_LIBRARY_PATH="${frame_graph_work_dir}/runtime_project_build:${LD_LIBRARY_PATH}"

frame_graph_server_log="${frame_graph_work_dir}/logs/ibmw_frame_graph_server.log"
ros2_frame_graph_server_log="${frame_graph_work_dir}/logs/ros2_frame_graph_server.log"
frame_graph_client_log="${frame_graph_work_dir}/logs/ibmw_frame_graph_client.log"
frame_graph_service_list_log="${frame_graph_work_dir}/logs/ros2_service_list.log"
frame_graph_service_type_log="${frame_graph_work_dir}/logs/ros2_service_type.log"
frame_graph_service_call_log="${frame_graph_work_dir}/logs/ros2_service_call.log"
ros2_frame_graph_server_file="${frame_graph_work_dir}/ros2_frame_graph_server.py"
write_ros2_frame_graph_server "${ros2_frame_graph_server_file}"

setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${frame_graph_work_dir}/runtime_project_build" "${frame_graph_app}" "${frame_graph_cfg}" >"${frame_graph_server_log}" 2>&1 &
frame_graph_server_pid=$!
PIDS+=("${frame_graph_server_pid}")
sleep 3
if ! kill -0 "${frame_graph_server_pid}" 2>/dev/null; then
  cat "${frame_graph_server_log}" >&2 || true
  fail "IBMW reference FrameGraph server exited before live acceptance"
fi

wait_for_service "${TF2_FRAME_GRAPH_SERVICE_NAME}" "${frame_graph_service_list_log}"

timeout 10 ros2 service type "${TF2_FRAME_GRAPH_SERVICE_NAME}" >"${frame_graph_service_type_log}" 2>&1 || {
  cat "${frame_graph_service_type_log}" >&2 || true
  fail "ros2 service type ${TF2_FRAME_GRAPH_SERVICE_NAME} failed"
}
grep -q "^${frame_graph_package}/srv/FrameGraph$" "${frame_graph_service_type_log}" || {
  cat "${frame_graph_service_type_log}" >&2 || true
  fail "service ${TF2_FRAME_GRAPH_SERVICE_NAME} did not report type ${frame_graph_package}/srv/FrameGraph"
}

timeout 30 ros2 service call "${TF2_FRAME_GRAPH_SERVICE_NAME}" "${frame_graph_package}/srv/FrameGraph" \
  '{}' >"${frame_graph_service_call_log}" 2>&1 || {
  cat "${frame_graph_service_call_log}" >&2 || true
  fail "ros2 service call ${TF2_FRAME_GRAPH_SERVICE_NAME} failed"
}
grep -q 'base_link' "${frame_graph_service_call_log}" || {
  cat "${frame_graph_service_call_log}" >&2 || true
  fail "FrameGraph service call did not return base_link"
}
grep -q 'parent: map' "${frame_graph_service_call_log}" || {
  cat "${frame_graph_service_call_log}" >&2 || true
  fail "FrameGraph service call did not return parent map"
}

stop_pid "${frame_graph_server_pid}"

setsid env TF2_FRAME_GRAPH_SERVICE_NAME="${TF2_FRAME_GRAPH_SERVICE_NAME}" /usr/bin/python3 "${ros2_frame_graph_server_file}" >"${ros2_frame_graph_server_log}" 2>&1 &
ros2_frame_graph_server_pid=$!
PIDS+=("${ros2_frame_graph_server_pid}")
sleep 3
if ! kill -0 "${ros2_frame_graph_server_pid}" 2>/dev/null; then
  cat "${ros2_frame_graph_server_log}" >&2 || true
  fail "ROS2 reference FrameGraph server exited before reverse live acceptance"
fi

wait_for_service "${TF2_FRAME_GRAPH_SERVICE_NAME}" "${frame_graph_service_list_log}"

timeout 20 setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${frame_graph_work_dir}/runtime_project_build" "${frame_graph_client_app}" "${frame_graph_client_cfg}" >"${frame_graph_client_log}" 2>&1 || true

grep -q 'IBMW_REFERENCE_SRV_CLIENT_FRAME_GRAPH_OK' "${frame_graph_client_log}" || {
  cat "${frame_graph_client_log}" >&2 || true
  fail "IBMW reference FrameGraph client did not receive expected frame YAML from ROS2 server"
}

stop_pid "${ros2_frame_graph_server_pid}"

self_test_package="diagnostic_msgs"
self_test_type="srv/SelfTest"
self_test_work_dir="${WORK_DIR}/diagnostic_self_test"
require_file "${ROS2_PREFIX}/share/${self_test_package}/package.xml"
mkdir -p "${self_test_work_dir}/logs"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" import \
  --ros2-prefix "${ROS2_PREFIX}" \
  --ros2-package "${self_test_package}" \
  --ros2-type "${self_test_type}" \
  --artifact-only \
  --output-dir "${self_test_work_dir}/reference" \
  --format json >"${self_test_work_dir}/logs/reference_import_report.json"

self_test_runtime_yaml="${self_test_work_dir}/reference/${self_test_package}_reference_existing_srv_runtime.ibmw.yaml"
require_file "${self_test_runtime_yaml}"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" \
  "${self_test_runtime_yaml}" \
  -o "${self_test_work_dir}/runtime_project" \
  --force

self_test_handler_file="${self_test_work_dir}/runtime_project/src/self_test_rpc_server_module/self_test_rpc_server_module_typesupport.inc.cc"
self_test_client_file="${self_test_work_dir}/runtime_project/src/self_test_rpc_client_module/self_test_rpc_client_module_typesupport.inc.cc"
require_file "${self_test_handler_file}"
require_file "${self_test_client_file}"
patch_self_test_handler "${self_test_handler_file}"
patch_self_test_client "${self_test_client_file}" "${self_test_package}"

cmake \
  -S "${self_test_work_dir}/runtime_project" \
  -B "${self_test_work_dir}/runtime_project_build" \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DCMAKE_PREFIX_PATH="${IBMW_INSTALL_PREFIX};${ROS2_PREFIX};${CMAKE_PREFIX_PATH:-}" \
  -Dibmw_DIR="${IBMW_INSTALL_PREFIX}/lib/cmake/ibmw"
cmake --build "${self_test_work_dir}/runtime_project_build" -j"$(nproc)"

self_test_app="${self_test_work_dir}/runtime_project_build/self_test_imported_srv_server_app"
self_test_client_app="${self_test_work_dir}/runtime_project_build/self_test_imported_srv_client_app"
self_test_cfg="${self_test_work_dir}/runtime_project/cfg/self_test_imported_srv_server_app.yaml"
self_test_client_cfg="${self_test_work_dir}/runtime_project/cfg/self_test_imported_srv_client_app.yaml"
require_executable "${self_test_app}"
require_executable "${self_test_client_app}"
require_file "${self_test_cfg}"
require_file "${self_test_client_cfg}"

export LD_LIBRARY_PATH="${self_test_work_dir}/runtime_project_build:${LD_LIBRARY_PATH}"

self_test_server_log="${self_test_work_dir}/logs/ibmw_self_test_server.log"
ros2_self_test_server_log="${self_test_work_dir}/logs/ros2_self_test_server.log"
self_test_client_log="${self_test_work_dir}/logs/ibmw_self_test_client.log"
self_test_service_list_log="${self_test_work_dir}/logs/ros2_service_list.log"
self_test_service_type_log="${self_test_work_dir}/logs/ros2_service_type.log"
self_test_service_call_log="${self_test_work_dir}/logs/ros2_service_call.log"
ros2_self_test_server_file="${self_test_work_dir}/ros2_self_test_server.py"
write_ros2_self_test_server "${ros2_self_test_server_file}"

setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${self_test_work_dir}/runtime_project_build" "${self_test_app}" "${self_test_cfg}" >"${self_test_server_log}" 2>&1 &
self_test_server_pid=$!
PIDS+=("${self_test_server_pid}")
sleep 3
if ! kill -0 "${self_test_server_pid}" 2>/dev/null; then
  cat "${self_test_server_log}" >&2 || true
  fail "IBMW reference SelfTest server exited before live acceptance"
fi

wait_for_service "${DIAGNOSTIC_SELF_TEST_SERVICE_NAME}" "${self_test_service_list_log}"

timeout 10 ros2 service type "${DIAGNOSTIC_SELF_TEST_SERVICE_NAME}" >"${self_test_service_type_log}" 2>&1 || {
  cat "${self_test_service_type_log}" >&2 || true
  fail "ros2 service type ${DIAGNOSTIC_SELF_TEST_SERVICE_NAME} failed"
}
grep -q "^${self_test_package}/srv/SelfTest$" "${self_test_service_type_log}" || {
  cat "${self_test_service_type_log}" >&2 || true
  fail "service ${DIAGNOSTIC_SELF_TEST_SERVICE_NAME} did not report type ${self_test_package}/srv/SelfTest"
}

timeout 30 ros2 service call "${DIAGNOSTIC_SELF_TEST_SERVICE_NAME}" "${self_test_package}/srv/SelfTest" \
  '{}' >"${self_test_service_call_log}" 2>&1 || {
  cat "${self_test_service_call_log}" >&2 || true
  fail "ros2 service call ${DIAGNOSTIC_SELF_TEST_SERVICE_NAME} failed"
}
grep -q 'ibmw-diagnostic-self-test' "${self_test_service_call_log}" || {
  cat "${self_test_service_call_log}" >&2 || true
  fail "SelfTest service call did not return expected id"
}
grep -q 'ros2_reference_existing' "${self_test_service_call_log}" || {
  cat "${self_test_service_call_log}" >&2 || true
  fail "SelfTest service call did not return nested KeyValue"
}

stop_pid "${self_test_server_pid}"

setsid env DIAGNOSTIC_SELF_TEST_SERVICE_NAME="${DIAGNOSTIC_SELF_TEST_SERVICE_NAME}" /usr/bin/python3 "${ros2_self_test_server_file}" >"${ros2_self_test_server_log}" 2>&1 &
ros2_self_test_server_pid=$!
PIDS+=("${ros2_self_test_server_pid}")
sleep 3
if ! kill -0 "${ros2_self_test_server_pid}" 2>/dev/null; then
  cat "${ros2_self_test_server_log}" >&2 || true
  fail "ROS2 reference SelfTest server exited before reverse live acceptance"
fi

wait_for_service "${DIAGNOSTIC_SELF_TEST_SERVICE_NAME}" "${self_test_service_list_log}"

timeout 20 setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${self_test_work_dir}/runtime_project_build" "${self_test_client_app}" "${self_test_client_cfg}" >"${self_test_client_log}" 2>&1 || true

grep -q 'IBMW_REFERENCE_SRV_CLIENT_SELF_TEST_OK' "${self_test_client_log}" || {
  cat "${self_test_client_log}" >&2 || true
  fail "IBMW reference SelfTest client did not receive expected diagnostic array from ROS2 server"
}

stop_pid "${ros2_self_test_server_pid}"

set_camera_info_package="sensor_msgs"
set_camera_info_type="srv/SetCameraInfo"
set_camera_info_work_dir="${WORK_DIR}/sensor_set_camera_info"
require_file "${ROS2_PREFIX}/share/${set_camera_info_package}/package.xml"
mkdir -p "${set_camera_info_work_dir}/logs"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" import \
  --ros2-prefix "${ROS2_PREFIX}" \
  --ros2-package "${set_camera_info_package}" \
  --ros2-type "${set_camera_info_type}" \
  --artifact-only \
  --output-dir "${set_camera_info_work_dir}/reference" \
  --format json >"${set_camera_info_work_dir}/logs/reference_import_report.json"

set_camera_info_runtime_yaml="${set_camera_info_work_dir}/reference/${set_camera_info_package}_reference_existing_srv_runtime.ibmw.yaml"
require_file "${set_camera_info_runtime_yaml}"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" \
  "${set_camera_info_runtime_yaml}" \
  -o "${set_camera_info_work_dir}/runtime_project" \
  --force

set_camera_info_handler_file="${set_camera_info_work_dir}/runtime_project/src/set_camera_info_rpc_server_module/set_camera_info_rpc_server_module_typesupport.inc.cc"
set_camera_info_client_file="${set_camera_info_work_dir}/runtime_project/src/set_camera_info_rpc_client_module/set_camera_info_rpc_client_module_typesupport.inc.cc"
require_file "${set_camera_info_handler_file}"
require_file "${set_camera_info_client_file}"
patch_set_camera_info_handler "${set_camera_info_handler_file}"
patch_set_camera_info_client "${set_camera_info_client_file}" "${set_camera_info_package}"

cmake \
  -S "${set_camera_info_work_dir}/runtime_project" \
  -B "${set_camera_info_work_dir}/runtime_project_build" \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DCMAKE_PREFIX_PATH="${IBMW_INSTALL_PREFIX};${ROS2_PREFIX};${CMAKE_PREFIX_PATH:-}" \
  -Dibmw_DIR="${IBMW_INSTALL_PREFIX}/lib/cmake/ibmw"
cmake --build "${set_camera_info_work_dir}/runtime_project_build" -j"$(nproc)"

set_camera_info_app="${set_camera_info_work_dir}/runtime_project_build/set_camera_info_imported_srv_server_app"
set_camera_info_client_app="${set_camera_info_work_dir}/runtime_project_build/set_camera_info_imported_srv_client_app"
set_camera_info_cfg="${set_camera_info_work_dir}/runtime_project/cfg/set_camera_info_imported_srv_server_app.yaml"
set_camera_info_client_cfg="${set_camera_info_work_dir}/runtime_project/cfg/set_camera_info_imported_srv_client_app.yaml"
require_executable "${set_camera_info_app}"
require_executable "${set_camera_info_client_app}"
require_file "${set_camera_info_cfg}"
require_file "${set_camera_info_client_cfg}"

export LD_LIBRARY_PATH="${set_camera_info_work_dir}/runtime_project_build:${LD_LIBRARY_PATH}"

set_camera_info_server_log="${set_camera_info_work_dir}/logs/ibmw_set_camera_info_server.log"
ros2_set_camera_info_server_log="${set_camera_info_work_dir}/logs/ros2_set_camera_info_server.log"
set_camera_info_client_log="${set_camera_info_work_dir}/logs/ibmw_set_camera_info_client.log"
set_camera_info_service_list_log="${set_camera_info_work_dir}/logs/ros2_service_list.log"
set_camera_info_service_type_log="${set_camera_info_work_dir}/logs/ros2_service_type.log"
set_camera_info_service_call_log="${set_camera_info_work_dir}/logs/ros2_service_call.log"
ros2_set_camera_info_server_file="${set_camera_info_work_dir}/ros2_set_camera_info_server.py"
write_ros2_set_camera_info_server "${ros2_set_camera_info_server_file}"

setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${set_camera_info_work_dir}/runtime_project_build" "${set_camera_info_app}" "${set_camera_info_cfg}" >"${set_camera_info_server_log}" 2>&1 &
set_camera_info_server_pid=$!
PIDS+=("${set_camera_info_server_pid}")
sleep 3
if ! kill -0 "${set_camera_info_server_pid}" 2>/dev/null; then
  cat "${set_camera_info_server_log}" >&2 || true
  fail "IBMW reference SetCameraInfo server exited before live acceptance"
fi

wait_for_service "${SENSOR_SET_CAMERA_INFO_SERVICE_NAME}" "${set_camera_info_service_list_log}"

timeout 10 ros2 service type "${SENSOR_SET_CAMERA_INFO_SERVICE_NAME}" >"${set_camera_info_service_type_log}" 2>&1 || {
  cat "${set_camera_info_service_type_log}" >&2 || true
  fail "ros2 service type ${SENSOR_SET_CAMERA_INFO_SERVICE_NAME} failed"
}
grep -q "^${set_camera_info_package}/srv/SetCameraInfo$" "${set_camera_info_service_type_log}" || {
  cat "${set_camera_info_service_type_log}" >&2 || true
  fail "service ${SENSOR_SET_CAMERA_INFO_SERVICE_NAME} did not report type ${set_camera_info_package}/srv/SetCameraInfo"
}

timeout 30 ros2 service call "${SENSOR_SET_CAMERA_INFO_SERVICE_NAME}" "${set_camera_info_package}/srv/SetCameraInfo" \
  "{camera_info: {header: {stamp: {sec: 12, nanosec: 345000000}, frame_id: 'camera_optical_frame'}, height: 480, width: 640, distortion_model: 'plumb_bob', d: [0.1, -0.01, 0.001, 0.002, 0.0], k: [500.0, 0.0, 320.0, 0.0, 501.0, 240.0, 0.0, 0.0, 1.0], r: [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0], p: [500.0, 0.0, 320.0, 0.0, 0.0, 501.0, 240.0, 0.0, 0.0, 0.0, 1.0, 0.0], binning_x: 1, binning_y: 1, roi: {x_offset: 10, y_offset: 20, height: 240, width: 320, do_rectify: true}}}" >"${set_camera_info_service_call_log}" 2>&1 || {
  cat "${set_camera_info_service_call_log}" >&2 || true
  fail "ros2 service call ${SENSOR_SET_CAMERA_INFO_SERVICE_NAME} failed"
}
grep -Eq 'success[=:] ?(True|true)' "${set_camera_info_service_call_log}" || {
  cat "${set_camera_info_service_call_log}" >&2 || true
  fail "SetCameraInfo service call did not return success=true"
}
grep -q 'camera_info accepted' "${set_camera_info_service_call_log}" || {
  cat "${set_camera_info_service_call_log}" >&2 || true
  fail "SetCameraInfo service call did not return accepted message"
}

stop_pid "${set_camera_info_server_pid}"

setsid env SENSOR_SET_CAMERA_INFO_SERVICE_NAME="${SENSOR_SET_CAMERA_INFO_SERVICE_NAME}" /usr/bin/python3 "${ros2_set_camera_info_server_file}" >"${ros2_set_camera_info_server_log}" 2>&1 &
ros2_set_camera_info_server_pid=$!
PIDS+=("${ros2_set_camera_info_server_pid}")
sleep 3
if ! kill -0 "${ros2_set_camera_info_server_pid}" 2>/dev/null; then
  cat "${ros2_set_camera_info_server_log}" >&2 || true
  fail "ROS2 reference SetCameraInfo server exited before reverse live acceptance"
fi

wait_for_service "${SENSOR_SET_CAMERA_INFO_SERVICE_NAME}" "${set_camera_info_service_list_log}"

timeout 20 setsid bash -c 'cd "$1" && exec "$2" "$3"' _ \
  "${set_camera_info_work_dir}/runtime_project_build" "${set_camera_info_client_app}" "${set_camera_info_client_cfg}" >"${set_camera_info_client_log}" 2>&1 || true

grep -q 'IBMW_REFERENCE_SRV_CLIENT_SET_CAMERA_INFO_OK' "${set_camera_info_client_log}" || {
  cat "${set_camera_info_client_log}" >&2 || true
  fail "IBMW reference SetCameraInfo client did not receive accepted response from ROS2 server"
}

python3 - "${WORK_DIR}" <<'PY'
import json
import pathlib
import sys

work = pathlib.Path(sys.argv[1])
cases = [
    ("add_two_ints", "example_interfaces/srv/AddTwoInts", "/add_two_ints"),
    ("set_bool", "example_interfaces/srv/SetBool", "/set_bool"),
    ("trigger", "std_srvs/srv/Trigger", "/trigger"),
    ("empty", "std_srvs/srv/Empty", "/empty"),
    ("get_state", "lifecycle_msgs/srv/GetState", "/get_state"),
    ("change_state", "lifecycle_msgs/srv/ChangeState", "/change_state"),
    ("get_available_states", "lifecycle_msgs/srv/GetAvailableStates", "/get_available_states"),
    ("frame_graph", "tf2_msgs/srv/FrameGraph", "/frame_graph"),
    ("self_test", "diagnostic_msgs/srv/SelfTest", "/self_test"),
    ("set_camera_info", "sensor_msgs/srv/SetCameraInfo", "/set_camera_info"),
]
summary = {
    "schema_version": 1,
    "artifact": "reference_srv_live_matrix_summary",
    "lane": "reference_srv",
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
path = work / "reference_srv_live_matrix_summary.json"
path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
print(f"[PASS] reference srv live matrix summary: {path}")
PY

echo "PASS: reference_existing .srv live bidirectional ROS2/IBMW service acceptance (${WORK_DIR}; AddTwoInts + SetBool + Trigger + Empty + GetState + ChangeState + GetAvailableStates + FrameGraph + SelfTest + SetCameraInfo)"
