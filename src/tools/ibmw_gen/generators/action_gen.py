#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Experimental private ROS2 Action proof generator for ibmw-gen.

"""Generate private DDS-only Fibonacci Action proof sources."""

import pathlib
from collections.abc import Mapping
from typing import TypedDict, cast

from generators._common import _snake


class _ExperimentalAction(TypedDict):
    name: str
    action: str
    transport: str
    roles: list[str]


class _ActionMeta(TypedDict):
    pkg: str
    name: str
    snake: str
    cpp_root: str
    ts_ns: str
    sample_ns: str


_ROOT = pathlib.Path(__file__).resolve().parents[4]
_SAMPLE_ROOT = _ROOT / "src" / "samples" / "cpp"
_COMMON_DIR = _SAMPLE_ROOT / "ros2_action_fibonacci_common"
_INTERNAL_ACTION_DDS_DIR = _ROOT / "src" / "internal" / "ros2_action_dds"
_PUBLIC_ACTION_DIR = _ROOT / "src" / "api" / "cpp" / "action"
_SERVER_SRC = (
    _SAMPLE_ROOT / "ros2_action_fibonacci_dds_server" / "ros2_server" /
    "fibonacci_action_server.cpp"
)
_CLIENT_SRC = (
    _SAMPLE_ROOT / "ros2_action_fibonacci_dds_client" / "dds_client" /
    "fibonacci_action_client.cpp"
)
_COMMON_HEADERS = [
    "fibonacci_action_common.h",
    "fibonacci_action_fastdds_helpers.h",
    "fibonacci_action_fastdds_typesupport.h",
    "fibonacci_action_traits.h",
    "fibonacci_action_server_runtime.h",
    "fibonacci_action_client_runtime.h",
]
_INTERNAL_ACTION_DDS_HEADERS = [
    "action_wire_contract.h",
    "fastdds_wire_contract.h",
    "fastdds_typesupport.h",
    "action_goal_ledger.h",
    "action_client_observer.h",
]
_PUBLIC_ACTION_HEADERS = [
    "action_log.h",
    "action_traits.h",
    "server.h",
    "server_runtime.h",
    "client.h",
    "client_runtime.h",
]
_MARKER = "IBMW_GEN_EXPERIMENTAL_ACTION_FIBONACCI_DDS_ONLY"


