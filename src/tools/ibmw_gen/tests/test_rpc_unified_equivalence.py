#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

import pathlib
import sys
from collections.abc import Callable
from importlib import import_module
from typing import Protocol, TypedDict, cast

ROOT = pathlib.Path(__file__).resolve().parents[1]  # ibmw_gen/
sys.path.insert(0, str(ROOT))


class _TypesupportInfo(TypedDict):
    msg_type_cpp: str
    inc_h_content: str
    inc_cc_content: str


class _RpcTypesupportSpec(Protocol):
    emit_rpc_server_typesupport: Callable[[dict[str, object], str], _TypesupportInfo] | None
    emit_rpc_client_typesupport: Callable[[dict[str, object], str], _TypesupportInfo] | None
    rpc_server_markers: list[str]
    rpc_client_markers: list[str]


class _TransportsModule(Protocol):
    def get(self, name: str) -> object | None: ...


def _transports_module() -> _TransportsModule:
    module = import_module("transports")
    return cast(_TransportsModule, cast(object, module))


def _spec(name: str) -> _RpcTypesupportSpec:
    spec = _transports_module().get(name)
    assert spec is not None
    return cast(_RpcTypesupportSpec, spec)


def _resolve_rpc_markerized_typesupport(
    mod: dict[str, object],
    ns: str,
    role: str,
) -> _TypesupportInfo:
    module = import_module("generators.module_gen")
    resolve = cast(
        Callable[[dict[str, object], str, str], _TypesupportInfo],
        getattr(module, "_resolve_rpc_markerized_typesupport"),
    )
    return resolve(mod, ns, role)


def _gen_rpc_server_unified_header(
    mod: dict[str, object],
    ns: str,
    ts_info: _TypesupportInfo,
) -> str:
    module = import_module("generators.module_gen")
    render = cast(
        Callable[[dict[str, object], str, _TypesupportInfo], str],
        getattr(module, "_gen_rpc_server_unified_header"),
    )
    return render(mod, ns, ts_info)


def _gen_rpc_server_unified_cc(
    mod: dict[str, object],
    ns: str,
    snake_dir: str,
    ts_info: _TypesupportInfo,
) -> str:
    module = import_module("generators.module_gen")
    render = cast(
        Callable[[dict[str, object], str, str, _TypesupportInfo], str],
        getattr(module, "_gen_rpc_server_unified_cc"),
    )
    return render(mod, ns, snake_dir, ts_info)


def _gen_rpc_client_unified_header(
    mod: dict[str, object],
    ns: str,
    ts_info: _TypesupportInfo,
) -> str:
    module = import_module("generators.module_gen")
    render = cast(
        Callable[[dict[str, object], str, _TypesupportInfo], str],
        getattr(module, "_gen_rpc_client_unified_header"),
    )
    return render(mod, ns, ts_info)


def _gen_rpc_client_unified_cc(
    mod: dict[str, object],
    ns: str,
    snake_dir: str,
    ts_info: _TypesupportInfo,
) -> str:
    module = import_module("generators.module_gen")
    render = cast(
        Callable[[dict[str, object], str, str, _TypesupportInfo], str],
        getattr(module, "_gen_rpc_client_unified_cc"),
    )
    return render(mod, ns, snake_dir, ts_info)


def _generate_modules(project: dict[str, object]) -> list[tuple[str, str]]:
    module = import_module("generators.module_gen")
    render = cast(
        Callable[[dict[str, object]], list[tuple[str, str]]],
        getattr(module, "generate_modules"),
    )
    return render(project)


def _server_inc_h_by_transport() -> dict[str, str]:
    dds_mod: dict[str, object] = {
        "name": "demo_rpc_server",
        "type": "rpc_server",
        "transport": "dds",
        "_dds_namespace_cpp": "ibmw::protocols::demo",
        "_rpc_header": "demo_rpc.ibmw_rpc.dds.h",
        "_dds_service_info": {
            "name": "CalculatorService",
            "methods": [{
                "name": "Add",
                "request": "Add_Request",
                "response": "Add_Response",
            }],
        },
    }
    ros2_mod: dict[str, object] = {
        "name": "demo_rpc_server",
        "type": "rpc_server",
        "transport": "ros2",
        "ros2_package": "example_ros2",
        "ros2_service": "RosTestRpc",
        "_has_ros2_service_info": True,
    }
    json_mod: dict[str, object] = {
        "name": "demo_json_rpc_server",
        "type": "rpc_server",
        "transport": "json",
        "service_name": "DemoService",
        "methods": [{
            "name": "Add",
            "request": "demo::Req",
            "response": "demo::Resp",
        }],
    }

    dds_spec = _spec("dds")
    ros2_spec = _spec("ros2")
    json_spec = _spec("json")
    assert dds_spec.emit_rpc_server_typesupport is not None
    assert ros2_spec.emit_rpc_server_typesupport is not None
    assert json_spec.emit_rpc_server_typesupport is not None

    return {
        "dds": dds_spec.emit_rpc_server_typesupport(dds_mod, "ibmw::demo")["inc_h_content"],
        "ros2": ros2_spec.emit_rpc_server_typesupport(ros2_mod, "ibmw::demo")["inc_h_content"],
        "json": json_spec.emit_rpc_server_typesupport(json_mod, "ibmw::demo")["inc_h_content"],
    }


