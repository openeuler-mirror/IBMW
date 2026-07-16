#!/usr/bin/env bash
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${1:-${TMPDIR:-/tmp}/ibmw-rosidl-artifact-smoke-script}"
PACKAGE_NAME="add_two_ints_interfaces"
IBMW_INSTALL_PREFIX="${IBMW_INSTALL_PREFIX:-${TMPDIR:-/tmp}/ibmw-msggen-validation/ibmw-host-install-phase0}"

if [[ ! -f "${IBMW_INSTALL_PREFIX}/lib/cmake/ibmw/ibmw-config.cmake" ]]; then
  echo "error: IBMW install prefix not found or incomplete: ${IBMW_INSTALL_PREFIX}" >&2
  echo "hint: set IBMW_INSTALL_PREFIX to a validated IBMW install tree" >&2
  exit 2
fi

rm -rf "${WORK_DIR}"
mkdir -p "${WORK_DIR}/input/msg" "${WORK_DIR}/input/srv" "${WORK_DIR}/ws/src"

cat > "${WORK_DIR}/input/msg/NestedLeaf.msg" <<'EOF'
int32 id
string label
EOF

cat > "${WORK_DIR}/input/msg/NestedTop.msg" <<'EOF'
NestedLeaf leaf
float64[3] xyz
EOF

cat > "${WORK_DIR}/input/msg/DynamicBlob.msg" <<'EOF'
string name
uint8[] payload
NestedLeaf[] leaves
EOF

cat > "${WORK_DIR}/input/srv/AddTwoInts.srv" <<'EOF'
int64 a
int64 b
---
int64 sum
EOF

cat > "${WORK_DIR}/input/srv/SetMode.srv" <<'EOF'
string mode
bool enable
---
bool accepted
string reason
EOF

cat > "${WORK_DIR}/input/srv/ProcessNested.srv" <<'EOF'
NestedTop input
---
NestedLeaf selected
float64 score
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
  --output-dir "${WORK_DIR}/${PACKAGE_NAME}" \
  --package-name "${PACKAGE_NAME}" \
  --format json > "${WORK_DIR}/ibmw_import_report.json"

cp -a "${WORK_DIR}/${PACKAGE_NAME}" "${WORK_DIR}/ws/src/${PACKAGE_NAME}"

set +u
source /opt/ros/humble/setup.bash
set -u
export LD_LIBRARY_PATH="/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
export PYTHONPATH="/opt/ros/humble/lib/python3.10/site-packages:/opt/ros/humble/local/lib/python3.10/dist-packages${PYTHONPATH:+:${PYTHONPATH}}"
PYTHON_INCLUDE_DIR=$(/usr/bin/python3 - <<'PY'
import sysconfig
print(sysconfig.get_path('include'))
PY
)
PYTHON_LIBRARY=$(/usr/bin/python3 - <<'PY'
import pathlib
import sysconfig
libdir = pathlib.Path(sysconfig.get_config_var('LIBDIR'))
ldlibrary = sysconfig.get_config_var('LDLIBRARY')
print(libdir / ldlibrary)
PY
)

(
  cd "${WORK_DIR}/ws"
  colcon build \
    --packages-select "${PACKAGE_NAME}" \
    --cmake-args \
      -DPython3_EXECUTABLE=/usr/bin/python3 \
      -DPYTHON_EXECUTABLE=/usr/bin/python3 \
      -DPYTHON_INCLUDE_DIR="${PYTHON_INCLUDE_DIR}" \
      -DPYTHON_LIBRARY="${PYTHON_LIBRARY}"
)