def _copy_text(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8")


def _generated_source(path: pathlib.Path) -> str:
    text = _copy_text(path)
    text = text.replace(
        '#include "../../ros2_action_fibonacci_common/',
        '#include "../common/',
    )
    header_end = text.find("\n\n")
    if header_end == -1:
        return text + f"\n// {_MARKER}\n"
    return text[:header_end + 2] + f"// {_MARKER}\n" + text[header_end + 2:]


def _as_object_mapping(value: object) -> Mapping[str, object] | None:
    if isinstance(value, dict):
        return cast(Mapping[str, object], value)
    return None


def _action_specs(project: Mapping[str, object]) -> list[_ExperimentalAction]:
    actions = project.get("experimental_actions", [])
    if not isinstance(actions, list):
        return []
    action_values = cast(list[object], actions)
    specs: list[_ExperimentalAction] = []
    for action_value in action_values:
        action = _as_object_mapping(action_value)
        if action is None:
            continue
        name = action.get("name")
        transport = action.get("transport")
        action_name = action.get("action")
        roles = action.get("roles")
        typed_roles = cast(list[object], roles) if isinstance(roles, list) else []
        if (isinstance(name, str) and isinstance(action_name, str)
                and isinstance(transport, str) and isinstance(roles, list)
                and all(isinstance(role, str) for role in typed_roles)):
            specs.append({
                "name": name,
                "action": action_name,
                "transport": transport,
                "roles": cast(list[str], typed_roles),
            })
    return specs


# The five action-specific message endpoints every ROS 2 action exposes over
# DDS. Each tuple is (C++ Impl path under <Action>, DDS type-name suffix). The
# DDS type name is <pkg>::action::dds_::<Action>_<suffix>_.
_ACTION_SUBTYPES = [
    ("Impl::SendGoalService::Request", "SendGoal_Request"),
    ("Impl::SendGoalService::Response", "SendGoal_Response"),
    ("Impl::GetResultService::Request", "GetResult_Request"),
    ("Impl::GetResultService::Response", "GetResult_Response"),
    ("Impl::FeedbackMessage", "FeedbackMessage"),
]


def _derive_action_meta(action: str) -> _ActionMeta:
    """Derive code-generation facts from a "<pkg>/action/<Name>" spec."""
    parts = action.split("/")
    if len(parts) != 3 or parts[1] != "action":
        raise ValueError(
            f"experimental action must be '<pkg>/action/<Name>', got: {action!r}"
        )
    pkg, _, name = parts
    snake = _snake(name)
    return {
        "pkg": pkg,
        "name": name,
        "snake": snake,
        "cpp_root": f"{pkg}::action::{name}",
        "ts_ns": f"{pkg}::action::typesupport_fastrtps_cpp",
        "sample_ns": f"ibmw::samples::ros2_action_{snake}",
    }


def _render_typesupport_specialization(meta: _ActionMeta,
                                       impl_path: str,
                                       suffix: str) -> str:
    cpp_type = f"{meta['name']}::{impl_path}"
    dds_name = f"{meta['pkg']}::action::dds_::{meta['name']}_{suffix}_"
    ts_ns = meta["ts_ns"]
    return (
        f"template <>\n"
        f"struct Ros2FastTypeTraits<{cpp_type}> {{\n"
        f'  static constexpr const char* kName = "{dds_name}";\n'
        f"  static bool Serialize(const {cpp_type}& msg, eprosima::fastcdr::Cdr& cdr) {{\n"
        f"    return {ts_ns}::cdr_serialize(msg, cdr);\n"
        f"  }}\n"
        f"  static bool Deserialize(eprosima::fastcdr::Cdr& cdr, {cpp_type}& msg) {{\n"
        f"    return {ts_ns}::cdr_deserialize(cdr, msg);\n"
        f"  }}\n"
        f"  static size_t Size(const {cpp_type}& msg) {{\n"
        f"    return {ts_ns}::get_serialized_size(msg, 0);\n"
        f"  }}\n"
        f"}};\n"
    )


def _render_typesupport_header(meta: _ActionMeta) -> str:
    """Emit <Action> FastDDS typesupport header from metadata (byte-exact).

    Reproduces the hand-written
    ros2_action_<snake>_common/<snake>_action_fastdds_typesupport.h. A golden
    pytest asserts byte-equality so the runtime source of truth and this
    emitter cannot drift.
    """
    upper = meta["snake"].upper()
    guard = (
        f"IBMW_SAMPLES_CPP_ROS2_ACTION_{upper}_COMMON_"
        f"{upper}_ACTION_FASTDDS_TYPESUPPORT_H_"
    )
    specializations = "\n".join(
        _render_typesupport_specialization(meta, impl_path, suffix)
        for impl_path, suffix in _ACTION_SUBTYPES
    )
    return (
        "// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.\n"
        "// All rights reserved.\n"
        "\n"
        f"#ifndef {guard}\n"
        f"#define {guard}\n"
        "\n"
        "#include <cstddef>\n"
        "\n"
        f"#include <{meta['pkg']}/action/detail/{meta['snake']}__rosidl_typesupport_fastrtps_cpp.hpp>\n"
        f"#include <{meta['pkg']}/action/{meta['snake']}.hpp>\n"
        "#include <fastcdr/Cdr.h>\n"
        "\n"
        '#include "internal/ros2_action_dds/fastdds_typesupport.h"\n'
        "\n"
        "namespace ibmw::internal::ros2_action_dds {\n"
        "\n"
        f"using {meta['name']} = {meta['cpp_root']};\n"
        "\n"
        f"{specializations}"
        "\n"
        "}  // namespace ibmw::internal::ros2_action_dds\n"
        "\n"
        f"namespace {meta['sample_ns']} {{\n"
        "\n"
        f"using {meta['name']} = {meta['cpp_root']};\n"
        "namespace action_rtps = ibmw::internal::ros2_action_dds::rtps_compat;\n"
        "\n"
        "using ibmw::internal::ros2_action_dds::kActionCdrType;\n"
        "using ibmw::internal::ros2_action_dds::MakeTypeSupport;\n"
        "using ibmw::internal::ros2_action_dds::Ros2FastTopicDataType;\n"
        "using ibmw::internal::ros2_action_dds::Ros2FastTypeTraits;\n"
        "\n"
        f"}}  // namespace {meta['sample_ns']}\n"
        "\n"
        f"#endif  // {guard}\n"
    )


def generate_experimental_actions(project: Mapping[str, object]) -> list[tuple[str, str]]:
    """Return generated files for the private Fibonacci Action proof lane."""
    results: list[tuple[str, str]] = []
    for action in _action_specs(project):
        if action["name"] != "fibonacci":
            continue
        base = "src/experimental_actions/fibonacci"
        meta = _derive_action_meta(action["action"])
        for header in _COMMON_HEADERS:
            if header == "fibonacci_action_fastdds_typesupport.h":
                content = _render_typesupport_header(meta)
            else:
                content = _copy_text(_COMMON_DIR / header)
            results.append((f"{base}/common/{header}", content))
        for header in _INTERNAL_ACTION_DDS_HEADERS:
            results.append((
                f"{base}/common/internal/ros2_action_dds/{header}",
                _copy_text(_INTERNAL_ACTION_DDS_DIR / header),
            ))
        for header in _PUBLIC_ACTION_HEADERS:
            results.append((
                f"{base}/common/cpp/action/{header}",
                _copy_text(_PUBLIC_ACTION_DIR / header),
            ))
        roles = set(action["roles"])
        if "server" in roles:
            results.append((
                f"{base}/server/fibonacci_action_server.cpp",
                _generated_source(_SERVER_SRC),
            ))
        if "client" in roles:
            results.append((
                f"{base}/client/fibonacci_action_client.cpp",
                _generated_source(_CLIENT_SRC),
            ))
    return results