def _client_inc_h_by_transport() -> dict[str, str]:
    dds_mod: dict[str, object] = {
        "name": "demo_rpc_client",
        "type": "rpc_client",
        "transport": "dds",
        "_dds_namespace_cpp": "ibmw::protocols::demo",
        "_rpc_header": "demo_rpc.ibmw_rpc.dds.h",
        "_dds_service_info": {
            "name": "CalculatorService",
            "methods": [{
                "name": "Add",
                "request": "Add_Request",
                "response": "Add_Response",
            }],
        },
    }
    ros2_mod: dict[str, object] = {
        "name": "demo_rpc_client",
        "type": "rpc_client",
        "transport": "ros2",
        "ros2_package": "example_ros2",
        "ros2_service": "RosTestRpc",
        "_has_ros2_service_info": True,
    }
    json_mod: dict[str, object] = {
        "name": "demo_json_rpc_client",
        "type": "rpc_client",
        "transport": "json",
        "service_name": "DemoService",
        "methods": [{
            "name": "Add",
            "request": "demo::Req",
            "response": "demo::Resp",
        }],
    }

    dds_spec = _spec("dds")
    ros2_spec = _spec("ros2")
    json_spec = _spec("json")
    assert dds_spec.emit_rpc_client_typesupport is not None
    assert ros2_spec.emit_rpc_client_typesupport is not None
    assert json_spec.emit_rpc_client_typesupport is not None

    return {
        "dds": dds_spec.emit_rpc_client_typesupport(dds_mod, "ibmw::demo")["inc_h_content"],
        "ros2": ros2_spec.emit_rpc_client_typesupport(ros2_mod, "ibmw::demo")["inc_h_content"],
        "json": json_spec.emit_rpc_client_typesupport(json_mod, "ibmw::demo")["inc_h_content"],
    }


def _transport_cases() -> dict[str, dict[str, dict[str, object]]]:
    return {
        "dds": {
            "server": {
                "name": "demo_rpc_server",
                "type": "rpc_server",
                "transport": "dds",
                "_dds_namespace_cpp": "ibmw::protocols::demo",
                "_rpc_header": "demo_rpc.ibmw_rpc.dds.h",
                "_dds_service_info": {
                    "name": "CalculatorService",
                    "methods": [{
                        "name": "Add",
                        "request": "Add_Request",
                        "response": "Add_Response",
                    }],
                },
            },
            "client": {
                "name": "demo_rpc_client",
                "type": "rpc_client",
                "transport": "dds",
                "_dds_namespace_cpp": "ibmw::protocols::demo",
                "_rpc_header": "demo_rpc.ibmw_rpc.dds.h",
                "_dds_service_info": {
                    "name": "CalculatorService",
                    "methods": [{
                        "name": "Add",
                        "request": "Add_Request",
                        "response": "Add_Response",
                    }],
                },
            },
        },
        "ros2": {
            "server": {
                "name": "demo_rpc_server",
                "type": "rpc_server",
                "transport": "ros2",
                "ros2_package": "example_ros2",
                "ros2_service": "RosTestRpc",
                "_has_ros2_service_info": True,
            },
            "client": {
                "name": "demo_rpc_client",
                "type": "rpc_client",
                "transport": "ros2",
                "ros2_package": "example_ros2",
                "ros2_service": "RosTestRpc",
                "_has_ros2_service_info": True,
            },
        },
        "json": {
            "server": {
                "name": "demo_json_rpc_server",
                "type": "rpc_server",
                "transport": "json",
                "service_name": "DemoService",
                "methods": [{
                    "name": "Add",
                    "request": "demo::Req",
                    "response": "demo::Resp",
                }],
            },
            "client": {
                "name": "demo_json_rpc_client",
                "type": "rpc_client",
                "transport": "json",
                "service_name": "DemoService",
                "methods": [{
                    "name": "Add",
                    "request": "demo::Req",
                    "response": "demo::Resp",
                }],
            },
        },
    }


def test_macro_name_parity_across_transports() -> None:
    server_inc_h = _server_inc_h_by_transport()
    client_inc_h = _client_inc_h_by_transport()

    for transport, inc_h in server_inc_h.items():
        assert "#define IBMW_TS_RPC_SERVER_SETUP(" in inc_h, (
            f"{transport} server missing SETUP macro"
        )
        assert "#define IBMW_TS_RPC_SERVER_HANDLER_DECL" in inc_h, (
            f"{transport} server missing HANDLER_DECL macro"
        )
        assert "#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(" in inc_h, (
            f"{transport} server missing HANDLER_IMPL macro"
        )

    for transport, inc_h in client_inc_h.items():
        assert "#define IBMW_TS_RPC_CLIENT_LOOP_DECL" in inc_h, (
            f"{transport} client missing LOOP_DECL macro"
        )
        assert "#define IBMW_TS_RPC_CLIENT_LOOP_IMPL(" in inc_h, (
            f"{transport} client missing LOOP_IMPL macro"
        )