test -f "${WORK_DIR}/ws/install/${PACKAGE_NAME}/share/${PACKAGE_NAME}/package.xml"
INSTALL_PREFIX="${WORK_DIR}/ws/install/${PACKAGE_NAME}"
FASTRTPS_CPP_LIB="${INSTALL_PREFIX}/lib/lib${PACKAGE_NAME}__rosidl_typesupport_fastrtps_cpp.so"
FASTRTPS_C_LIB="${INSTALL_PREFIX}/lib/lib${PACKAGE_NAME}__rosidl_typesupport_fastrtps_c.so"
FASTRTPS_CPP_HEADER="${INSTALL_PREFIX}/include/${PACKAGE_NAME}/${PACKAGE_NAME}/srv/detail/add_two_ints__rosidl_typesupport_fastrtps_cpp.hpp"
FASTRTPS_C_HEADER="${INSTALL_PREFIX}/include/${PACKAGE_NAME}/${PACKAGE_NAME}/srv/detail/add_two_ints__rosidl_typesupport_fastrtps_c.h"
NESTED_TOP_HEADER="${INSTALL_PREFIX}/include/${PACKAGE_NAME}/${PACKAGE_NAME}/msg/nested_top.hpp"
NESTED_TOP_FASTRTPS_HEADER="${INSTALL_PREFIX}/include/${PACKAGE_NAME}/${PACKAGE_NAME}/msg/detail/nested_top__rosidl_typesupport_fastrtps_cpp.hpp"
DYNAMIC_BLOB_HEADER="${INSTALL_PREFIX}/include/${PACKAGE_NAME}/${PACKAGE_NAME}/msg/dynamic_blob.hpp"
DYNAMIC_BLOB_FASTRTPS_HEADER="${INSTALL_PREFIX}/include/${PACKAGE_NAME}/${PACKAGE_NAME}/msg/detail/dynamic_blob__rosidl_typesupport_fastrtps_cpp.hpp"

test -f "${FASTRTPS_CPP_LIB}"
test -f "${FASTRTPS_C_LIB}"
test -f "${FASTRTPS_CPP_HEADER}"
test -f "${FASTRTPS_C_HEADER}"
test -f "${INSTALL_PREFIX}/include/${PACKAGE_NAME}/${PACKAGE_NAME}/srv/detail/set_mode__rosidl_typesupport_fastrtps_cpp.hpp"
test -f "${INSTALL_PREFIX}/include/${PACKAGE_NAME}/${PACKAGE_NAME}/srv/detail/process_nested__rosidl_typesupport_fastrtps_cpp.hpp"
test -f "${INSTALL_PREFIX}/include/${PACKAGE_NAME}/${PACKAGE_NAME}/srv/detail/batch_echo__rosidl_typesupport_fastrtps_cpp.hpp"
test -f "${NESTED_TOP_HEADER}"
test -f "${NESTED_TOP_FASTRTPS_HEADER}"
test -f "${DYNAMIC_BLOB_HEADER}"
test -f "${DYNAMIC_BLOB_FASTRTPS_HEADER}"
test -f "${INSTALL_PREFIX}/share/${PACKAGE_NAME}/cmake/export_${PACKAGE_NAME}__rosidl_typesupport_fastrtps_cppExport.cmake"
test -f "${INSTALL_PREFIX}/share/${PACKAGE_NAME}/cmake/export_${PACKAGE_NAME}__rosidl_typesupport_fastrtps_cExport.cmake"
test -f "${INSTALL_PREFIX}/include/${PACKAGE_NAME}/ibmw_interface_types.hpp"
test -f "${INSTALL_PREFIX}/include/${PACKAGE_NAME}/ibmw_rpc_bridge_skeleton.hpp"
test -f "${INSTALL_PREFIX}/include/${PACKAGE_NAME}/ibmw_rpc_bridge_runtime.hpp"

mkdir -p "${WORK_DIR}/consumer"
cat > "${WORK_DIR}/consumer/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.16)
project(generated_interface_consumer)

find_package(ament_cmake REQUIRED)
find_package(${PACKAGE_NAME} REQUIRED)
find_package(fastcdr REQUIRED)
find_package(fastrtps REQUIRED)

