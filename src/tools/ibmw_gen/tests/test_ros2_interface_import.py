#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

import pathlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
IBMW_GEN = ROOT / "ibmw_gen.py"
sys.path.insert(0, str(ROOT))

from ros2_interface_import import InterfaceParseError, audit_target_artifacts, build_ros2_compatibility_lock, collect_ros2_compatibility, diagnose_interface, diagnose_interface_json, emit_reference_existing_discovery_report, emit_reference_existing_interface_report, emit_rosidl_artifact_package, emit_workspace_interface_discovery_report, format_ros2_compatibility_lock_yaml, parse_interface_file, resolve_reference_existing_prefix_from_environment  # noqa: E402


class Ros2InterfaceImportTests(unittest.TestCase):
    def _write_tmp(self, suffix: str, text: str) -> pathlib.Path:
        f = tempfile.NamedTemporaryFile(
            mode="w", suffix=suffix, delete=False, encoding="utf-8"
        )
        with f:
            f.write(text)
        return pathlib.Path(f.name)

    def _write_fake_ros2_prefix(self, root: pathlib.Path, version: str = "1.2.3") -> pathlib.Path:
        prefix = root
        package_xml_names = (
            "rosidl_generator_cpp",
            "rosidl_runtime_c",
            "rosidl_runtime_cpp",
            "rosidl_typesupport_fastrtps_c",
            "rosidl_typesupport_fastrtps_cpp",
        )
        for name in package_xml_names:
            package_dir = prefix / "share" / name
            package_dir.mkdir(parents=True)
            package_dir.joinpath("package.xml").write_text(
                f"<package format='3'><name>{name}</name><version>{version}</version></package>\n",
                encoding="utf-8",
            )
        fastrtps_dir = prefix / "share" / "fastrtps" / "cmake"
        fastrtps_dir.mkdir(parents=True)
        fastrtps_dir.joinpath("fastrtps-config-version.cmake").write_text(
            f"set(PACKAGE_VERSION \"{version}\")\n", encoding="utf-8"
        )
        fastcdr_dir = prefix / "lib" / "cmake" / "fastcdr"
        fastcdr_dir.mkdir(parents=True)
        fastcdr_dir.joinpath("fastcdr-config-version.cmake").write_text(
            f"set(PACKAGE_VERSION \"{version}\")\n", encoding="utf-8"
        )
        return prefix

    def _write_known_good_humble_prefix(self, root: pathlib.Path) -> pathlib.Path:
        prefix = root
        package_versions = {
            "rosidl_generator_cpp": "3.1.8",
            "rosidl_runtime_c": "3.1.8",
            "rosidl_runtime_cpp": "3.1.8",
            "rosidl_typesupport_fastrtps_c": "2.2.4",
            "rosidl_typesupport_fastrtps_cpp": "2.2.4",
        }
        for name, version in package_versions.items():
            package_dir = prefix / "share" / name
            package_dir.mkdir(parents=True)
            package_dir.joinpath("package.xml").write_text(
                f"<package format='3'><name>{name}</name><version>{version}</version></package>\n",
                encoding="utf-8",
            )
        fastrtps_dir = prefix / "share" / "fastrtps" / "cmake"
        fastrtps_dir.mkdir(parents=True)
        fastrtps_dir.joinpath("fastrtps-config-version.cmake").write_text(
            "set(PACKAGE_VERSION \"2.6.11\")\n", encoding="utf-8"
        )
        fastcdr_dir = prefix / "lib" / "cmake" / "fastcdr"
        fastcdr_dir.mkdir(parents=True)
        fastcdr_dir.joinpath("fastcdr-config-version.cmake").write_text(
            "set(PACKAGE_VERSION \"1.0.29\")\n", encoding="utf-8"
        )
        return prefix

    def _write_fake_elf(self, path: pathlib.Path, arch: str, payload: bytes = b"") -> pathlib.Path:
        machine = {"x86": 3, "arm": 40, "x86_64": 62, "aarch64": 183}[arch]
        data = bytearray(b"\x7fELF")
        data.extend(b"\x02\x01\x01")
        data.extend(b"\x00" * (18 - len(data)))
        data.extend(machine.to_bytes(2, "little"))
        data.extend(b"\x00" * 44)
        data.extend(payload)
        path.write_bytes(bytes(data))
        return path

    def test_parser_msg_constants_primitives_string_array_external(self) -> None:
        path = self._write_tmp(
            ".msg",
            """
# leading comment
uint8 FOO=1
string BAR="x"  # trailing comment
int32 count
float64[3] samples
uint8[] payload
string<=16 label
std_msgs/Header header
geometry_msgs/msg/Point point
""",
        )
        try:
            ir = parse_interface_file(str(path))
        finally:
            path.unlink(missing_ok=True)

        self.assertEqual(ir.kind, "msg")
        self.assertEqual(ir.package, "<unknown>")
        self.assertEqual(ir.type_path, f"msg/{path.stem}")
        self.assertEqual(len(ir.messages), 1)
        msg = ir.messages[0]
        self.assertEqual([c.name for c in msg.constants], ["FOO", "BAR"])
        self.assertEqual([f.name for f in msg.fields], [
            "count", "samples", "payload", "label", "header", "point"
        ])
        self.assertEqual(msg.external_refs, ["geometry_msgs/msg/Point", "std_msgs/Header"])
        self.assertFalse(msg.is_plain_compatible)
        self.assertIn("field payload is an unbounded sequence", msg.plain_reasons)
        self.assertIn("field label uses string type string<=16", msg.plain_reasons)

    def test_parser_srv_request_response_split(self) -> None:
        path = self._write_tmp(
            ".srv",
            """
int64 a
int64 b
--- # request/response separator comment
int64 sum
""",
        )
        try:
            ir = parse_interface_file(str(path))
        finally:
            path.unlink(missing_ok=True)

        self.assertEqual(ir.kind, "srv")
        self.assertEqual(ir.package, "<unknown>")
        self.assertEqual(ir.type_path, f"srv/{path.stem}")
        self.assertEqual([m.name for m in ir.messages], [f"{path.stem}_Request", f"{path.stem}_Response"])
        self.assertTrue(all(m.is_plain_compatible for m in ir.messages))

    def test_parser_action_goal_result_feedback_split(self) -> None:
        path = self._write_tmp(
            ".action",
            """
int32 order
string<=16 label
--- # goal/result separator comment
bool success
string message
---
float32[] progress
geometry_msgs/msg/Point waypoint
""",
        )
        try:
            ir = parse_interface_file(str(path))
        finally:
            path.unlink(missing_ok=True)

        self.assertEqual(ir.kind, "action")
        self.assertEqual(ir.package, "<unknown>")
        self.assertEqual(ir.type_path, f"action/{path.stem}")
        self.assertEqual(
            [m.name for m in ir.messages],
            [f"{path.stem}_Goal", f"{path.stem}_Result", f"{path.stem}_Feedback"],
        )
        self.assertEqual([f.name for f in ir.messages[0].fields], ["order", "label"])
        self.assertFalse(ir.messages[0].is_plain_compatible)
        self.assertIn("field label uses string type string<=16", ir.messages[0].plain_reasons)
        self.assertEqual([f.name for f in ir.messages[1].fields], ["success", "message"])
        self.assertFalse(ir.messages[1].is_plain_compatible)
        self.assertIn("field message uses string type string", ir.messages[1].plain_reasons)
        self.assertEqual([f.name for f in ir.messages[2].fields], ["progress", "waypoint"])
        self.assertEqual(ir.messages[2].external_refs, ["geometry_msgs/msg/Point"])
        self.assertFalse(ir.messages[2].is_plain_compatible)
        self.assertIn("field progress is an unbounded sequence", ir.messages[2].plain_reasons)

    def test_cli_diagnose_interface_success(self) -> None:
        path = self._write_tmp(
            ".msg",
            """
uint8 OK=1
int32[2] point
builtin_interfaces/Time stamp
string name
""",
        )
        try:
            result = subprocess.run(
                [sys.executable, str(IBMW_GEN), "diagnose-interface", str(path)],
                capture_output=True,
                text=True,
                timeout=30,
            )
        finally:
            path.unlink(missing_ok=True)

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"interface: msg {path.stem}", result.stdout)
        self.assertIn("package: <unknown>", result.stdout)
        self.assertIn(f"type-path: msg/{path.stem}", result.stdout)
        self.assertIn("  - uint8 OK=1", result.stdout)
        self.assertIn("  - int32[2] point (builtin)", result.stdout)
        self.assertIn("  - builtin_interfaces/Time", result.stdout)
        self.assertIn("plain-compatible: no", result.stdout)
        self.assertIn("field name uses string type string", result.stdout)

    def test_emit_rosidl_artifact_package_skeleton(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            src = root / "demo_interfaces"
            (src / "msg").mkdir(parents=True)
            (src / "srv").mkdir()
            src.joinpath("package.xml").write_text(
                "<package format='3'><name>demo_interfaces</name><version>1.2.3</version></package>\n",
                encoding="utf-8",
            )
            src.joinpath("msg", "Widget.msg").write_text(
                "std_msgs/Header header\nint32 count\n", encoding="utf-8"
            )
            src.joinpath("msg", "Plain.msg").write_text("float64 x\nfloat64 y\nint32[3] bins\n", encoding="utf-8")
            src.joinpath("msg", "Empty.msg").write_text("", encoding="utf-8")
            src.joinpath("srv", "Add.srv").write_text("int32 a\n---\nint32 sum\n", encoding="utf-8")
            src.joinpath("srv", "SetMode.srv").write_text(
                "string mode\nbool enable\n---\nbool accepted\nstring reason\n", encoding="utf-8"
            )
            out = root / "out"

            report = emit_rosidl_artifact_package(str(src), str(out), targets=["dds_ros2", "dds_native", "iceoryx", "json"])

            self.assertTrue(report["ok"])
            self.assertEqual(report["package"], "demo_interfaces")
            self.assertEqual(report["files"], ["msg/Empty.msg", "msg/Plain.msg", "msg/Widget.msg", "srv/Add.srv", "srv/SetMode.srv"])
            self.assertEqual(report["external_dependencies"], ["std_msgs"])
            self.assertEqual(report["targets"], ["dds_native", "dds_ros2", "iceoryx", "json"])
            self.assertEqual(
                report["target_backend_status"]["summary"],
                {"dds_native": "degraded", "dds_ros2": "supported", "iceoryx": "degraded", "json": "degraded"},
            )
            status_by_target = {entry["target"]: entry for entry in report["target_backend_status"]["entries"]}
            self.assertEqual(status_by_target["dds_ros2"]["runtime_boundary"], "rcl_free_rosidl_fastrtps")
            self.assertEqual(status_by_target["dds_native"]["runtime_boundary"], "metadata_only_no_native_dds_lowering")
            self.assertEqual(status_by_target["iceoryx"]["runtime_boundary"], "metadata_only_no_iceoryx_lowering")
            self.assertEqual(
                report["backend_metadata_files"],
                [
                    "demo_interfaces_dds_native_backend_metadata.json",
                    "demo_interfaces_iceoryx_backend_metadata.json",
                    "demo_interfaces_json_backend_metadata.json",
                ],
            )
            self.assertEqual(
                report["backend_generated_files"],
                [
                    "demo_interfaces_dds_native_lowering_plan.json",
                    "demo_interfaces_iceoryx_lowering_plan.json",
                    "demo_interfaces_json_types.hpp",
                    "demo_interfaces_json_runtime_scaffold.ibmw.yaml",
                ],
            )
            self.assertEqual(
                report["rosidl_targets"]["typesupport_fastrtps_cpp"],
                "demo_interfaces::demo_interfaces__rosidl_typesupport_fastrtps_cpp",
            )
            self.assertEqual(
                report["rosidl_targets"]["typesupport_cpp"],
                "demo_interfaces::demo_interfaces__rosidl_typesupport_cpp",
            )
            self.assertEqual(
                report["rosidl_targets"]["typesupport_fastrtps_c"],
                "demo_interfaces::demo_interfaces__rosidl_typesupport_fastrtps_c",
            )
            self.assertEqual(out.joinpath("msg", "Widget.msg").read_text(encoding="utf-8"), "std_msgs/Header header\nint32 count\n")
            package_xml = out.joinpath("package.xml").read_text(encoding="utf-8")
            cmake_lists = out.joinpath("CMakeLists.txt").read_text(encoding="utf-8")
            wrapper_header = out.joinpath("include", "demo_interfaces", "ibmw_interface_types.hpp").read_text(encoding="utf-8")
            rpc_bridge_header = out.joinpath("include", "demo_interfaces", "ibmw_rpc_bridge_skeleton.hpp").read_text(encoding="utf-8")
            rpc_runtime_header = out.joinpath("include", "demo_interfaces", "ibmw_rpc_bridge_runtime.hpp").read_text(encoding="utf-8")
            report_json = json.loads(out.joinpath("ibmw_interface_report.json").read_text(encoding="utf-8"))
            self.assertIn("<member_of_group>rosidl_interface_packages</member_of_group>", package_xml)
            self.assertIn("<depend>std_msgs</depend>", package_xml)
            self.assertIn("find_package(rosidl_default_generators REQUIRED)", cmake_lists)
            self.assertIn('  "msg/Widget.msg"', cmake_lists)
            self.assertIn("  DEPENDENCIES\n  std_msgs", cmake_lists)
            self.assertIn("install(DIRECTORY include/ DESTINATION include)", cmake_lists)
            self.assertIn("using Widget = ::demo_interfaces::msg::Widget;", wrapper_header)
            self.assertIn("using Add_Request = ::demo_interfaces::srv::Add::Request;", wrapper_header)
            self.assertIn('"srv", "Add", "demo_interfaces/srv/Add"', wrapper_header)
            self.assertIn("struct AddRpcBridgeSkeleton", rpc_bridge_header)
            self.assertIn("struct SetModeRpcBridgeSkeleton", rpc_bridge_header)
            self.assertIn('kCanonicalService = "demo_interfaces/srv/Add"', rpc_bridge_header)
            self.assertIn('kRos2ServiceName = "/add"', rpc_bridge_header)
            self.assertIn('kCanonicalService = "demo_interfaces/srv/SetMode"', rpc_bridge_header)
            self.assertIn('kRos2ServiceName = "/set_mode"', rpc_bridge_header)
            self.assertIn('kRequestDdsTypeName = "demo_interfaces::srv::dds_::Add_Request_"', rpc_bridge_header)
            self.assertIn('kResponseDdsTypeName = "demo_interfaces::srv::dds_::Add_Response_"', rpc_bridge_header)
            self.assertIn("kRuntimeGlueGenerated = false", rpc_bridge_header)
            self.assertIn("struct AddRpcBridgeRuntime : AddRpcBridgeSkeleton", rpc_runtime_header)
            self.assertIn("struct SetModeRpcBridgeRuntime : SetModeRpcBridgeSkeleton", rpc_runtime_header)
            self.assertIn("kTypedRegistrationGenerated = true", rpc_runtime_header)
            self.assertIn("kRuntimeGlueGenerated = false", rpc_runtime_header)
            self.assertIn("Consumer contract: include this header only from an IBMW-aware target", rpc_runtime_header)
            self.assertIn("demo_interfaces::demo_interfaces__rosidl_typesupport_cpp", rpc_runtime_header)
            self.assertIn("C++20 is required", rpc_runtime_header)
            self.assertIn("#include <cpp/ibmw_sdk.h>", rpc_runtime_header)
            self.assertIn("#include <ros2_types/util/ros2_type_support_factory.h>", rpc_runtime_header)
            self.assertIn('kRpcType = "ros2"', rpc_runtime_header)
            self.assertIn('kServiceGroup = "demo_interfaces/srv"', rpc_runtime_header)
            self.assertIn("rosidl_typesupport_cpp::get_service_type_support_handle<Service>()", rpc_runtime_header)
            self.assertIn("ibmw::GetRos2MessageTypeSupport<Request>()", rpc_runtime_header)
            self.assertIn("class AddSyncServiceAdapter : public ibmw::rpc::ServiceBase", rpc_runtime_header)
            self.assertIn("class SetModeSyncServiceAdapter : public ibmw::rpc::ServiceBase", rpc_runtime_header)
            self.assertIn("inline bool RegisterAddClientFunc(ibmw::rpc::ServiceRef service_ref", rpc_runtime_header)
            self.assertIn("inline bool RegisterSetModeClientFunc(ibmw::rpc::ServiceRef service_ref", rpc_runtime_header)
            self.assertIn("class AddSyncClientAdapter : public ibmw::rpc::SyncAccessBase", rpc_runtime_header)
            self.assertIn("class SetModeSyncClientAdapter : public ibmw::rpc::SyncAccessBase", rpc_runtime_header)
            self.assertIn("HandleAdd(", rpc_runtime_header)
            self.assertIn("HandleSetMode(", rpc_runtime_header)
            self.assertIn("ibmw::rpc::GetFullFuncName(", rpc_runtime_header)
            self.assertTrue(report_json["artifact_only"])
            self.assertIn("include/demo_interfaces/ibmw_interface_types.hpp", report_json["generated_files"])
            self.assertIn("include/demo_interfaces/ibmw_rpc_bridge_skeleton.hpp", report_json["generated_files"])
            self.assertIn("include/demo_interfaces/ibmw_rpc_bridge_runtime.hpp", report_json["generated_files"])
            self.assertIn("include/demo_interfaces/ibmw_ros2_conversion.hpp", report_json["generated_files"])
            self.assertIn("demo_interfaces_imported_srv_runtime.ibmw.yaml", report_json["generated_files"])
            self.assertIn("demo_interfaces_dependency_closure.json", report_json["generated_files"])
            self.assertIn("demo_interfaces_dds_native_backend_metadata.json", report_json["generated_files"])
            self.assertIn("demo_interfaces_iceoryx_backend_metadata.json", report_json["generated_files"])
            self.assertIn("demo_interfaces_json_backend_metadata.json", report_json["generated_files"])
            self.assertIn("demo_interfaces_json_types.hpp", report_json["generated_files"])
            self.assertIn("demo_interfaces_json_runtime_scaffold.ibmw.yaml", report_json["generated_files"])
            self.assertIn("demo_interfaces_migration_manifest.json", report_json["generated_files"])
            json_types_header = out.joinpath("demo_interfaces_json_types.hpp").read_text(encoding="utf-8")
            self.assertIn("struct Plain", json_types_header)
            self.assertIn('static constexpr const char* kTypeName = "demo_interfaces/msg/Plain";', json_types_header)
            self.assertIn("double x{};", json_types_header)
            self.assertIn("std::array<int32_t, 3> bins{};", json_types_header)
            self.assertIn("NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Plain, x, y, bins)", json_types_header)
            self.assertNotIn("struct Empty", json_types_header)
            self.assertNotIn("struct Widget", json_types_header)
            json_scaffold = out.joinpath("demo_interfaces_json_runtime_scaffold.ibmw.yaml").read_text(encoding="utf-8")
            self.assertIn("name: demo_interfaces_json_runtime_scaffold", json_scaffold)
            self.assertIn("type: publisher", json_scaffold)
            self.assertIn("type: subscriber", json_scaffold)
            self.assertIn("message_type: ibmw::ros2_interfaces::json::demo_interfaces::Plain", json_scaffold)
            self.assertIn("ros2_import_json_types_header: demo_interfaces_json_types.hpp", json_scaffold)
            self.assertIn(f"ros2_import_json_types_include_dir: {out}", json_scaffold)
            self.assertIn("runtime_lowering_generated: false", json_scaffold)
            self.assertIn("include_contract: ros2_import_json_types_header", json_scaffold)
            self.assertIn("add a JSON pub/sub runtime smoke", json_scaffold)
            dds_native_backend = json.loads(out.joinpath("demo_interfaces_dds_native_backend_metadata.json").read_text(encoding="utf-8"))
            self.assertEqual(dds_native_backend["target"], "dds_native")
            self.assertEqual(dds_native_backend["status"], "degraded")
            self.assertIn("demo_interfaces/msg/Widget", dds_native_backend["degraded_interfaces"])
            self.assertFalse(dds_native_backend["runtime_lowering_generated"])
            dds_native_plan = json.loads(out.joinpath("demo_interfaces_dds_native_lowering_plan.json").read_text(encoding="utf-8"))
            self.assertEqual(dds_native_plan["artifact"], "dds_native_lowering_plan")
            self.assertFalse(dds_native_plan["runtime_lowering_generated"])
            self.assertIn("demo_interfaces/msg/Plain", [entry["canonical"] for entry in dds_native_plan["eligible_interfaces"]])
            iceoryx_backend = json.loads(out.joinpath("demo_interfaces_iceoryx_backend_metadata.json").read_text(encoding="utf-8"))
            self.assertEqual(iceoryx_backend["target"], "iceoryx")
            self.assertEqual(iceoryx_backend["status"], "degraded")
            self.assertIn("demo_interfaces/srv/SetMode", iceoryx_backend["blocked_interfaces"])
            self.assertFalse(iceoryx_backend["runtime_lowering_generated"])
            iceoryx_plan = json.loads(out.joinpath("demo_interfaces_iceoryx_lowering_plan.json").read_text(encoding="utf-8"))
            self.assertEqual(iceoryx_plan["artifact"], "iceoryx_lowering_plan")
            self.assertFalse(iceoryx_plan["runtime_lowering_generated"])
            self.assertIn("demo_interfaces/msg/Plain", [entry["canonical"] for entry in iceoryx_plan["eligible_interfaces"]])
            json_backend = json.loads(out.joinpath("demo_interfaces_json_backend_metadata.json").read_text(encoding="utf-8"))
            self.assertEqual(json_backend["target"], "json")
            self.assertEqual(json_backend["status"], "degraded")
            self.assertTrue(json_backend["metadata_only"])
            self.assertTrue(json_backend["types_generated"])
            self.assertEqual(json_backend["types_header"], "demo_interfaces_json_types.hpp")
            self.assertFalse(json_backend["runtime_lowering_generated"])
            self.assertEqual(
                [entry["canonical"] for entry in json_backend["interfaces"]],
                [
                    "demo_interfaces/msg/Empty",
                    "demo_interfaces/msg/Plain",
                    "demo_interfaces/msg/Widget",
                    "demo_interfaces/srv/Add",
                    "demo_interfaces/srv/SetMode",
                ],
            )
            widget = next(entry for entry in json_backend["interfaces"] if entry["name"] == "Widget")
            self.assertEqual(widget["messages"][0]["fields"][0]["type"], "std_msgs/Header")
            self.assertEqual(report_json["dependency_resolution"]["mode"], "generate_package")
            self.assertEqual(report_json["dependency_resolution"]["closure_status"], "incomplete")
            source_closure = json.loads(out.joinpath("demo_interfaces_dependency_closure.json").read_text(encoding="utf-8"))
            self.assertEqual(source_closure["mode"], "generate_package_dependency_closure")
            self.assertIn("std_msgs/msg/Header", source_closure["unresolved"])
            source_migration = json.loads(out.joinpath("demo_interfaces_migration_manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(source_migration["recommended_mode"], "generate_package")
            self.assertFalse(source_migration["uses_ros2_cli"])
            self.assertFalse(source_migration["uses_ament_index_python"])
            self.assertEqual(source_migration["verification"]["entrypoint"], "scripts/verify_ros2_interface_import_live.sh")
            self.assertEqual(source_migration["verification"]["selected_lanes"], ["msg", "imported_srv", "json_scaffold", "perf_boundary"])
            self.assertIn("IBMW_JSON_SMOKE_IBMW_PREFIX", source_migration["verification"]["lanes"][2]["requires_ibmw_prefix_env"])
            self.assertEqual(source_migration["verification"]["lanes"][3]["guards_targets"], ["dds_native", "iceoryx"])
            self.assertEqual(source_migration["verification"]["optional_lanes"], ["route_b"])
            self.assertEqual(
                source_migration["target_backend_status"]["summary"],
                {"dds_native": "degraded", "dds_ros2": "supported", "iceoryx": "degraded", "json": "degraded"},
            )
            self.assertEqual(
                source_migration["backend_metadata_files"],
                [
                    "demo_interfaces_dds_native_backend_metadata.json",
                    "demo_interfaces_iceoryx_backend_metadata.json",
                    "demo_interfaces_json_backend_metadata.json",
                ],
            )
            self.assertEqual(
                source_migration["backend_generated_files"],
                [
                    "demo_interfaces_dds_native_lowering_plan.json",
                    "demo_interfaces_iceoryx_lowering_plan.json",
                    "demo_interfaces_json_types.hpp",
                    "demo_interfaces_json_runtime_scaffold.ibmw.yaml",
                ],
            )
            self.assertEqual(report_json["verification"], source_migration["verification"])
            runtime_project_yaml = out.joinpath("demo_interfaces_imported_srv_runtime.ibmw.yaml").read_text(encoding="utf-8")
            self.assertIn("name: demo_interfaces_imported_srv_runtime", runtime_project_yaml)
            self.assertIn("name: AddRpcServerModule", runtime_project_yaml)
            self.assertIn("name: AddRpcClientModule", runtime_project_yaml)
            self.assertIn("ros2_package: demo_interfaces", runtime_project_yaml)
            self.assertIn("ros2_service: Add", runtime_project_yaml)
            self.assertIn("ros2_service_name: /add", runtime_project_yaml)
            self.assertIn("ros2_service_name: /set_mode", runtime_project_yaml)
            self.assertIn("ros2_imported_srv_adapter: true", runtime_project_yaml)
            self.assertIn("name: set_mode_imported_srv_server_app", runtime_project_yaml)
            self.assertIn("name: set_mode_imported_srv_client_app", runtime_project_yaml)
            self.assertEqual(report_json["ibmw_wrapper"]["header"], "include/demo_interfaces/ibmw_interface_types.hpp")
            self.assertEqual(report_json["ibmw_wrapper"]["conversion_header"], "include/demo_interfaces/ibmw_ros2_conversion.hpp")
            self.assertEqual(report_json["ibmw_wrapper"]["conversion_mode"], "identity_rosidl_alias")
            self.assertEqual(report_json["ibmw_wrapper"]["rpc_bridge_skeleton"]["header"], "include/demo_interfaces/ibmw_rpc_bridge_skeleton.hpp")
            self.assertTrue(report_json["ibmw_wrapper"]["rpc_bridge_skeleton"]["generated"])
            self.assertFalse(report_json["ibmw_wrapper"]["rpc_bridge_skeleton"]["runtime_glue_generated"])
            self.assertEqual(report_json["ibmw_wrapper"]["rpc_bridge_runtime"]["header"], "include/demo_interfaces/ibmw_rpc_bridge_runtime.hpp")
            self.assertTrue(report_json["ibmw_wrapper"]["rpc_bridge_runtime"]["generated"])
            self.assertEqual(report_json["ibmw_wrapper"]["rpc_bridge_runtime"]["kind"], "typed_server_client_adapter_header")
            self.assertEqual(
                report_json["ibmw_wrapper"]["rpc_bridge_runtime"]["consumer_contract"],
                "requires_ibmw_api_and_rosidl_typesupport_cpp",
            )
            self.assertEqual(report_json["ibmw_wrapper"]["rpc_bridge_runtime"]["cxx_standard"], "cxx20")
            self.assertEqual(
                report_json["ibmw_wrapper"]["rpc_bridge_runtime"]["required_rosidl_targets"],
                ["demo_interfaces::demo_interfaces__rosidl_typesupport_cpp"],
            )
            self.assertFalse(report_json["ibmw_wrapper"]["rpc_bridge_runtime"]["runtime_module_generated"])
            self.assertFalse(report_json["ibmw_wrapper"]["runtime_module_generated"])
            self.assertEqual(report_json["ibmw_wrapper"]["services"][0]["request_alias"], "Add_Request")
            self.assertEqual(report_json["ibmw_wrapper"]["services"][0]["ros2_service_name"], "/add")
            self.assertEqual(report_json["ibmw_wrapper"]["services"][1]["request_alias"], "SetMode_Request")
            self.assertEqual(report_json["ibmw_wrapper"]["services"][1]["ros2_service_name"], "/set_mode")
            self.assertEqual(
                report_json["ibmw_wrapper"]["services"][0]["request_dds_type_name"],
                "demo_interfaces::srv::dds_::Add_Request_",
            )
            self.assertEqual(
                report_json["ibmw_wrapper"]["services"][0]["response_dds_type_name"],
                "demo_interfaces::srv::dds_::Add_Response_",
            )
            self.assertEqual(report_json["ibmw_wrapper"]["services"][0]["runtime_glue"], "not_generated")

    def test_verify_wrapper_checks_backend_metadata_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            src = root / "demo_interfaces"
            (src / "msg").mkdir(parents=True)
            src.joinpath("package.xml").write_text(
                "<package format='3'><name>demo_interfaces</name><version>0.1.0</version></package>\n",
                encoding="utf-8",
            )
            src.joinpath("msg", "Plain.msg").write_text("float64 x\nfloat64 y\n", encoding="utf-8")
            out = root / "out"

            emit_rosidl_artifact_package(
                str(src),
                str(out),
                targets=["dds_ros2", "dds_native", "iceoryx", "json"],
            )

            repo_root = ROOT.parents[2]
            result = subprocess.run(
                [
                    "bash",
                    str(repo_root / "scripts" / "verify_ros2_interface_import_live.sh"),
                    "--manifest",
                    str(out / "demo_interfaces_migration_manifest.json"),
                    str(root / "verify-work"),
                ],
                cwd=repo_root,
                env={**os.environ, "VERIFY_LANES": "backend_metadata"},
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("[PASS] backend metadata manifest consistency", result.stdout)

    def test_verify_wrapper_checks_perf_boundary_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            src = root / "demo_interfaces"
            (src / "msg").mkdir(parents=True)
            src.joinpath("package.xml").write_text(
                "<package format='3'><name>demo_interfaces</name><version>0.1.0</version></package>\n",
                encoding="utf-8",
            )
            src.joinpath("msg", "Plain.msg").write_text("float64 x\nfloat64 y\n", encoding="utf-8")
            out = root / "out"

            emit_rosidl_artifact_package(
                str(src),
                str(out),
                targets=["dds_ros2", "dds_native", "iceoryx", "json"],
            )

            repo_root = ROOT.parents[2]
            work_root = root / "verify-work"
            result = subprocess.run(
                [
                    "bash",
                    str(repo_root / "scripts" / "verify_ros2_interface_import_live.sh"),
                    "--manifest",
                    str(out / "demo_interfaces_migration_manifest.json"),
                    str(work_root),
                ],
                cwd=repo_root,
                env={**os.environ, "VERIFY_LANES": "perf_boundary"},
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )

            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("[PASS] perf boundary manifest guard", result.stdout)
            summary_path = work_root / "perf_boundary" / "perf_boundary_summary.json"
            self.assertTrue(summary_path.is_file())
            summary = json.loads(summary_path.read_text(encoding="utf-8"))
            self.assertEqual(summary["status"], "pass")
            self.assertTrue(summary["no_runtime_hot_path_changes"])
            self.assertFalse(summary["uses_rcl_in_ibmw_runtime"])
            self.assertEqual(summary["zero_copy_sized_interfaces"], 1)
            self.assertIn(
                "demo_interfaces_dds_native_backend_metadata.json",
                summary["checked_backend_metadata_files"],
            )
            self.assertIn(
                "demo_interfaces_iceoryx_backend_metadata.json",
                summary["checked_backend_metadata_files"],
            )
            self.assertEqual(
                summary["checked_lowering_plan_files"],
                ["demo_interfaces_dds_native_lowering_plan.json", "demo_interfaces_iceoryx_lowering_plan.json"],
            )
            iceoryx_backend = json.loads(out.joinpath("demo_interfaces_iceoryx_backend_metadata.json").read_text(encoding="utf-8"))
            self.assertEqual(iceoryx_backend["interfaces"][0]["capability"]["max_serialized_size_bytes"], 16)

    def test_verify_wrapper_guards_srv_action_summary_artifacts(self) -> None:
        repo_root = ROOT.parents[2]
        wrapper = repo_root / "scripts" / "verify_ros2_interface_import_live.sh"
        text = wrapper.read_text(encoding="utf-8")

        self.assertIn("verify_live_matrix_summary_artifact", text)
        self.assertIn("msg_live_matrix_summary.json", text)
        self.assertIn("imported_srv_live_matrix_summary.json", text)
        self.assertIn("reference_srv_live_matrix_summary.json", text)
        self.assertIn("action_runtime_matrix_summary.json", text)
        self.assertIn("generic_business_semantics_generated", text)
        self.assertIn("action_type", text)
        self.assertIn("action_name", text)
        self.assertIn("verify_json_scaffold_summary_artifacts", text)
        self.assertIn("json_scaffold_generated_shape_matrix_summary.json", text)
        self.assertIn("json_scaffold_reference_matrix_summary.json", text)
        self.assertIn("verify_route_b_target_artifact_audit", text)
        self.assertIn("route_b_target_artifact_audit.json", text)
        self.assertIn("http_selected_cases", text)
        self.assertIn("http_runtime_pass", text)
        self.assertIn("IBMW_VERIFY_LANE_TIMEOUT_SECONDS", text)
        self.assertIn("run_with_lane_timeout", text)

    def test_cli_import_artifact_only_json(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            src = root / "src_pkg"
            (src / "msg").mkdir(parents=True)
            src.joinpath("msg", "Ping.msg").write_text("uint32 seq\n", encoding="utf-8")
            out = root / "generated"

            result = subprocess.run(
                [
                    sys.executable,
                    str(IBMW_GEN),
                    "import",
                    "--ros2-src",
                    str(src),
                    "--artifact-only",
                    "--output-dir",
                    str(out),
                    "--package-name",
                    "custom_interfaces",
                    "--format",
                    "json",
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(result.stdout)
            self.assertEqual(data["package"], "custom_interfaces")
            self.assertEqual(data["files"], ["msg/Ping.msg"])
            self.assertEqual(
                data["rosidl_targets"]["generator_cpp"],
                "custom_interfaces::custom_interfaces__rosidl_generator_cpp",
            )
            self.assertEqual(data["ibmw_wrapper"]["messages"][0]["alias"], "Ping")
            self.assertFalse(data["ibmw_wrapper"]["rpc_bridge_skeleton"]["generated"])
            self.assertTrue(out.joinpath("CMakeLists.txt").is_file())
            self.assertEqual(result.stdout, json.dumps(data, sort_keys=True, indent=2) + "\n")

    def test_emit_rosidl_artifact_package_action_skeleton(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            src = root / "action_interfaces"
            (src / "action").mkdir(parents=True)
            src.joinpath("action", "DoThing.action").write_text(
                "int32 order\n---\nbool ok\n---\nint32 progress\n",
                encoding="utf-8",
            )
            out = root / "out"

            report = emit_rosidl_artifact_package(str(src), str(out), package_name="action_interfaces")

            traits = out.joinpath("include", "action_interfaces", "ibmw_action_traits.hpp").read_text(encoding="utf-8")
            binding = out.joinpath("include", "action_interfaces", "ibmw_action_binding_skeleton.hpp").read_text(encoding="utf-8")
            lifecycle = out.joinpath("include", "action_interfaces", "ibmw_action_lifecycle_runtime_skeleton.hpp").read_text(encoding="utf-8")
            handler = out.joinpath("include", "action_interfaces", "ibmw_action_handler_skeleton.hpp").read_text(encoding="utf-8")
            server_runtime = out.joinpath("include", "action_interfaces", "ibmw_action_server_runtime.hpp").read_text(encoding="utf-8")
            contract = out.joinpath("action_interfaces_action_runtime_contract.yaml").read_text(encoding="utf-8")
            report_json = json.loads(out.joinpath("ibmw_interface_report.json").read_text(encoding="utf-8"))
            self.assertEqual(report["files"], ["action/DoThing.action"])
            self.assertIn("include/action_interfaces/ibmw_action_traits.hpp", report["generated_files"])
            self.assertIn("include/action_interfaces/ibmw_ros2_conversion.hpp", report["generated_files"])
            self.assertIn("include/action_interfaces/ibmw_action_binding_skeleton.hpp", report["generated_files"])
            self.assertIn("include/action_interfaces/ibmw_action_lifecycle_runtime_skeleton.hpp", report["generated_files"])
            self.assertIn("include/action_interfaces/ibmw_action_handler_skeleton.hpp", report["generated_files"])
            self.assertIn("include/action_interfaces/ibmw_action_server_runtime.hpp", report["generated_files"])
            self.assertIn("action_interfaces_action_runtime_contract.yaml", report["generated_files"])
            self.assertIn("struct DoThingActionTraits", traits)
            conversion = out.joinpath("include", "action_interfaces", "ibmw_ros2_conversion.hpp").read_text(encoding="utf-8")
            self.assertIn("ToIbmwRos2Compatible", conversion)
            self.assertIn("CopyRos2Compatible", conversion)
            self.assertIn("ToIbmwDoThingGoal", conversion)
            self.assertIn("CopyDoThingFeedbackRos2Compatible", conversion)
            self.assertIn("using SendGoalService = Action::Impl::SendGoalService;", traits)
            self.assertIn("using SendGoalRequest = Action::Impl::SendGoalService::Request;", traits)
            self.assertIn("using GetResultService = Action::Impl::GetResultService;", traits)
            self.assertIn("using CancelGoalService = Action::Impl::CancelGoalService;", traits)
            self.assertIn("using CancelGoalResponse = Action::Impl::CancelGoalService::Response;", traits)
            self.assertIn("#include <action_interfaces/action/detail/do_thing__rosidl_typesupport_fastrtps_cpp.hpp>", traits)
            self.assertIn("struct Ros2FastTypeTraits<::action_interfaces::action::DoThing::Impl::SendGoalService::Request>", traits)
            self.assertIn("action_interfaces::action::dds_::DoThing_SendGoal_Request_", traits)
            self.assertIn("return ::action_interfaces::action::typesupport_fastrtps_cpp::cdr_serialize(msg, cdr);", traits)
            self.assertIn('kGoalType = "action_interfaces/action/DoThing_Goal"', traits)
            self.assertIn('kSendGoalServiceType = "action_interfaces/action/DoThing_SendGoal"', traits)
            self.assertIn('kCancelGoalServiceType = "action_msgs/srv/CancelGoal"', traits)
            self.assertIn('kStatusMessageType = "action_msgs/msg/GoalStatusArray"', traits)
            self.assertIn('kSendGoalService = "/do_thing/_action/send_goal"', traits)
            self.assertIn('kGetResultService = "/do_thing/_action/get_result"', traits)
            self.assertIn('kCancelGoalService = "/do_thing/_action/cancel_goal"', traits)
            self.assertIn('kFeedbackTopic = "/do_thing/_action/feedback"', traits)
            self.assertIn('kStatusTopic = "/do_thing/_action/status"', traits)
            self.assertIn('kRuntimeStatus = "compile_skeleton"', traits)
            self.assertIn("struct DoThingActionBindingSkeleton", binding)
            self.assertIn("kRuntimeGlueGenerated = false", binding)
            self.assertIn("enum class ActionLifecycleStage", lifecycle)
            self.assertIn("struct DoThingActionLifecycleRuntimeSkeleton", lifecycle)
            self.assertIn("kLifecycleSkeletonGenerated = true", lifecycle)
            self.assertIn("kLiveRuntimeClaimed = false", lifecycle)
            self.assertIn("EndpointForStage(ActionLifecycleStage stage)", lifecycle)
            self.assertIn("struct DoThingActionHandlerSkeleton", handler)
            self.assertIn("kHandlerSkeletonGenerated = true", handler)
            self.assertIn("kBusinessLogicGenerated = false", handler)
            self.assertIn("SendGoalOutcome OnSendGoal", handler)
            self.assertIn("GetResultOutcome OnGetResult", handler)
            self.assertIn("CancelGoalOutcome OnCancelGoal", handler)
            self.assertIn("FeedbackOutcome OnFeedback", handler)
            self.assertIn("StatusOutcome OnStatus", handler)
            self.assertIn("using DoThingActionServerNode", server_runtime)
            self.assertIn("class DoThingActionServerRuntime", server_runtime)
            self.assertIn("struct DoThingRejectingHook", server_runtime)
            self.assertIn("ValidateGoal(const GoalInput&) { return false; }", server_runtime)
            self.assertIn("using DoThingRejectingActionServerRuntime", server_runtime)
            self.assertIn("kActionServerBindingSpecialized<", server_runtime)
            self.assertIn("::ibmw::action::ActionServerNode<DoThingActionTraits", server_runtime)
            self.assertIn("kRclFreeRuntime = true", server_runtime)
            self.assertIn("kUsesRcl = false", server_runtime)
            self.assertIn("bool Start(uint32_t domain_id = 0U)", server_runtime)
            self.assertEqual(report_json["ibmw_wrapper"]["action_runtime"]["status"], "rcl_free_server_runtime_template")
            self.assertEqual(
                report_json["ibmw_wrapper"]["action_runtime"]["lifecycle_runtime_skeleton_header"],
                "include/action_interfaces/ibmw_action_lifecycle_runtime_skeleton.hpp",
            )
            self.assertEqual(report_json["ibmw_wrapper"]["action_runtime"]["kind"], "action_server_node_template_header")
            self.assertEqual(
                report_json["ibmw_wrapper"]["action_runtime"]["handler_skeleton_header"],
                "include/action_interfaces/ibmw_action_handler_skeleton.hpp",
            )
            self.assertEqual(
                report_json["ibmw_wrapper"]["action_runtime"]["server_runtime_header"],
                "include/action_interfaces/ibmw_action_server_runtime.hpp",
            )
            self.assertTrue(report_json["ibmw_wrapper"]["action_runtime"]["generic_runtime_claimed"])
            self.assertFalse(report_json["ibmw_wrapper"]["action_runtime"]["live_runtime_claimed"])
            self.assertEqual(report_json["ibmw_wrapper"]["actions"][0]["runtime_glue"], "rcl_free_server_runtime_template")
            self.assertEqual(report_json["ibmw_wrapper"]["actions"][0]["rejecting_runtime_alias"], "DoThingRejectingActionServerRuntime")
            self.assertEqual(report_json["ibmw_wrapper"]["actions"][0]["send_goal_service"], "/do_thing/_action/send_goal")
            self.assertEqual(report_json["ibmw_wrapper"]["actions"][0]["feedback_topic"], "/do_thing/_action/feedback")
            self.assertEqual(
                report_json["ibmw_wrapper"]["actions"][0]["hidden_rosidl_types"]["send_goal_service"],
                "action_interfaces/action/DoThing_SendGoal",
            )
            self.assertEqual(
                report_json["ibmw_wrapper"]["actions"][0]["hidden_rosidl_types"]["status_message"],
                "action_msgs/msg/GoalStatusArray",
            )
            self.assertEqual(
                report_json["ibmw_wrapper"]["action_runtime"]["contract_manifest"],
                "action_interfaces_action_runtime_contract.yaml",
            )
            self.assertIn("kind: ros2_action_rcl_free_server_runtime_template_contract", contract)
            self.assertIn("lifecycle_runtime_skeleton_header: include/action_interfaces/ibmw_action_lifecycle_runtime_skeleton.hpp", contract)
            self.assertIn("handler_skeleton_header: include/action_interfaces/ibmw_action_handler_skeleton.hpp", contract)
            self.assertIn("server_runtime_header: include/action_interfaces/ibmw_action_server_runtime.hpp", contract)
            self.assertIn("generic_runtime_claimed: true", contract)
            self.assertIn("live_runtime_claimed: false", contract)
            self.assertIn("generated_rejecting_runtime_alias: DoThingRejectingActionServerRuntime", contract)
            self.assertIn("send_goal_service: /do_thing/_action/send_goal", contract)
            self.assertIn("send_goal_service: action_interfaces/action/DoThing_SendGoal", contract)

    def test_emit_reference_existing_interface_report(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            pkg = prefix / "share" / "existing_interfaces"
            (pkg / "msg").mkdir(parents=True)
            (pkg / "srv").mkdir(parents=True)
            pkg.joinpath("package.xml").write_text(
                "<package format='3'><name>existing_interfaces</name><version>0.0.0</version></package>\n",
                encoding="utf-8",
            )
            pkg.joinpath("msg", "Status.msg").write_text("std_msgs/Header header\nuint32 code\n", encoding="utf-8")
            pkg.joinpath("srv", "SetFlag.srv").write_text("bool enable\n---\nbool accepted\n", encoding="utf-8")
            out = root / "out"

            report = emit_reference_existing_interface_report(str(prefix), "existing_interfaces", str(out))

            self.assertEqual(report["mode"], "reference_existing")
            self.assertEqual(report["input_mode"], "installed_package")
            self.assertEqual(report["package"], "existing_interfaces")
            self.assertEqual(report["files"], ["msg/Status.msg", "srv/SetFlag.srv"])
            self.assertEqual(
                report["available_types"],
                [
                    {
                        "name": "Status",
                        "kind": "msg",
                        "selector": "msg/Status",
                        "canonical": "existing_interfaces/msg/Status",
                        "relative_path": "msg/Status.msg",
                    },
                    {
                        "name": "SetFlag",
                        "kind": "srv",
                        "selector": "srv/SetFlag",
                        "canonical": "existing_interfaces/srv/SetFlag",
                        "relative_path": "srv/SetFlag.srv",
                    },
                ],
            )
            self.assertEqual(report["external_dependencies"], ["std_msgs"])
            self.assertEqual(report["boundary"]["discovery_source"], "share_tree_scan")
            self.assertEqual(report["boundary"]["host_tools_prefix"], str(prefix))
            self.assertIsNone(report["boundary"]["target_runtime_prefix"])
            self.assertFalse(report["boundary"]["host_prefix_used_for_target_runtime"])
            self.assertTrue(report["cross_compile_safe"])
            self.assertIn("share tree scanning", report["cross_compile_reasons"][0])
            self.assertEqual(report["prefix_provenance"]["package_version"], "0.0.0")
            self.assertEqual(report["prefix_provenance"]["package_share"], str(pkg))
            self.assertEqual(report["external_dependencies_resolved"][0]["package"], "std_msgs")
            self.assertFalse(report["external_dependencies_resolved"][0]["found_in_ros2_prefix"])
            self.assertTrue(report["rcl_free_runtime"])
            self.assertEqual(report["dependency_resolution"]["mode"], "reference_existing")
            self.assertIn("rclcpp", report["dependency_resolution"]["forbidden_runtime_dependencies"])
            self.assertIn("ament_index_python", report["dependency_resolution"]["forbidden_runtime_dependencies"])
            self.assertEqual(
                report["generated_files"],
                [
                    "ibmw_interface_report.json",
                    "include/existing_interfaces/ibmw_interface_types.hpp",
                    "include/existing_interfaces/ibmw_ros2_conversion.hpp",
                    "include/existing_interfaces/ibmw_rpc_bridge_skeleton.hpp",
                    "include/existing_interfaces/ibmw_rpc_bridge_runtime.hpp",
                    "existing_interfaces_reference_existing_srv_runtime.ibmw.yaml",
                    "existing_interfaces_dependency_closure.json",
                    "existing_interfaces_migration_manifest.json",
                ],
            )
            self.assertFalse(out.joinpath("CMakeLists.txt").exists())
            self.assertTrue(out.joinpath("ibmw_interface_report.json").is_file())
            self.assertTrue(out.joinpath("include", "existing_interfaces", "ibmw_ros2_conversion.hpp").is_file())
            self.assertTrue(out.joinpath("include", "existing_interfaces", "ibmw_rpc_bridge_runtime.hpp").is_file())
            runtime_yaml = out.joinpath("existing_interfaces_reference_existing_srv_runtime.ibmw.yaml").read_text(encoding="utf-8")
            self.assertIn("SetFlagRpcServerModule", runtime_yaml)
            self.assertIn("ros2_package: existing_interfaces", runtime_yaml)
            self.assertIn("ros2_service_name: /set_flag", runtime_yaml)
            self.assertIn(f"ros2_imported_srv_adapter_include_dir: {out / 'include'}", runtime_yaml)
            self.assertEqual(
                report["rosidl_targets"]["typesupport_fastrtps_cpp"],
                "existing_interfaces::existing_interfaces__rosidl_typesupport_fastrtps_cpp",
            )
            self.assertEqual(report["ibmw_wrapper"]["services"][0]["ros2_service_name"], "/set_flag")
            self.assertEqual(report["ibmw_wrapper"]["conversion_mode"], "identity_rosidl_alias")
            self.assertEqual(report["dependency_resolution"]["closure_manifest"], "existing_interfaces_dependency_closure.json")
            self.assertEqual(report["dependency_closure"]["status"], "incomplete")
            self.assertEqual(report["dependency_resolution"]["migration_manifest"], "existing_interfaces_migration_manifest.json")
            migration_manifest = json.loads(out.joinpath("existing_interfaces_migration_manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(migration_manifest["recommended_mode"], "reference_existing")
            self.assertFalse(migration_manifest["build_behavior_changed"])
            self.assertEqual(migration_manifest["verification"]["selected_lanes"], ["msg", "reference_srv"])
            self.assertFalse(migration_manifest["verification"]["uses_ros2_cli_as_import_prerequisite"])
            self.assertFalse(migration_manifest["verification"]["uses_rcl_in_ibmw_runtime"])
            self.assertEqual(migration_manifest["target_backend_status"]["summary"], {"dds_ros2": "supported"})
            self.assertEqual(report["verification"], migration_manifest["verification"])

    def test_reference_existing_json_backend_metadata_is_degraded(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            pkg = prefix / "share" / "existing_interfaces"
            (pkg / "msg").mkdir(parents=True)
            pkg.joinpath("package.xml").write_text(
                "<package format='3'><name>existing_interfaces</name><version>0.1.0</version></package>\n",
                encoding="utf-8",
            )
            pkg.joinpath("msg", "Flag.msg").write_text("bool enabled\n", encoding="utf-8")
            out = root / "out"

            report = emit_reference_existing_interface_report(
                str(prefix),
                "existing_interfaces",
                str(out),
                targets=["dds_ros2", "json"],
            )

            self.assertEqual(report["target_backend_status"]["summary"], {"dds_ros2": "supported", "json": "degraded"})
            self.assertEqual(report["backend_metadata_files"], ["existing_interfaces_json_backend_metadata.json"])
            self.assertEqual(
                report["backend_generated_files"],
                ["existing_interfaces_json_types.hpp", "existing_interfaces_json_runtime_scaffold.ibmw.yaml"],
            )
            self.assertIn("existing_interfaces_json_backend_metadata.json", report["generated_files"])
            self.assertIn("existing_interfaces_json_types.hpp", report["generated_files"])
            self.assertIn("existing_interfaces_json_runtime_scaffold.ibmw.yaml", report["generated_files"])
            migration_manifest = json.loads(out.joinpath("existing_interfaces_migration_manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(migration_manifest["target_backend_status"]["summary"]["json"], "degraded")
            self.assertEqual(migration_manifest["verification"]["selected_lanes"], ["msg", "json_scaffold"])
            self.assertEqual(migration_manifest["backend_metadata_files"], ["existing_interfaces_json_backend_metadata.json"])
            self.assertEqual(
                migration_manifest["backend_generated_files"],
                ["existing_interfaces_json_types.hpp", "existing_interfaces_json_runtime_scaffold.ibmw.yaml"],
            )
            json_backend = json.loads(out.joinpath("existing_interfaces_json_backend_metadata.json").read_text(encoding="utf-8"))
            self.assertEqual(json_backend["mode"], "reference_existing")
            self.assertTrue(json_backend["types_generated"])
            self.assertEqual(json_backend["types_header"], "existing_interfaces_json_types.hpp")
            self.assertEqual(json_backend["interfaces"][0]["canonical"], "existing_interfaces/msg/Flag")
            self.assertEqual(json_backend["interfaces"][0]["messages"][0]["fields"][0]["base_type"], "bool")
            json_types_header = out.joinpath("existing_interfaces_json_types.hpp").read_text(encoding="utf-8")
            self.assertIn("struct Flag", json_types_header)
            self.assertIn('static constexpr const char* kTypeName = "existing_interfaces/msg/Flag";', json_types_header)
            self.assertIn("bool enabled{};", json_types_header)
            json_scaffold = out.joinpath("existing_interfaces_json_runtime_scaffold.ibmw.yaml").read_text(encoding="utf-8")
            self.assertIn("message_type: ibmw::ros2_interfaces::json::existing_interfaces::Flag", json_scaffold)
            self.assertIn("include_contract: ros2_import_json_types_header", json_scaffold)
            self.assertIn(f"ros2_import_json_types_include_dir: {out}", json_scaffold)

    def test_generate_package_json_backend_supports_same_package_nested_messages(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            src = root / "nested_interfaces"
            (src / "msg").mkdir(parents=True)
            src.joinpath("package.xml").write_text(
                "<package format='3'><name>nested_interfaces</name><version>0.1.0</version></package>\n",
                encoding="utf-8",
            )
            src.joinpath("msg", "Detail.msg").write_text("float64 value\nint32[2] bins\n", encoding="utf-8")
            src.joinpath("msg", "Nested.msg").write_text("Detail detail\nbool ok\n", encoding="utf-8")
            out = root / "out"

            report = emit_rosidl_artifact_package(str(src), str(out), targets=["json"])

            self.assertEqual(
                report["backend_generated_files"],
                ["nested_interfaces_json_types.hpp", "nested_interfaces_json_runtime_scaffold.ibmw.yaml"],
            )
            json_types_header = out.joinpath("nested_interfaces_json_types.hpp").read_text(encoding="utf-8")
            self.assertLess(json_types_header.index("struct Detail"), json_types_header.index("struct Nested"))
            self.assertIn("std::array<int32_t, 2> bins{};", json_types_header)
            self.assertIn("Detail detail{};", json_types_header)
            self.assertIn("NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Nested, detail, ok)", json_types_header)
            json_scaffold = out.joinpath("nested_interfaces_json_runtime_scaffold.ibmw.yaml").read_text(encoding="utf-8")
            self.assertIn("name: DetailJsonPublisherModule", json_scaffold)
            self.assertIn("name: NestedJsonPublisherModule", json_scaffold)
            manifest = json.loads(out.joinpath("nested_interfaces_migration_manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["verification"]["selected_lanes"], ["msg", "json_scaffold"])

    def test_generate_package_json_backend_supports_bounded_string_and_sequence(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            src = root / "bounded_interfaces"
            (src / "msg").mkdir(parents=True)
            src.joinpath("package.xml").write_text(
                "<package format='3'><name>bounded_interfaces</name><version>0.1.0</version></package>\n",
                encoding="utf-8",
            )
            src.joinpath("msg", "Bounded.msg").write_text(
                "string<=16 label\nuint8[<=8] payload\nfloat64[<=2] samples\n",
                encoding="utf-8",
            )
            out = root / "out"

            report = emit_rosidl_artifact_package(str(src), str(out), targets=["json"])

            self.assertEqual(
                report["backend_generated_files"],
                ["bounded_interfaces_json_types.hpp", "bounded_interfaces_json_runtime_scaffold.ibmw.yaml"],
            )
            json_types_header = out.joinpath("bounded_interfaces_json_types.hpp").read_text(encoding="utf-8")
            self.assertIn("#include <string>", json_types_header)
            self.assertIn("#include <vector>", json_types_header)
            self.assertIn("std::string label{};", json_types_header)
            self.assertIn("std::vector<uint8_t> payload{};", json_types_header)
            self.assertIn("std::vector<double> samples{};", json_types_header)
            self.assertIn("NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Bounded, label, payload, samples)", json_types_header)
            json_scaffold = out.joinpath("bounded_interfaces_json_runtime_scaffold.ibmw.yaml").read_text(encoding="utf-8")
            self.assertIn("name: BoundedJsonPublisherModule", json_scaffold)
            manifest = json.loads(out.joinpath("bounded_interfaces_migration_manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["verification"]["selected_lanes"], ["msg", "json_scaffold"])

    def test_generate_package_json_backend_supports_unbounded_sequence(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            src = root / "dynamic_interfaces"
            (src / "msg").mkdir(parents=True)
            src.joinpath("package.xml").write_text(
                "<package format='3'><name>dynamic_interfaces</name><version>0.1.0</version></package>\n",
                encoding="utf-8",
            )
            src.joinpath("msg", "Dynamic.msg").write_text(
                "string name\nuint8[] payload\nfloat64[] samples\n",
                encoding="utf-8",
            )
            out = root / "out"

            report = emit_rosidl_artifact_package(str(src), str(out), targets=["json"])

            self.assertEqual(
                report["backend_generated_files"],
                ["dynamic_interfaces_json_types.hpp", "dynamic_interfaces_json_runtime_scaffold.ibmw.yaml"],
            )
            json_types_header = out.joinpath("dynamic_interfaces_json_types.hpp").read_text(encoding="utf-8")
            self.assertIn("#include <string>", json_types_header)
            self.assertIn("#include <vector>", json_types_header)
            self.assertIn("std::string name{};", json_types_header)
            self.assertIn("std::vector<uint8_t> payload{};", json_types_header)
            self.assertIn("std::vector<double> samples{};", json_types_header)
            self.assertIn("NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Dynamic, name, payload, samples)", json_types_header)
            json_scaffold = out.joinpath("dynamic_interfaces_json_runtime_scaffold.ibmw.yaml").read_text(encoding="utf-8")
            self.assertIn("name: DynamicJsonPublisherModule", json_scaffold)
            manifest = json.loads(out.joinpath("dynamic_interfaces_migration_manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["verification"]["selected_lanes"], ["msg", "json_scaffold"])

    def test_generate_package_json_backend_supports_external_nested_messages(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            builtin = root / "builtin_interfaces"
            std_msgs = root / "std_msgs"
            robot = root / "robot_interfaces"
            for pkg in (builtin, std_msgs, robot):
                (pkg / "msg").mkdir(parents=True)
                pkg.joinpath("package.xml").write_text(
                    f"<package format='3'><name>{pkg.name}</name><version>0.1.0</version></package>\n",
                    encoding="utf-8",
                )
            builtin.joinpath("msg", "Time.msg").write_text("int32 sec\nuint32 nanosec\n", encoding="utf-8")
            std_msgs.joinpath("msg", "Header.msg").write_text(
                "builtin_interfaces/Time stamp\nstring frame_id\n",
                encoding="utf-8",
            )
            robot.joinpath("msg", "Stamped.msg").write_text("std_msgs/Header header\nfloat64 value\n", encoding="utf-8")
            out = root / "out"

            report = emit_rosidl_artifact_package(str(robot), str(out), targets=["json"])

            self.assertEqual(
                report["backend_generated_files"],
                ["robot_interfaces_json_types.hpp", "robot_interfaces_json_runtime_scaffold.ibmw.yaml"],
            )
            json_types_header = out.joinpath("robot_interfaces_json_types.hpp").read_text(encoding="utf-8")
            self.assertLess(json_types_header.index("namespace builtin_interfaces"), json_types_header.index("namespace std_msgs"))
            self.assertLess(json_types_header.index("namespace std_msgs"), json_types_header.index("namespace robot_interfaces"))
            self.assertIn("static constexpr const char* kTypeName = \"builtin_interfaces/msg/Time\";", json_types_header)
            self.assertIn("static constexpr const char* kTypeName = \"std_msgs/msg/Header\";", json_types_header)
            self.assertIn("static constexpr const char* kTypeName = \"robot_interfaces/msg/Stamped\";", json_types_header)
            self.assertIn("::ibmw::ros2_interfaces::json::builtin_interfaces::Time stamp{};", json_types_header)
            self.assertIn("::ibmw::ros2_interfaces::json::std_msgs::Header header{};", json_types_header)
            json_scaffold = out.joinpath("robot_interfaces_json_runtime_scaffold.ibmw.yaml").read_text(encoding="utf-8")
            self.assertIn("name: StampedJsonPublisherModule", json_scaffold)
            self.assertNotIn("name: HeaderJsonPublisherModule", json_scaffold)
            manifest = json.loads(out.joinpath("robot_interfaces_migration_manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(manifest["verification"]["selected_lanes"], ["msg", "json_scaffold"])

    def test_reference_existing_json_backend_supports_external_nested_messages(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "prefix"
            builtin = prefix / "share" / "builtin_interfaces"
            std_msgs = prefix / "share" / "std_msgs"
            for pkg in (builtin, std_msgs):
                (pkg / "msg").mkdir(parents=True)
                pkg.joinpath("package.xml").write_text(
                    f"<package format='3'><name>{pkg.name}</name><version>0.1.0</version></package>\n",
                    encoding="utf-8",
                )
            builtin.joinpath("msg", "Time.msg").write_text("int32 sec\nuint32 nanosec\n", encoding="utf-8")
            std_msgs.joinpath("msg", "Header.msg").write_text(
                "builtin_interfaces/Time stamp\nstring frame_id\n",
                encoding="utf-8",
            )
            out = root / "out"

            report = emit_reference_existing_interface_report(
                str(prefix),
                "std_msgs",
                str(out),
                targets=["json"],
                selected_types=["msg/Header"],
            )

            self.assertEqual(
                report["backend_generated_files"],
                ["std_msgs_json_types.hpp", "std_msgs_json_runtime_scaffold.ibmw.yaml"],
            )
            json_types_header = out.joinpath("std_msgs_json_types.hpp").read_text(encoding="utf-8")
            self.assertLess(json_types_header.index("namespace builtin_interfaces"), json_types_header.index("namespace std_msgs"))
            self.assertIn("static constexpr const char* kTypeName = \"builtin_interfaces/msg/Time\";", json_types_header)
            self.assertIn("static constexpr const char* kTypeName = \"std_msgs/msg/Header\";", json_types_header)
            self.assertIn("::ibmw::ros2_interfaces::json::builtin_interfaces::Time stamp{};", json_types_header)
            json_scaffold = out.joinpath("std_msgs_json_runtime_scaffold.ibmw.yaml").read_text(encoding="utf-8")
            self.assertIn("name: HeaderJsonPublisherModule", json_scaffold)
            self.assertNotIn("name: TimeJsonPublisherModule", json_scaffold)
            migration_manifest = json.loads(out.joinpath("std_msgs_migration_manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(migration_manifest["verification"]["selected_lanes"], ["msg", "json_scaffold"])
            self.assertEqual(migration_manifest["dependency_closure_status"], "complete")

    def test_reference_existing_dds_native_backend_metadata_is_degraded(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            pkg = prefix / "share" / "existing_interfaces"
            (pkg / "msg").mkdir(parents=True)
            pkg.joinpath("package.xml").write_text(
                "<package format='3'><name>existing_interfaces</name><version>0.1.0</version></package>\n",
                encoding="utf-8",
            )
            pkg.joinpath("msg", "Plain.msg").write_text("float64 x\nfloat64 y\n", encoding="utf-8")
            pkg.joinpath("msg", "Dynamic.msg").write_text("string label\n", encoding="utf-8")
            out = root / "out"

            report = emit_reference_existing_interface_report(
                str(prefix),
                "existing_interfaces",
                str(out),
                targets=["dds_ros2", "dds_native"],
            )

            self.assertEqual(report["target_backend_status"]["summary"], {"dds_native": "degraded", "dds_ros2": "supported"})
            self.assertEqual(report["backend_metadata_files"], ["existing_interfaces_dds_native_backend_metadata.json"])
            self.assertIn("existing_interfaces_dds_native_backend_metadata.json", report["generated_files"])
            migration_manifest = json.loads(out.joinpath("existing_interfaces_migration_manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(migration_manifest["target_backend_status"]["summary"]["dds_native"], "degraded")
            self.assertEqual(migration_manifest["verification"]["selected_lanes"], ["msg", "perf_boundary"])
            self.assertEqual(migration_manifest["verification"]["lanes"][1]["guards_targets"], ["dds_native"])
            self.assertEqual(migration_manifest["backend_metadata_files"], ["existing_interfaces_dds_native_backend_metadata.json"])
            self.assertEqual(migration_manifest["backend_generated_files"], ["existing_interfaces_dds_native_lowering_plan.json"])
            dds_native_backend = json.loads(out.joinpath("existing_interfaces_dds_native_backend_metadata.json").read_text(encoding="utf-8"))
            self.assertEqual(dds_native_backend["mode"], "reference_existing")
            self.assertEqual(dds_native_backend["target"], "dds_native")
            self.assertIn("existing_interfaces/msg/Plain", dds_native_backend["eligible_interfaces"])
            self.assertIn("existing_interfaces/msg/Dynamic", dds_native_backend["degraded_interfaces"])
            dynamic = next(entry for entry in dds_native_backend["interfaces"] if entry["name"] == "Dynamic")
            self.assertEqual(dynamic["capability"]["transports"]["dds_native"], "warn")
            dds_native_plan = json.loads(out.joinpath("existing_interfaces_dds_native_lowering_plan.json").read_text(encoding="utf-8"))
            self.assertEqual(dds_native_plan["totals"], {"eligible": 1, "degraded": 1})
            self.assertEqual(dds_native_plan["eligible_interfaces"][0]["max_serialized_size_bytes"], 16)

    def test_reference_existing_iceoryx_backend_metadata_records_eligibility(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            pkg = prefix / "share" / "existing_interfaces"
            (pkg / "msg").mkdir(parents=True)
            pkg.joinpath("package.xml").write_text(
                "<package format='3'><name>existing_interfaces</name><version>0.1.0</version></package>\n",
                encoding="utf-8",
            )
            pkg.joinpath("msg", "Plain.msg").write_text("float64 x\nfloat64 y\n", encoding="utf-8")
            pkg.joinpath("msg", "Dynamic.msg").write_text("string label\n", encoding="utf-8")
            out = root / "out"

            report = emit_reference_existing_interface_report(
                str(prefix),
                "existing_interfaces",
                str(out),
                targets=["dds_ros2", "iceoryx"],
            )

            self.assertEqual(report["target_backend_status"]["summary"], {"dds_ros2": "supported", "iceoryx": "degraded"})
            self.assertEqual(report["backend_metadata_files"], ["existing_interfaces_iceoryx_backend_metadata.json"])
            status_by_target = {entry["target"]: entry for entry in report["target_backend_status"]["entries"]}
            self.assertEqual(status_by_target["iceoryx"]["runtime_boundary"], "metadata_only_no_iceoryx_lowering")
            migration_manifest = json.loads(out.joinpath("existing_interfaces_migration_manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(migration_manifest["target_backend_status"]["summary"]["iceoryx"], "degraded")
            self.assertEqual(migration_manifest["verification"]["selected_lanes"], ["msg", "perf_boundary"])
            self.assertEqual(migration_manifest["verification"]["lanes"][1]["guards_targets"], ["iceoryx"])
            self.assertEqual(migration_manifest["backend_generated_files"], ["existing_interfaces_iceoryx_lowering_plan.json"])
            iceoryx_backend = json.loads(out.joinpath("existing_interfaces_iceoryx_backend_metadata.json").read_text(encoding="utf-8"))
            self.assertEqual(iceoryx_backend["mode"], "reference_existing")
            self.assertEqual(iceoryx_backend["target"], "iceoryx")
            self.assertIn("existing_interfaces/msg/Plain", iceoryx_backend["eligible_interfaces"])
            self.assertIn("existing_interfaces/msg/Dynamic", iceoryx_backend["blocked_interfaces"])
            dynamic = next(entry for entry in iceoryx_backend["interfaces"] if entry["name"] == "Dynamic")
            self.assertEqual(dynamic["capability"]["zero_copy"]["iceoryx"], "no")
            iceoryx_plan = json.loads(out.joinpath("existing_interfaces_iceoryx_lowering_plan.json").read_text(encoding="utf-8"))
            self.assertEqual(iceoryx_plan["totals"], {"eligible": 1, "blocked": 1})
            self.assertEqual(iceoryx_plan["eligible_interfaces"][0]["max_serialized_size_bytes"], 16)

    def test_reference_existing_action_skeleton_report(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            pkg = prefix / "share" / "action_interfaces"
            (pkg / "action").mkdir(parents=True)
            pkg.joinpath("package.xml").write_text(
                "<package format='3'><name>action_interfaces</name><version>0.1.0</version></package>\n",
                encoding="utf-8",
            )
            pkg.joinpath("action", "DoThing.action").write_text(
                "int32 order\n---\nbool ok\n---\nint32 progress\n",
                encoding="utf-8",
            )
            out = root / "out"

            report = emit_reference_existing_interface_report(str(prefix), "action_interfaces", str(out))

            self.assertEqual(report["files"], ["action/DoThing.action"])
            self.assertEqual(report["ibmw_wrapper"]["actions"][0]["traits_alias"], "DoThingActionTraits")
            self.assertEqual(report["ibmw_wrapper"]["actions"][0]["default_ros2_action_name"], "/do_thing")
            self.assertEqual(report["ibmw_wrapper"]["actions"][0]["status_topic"], "/do_thing/_action/status")
            self.assertEqual(
                report["ibmw_wrapper"]["actions"][0]["hidden_rosidl_types"]["feedback_message"],
                "action_interfaces/action/DoThing_FeedbackMessage",
            )
            self.assertEqual(report["ibmw_wrapper"]["action_runtime"]["status"], "rcl_free_server_runtime_template")
            self.assertTrue(report["ibmw_wrapper"]["action_runtime"]["generic_runtime_claimed"])
            self.assertFalse(report["ibmw_wrapper"]["action_runtime"]["live_runtime_claimed"])
            self.assertFalse(report["ibmw_wrapper"]["action_runtime"]["runtime_module_generated"])
            self.assertIn("include/action_interfaces/ibmw_interface_types.hpp", report["generated_files"])
            self.assertIn("include/action_interfaces/ibmw_ros2_conversion.hpp", report["generated_files"])
            self.assertIn("include/action_interfaces/ibmw_action_traits.hpp", report["generated_files"])
            self.assertIn("include/action_interfaces/ibmw_action_binding_skeleton.hpp", report["generated_files"])
            self.assertIn("include/action_interfaces/ibmw_action_lifecycle_runtime_skeleton.hpp", report["generated_files"])
            self.assertIn("include/action_interfaces/ibmw_action_handler_skeleton.hpp", report["generated_files"])
            self.assertIn("include/action_interfaces/ibmw_action_server_runtime.hpp", report["generated_files"])
            self.assertIn("action_interfaces_action_runtime_contract.yaml", report["generated_files"])
            self.assertIn("action_interfaces_dependency_closure.json", report["generated_files"])
            self.assertIn("action_interfaces_migration_manifest.json", report["generated_files"])
            self.assertFalse(out.joinpath("CMakeLists.txt").exists())
            self.assertTrue(out.joinpath("include", "action_interfaces", "ibmw_interface_types.hpp").is_file())
            self.assertTrue(out.joinpath("include", "action_interfaces", "ibmw_ros2_conversion.hpp").is_file())
            self.assertTrue(out.joinpath("include", "action_interfaces", "ibmw_action_traits.hpp").is_file())
            self.assertTrue(out.joinpath("include", "action_interfaces", "ibmw_action_lifecycle_runtime_skeleton.hpp").is_file())
            self.assertTrue(out.joinpath("include", "action_interfaces", "ibmw_action_handler_skeleton.hpp").is_file())
            self.assertTrue(out.joinpath("include", "action_interfaces", "ibmw_action_server_runtime.hpp").is_file())
            self.assertTrue(out.joinpath("action_interfaces_action_runtime_contract.yaml").is_file())
            self.assertTrue(out.joinpath("action_interfaces_dependency_closure.json").is_file())
            self.assertTrue(out.joinpath("action_interfaces_migration_manifest.json").is_file())
            persisted = json.loads(out.joinpath("ibmw_interface_report.json").read_text(encoding="utf-8"))
            self.assertEqual(persisted["ibmw_wrapper"]["action_runtime"]["consumer_contract"], "requires_ibmw_action_runtime_and_rosidl_typesupport_cpp")
            self.assertEqual(persisted["ibmw_wrapper"]["action_runtime"]["contract_manifest"], "action_interfaces_action_runtime_contract.yaml")

    def test_reference_existing_dependency_closure_manifest_resolves_transitive_msgs(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            robot_pkg = prefix / "share" / "robot_interfaces"
            geometry_pkg = prefix / "share" / "geometry_msgs"
            std_pkg = prefix / "share" / "std_msgs"
            (robot_pkg / "msg").mkdir(parents=True)
            (geometry_pkg / "msg").mkdir(parents=True)
            (std_pkg / "msg").mkdir(parents=True)
            robot_pkg.joinpath("package.xml").write_text("<package format='3'><name>robot_interfaces</name><version>0.1.0</version></package>\n", encoding="utf-8")
            geometry_pkg.joinpath("package.xml").write_text("<package format='3'><name>geometry_msgs</name><version>0.1.0</version></package>\n", encoding="utf-8")
            std_pkg.joinpath("package.xml").write_text("<package format='3'><name>std_msgs</name><version>0.1.0</version></package>\n", encoding="utf-8")
            robot_pkg.joinpath("msg", "StampedPose.msg").write_text(
                "std_msgs/Header header\ngeometry_msgs/Pose pose\n",
                encoding="utf-8",
            )
            geometry_pkg.joinpath("msg", "Pose.msg").write_text("geometry_msgs/Point position\n", encoding="utf-8")
            geometry_pkg.joinpath("msg", "Point.msg").write_text("float64 x\nfloat64 y\nfloat64 z\n", encoding="utf-8")
            std_pkg.joinpath("msg", "Header.msg").write_text("builtin_interfaces/Time stamp\nstring frame_id\n", encoding="utf-8")
            out = root / "out"

            report = emit_reference_existing_interface_report(
                str(prefix),
                "robot_interfaces",
                str(out),
                selected_types=["msg/StampedPose"],
            )

            manifest = json.loads(out.joinpath("robot_interfaces_dependency_closure.json").read_text(encoding="utf-8"))
            canonicals = [entry["canonical"] for entry in manifest["dependencies"]]
            self.assertIn("geometry_msgs/msg/Pose", canonicals)
            self.assertIn("geometry_msgs/msg/Point", canonicals)
            self.assertIn("std_msgs/msg/Header", canonicals)
            self.assertIn("builtin_interfaces/msg/Time", manifest["unresolved"])
            self.assertEqual(manifest["status"], "incomplete")
            self.assertEqual(report["dependency_closure"], manifest)
            self.assertEqual(report["dependency_resolution"]["closure_status"], "incomplete")
            self.assertEqual(report["dependency_closure"]["external_packages"], ["builtin_interfaces", "geometry_msgs", "std_msgs"])
            migration_manifest = json.loads(out.joinpath("robot_interfaces_migration_manifest.json").read_text(encoding="utf-8"))
            self.assertEqual(migration_manifest["dependency_closure_manifest"], "robot_interfaces_dependency_closure.json")
            self.assertEqual(migration_manifest["dependency_count"], len(manifest["dependencies"]))
            self.assertEqual(migration_manifest["unresolved_dependencies"], ["builtin_interfaces/msg/Time"])
            self.assertEqual(migration_manifest["selected"][0]["selector"], "msg/StampedPose")
            self.assertFalse(migration_manifest["uses_ros2_cli"])

    def test_reference_existing_records_host_target_boundary(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            host = root / "host_ros"
            target = root / "target_sysroot" / "opt" / "ros"
            pkg = prefix / "share" / "existing_interfaces"
            (pkg / "srv").mkdir(parents=True)
            pkg.joinpath("package.xml").write_text(
                "<package format='3'><name>existing_interfaces</name><version>1.2.3</version></package>\n",
                encoding="utf-8",
            )
            pkg.joinpath("srv", "SetFlag.srv").write_text("bool enable\n---\nbool accepted\n", encoding="utf-8")

            report = emit_reference_existing_interface_report(
                str(prefix),
                "existing_interfaces",
                str(root / "out"),
                host_tools_prefix=str(host),
                target_runtime_prefix=str(target),
            )

            self.assertEqual(report["boundary"]["ros2_prefix"], str(prefix))
            self.assertEqual(report["boundary"]["host_tools_prefix"], str(host))
            self.assertEqual(report["boundary"]["target_runtime_prefix"], str(target))
            self.assertEqual(report["boundary"]["runtime_dependency_policy"], "rcl_free_ibmw_runtime")
            self.assertFalse(report["boundary"]["host_prefix_used_for_target_runtime"])
            self.assertTrue(report["cross_compile_safe"])
            self.assertEqual(report["prefix_provenance"]["package_version"], "1.2.3")
            self.assertIn("ament_index_cpp", report["dependency_resolution"]["forbidden_runtime_dependencies"])

    def test_reference_existing_records_target_artifact_audit(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            pkg = prefix / "share" / "existing_interfaces"
            (pkg / "srv").mkdir(parents=True)
            pkg.joinpath("srv", "SetFlag.srv").write_text("bool enable\n---\nbool accepted\n", encoding="utf-8")
            artifact = root / "target" / "libexisting_runtime.so"
            artifact.parent.mkdir(parents=True)
            artifact.write_bytes(b"target payload without forbidden host prefix")

            report = emit_reference_existing_interface_report(
                str(prefix),
                "existing_interfaces",
                str(root / "out"),
                target_artifacts=[str(artifact)],
                forbidden_prefixes=["/forbidden/host"],
            )

            audit = report["target_artifact_audit"]
            self.assertEqual(audit["status"], "pass")
            self.assertIn(str(prefix), audit["forbidden_prefixes"])
            self.assertIn("/forbidden/host", audit["forbidden_prefixes"])
            self.assertEqual(audit["artifacts"][0]["path"], str(artifact))
            persisted = json.loads((root / "out" / "ibmw_interface_report.json").read_text(encoding="utf-8"))
            self.assertEqual(persisted["target_artifact_audit"]["status"], "pass")

    def test_reference_existing_can_require_target_artifact_audit_pass(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            pkg = prefix / "share" / "existing_interfaces"
            (pkg / "srv").mkdir(parents=True)
            pkg.joinpath("srv", "SetFlag.srv").write_text("bool enable\n---\nbool accepted\n", encoding="utf-8")
            artifact = root / "target" / "libexisting_runtime.so"
            artifact.parent.mkdir(parents=True)
            artifact.write_text(f"leaked host prefix {prefix}\n", encoding="utf-8")

            with self.assertRaises(InterfaceParseError) as ctx:
                emit_reference_existing_interface_report(
                    str(prefix),
                    "existing_interfaces",
                    str(root / "out"),
                    target_artifacts=[str(artifact)],
                    require_target_audit_pass=True,
                )

            self.assertIn("target artifact audit failed", str(ctx.exception))

    def test_emit_reference_existing_interface_report_selected_types(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            pkg = prefix / "share" / "existing_interfaces"
            (pkg / "msg").mkdir(parents=True)
            (pkg / "srv").mkdir(parents=True)
            pkg.joinpath("msg", "Status.msg").write_text("uint32 code\n", encoding="utf-8")
            pkg.joinpath("msg", "Detail.msg").write_text("string text\n", encoding="utf-8")
            pkg.joinpath("srv", "SetFlag.srv").write_text("bool enable\n---\nbool accepted\n", encoding="utf-8")

            report = emit_reference_existing_interface_report(
                str(prefix),
                "existing_interfaces",
                str(root / "out"),
                selected_types=["Status", "existing_interfaces/srv/SetFlag"],
            )

            self.assertEqual(report["files"], ["msg/Status.msg", "srv/SetFlag.srv"])
            self.assertEqual(report["selected_types"], ["Status", "existing_interfaces/srv/SetFlag"])
            self.assertEqual(
                [entry["selector"] for entry in report["available_types"]],
                ["msg/Detail", "msg/Status", "srv/SetFlag"],
            )
            self.assertEqual([msg["name"] for msg in report["ibmw_wrapper"]["messages"]], ["Status"])
            self.assertEqual([srv["name"] for srv in report["ibmw_wrapper"]["services"]], ["SetFlag"])

    def test_reference_existing_rejects_missing_selected_type(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            pkg = prefix / "share" / "existing_interfaces"
            (pkg / "msg").mkdir(parents=True)
            pkg.joinpath("msg", "Status.msg").write_text("uint32 code\n", encoding="utf-8")

            with self.assertRaisesRegex(InterfaceParseError, "not found"):
                emit_reference_existing_interface_report(
                    str(prefix),
                    "existing_interfaces",
                    str(root / "out"),
                    selected_types=["Missing"],
                )

    def test_reference_existing_missing_type_lists_available_types(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            pkg = prefix / "share" / "existing_interfaces"
            (pkg / "msg").mkdir(parents=True)
            (pkg / "srv").mkdir(parents=True)
            pkg.joinpath("msg", "Status.msg").write_text("uint32 code\n", encoding="utf-8")
            pkg.joinpath("srv", "SetFlag.srv").write_text("bool enable\n---\nbool accepted\n", encoding="utf-8")

            with self.assertRaisesRegex(
                InterfaceParseError,
                r"available types: msg/Status, srv/SetFlag",
            ):
                emit_reference_existing_interface_report(
                    str(prefix),
                    "existing_interfaces",
                    str(root / "out"),
                    selected_types=["Missing"],
                )

    def test_cli_import_reference_existing_json(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            pkg = prefix / "share" / "existing_interfaces"
            (pkg / "msg").mkdir(parents=True)
            pkg.joinpath("msg", "Status.msg").write_text("uint32 code\n", encoding="utf-8")
            out = root / "out"

            result = subprocess.run(
                [
                    sys.executable,
                    str(IBMW_GEN),
                    "import",
                    "--ros2-prefix",
                    str(prefix),
                    "--ros2-package",
                    "existing_interfaces",
                    "--ros2-type",
                    "msg/Status",
                    "--artifact-only",
                    "--output-dir",
                    str(out),
                    "--format",
                    "json",
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(result.stdout)
            self.assertEqual(data["mode"], "reference_existing")
            self.assertEqual(data["package"], "existing_interfaces")
            self.assertEqual(data["files"], ["msg/Status.msg"])
            self.assertEqual(data["selected_types"], ["msg/Status"])
            self.assertEqual(data["available_types"][0]["canonical"], "existing_interfaces/msg/Status")
            self.assertEqual(
                data["generated_files"],
                [
                    "ibmw_interface_report.json",
                    "include/existing_interfaces/ibmw_interface_types.hpp",
                    "include/existing_interfaces/ibmw_ros2_conversion.hpp",
                    "existing_interfaces_dependency_closure.json",
                    "existing_interfaces_migration_manifest.json",
                ],
            )
            self.assertEqual(result.stdout, json.dumps(data, sort_keys=True, indent=2) + "\n")

    def test_cli_import_reference_existing_resolves_prefix_from_ament_env(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            pkg = prefix / "share" / "env_interfaces"
            (pkg / "msg").mkdir(parents=True)
            pkg.joinpath("msg", "Status.msg").write_text("uint32 code\n", encoding="utf-8")
            out = root / "out"
            env = os.environ.copy()
            env["AMENT_PREFIX_PATH"] = str(prefix)
            env.pop("COLCON_PREFIX_PATH", None)

            result = subprocess.run(
                [
                    sys.executable,
                    str(IBMW_GEN),
                    "import",
                    "--ros2-package",
                    "env_interfaces",
                    "--ros2-type",
                    "msg/Status",
                    "--artifact-only",
                    "--output-dir",
                    str(out),
                    "--format",
                    "json",
                ],
                capture_output=True,
                text=True,
                timeout=30,
                env=env,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(result.stdout)
            self.assertEqual(data["ros2_prefix"], str(prefix))
            self.assertEqual(data["ros2_prefix_source"], "environment:AMENT_PREFIX_PATH")
            self.assertEqual(data["boundary"]["ros2_prefix_source"], "environment:AMENT_PREFIX_PATH")
            self.assertFalse(data["migration_manifest"]["uses_ros2_cli"])

    def test_resolve_reference_existing_prefix_from_environment_is_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            first = root / "first"
            second = root / "second"
            for prefix in (first, second):
                pkg = prefix / "share" / "env_interfaces"
                (pkg / "msg").mkdir(parents=True)
                pkg.joinpath("msg", "Status.msg").write_text("uint32 code\n", encoding="utf-8")

            result = resolve_reference_existing_prefix_from_environment(
                "env_interfaces",
                env={"AMENT_PREFIX_PATH": os.pathsep.join([str(first), str(second)])},
            )

            self.assertEqual(result["prefix"], str(first))
            self.assertEqual(result["source"], "environment:AMENT_PREFIX_PATH")
            self.assertEqual(result["candidate_count"], 2)
            self.assertEqual(result["match_count"], 2)

    def test_emit_reference_existing_discovery_report_lists_packages(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            demo_pkg = prefix / "share" / "demo_interfaces"
            other_pkg = prefix / "share" / "other_interfaces"
            non_interface_pkg = prefix / "share" / "plain_pkg"
            (demo_pkg / "msg").mkdir(parents=True)
            (demo_pkg / "srv").mkdir(parents=True)
            (other_pkg / "action").mkdir(parents=True)
            non_interface_pkg.mkdir(parents=True)
            demo_pkg.joinpath("package.xml").write_text(
                "<package format='3'><name>demo_interfaces</name><version>1.2.0</version></package>\n",
                encoding="utf-8",
            )
            demo_pkg.joinpath("msg", "Status.msg").write_text("uint32 code\n", encoding="utf-8")
            demo_pkg.joinpath("srv", "SetFlag.srv").write_text("bool enable\n---\nbool accepted\n", encoding="utf-8")
            other_pkg.joinpath("action", "DoThing.action").write_text("int32 order\n---\nbool ok\n---\nint32 progress\n", encoding="utf-8")

            report = emit_reference_existing_discovery_report(str(prefix))

            self.assertEqual(report["mode"], "reference_existing_discovery")
            self.assertEqual(report["boundary"]["discovery_source"], "share_tree_scan")
            self.assertFalse(report["boundary"]["uses_ros2_cli"])
            self.assertFalse(report["boundary"]["uses_ament_index_python"])
            self.assertEqual(report["package_count"], 2)
            self.assertEqual(report["type_count"], 3)
            self.assertEqual([pkg["package"] for pkg in report["packages"]], ["demo_interfaces", "other_interfaces"])
            self.assertEqual(
                [entry["selector"] for entry in report["packages"][0]["available_types"]],
                ["msg/Status", "srv/SetFlag"],
            )
            self.assertEqual(report["packages"][1]["available_types"][0]["canonical"], "other_interfaces/action/DoThing")

    def test_emit_reference_existing_discovery_report_filters_package(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            pkg = prefix / "share" / "demo_interfaces"
            (pkg / "msg").mkdir(parents=True)
            pkg.joinpath("msg", "Status.msg").write_text("uint32 code\n", encoding="utf-8")

            report = emit_reference_existing_discovery_report(str(prefix), package_name="demo_interfaces")

            self.assertEqual(report["package"], "demo_interfaces")
            self.assertEqual(report["package_count"], 1)
            self.assertEqual(report["packages"][0]["available_types"][0]["selector"], "msg/Status")

    def test_cli_list_reference_existing_resolves_prefix_from_env_for_package(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            pkg = prefix / "share" / "env_interfaces"
            (pkg / "msg").mkdir(parents=True)
            pkg.joinpath("msg", "Status.msg").write_text("uint32 code\n", encoding="utf-8")
            env = os.environ.copy()
            env["AMENT_PREFIX_PATH"] = str(prefix)

            result = subprocess.run(
                [
                    sys.executable,
                    str(IBMW_GEN),
                    "import",
                    "--ros2-package",
                    "env_interfaces",
                    "--list-ros2-interfaces",
                    "--format",
                    "json",
                ],
                capture_output=True,
                text=True,
                timeout=30,
                env=env,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(result.stdout)
            self.assertEqual(data["ros2_prefix"], str(prefix))
            self.assertEqual(data["ros2_prefix_source"], "environment:AMENT_PREFIX_PATH")
            self.assertEqual(data["packages"][0]["package"], "env_interfaces")

    def test_emit_workspace_interface_discovery_report_lists_source_packages(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            ws = root / "ws"
            src = ws / "src"
            robot_pkg = src / "robot_interfaces"
            other_pkg = src / "other_interfaces"
            plain_pkg = src / "plain_pkg"
            (robot_pkg / "msg").mkdir(parents=True)
            (robot_pkg / "srv").mkdir(parents=True)
            (other_pkg / "action").mkdir(parents=True)
            plain_pkg.mkdir(parents=True)
            robot_pkg.joinpath("package.xml").write_text(
                "<package format='3'><name>robot_interfaces</name><version>0.1.0</version></package>\n",
                encoding="utf-8",
            )
            robot_pkg.joinpath("msg", "Pose.msg").write_text("float64 x\n", encoding="utf-8")
            robot_pkg.joinpath("srv", "SetMode.srv").write_text("bool enable\n---\nbool ok\n", encoding="utf-8")
            other_pkg.joinpath("action", "DoThing.action").write_text("int32 order\n---\nbool ok\n---\nint32 progress\n", encoding="utf-8")

            report = emit_workspace_interface_discovery_report(str(ws))

            self.assertEqual(report["mode"], "workspace_source_discovery")
            self.assertEqual(report["boundary"]["discovery_source"], "workspace_source_tree_scan")
            self.assertFalse(report["boundary"]["uses_ros2_cli"])
            self.assertFalse(report["boundary"]["build_behavior_changed"])
            self.assertEqual(report["package_count"], 2)
            self.assertEqual([pkg["package"] for pkg in report["packages"]], ["other_interfaces", "robot_interfaces"])
            self.assertEqual(report["packages"][1]["recommended_import_mode"], "generate_package")
            self.assertEqual(
                [entry["selector"] for entry in report["packages"][1]["available_types"]],
                ["msg/Pose", "srv/SetMode"],
            )

    def test_cli_workspace_interface_discovery_json(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            ws = root / "ws"
            pkg = ws / "src" / "robot_interfaces"
            (pkg / "msg").mkdir(parents=True)
            pkg.joinpath("package.xml").write_text(
                "<package format='3'><name>robot_interfaces</name><version>0.1.0</version></package>\n",
                encoding="utf-8",
            )
            pkg.joinpath("msg", "Pose.msg").write_text("float64 x\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    str(IBMW_GEN),
                    "import",
                    "--ros2-workspace",
                    str(ws),
                    "--ros2-package",
                    "robot_interfaces",
                    "--list-ros2-interfaces",
                    "--format",
                    "json",
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(result.stdout)
            self.assertEqual(data["mode"], "workspace_source_discovery")
            self.assertEqual(data["package"], "robot_interfaces")
            self.assertEqual(data["packages"][0]["available_types"][0]["canonical"], "robot_interfaces/msg/Pose")
            self.assertEqual(result.stdout, json.dumps(data, sort_keys=True, indent=2) + "\n")

    def test_cli_workspace_package_import_generates_artifacts(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            ws = root / "ws"
            pkg = ws / "src" / "robot_interfaces"
            (pkg / "msg").mkdir(parents=True)
            pkg.joinpath("package.xml").write_text(
                "<package format='3'><name>robot_interfaces</name><version>0.1.0</version></package>\n",
                encoding="utf-8",
            )
            pkg.joinpath("msg", "Pose.msg").write_text("float64 x\n", encoding="utf-8")
            out = root / "out"

            result = subprocess.run(
                [
                    sys.executable,
                    str(IBMW_GEN),
                    "import",
                    "--ros2-workspace",
                    str(ws),
                    "--ros2-package",
                    "robot_interfaces",
                    "--artifact-only",
                    "--output-dir",
                    str(out),
                    "--format",
                    "json",
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(result.stdout)
            self.assertEqual(data["mode"], "generate_package")
            self.assertEqual(data["input_mode"], "workspace_source_package")
            self.assertEqual(data["workspace_source"]["workspace_root"], str(ws))
            self.assertEqual(data["workspace_source"]["package_root"], str(pkg))
            self.assertEqual(data["files"], ["msg/Pose.msg"])
            self.assertEqual(data["boundary"]["discovery_source"], "workspace_source_tree_scan")
            self.assertTrue(out.joinpath("msg", "Pose.msg").is_file())
            persisted = json.loads(out.joinpath("ibmw_interface_report.json").read_text(encoding="utf-8"))
            self.assertEqual(persisted, data)

    def test_cli_workspace_import_requires_package(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                [
                    sys.executable,
                    str(IBMW_GEN),
                    "import",
                    "--ros2-workspace",
                    tmp,
                    "--artifact-only",
                    "--output-dir",
                    str(pathlib.Path(tmp) / "out"),
                    "--format",
                    "json",
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("--ros2-workspace artifact import requires --ros2-package", result.stderr)

    def test_cli_import_list_ros2_interfaces_json(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            pkg = prefix / "share" / "existing_interfaces"
            (pkg / "msg").mkdir(parents=True)
            pkg.joinpath("msg", "Status.msg").write_text("uint32 code\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    str(IBMW_GEN),
                    "import",
                    "--ros2-prefix",
                    str(prefix),
                    "--list-ros2-interfaces",
                    "--format",
                    "json",
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            data = json.loads(result.stdout)
            self.assertEqual(data["mode"], "reference_existing_discovery")
            self.assertEqual(data["package_count"], 1)
            self.assertEqual(data["packages"][0]["available_types"][0]["canonical"], "existing_interfaces/msg/Status")
            self.assertEqual(result.stdout, json.dumps(data, sort_keys=True, indent=2) + "\n")

    def test_cli_import_rejects_mixed_reference_and_source_modes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            prefix = root / "ros"
            (prefix / "share" / "existing_interfaces" / "msg").mkdir(parents=True)
            src = root / "src_pkg"
            (src / "msg").mkdir(parents=True)
            src.joinpath("msg", "Ping.msg").write_text("uint32 seq\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    str(IBMW_GEN),
                    "import",
                    "--ros2-src",
                    str(src),
                    "--ros2-prefix",
                    str(prefix),
                    "--ros2-package",
                    "existing_interfaces",
                    "--artifact-only",
                    "--output-dir",
                    str(root / "out"),
                    "--format",
                    "json",
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn("choose either --ros2-src", result.stderr)

    def test_imported_srv_config_remaps_to_friendly_ros2_service_name(self) -> None:
        from generators import config_gen

        module_map = {
            "SetModeRpcServerModule": {
                "name": "SetModeRpcServerModule",
                "type": "rpc_server",
                "transport": "ros2",
                "ros2_package": "demo_interfaces",
                "ros2_service": "SetMode",
                "ros2_service_name": "/set_mode",
                "ros2_imported_srv_adapter": True,
            }
        }
        project = {
            "name": "demo",
            "namespace": "demo",
            "project": {"_namespace_cpp": "demo"},
            "_module_map": module_map,
        }
        app = {
            "name": "set_mode_imported_srv_server_app",
            "_snake_name": "set_mode_imported_srv_server_app",
            "type": "rpc",
            "modules": ["SetModeRpcServerModule"],
        }

        text = config_gen._gen_config(app, project, is_pkg=False)

        self.assertIn("      - type: ros2", text)
        self.assertIn("          servers:", text)
        self.assertIn('            - func_name: "^ros2:/demo_interfaces/srv/SetMode$"', text)
        self.assertIn('              remapping_rule: "/set_mode"', text)

    def test_emit_rosidl_artifact_package_from_single_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            src = root / "Standalone.msg"
            src.write_text("uint32 seq\n", encoding="utf-8")
            out = root / "out"

            report = emit_rosidl_artifact_package(str(src), str(out))

            self.assertEqual(report["input_mode"], "single_file")
            self.assertEqual(report["package"], "ibmw_imported_interfaces")
            self.assertEqual(report["files"], ["msg/Standalone.msg"])
            self.assertEqual(out.joinpath("msg", "Standalone.msg").read_text(encoding="utf-8"), "uint32 seq\n")
            cmake_lists = out.joinpath("CMakeLists.txt").read_text(encoding="utf-8")
            self.assertIn('  "msg/Standalone.msg"', cmake_lists)

    def test_emit_rosidl_artifact_package_from_workspace_root(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            ws = root / "ws"
            pkg_a = ws / "src" / "alpha_interfaces"
            pkg_b = ws / "src" / "beta_interfaces"
            (pkg_a / "msg").mkdir(parents=True)
            (pkg_b / "srv").mkdir(parents=True)
            pkg_a.joinpath("package.xml").write_text(
                "<package format='3'><name>alpha_interfaces</name><version>0.0.0</version></package>\n",
                encoding="utf-8",
            )
            pkg_b.joinpath("package.xml").write_text(
                "<package format='3'><name>beta_interfaces</name><version>0.0.0</version></package>\n",
                encoding="utf-8",
            )
            pkg_a.joinpath("msg", "Alpha.msg").write_text("int32 value\n", encoding="utf-8")
            pkg_b.joinpath("srv", "Beta.srv").write_text("alpha_interfaces/Alpha input\n---\nbool ok\n", encoding="utf-8")
            out = root / "generated"

            report = emit_rosidl_artifact_package(str(ws), str(out))

            self.assertEqual(report["mode"], "workspace")
            self.assertEqual([pkg["package"] for pkg in report["packages"]], ["alpha_interfaces", "beta_interfaces"])
            self.assertTrue(out.joinpath("alpha_interfaces", "msg", "Alpha.msg").is_file())
            self.assertTrue(out.joinpath("beta_interfaces", "srv", "Beta.srv").is_file())
            beta_cmake = out.joinpath("beta_interfaces", "CMakeLists.txt").read_text(encoding="utf-8")
            self.assertIn("find_package(alpha_interfaces REQUIRED)", beta_cmake)
            self.assertTrue(out.joinpath("ibmw_workspace_interface_report.json").is_file())

    def test_json_success_is_deterministic(self) -> None:
        path = self._write_tmp(
            ".msg",
            "uint8 OK=1\nint32 count 5\nstring<=5[3] labels\n",
        )
        try:
            first = diagnose_interface_json(str(path))
            second = diagnose_interface_json(str(path))
            result = subprocess.run(
                [sys.executable, str(IBMW_GEN), "diagnose-interface", "--format", "json", str(path)],
                capture_output=True,
                text=True,
                timeout=30,
            )
        finally:
            path.unlink(missing_ok=True)

        self.assertEqual(first, second)
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertTrue(data["ok"])
        self.assertEqual(data["schema_version"], 1)
        self.assertEqual(data["interface"]["kind"], "msg")
        self.assertEqual(data["ros2_compatibility"]["status"], "not_requested")
        self.assertEqual(data["messages"][0]["fields"][0]["default"], "5")
        self.assertIn("capability", data)
        self.assertEqual(result.stdout, json.dumps(data, sort_keys=True, indent=2) + "\n")

    def test_collect_ros2_compatibility_from_prefix(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            prefix = self._write_fake_ros2_prefix(pathlib.Path(tmp))

            report = collect_ros2_compatibility(str(prefix), ros2_distro="humble")

        self.assertEqual(report["distro"], "humble")
        self.assertEqual(report["status"], "complete")
        self.assertEqual(report["boundary_status"], "ok")
        self.assertEqual(report["matrix_status"], "unknown_combo")
        self.assertEqual(report["known_good"], "no")
        self.assertEqual(report["missing_packages"], [])
        self.assertEqual([p["version"] for p in report["packages"]], ["1.2.3"] * 7)
        self.assertTrue(report["packages"][-1]["version_source"].endswith("fastcdr-config-version.cmake"))
        self.assertTrue(any("not in the known-good matrix" in reason for reason in report["reasons"]))

    def test_collect_ros2_compatibility_matches_humble_known_good_matrix(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            prefix = self._write_known_good_humble_prefix(pathlib.Path(tmp))

            report = collect_ros2_compatibility(str(prefix), ros2_distro="humble")

        self.assertEqual(report["status"], "complete")
        self.assertEqual(report["boundary_status"], "ok")
        self.assertEqual(report["matrix_status"], "matched")
        self.assertEqual(report["known_good"], "yes")
        self.assertTrue(any("matched known-good matrix entry" in reason for reason in report["reasons"]))

    def test_collect_ros2_compatibility_cross_prefix_boundary_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            host = self._write_fake_ros2_prefix(root / "host", "1.2.3")
            target = self._write_fake_ros2_prefix(root / "target", "1.2.3")

            report = collect_ros2_compatibility(
                host_tools_prefix=str(host),
                target_runtime_prefix=str(target),
                ros2_distro="humble",
                target_runtime_distro="jazzy",
            )

        self.assertEqual(report["host_tools"]["status"], "complete")
        self.assertEqual(report["target_runtime"]["status"], "complete")
        self.assertEqual(report["boundary_status"], "mismatch")
        self.assertTrue(any("host tools distro humble != target runtime distro jazzy" in reason for reason in report["boundary_reasons"]))
        self.assertTrue(any("target runtime distro jazzy" in reason for reason in report["reasons"]))

    def test_collect_ros2_compatibility_reports_missing_versions(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            report = collect_ros2_compatibility(tmp, ros2_distro="jazzy")

        self.assertEqual(report["distro"], "jazzy")
        self.assertEqual(report["status"], "incomplete")
        self.assertEqual(report["known_good"], "no")
        self.assertIn("rosidl_generator_cpp", report["missing_packages"])
        self.assertTrue(any("outside the current Humble-only" in reason for reason in report["reasons"]))

    def test_cli_json_ros2_compatibility_prefix(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            msg = root / "Msg.msg"
            msg.write_text("int32 value\n", encoding="utf-8")
            self._write_fake_ros2_prefix(root / "ros", "9.8.7")

            result = subprocess.run(
                [
                    sys.executable,
                    str(IBMW_GEN),
                    "diagnose-interface",
                    "--format",
                    "json",
                    "--ros2-prefix",
                    str(root / "ros"),
                    "--ros2-distro",
                    "humble",
                    "--allow-unknown-ros2-dds-combo",
                    str(msg),
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data["ros2_compatibility"]["status"], "complete")
        self.assertEqual(data["ros2_compatibility"]["matrix_status"], "unknown_combo")
        self.assertEqual(data["ros2_compatibility"]["known_good"], "no")
        self.assertEqual(data["ros2_compatibility"]["boundary_status"], "ok")
        self.assertEqual(data["ros2_compatibility"]["packages"][0]["version"], "9.8.7")

    def test_format_ros2_compatibility_lock_yaml_is_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            prefix = pathlib.Path(tmp)
            package_dir = prefix / "share" / "rosidl_generator_cpp"
            package_dir.mkdir(parents=True)
            package_dir.joinpath("package.xml").write_text(
                "<package format='3'><name>rosidl_generator_cpp</name><version>4.0.0</version></package>\n",
                encoding="utf-8",
            )
            report = collect_ros2_compatibility(str(prefix), ros2_distro="humble")

        yaml_text = format_ros2_compatibility_lock_yaml(build_ros2_compatibility_lock(report))
        self.assertEqual(yaml_text, format_ros2_compatibility_lock_yaml(build_ros2_compatibility_lock(report)))
        self.assertIn("ros2_compatibility_lock:\n", yaml_text)
        self.assertIn("schema_version: 1", yaml_text)
        self.assertIn("matrix_status: unknown_combo", yaml_text)
        self.assertIn("known_good: \"no\"", yaml_text)
        self.assertIn("name: rosidl_generator_cpp", yaml_text)
        self.assertIn("version: 4.0.0", yaml_text)

    def test_cli_writes_compatibility_lock_output(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            msg = root / "Msg.msg"
            lock = root / "out" / "ros2_compatibility.lock.yaml"
            msg.write_text("int32 value\n", encoding="utf-8")
            self._write_fake_ros2_prefix(root / "host", "4.0.0")
            self._write_fake_ros2_prefix(root / "target", "4.0.0")

            result = subprocess.run(
                [
                    sys.executable,
                    str(IBMW_GEN),
                    "diagnose-interface",
                    "--host-tools-prefix",
                    str(root / "host"),
                    "--target-runtime-prefix",
                    str(root / "target"),
                    "--ros2-distro",
                    "humble",
                    "--allow-unknown-ros2-dds-combo",
                    "--compatibility-lock-output",
                    str(lock),
                    str(msg),
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )

            lock_text = lock.read_text(encoding="utf-8")

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ros2-compatibility:", result.stdout)
        self.assertIn("ros2_compatibility_lock:", lock_text)
        self.assertIn("host_tools:", lock_text)
        self.assertIn("target_runtime:", lock_text)
        self.assertIn("boundary_status: ok", lock_text)
        self.assertIn("target_artifact_audit:", lock_text)
        self.assertIn("matrix_status: unknown_combo", lock_text)

    def test_cli_unknown_complete_combo_fails_by_default(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            msg = root / "Msg.msg"
            msg.write_text("int32 value\n", encoding="utf-8")
            self._write_fake_ros2_prefix(root / "ros", "9.8.7")

            result = subprocess.run(
                [
                    sys.executable,
                    str(IBMW_GEN),
                    "diagnose-interface",
                    "--format",
                    "json",
                    "--ros2-prefix",
                    str(root / "ros"),
                    "--ros2-distro",
                    "humble",
                    str(msg),
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )

        self.assertNotEqual(result.returncode, 0)
        data = json.loads(result.stderr)
        self.assertFalse(data["ok"])
        self.assertIn("not known-good", data["error"])

    def test_cli_known_good_combo_succeeds_without_allow_unknown(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            msg = root / "Msg.msg"
            msg.write_text("int32 value\n", encoding="utf-8")
            self._write_known_good_humble_prefix(root / "ros")

            result = subprocess.run(
                [
                    sys.executable,
                    str(IBMW_GEN),
                    "diagnose-interface",
                    "--format",
                    "json",
                    "--ros2-prefix",
                    str(root / "ros"),
                    "--ros2-distro",
                    "humble",
                    str(msg),
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data["ros2_compatibility"]["matrix_status"], "matched")
        self.assertEqual(data["ros2_compatibility"]["known_good"], "yes")

    @unittest.skipIf(
        not pathlib.Path("/opt/ros/humble").is_dir() or shutil.which("cmake") is None,
        "requires local ROS 2 Humble install and cmake",
    )
    def test_cpp_serializer_uses_generated_fastrtps_callbacks_for_unlisted_msg_matrix(self) -> None:
        repo_root = ROOT.parents[2]
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            src = root / "main.cc"
            src.write_text(
                r'''
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <builtin_interfaces/msg/detail/time__rosidl_typesupport_fastrtps_cpp.hpp>
#include <builtin_interfaces/msg/time.hpp>
#include <fastcdr/Cdr.h>
#include <fastcdr/FastBuffer.h>
#include <nav_msgs/msg/detail/odometry__rosidl_typesupport_fastrtps_cpp.hpp>
#include <nav_msgs/msg/odometry.hpp>
#include <sensor_msgs/msg/detail/point_cloud2__rosidl_typesupport_fastrtps_cpp.hpp>
#include <sensor_msgs/msg/point_cloud2.hpp>

#include "ros2_types/util/ros2_cdr_serializer.h"

template <typename MsgT, typename Mutate, typename Verify>
int RoundTrip(MsgT in, Mutate mutate, Verify verify) {
  mutate(in);

  const uint32_t estimated = ibmw::ros2::ROS2CdrSerializer::EstimateSize(in);
  if (estimated == 0 || estimated > 8192) {
    return 10;
  }

  char storage[8192]{};
  eprosima::fastcdr::FastBuffer buffer(storage, sizeof(storage));
  eprosima::fastcdr::Cdr ser(
      buffer,
      eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
      ibmw::ros2::ROS2CdrSerializer::kCdrType);
  ibmw::ros2::ROS2CdrSerializer::SerializeMessage(ser, in);

  MsgT out;
  eprosima::fastcdr::Cdr deser(
      buffer,
      eprosima::fastcdr::Cdr::DEFAULT_ENDIAN,
      ibmw::ros2::ROS2CdrSerializer::kCdrType);
  ibmw::ros2::ROS2CdrSerializer::DeserializeMessage(deser, out);
  return verify(out) ? 0 : 11;
}

int main() {
  const int time_rc = RoundTrip(
      builtin_interfaces::msg::Time{},
      [](auto& msg) {
        msg.sec = 7;
        msg.nanosec = 42;
      },
      [](const auto& msg) {
        return msg.sec == 7 && msg.nanosec == 42;
      });
  if (time_rc != 0) {
    return time_rc;
  }

  const int cloud_rc = RoundTrip(
      sensor_msgs::msg::PointCloud2{},
      [](auto& msg) {
        msg.header.frame_id = "map";
        msg.height = 1;
        msg.width = 2;
        sensor_msgs::msg::PointField field;
        field.name = "x";
        field.offset = 0;
        field.datatype = sensor_msgs::msg::PointField::FLOAT32;
        field.count = 1;
        msg.fields.push_back(field);
        msg.is_bigendian = false;
        msg.point_step = 4;
        msg.row_step = 8;
        msg.data = {0, 0, 128, 63, 0, 0, 0, 64};
        msg.is_dense = true;
      },
      [](const auto& msg) {
        return msg.header.frame_id == "map" && msg.fields.size() == 1 &&
            msg.fields[0].name == "x" && msg.data.size() == 8 &&
            msg.data[3] == 63 && msg.is_dense;
      });
  if (cloud_rc != 0) {
    return cloud_rc + 20;
  }

  const int odom_rc = RoundTrip(
      nav_msgs::msg::Odometry{},
      [](auto& msg) {
        msg.header.frame_id = "odom";
        msg.child_frame_id = "base_link";
        msg.pose.pose.position.x = 1.25;
        msg.pose.pose.orientation.w = 1.0;
        msg.twist.twist.linear.x = 0.5;
        msg.pose.covariance[0] = 0.01;
        msg.twist.covariance[35] = 0.02;
      },
      [](const auto& msg) {
        return msg.header.frame_id == "odom" && msg.child_frame_id == "base_link" &&
            msg.pose.pose.position.x == 1.25 && msg.pose.pose.orientation.w == 1.0 &&
            msg.twist.twist.linear.x == 0.5 && msg.pose.covariance[0] == 0.01 &&
            msg.twist.covariance[35] == 0.02;
      });
  if (odom_rc != 0) {
    return odom_rc + 40;
  }

  return 0;
}
''',
                encoding="utf-8",
            )
            root.joinpath("CMakeLists.txt").write_text(
                f"""
cmake_minimum_required(VERSION 3.16)
project(ros2_callback_serializer_probe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(builtin_interfaces REQUIRED)
find_package(fastcdr REQUIRED)
find_package(fastrtps REQUIRED)
find_package(nav_msgs REQUIRED)
find_package(rosidl_typesupport_fastrtps_cpp REQUIRED)
find_package(sensor_msgs REQUIRED)
add_executable(probe main.cc)
target_include_directories(probe PRIVATE {repo_root / 'src' / 'extensions' / 'transport'})
target_link_libraries(probe PRIVATE
  builtin_interfaces::builtin_interfaces__rosidl_generator_cpp
  builtin_interfaces::builtin_interfaces__rosidl_typesupport_fastrtps_cpp
  nav_msgs::nav_msgs__rosidl_generator_cpp
  nav_msgs::nav_msgs__rosidl_typesupport_fastrtps_cpp
  sensor_msgs::sensor_msgs__rosidl_generator_cpp
  sensor_msgs::sensor_msgs__rosidl_typesupport_fastrtps_cpp
  fastrtps
  fastcdr)
""",
                encoding="utf-8",
            )
            build = root / "build"
            env = os.environ.copy()
            env["CMAKE_PREFIX_PATH"] = "/opt/ros/humble" + os.pathsep + env.get("CMAKE_PREFIX_PATH", "")
            env["AMENT_PREFIX_PATH"] = "/opt/ros/humble" + os.pathsep + env.get("AMENT_PREFIX_PATH", "")
            env["LD_LIBRARY_PATH"] = "/opt/ros/humble/lib" + os.pathsep + env.get("LD_LIBRARY_PATH", "")
            env["PYTHONPATH"] = os.pathsep.join(
                [
                    "/opt/ros/humble/lib/python3.10/site-packages",
                    "/opt/ros/humble/local/lib/python3.10/dist-packages",
                    env.get("PYTHONPATH", ""),
                ]
            )
            configure = subprocess.run(
                ["cmake", "-S", str(root), "-B", str(build), "-DPython3_EXECUTABLE=/usr/bin/python3"],
                capture_output=True,
                text=True,
                timeout=60,
                env=env,
            )
            self.assertEqual(configure.returncode, 0, configure.stderr + configure.stdout)
            build_result = subprocess.run(
                ["cmake", "--build", str(build), "-j2"],
                capture_output=True,
                text=True,
                timeout=60,
                env=env,
            )
            self.assertEqual(build_result.returncode, 0, build_result.stderr + build_result.stdout)
            run_result = subprocess.run(
                [str(build / "probe")],
                capture_output=True,
                text=True,
                timeout=30,
                env=env,
            )
            self.assertEqual(run_result.returncode, 0, run_result.stderr + run_result.stdout)

    @unittest.skipIf(
        not pathlib.Path("/opt/ros/humble").is_dir()
        or shutil.which("cmake") is None
        or shutil.which("ldd") is None,
        "requires local ROS 2 Humble install, cmake, and ldd",
    )
    def test_generated_action_rejecting_runtime_compiles_and_links_without_rcl(self) -> None:
        repo_root = ROOT.parents[2]
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            import_dir = root / "import"
            result = subprocess.run(
                [
                    sys.executable,
                    str(IBMW_GEN),
                    "import",
                    "--ros2-prefix",
                    "/opt/ros/humble",
                    "--ros2-package",
                    "example_interfaces",
                    "--ros2-type",
                    "action/Fibonacci",
                    "--artifact-only",
                    "--output-dir",
                    str(import_dir),
                    "--format",
                    "json",
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)

            src = root / "main.cc"
            src.write_text(
                r'''
#include <example_interfaces/ibmw_action_server_runtime.hpp>

int main(int argc, char**) {
  ibmw::ros2_interfaces::example_interfaces::FibonacciRejectingActionServerRuntime runtime;
  if (argc == -1 && !runtime.Start(0)) {
    return 1;
  }
  runtime.Stop();
  (void)runtime;
  return 0;
}
''',
                encoding="utf-8",
            )
            root.joinpath("CMakeLists.txt").write_text(
                f"""
cmake_minimum_required(VERSION 3.16)
project(generated_action_rejecting_runtime_probe LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(action_msgs REQUIRED)
find_package(builtin_interfaces REQUIRED)
find_package(example_interfaces REQUIRED)
find_package(fastcdr REQUIRED)
find_package(fastrtps REQUIRED)
find_package(rmw REQUIRED)
find_package(unique_identifier_msgs REQUIRED)
add_executable(probe main.cc)
target_include_directories(probe PRIVATE
  {import_dir / 'include'}
  {repo_root / 'src' / 'api'}
  {repo_root / 'src'})
target_link_libraries(probe PRIVATE
  action_msgs::action_msgs__rosidl_generator_cpp
  action_msgs::action_msgs__rosidl_typesupport_fastrtps_cpp
  builtin_interfaces::builtin_interfaces__rosidl_generator_cpp
  builtin_interfaces::builtin_interfaces__rosidl_typesupport_fastrtps_cpp
  example_interfaces::example_interfaces__rosidl_generator_cpp
  example_interfaces::example_interfaces__rosidl_typesupport_fastrtps_cpp
  unique_identifier_msgs::unique_identifier_msgs__rosidl_generator_cpp
  unique_identifier_msgs::unique_identifier_msgs__rosidl_typesupport_fastrtps_cpp
  rmw::rmw
  fastrtps
  fastcdr)
""",
                encoding="utf-8",
            )

            build = root / "build"
            env = os.environ.copy()
            env["CMAKE_PREFIX_PATH"] = "/opt/ros/humble" + os.pathsep + env.get("CMAKE_PREFIX_PATH", "")
            env["AMENT_PREFIX_PATH"] = "/opt/ros/humble" + os.pathsep + env.get("AMENT_PREFIX_PATH", "")
            env["LD_LIBRARY_PATH"] = "/opt/ros/humble/lib" + os.pathsep + env.get("LD_LIBRARY_PATH", "")
            env["PYTHONPATH"] = os.pathsep.join(
                [
                    "/opt/ros/humble/lib/python3.10/site-packages",
                    "/opt/ros/humble/local/lib/python3.10/dist-packages",
                    env.get("PYTHONPATH", ""),
                ]
            )
            configure = subprocess.run(
                ["cmake", "-S", str(root), "-B", str(build), "-DPython3_EXECUTABLE=/usr/bin/python3"],
                capture_output=True,
                text=True,
                timeout=60,
                env=env,
            )
            self.assertEqual(configure.returncode, 0, configure.stderr + configure.stdout)
            build_result = subprocess.run(
                ["cmake", "--build", str(build), "-j2"],
                capture_output=True,
                text=True,
                timeout=60,
                env=env,
            )
            self.assertEqual(build_result.returncode, 0, build_result.stderr + build_result.stdout)

            ldd_result = subprocess.run(
                ["ldd", str(build / "probe")],
                capture_output=True,
                text=True,
                timeout=30,
                env=env,
            )
            self.assertEqual(ldd_result.returncode, 0, ldd_result.stderr + ldd_result.stdout)
            for forbidden in ("librcl", "librclcpp", "librclpy", "librcl_action", "librclcpp_action", "librmw"):
                self.assertNotIn(forbidden, ldd_result.stdout)

    def test_target_artifact_audit_detects_arch_and_prefix_failures(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            good = self._write_fake_elf(root / "good.so", "aarch64")
            bad = self._write_fake_elf(root / "bad.so", "x86_64", b"/opt/ros/humble/lib")

            report = audit_target_artifacts(
                [str(good), str(bad)],
                forbidden_prefixes=["/opt/ros/humble"],
                expected_arch="aarch64",
            )

        self.assertEqual(report["status"], "fail")
        self.assertEqual(report["artifacts"][0]["status"], "pass")
        self.assertEqual(report["artifacts"][0]["arch"], "aarch64")
        self.assertEqual(report["artifacts"][1]["status"], "fail")
        self.assertIn("/opt/ros/humble", report["artifacts"][1]["forbidden_prefix_hits"])
        self.assertTrue(any("arch x86_64 != expected aarch64" in reason for reason in report["reasons"]))

    def test_cli_target_artifact_audit_json_and_required_failure(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            msg = root / "Msg.msg"
            artifact = self._write_fake_elf(root / "target.so", "x86_64", b"/host/ros")
            msg.write_text("int32 value\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    str(IBMW_GEN),
                    "diagnose-interface",
                    "--format",
                    "json",
                    "--target-artifact",
                    str(artifact),
                    "--forbid-prefix",
                    "/host/ros",
                    "--expected-target-arch",
                    "aarch64",
                    "--require-target-audit-pass",
                    str(msg),
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )

        self.assertNotEqual(result.returncode, 0)
        data = json.loads(result.stderr)
        self.assertFalse(data["ok"])
        self.assertIn("target artifact audit did not pass", data["error"])

    def test_cli_require_known_good_fails_unknown_combo(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            msg = root / "Msg.msg"
            msg.write_text("int32 value\n", encoding="utf-8")
            package_dir = root / "ros" / "share" / "rosidl_generator_cpp"
            package_dir.mkdir(parents=True)
            package_dir.joinpath("package.xml").write_text(
                "<package format='3'><name>rosidl_generator_cpp</name><version>4.0.0</version></package>\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(IBMW_GEN),
                    "diagnose-interface",
                    "--format",
                    "json",
                    "--ros2-prefix",
                    str(root / "ros"),
                    "--ros2-distro",
                    "humble",
                    "--require-known-good",
                    str(msg),
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )

        self.assertNotEqual(result.returncode, 0)
        data = json.loads(result.stderr)
        self.assertFalse(data["ok"])
        self.assertIn("not known-good", data["error"])

    def test_json_capability_for_plain_fixed_message(self) -> None:
        path = self._write_tmp(".msg", "int32 count\nfloat64[3] xyz\n")
        try:
            data = diagnose_interface_json(str(path))
        finally:
            path.unlink(missing_ok=True)

        cap = data["capability"]
        self.assertTrue(cap["is_plain"])
        self.assertTrue(cap["is_fixed_size"])
        self.assertTrue(cap["is_bounded"])
        self.assertFalse(cap["contains_heap"])
        self.assertEqual(cap["max_serialized_size_bytes"], 28)
        self.assertEqual(cap["zero_copy"], {"dds_loaned": "yes", "iceoryx": "yes"})
        self.assertEqual(cap["transports"]["iceoryx"], "yes")
        self.assertEqual(cap["suggestions"], [])
        fields = data["messages"][0]["fields"]
        self.assertEqual(fields[0]["capability"]["size_bytes"], 4)
        self.assertEqual(fields[1]["capability"]["size_bytes"], 24)

    def test_json_capability_reports_primitive_size_bounds(self) -> None:
        path = self._write_tmp(".msg", "float64 x\nfloat64 y\nfloat32[4] quat\nint16 s\n")
        try:
            data = diagnose_interface_json(str(path))
        finally:
            path.unlink(missing_ok=True)

        fields = data["messages"][0]["fields"]
        self.assertEqual([field["capability"]["size_bytes"] for field in fields], [8, 8, 16, 2])
        self.assertEqual(data["messages"][0]["capability"]["max_serialized_size_bytes"], 34)
        self.assertEqual(data["capability"]["max_serialized_size_bytes"], 34)

    def test_json_capability_reports_resolved_nested_plain_size_bounds(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            builtin = root / "builtin_interfaces" / "msg" / "Time.msg"
            stamped = root / "my_pkg" / "msg" / "Stamped.msg"
            builtin.parent.mkdir(parents=True)
            stamped.parent.mkdir(parents=True)
            builtin.write_text("int32 sec\nuint32 nanosec\n", encoding="utf-8")
            stamped.write_text("builtin_interfaces/Time stamp\nuint32 seq\n", encoding="utf-8")

            data = diagnose_interface_json(str(stamped), search_roots=[str(root)])

        fields = data["messages"][0]["fields"]
        self.assertEqual(fields[0]["capability"]["size_bytes"], 8)
        self.assertEqual(fields[1]["capability"]["size_bytes"], 4)
        self.assertEqual(data["messages"][0]["capability"]["max_serialized_size_bytes"], 12)
        self.assertEqual(data["capability"]["max_serialized_size_bytes"], 12)
        self.assertEqual(data["dependencies"][0]["serialized_size_bytes"], 8)

    def test_backend_lowering_plans_include_resolved_nested_plain_size_bounds(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            builtin = root / "builtin_interfaces" / "msg" / "Time.msg"
            src = root / "my_pkg"
            stamped = src / "msg" / "Stamped.msg"
            builtin.parent.mkdir(parents=True)
            stamped.parent.mkdir(parents=True)
            builtin.write_text("int32 sec\nuint32 nanosec\n", encoding="utf-8")
            stamped.write_text("builtin_interfaces/Time stamp\nuint32 seq\n", encoding="utf-8")
            out = root / "out"

            emit_rosidl_artifact_package(str(src), str(out), targets=["dds_native", "iceoryx"])

            dds_plan = json.loads(out.joinpath("my_pkg_dds_native_lowering_plan.json").read_text(encoding="utf-8"))
            iceoryx_plan = json.loads(out.joinpath("my_pkg_iceoryx_lowering_plan.json").read_text(encoding="utf-8"))
            self.assertEqual(dds_plan["eligible_interfaces"][0]["max_serialized_size_bytes"], 12)
            self.assertEqual(iceoryx_plan["eligible_interfaces"][0]["max_serialized_size_bytes"], 12)

    def test_json_capability_suggestions_for_string_and_sequence(self) -> None:
        path = self._write_tmp(".msg", "string frame_id\nuint8[] data\nstring<=8 label\n")
        try:
            data = diagnose_interface_json(str(path))
        finally:
            path.unlink(missing_ok=True)

        cap = data["capability"]
        self.assertFalse(cap["is_plain"])
        self.assertFalse(cap["is_fixed_size"])
        self.assertFalse(cap["is_bounded"])
        self.assertTrue(cap["contains_heap"])
        self.assertTrue(cap["contains_string"])
        self.assertTrue(cap["contains_unbounded_sequence"])
        self.assertIsNone(cap["max_serialized_size_bytes"])
        for field in data["messages"][0]["fields"]:
            self.assertIsNone(field["capability"]["size_bytes"])
        self.assertEqual(cap["zero_copy"], {"dds_loaned": "no", "iceoryx": "no"})
        self.assertEqual(cap["transports"]["dds_ros2_compatible"], "yes")
        self.assertEqual(cap["transports"]["iceoryx"], "no")
        suggestions = "\n".join(item["suggestion"] for item in cap["suggestions"])
        self.assertIn("fixed-size char array", suggestions)
        self.assertIn("loaned/shared buffer", suggestions)

    def test_json_capability_reports_nested_non_plain_dependency(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            header = root / "std_msgs" / "msg" / "Header.msg"
            stamped = root / "my_pkg" / "msg" / "Stamped.msg"
            header.parent.mkdir(parents=True)
            stamped.parent.mkdir(parents=True)
            header.write_text("string frame_id\n", encoding="utf-8")
            stamped.write_text("std_msgs/Header header\n", encoding="utf-8")

            data = diagnose_interface_json(str(stamped), search_roots=[str(root)])

        cap = data["capability"]
        self.assertTrue(cap["contains_nested_non_plain"])
        self.assertFalse(cap["is_plain"])
        reasons = "\n".join(item["reason"] for item in cap["suggestions"])
        self.assertIn("std_msgs/msg/Header", reasons)

    def test_cli_action_invalid_separator_count_json(self) -> None:
        path = self._write_tmp(".action", "int32 goal\n---\nint32 result\n")
        try:
            result = subprocess.run(
                [sys.executable, str(IBMW_GEN), "diagnose-interface", "--format", "json", str(path)],
                capture_output=True,
                text=True,
                timeout=30,
            )
        finally:
            path.unlink(missing_ok=True)

        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "")
        data = json.loads(result.stderr)
        self.assertFalse(data["ok"])
        self.assertIn("action interface must contain exactly two '---' separators", data["error"])

    def test_same_package_unqualified_ref_resolves(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            point = root / "my_pkg" / "msg" / "Point.msg"
            pose = root / "my_pkg" / "msg" / "Pose.msg"
            point.parent.mkdir(parents=True)
            point.write_text("float64 x\nfloat64 y\n", encoding="utf-8")
            pose.write_text("Point p\n", encoding="utf-8")

            ir = parse_interface_file(str(pose), search_roots=[str(root)], resolve_dependencies=True)
            diagnosis = diagnose_interface(str(pose), search_roots=[str(root)])

        self.assertEqual(ir.dependencies[0].canonical, "my_pkg/msg/Point")
        self.assertTrue(ir.dependencies[0].resolved)
        self.assertTrue(ir.messages[0].is_plain_compatible)
        self.assertIn("my_pkg/msg/Point -> ", diagnosis)

    def test_field_defaults_text_and_json(self) -> None:
        path = self._write_tmp(".msg", "int32 count 5\nstring label hello\n")
        try:
            ir = parse_interface_file(str(path))
            diagnosis = diagnose_interface(str(path))
            json_data = diagnose_interface_json(str(path))
        finally:
            path.unlink(missing_ok=True)

        self.assertEqual(ir.messages[0].fields[0].default, "5")
        self.assertIn("  - int32 count default=5 (builtin)", diagnosis)
        self.assertEqual(json_data["messages"][0]["fields"][0]["default"], "5")
        self.assertEqual(json_data["messages"][0]["fields"][1]["default"], "hello")

    def test_lowercase_constant_name_errors(self) -> None:
        path = self._write_tmp(".msg", "foo=1\n")
        try:
            with self.assertRaisesRegex(InterfaceParseError, "uppercase-style"):
                parse_interface_file(str(path))
        finally:
            path.unlink(missing_ok=True)

    def test_zero_bounds_error(self) -> None:
        for text in ("int32[0] xs\n", "int32[<=0] xs\n", "string<=0 name\n"):
            path = self._write_tmp(".msg", text)
            try:
                with self.assertRaisesRegex(InterfaceParseError, "greater than zero"):
                    parse_interface_file(str(path))
            finally:
                path.unlink(missing_ok=True)

    def test_package_type_path_inference_for_action_layout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "example_interfaces" / "action" / "Fibonacci.action"
            path.parent.mkdir(parents=True)
            path.write_text("int32 order\n---\nint32[] sequence\n---\nint32[] partial_sequence\n", encoding="utf-8")
            ir = parse_interface_file(str(path))

        self.assertEqual(ir.package, "example_interfaces")
        self.assertEqual(ir.type_path, "example_interfaces/action/Fibonacci")

    def test_package_type_path_inference_for_msg_layout(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "my_pkg" / "msg" / "Foo.msg"
            path.parent.mkdir(parents=True)
            path.write_text("int32 value\n", encoding="utf-8")
            ir = parse_interface_file(str(path))
            diagnosis = diagnose_interface(str(path))

        self.assertEqual(ir.package, "my_pkg")
        self.assertEqual(ir.type_path, "my_pkg/msg/Foo")
        self.assertIn("package: my_pkg", diagnosis)
        self.assertIn("type-path: my_pkg/msg/Foo", diagnosis)

    def test_resolved_plain_dependency_makes_parent_plain(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            point = root / "geometry_msgs" / "msg" / "Point.msg"
            pose = root / "my_pkg" / "msg" / "Pose.msg"
            point.parent.mkdir(parents=True)
            pose.parent.mkdir(parents=True)
            point.write_text("float64 x\nfloat64 y\nfloat64 z\n", encoding="utf-8")
            pose.write_text("geometry_msgs/msg/Point position\ngeometry_msgs/Point[2] fixed_points\n", encoding="utf-8")

            ir = parse_interface_file(str(pose), search_roots=[str(root)], resolve_dependencies=True)
            diagnosis = diagnose_interface(str(pose), search_roots=[str(root)])

        self.assertTrue(ir.messages[0].is_plain_compatible)
        self.assertEqual(ir.dependencies[0].canonical, "geometry_msgs/msg/Point")
        self.assertTrue(ir.dependencies[0].resolved)
        self.assertIn("  - geometry_msgs/msg/Point -> ", diagnosis)
        self.assertIn("plain-compatible: yes", diagnosis)

    def test_unresolved_dependency_reported_and_non_plain(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            path = pathlib.Path(tmp) / "my_pkg" / "msg" / "Pose.msg"
            path.parent.mkdir(parents=True)
            path.write_text("std_msgs/Header header\n", encoding="utf-8")

            diagnosis = diagnose_interface(str(path))
            ir = parse_interface_file(str(path))

        self.assertFalse(ir.messages[0].is_plain_compatible)
        self.assertIn("dependencies:\n  - std_msgs/msg/Header (unresolved)", diagnosis)
        self.assertIn("external message reference std_msgs/Header is unresolved", diagnosis)

    def test_cli_search_root_resolves_dependency(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            point = root / "geometry_msgs" / "msg" / "Point.msg"
            pose = root / "my_pkg" / "msg" / "Pose.msg"
            point.parent.mkdir(parents=True)
            pose.parent.mkdir(parents=True)
            point.write_text("float64 x\nfloat64 y\nfloat64 z\n", encoding="utf-8")
            pose.write_text("geometry_msgs/msg/Point position\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    str(IBMW_GEN),
                    "diagnose-interface",
                    "--search-root",
                    str(root),
                    str(pose),
                ],
                capture_output=True,
                text=True,
                timeout=30,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("geometry_msgs/msg/Point -> ", result.stdout)
        self.assertIn("plain-compatible: yes", result.stdout)

    def test_search_root_supports_workspace_src_and_package_roots(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            workspace = pathlib.Path(tmp) / "ws"
            src = workspace / "src"
            point = src / "geometry_msgs" / "msg" / "Point.msg"
            pose = src / "my_pkg" / "msg" / "Pose.msg"
            point.parent.mkdir(parents=True)
            pose.parent.mkdir(parents=True)
            point.write_text("float64 x\nfloat64 y\nfloat64 z\n", encoding="utf-8")
            pose.write_text("geometry_msgs/Point position\n", encoding="utf-8")

            workspace_ir = parse_interface_file(
                str(pose), search_roots=[str(workspace)], resolve_dependencies=True
            )
            package_ir = parse_interface_file(
                str(pose), search_roots=[str(src / "geometry_msgs")], resolve_dependencies=True
            )

        self.assertTrue(workspace_ir.dependencies[0].resolved)
        self.assertTrue(workspace_ir.messages[0].is_plain_compatible)
        self.assertTrue(package_ir.dependencies[0].resolved)
        self.assertTrue(package_ir.messages[0].is_plain_compatible)

    def test_cycle_dependency_does_not_hang_and_is_non_plain(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            a_path = root / "a" / "msg" / "A.msg"
            b_path = root / "b" / "msg" / "B.msg"
            a_path.parent.mkdir(parents=True)
            b_path.parent.mkdir(parents=True)
            a_path.write_text("b/msg/B b\n", encoding="utf-8")
            b_path.write_text("a/msg/A a\n", encoding="utf-8")

            diagnosis = diagnose_interface(str(a_path), search_roots=[str(root)])
            ir = parse_interface_file(str(a_path), search_roots=[str(root)], resolve_dependencies=True)

        self.assertFalse(ir.messages[0].is_plain_compatible)
        self.assertIn("cycle detected", diagnosis)
        self.assertIn("plain-compatible: no", diagnosis)

    def test_cli_action_invalid_separator_count(self) -> None:
        path = self._write_tmp(".action", "int32 goal\n---\nint32 result\n")
        try:
            result = subprocess.run(
                [sys.executable, str(IBMW_GEN), "diagnose-interface", str(path)],
                capture_output=True,
                text=True,
                timeout=30,
            )
        finally:
            path.unlink(missing_ok=True)

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("action interface must contain exactly two '---' separators", result.stderr)

    def test_parser_action_invalid_separator_count(self) -> None:
        path = self._write_tmp(".action", "int32 goal\n---\nint32 result\n")
        try:
            with self.assertRaisesRegex(InterfaceParseError, "exactly two"):
                parse_interface_file(str(path))
        finally:
            path.unlink(missing_ok=True)

    def test_diagnose_action_runtime_contract_is_non_claiming(self) -> None:
        path = self._write_tmp(".action", "int32 order\n---\nbool ok\n---\nint32 progress\n")
        try:
            text = diagnose_interface(str(path))
            data = diagnose_interface_json(str(path))
        finally:
            path.unlink(missing_ok=True)

        self.assertIn("runtime-contract:", text)
        self.assertIn("  status: compile_skeleton", text)
        self.assertIn("  generic-runtime-claimed: no", text)
        self.assertIn("generic goal/result/feedback/status/cancel runtime is not generated", text)
        self.assertEqual(data["runtime_contract"]["status"], "compile_skeleton")
        self.assertFalse(data["runtime_contract"]["generic_runtime_claimed"])
        self.assertFalse(data["runtime_contract"]["runtime_module_generated"])
        self.assertFalse(data["runtime_contract"]["live_runtime_claimed"])
        self.assertEqual(
            data["runtime_contract"]["consumer_contract"],
            "requires_ibmw_action_runtime_and_rosidl_typesupport_cpp",
        )

    def test_diagnosis_text_is_deterministic(self) -> None:
        path = self._write_tmp(".msg", "float32[4] values\n")
        try:
            self.assertEqual(diagnose_interface(str(path)), diagnose_interface(str(path)))
        finally:
            path.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()