def test_client_loop_expansion_diverges_by_transport() -> None:
    client_inc_h = _client_inc_h_by_transport()

    assert "co_await" in client_inc_h["dds"]
    assert "std::this_thread::sleep_for" in client_inc_h["ros2"]
    assert "co_await" not in client_inc_h["ros2"]
    assert "std::this_thread::sleep_for" in client_inc_h["json"]
    assert "co_await" not in client_inc_h["json"]


def _module_output(paths: list[tuple[str, str]]) -> dict[str, str]:
    return {path: content for path, content in paths}


def test_unified_rpc_dispatch_emits_marker_driven_shells() -> None:
    project: dict[str, object] = {
        "project": {"_namespace_cpp": "ibmw::demo"},
        "modules": [
            {
                "name": "demo_rpc_server",
                "type": "rpc_server",
                "transport": "dds",
                "_dds_namespace_cpp": "ibmw::protocols::demo",
                "_rpc_header": "demo_rpc.ibmw_rpc.dds.h",
                "_dds_service_info": {
                    "name": "CalculatorService",
                    "methods": [{
                        "name": "Add",
                        "request": "Add_Request",
                        "response": "Add_Response",
                    }],
                },
            },
            {
                "name": "demo_rpc_client",
                "type": "rpc_client",
                "transport": "ros2",
                "ros2_package": "example_ros2",
                "ros2_service": "RosTestRpc",
                "_has_ros2_service_info": True,
            },
        ],
    }

    out = _module_output(_generate_modules(project))

    server_h = out["src/demo_rpc_server_module/demo_rpc_server.h"]
    server_cc = out["src/demo_rpc_server_module/demo_rpc_server.cc"]
    server_inc_cc = out["src/demo_rpc_server_module/demo_rpc_server_typesupport.inc.cc"]
    client_h = out["src/demo_rpc_client_module/demo_rpc_client.h"]
    client_cc = out["src/demo_rpc_client_module/demo_rpc_client.cc"]
    client_inc_h = out["src/demo_rpc_client_module/demo_rpc_client_typesupport.inc.h"]
    client_inc_cc = out["src/demo_rpc_client_module/demo_rpc_client_typesupport.inc.cc"]

    assert "demo_rpc_server_typesupport.inc.h" in server_h
    assert "IBMW_TS_RPC_SERVER_HANDLER_DECL" in server_h
    assert "IBMW_TS_RPC_SERVER_SETUP(core);" in server_cc
    assert "IBMW_TS_RPC_SERVER_HANDLER_IMPL(demo_rpc_server)" in server_cc
    assert "// IBMW_RPC_SERVER_HANDLER_BODY_ADD_BEGIN" in server_inc_cc

    assert "demo_rpc_client_typesupport.inc.h" in client_h
    assert "IBMW_TS_RPC_CLIENT_LOOP_DECL" in client_h
    assert "IBMW_TS_RPC_CLIENT_LOOP_IMPL(demo_rpc_client)" in client_cc
    assert "void RpcLoop();" in client_inc_h
    assert "std::thread([this]() { RpcLoop(); }).detach();" not in client_cc
    assert "loop_thread_ = std::thread([this]() { RpcLoop(); });" in client_cc
    assert "// IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_ROS_TEST_RPC_BEGIN" in client_inc_cc


def test_canonical_rpc_marker_names_appear_in_unified_outputs_across_transports() -> None:
    ns = "ibmw::demo"
    expected_server_markers = ["IBMW_RPC_SERVER_HANDLER_BODY"]
    expected_client_markers = [
        "IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD",
        "IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME",
        "IBMW_RPC_CLIENT_RETURN_STATUS_HANDLING",
    ]

    for transport, roles in _transport_cases().items():
        spec = _spec(transport)
        assert spec.rpc_server_markers == expected_server_markers
        assert spec.rpc_client_markers == expected_client_markers

        server_mod = roles["server"]
        server_ts = _resolve_rpc_markerized_typesupport(server_mod, ns, "server")
        server_output = "\n".join([
            _gen_rpc_server_unified_header(server_mod, ns, server_ts),
            _gen_rpc_server_unified_cc(server_mod, ns, "src/demo_rpc_server_module", server_ts),
            server_ts["inc_h_content"],
            server_ts["inc_cc_content"],
        ])
        for marker in spec.rpc_server_markers:
            assert marker in server_output, (
                f"{transport} rpc_server unified output missing canonical marker {marker}"
            )

        client_mod = roles["client"]
        client_ts = _resolve_rpc_markerized_typesupport(client_mod, ns, "client")
        client_output = "\n".join([
            _gen_rpc_client_unified_header(client_mod, ns, client_ts),
            _gen_rpc_client_unified_cc(client_mod, ns, "src/demo_rpc_client_module", client_ts),
            client_ts["inc_h_content"],
            client_ts["inc_cc_content"],
        ])
        for marker in spec.rpc_client_markers:
            assert marker in client_output, (
                f"{transport} rpc_client unified output missing canonical marker {marker}"
            )