include("${ROOT_DIR}/cmake/IbmwHelpers.cmake")

ibmw_add_rosidl_interface_target(${PACKAGE_NAME}_ibmw_glue
  PACKAGE_NAME ${PACKAGE_NAME})

if(NOT TARGET ${PACKAGE_NAME}::${PACKAGE_NAME}__rosidl_typesupport_fastrtps_cpp)
  message(FATAL_ERROR "missing FastRTPS C++ typesupport target")
endif()
if(NOT TARGET ${PACKAGE_NAME}::${PACKAGE_NAME}__rosidl_typesupport_fastrtps_c)
  message(FATAL_ERROR "missing FastRTPS C typesupport target")
endif()

add_executable(consumer main.cpp)
target_include_directories(consumer PRIVATE
  "${ROOT_DIR}/src/extensions/transport")
ament_target_dependencies(consumer ${PACKAGE_NAME})
target_link_libraries(consumer
  ${PACKAGE_NAME}_ibmw_glue
  fastrtps
  fastcdr)
EOF

cat > "${WORK_DIR}/consumer/main.cpp" <<EOF
#include <cstdint>

#include "${PACKAGE_NAME}/ibmw_interface_types.hpp"
#include "${PACKAGE_NAME}/ibmw_rpc_bridge_skeleton.hpp"
#include "${PACKAGE_NAME}/msg/detail/dynamic_blob__rosidl_typesupport_fastrtps_cpp.hpp"
#include "${PACKAGE_NAME}/msg/detail/nested_top__rosidl_typesupport_fastrtps_cpp.hpp"
#include "${PACKAGE_NAME}/msg/dynamic_blob.hpp"
#include "${PACKAGE_NAME}/msg/nested_top.hpp"
#include "${PACKAGE_NAME}/srv/add_two_ints.hpp"

#include "ros2_types/util/ros2_cdr_serializer.h"

#include <string_view>

int main() {
  static_assert(ibmw::ros2_interfaces::${PACKAGE_NAME}::kInterfaceTypes.size() == 7);
  static_assert(ibmw::ros2_interfaces::${PACKAGE_NAME}::kPackageName[0] == 'a');
  ibmw::ros2_interfaces::${PACKAGE_NAME}::NestedTop nested;
  nested.leaf.id = 7;
  nested.leaf.label = "leaf";
  nested.xyz[0] = 1.0;
  nested.xyz[1] = 2.0;
  nested.xyz[2] = 3.0;

  char storage[512]{};
  eprosima::fastcdr::FastBuffer buffer(storage, sizeof(storage));
  eprosima::fastcdr::Cdr ser(
      buffer,
      eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
      ibmw::ros2::ROS2CdrSerializer::kCdrType);
  ibmw::ros2::ROS2CdrSerializer::SerializeMessage(ser, nested);

  ibmw::ros2_interfaces::${PACKAGE_NAME}::NestedTop nested_out;
  eprosima::fastcdr::Cdr deser(
      buffer,
      eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
      ibmw::ros2::ROS2CdrSerializer::kCdrType);
  ibmw::ros2::ROS2CdrSerializer::DeserializeMessage(deser, nested_out);
  if (nested_out.leaf.id != 7 || nested_out.leaf.label != "leaf" || nested_out.xyz[2] != 3.0) {
    return 2;
  }

  ibmw::ros2_interfaces::${PACKAGE_NAME}::DynamicBlob dynamic;
  dynamic.name = "dynamic";
  dynamic.payload = {1, 2, 3, 4, 5};
  dynamic.leaves.resize(2);
  dynamic.leaves[0].id = 11;
  dynamic.leaves[0].label = "left";
  dynamic.leaves[1].id = 12;
  dynamic.leaves[1].label = "right";

  char dynamic_storage[2048]{};
  eprosima::fastcdr::FastBuffer dynamic_buffer(dynamic_storage, sizeof(dynamic_storage));
  eprosima::fastcdr::Cdr dynamic_ser(
      dynamic_buffer,
      eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
      ibmw::ros2::ROS2CdrSerializer::kCdrType);
  ibmw::ros2::ROS2CdrSerializer::SerializeMessage(dynamic_ser, dynamic);

  ibmw::ros2_interfaces::${PACKAGE_NAME}::DynamicBlob dynamic_out;
  eprosima::fastcdr::Cdr dynamic_deser(
      dynamic_buffer,
      eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
      ibmw::ros2::ROS2CdrSerializer::kCdrType);
  ibmw::ros2::ROS2CdrSerializer::DeserializeMessage(dynamic_deser, dynamic_out);
  if (dynamic_out.name != "dynamic" || dynamic_out.payload.size() != 5 ||
      dynamic_out.payload[4] != 5 || dynamic_out.leaves.size() != 2 ||
      dynamic_out.leaves[1].id != 12 || dynamic_out.leaves[1].label != "right") {
    return 3;
  }

  using Bridge = ibmw::ros2_interfaces::${PACKAGE_NAME}::AddTwoIntsRpcBridgeSkeleton;
  static_assert(!Bridge::kRuntimeGlueGenerated);
  static_assert(std::string_view(Bridge::kCanonicalService) == "${PACKAGE_NAME}/srv/AddTwoInts");
  static_assert(std::string_view(Bridge::kRos2ServiceName) == "/add_two_ints");
  static_assert(std::string_view(Bridge::kRequestDdsTypeName) == "${PACKAGE_NAME}::srv::dds_::AddTwoInts_Request_");
  static_assert(std::string_view(Bridge::kResponseDdsTypeName) == "${PACKAGE_NAME}::srv::dds_::AddTwoInts_Response_");
  ibmw::ros2_interfaces::${PACKAGE_NAME}::AddTwoInts_Request request;
  request.a = 20;
  request.b = 22;
  Bridge::Response response;
  response.sum = request.a + request.b;
  using SetModeBridge = ibmw::ros2_interfaces::${PACKAGE_NAME}::SetModeRpcBridgeSkeleton;
  static_assert(std::string_view(SetModeBridge::kCanonicalService) == "${PACKAGE_NAME}/srv/SetMode");
  static_assert(std::string_view(SetModeBridge::kRos2ServiceName) == "/set_mode");
  static_assert(std::string_view(SetModeBridge::kRequestDdsTypeName) == "${PACKAGE_NAME}::srv::dds_::SetMode_Request_");
  static_assert(std::string_view(SetModeBridge::kResponseDdsTypeName) == "${PACKAGE_NAME}::srv::dds_::SetMode_Response_");

  using ProcessNestedBridge = ibmw::ros2_interfaces::${PACKAGE_NAME}::ProcessNestedRpcBridgeSkeleton;
  static_assert(std::string_view(ProcessNestedBridge::kCanonicalService) == "${PACKAGE_NAME}/srv/ProcessNested");
  static_assert(std::string_view(ProcessNestedBridge::kRos2ServiceName) == "/process_nested");

  using BatchEchoBridge = ibmw::ros2_interfaces::${PACKAGE_NAME}::BatchEchoRpcBridgeSkeleton;
  static_assert(std::string_view(BatchEchoBridge::kCanonicalService) == "${PACKAGE_NAME}/srv/BatchEcho");
  static_assert(std::string_view(BatchEchoBridge::kRos2ServiceName) == "/batch_echo");
  return response.sum == 42 ? 0 : 1;
}
EOF

set +u
source "${WORK_DIR}/ws/install/setup.bash"
set -u

cmake \
  -S "${WORK_DIR}/consumer" \
  -B "${WORK_DIR}/consumer_build" \
  -DPython3_EXECUTABLE=/usr/bin/python3
cmake --build "${WORK_DIR}/consumer_build" -j"$(nproc)"
"${WORK_DIR}/consumer_build/consumer"

mkdir -p "${WORK_DIR}/runtime_consumer"
cat > "${WORK_DIR}/runtime_consumer/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.16)
project(generated_srv_runtime_consumer)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(ament_cmake REQUIRED)
find_package(ibmw REQUIRED)
find_package(${PACKAGE_NAME} REQUIRED)
find_package(fastcdr REQUIRED)
find_package(fastrtps REQUIRED)
find_package(rosidl_typesupport_cpp REQUIRED)

ibmw_add_rosidl_interface_target(${PACKAGE_NAME}_ibmw_glue
  PACKAGE_NAME ${PACKAGE_NAME})

if(NOT TARGET ${PACKAGE_NAME}::${PACKAGE_NAME}__rosidl_typesupport_cpp)
  message(FATAL_ERROR "missing ROSIDL C++ service typesupport target")
endif()

add_executable(runtime_consumer main.cpp)
target_link_libraries(runtime_consumer
  ${PACKAGE_NAME}_ibmw_glue
  ${PACKAGE_NAME}::${PACKAGE_NAME}__rosidl_typesupport_cpp
  ibmw::api::cpp
  ibmw::extensions::ros2_types
  rosidl_typesupport_cpp::rosidl_typesupport_cpp
  fastrtps
  fastcdr)
EOF

cat > "${WORK_DIR}/runtime_consumer/main.cpp" <<EOF
#include <algorithm>
#include <string_view>
#include <type_traits>

#include "${PACKAGE_NAME}/ibmw_rpc_bridge_runtime.hpp"

namespace iface = ibmw::ros2_interfaces::${PACKAGE_NAME};

class TestAddTwoIntsServer : public iface::AddTwoIntsSyncServiceAdapter {
 public:
  ibmw::rpc::Status HandleAddTwoInts(
      ibmw::rpc::ContextRef ctx_ref,
      const Request& request,
      Response& response) override {
    (void)ctx_ref;
    response.sum = request.a + request.b;
    return ibmw::rpc::Status(IBMW_RPC_STATUS_OK);
  }
};

class TestSetModeServer : public iface::SetModeSyncServiceAdapter {
 public:
  ibmw::rpc::Status HandleSetMode(
      ibmw::rpc::ContextRef ctx_ref,
      const Request& request,
      Response& response) override {
    (void)ctx_ref;
    response.accepted = request.enable && !request.mode.empty();
    response.reason = response.accepted ? "accepted" : "rejected";
    return ibmw::rpc::Status(IBMW_RPC_STATUS_OK);
  }
};

class TestProcessNestedServer : public iface::ProcessNestedSyncServiceAdapter {
 public:
  ibmw::rpc::Status HandleProcessNested(
      ibmw::rpc::ContextRef ctx_ref,
      const Request& request,
      Response& response) override {
    (void)ctx_ref;
    response.selected = request.input.leaf;
    response.score = request.input.xyz[0] + request.input.xyz[1] + request.input.xyz[2];
    return ibmw::rpc::Status(IBMW_RPC_STATUS_OK);
  }
};

class TestBatchEchoServer : public iface::BatchEchoSyncServiceAdapter {
 public:
  ibmw::rpc::Status HandleBatchEcho(
      ibmw::rpc::ContextRef ctx_ref,
      const Request& request,
      Response& response) override {
    (void)ctx_ref;
    response.echoed_values.assign(request.fixed_values.begin(), request.fixed_values.end());
    response.bounded_payload.assign(request.payload.begin(), request.payload.begin() + std::min<size_t>(request.payload.size(), 8));
    return ibmw::rpc::Status(IBMW_RPC_STATUS_OK);
  }
};

int main() {
  static_assert(iface::AddTwoIntsRpcBridgeRuntime::kTypedRegistrationGenerated);
  static_assert(!iface::AddTwoIntsRpcBridgeRuntime::kRuntimeGlueGenerated);
  static_assert(std::string_view(iface::AddTwoIntsRpcBridgeRuntime::kRpcType) == "ros2");
  static_assert(std::string_view(iface::AddTwoIntsRpcBridgeRuntime::kServiceGroup) == "${PACKAGE_NAME}/srv");
  static_assert(std::is_base_of_v<ibmw::rpc::ServiceBase, iface::AddTwoIntsSyncServiceAdapter>);
  static_assert(std::is_base_of_v<ibmw::rpc::ServiceBase, iface::SetModeSyncServiceAdapter>);
  static_assert(std::is_base_of_v<ibmw::rpc::ServiceBase, iface::ProcessNestedSyncServiceAdapter>);
  static_assert(std::is_base_of_v<ibmw::rpc::ServiceBase, iface::BatchEchoSyncServiceAdapter>);
  static_assert(std::is_base_of_v<ibmw::rpc::SyncAccessBase, iface::AddTwoIntsSyncClientAdapter>);
  static_assert(std::is_base_of_v<ibmw::rpc::SyncAccessBase, iface::SetModeSyncClientAdapter>);
  static_assert(std::is_base_of_v<ibmw::rpc::SyncAccessBase, iface::ProcessNestedSyncClientAdapter>);
  static_assert(std::is_base_of_v<ibmw::rpc::SyncAccessBase, iface::BatchEchoSyncClientAdapter>);
  auto* service_ts = iface::AddTwoIntsRpcBridgeRuntime::ServiceTypeSupport();
  auto* request_ts = iface::AddTwoIntsRpcBridgeRuntime::RequestTypeSupport();
  auto* response_ts = iface::AddTwoIntsRpcBridgeRuntime::ResponseTypeSupport();
  if (service_ts == nullptr || request_ts == nullptr || response_ts == nullptr ||
      iface::SetModeRpcBridgeRuntime::ServiceTypeSupport() == nullptr ||
      iface::ProcessNestedRpcBridgeRuntime::RequestTypeSupport() == nullptr ||
      iface::BatchEchoRpcBridgeRuntime::ResponseTypeSupport() == nullptr) {
    return 2;
  }

  TestAddTwoIntsServer server;
  iface::AddTwoIntsRpcBridgeRuntime::Request request;
  iface::AddTwoIntsRpcBridgeRuntime::Response response;
  request.a = 19;
  request.b = 23;
  auto status = server.HandleAddTwoInts(ibmw::rpc::ContextRef(), request, response);
  if (status.Code() != IBMW_RPC_STATUS_OK || response.sum != 42) {
    return 3;
  }

  TestSetModeServer set_mode_server;
  iface::SetModeRpcBridgeRuntime::Request set_mode_request;
  iface::SetModeRpcBridgeRuntime::Response set_mode_response;
  set_mode_request.mode = "auto";
  set_mode_request.enable = true;
  auto set_mode_status = set_mode_server.HandleSetMode(ibmw::rpc::ContextRef(), set_mode_request, set_mode_response);
  if (set_mode_status.Code() != IBMW_RPC_STATUS_OK || !set_mode_response.accepted || set_mode_response.reason != "accepted") {
    return 4;
  }

  TestProcessNestedServer process_nested_server;
  iface::ProcessNestedRpcBridgeRuntime::Request process_nested_request;
  iface::ProcessNestedRpcBridgeRuntime::Response process_nested_response;
  process_nested_request.input.leaf.id = 99;
  process_nested_request.input.leaf.label = "chosen";
  process_nested_request.input.xyz[0] = 1.0;
  process_nested_request.input.xyz[1] = 2.0;
  process_nested_request.input.xyz[2] = 4.0;
  auto process_status = process_nested_server.HandleProcessNested(ibmw::rpc::ContextRef(), process_nested_request, process_nested_response);
  if (process_status.Code() != IBMW_RPC_STATUS_OK || process_nested_response.selected.id != 99 ||
      process_nested_response.selected.label != "chosen" || process_nested_response.score != 7.0) {
    return 5;
  }

  TestBatchEchoServer batch_echo_server;
  iface::BatchEchoRpcBridgeRuntime::Request batch_request;
  iface::BatchEchoRpcBridgeRuntime::Response batch_response;
  batch_request.fixed_values = {3, 1, 4};
  batch_request.payload = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  batch_request.leaves.resize(1);
  batch_request.leaves[0].id = 10;
  batch_request.leaves[0].label = "leaf";
  auto batch_status = batch_echo_server.HandleBatchEcho(ibmw::rpc::ContextRef(), batch_request, batch_response);
  if (batch_status.Code() != IBMW_RPC_STATUS_OK || batch_response.echoed_values.size() != 3 ||
      batch_response.echoed_values[2] != 4 || batch_response.bounded_payload.size() != 8 ||
      batch_response.bounded_payload[7] != 8) {
    return 6;
  }
  return 0;
}
EOF

cmake \
  -S "${WORK_DIR}/runtime_consumer" \
  -B "${WORK_DIR}/runtime_consumer_build" \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DCMAKE_PREFIX_PATH="${IBMW_INSTALL_PREFIX};${CMAKE_PREFIX_PATH:-}" \
  -Dibmw_DIR="${IBMW_INSTALL_PREFIX}/lib/cmake/ibmw"
cmake --build "${WORK_DIR}/runtime_consumer_build" -j"$(nproc)"
"${WORK_DIR}/runtime_consumer_build/runtime_consumer"

python3 "${ROOT_DIR}/src/tools/ibmw_gen/ibmw_gen.py" \
  "${WORK_DIR}/${PACKAGE_NAME}/${PACKAGE_NAME}_imported_srv_runtime.ibmw.yaml" \
  -o "${WORK_DIR}/generated_runtime_project" \
  --force

test -f "${WORK_DIR}/generated_runtime_project/CMakeLists.txt"
test -f "${WORK_DIR}/generated_runtime_project/src/add_two_ints_rpc_server_module/add_two_ints_rpc_server_module.cc"
test -f "${WORK_DIR}/generated_runtime_project/src/add_two_ints_rpc_client_module/add_two_ints_rpc_client_module.cc"

cmake \
  -S "${WORK_DIR}/generated_runtime_project" \
  -B "${WORK_DIR}/generated_runtime_project_build" \
  -DPython3_EXECUTABLE=/usr/bin/python3 \
  -DCMAKE_PREFIX_PATH="${IBMW_INSTALL_PREFIX};${INSTALL_PREFIX};${CMAKE_PREFIX_PATH:-}" \
  -Dibmw_DIR="${IBMW_INSTALL_PREFIX}/lib/cmake/ibmw"
cmake --build "${WORK_DIR}/generated_runtime_project_build" -j"$(nproc)"

PYTHONPATH="${ROOT_DIR}/src/tools/ibmw_gen:${PYTHONPATH:-}" \
python3 - "${FASTRTPS_CPP_LIB}" "${FASTRTPS_C_LIB}" <<'PY'
import subprocess
import sys
from ros2_interface_import import audit_target_artifacts

artifacts = sys.argv[1:]
report = audit_target_artifacts(
    artifacts,
    forbidden_prefixes=["/opt/ros/humble", "/usr/lib/x86_64-linux-gnu"],
    expected_arch="x86_64",
)
if report["status"] != "pass":
    raise SystemExit("artifact audit failed: " + "; ".join(report["reasons"]))

for artifact in artifacts:
    dynamic = subprocess.run(
        ["readelf", "-d", artifact],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    if "RPATH" in dynamic or "RUNPATH" in dynamic:
        raise SystemExit(f"unexpected RPATH/RUNPATH in {artifact}")
PY

echo "PASS: generated rosidl artifact package host build + external CMake consumer smoke (${WORK_DIR})"
