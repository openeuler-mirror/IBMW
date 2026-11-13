#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

"""Minimal pure-Python ROS 2 interface parser and diagnoser for ibmw-gen."""

from __future__ import annotations

from dataclasses import dataclass, field
import json
import os
import re
import shutil
import xml.etree.ElementTree as ET


class InterfaceParseError(ValueError):
    """Raised when a ROS 2 interface file cannot be parsed."""


_FIELD_RE = re.compile(r"^(?P<type>\S+)\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)(?:\s+(?P<default>.+))?$")
_CONST_RE = re.compile(
    r"^(?P<type>\S+)\s+(?P<name>[A-Z][A-Z0-9_]*)\s*=\s*(?P<value>.*)$"
)
_ARRAY_RE = re.compile(r"^(?P<base>.+)\[(?P<bound><=\d+|\d*)\]$")
_BOUNDED_STRING_RE = re.compile(r"^(?P<base>w?string)<=(?P<bound>\d+)$")
_CONST_NAME_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")
_CONST_ASSIGN_RE = re.compile(r"(?<!<)=")

_PRIMITIVE_NUMERIC_BOOL = {
    "bool",
    "byte",
    "char",
    "float32",
    "float64",
    "int8",
    "uint8",
    "int16",
    "uint16",
    "int32",
    "uint32",
    "int64",
    "uint64",
}
_PRIMITIVE_SERIALIZED_SIZE_BYTES = {
    "bool": 1,
    "byte": 1,
    "char": 1,
    "int8": 1,
    "uint8": 1,
    "int16": 2,
    "uint16": 2,
    "int32": 4,
    "uint32": 4,
    "float32": 4,
    "int64": 8,
    "uint64": 8,
    "float64": 8,
}
_BUILTIN_PRIMITIVES = _PRIMITIVE_NUMERIC_BOOL | {"string", "wstring"}
_ROS2_COMPAT_PACKAGES = (
    "rosidl_generator_cpp",
    "rosidl_runtime_c",
    "rosidl_runtime_cpp",
    "rosidl_typesupport_fastrtps_c",
    "rosidl_typesupport_fastrtps_cpp",
    "fastrtps",
    "fastcdr",
)
_KNOWN_GOOD_ROS2_COMPATIBILITY: tuple[dict, ...] = (
    {
        "name": "humble-rosidl-3.1.8-fastrtps-2.2.4-fastdds-2.6.11-fastcdr-1.0.29",
        "distro": "humble",
        "packages": {
            "rosidl_generator_cpp": "3.1.8",
            "rosidl_runtime_c": "3.1.8",
            "rosidl_runtime_cpp": "3.1.8",
            "rosidl_typesupport_fastrtps_c": "2.2.4",
            "rosidl_typesupport_fastrtps_cpp": "2.2.4",
            "fastrtps": "2.6.11",
            "fastcdr": "1.0.29",
        },
        "validation": {
            "ros2_distro": "humble",
            "rmw": "rmw_fastrtps_cpp",
            "std_msgs_string_pubsub": "live_pass",
            "add_two_ints_service": "live_pass",
            "route_b_artifact_audit": "pass",
        },
    },
)


@dataclass(frozen=True)
class Ros2Constant:
    type: str
    name: str
    value: str


@dataclass(frozen=True)
class Ros2Field:
    type: str
    name: str
    default: str | None = None
    dependency: "Ros2Dependency | None" = None

    @property
    def base_type(self) -> str:
        parsed = _parse_array_type(self.type)
        return parsed[0]

    @property
    def array_kind(self) -> str | None:
        parsed = _parse_array_type(self.type)
        return parsed[1]

    @property
    def is_external_ref(self) -> bool:
        return _is_external_type(self.base_type)

    @property
    def plain_reason(self) -> str | None:
        base = self.base_type
        array_kind = self.array_kind
        if self.is_external_ref:
            if array_kind == "unbounded":
                return f"field {self.name} is an unbounded sequence"
            if array_kind == "bounded":
                return f"field {self.name} is a bounded sequence"
            if self.dependency is None or not self.dependency.resolved:
                return f"external message reference {base} is unresolved for plain analysis"
            if self.dependency.kind != "msg":
                return f"external {self.dependency.kind} reference {self.dependency.canonical} is unsupported for plain analysis"
            if not self.dependency.plain_compatible:
                return (
                    f"field {self.name} references nested dependency "
                    f"{self.dependency.canonical}: {self.dependency.reason}"
                )
            if array_kind in (None, "fixed"):
                return None
            return f"field {self.name} has unsupported array form {self.type}"
        if base in ("string", "wstring") or _BOUNDED_STRING_RE.match(base):
            return f"field {self.name} uses string type {self.type}"
        if base not in _PRIMITIVE_NUMERIC_BOOL:
            return f"field {self.name} uses non-primitive type {self.type}"
        if array_kind in (None, "fixed"):
            return None
        if array_kind == "unbounded":
            return f"field {self.name} is an unbounded sequence"
        if array_kind == "bounded":
            return f"field {self.name} is a bounded sequence"
        return f"field {self.name} has unsupported array form {self.type}"


@dataclass(frozen=True)
class Ros2Dependency:
    ref: str
    canonical: str
    kind: str
    path: str | None = None
    resolved: bool = False
    plain_compatible: bool = False
    reason: str = "unresolved"
    nested_dependencies: tuple["Ros2Dependency", ...] = ()
    serialized_size_bytes: int | None = None


@dataclass(frozen=True)
class Ros2MessageIR:
    name: str
    fields: list[Ros2Field] = field(default_factory=list)
    constants: list[Ros2Constant] = field(default_factory=list)

    @property
    def external_refs(self) -> list[str]:
        refs = sorted({f.base_type for f in self.fields if f.is_external_ref})
        return refs

    @property
    def plain_reasons(self) -> list[str]:
        return [reason for f in self.fields if (reason := f.plain_reason) is not None]

    @property
    def is_plain_compatible(self) -> bool:
        return not self.plain_reasons


@dataclass(frozen=True)
class Ros2InterfaceIR:
    kind: str
    name: str
    package: str
    type_path: str
    messages: list[Ros2MessageIR]
    dependencies: list[Ros2Dependency] = field(default_factory=list)


def parse_interface_file(
    path: str,
    search_roots: list[str] | None = None,
    resolve_dependencies: bool = False,
) -> Ros2InterfaceIR:
    """Parse a local .msg, .srv, or .action path into a small diagnostic IR."""
    return _parse_interface_file(
        path,
        list(search_roots or []),
        resolve_dependencies,
        stack=[],
        cache={},
    )


def _parse_interface_file(
    path: str,
    search_roots: list[str],
    resolve_dependencies: bool,
    stack: list[str],
    cache: dict[str, Ros2Dependency],
) -> Ros2InterfaceIR:
    ext = os.path.splitext(path)[1]
    name = os.path.splitext(os.path.basename(path))[0]
    if ext not in (".msg", ".srv", ".action"):
        raise InterfaceParseError(f"unsupported interface extension: {ext or '<none>'}")
    kind = ext[1:]
    package, type_path = _infer_package_type_path(path, kind, name)
    try:
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
    except OSError as exc:
        raise InterfaceParseError(f"failed to read interface file: {exc}") from exc

    if ext == ".msg":
        ir = Ros2InterfaceIR(kind, name, package, type_path, [_parse_message_text(name, text)])
        return _attach_dependencies(ir, search_roots, resolve_dependencies, stack, cache)

    srv_parts = _split_srv_text(text)
    if ext == ".srv":
        if len(srv_parts) != 2:
            raise InterfaceParseError("srv interface must contain exactly one '---' separator")
        request_text, response_text = srv_parts
        ir = Ros2InterfaceIR(
            kind,
            name,
            package,
            type_path,
            [
                _parse_message_text(f"{name}_Request", request_text),
                _parse_message_text(f"{name}_Response", response_text),
            ],
        )
        return _attach_dependencies(ir, search_roots, resolve_dependencies, stack, cache)

    action_parts = srv_parts
    if len(action_parts) != 3:
        raise InterfaceParseError("action interface must contain exactly two '---' separators")
    goal_text, result_text, feedback_text = action_parts
    ir = Ros2InterfaceIR(
        kind,
        name,
        package,
        type_path,
        [
            _parse_message_text(f"{name}_Goal", goal_text),
            _parse_message_text(f"{name}_Result", result_text),
            _parse_message_text(f"{name}_Feedback", feedback_text),
        ],
    )
    return _attach_dependencies(ir, search_roots, resolve_dependencies, stack, cache)


def diagnose_interface(
    path: str,
    search_roots: list[str] | None = None,
    ros2_prefix: str | None = None,
    ros2_distro: str | None = None,
    host_tools_prefix: str | None = None,
    target_runtime_prefix: str | None = None,
    target_runtime_distro: str | None = None,
    target_artifacts: list[str] | None = None,
    forbidden_prefixes: list[str] | None = None,
    expected_target_arch: str | None = None,
) -> str:
    """Return deterministic text diagnosis for a local ROS 2 interface file."""
    roots = list(search_roots or [])
    ir = parse_interface_file(path, search_roots=roots, resolve_dependencies=bool(roots))
    lines = [f"interface: {ir.kind} {ir.name}", f"package: {ir.package}", f"type-path: {ir.type_path}"]
    compatibility = collect_ros2_compatibility(
        ros2_prefix=ros2_prefix,
        ros2_distro=ros2_distro,
        host_tools_prefix=host_tools_prefix,
        target_runtime_prefix=target_runtime_prefix,
        target_runtime_distro=target_runtime_distro,
    )
    artifact_audit = audit_target_artifacts(
        target_artifacts,
        forbidden_prefixes=_default_forbidden_prefixes(forbidden_prefixes, host_tools_prefix or ros2_prefix),
        expected_arch=expected_target_arch,
    )
    lines.append("ros2-compatibility:")
    lines.append(f"  distro: {compatibility['distro']}")
    lines.append(f"  target-distro: {compatibility['target_runtime']['distro']}")
    lines.append(f"  status: {compatibility['status']}")
    lines.append(f"  boundary-status: {compatibility['boundary_status']}")
    lines.append(f"  policy: {compatibility['policy']}")
    lines.append(f"  known-good: {compatibility['known_good']}")
    lines.append("  packages:")
    if compatibility["packages"]:
        for package in compatibility["packages"]:
            version = package["version"] if package["version"] is not None else "<missing>"
            lines.append(f"    - {package['name']}: {version}")
    else:
        lines.append("    - <none>")
    lines.append("target-artifact-audit:")
    lines.append(f"  status: {artifact_audit['status']}")
    lines.append(f"  expected-arch: {artifact_audit['expected_arch']}")
    lines.append("  artifacts:")
    if artifact_audit["artifacts"]:
        for artifact in artifact_audit["artifacts"]:
            lines.append(f"    - {artifact['path']}: {artifact['status']} arch={artifact['arch']}")
    else:
        lines.append("    - <none>")
    runtime_contract = _interface_runtime_contract(ir)
    lines.append("runtime-contract:")
    lines.append(f"  status: {runtime_contract['status']}")
    lines.append(f"  generic-runtime-claimed: {'yes' if runtime_contract['generic_runtime_claimed'] else 'no'}")
    lines.append(f"  runtime-module-generated: {'yes' if runtime_contract['runtime_module_generated'] else 'no'}")
    lines.append(f"  consumer-contract: {runtime_contract['consumer_contract']}")
    lines.append("  notes:")
    for note in runtime_contract["notes"]:
        lines.append(f"    - {note}")
    lines.append("dependencies:")
    if ir.dependencies:
        for dep in ir.dependencies:
            if dep.resolved:
                lines.append(f"  - {dep.canonical} -> {dep.path} (resolved)")
            else:
                lines.append(f"  - {dep.canonical} (unresolved)")
    else:
        lines.append("  - <none>")
    for message in ir.messages:
        lines.append(f"message: {message.name}")
        lines.append("constants:")
        if message.constants:
            for constant in message.constants:
                lines.append(f"  - {constant.type} {constant.name}={constant.value}")
        else:
            lines.append("  - <none>")
        lines.append("fields:")
        if message.fields:
            for field in message.fields:
                marker = "external" if field.is_external_ref else "builtin"
                default = f" default={field.default}" if field.default is not None else ""
                lines.append(f"  - {field.type} {field.name}{default} ({marker})")
        else:
            lines.append("  - <none>")
        lines.append("external refs:")
        if message.external_refs:
            for ref in message.external_refs:
                lines.append(f"  - {ref}")
        else:
            lines.append("  - <none>")
        lines.append(f"plain-compatible: {'yes' if message.is_plain_compatible else 'no'}")
        lines.append("reasons:")
        if message.plain_reasons:
            for reason in message.plain_reasons:
                lines.append(f"  - {reason}")
        else:
            lines.append("  - <none>")
    return "\n".join(lines) + "\n"


def diagnose_interface_json(
    path: str,
    search_roots: list[str] | None = None,
    ros2_prefix: str | None = None,
    ros2_distro: str | None = None,
    host_tools_prefix: str | None = None,
    target_runtime_prefix: str | None = None,
    target_runtime_distro: str | None = None,
    target_artifacts: list[str] | None = None,
    forbidden_prefixes: list[str] | None = None,
    expected_target_arch: str | None = None,
) -> dict:
    """Return deterministic JSON-serializable diagnosis for a local ROS 2 interface file."""
    roots = list(search_roots or [])
    ir = parse_interface_file(path, search_roots=roots, resolve_dependencies=bool(roots))
    artifact_audit = audit_target_artifacts(
        target_artifacts,
        forbidden_prefixes=_default_forbidden_prefixes(forbidden_prefixes, host_tools_prefix or ros2_prefix),
        expected_arch=expected_target_arch,
    )
    return _interface_to_json_dict(
        ir,
        collect_ros2_compatibility(
            ros2_prefix=ros2_prefix,
            ros2_distro=ros2_distro,
            host_tools_prefix=host_tools_prefix,
            target_runtime_prefix=target_runtime_prefix,
            target_runtime_distro=target_runtime_distro,
        ),
        artifact_audit,
    )


def _interface_to_json_dict(ir: Ros2InterfaceIR, ros2_compatibility: dict, target_artifact_audit: dict) -> dict:
    messages = []
    for message in ir.messages:
        capability = _message_capability(message)
        messages.append(
            {
                "name": message.name,
                "constants": [
                    {"type": c.type, "name": c.name, "value": c.value}
                    for c in message.constants
                ],
                "fields": [
                    {
                        "type": f.type,
                        "name": f.name,
                        "default": f.default,
                        "classification": "external" if f.is_external_ref else "builtin",
                        "dependency": f.dependency.canonical if f.dependency is not None else None,
                        "capability": _field_capability(f),
                    }
                    for f in message.fields
                ],
                "external_refs": message.external_refs,
                "plain_compatible": message.is_plain_compatible,
                "reasons": message.plain_reasons,
                "capability": capability,
            }
        )
    interface_capability = _interface_capability(messages)
    return {
        "ok": True,
        "schema_version": 1,
        "interface": {
            "kind": ir.kind,
            "name": ir.name,
            "package": ir.package,
            "type_path": ir.type_path,
        },
        "ros2_compatibility": ros2_compatibility,
        "target_artifact_audit": target_artifact_audit,
        "runtime_contract": _interface_runtime_contract(ir),
        "dependencies": [
            {
                "ref": d.ref,
                "canonical": d.canonical,
                "kind": d.kind,
                "path": d.path,
                "resolved": d.resolved,
                "plain_compatible": d.plain_compatible,
                "serialized_size_bytes": d.serialized_size_bytes,
                "reason": d.reason,
            }
            for d in ir.dependencies
        ],
        "messages": messages,
        "external_refs": sorted({ref for message in ir.messages for ref in message.external_refs}),
        "plain_compatible": all(message.is_plain_compatible for message in ir.messages),
        "reasons": [reason for message in ir.messages for reason in message.plain_reasons],
        "capability": interface_capability,
    }


def _interface_runtime_contract(ir: Ros2InterfaceIR) -> dict:
    if ir.kind == "action":
        return {
            "status": "compile_skeleton",
            "generic_runtime_claimed": False,
            "runtime_module_generated": False,
            "live_runtime_claimed": False,
            "consumer_contract": "requires_ibmw_action_runtime_and_rosidl_typesupport_cpp",
            "notes": [
                "diagnosis covers parse/dependency/metadata readiness only",
                "generic goal/result/feedback/status/cancel runtime is not generated",
                "use generated action traits and binding skeleton as compile-time integration points",
            ],
        }
    return {
        "status": "metadata_only",
        "generic_runtime_claimed": False,
        "runtime_module_generated": False,
        "live_runtime_claimed": False,
        "consumer_contract": "not_applicable",
        "notes": ["diagnosis does not generate runtime glue"],
    }


def collect_ros2_compatibility(
    ros2_prefix: str | None = None,
    ros2_distro: str | None = None,
    host_tools_prefix: str | None = None,
    target_runtime_prefix: str | None = None,
    target_runtime_distro: str | None = None,
) -> dict:
    """Collect a deterministic, pure-Python ROS2 compatibility report.

    This intentionally avoids importing ROS2 Python modules or invoking `ros2`.
    When a prefix is supplied it reads package versions from
    `<prefix>/share/<package>/package.xml`.
    """
    host_prefix = host_tools_prefix or ros2_prefix
    target_prefix = target_runtime_prefix or ros2_prefix
    host_distro = ros2_distro or os.environ.get("ROS_DISTRO") or "<unknown>"
    target_distro = target_runtime_distro or host_distro
    host = _collect_ros2_compatibility_layer("host_tools", host_prefix, host_distro)
    target = _collect_ros2_compatibility_layer("target_runtime", target_prefix, target_distro)
    packages = target["packages"] if target_prefix else host["packages"]
    missing = sorted(set(host["missing_packages"] + target["missing_packages"]))
    if not host_prefix and not target_prefix:
        status = "not_requested"
    elif host["status"] == "complete" and target["status"] == "complete":
        status = "complete"
    else:
        status = "incomplete"
    policy = "humble_only_v1_exact_versions"
    matrix_match = _match_known_good_matrix(target_distro, target["packages"])
    matrix_status = "matched" if matrix_match is not None else "unknown_combo"
    known_good = "yes" if matrix_match is not None else "no"
    boundary = _compatibility_boundary_status(host, target)
    reasons = []
    if status == "not_requested":
        reasons.append("no ROS2 prefix supplied; exact version lock not collected")
    if host_distro != "humble":
        reasons.append(f"host tools distro {host_distro} is outside the current Humble-only v1 known-good policy")
    if target_distro != "humble":
        reasons.append(f"target runtime distro {target_distro} is outside the current Humble-only v1 known-good policy")
    if boundary["status"] != "ok":
        reasons.extend(boundary["reasons"])
    if missing:
        reasons.append("missing package versions: " + ", ".join(missing))
    if status == "complete" and matrix_match is None:
        reasons.append("exact ROS2/FastDDS/FastCDR version combo is not in the known-good matrix")
    if matrix_match is not None:
        reasons.append(f"matched known-good matrix entry: {matrix_match}")
    return {
        "distro": host_distro,
        "mode": "reference_existing" if (host_prefix or target_prefix) else "not_selected",
        "prefix": os.path.abspath(ros2_prefix) if ros2_prefix else None,
        "host_tools": host,
        "target_runtime": target,
        "boundary_status": boundary["status"],
        "boundary_reasons": boundary["reasons"],
        "policy": policy,
        "status": status,
        "matrix_status": matrix_status,
        "known_good": known_good,
        "packages": packages,
        "missing_packages": missing,
        "reasons": reasons,
    }


def _collect_ros2_compatibility_layer(role: str, prefix: str | None, distro: str) -> dict:
    packages = []
    if prefix:
        for package_name in _ROS2_COMPAT_PACKAGES:
            version, source = _read_ros2_package_version(prefix, package_name)
            packages.append(
                {
                    "name": package_name,
                    "version": version,
                    "package_xml": source if source and source.endswith("package.xml") else None,
                    "version_source": source,
                    "found": version is not None,
                }
            )
    missing = [package["name"] for package in packages if not package["found"]]
    if not prefix:
        status = "not_requested"
    elif missing:
        status = "incomplete"
    else:
        status = "complete"
    return {
        "role": role,
        "distro": distro,
        "prefix": os.path.abspath(prefix) if prefix else None,
        "status": status,
        "packages": packages,
        "missing_packages": missing,
    }


def _compatibility_boundary_status(host: dict, target: dict) -> dict:
    reasons = []
    if host["status"] == "not_requested" and target["status"] == "not_requested":
        return {"status": "not_checked", "reasons": []}
    if host["distro"] != target["distro"]:
        reasons.append(f"host tools distro {host['distro']} != target runtime distro {target['distro']}")
    if host["status"] == "not_requested":
        reasons.append("host tools prefix not supplied")
    if target["status"] == "not_requested":
        reasons.append("target runtime prefix not supplied")
    if host["status"] == "incomplete":
        reasons.append("host tools compatibility versions are incomplete")
    if target["status"] == "incomplete":
        reasons.append("target runtime compatibility versions are incomplete")
    if reasons:
        return {"status": "mismatch", "reasons": reasons}
    return {"status": "ok", "reasons": []}


def build_ros2_compatibility_lock(compatibility: dict, target_artifact_audit: dict | None = None) -> dict:
    """Build deterministic ros2_compatibility.lock.yaml payload data."""
    return {
        "ros2_compatibility_lock": {
            "schema_version": 1,
            "policy": compatibility["policy"],
            "status": compatibility["status"],
            "matrix_status": compatibility["matrix_status"],
            "known_good": compatibility["known_good"],
            "distro": compatibility["distro"],
            "mode": compatibility["mode"],
            "prefix": compatibility["prefix"],
            "host_tools": compatibility["host_tools"],
            "target_runtime": compatibility["target_runtime"],
            "boundary_status": compatibility["boundary_status"],
            "boundary_reasons": compatibility["boundary_reasons"],
            "target_artifact_audit": target_artifact_audit or audit_target_artifacts(),
            "packages": [
                {
                    "name": package["name"],
                    "version": package["version"],
                    "found": package["found"],
                    "package_xml": package["package_xml"],
                    "version_source": package["version_source"],
                }
                for package in compatibility["packages"]
            ],
            "missing_packages": compatibility["missing_packages"],
            "reasons": compatibility["reasons"],
        }
    }


def audit_target_artifacts(
    artifact_paths: list[str] | None = None,
    forbidden_prefixes: list[str] | None = None,
    expected_arch: str | None = None,
) -> dict:
    """Audit target artifacts without external tools.

    Checks are intentionally small and deterministic for cross-compile smoke:
    - binary/text payload must not contain forbidden host prefixes
    - ELF e_machine must match expected_arch when requested
    """
    paths = list(artifact_paths or [])
    prefixes = [p for p in (forbidden_prefixes or []) if p]
    if not paths:
        return {
            "status": "not_requested",
            "expected_arch": expected_arch,
            "forbidden_prefixes": prefixes,
            "artifacts": [],
            "reasons": [],
        }
    artifacts = []
    reasons = []
    for path in paths:
        artifact = _audit_one_target_artifact(path, prefixes, expected_arch)
        artifacts.append(artifact)
        reasons.extend(artifact["reasons"])
    return {
        "status": "fail" if reasons else "pass",
        "expected_arch": expected_arch,
        "forbidden_prefixes": prefixes,
        "artifacts": artifacts,
        "reasons": reasons,
    }


def _audit_one_target_artifact(path: str, forbidden_prefixes: list[str], expected_arch: str | None) -> dict:
    abs_path = os.path.abspath(path)
    reasons = []
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as exc:
        return {
            "path": abs_path,
            "status": "fail",
            "format": "unreadable",
            "arch": None,
            "forbidden_prefix_hits": [],
            "reasons": [f"failed to read target artifact {abs_path}: {exc}"],
        }
    fmt, arch = _detect_elf_arch(data)
    hits = []
    for prefix in forbidden_prefixes:
        if prefix.encode("utf-8") in data:
            hits.append(prefix)
            reasons.append(f"target artifact {abs_path} contains forbidden prefix {prefix}")
    if expected_arch and arch is not None and arch != expected_arch:
        reasons.append(f"target artifact {abs_path} arch {arch} != expected {expected_arch}")
    elif expected_arch and arch is None and fmt == "elf":
        reasons.append(f"target artifact {abs_path} has unknown ELF architecture")
    return {
        "path": abs_path,
        "status": "fail" if reasons else "pass",
        "format": fmt,
        "arch": arch,
        "forbidden_prefix_hits": hits,
        "reasons": reasons,
    }


def _detect_elf_arch(data: bytes) -> tuple[str, str | None]:
    if len(data) < 20 or data[:4] != b"\x7fELF":
        return "non_elf", None
    endian = data[5]
    if endian == 1:
        e_machine = int.from_bytes(data[18:20], "little")
    elif endian == 2:
        e_machine = int.from_bytes(data[18:20], "big")
    else:
        return "elf", None
    return "elf", {
        3: "x86",
        40: "arm",
        62: "x86_64",
        183: "aarch64",
    }.get(e_machine, f"unknown-{e_machine}")


def _default_forbidden_prefixes(prefixes: list[str] | None, host_prefix: str | None) -> list[str]:
    values = list(prefixes or [])
    if host_prefix:
        values.append(os.path.abspath(host_prefix))
    return sorted(set(values))


def format_ros2_compatibility_lock_yaml(lock: dict) -> str:
    """Return deterministic YAML for ros2_compatibility.lock.yaml.

    Kept dependency-free so `diagnose-interface` remains usable in minimal Python
    environments before ROS2/ament dependencies are installed.
    """
    return _format_yaml(lock) + "\n"


def _format_yaml(value: object, indent: int = 0) -> str:
    pad = "  " * indent
    if isinstance(value, dict):
        lines = []
        for key, item in value.items():
            if isinstance(item, (dict, list)):
                lines.append(f"{pad}{key}:")
                lines.append(_format_yaml(item, indent + 1))
            else:
                lines.append(f"{pad}{key}: {_yaml_scalar(item)}")
        return "\n".join(lines)
    if isinstance(value, list):
        if not value:
            return f"{pad}[]"
        lines = []
        for item in value:
            if isinstance(item, dict):
                lines.append(f"{pad}-")
                lines.append(_format_yaml(item, indent + 1))
            elif isinstance(item, list):
                lines.append(f"{pad}-")
                lines.append(_format_yaml(item, indent + 1))
            else:
                lines.append(f"{pad}- {_yaml_scalar(item)}")
        return "\n".join(lines)
    return f"{pad}{_yaml_scalar(value)}"


def _yaml_scalar(value: object) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, int):
        return str(value)
    text = str(value)
    if text == "" or any(ch in text for ch in ":#[]{}&,*?!|>'\"%@`\n") or text.strip() != text:
        return json.dumps(text)
    if text.lower() in ("null", "true", "false", "yes", "no"):
        return json.dumps(text)
    return text


def _match_known_good_matrix(distro: str, packages: list[dict]) -> str | None:
    versions = {package["name"]: package["version"] for package in packages}
    if not versions or any(version is None for version in versions.values()):
        return None
    for idx, entry in enumerate(_KNOWN_GOOD_ROS2_COMPATIBILITY, start=1):
        if entry.get("distro") != distro:
            continue
        expected = entry.get("packages", {})
        if all(versions.get(name) == version for name, version in expected.items()):
            return str(entry.get("name", f"entry-{idx}"))
    return None


def _read_ros2_package_version(ros2_prefix: str, package_name: str) -> tuple[str | None, str | None]:
    package_xml = os.path.join(ros2_prefix, "share", package_name, "package.xml")
    if os.path.isfile(package_xml):
        try:
            root = ET.parse(package_xml).getroot()
        except ET.ParseError:
            return None, package_xml
        version = root.findtext("version")
        if version is not None:
            return version.strip(), package_xml
    return _read_cmake_package_version(ros2_prefix, package_name)


def _read_cmake_package_version(prefix: str, package_name: str) -> tuple[str | None, str | None]:
    config_names = [
        f"{package_name}-config-version.cmake",
        f"{package_name}Config-version.cmake",
    ]
    candidate_dirs = [
        os.path.join(prefix, "share", package_name, "cmake"),
        os.path.join(prefix, "lib", "cmake", package_name),
    ]
    for directory in candidate_dirs:
        for config_name in config_names:
            candidate = os.path.join(directory, config_name)
            version = _read_cmake_package_version_file(candidate)
            if version is not None:
                return version, candidate
    return None, None


def _read_cmake_package_version_file(path: str) -> str | None:
    if not os.path.isfile(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return None
    match = re.search(r"set\s*\(\s*PACKAGE_VERSION\s+\"([^\"]+)\"\s*\)", text)
    if not match:
        return None
    return match.group(1)


def _interface_capability(message_dicts: list[dict]) -> dict:
    caps = [m["capability"] for m in message_dicts]
    sized_caps = [c.get("max_serialized_size_bytes") for c in caps]
    max_serialized_size_bytes = (
        sum(size for size in sized_caps if isinstance(size, int))
        if all(isinstance(size, int) for size in sized_caps)
        else None
    )
    suggestions = []
    for message in message_dicts:
        for suggestion in message["capability"]["suggestions"]:
            scoped = dict(suggestion)
            scoped["message"] = message["name"]
            suggestions.append(scoped)
    return {
        "is_plain": all(c["is_plain"] for c in caps),
        "is_fixed_size": all(c["is_fixed_size"] for c in caps),
        "is_bounded": all(c["is_bounded"] for c in caps),
        "contains_heap": any(c["contains_heap"] for c in caps),
        "contains_unbounded_sequence": any(c["contains_unbounded_sequence"] for c in caps),
        "contains_bounded_sequence": any(c["contains_bounded_sequence"] for c in caps),
        "contains_string": any(c["contains_string"] for c in caps),
        "contains_nested_non_plain": any(c["contains_nested_non_plain"] for c in caps),
        "max_serialized_size_known": all(c["max_serialized_size_known"] for c in caps),
        "max_serialized_size_bytes": max_serialized_size_bytes,
        "zero_copy": _zero_copy_capability(all(c["is_plain"] for c in caps)),
        "transports": _transport_capability(all(c["is_plain"] for c in caps)),
        "suggestions": sorted(suggestions, key=lambda item: (item.get("message", ""), item.get("field", ""), item["reason"])),
    }


def _message_capability(message: Ros2MessageIR) -> dict:
    field_caps = [_field_capability(field) for field in message.fields]
    is_plain = message.is_plain_compatible
    field_sizes = [c.get("size_bytes") for c in field_caps]
    max_serialized_size_bytes = (
        sum(size for size in field_sizes if isinstance(size, int))
        if is_plain and all(isinstance(size, int) for size in field_sizes)
        else None
    )
    suggestions = []
    for field_cap in field_caps:
        suggestions.extend(field_cap["suggestions"])
    return {
        "is_plain": is_plain,
        "is_fixed_size": all(c["is_fixed_size"] for c in field_caps),
        "is_bounded": all(c["is_bounded"] for c in field_caps),
        "contains_heap": any(c["contains_heap"] for c in field_caps),
        "contains_unbounded_sequence": any(c["contains_unbounded_sequence"] for c in field_caps),
        "contains_bounded_sequence": any(c["contains_bounded_sequence"] for c in field_caps),
        "contains_string": any(c["contains_string"] for c in field_caps),
        "contains_nested_non_plain": any(c["contains_nested_non_plain"] for c in field_caps),
        "max_serialized_size_known": all(c["max_serialized_size_known"] for c in field_caps),
        "max_serialized_size_bytes": max_serialized_size_bytes,
        "zero_copy": _zero_copy_capability(is_plain),
        "transports": _transport_capability(is_plain),
        "suggestions": sorted(suggestions, key=lambda item: (item.get("field", ""), item["reason"])),
    }


def _zero_copy_capability(is_plain: bool) -> dict:
    value = "yes" if is_plain else "no"
    return {"dds_loaned": value, "iceoryx": value, "shm": value}


def _transport_capability(is_plain: bool) -> dict:
    return {
        "dds_ros2_compatible": "yes",
        "dds_native": "yes" if is_plain else "warn",
        "json": "yes",
        "iceoryx": "yes" if is_plain else "no",
        "shm": "yes" if is_plain else "no",
    }


def _field_serialized_size_bytes(field: Ros2Field) -> int | None:
    """Return an IR-derived packed primitive size bound, not C++ sizeof(T)."""

    base = field.base_type
    base_size = _PRIMITIVE_SERIALIZED_SIZE_BYTES.get(base)
    if base_size is not None and field.array_kind is None:
        return base_size
    if base_size is None:
        if not field.dependency or not field.dependency.plain_compatible:
            return None
        base_size = field.dependency.serialized_size_bytes
        if base_size is None:
            return None
        if field.array_kind is None:
            return base_size
    if field.array_kind != "fixed":
        return None
    match = _ARRAY_RE.match(field.type)
    if not match:
        return None
    return base_size * int(match.group("bound"))


def _message_ir_serialized_size_bytes(message: Ros2MessageIR) -> int | None:
    sizes = [_field_serialized_size_bytes(field) for field in message.fields]
    if not all(isinstance(size, int) for size in sizes):
        return None
    return sum(size for size in sizes if isinstance(size, int))


def _interface_ir_serialized_size_bytes(ir: Ros2InterfaceIR) -> int | None:
    sizes = [_message_ir_serialized_size_bytes(message) for message in ir.messages]
    if not all(isinstance(size, int) for size in sizes):
        return None
    return sum(size for size in sizes if isinstance(size, int))


def _field_capability(field: Ros2Field) -> dict:
    base = field.base_type
    array_kind = field.array_kind
    contains_string = base in ("string", "wstring") or bool(_BOUNDED_STRING_RE.match(base))
    contains_unbounded_sequence = array_kind == "unbounded"
    contains_bounded_sequence = array_kind == "bounded"
    contains_nested_non_plain = bool(
        field.is_external_ref and field.dependency is not None and not field.dependency.plain_compatible
    )
    unresolved_external = bool(field.is_external_ref and (field.dependency is None or not field.dependency.resolved))
    fixed_size = not (
        contains_string
        or array_kind in ("unbounded", "bounded")
        or contains_nested_non_plain
        or unresolved_external
    )
    bounded = not contains_unbounded_sequence and not unresolved_external
    if base in ("string", "wstring"):
        bounded = False
    max_serialized_size_known = bounded and not unresolved_external
    contains_heap = contains_string or array_kind in ("unbounded", "bounded") or contains_nested_non_plain or unresolved_external
    size_bytes = _field_serialized_size_bytes(field)
    suggestions = _field_suggestions(field, contains_string, contains_unbounded_sequence, contains_bounded_sequence, contains_nested_non_plain, unresolved_external)
    return {
        "field": field.name,
        "size_bytes": size_bytes,
        "is_fixed_size": fixed_size,
        "is_bounded": bounded,
        "contains_heap": contains_heap,
        "contains_unbounded_sequence": contains_unbounded_sequence,
        "contains_bounded_sequence": contains_bounded_sequence,
        "contains_string": contains_string,
        "contains_nested_non_plain": contains_nested_non_plain,
        "max_serialized_size_known": max_serialized_size_known,
        "suggestions": suggestions,
    }


def _field_suggestions(
    field: Ros2Field,
    contains_string: bool,
    contains_unbounded_sequence: bool,
    contains_bounded_sequence: bool,
    contains_nested_non_plain: bool,
    unresolved_external: bool,
) -> list[dict]:
    suggestions = []
    base = field.base_type
    if base in ("string", "wstring"):
        suggestions.append(
            {
                "field": field.name,
                "reason": f"field {field.name} uses unbounded string type {field.type}",
                "suggestion": "replace with string<=N or a fixed-size char array for zero-copy/plain transports",
            }
        )
    elif contains_string:
        suggestions.append(
            {
                "field": field.name,
                "reason": f"field {field.name} uses bounded string type {field.type}",
                "suggestion": "replace with a fixed-size char array if strict plain/zero-copy is required",
            }
        )
    if contains_unbounded_sequence:
        suggestions.append(
            {
                "field": field.name,
                "reason": f"field {field.name} uses unbounded sequence type {field.type}",
                "suggestion": "replace with T[<=N], T[N], or split large payload into a loaned/shared buffer",
            }
        )
    if contains_bounded_sequence:
        suggestions.append(
            {
                "field": field.name,
                "reason": f"field {field.name} uses bounded sequence type {field.type}",
                "suggestion": "replace with T[N] plus an explicit size field if strict plain/zero-copy is required",
            }
        )
    if contains_nested_non_plain and field.dependency is not None:
        suggestions.append(
            {
                "field": field.name,
                "reason": f"field {field.name} references non-plain nested type {field.dependency.canonical}",
                "suggestion": f"make dependency plain-compatible first: {field.dependency.reason}",
            }
        )
    if unresolved_external:
        suggestions.append(
            {
                "field": field.name,
                "reason": f"field {field.name} has unresolved external type {base}",
                "suggestion": "add a --search-root that contains the referenced ROS2 interface package",
            }
        )
    return suggestions


def _parse_message_text(name: str, text: str) -> Ros2MessageIR:
    fields: list[Ros2Field] = []
    constants: list[Ros2Constant] = []
    for line_no, raw in enumerate(text.splitlines(), start=1):
        line = _strip_comment(raw).strip()
        if not line:
            continue
        const_assign = _CONST_ASSIGN_RE.search(line)
        if const_assign:
            left = line[:const_assign.start()].strip()
            const_name = left.split()[-1] if left else ""
            if not _CONST_NAME_RE.match(const_name):
                raise InterfaceParseError(
                    f"{name}:{line_no}: invalid constant name '{const_name}': constants must be uppercase-style"
                )
            const_match = _CONST_RE.match(line)
            if not const_match:
                raise InterfaceParseError(f"{name}:{line_no}: malformed constant line: {raw.strip()}")
            const_type = const_match.group("type")
            _validate_constant_type(const_type, name, line_no)
            constants.append(
                Ros2Constant(
                    const_type,
                    const_match.group("name"),
                    const_match.group("value").strip(),
                )
            )
            continue
        const_match = _CONST_RE.match(line)
        if const_match:
            raise InterfaceParseError(f"{name}:{line_no}: internal constant parser error")
        field_match = _FIELD_RE.match(line)
        if field_match:
            field_type = field_match.group("type")
            _validate_field_type(field_type, name, line_no)
            fields.append(Ros2Field(field_type, field_match.group("name"), field_match.group("default")))
            continue
        raise InterfaceParseError(f"{name}:{line_no}: unsupported interface line: {raw.strip()}")
    return Ros2MessageIR(name, fields, constants)


def _attach_dependencies(
    ir: Ros2InterfaceIR,
    search_roots: list[str],
    resolve_dependencies: bool,
    stack: list[str],
    cache: dict[str, Ros2Dependency],
) -> Ros2InterfaceIR:
    dependencies: dict[str, Ros2Dependency] = {}
    messages: list[Ros2MessageIR] = []
    for message in ir.messages:
        fields: list[Ros2Field] = []
        for field in message.fields:
            if not field.is_external_ref:
                fields.append(field)
                continue
            dep = _resolve_dependency(field.base_type, ir.package, search_roots, resolve_dependencies, stack, cache)
            _add_dependency(dependencies, dep)
            fields.append(Ros2Field(field.type, field.name, field.default, dep))
        messages.append(Ros2MessageIR(message.name, fields, message.constants))
    return Ros2InterfaceIR(
        ir.kind,
        ir.name,
        ir.package,
        ir.type_path,
        messages,
        [dependencies[key] for key in sorted(dependencies)],
    )


def _add_dependency(dependencies: dict[str, Ros2Dependency], dep: Ros2Dependency) -> None:
    if dep.canonical in dependencies:
        return
    dependencies[dep.canonical] = dep
    for nested in dep.nested_dependencies:
        _add_dependency(dependencies, nested)


def _resolve_dependency(
    ref: str,
    current_package: str,
    search_roots: list[str],
    resolve_dependencies: bool,
    stack: list[str],
    cache: dict[str, Ros2Dependency],
) -> Ros2Dependency:
    package, kind, type_name, canonical = _parse_external_ref(ref, current_package)
    if not resolve_dependencies:
        return Ros2Dependency(ref, canonical, kind)
    path = _find_dependency_path(search_roots, package, kind, type_name)
    if path is None:
        return Ros2Dependency(ref, canonical, kind)
    abs_path = os.path.abspath(path)
    if kind != "msg":
        return Ros2Dependency(
            ref,
            canonical,
            kind,
            abs_path,
            True,
            False,
            f"external {kind} references are unsupported for plain analysis",
        )
    if canonical in stack:
        cycle = " -> ".join(stack + [canonical])
        return Ros2Dependency(ref, canonical, kind, abs_path, True, False, f"cycle detected: {cycle}")
    if canonical in cache:
        return cache[canonical]

    nested_ir = _parse_interface_file(abs_path, search_roots, True, stack + [canonical], cache)
    nested_reasons = [reason for msg in nested_ir.messages for reason in msg.plain_reasons]
    plain_compatible = not nested_reasons
    reason = "plain-compatible" if plain_compatible else nested_reasons[0]
    serialized_size_bytes = _interface_ir_serialized_size_bytes(nested_ir) if plain_compatible else None
    dep = Ros2Dependency(
        ref,
        canonical,
        kind,
        abs_path,
        True,
        plain_compatible,
        reason,
        tuple(nested_ir.dependencies),
        serialized_size_bytes,
    )
    cache[canonical] = dep
    return dep


def _parse_external_ref(ref: str, current_package: str = "<unknown>") -> tuple[str, str, str, str]:
    parts = ref.split("/")
    if len(parts) == 1 and current_package != "<unknown>":
        package = current_package
        kind = "msg"
        type_name = ref
    elif len(parts) == 1:
        package = ref
        kind = "msg"
        type_name = ref
    elif len(parts) == 2:
        package, type_name = parts
        kind = "msg"
    elif len(parts) == 3 and parts[1] in ("msg", "srv", "action"):
        package, kind, type_name = parts
    else:
        package = parts[0]
        kind = "msg"
        type_name = parts[-1]
    return package, kind, type_name, f"{package}/{kind}/{type_name}"


def _find_dependency_path(search_roots: list[str], package: str, kind: str, type_name: str) -> str | None:
    ext = {"msg": ".msg", "srv": ".srv", "action": ".action"}.get(kind)
    if ext is None:
        return None
    for root in search_roots:
        for candidate in _dependency_path_candidates(root, package, kind, type_name + ext):
            if os.path.isfile(candidate):
                return candidate
    return None


def _dependency_path_candidates(root: str, package: str, kind: str, filename: str) -> list[str]:
    """Return deterministic local ROS 2 interface path candidates for a search root.

    Supported roots are intentionally simple and pure-Python:
    - package collection root: <root>/<package>/<kind>/<Name>.<ext>
    - ROS workspace root:      <root>/src/<package>/<kind>/<Name>.<ext>
    - package root:            <root>/<kind>/<Name>.<ext> when basename(root) == package
    """
    candidates = [
        os.path.join(root, package, kind, filename),
        os.path.join(root, "src", package, kind, filename),
    ]
    if os.path.basename(os.path.normpath(root)) == package:
        candidates.append(os.path.join(root, kind, filename))
    return candidates


def _split_srv_text(text: str) -> list[str]:
    parts = [""]
    for raw in text.splitlines():
        line = _strip_comment(raw).strip()
        if line == "---":
            parts.append("")
            continue
        parts[-1] += raw + "\n"
    return parts


def _infer_package_type_path(path: str, kind: str, name: str) -> tuple[str, str]:
    parts = os.path.normpath(path).split(os.sep)
    for idx, part in enumerate(parts):
        if part == kind and idx > 0 and idx == len(parts) - 2:
            package = parts[idx - 1]
            return package, f"{package}/{kind}/{name}"
    return "<unknown>", f"{kind}/{name}"


def _strip_comment(line: str) -> str:
    in_single = False
    in_double = False
    escaped = False
    for idx, ch in enumerate(line):
        if escaped:
            escaped = False
            continue
        if ch == "\\":
            escaped = True
            continue
        if ch == "'" and not in_double:
            in_single = not in_single
            continue
        if ch == '"' and not in_single:
            in_double = not in_double
            continue
        if ch == "#" and not in_single and not in_double:
            return line[:idx]
    return line


def _parse_array_type(type_name: str) -> tuple[str, str | None]:
    match = _ARRAY_RE.match(type_name)
    if not match:
        return type_name, None
    bound = match.group("bound")
    if bound == "":
        return match.group("base"), "unbounded"
    if bound.startswith("<="):
        return match.group("base"), "bounded"
    return match.group("base"), "fixed"


def _validate_field_type(type_name: str, message_name: str, line_no: int) -> None:
    base, _ = _validate_array_suffix(type_name, message_name, line_no)
    match = _BOUNDED_STRING_RE.match(base)
    if match and int(match.group("bound")) <= 0:
        raise InterfaceParseError(f"{message_name}:{line_no}: string bound must be greater than zero: {type_name}")


def _validate_constant_type(type_name: str, message_name: str, line_no: int) -> None:
    base, array_kind = _validate_array_suffix(type_name, message_name, line_no)
    if array_kind is not None:
        raise InterfaceParseError(f"{message_name}:{line_no}: constants cannot use array type: {type_name}")
    if _BOUNDED_STRING_RE.match(base):
        raise InterfaceParseError(f"{message_name}:{line_no}: constants cannot use bounded string type: {type_name}")
    if base not in _BUILTIN_PRIMITIVES:
        raise InterfaceParseError(f"{message_name}:{line_no}: constants must use primitive types: {type_name}")


def _validate_array_suffix(type_name: str, message_name: str, line_no: int) -> tuple[str, str | None]:
    match = _ARRAY_RE.match(type_name)
    if not match:
        return type_name, None
    base = match.group("base")
    bound = match.group("bound")
    if bound == "":
        return base, "unbounded"
    value = int(bound[2:] if bound.startswith("<=") else bound)
    if value <= 0:
        raise InterfaceParseError(f"{message_name}:{line_no}: array bound must be greater than zero: {type_name}")
    return base, "bounded" if bound.startswith("<=") else "fixed"


def _is_external_type(type_name: str) -> bool:
    if type_name in _BUILTIN_PRIMITIVES or _BOUNDED_STRING_RE.match(type_name):
        return False
    return "/" in type_name or (bool(type_name) and type_name[0].isupper())


def format_diagnosis_json(data: dict) -> str:
    """Return stable pretty JSON for diagnosis CLI output."""
    return json.dumps(data, sort_keys=True, indent=2) + "\n"


def emit_rosidl_artifact_package(
    ros2_src: str,
    output_dir: str,
    package_name: str | None = None,
    targets: list[str] | None = None,
) -> dict:
    """Emit a minimal artifact-only rosidl interface package.

    This is the first Phase-2 source-mode slice: it copies local msg/srv/action
    files into a standard ROS2 package skeleton and emits a deterministic report.
    It intentionally does not generate IBMW wrappers yet.
    """
    src_root = os.path.abspath(ros2_src)
    out_root = os.path.abspath(output_dir)
    if os.path.isfile(src_root):
        return _emit_rosidl_artifact_package_from_files(
            [src_root],
            out_root,
            package_name or _infer_single_file_package_name(src_root),
            targets,
            source_root=src_root,
            input_mode="single_file",
        )
    if not os.path.isdir(src_root):
        raise InterfaceParseError(f"ROS2 source root not found: {ros2_src}")

    workspace_packages = _discover_workspace_interface_packages(src_root)
    if workspace_packages and not _discover_local_interface_files(src_root):
        if package_name and len(workspace_packages) != 1:
            raise InterfaceParseError("--package-name can only be used with a single package import")
        package_reports = []
        for package_root in workspace_packages:
            detected_package = _read_package_name_from_xml(package_root) or os.path.basename(package_root)
            package_out = out_root if len(workspace_packages) == 1 else os.path.join(out_root, detected_package)
            package_reports.append(
                _emit_rosidl_artifact_package_from_files(
                    _discover_local_interface_files(package_root),
                    package_out,
                    package_name or detected_package,
                    targets,
                    source_root=package_root,
                    input_mode="package",
                )
            )
        report = {
            "ok": True,
            "schema_version": 1,
            "artifact_only": True,
            "mode": "workspace",
            "source_root": src_root,
            "output_dir": out_root,
            "targets": sorted(targets or ["dds_ros2"]),
            "packages": package_reports,
            "rosidl_first": True,
        }
        os.makedirs(out_root, exist_ok=True)
        _write_text(os.path.join(out_root, "ibmw_workspace_interface_report.json"), format_diagnosis_json(report))
        return report

    files = _discover_local_interface_files(src_root)
    if not files:
        raise InterfaceParseError(f"no .msg, .srv, or .action files found under {src_root}")
    package = package_name or _read_package_name_from_xml(src_root) or os.path.basename(src_root)
    return _emit_rosidl_artifact_package_from_files(
        files,
        out_root,
        package,
        targets,
        source_root=src_root,
        input_mode="package",
    )


def emit_reference_existing_interface_report(
    ros2_prefix: str,
    package_name: str,
    output_dir: str,
    targets: list[str] | None = None,
    selected_types: list[str] | None = None,
    host_tools_prefix: str | None = None,
    target_runtime_prefix: str | None = None,
    target_artifacts: list[str] | None = None,
    forbidden_prefixes: list[str] | None = None,
    expected_target_arch: str | None = None,
    require_target_audit_pass: bool = False,
    ros2_prefix_source: str = "explicit",
) -> dict:
    """Emit a deterministic report for an already-installed ROS2 interface package.

    This reference-existing slice intentionally does not copy interface files or
    regenerate a rosidl package.  It records the installed package metadata and
    the IBMW-side wrapper contract needed by later generator slices.
    """
    prefix = os.path.abspath(ros2_prefix)
    host_prefix = os.path.abspath(host_tools_prefix) if host_tools_prefix else prefix
    target_prefix = os.path.abspath(target_runtime_prefix) if target_runtime_prefix else None
    out_root = os.path.abspath(output_dir)
    if not os.path.isdir(prefix):
        raise InterfaceParseError(f"ROS2 prefix not found: {ros2_prefix}")
    if not re.match(r"^[A-Za-z][A-Za-z0-9_]*$", package_name):
        raise InterfaceParseError(f"invalid ROS2 package name for reference_existing import: {package_name}")

    package_root = _resolve_existing_ros2_package_root(prefix, package_name)
    available_files = _discover_local_interface_files(package_root)
    files = _select_existing_interface_files(
        package_root,
        package_name,
        selected_types,
    )
    if not files:
        raise InterfaceParseError(
            f"no installed .msg, .srv, or .action files found for ROS2 package {package_name} under {prefix}"
        )

    rel_files = [_interface_relative_path(path, package_root) for path in files]
    external_packages = sorted(_external_dependency_packages(files, package_name, package_root))
    dependency_closure = _reference_existing_dependency_closure(prefix, package_root, package_name, files)
    package_version, package_version_source = _read_ros2_package_version(prefix, package_name)
    forbidden_runtime_dependencies = [
        "rcl",
        "rclcpp",
        "rclpy",
        "rcl_action",
        "rclcpp_action",
        "ament_index_cpp",
        "ament_index_python",
    ]
    external_dependencies_resolved = []
    for dep in external_packages:
        dep_share = os.path.join(prefix, "share", dep)
        external_dependencies_resolved.append(
            {
                "package": dep,
                "found_in_ros2_prefix": os.path.isdir(dep_share),
                "package_share": dep_share if os.path.isdir(dep_share) else None,
            }
        )
    audit_forbidden_prefixes = list(forbidden_prefixes or [])
    if host_prefix:
        audit_forbidden_prefixes.append(host_prefix)
    target_artifact_audit = audit_target_artifacts(
        target_artifacts,
        forbidden_prefixes=sorted(set(audit_forbidden_prefixes)),
        expected_arch=expected_target_arch,
    )
    if require_target_audit_pass and target_artifact_audit["status"] == "fail":
        raise InterfaceParseError(
            "reference_existing target artifact audit failed: " + "; ".join(target_artifact_audit["reasons"])
        )
    generated_files = ["ibmw_interface_report.json"]
    wrapper_header = _render_ibmw_interface_types_header(package_name, files)
    conversion_header = _render_ibmw_ros2_conversion_header(package_name, files)
    rpc_bridge_header = _render_ibmw_rpc_bridge_skeleton_header(package_name, files)
    rpc_runtime_header = _render_ibmw_rpc_bridge_runtime_header(package_name, files)
    action_traits_header = _render_ibmw_action_traits_header(package_name, files)
    action_binding_header = _render_ibmw_action_binding_skeleton_header(package_name, files)
    action_lifecycle_header = _render_ibmw_action_lifecycle_runtime_skeleton_header(package_name, files)
    action_handler_header = _render_ibmw_action_handler_skeleton_header(package_name, files)
    action_server_runtime_header = _render_ibmw_action_server_runtime_header(package_name, files)
    action_contract_yaml = _render_ibmw_action_runtime_contract_yaml(package_name, files)
    runtime_project_yaml = _render_ibmw_imported_srv_runtime_project_yaml(
        package_name,
        files,
        adapter_include_dir=os.path.join(out_root, "include"),
    )
    if files:
        _write_text(os.path.join(out_root, "include", package_name, "ibmw_interface_types.hpp"), wrapper_header)
        generated_files.append(f"include/{package_name}/ibmw_interface_types.hpp")
    if conversion_header is not None:
        _write_text(os.path.join(out_root, "include", package_name, "ibmw_ros2_conversion.hpp"), conversion_header)
        generated_files.append(f"include/{package_name}/ibmw_ros2_conversion.hpp")
    if rpc_bridge_header is not None:
        _write_text(os.path.join(out_root, "include", package_name, "ibmw_rpc_bridge_skeleton.hpp"), rpc_bridge_header)
        _write_text(os.path.join(out_root, "include", package_name, "ibmw_rpc_bridge_runtime.hpp"), rpc_runtime_header or "")
        generated_files.extend(
            [
                f"include/{package_name}/ibmw_rpc_bridge_skeleton.hpp",
                f"include/{package_name}/ibmw_rpc_bridge_runtime.hpp",
            ]
        )
    if action_traits_header is not None:
        _write_text(os.path.join(out_root, "include", package_name, "ibmw_action_traits.hpp"), action_traits_header)
        generated_files.append(f"include/{package_name}/ibmw_action_traits.hpp")
    if action_binding_header is not None:
        _write_text(os.path.join(out_root, "include", package_name, "ibmw_action_binding_skeleton.hpp"), action_binding_header)
        generated_files.append(f"include/{package_name}/ibmw_action_binding_skeleton.hpp")
    if action_lifecycle_header is not None:
        _write_text(os.path.join(out_root, "include", package_name, "ibmw_action_lifecycle_runtime_skeleton.hpp"), action_lifecycle_header)
        generated_files.append(f"include/{package_name}/ibmw_action_lifecycle_runtime_skeleton.hpp")
    if action_handler_header is not None:
        _write_text(os.path.join(out_root, "include", package_name, "ibmw_action_handler_skeleton.hpp"), action_handler_header)
        generated_files.append(f"include/{package_name}/ibmw_action_handler_skeleton.hpp")
    if action_server_runtime_header is not None:
        _write_text(os.path.join(out_root, "include", package_name, "ibmw_action_server_runtime.hpp"), action_server_runtime_header)
        generated_files.append(f"include/{package_name}/ibmw_action_server_runtime.hpp")
    if action_contract_yaml is not None:
        action_contract_name = f"{package_name}_action_runtime_contract.yaml"
        _write_text(os.path.join(out_root, action_contract_name), action_contract_yaml)
        generated_files.append(action_contract_name)
    if runtime_project_yaml is not None:
        runtime_yaml_name = f"{package_name}_reference_existing_srv_runtime.ibmw.yaml"
        _write_text(os.path.join(out_root, runtime_yaml_name), runtime_project_yaml)
        generated_files.append(runtime_yaml_name)
    dependency_closure_name = f"{package_name}_dependency_closure.json"
    _write_text(os.path.join(out_root, dependency_closure_name), format_diagnosis_json(dependency_closure))
    generated_files.append(dependency_closure_name)
    backend_artifacts = _emit_requested_backend_metadata(
        out_root,
        package_name,
        files,
        sorted(targets or ["dds_ros2"]),
        mode="reference_existing",
        source_root=package_root,
    )
    backend_metadata_files = backend_artifacts["metadata_files"]
    backend_generated_files = backend_artifacts["generated_files"]
    generated_files.extend(backend_metadata_files)
    generated_files.extend(backend_generated_files)
    migration_manifest_name = f"{package_name}_migration_manifest.json"
    migration_manifest = _reference_existing_migration_manifest(
        package_name,
        files,
        dependency_closure,
        dependency_closure_name=dependency_closure_name,
        ros2_prefix=prefix,
        ros2_prefix_source=ros2_prefix_source,
        package_root=package_root,
        host_prefix=host_prefix,
        target_prefix=target_prefix,
        forbidden_runtime_dependencies=forbidden_runtime_dependencies,
        targets=sorted(targets or ["dds_ros2"]),
        backend_metadata_files=backend_metadata_files,
        backend_generated_files=backend_generated_files,
    )
    _write_text(os.path.join(out_root, migration_manifest_name), format_diagnosis_json(migration_manifest))
    generated_files.append(migration_manifest_name)
    report = {
        "ok": True,
        "schema_version": 1,
        "artifact_only": True,
        "mode": "reference_existing",
        "input_mode": "installed_package",
        "package": package_name,
        "rosidl_targets": _rosidl_target_report(package_name),
        "ibmw_wrapper": _ibmw_wrapper_report(package_name, files),
        "source_root": package_root,
        "ros2_prefix": prefix,
        "ros2_prefix_source": ros2_prefix_source,
        "boundary": {
            "discovery_source": "share_tree_scan",
            "ros2_prefix": prefix,
            "ros2_prefix_source": ros2_prefix_source,
            "host_tools_prefix": host_prefix,
            "target_runtime_prefix": target_prefix,
            "host_prefix_used_for_target_runtime": False,
            "runtime_dependency_policy": "rcl_free_ibmw_runtime",
        },
        "prefix_provenance": {
            "package_share": package_root,
            "package_xml": os.path.join(package_root, "package.xml") if os.path.isfile(os.path.join(package_root, "package.xml")) else None,
            "package_version": package_version,
            "version_source": package_version_source,
        },
        "output_dir": out_root,
        "targets": sorted(targets or ["dds_ros2"]),
        "target_backend_status": _target_backend_status_report(sorted(targets or ["dds_ros2"]), files, backend_metadata_files),
        "backend_metadata_files": backend_metadata_files,
        "backend_generated_files": backend_generated_files,
        "selected_types": sorted(selected_types or []),
        "available_types": _available_reference_types_report(available_files, package_root, package_name),
        "files": rel_files,
        "external_dependencies": external_packages,
        "external_dependencies_resolved": external_dependencies_resolved,
        "dependency_closure": dependency_closure,
        "migration_manifest": migration_manifest,
        "verification": migration_manifest["verification"],
        "generated_files": generated_files,
        "rosidl_first": True,
        "rcl_free_runtime": True,
        "cross_compile_safe": True,
        "cross_compile_reasons": [
            "reference_existing discovery uses host-side share tree scanning only",
            "generated IBMW runtime forbids rcl/rclcpp/rclpy/ament runtime dependencies",
            "target runtime prefix is recorded separately and is not used for host discovery",
        ],
        "target_artifact_audit": target_artifact_audit,
        "dependency_resolution": {
            "mode": "reference_existing",
            "strategy": "find_package_existing_rosidl_targets",
            "package_share": package_root,
            "closure_manifest": dependency_closure_name,
            "closure_status": dependency_closure["status"],
            "migration_manifest": migration_manifest_name,
            "required_targets": _rosidl_target_report(package_name),
            "forbidden_runtime_dependencies": forbidden_runtime_dependencies,
        },
    }
    os.makedirs(out_root, exist_ok=True)
    _write_text(os.path.join(out_root, "ibmw_interface_report.json"), format_diagnosis_json(report))
    return report


def emit_reference_existing_discovery_report(
    ros2_prefix: str,
    package_name: str | None = None,
    ros2_prefix_source: str = "explicit",
) -> dict:
    """Return deterministic package/type choices from an installed ROS2 prefix.

    This discovery helper intentionally performs only host-side share-tree scans.
    It does not import ROS2 Python modules and does not invoke the ros2 CLI.
    """
    prefix = os.path.abspath(ros2_prefix)
    if not os.path.isdir(prefix):
        raise InterfaceParseError(f"ROS2 prefix not found: {ros2_prefix}")

    packages = []
    if package_name:
        if not re.match(r"^[A-Za-z][A-Za-z0-9_]*$", package_name):
            raise InterfaceParseError(f"invalid ROS2 package name for reference_existing discovery: {package_name}")
        package_roots = [(package_name, _resolve_existing_ros2_package_root(prefix, package_name))]
    else:
        package_roots = _discover_existing_interface_package_roots(prefix)

    for package, package_root in package_roots:
        files = _discover_local_interface_files(package_root)
        if not files:
            if package_name:
                raise InterfaceParseError(
                    f"no installed .msg, .srv, or .action files found for ROS2 package {package_name} under {prefix}"
                )
            continue
        version, version_source = _read_ros2_package_version(prefix, package)
        available_types = _available_reference_types_report(files, package_root, package)
        packages.append(
            {
                "package": package,
                "package_share": package_root,
                "package_xml": os.path.join(package_root, "package.xml") if os.path.isfile(os.path.join(package_root, "package.xml")) else None,
                "package_version": version,
                "version_source": version_source,
                "available_types": available_types,
                "type_count": len(available_types),
                "kinds": sorted({entry["kind"] for entry in available_types}),
            }
        )

    packages.sort(key=lambda entry: entry["package"])
    return {
        "ok": True,
        "schema_version": 1,
        "mode": "reference_existing_discovery",
        "input_mode": "installed_prefix",
        "ros2_prefix": prefix,
        "ros2_prefix_source": ros2_prefix_source,
        "boundary": {
            "discovery_source": "share_tree_scan",
            "ros2_prefix": prefix,
            "ros2_prefix_source": ros2_prefix_source,
            "runtime_dependency_policy": "rcl_free_ibmw_runtime",
            "uses_ros2_cli": False,
            "uses_ament_index_python": False,
        },
        "package": package_name,
        "packages": packages,
        "package_count": len(packages),
        "type_count": sum(entry["type_count"] for entry in packages),
        "rosidl_first": True,
        "rcl_free_runtime": True,
    }


def emit_workspace_interface_discovery_report(
    workspace_root: str,
    package_name: str | None = None,
) -> dict:
    """Return deterministic interface package/type choices from a ROS2 workspace source tree."""
    root = os.path.abspath(workspace_root)
    if not os.path.isdir(root):
        raise InterfaceParseError(f"ROS2 workspace not found: {workspace_root}")
    if package_name and not re.match(r"^[A-Za-z][A-Za-z0-9_]*$", package_name):
        raise InterfaceParseError(f"invalid ROS2 package name for workspace discovery: {package_name}")

    packages = []
    for package_root in _discover_workspace_interface_packages(root):
        package = _read_package_name_from_xml(package_root) or os.path.basename(package_root)
        if package_name and package != package_name:
            continue
        files = _discover_local_interface_files(package_root)
        available_types = _available_reference_types_report(files, package_root, package)
        packages.append(
            {
                "package": package,
                "package_root": package_root,
                "package_xml": os.path.join(package_root, "package.xml") if os.path.isfile(os.path.join(package_root, "package.xml")) else None,
                "available_types": available_types,
                "type_count": len(available_types),
                "kinds": sorted({entry["kind"] for entry in available_types}),
                "recommended_import_mode": "generate_package",
                "reference_existing_ready": False,
            }
        )
    if package_name and not packages:
        raise InterfaceParseError(f"no source interface package {package_name} found under workspace {root}")
    packages.sort(key=lambda entry: entry["package"])
    return {
        "ok": True,
        "schema_version": 1,
        "mode": "workspace_source_discovery",
        "input_mode": "workspace_source_tree",
        "workspace_root": root,
        "package": package_name,
        "packages": packages,
        "package_count": len(packages),
        "type_count": sum(entry["type_count"] for entry in packages),
        "boundary": {
            "discovery_source": "workspace_source_tree_scan",
            "runtime_dependency_policy": "rcl_free_ibmw_runtime",
            "uses_ros2_cli": False,
            "uses_ament_index_python": False,
            "build_behavior_changed": False,
        },
        "rosidl_first": True,
        "rcl_free_runtime": True,
    }


def emit_workspace_interface_artifact_package(
    workspace_root: str,
    package_name: str,
    output_dir: str,
    artifact_package_name: str | None = None,
    targets: list[str] | None = None,
) -> dict:
    """Resolve a source package in a workspace and emit generate_package artifacts."""
    resolved = resolve_workspace_interface_package_root(workspace_root, package_name)
    report = emit_rosidl_artifact_package(
        resolved["package_root"],
        output_dir,
        package_name=artifact_package_name,
        targets=targets,
    )
    report["input_mode"] = "workspace_source_package"
    report["workspace_source"] = resolved
    report["boundary"] = {
        "discovery_source": "workspace_source_tree_scan",
        "runtime_dependency_policy": "rcl_free_ibmw_runtime",
        "uses_ros2_cli": False,
        "uses_ament_index_python": False,
    }
    _write_text(os.path.join(os.path.abspath(output_dir), "ibmw_interface_report.json"), format_diagnosis_json(report))
    return report


def resolve_workspace_interface_package_root(workspace_root: str, package_name: str) -> dict:
    """Resolve a single source interface package from a workspace tree."""
    root = os.path.abspath(workspace_root)
    if not os.path.isdir(root):
        raise InterfaceParseError(f"ROS2 workspace not found: {workspace_root}")
    if not re.match(r"^[A-Za-z][A-Za-z0-9_]*$", package_name):
        raise InterfaceParseError(f"invalid ROS2 package name for workspace import: {package_name}")
    matches = []
    for package_root in _discover_workspace_interface_packages(root):
        detected = _read_package_name_from_xml(package_root) or os.path.basename(package_root)
        if detected == package_name:
            matches.append(package_root)
    if not matches:
        raise InterfaceParseError(f"no source interface package {package_name} found under workspace {root}")
    return {
        "workspace_root": root,
        "package": package_name,
        "package_root": matches[0],
        "match_count": len(matches),
        "source": "workspace_source_tree_scan",
    }


def resolve_reference_existing_prefix_from_environment(
    package_name: str,
    env: dict[str, str] | None = None,
) -> dict:
    """Resolve an installed ROS2 prefix for package_name from sourced shell env.

    This helper only inspects prefix environment variables and share-tree files.
    It never imports ament_index_python and never invokes ros2 CLI.
    """
    if not re.match(r"^[A-Za-z][A-Za-z0-9_]*$", package_name):
        raise InterfaceParseError(f"invalid ROS2 package name for reference_existing import: {package_name}")
    candidates = _reference_existing_prefix_candidates_from_environment(env)
    matches = []
    for candidate in candidates:
        package_root = os.path.join(candidate["prefix"], "share", package_name)
        if os.path.isdir(package_root) and _discover_local_interface_files(package_root):
            matches.append({**candidate, "package_share": package_root})
    if not matches:
        searched = [candidate["prefix"] for candidate in candidates]
        raise InterfaceParseError(
            "reference_existing import requires --ros2-prefix or a sourced ROS2 install prefix containing "
            f"{package_name}; searched environment prefixes: {searched}"
        )
    match = matches[0]
    return {
        "prefix": match["prefix"],
        "source": f"environment:{match['variable']}",
        "package_share": match["package_share"],
        "candidate_count": len(candidates),
        "match_count": len(matches),
    }


def _reference_existing_prefix_candidates_from_environment(env: dict[str, str] | None = None) -> list[dict]:
    values = os.environ if env is None else env
    candidates = []
    seen = set()
    for variable in ("AMENT_PREFIX_PATH", "COLCON_PREFIX_PATH", "CMAKE_PREFIX_PATH"):
        for raw in values.get(variable, "").split(os.pathsep):
            if not raw:
                continue
            prefix = os.path.abspath(raw)
            if prefix in seen or not os.path.isdir(os.path.join(prefix, "share")):
                continue
            seen.add(prefix)
            candidates.append({"prefix": prefix, "variable": variable})
    return candidates


def _emit_rosidl_artifact_package_from_files(
    files: list[str],
    out_root: str,
    package: str,
    targets: list[str] | None,
    source_root: str,
    input_mode: str,
) -> dict:
    if not re.match(r"^[A-Za-z][A-Za-z0-9_]*$", package):
        raise InterfaceParseError(f"invalid ROS2 package name for artifact output: {package}")
    if not files:
        raise InterfaceParseError(f"no .msg, .srv, or .action files found under {source_root}")

    external_packages = sorted(_external_dependency_packages(files, package, source_root))
    os.makedirs(out_root, exist_ok=True)
    emitted_files = []
    for path in files:
        kind = _interface_kind_from_path(path)
        rel = os.path.join(kind, os.path.basename(path))
        dst = os.path.join(out_root, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(path, dst)
        emitted_files.append(rel)

    package_xml = _render_rosidl_package_xml(package, external_packages)
    cmake_lists = _render_rosidl_cmake_lists(package, emitted_files, external_packages)
    wrapper_header = _render_ibmw_interface_types_header(package, files)
    conversion_header = _render_ibmw_ros2_conversion_header(package, files)
    rpc_bridge_header = _render_ibmw_rpc_bridge_skeleton_header(package, files)
    rpc_runtime_header = _render_ibmw_rpc_bridge_runtime_header(package, files)
    action_traits_header = _render_ibmw_action_traits_header(package, files)
    action_binding_header = _render_ibmw_action_binding_skeleton_header(package, files)
    action_lifecycle_header = _render_ibmw_action_lifecycle_runtime_skeleton_header(package, files)
    action_handler_header = _render_ibmw_action_handler_skeleton_header(package, files)
    action_server_runtime_header = _render_ibmw_action_server_runtime_header(package, files)
    action_contract_yaml = _render_ibmw_action_runtime_contract_yaml(package, files)
    runtime_project_yaml = _render_ibmw_imported_srv_runtime_project_yaml(package, files)
    _write_text(os.path.join(out_root, "package.xml"), package_xml)
    _write_text(os.path.join(out_root, "CMakeLists.txt"), cmake_lists)
    _write_text(os.path.join(out_root, "include", package, "ibmw_interface_types.hpp"), wrapper_header)
    if conversion_header is not None:
        _write_text(os.path.join(out_root, "include", package, "ibmw_ros2_conversion.hpp"), conversion_header)
    if rpc_bridge_header is not None:
        _write_text(os.path.join(out_root, "include", package, "ibmw_rpc_bridge_skeleton.hpp"), rpc_bridge_header)
    if rpc_runtime_header is not None:
        _write_text(os.path.join(out_root, "include", package, "ibmw_rpc_bridge_runtime.hpp"), rpc_runtime_header)
    if action_traits_header is not None:
        _write_text(os.path.join(out_root, "include", package, "ibmw_action_traits.hpp"), action_traits_header)
    if action_binding_header is not None:
        _write_text(os.path.join(out_root, "include", package, "ibmw_action_binding_skeleton.hpp"), action_binding_header)
    if action_lifecycle_header is not None:
        _write_text(os.path.join(out_root, "include", package, "ibmw_action_lifecycle_runtime_skeleton.hpp"), action_lifecycle_header)
    if action_handler_header is not None:
        _write_text(os.path.join(out_root, "include", package, "ibmw_action_handler_skeleton.hpp"), action_handler_header)
    if action_server_runtime_header is not None:
        _write_text(os.path.join(out_root, "include", package, "ibmw_action_server_runtime.hpp"), action_server_runtime_header)
    if action_contract_yaml is not None:
        _write_text(os.path.join(out_root, f"{package}_action_runtime_contract.yaml"), action_contract_yaml)
    if runtime_project_yaml is not None:
        _write_text(os.path.join(out_root, f"{package}_imported_srv_runtime.ibmw.yaml"), runtime_project_yaml)
    dependency_closure_name = f"{package}_dependency_closure.json"
    dependency_closure = _generate_package_dependency_closure(source_root, package, files)
    _write_text(os.path.join(out_root, dependency_closure_name), format_diagnosis_json(dependency_closure))
    backend_artifacts = _emit_requested_backend_metadata(
        out_root,
        package,
        files,
        sorted(targets or ["dds_ros2"]),
        mode="generate_package",
        source_root=source_root,
    )
    backend_metadata_files = backend_artifacts["metadata_files"]
    backend_generated_files = backend_artifacts["generated_files"]
    migration_manifest_name = f"{package}_migration_manifest.json"
    migration_manifest = _generate_package_migration_manifest(
        source_root,
        package,
        files,
        dependency_closure,
        dependency_closure_name=dependency_closure_name,
        targets=sorted(targets or ["dds_ros2"]),
        input_mode=input_mode,
        backend_metadata_files=backend_metadata_files,
        backend_generated_files=backend_generated_files,
    )
    _write_text(os.path.join(out_root, migration_manifest_name), format_diagnosis_json(migration_manifest))

    generated_files = ["package.xml", "CMakeLists.txt", f"include/{package}/ibmw_interface_types.hpp"]
    if conversion_header is not None:
        generated_files.append(f"include/{package}/ibmw_ros2_conversion.hpp")
    if rpc_bridge_header is not None:
        generated_files.append(f"include/{package}/ibmw_rpc_bridge_skeleton.hpp")
    if rpc_runtime_header is not None:
        generated_files.append(f"include/{package}/ibmw_rpc_bridge_runtime.hpp")
    if action_traits_header is not None:
        generated_files.append(f"include/{package}/ibmw_action_traits.hpp")
    if action_binding_header is not None:
        generated_files.append(f"include/{package}/ibmw_action_binding_skeleton.hpp")
    if action_lifecycle_header is not None:
        generated_files.append(f"include/{package}/ibmw_action_lifecycle_runtime_skeleton.hpp")
    if action_handler_header is not None:
        generated_files.append(f"include/{package}/ibmw_action_handler_skeleton.hpp")
    if action_server_runtime_header is not None:
        generated_files.append(f"include/{package}/ibmw_action_server_runtime.hpp")
    if action_contract_yaml is not None:
        generated_files.append(f"{package}_action_runtime_contract.yaml")
    if runtime_project_yaml is not None:
        generated_files.append(f"{package}_imported_srv_runtime.ibmw.yaml")
    generated_files.append(dependency_closure_name)
    generated_files.extend(backend_metadata_files)
    generated_files.extend(backend_generated_files)
    generated_files.append(migration_manifest_name)
    generated_files += emitted_files
    report = {
        "ok": True,
        "schema_version": 1,
        "artifact_only": True,
        "mode": "generate_package",
        "input_mode": input_mode,
        "package": package,
        "rosidl_targets": _rosidl_target_report(package),
        "ibmw_wrapper": _ibmw_wrapper_report(package, files),
        "source_root": source_root,
        "output_dir": out_root,
        "targets": sorted(targets or ["dds_ros2"]),
        "target_backend_status": _target_backend_status_report(sorted(targets or ["dds_ros2"]), files, backend_metadata_files),
        "backend_metadata_files": backend_metadata_files,
        "backend_generated_files": backend_generated_files,
        "files": emitted_files,
        "external_dependencies": external_packages,
        "dependency_closure": dependency_closure,
        "migration_manifest": migration_manifest,
        "verification": migration_manifest["verification"],
        "dependency_resolution": {
            "mode": "generate_package",
            "strategy": "source_tree_recursive_parse",
            "closure_manifest": dependency_closure_name,
            "closure_status": dependency_closure["status"],
            "migration_manifest": migration_manifest_name,
        },
        "generated_files": generated_files,
        "rosidl_first": True,
    }
    _write_text(os.path.join(out_root, "ibmw_interface_report.json"), format_diagnosis_json(report))
    return report


def _rosidl_target_report(package: str) -> dict:
    return {
        "generator_cpp": f"{package}::{package}__rosidl_generator_cpp",
        "typesupport_fastrtps_cpp": f"{package}::{package}__rosidl_typesupport_fastrtps_cpp",
        "typesupport_fastrtps_c": f"{package}::{package}__rosidl_typesupport_fastrtps_c",
        "typesupport_cpp": f"{package}::{package}__rosidl_typesupport_cpp",
        "typesupport_introspection_cpp": f"{package}::{package}__rosidl_typesupport_introspection_cpp",
        "typesupport_introspection_c": f"{package}::{package}__rosidl_typesupport_introspection_c",
    }


def _ibmw_wrapper_report(package: str, files: list[str]) -> dict:
    services = []
    messages = []
    actions = []
    for path in files:
        kind = _interface_kind_from_path(path)
        type_name = os.path.splitext(os.path.basename(path))[0]
        canonical = f"{package}/{kind}/{type_name}"
        if kind == "msg":
            messages.append({"name": type_name, "canonical": canonical, "alias": type_name})
        elif kind == "srv":
            ros2_service_name = f"/{_to_ros2_snake_case(type_name)}"
            services.append(
                {
                    "name": type_name,
                    "canonical": canonical,
                    "ros2_service_name": ros2_service_name,
                    "service_alias": type_name,
                    "request_alias": f"{type_name}_Request",
                    "response_alias": f"{type_name}_Response",
                    "request_dds_type_name": _ros2_srv_dds_type_name(package, type_name, "Request"),
                    "response_dds_type_name": _ros2_srv_dds_type_name(package, type_name, "Response"),
                    "runtime_glue": "not_generated",
                }
            )
        elif kind == "action":
            ros2_action_name = f"/{_to_ros2_snake_case(type_name)}"
            actions.append(
                {
                    "name": type_name,
                    "canonical": canonical,
                    "alias": type_name,
                    "default_ros2_action_name": ros2_action_name,
                    "send_goal_service": f"{ros2_action_name}/_action/send_goal",
                    "get_result_service": f"{ros2_action_name}/_action/get_result",
                    "cancel_goal_service": f"{ros2_action_name}/_action/cancel_goal",
                    "feedback_topic": f"{ros2_action_name}/_action/feedback",
                    "status_topic": f"{ros2_action_name}/_action/status",
                    "hidden_rosidl_types": {
                        "goal": f"{package}/action/{type_name}_Goal",
                        "result": f"{package}/action/{type_name}_Result",
                        "feedback": f"{package}/action/{type_name}_Feedback",
                        "send_goal_service": f"{package}/action/{type_name}_SendGoal",
                        "get_result_service": f"{package}/action/{type_name}_GetResult",
                        "cancel_goal_service": "action_msgs/srv/CancelGoal",
                        "feedback_message": f"{package}/action/{type_name}_FeedbackMessage",
                        "status_message": "action_msgs/msg/GoalStatusArray",
                    },
                    "runtime_glue": "rcl_free_server_runtime_template",
                    "traits_alias": f"{type_name}ActionTraits",
                    "binding_alias": f"{type_name}ActionBindingSkeleton",
                    "server_runtime_alias": f"{type_name}ActionServerRuntime",
                    "rejecting_runtime_alias": f"{type_name}RejectingActionServerRuntime",
                }
            )
    return {
        "header": f"include/{package}/ibmw_interface_types.hpp",
        "conversion_header": f"include/{package}/ibmw_ros2_conversion.hpp" if files else None,
        "conversion_mode": "identity_rosidl_alias" if files else "not_applicable",
        "rpc_bridge_skeleton": {
            "header": f"include/{package}/ibmw_rpc_bridge_skeleton.hpp" if services else None,
            "generated": bool(services),
            "runtime_glue_generated": False,
        },
        "rpc_bridge_runtime": {
            "header": f"include/{package}/ibmw_rpc_bridge_runtime.hpp" if services else None,
            "generated": bool(services),
            "kind": "typed_server_client_adapter_header" if services else None,
            "consumer_contract": "requires_ibmw_api_and_rosidl_typesupport_cpp" if services else None,
            "cxx_standard": "cxx20" if services else None,
            "required_rosidl_targets": [f"{package}::{package}__rosidl_typesupport_cpp"] if services else [],
            "runtime_module_generated": False,
        },
        "action_runtime": {
            "traits_header": f"include/{package}/ibmw_action_traits.hpp" if actions else None,
            "binding_skeleton_header": f"include/{package}/ibmw_action_binding_skeleton.hpp" if actions else None,
            "lifecycle_runtime_skeleton_header": f"include/{package}/ibmw_action_lifecycle_runtime_skeleton.hpp" if actions else None,
            "handler_skeleton_header": f"include/{package}/ibmw_action_handler_skeleton.hpp" if actions else None,
            "server_runtime_header": f"include/{package}/ibmw_action_server_runtime.hpp" if actions else None,
            "contract_manifest": f"{package}_action_runtime_contract.yaml" if actions else None,
            "generated": bool(actions),
            "status": "rcl_free_server_runtime_template" if actions else "not_applicable",
            "kind": "action_server_node_template_header" if actions else None,
            "runtime_module_generated": False,
            "generic_runtime_claimed": bool(actions),
            "live_runtime_claimed": False,
            "consumer_contract": "requires_ibmw_action_runtime_and_rosidl_typesupport_cpp" if actions else None,
            "required_rosidl_targets": [f"{package}::{package}__rosidl_typesupport_cpp"] if actions else [],
        },
        "runtime_module_generated": False,
        "messages": messages,
        "services": services,
        "actions": actions,
    }


def _render_ibmw_interface_types_header(package: str, files: list[str]) -> str:
    includes = []
    aliases = []
    metadata = []
    for path in files:
        kind = _interface_kind_from_path(path)
        type_name = os.path.splitext(os.path.basename(path))[0]
        snake = _to_ros2_snake_case(type_name)
        includes.append(f"#include <{package}/{kind}/{snake}.hpp>")
        if kind == "msg":
            aliases.append(f"using {type_name} = ::{package}::msg::{type_name};")
        elif kind == "srv":
            aliases.extend(
                [
                    f"using {type_name} = ::{package}::srv::{type_name};",
                    f"using {type_name}_Request = ::{package}::srv::{type_name}::Request;",
                    f"using {type_name}_Response = ::{package}::srv::{type_name}::Response;",
                ]
            )
        elif kind == "action":
            aliases.append(f"using {type_name} = ::{package}::action::{type_name};")
        metadata.append(
            "  {"
            + f'"{kind}", "{type_name}", "{package}/{kind}/{type_name}"'
            + "},"
        )
    include_block = "\n".join(sorted(includes))
    alias_block = "\n".join(aliases)
    metadata_block = "\n".join(metadata)
    return f"""// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Generated by ibmw-gen import --artifact-only.
// This header intentionally contains only type aliases and metadata for
// rosidl-generated interfaces. It does not implement runtime wire glue.

#pragma once

#include <array>

{include_block}

namespace ibmw::ros2_interfaces::{package} {{

struct InterfaceTypeMetadata {{
  const char* kind;
  const char* name;
  const char* canonical;
}};

inline constexpr const char* kPackageName = "{package}";
inline constexpr std::array<InterfaceTypeMetadata, {len(files)}> kInterfaceTypes{{{{
{metadata_block}
}}}};

{alias_block}

}}  // namespace ibmw::ros2_interfaces::{package}
"""


def _render_ibmw_ros2_conversion_header(package: str, files: list[str]) -> str | None:
    if not files:
        return None

    typed_helpers = []
    for path in files:
        kind = _interface_kind_from_path(path)
        type_name = os.path.splitext(os.path.basename(path))[0]
        if kind == "msg":
            typed_helpers.append(
                f"""
inline {type_name}& ToIbmw{type_name}({type_name}& value) noexcept {{ return value; }}
inline const {type_name}& ToIbmw{type_name}(const {type_name}& value) noexcept {{ return value; }}
inline {type_name}& ToRos2{type_name}({type_name}& value) noexcept {{ return value; }}
inline const {type_name}& ToRos2{type_name}(const {type_name}& value) noexcept {{ return value; }}
inline void Copy{type_name}Ros2Compatible({type_name}& dst, const {type_name}& src) {{ dst = src; }}
""".strip()
            )
        elif kind == "srv":
            typed_helpers.append(
                f"""
inline {type_name}_Request& ToIbmw{type_name}Request({type_name}_Request& value) noexcept {{ return value; }}
inline const {type_name}_Request& ToIbmw{type_name}Request(const {type_name}_Request& value) noexcept {{ return value; }}
inline {type_name}_Response& ToIbmw{type_name}Response({type_name}_Response& value) noexcept {{ return value; }}
inline const {type_name}_Response& ToIbmw{type_name}Response(const {type_name}_Response& value) noexcept {{ return value; }}
inline void Copy{type_name}RequestRos2Compatible({type_name}_Request& dst, const {type_name}_Request& src) {{ dst = src; }}
inline void Copy{type_name}ResponseRos2Compatible({type_name}_Response& dst, const {type_name}_Response& src) {{ dst = src; }}
""".strip()
            )
        elif kind == "action":
            typed_helpers.append(
                f"""
inline {type_name}::Goal& ToIbmw{type_name}Goal({type_name}::Goal& value) noexcept {{ return value; }}
inline const {type_name}::Goal& ToIbmw{type_name}Goal(const {type_name}::Goal& value) noexcept {{ return value; }}
inline {type_name}::Result& ToIbmw{type_name}Result({type_name}::Result& value) noexcept {{ return value; }}
inline const {type_name}::Result& ToIbmw{type_name}Result(const {type_name}::Result& value) noexcept {{ return value; }}
inline {type_name}::Feedback& ToIbmw{type_name}Feedback({type_name}::Feedback& value) noexcept {{ return value; }}
inline const {type_name}::Feedback& ToIbmw{type_name}Feedback(const {type_name}::Feedback& value) noexcept {{ return value; }}
inline void Copy{type_name}GoalRos2Compatible({type_name}::Goal& dst, const {type_name}::Goal& src) {{ dst = src; }}
inline void Copy{type_name}ResultRos2Compatible({type_name}::Result& dst, const {type_name}::Result& src) {{ dst = src; }}
inline void Copy{type_name}FeedbackRos2Compatible({type_name}::Feedback& dst, const {type_name}::Feedback& src) {{ dst = src; }}
""".strip()
            )

    helpers_block = "\n\n".join(typed_helpers)
    return f"""// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Generated by ibmw-gen import --artifact-only.
// Fast ROS2-compatible conversion helpers for imported interfaces.
//
// In the ROS2-compatible DDS lane, IBMW exposes rosidl-generated C++ types as
// aliases. Conversion is therefore intentionally zero-wrapper identity for refs
// and assignment for copies. Non-ROS2-native transport drawers should generate
// field-level specializations in separate backend-specific headers.

#pragma once

#include <memory>
#include <type_traits>
#include <utility>

#include <{package}/ibmw_interface_types.hpp>

namespace ibmw::ros2_interfaces::{package} {{

template <typename T>
constexpr T& ToIbmwRos2Compatible(T& value) noexcept {{
  return value;
}}

template <typename T>
constexpr const T& ToIbmwRos2Compatible(const T& value) noexcept {{
  return value;
}}

template <typename T>
constexpr T& ToRos2Compatible(T& value) noexcept {{
  return value;
}}

template <typename T>
constexpr const T& ToRos2Compatible(const T& value) noexcept {{
  return value;
}}

template <typename T>
void CopyRos2Compatible(T& dst, const T& src) {{
  static_assert(std::is_assignable_v<T&, const T&>, "ROS2-compatible conversion requires assignable rosidl C++ type");
  dst = src;
}}

template <typename T>
std::shared_ptr<T> MakeRos2CompatibleShared(const T& value) {{
  return std::make_shared<T>(value);
}}

{helpers_block}

}}  // namespace ibmw::ros2_interfaces::{package}
"""


def _render_ibmw_rpc_bridge_runtime_header(package: str, files: list[str]) -> str | None:
    services = []
    for path in files:
        if _interface_kind_from_path(path) != "srv":
            continue
        type_name = os.path.splitext(os.path.basename(path))[0]
        services.append((type_name, f"{package}/srv/{type_name}"))
    if not services:
        return None

    adapters = []
    for type_name, canonical in services:
        adapters.append(
            f"""
struct {type_name}RpcBridgeRuntime : {type_name}RpcBridgeSkeleton {{
  static constexpr bool kTypedRegistrationGenerated = true;
  static constexpr bool kRuntimeGlueGenerated = false;
  static constexpr const char* kRpcType = "ros2";
  static constexpr const char* kServiceGroup = "{package}/srv";
  static constexpr const char* kFuncName = "{type_name}";

  static const void* ServiceTypeSupport() {{
    return rosidl_typesupport_cpp::get_service_type_support_handle<Service>();
  }}

  static const ibmw_type_support_base_t* RequestTypeSupport() {{
    return ibmw::GetRos2MessageTypeSupport<Request>();
  }}

  static const ibmw_type_support_base_t* ResponseTypeSupport() {{
    return ibmw::GetRos2MessageTypeSupport<Response>();
  }}
}};

class {type_name}SyncServiceAdapter : public ibmw::rpc::ServiceBase {{
 public:
  using Request = {type_name}RpcBridgeRuntime::Request;
  using Response = {type_name}RpcBridgeRuntime::Response;

  {type_name}SyncServiceAdapter() : ibmw::rpc::ServiceBase({type_name}RpcBridgeRuntime::kRpcType,
                                                            {type_name}RpcBridgeRuntime::kServiceGroup) {{
    ibmw::rpc::ServiceFunc service_callback(
        [this](const ibmw_service_context_base_t* ctx, const void* req, void* rsp,
               ibmw_function_base_t* result_callback_ptr) {{
          ibmw::rpc::ServiceCallback result_callback(result_callback_ptr);
          ibmw::rpc::ContextRef ctx_ref(ctx);
          auto status = Handle{type_name}(
              ctx_ref,
              *static_cast<const Request*>(req),
              *static_cast<Response*>(rsp));
          result_callback(status.Code());
        }});
    RegisterServiceFunc(
        {type_name}RpcBridgeRuntime::kFuncName,
        {type_name}RpcBridgeRuntime::ServiceTypeSupport(),
        {type_name}RpcBridgeRuntime::RequestTypeSupport(),
        {type_name}RpcBridgeRuntime::ResponseTypeSupport(),
        std::move(service_callback));
  }}

  ~{type_name}SyncServiceAdapter() override = default;

  virtual ibmw::rpc::Status Handle{type_name}(
      ibmw::rpc::ContextRef ctx_ref,
      const Request& request,
      Response& response) {{
    (void)ctx_ref;
    (void)request;
    (void)response;
    return ibmw::rpc::Status(IBMW_RPC_STATUS_SVR_NOT_IMPLEMENTED);
  }}
}};

inline bool Register{type_name}ClientFunc(ibmw::rpc::ServiceRef service_ref,
                                          std::string_view service_name) {{
  return service_ref.RegisterClientFunc(
      {type_name}RpcBridgeRuntime::kRpcType,
      service_name,
      {type_name}RpcBridgeRuntime::kFuncName,
      {type_name}RpcBridgeRuntime::ServiceTypeSupport(),
      {type_name}RpcBridgeRuntime::RequestTypeSupport(),
      {type_name}RpcBridgeRuntime::ResponseTypeSupport());
}}

inline bool Register{type_name}ClientFunc(ibmw::rpc::ServiceRef service_ref) {{
  return Register{type_name}ClientFunc(service_ref, {type_name}RpcBridgeRuntime::kServiceGroup);
}}

class {type_name}SyncClientAdapter : public ibmw::rpc::SyncAccessBase {{
 public:
  using Request = {type_name}RpcBridgeRuntime::Request;
  using Response = {type_name}RpcBridgeRuntime::Response;

  explicit {type_name}SyncClientAdapter(ibmw::rpc::ServiceRef service_ref)
      : ibmw::rpc::SyncAccessBase(service_ref,
                                  {type_name}RpcBridgeRuntime::kRpcType,
                                  {type_name}RpcBridgeRuntime::kServiceGroup) {{}}

  {type_name}SyncClientAdapter(ibmw::rpc::ServiceRef service_ref, std::string_view service_name)
      : ibmw::rpc::SyncAccessBase(service_ref,
                                  {type_name}RpcBridgeRuntime::kRpcType,
                                  service_name) {{}}

  ibmw::rpc::Status {type_name}(
      ibmw::rpc::ContextRef ctx_ref,
      const Request& request,
      Response& response) {{
    const std::string full_func_name = ibmw::rpc::GetFullFuncName(
        rpc_type_, service_name_, {type_name}RpcBridgeRuntime::kFuncName);
    return Call(full_func_name, ctx_ref, request, response);
  }}
}};
""".strip()
        )

    adapters_block = "\n\n".join(adapters)
    return f"""// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Generated by ibmw-gen import --artifact-only.
// Typed ROS2 service registration helpers for rosidl-generated services.
// This header provides server adapter classes, but does not generate an IBMW
// runtime module or business handler implementation.
// Consumer contract: include this header only from an IBMW-aware target that
// exposes IBMW public API headers and rosidl_typesupport_cpp, for example a
// target linked through ibmw_add_rosidl_interface_target(...) plus the normal
// IBMW package/module targets and {package}::{package}__rosidl_typesupport_cpp.
// C++20 is required through the IBMW C++ API. Artifact-only rosidl package
// builds do not compile this header by themselves.

#pragma once

#include <utility>

#include <rosidl_typesupport_cpp/service_type_support.hpp>

#include <string>
#include <string_view>

#include <{package}/ibmw_rpc_bridge_skeleton.hpp>

#include <cpp/ibmw_sdk.h>
#include <ros2_types/util/ros2_type_support_factory.h>

namespace ibmw::ros2_interfaces::{package} {{

{adapters_block}

}}  // namespace ibmw::ros2_interfaces::{package}
"""


def _render_ibmw_imported_srv_runtime_project_yaml(
    package: str,
    files: list[str],
    adapter_include_dir: str | None = None,
) -> str | None:
    services = []
    for path in files:
        if _interface_kind_from_path(path) != "srv":
            continue
        services.append(os.path.splitext(os.path.basename(path))[0])
    if not services:
        return None

    module_lines = []
    app_lines = []
    include_dir_line = f"\n    ros2_imported_srv_adapter_include_dir: {adapter_include_dir}" if adapter_include_dir else ""
    for service in services:
        server_module = f"{service}RpcServerModule"
        client_module = f"{service}RpcClientModule"
        module_lines.append(f"""  - name: {server_module}
    type: rpc_server
    transport: ros2
    ros2_package: {package}
    ros2_service: {service}
    ros2_service_name: /{_to_ros2_snake_case(service)}
    ros2_imported_srv_adapter: true{include_dir_line}
  - name: {client_module}
    type: rpc_client
    transport: ros2
    ros2_package: {package}
    ros2_service: {service}
    ros2_service_name: /{_to_ros2_snake_case(service)}
    ros2_imported_srv_adapter: true{include_dir_line}
    custom_config:
      rpc_frq: 1.0""")
        app_snake = _to_ros2_snake_case(service)
        app_lines.append(f"""  - name: {app_snake}_imported_srv_server_app
    type: rpc
    modules:
      - {server_module}
  - name: {app_snake}_imported_srv_client_app
    type: rpc
    modules:
      - {client_module}""")

    return f"""# Generated by ibmw-gen import --artifact-only.
# Optional IBMW runtime project skeleton for imported ROS2 .srv adapters.
#
# This file is intentionally conservative: it wires generated rpc_server and
# rpc_client modules to the imported <{package}/ibmw_rpc_bridge_runtime.hpp>
# adapter path, but it does not implement business handlers or request field
# population. Build it with CMAKE_PREFIX_PATH containing both IBMW and this
# rosidl artifact package install prefix.

schema_version: 2
project:
  name: {package}_imported_srv_runtime
  namespace: {package}_imported_srv_runtime

modules:
{chr(10).join(module_lines)}

apps:
{chr(10).join(app_lines)}
"""


def _render_ibmw_action_runtime_contract_yaml(package: str, files: list[str]) -> str | None:
    actions = []
    for path in files:
        if _interface_kind_from_path(path) != "action":
            continue
        actions.append(os.path.splitext(os.path.basename(path))[0])
    if not actions:
        return None

    action_blocks = []
    for action in actions:
        action_name = f"/{_to_ros2_snake_case(action)}"
        action_blocks.append(f"""  - name: {action}
    canonical: {package}/action/{action}
    default_ros2_action_name: {action_name}
    runtime_status: rcl_free_server_runtime_template
    generic_runtime_claimed: true
    runtime_module_generated: false
    endpoints:
      send_goal_service: {action_name}/_action/send_goal
      get_result_service: {action_name}/_action/get_result
      cancel_goal_service: {action_name}/_action/cancel_goal
      feedback_topic: {action_name}/_action/feedback
      status_topic: {action_name}/_action/status
    hidden_rosidl_types:
      goal: {package}/action/{action}_Goal
      result: {package}/action/{action}_Result
      feedback: {package}/action/{action}_Feedback
      send_goal_service: {package}/action/{action}_SendGoal
      get_result_service: {package}/action/{action}_GetResult
      cancel_goal_service: action_msgs/srv/CancelGoal
      feedback_message: {package}/action/{action}_FeedbackMessage
      status_message: action_msgs/msg/GoalStatusArray
    generated_traits_alias: {action}ActionTraits
    generated_binding_alias: {action}ActionBindingSkeleton
    generated_server_runtime_alias: {action}ActionServerRuntime
    generated_rejecting_runtime_alias: {action}RejectingActionServerRuntime""")

    return f"""# Generated by ibmw-gen import --artifact-only.
# RCL-free runtime-template contract manifest for imported ROS2 .action interfaces.
#
# This manifest records the generated ActionServerNode template bridge. It is
# not an ibmw-gen project config and does not generate business Hook/Binding
# implementations or claim arbitrary action live runtime before live gates pass.

schema_version: 1
kind: ros2_action_rcl_free_server_runtime_template_contract
package: {package}
traits_header: include/{package}/ibmw_action_traits.hpp
binding_skeleton_header: include/{package}/ibmw_action_binding_skeleton.hpp
lifecycle_runtime_skeleton_header: include/{package}/ibmw_action_lifecycle_runtime_skeleton.hpp
handler_skeleton_header: include/{package}/ibmw_action_handler_skeleton.hpp
server_runtime_header: include/{package}/ibmw_action_server_runtime.hpp
required_rosidl_targets:
  - {package}::{package}__rosidl_typesupport_cpp
runtime_status: rcl_free_server_runtime_template
generic_runtime_claimed: true
runtime_module_generated: false
live_runtime_claimed: false
consumer_contract: requires_ibmw_action_runtime_and_rosidl_typesupport_cpp
actions:
{chr(10).join(action_blocks)}
notes:
  - diagnosis and import cover parse/dependency/metadata readiness only
  - generated server runtime uses IBMW ActionServerNode with DDS-only 3 services + 2 topics
  - generated rejecting runtime can start DDS endpoints and reject goals until business Hook is supplied
  - action-specific Hook and ActionServerBinding field mapping remain user-owned
  - live_runtime_claimed remains false until peer live gates pass
"""


def _render_ibmw_action_traits_header(package: str, files: list[str]) -> str | None:
    actions = []
    for path in files:
        if _interface_kind_from_path(path) != "action":
            continue
        type_name = os.path.splitext(os.path.basename(path))[0]
        actions.append(type_name)
    if not actions:
        return None

    includes = []
    fastdds_specializations = []
    structs = []
    for type_name in actions:
        canonical = f"{package}/action/{type_name}"
        ros2_action_name = f"/{_to_ros2_snake_case(type_name)}"
        action_cpp = f"::{package}::action::{type_name}"
        ts_ns = f"::{package}::action::typesupport_fastrtps_cpp"
        snake = _to_ros2_snake_case(type_name)
        includes.append(f"#include <{package}/action/detail/{snake}__rosidl_typesupport_fastrtps_cpp.hpp>")
        for impl_path, suffix in (
            ("Impl::SendGoalService::Request", "SendGoal_Request"),
            ("Impl::SendGoalService::Response", "SendGoal_Response"),
            ("Impl::GetResultService::Request", "GetResult_Request"),
            ("Impl::GetResultService::Response", "GetResult_Response"),
            ("Impl::FeedbackMessage", "FeedbackMessage"),
        ):
            cpp_type = f"{action_cpp}::{impl_path}"
            dds_name = f"{package}::action::dds_::{type_name}_{suffix}_"
            fastdds_specializations.append(
                f"""
template <>
struct Ros2FastTypeTraits<{cpp_type}> {{
  static constexpr const char* kName = "{dds_name}";
  static bool Serialize(const {cpp_type}& msg, eprosima::fastcdr::Cdr& cdr) {{
    return {ts_ns}::cdr_serialize(msg, cdr);
  }}
  static bool Deserialize(eprosima::fastcdr::Cdr& cdr, {cpp_type}& msg) {{
    return {ts_ns}::cdr_deserialize(cdr, msg);
  }}
  static size_t Size(const {cpp_type}& msg) {{
    return {ts_ns}::get_serialized_size(msg, 0);
  }}
}};
""".strip()
            )
        structs.append(
            f"""
struct {type_name}ActionTraits {{
  using Action = {type_name};
  using Goal = Action::Goal;
  using Result = Action::Result;
  using Feedback = Action::Feedback;
  using SendGoalService = Action::Impl::SendGoalService;
  using SendGoalRequest = Action::Impl::SendGoalService::Request;
  using SendGoalResponse = Action::Impl::SendGoalService::Response;
  using GetResultService = Action::Impl::GetResultService;
  using GetResultRequest = Action::Impl::GetResultService::Request;
  using GetResultResponse = Action::Impl::GetResultService::Response;
  using CancelGoalService = Action::Impl::CancelGoalService;
  using CancelGoalRequest = Action::Impl::CancelGoalService::Request;
  using CancelGoalResponse = Action::Impl::CancelGoalService::Response;
  using FeedbackMessage = Action::Impl::FeedbackMessage;
  using StatusMessage = Action::Impl::GoalStatusMessage;
  using GoalId = ::unique_identifier_msgs::msg::UUID;

  static constexpr const char* kCanonicalAction = "{canonical}";
  static constexpr const char* kGoalType = "{package}/action/{type_name}_Goal";
  static constexpr const char* kResultType = "{package}/action/{type_name}_Result";
  static constexpr const char* kFeedbackType = "{package}/action/{type_name}_Feedback";
  static constexpr const char* kSendGoalServiceType = "{package}/action/{type_name}_SendGoal";
  static constexpr const char* kGetResultServiceType = "{package}/action/{type_name}_GetResult";
  static constexpr const char* kCancelGoalServiceType = "action_msgs/srv/CancelGoal";
  static constexpr const char* kFeedbackMessageType = "{package}/action/{type_name}_FeedbackMessage";
  static constexpr const char* kStatusMessageType = "action_msgs/msg/GoalStatusArray";
  static constexpr const char* kRos2ActionName = "{ros2_action_name}";
  static constexpr const char* kSendGoalService = "{ros2_action_name}/_action/send_goal";
  static constexpr const char* kGetResultService = "{ros2_action_name}/_action/get_result";
  static constexpr const char* kCancelGoalService = "{ros2_action_name}/_action/cancel_goal";
  static constexpr const char* kFeedbackTopic = "{ros2_action_name}/_action/feedback";
  static constexpr const char* kStatusTopic = "{ros2_action_name}/_action/status";
  static constexpr bool kRuntimeGenerated = false;
  static constexpr const char* kRuntimeStatus = "compile_skeleton";
}};
""".strip()
        )

    includes_block = "\n".join(includes)
    fastdds_block = "\n\n".join(fastdds_specializations)
    structs_block = "\n\n".join(structs)
    return f"""// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Generated by ibmw-gen import --artifact-only.
// Action traits metadata and FastDDS wire type support for rosidl-generated actions.
// This header does not implement action-specific business logic.

#pragma once

#include <{package}/ibmw_interface_types.hpp>
{includes_block}
#include <cstddef>
#include <fastcdr/Cdr.h>
#include <unique_identifier_msgs/msg/uuid.hpp>

#include "internal/ros2_action_dds/fastdds_typesupport.h"

namespace ibmw::internal::ros2_action_dds {{

{fastdds_block}

}}  // namespace ibmw::internal::ros2_action_dds

namespace ibmw::ros2_interfaces::{package} {{

{structs_block}

}}  // namespace ibmw::ros2_interfaces::{package}
"""


def _render_ibmw_action_binding_skeleton_header(package: str, files: list[str]) -> str | None:
    actions = []
    for path in files:
        if _interface_kind_from_path(path) != "action":
            continue
        actions.append(os.path.splitext(os.path.basename(path))[0])
    if not actions:
        return None

    structs = []
    for type_name in actions:
        structs.append(
            f"""
struct {type_name}ActionBindingSkeleton {{
  using Traits = {type_name}ActionTraits;
  using Action = typename Traits::Action;
  using Goal = typename Traits::Goal;
  using Result = typename Traits::Result;
  using Feedback = typename Traits::Feedback;

  static constexpr bool kBindingSkeletonGenerated = true;
  static constexpr bool kRuntimeGlueGenerated = false;
  static constexpr const char* kRuntimeStatus = "compile_skeleton";

  // User-owned future mapping points. They are intentionally declaration-only
  // until generic Action runtime acceptance gates exist and pass.
  struct GoalInput;
  struct FeedbackPayload;
  struct ResultPayload;
}};
""".strip()
        )

    structs_block = "\n\n".join(structs)
    return f"""// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Generated by ibmw-gen import --artifact-only.
// Compile-only Action binding skeleton metadata for rosidl-generated actions.
// This header deliberately avoids claiming generic .action runtime support.

#pragma once

#include <{package}/ibmw_action_traits.hpp>

namespace ibmw::ros2_interfaces::{package} {{

{structs_block}

}}  // namespace ibmw::ros2_interfaces::{package}
"""


def _render_ibmw_action_lifecycle_runtime_skeleton_header(package: str, files: list[str]) -> str | None:
    actions = []
    for path in files:
        if _interface_kind_from_path(path) != "action":
            continue
        actions.append(os.path.splitext(os.path.basename(path))[0])
    if not actions:
        return None

    structs = []
    for type_name in actions:
        structs.append(
            f"""
struct {type_name}ActionLifecycleRuntimeSkeleton {{
  using Binding = {type_name}ActionBindingSkeleton;
  using Traits = typename Binding::Traits;
  using Goal = typename Traits::Goal;
  using Result = typename Traits::Result;
  using Feedback = typename Traits::Feedback;
  using SendGoalRequest = typename Traits::SendGoalRequest;
  using SendGoalResponse = typename Traits::SendGoalResponse;
  using GetResultRequest = typename Traits::GetResultRequest;
  using GetResultResponse = typename Traits::GetResultResponse;
  using CancelGoalRequest = typename Traits::CancelGoalRequest;
  using CancelGoalResponse = typename Traits::CancelGoalResponse;
  using FeedbackMessage = typename Traits::FeedbackMessage;
  using StatusMessage = typename Traits::StatusMessage;

  static constexpr bool kLifecycleSkeletonGenerated = true;
  static constexpr bool kRuntimeGlueGenerated = false;
  static constexpr bool kLiveRuntimeClaimed = false;
  static constexpr const char* kRuntimeStatus = "compile_skeleton";

  static constexpr std::array<std::string_view, 5> kLifecycleStages{{{{
      "send_goal", "get_result", "cancel_goal", "feedback", "status"}}}};
  static constexpr std::array<std::string_view, 5> kEndpointNames{{{{
      Traits::kSendGoalService,
      Traits::kGetResultService,
      Traits::kCancelGoalService,
      Traits::kFeedbackTopic,
      Traits::kStatusTopic}}}};

  static constexpr std::string_view EndpointForStage(ActionLifecycleStage stage) {{
    switch (stage) {{
      case ActionLifecycleStage::kSendGoal:
        return Traits::kSendGoalService;
      case ActionLifecycleStage::kGetResult:
        return Traits::kGetResultService;
      case ActionLifecycleStage::kCancelGoal:
        return Traits::kCancelGoalService;
      case ActionLifecycleStage::kFeedback:
        return Traits::kFeedbackTopic;
      case ActionLifecycleStage::kStatus:
        return Traits::kStatusTopic;
    }}
    return {{}};
  }}
}};
""".strip()
        )

    structs_block = "\n\n".join(structs)
    return f"""// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Generated by ibmw-gen import --artifact-only.
// Compile-only Action lifecycle runtime skeleton for rosidl-generated actions.
// This header encodes the send_goal/get_result/cancel/feedback/status contract
// but deliberately does not implement or claim generic live Action runtime.

#pragma once

#include <array>
#include <string_view>

#include <{package}/ibmw_action_binding_skeleton.hpp>

namespace ibmw::ros2_interfaces::{package} {{

enum class ActionLifecycleStage {{
  kSendGoal,
  kGetResult,
  kCancelGoal,
  kFeedback,
  kStatus,
}};

{structs_block}

}}  // namespace ibmw::ros2_interfaces::{package}
"""


def _render_ibmw_action_handler_skeleton_header(package: str, files: list[str]) -> str | None:
    actions = []
    for path in files:
        if _interface_kind_from_path(path) != "action":
            continue
        actions.append(os.path.splitext(os.path.basename(path))[0])
    if not actions:
        return None

    structs = []
    for type_name in actions:
        structs.append(
            f"""
struct {type_name}ActionHandlerSkeleton {{
  using Runtime = {type_name}ActionLifecycleRuntimeSkeleton;
  using Traits = typename Runtime::Traits;
  using Goal = typename Traits::Goal;
  using Result = typename Traits::Result;
  using Feedback = typename Traits::Feedback;
  using SendGoalRequest = typename Traits::SendGoalRequest;
  using SendGoalResponse = typename Traits::SendGoalResponse;
  using GetResultRequest = typename Traits::GetResultRequest;
  using GetResultResponse = typename Traits::GetResultResponse;
  using CancelGoalRequest = typename Traits::CancelGoalRequest;
  using CancelGoalResponse = typename Traits::CancelGoalResponse;
  using FeedbackMessage = typename Traits::FeedbackMessage;
  using StatusMessage = typename Traits::StatusMessage;

  static constexpr bool kHandlerSkeletonGenerated = true;
  static constexpr bool kBusinessLogicGenerated = false;
  static constexpr bool kRuntimeGlueGenerated = false;
  static constexpr bool kLiveRuntimeClaimed = false;
  static constexpr const char* kRuntimeStatus = "compile_skeleton";

  enum class Decision {{
    kAccepted,
    kRejected,
    kNotImplemented,
  }};

  struct SendGoalOutcome {{
    Decision decision{{Decision::kNotImplemented}};
    SendGoalResponse response{{}};
  }};

  struct GetResultOutcome {{
    Decision decision{{Decision::kNotImplemented}};
    GetResultResponse response{{}};
  }};

  struct CancelGoalOutcome {{
    Decision decision{{Decision::kNotImplemented}};
    CancelGoalResponse response{{}};
  }};

  struct FeedbackOutcome {{
    Decision decision{{Decision::kNotImplemented}};
    FeedbackMessage message{{}};
  }};

  struct StatusOutcome {{
    Decision decision{{Decision::kNotImplemented}};
    StatusMessage message{{}};
  }};

  SendGoalOutcome OnSendGoal(const SendGoalRequest& request) {{
    (void)request;
    return {{}};
  }}

  GetResultOutcome OnGetResult(const GetResultRequest& request) {{
    (void)request;
    return {{}};
  }}

  CancelGoalOutcome OnCancelGoal(const CancelGoalRequest& request) {{
    (void)request;
    return {{}};
  }}

  FeedbackOutcome OnFeedback() {{
    return {{}};
  }}

  StatusOutcome OnStatus() {{
    return {{}};
  }}
}};
""".strip()
        )

    structs_block = "\n\n".join(structs)
    return f"""// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Generated by ibmw-gen import --artifact-only.
// Compile-only Action handler skeleton for rosidl-generated actions.
// This header provides user-owned handler entry points for the five action
// lifecycle stages, but does not generate business logic or live runtime glue.

#pragma once

#include <{package}/ibmw_action_lifecycle_runtime_skeleton.hpp>

namespace ibmw::ros2_interfaces::{package} {{

{structs_block}

}}  // namespace ibmw::ros2_interfaces::{package}
"""


def _render_ibmw_action_server_runtime_header(package: str, files: list[str]) -> str | None:
    actions = []
    for path in files:
        if _interface_kind_from_path(path) != "action":
            continue
        actions.append(os.path.splitext(os.path.basename(path))[0])
    if not actions:
        return None

    runtime_blocks = []
    binding_blocks = []
    alias_blocks = []
    for type_name in actions:
        runtime_blocks.append(
            f"""
struct {type_name}RejectingHook {{
  using GoalInput = std::monostate;
  using FeedbackPayload = std::monostate;
  using ResultPayload = std::monostate;

  struct ExecState {{}};

  static bool ValidateGoal(const GoalInput&) {{ return false; }}
  static ExecState InitState(const GoalInput&) {{ return {{}}; }}
  static ::ibmw::action::TickOutcome Tick(ExecState&) {{
    return ::ibmw::action::TickOutcome::kAborted;
  }}
  static FeedbackPayload BuildFeedback(const ExecState&) {{ return {{}}; }}
  static ResultPayload BuildResult(const ExecState&) {{ return {{}}; }}
}};

template <typename Hook,
          typename Binding = ::ibmw::action::ActionServerBinding<{type_name}ActionTraits, Hook>>
using {type_name}ActionServerNode = ::ibmw::action::ActionServerNode<{type_name}ActionTraits, Hook, Binding>;

template <typename Hook,
          typename Binding = ::ibmw::action::ActionServerBinding<{type_name}ActionTraits, Hook>>
class {type_name}ActionServerRuntime {{
 public:
  using Traits = {type_name}ActionTraits;
  using ServerNode = {type_name}ActionServerNode<Hook, Binding>;

  static constexpr bool kRuntimeTemplateGenerated = true;
  static constexpr bool kRclFreeRuntime = true;
  static constexpr bool kUsesRos2Cli = false;
  static constexpr bool kUsesRcl = false;
  static constexpr bool kLiveRuntimeClaimed = false;
  static constexpr const char* kRuntimeStatus = "rcl_free_server_runtime_template";
  static constexpr const char* kConsumerContract =
      "provide Hook and ActionServerBinding specialization, then instantiate Start/Stop";

  explicit {type_name}ActionServerRuntime(std::string action_name = Traits::kRos2ActionName)
      : server_(std::move(action_name)) {{}}

  bool Start(uint32_t domain_id = 0U) {{ return server_.Start(domain_id); }}
  void Stop() {{ server_.Stop(); }}

  ServerNode& server() {{ return server_; }}
  const ServerNode& server() const {{ return server_; }}

 private:
  ServerNode server_;
}};
""".strip()
        )
        binding_blocks.append(
            f"""
template <>
struct ActionServerBinding<
    ::ibmw::ros2_interfaces::{package}::{type_name}ActionTraits,
    ::ibmw::ros2_interfaces::{package}::{type_name}RejectingHook> {{
  using Traits = ::ibmw::ros2_interfaces::{package}::{type_name}ActionTraits;
  using Hook = ::ibmw::ros2_interfaces::{package}::{type_name}RejectingHook;

  static Hook::GoalInput GoalInputOf(const Traits::SendGoalRequest&) {{ return {{}}; }}

  static Traits::GetResultResponse ToGetResultResponse(int8_t status,
                                                       const Hook::ResultPayload&) {{
    Traits::GetResultResponse response;
    response.status = status;
    return response;
  }}

  static Traits::FeedbackMessage ToFeedbackMessage(const Traits::GoalId& goal_id,
                                                   const Hook::FeedbackPayload&) {{
    Traits::FeedbackMessage message;
    message.goal_id = goal_id;
    return message;
  }}
}};

template <>
inline constexpr bool kActionServerBindingSpecialized<
    ::ibmw::ros2_interfaces::{package}::{type_name}ActionTraits,
    ::ibmw::ros2_interfaces::{package}::{type_name}RejectingHook> = true;
""".strip()
        )
        alias_blocks.append(
            f"using {type_name}RejectingActionServerRuntime = {type_name}ActionServerRuntime<{type_name}RejectingHook>;"
        )

    runtime_block = "\n\n".join(runtime_blocks)
    binding_block = "\n\n".join(binding_blocks)
    alias_block = "\n".join(alias_blocks)
    return f"""// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Generated by ibmw-gen import --artifact-only.
// RCL-free ROS2 Action server runtime template for rosidl-generated actions.
// This header wires generated action traits to ibmw::action::ActionServerNode,
// which owns the DDS-only send_goal/get_result/cancel services and
// feedback/status publishers.  Users still provide the action-specific Hook and
// ActionServerBinding field mapping before live runtime is claimed.

#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <variant>

#include "cpp/action/action_server_binding.h"
#include "cpp/action/action_server_node.h"

#include <{package}/ibmw_action_traits.hpp>

namespace ibmw::ros2_interfaces::{package} {{

{runtime_block}

}}  // namespace ibmw::ros2_interfaces::{package}

namespace ibmw::action {{

{binding_block}

}}  // namespace ibmw::action

namespace ibmw::ros2_interfaces::{package} {{

{alias_block}

}}  // namespace ibmw::ros2_interfaces::{package}
"""


def _render_ibmw_rpc_bridge_skeleton_header(package: str, files: list[str]) -> str | None:
    services = []
    for path in files:
        if _interface_kind_from_path(path) != "srv":
            continue
        type_name = os.path.splitext(os.path.basename(path))[0]
        services.append((type_name, f"{package}/srv/{type_name}"))
    if not services:
        return None

    structs = []
    for type_name, canonical in services:
        ros2_service_name = f"/{_to_ros2_snake_case(type_name)}"
        request_dds_type_name = _ros2_srv_dds_type_name(package, type_name, "Request")
        response_dds_type_name = _ros2_srv_dds_type_name(package, type_name, "Response")
        structs.append(
            f"""
struct {type_name}RpcBridgeSkeleton {{
  using Service = {type_name};
  using Request = {type_name}_Request;
  using Response = {type_name}_Response;

  static constexpr const char* kCanonicalService = "{canonical}";
  static constexpr const char* kRos2ServiceName = "{ros2_service_name}";
  static constexpr const char* kRequestDdsTypeName = "{request_dds_type_name}";
  static constexpr const char* kResponseDdsTypeName = "{response_dds_type_name}";
  static constexpr bool kRuntimeGlueGenerated = false;
}};
""".strip()
        )

    structs_block = "\n\n".join(structs)
    return f"""// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// Generated by ibmw-gen import --artifact-only.
// Compile-only RPC bridge skeleton metadata for rosidl-generated services.
// This header does not implement server/client runtime glue.

#pragma once

#include <{package}/ibmw_interface_types.hpp>

namespace ibmw::ros2_interfaces::{package} {{

{structs_block}

}}  // namespace ibmw::ros2_interfaces::{package}
"""


def _discover_local_interface_files(root: str) -> list[str]:
    files = []
    for kind, ext in (("msg", ".msg"), ("srv", ".srv"), ("action", ".action")):
        base = os.path.join(root, kind)
        if not os.path.isdir(base):
            continue
        for name in sorted(os.listdir(base)):
            path = os.path.join(base, name)
            if os.path.isfile(path) and name.endswith(ext):
                files.append(path)
    return files


def _discover_workspace_interface_packages(root: str) -> list[str]:
    candidates = []
    search_base = os.path.join(root, "src") if os.path.isdir(os.path.join(root, "src")) else root
    for name in sorted(os.listdir(search_base)):
        path = os.path.join(search_base, name)
        if os.path.isdir(path) and _discover_local_interface_files(path):
            candidates.append(path)
    return candidates


def _resolve_existing_ros2_package_root(prefix: str, package: str) -> str:
    candidates = [
        os.path.join(prefix, "share", package),
        os.path.join(prefix, package),
    ]
    for candidate in candidates:
        if os.path.isdir(candidate):
            return candidate
    raise InterfaceParseError(f"ROS2 package {package} not found under prefix {prefix}")


def _discover_existing_interface_package_roots(prefix: str) -> list[tuple[str, str]]:
    share_root = os.path.join(prefix, "share")
    if not os.path.isdir(share_root):
        return []
    packages = []
    for name in sorted(os.listdir(share_root)):
        if not re.match(r"^[A-Za-z][A-Za-z0-9_]*$", name):
            continue
        package_root = os.path.join(share_root, name)
        if os.path.isdir(package_root) and _discover_local_interface_files(package_root):
            packages.append((name, package_root))
    return packages


def _select_existing_interface_files(
    package_root: str,
    package_name: str,
    selected_types: list[str] | None,
) -> list[str]:
    files = _discover_local_interface_files(package_root)
    if not selected_types:
        return files

    by_rel = {_interface_relative_path(path, package_root): path for path in files}
    available_types = _format_available_reference_types(by_rel)
    by_short: dict[str, list[str]] = {}
    for rel, path in by_rel.items():
        kind, filename = rel.split("/", 1)
        type_name = os.path.splitext(os.path.basename(filename))[0]
        by_short.setdefault(type_name, []).append(path)
        by_short.setdefault(f"{kind}/{type_name}", []).append(path)
        by_short.setdefault(f"{package_name}/{kind}/{type_name}", []).append(path)

    selected = []
    seen = set()
    for spec in selected_types:
        key = _normalize_reference_type_spec(spec, package_name)
        matches = by_short.get(key, [])
        if not matches:
            raise InterfaceParseError(
                f"ROS2 interface type {spec} not found in installed package {package_name}; "
                f"available types: {available_types}"
            )
        unique_matches = sorted(set(matches))
        if len(unique_matches) > 1:
            raise InterfaceParseError(
                f"ROS2 interface type {spec} is ambiguous in package {package_name}; "
                f"use msg/<Type>, srv/<Type>, or action/<Type>; available types: {available_types}"
            )
        path = unique_matches[0]
        if path not in seen:
            selected.append(path)
            seen.add(path)
    return selected


def _format_available_reference_types(by_rel: dict[str, str]) -> str:
    selectors = []
    for rel in sorted(by_rel):
        kind, filename = rel.split("/", 1)
        selectors.append(f"{kind}/{os.path.splitext(os.path.basename(filename))[0]}")
    return ", ".join(selectors) if selectors else "(none)"


def _available_reference_types_report(files: list[str], package_root: str, package_name: str) -> list[dict[str, str]]:
    entries = []
    for path in sorted(files, key=lambda item: _interface_relative_path(item, package_root)):
        rel = _interface_relative_path(path, package_root)
        kind, filename = rel.split("/", 1)
        type_name = os.path.splitext(os.path.basename(filename))[0]
        entries.append(
            {
                "name": type_name,
                "kind": kind,
                "selector": f"{kind}/{type_name}",
                "canonical": f"{package_name}/{kind}/{type_name}",
                "relative_path": rel,
            }
        )
    return entries


def _normalize_reference_type_spec(spec: str, package_name: str) -> str:
    value = spec.strip().replace("\\", "/")
    for ext in (".msg", ".srv", ".action"):
        if value.endswith(ext):
            value = value[: -len(ext)]
            break
    parts = [part for part in value.split("/") if part]
    if len(parts) == 1:
        return parts[0]
    if len(parts) == 2 and parts[0] in {"msg", "srv", "action"}:
        return f"{parts[0]}/{parts[1]}"
    if len(parts) == 3 and parts[1] in {"msg", "srv", "action"}:
        if parts[0] != package_name:
            raise InterfaceParseError(
                f"ROS2 interface type {spec} belongs to package {parts[0]}, expected {package_name}"
            )
        return f"{package_name}/{parts[1]}/{parts[2]}"
    raise InterfaceParseError(
        f"unsupported ROS2 interface type selector: {spec}; use Type, msg/Type, srv/Type, action/Type, or <package>/<kind>/<Type>"
    )


def _interface_relative_path(path: str, root: str) -> str:
    try:
        return os.path.relpath(path, root).replace(os.sep, "/")
    except ValueError:
        kind = _interface_kind_from_path(path)
        return f"{kind}/{os.path.basename(path)}"


def _to_ros2_snake_case(name: str) -> str:
    result = []
    for idx, ch in enumerate(name):
        if ch.isupper() and idx > 0:
            prev = name[idx - 1]
            if prev.islower() or prev.isdigit():
                result.append("_")
            elif idx + 1 < len(name) and name[idx + 1].islower():
                result.append("_")
        result.append(ch.lower())
    return "".join(result)


def _ros2_srv_dds_type_name(package: str, service_name: str, suffix: str) -> str:
    return f"{package}::srv::dds_::{service_name}_{suffix}_"


def _interface_kind_from_path(path: str) -> str:
    parent = os.path.basename(os.path.dirname(path))
    if parent in {"msg", "srv", "action"}:
        return parent
    suffix = os.path.splitext(path)[1]
    if suffix == ".msg":
        return "msg"
    if suffix == ".srv":
        return "srv"
    if suffix == ".action":
        return "action"
    raise InterfaceParseError(f"unsupported ROS2 interface file extension: {path}")


def _infer_single_file_package_name(path: str) -> str:
    parent = os.path.basename(os.path.dirname(path))
    if parent in {"msg", "srv", "action"}:
        package = os.path.basename(os.path.dirname(os.path.dirname(path)))
        if re.match(r"^[A-Za-z][A-Za-z0-9_]*$", package):
            return package
    return "ibmw_imported_interfaces"


def _external_dependency_packages(paths: list[str], package: str, src_root: str) -> set[str]:
    deps = set()
    search_root = os.path.dirname(src_root)
    for path in paths:
        ir = parse_interface_file(path, search_roots=[search_root], resolve_dependencies=False)
        for message in ir.messages:
            for ref in message.external_refs:
                dep_package, _, _, _ = _parse_external_ref(ref, package)
                if dep_package != package:
                    deps.add(dep_package)
    return deps


def _reference_existing_dependency_closure(
    ros2_prefix: str,
    package_root: str,
    package: str,
    files: list[str],
) -> dict:
    """Build deterministic dependency closure metadata for reference_existing imports."""
    search_roots = [os.path.join(os.path.abspath(ros2_prefix), "share"), os.path.abspath(package_root)]
    selected = []
    by_canonical: dict[str, dict] = {}
    for path in files:
        rel = _interface_relative_path(path, package_root)
        kind = _interface_kind_from_path(path)
        name = os.path.splitext(os.path.basename(path))[0]
        canonical = f"{package}/{kind}/{name}"
        selected.append({"canonical": canonical, "relative_path": rel, "kind": kind, "name": name})
        ir = parse_interface_file(path, search_roots=search_roots, resolve_dependencies=True)
        for dep in ir.dependencies:
            by_canonical[dep.canonical] = _dependency_closure_entry(dep)
    dependencies = [by_canonical[key] for key in sorted(by_canonical)]
    unresolved = [dep["canonical"] for dep in dependencies if not dep["resolved"]]
    return {
        "schema_version": 1,
        "mode": "reference_existing_dependency_closure",
        "package": package,
        "search_roots": search_roots,
        "selected": selected,
        "dependencies": dependencies,
        "external_packages": sorted({dep["package"] for dep in dependencies if dep["package"] != package}),
        "status": "complete" if not unresolved else "incomplete",
        "unresolved": unresolved,
        "uses_ros2_cli": False,
        "uses_ament_index_python": False,
    }


def _generate_package_dependency_closure(source_root: str, package: str, files: list[str]) -> dict:
    """Build dependency closure metadata for source-mode generate_package imports."""
    abs_source = os.path.abspath(source_root)
    package_root = os.path.dirname(abs_source) if os.path.isfile(abs_source) else abs_source
    search_roots = [os.path.dirname(package_root), package_root]
    selected = []
    by_canonical: dict[str, dict] = {}
    for path in files:
        kind = _interface_kind_from_path(path)
        name = os.path.splitext(os.path.basename(path))[0]
        rel = f"{kind}/{os.path.basename(path)}" if os.path.isfile(abs_source) else _interface_relative_path(path, package_root)
        canonical = f"{package}/{kind}/{name}"
        selected.append({"canonical": canonical, "relative_path": rel, "kind": kind, "name": name})
        ir = parse_interface_file(path, search_roots=search_roots, resolve_dependencies=True)
        for dep in ir.dependencies:
            by_canonical[dep.canonical] = _dependency_closure_entry(dep)
    dependencies = [by_canonical[key] for key in sorted(by_canonical)]
    unresolved = [dep["canonical"] for dep in dependencies if not dep["resolved"]]
    return {
        "schema_version": 1,
        "mode": "generate_package_dependency_closure",
        "package": package,
        "source_root": abs_source,
        "search_roots": search_roots,
        "selected": selected,
        "dependencies": dependencies,
        "external_packages": sorted({dep["package"] for dep in dependencies if dep["package"] != package}),
        "status": "complete" if not unresolved else "incomplete",
        "unresolved": unresolved,
        "uses_ros2_cli": False,
        "uses_ament_index_python": False,
    }


def _generate_package_migration_manifest(
    source_root: str,
    package: str,
    files: list[str],
    dependency_closure: dict,
    *,
    dependency_closure_name: str,
    targets: list[str],
    input_mode: str,
    backend_metadata_files: list[str] | None = None,
    backend_generated_files: list[str] | None = None,
) -> dict:
    abs_source = os.path.abspath(source_root)
    package_root = os.path.dirname(abs_source) if os.path.isfile(abs_source) else abs_source
    selected = []
    for path in files:
        kind = _interface_kind_from_path(path)
        name = os.path.splitext(os.path.basename(path))[0]
        rel = f"{kind}/{os.path.basename(path)}" if os.path.isfile(abs_source) else _interface_relative_path(path, package_root)
        selected.append(
            {
                "canonical": f"{package}/{kind}/{name}",
                "selector": f"{kind}/{name}",
                "relative_path": rel,
                "kind": kind,
                "name": name,
            }
        )
    selected.sort(key=lambda entry: entry["canonical"])
    verification = _interface_import_verification_plan(
        files,
        mode="generate_package",
        backend_metadata_files=backend_metadata_files,
        backend_generated_files=backend_generated_files,
    )
    return {
        "schema_version": 1,
        "mode": "generate_package_migration_manifest",
        "recommended_mode": "generate_package",
        "fallback_mode": "reference_existing_if_installed",
        "package": package,
        "source_root": abs_source,
        "input_mode": input_mode,
        "selected": selected,
        "targets": targets,
        "target_backend_status": _target_backend_status_report(targets, files, backend_metadata_files),
        "backend_metadata_files": backend_metadata_files or [],
        "backend_generated_files": backend_generated_files or [],
        "dependency_closure_manifest": dependency_closure_name,
        "dependency_closure_status": dependency_closure["status"],
        "dependency_count": len(dependency_closure["dependencies"]),
        "unresolved_dependencies": dependency_closure["unresolved"],
        "external_packages": dependency_closure["external_packages"],
        "required_rosidl_targets": _rosidl_target_report(package),
        "build_behavior_changed": False,
        "uses_ros2_cli": False,
        "uses_ament_index_python": False,
        "verification": verification,
    }


def _reference_existing_migration_manifest(
    package: str,
    files: list[str],
    dependency_closure: dict,
    *,
    dependency_closure_name: str,
    ros2_prefix: str,
    ros2_prefix_source: str,
    package_root: str,
    host_prefix: str | None,
    target_prefix: str | None,
    forbidden_runtime_dependencies: list[str],
    targets: list[str],
    backend_metadata_files: list[str] | None = None,
    backend_generated_files: list[str] | None = None,
) -> dict:
    """Describe how an installed ROS2 interface package maps into IBMW."""
    selected = []
    for path in files:
        kind = _interface_kind_from_path(path)
        name = os.path.splitext(os.path.basename(path))[0]
        selected.append(
            {
                "canonical": f"{package}/{kind}/{name}",
                "selector": f"{kind}/{name}",
                "relative_path": _interface_relative_path(path, package_root),
                "kind": kind,
                "name": name,
            }
        )
    selected.sort(key=lambda entry: entry["canonical"])
    verification = _interface_import_verification_plan(
        files,
        mode="reference_existing",
        backend_metadata_files=backend_metadata_files,
        backend_generated_files=backend_generated_files,
    )
    return {
        "schema_version": 1,
        "mode": "reference_existing_migration_manifest",
        "recommended_mode": "reference_existing",
        "fallback_mode": "generate_package",
        "package": package,
        "selected": selected,
        "targets": targets,
        "target_backend_status": _target_backend_status_report(targets, files, backend_metadata_files),
        "backend_metadata_files": backend_metadata_files or [],
        "backend_generated_files": backend_generated_files or [],
        "dependency_closure_manifest": dependency_closure_name,
        "dependency_closure_status": dependency_closure["status"],
        "dependency_count": len(dependency_closure["dependencies"]),
        "unresolved_dependencies": dependency_closure["unresolved"],
        "external_packages": dependency_closure["external_packages"],
        "required_rosidl_targets": _rosidl_target_report(package),
        "generated_ibmw_artifacts": {
            "interface_types_header": f"include/{package}/ibmw_interface_types.hpp",
            "conversion_header": f"include/{package}/ibmw_ros2_conversion.hpp",
        },
        "boundary": {
            "discovery_source": "share_tree_scan",
            "ros2_prefix": ros2_prefix,
            "ros2_prefix_source": ros2_prefix_source,
            "package_share": package_root,
            "host_tools_prefix": host_prefix,
            "target_runtime_prefix": target_prefix,
            "runtime_dependency_policy": "rcl_free_ibmw_runtime",
            "forbidden_runtime_dependencies": forbidden_runtime_dependencies,
        },
        "build_behavior_changed": False,
        "uses_ros2_cli": False,
        "uses_ament_index_python": False,
        "verification": verification,
    }


def _interface_import_verification_plan(
    files: list[str],
    *,
    mode: str,
    backend_metadata_files: list[str] | None = None,
    backend_generated_files: list[str] | None = None,
) -> dict:
    kinds = sorted({_interface_kind_from_path(path) for path in files})
    lanes = []
    if "msg" in kinds:
        lanes.append(
            {
                "name": "msg",
                "script": "scripts/demo_ros2_native_msg_matrix.sh",
                "command": "VERIFY_LANES=msg bash scripts/verify_ros2_interface_import_live.sh",
            }
        )
    if "srv" in kinds:
        if mode == "reference_existing":
            lanes.append(
                {
                    "name": "reference_srv",
                    "script": "scripts/live_ros2_reference_srv_acceptance.sh",
                    "command": "VERIFY_LANES=reference_srv bash scripts/verify_ros2_interface_import_live.sh",
                }
            )
        else:
            lanes.append(
                {
                    "name": "imported_srv",
                    "script": "scripts/live_ros2_imported_srv_acceptance.sh",
                    "command": "VERIFY_LANES=imported_srv bash scripts/verify_ros2_interface_import_live.sh",
                }
            )
    if "action" in kinds:
        lanes.append(
            {
                "name": "action",
                "script": "scripts/live_ros2_generated_action_matrix.sh",
                "command": "VERIFY_LANES=action bash scripts/verify_ros2_interface_import_live.sh",
                "case_selector_env": "ACTION_RUN_CASES",
            }
        )
    if any(str(rel).endswith("_json_runtime_scaffold.ibmw.yaml") for rel in (backend_generated_files or [])):
        lanes.append(
            {
                "name": "json_scaffold",
                "script": "scripts/verify_ros2_interface_import_live.sh",
                "command": "VERIFY_LANES=json_scaffold bash scripts/verify_ros2_interface_import_live.sh",
                "requires_ibmw_prefix_env": "IBMW_JSON_SMOKE_IBMW_PREFIX",
            }
        )
    perf_targets = [
        target
        for target in ("dds_native", "iceoryx")
        if any(str(rel).endswith(f"_{target}_backend_metadata.json") for rel in (backend_metadata_files or []))
    ]
    if perf_targets:
        lanes.append(
            {
                "name": "perf_boundary",
                "script": "scripts/verify_ros2_interface_import_live.sh",
                "command": "VERIFY_LANES=perf_boundary bash scripts/verify_ros2_interface_import_live.sh --manifest <manifest>",
                "requires_manifest": True,
                "guards_targets": perf_targets,
            }
        )
    lanes.append(
        {
            "name": "route_b",
            "script": "scripts/check_ohos_ros2_rosidl_compile.sh",
            "command": "VERIFY_LANES=route_b bash scripts/verify_ros2_interface_import_live.sh",
            "optional": True,
        }
    )
    return {
        "schema_version": 1,
        "entrypoint": "scripts/verify_ros2_interface_import_live.sh",
        "mode": mode,
        "selected_lanes": [lane["name"] for lane in lanes if not lane.get("optional")],
        "optional_lanes": [lane["name"] for lane in lanes if lane.get("optional")],
        "lanes": lanes,
        "uses_ros2_cli_as_import_prerequisite": False,
        "uses_rcl_in_ibmw_runtime": False,
    }


def _emit_requested_backend_metadata(
    out_root: str,
    package: str,
    files: list[str],
    targets: list[str],
    *,
    mode: str,
    source_root: str,
) -> dict:
    metadata_files = []
    generated_files = []
    if "dds_native" in set(targets):
        rel = f"{package}_dds_native_backend_metadata.json"
        _write_text(
            os.path.join(out_root, rel),
            format_diagnosis_json(_dds_native_backend_metadata(package, files, mode=mode, source_root=source_root)),
        )
        metadata_files.append(rel)
        plan_rel = f"{package}_dds_native_lowering_plan.json"
        _write_text(
            os.path.join(out_root, plan_rel),
            format_diagnosis_json(_dds_native_lowering_plan(package, files, mode=mode, source_root=source_root)),
        )
        generated_files.append(plan_rel)
    # shm shares the iceoryx loan contract, so its metadata and plan are the
    # iceoryx ones under its own target name.
    for shm_target in ("iceoryx", "shm"):
        if shm_target not in set(targets):
            continue
        rel = f"{package}_{shm_target}_backend_metadata.json"
        _write_text(
            os.path.join(out_root, rel),
            format_diagnosis_json(
                _iceoryx_backend_metadata(package, files, mode=mode, source_root=source_root, target=shm_target)
            ),
        )
        metadata_files.append(rel)
        plan_rel = f"{package}_{shm_target}_lowering_plan.json"
        _write_text(
            os.path.join(out_root, plan_rel),
            format_diagnosis_json(
                _iceoryx_lowering_plan(package, files, mode=mode, source_root=source_root, target=shm_target)
            ),
        )
        generated_files.append(plan_rel)
    if "json" in set(targets):
        json_types_header_rel = f"{package}_json_types.hpp"
        json_types_header = _render_ibmw_json_types_header(package, files, source_root=source_root)
        if json_types_header is not None:
            _write_text(os.path.join(out_root, json_types_header_rel), json_types_header)
            generated_files.append(json_types_header_rel)
            json_scaffold_rel = f"{package}_json_runtime_scaffold.ibmw.yaml"
            _write_text(
                os.path.join(out_root, json_scaffold_rel),
                _render_ibmw_json_runtime_project_scaffold_yaml(
                    package,
                    files,
                    json_types_header_rel,
                    types_include_dir=out_root,
                    source_root=source_root,
                ),
            )
            generated_files.append(json_scaffold_rel)
        rel = f"{package}_json_backend_metadata.json"
        _write_text(
            os.path.join(out_root, rel),
            format_diagnosis_json(
                _json_backend_metadata(
                    package,
                    files,
                    mode=mode,
                    source_root=source_root,
                    types_header=json_types_header_rel if json_types_header is not None else None,
                )
            ),
        )
        metadata_files.append(rel)
    return {"metadata_files": metadata_files, "generated_files": generated_files}


def _json_backend_metadata(
    package: str,
    files: list[str],
    *,
    mode: str,
    source_root: str,
    types_header: str | None = None,
) -> dict:
    return {
        "schema_version": 1,
        "target": "json",
        "status": "degraded",
        "mode": mode,
        "package": package,
        "source_root": os.path.abspath(source_root),
        "runtime_lowering_generated": False,
        "metadata_only": True,
        "types_generated": types_header is not None,
        "types_header": types_header,
        "reason": "JSON schema/type metadata emitted; JSON runtime/module lowering from ROS2 interface IR is not generated yet",
        "interfaces": _backend_interface_metadata(package, files, source_root=source_root),
    }


def _render_ibmw_json_types_header(package: str, files: list[str], *, source_root: str | None = None) -> str | None:
    structs = []
    needs_array = False
    needs_string = False
    needs_vector = False
    for struct_package, message, fields in _json_resolved_message_fields(package, files, source_root=source_root):
        needs_array = needs_array or any(cpp_type.startswith("std::array<") for cpp_type, _ in fields)
        needs_string = needs_string or any("std::string" in cpp_type for cpp_type, _ in fields)
        needs_vector = needs_vector or any(cpp_type.startswith("std::vector<") for cpp_type, _ in fields)
        lines = [
            f"struct {message.name} {{",
            f"  static constexpr const char* kTypeName = \"{struct_package}/msg/{message.name}\";",
        ]
        for cpp_type, name in fields:
            lines.append(f"  {cpp_type} {name}{{}};")
        field_names = ", ".join(name for _, name in fields)
        lines.append(f"  NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT({message.name}{', ' if field_names else ''}{field_names})")
        lines.append("};")
        structs.append((struct_package, "\n".join(lines)))
    if not structs:
        return None
    includes = ["#include <cstdint>", "#include <nlohmann/json.hpp>"]
    if needs_array:
        includes.insert(0, "#include <array>")
    if needs_string:
        includes.insert(0, "#include <string>")
    if needs_vector:
        includes.insert(0, "#include <vector>")
    lines = [
        "#pragma once",
        "// Generated JSON type structs for ROS2 interface import.",
        "// Boundary: type artifacts only; JSON runtime/module lowering is not generated here.",
        *includes,
        "",
        "namespace ibmw::ros2_interfaces::json {",
        "",
        _render_json_namespaced_structs(structs),
        "",
        "}  // namespace ibmw::ros2_interfaces::json",
        "",
    ]
    return "\n".join(lines)


def _render_ibmw_json_runtime_project_scaffold_yaml(
    package: str,
    files: list[str],
    types_header: str,
    *,
    types_include_dir: str,
    source_root: str | None = None,
) -> str:
    module_lines = []
    app_lines = []
    for struct_package, message, _fields in _json_resolved_message_fields(package, files, source_root=source_root):
        if struct_package != package:
            continue
        snake = _to_ros2_snake_case(message.name)
        message_type = f"ibmw::ros2_interfaces::json::{package}::{message.name}"
        pub_module = f"{message.name}JsonPublisherModule"
        sub_module = f"{message.name}JsonSubscriberModule"
        module_lines.append(f"""  - name: {pub_module}
    type: publisher
    transport: json
    message_type: {message_type}
    topic: /{snake}
    ros2_import_json_types_header: {types_header}
    ros2_import_json_types_include_dir: {types_include_dir}
    runtime_lowering_generated: false
  - name: {sub_module}
    type: subscriber
    transport: json
    message_type: {message_type}
    topic: /{snake}
    ros2_import_json_types_header: {types_header}
    ros2_import_json_types_include_dir: {types_include_dir}
    runtime_lowering_generated: false""")
        app_lines.append(f"""  - name: {snake}_json_pub_app
    type: pubsub
    modules:
      - {pub_module}
  - name: {snake}_json_sub_app
    type: pubsub
    modules:
      - {sub_module}""")

    return f"""# Generated by ibmw-gen import --artifact-only.
# Optional JSON runtime scaffold for imported ROS2 plain .msg types.
#
# Boundary: this is a handoff scaffold, not a generated runnable JSON runtime.
# The JSON transport generator can consume ros2_import_json_types_header, but
# this scaffold has not been compiled or runtime-verified as a generated app.
# Keep runtime_lowering_generated=false until that verification is added.

schema_version: 2
project:
  name: {package}_json_runtime_scaffold
  namespace: {package}_json_runtime_scaffold

backend_contract:
  target: json
  status: degraded
  types_header: {types_header}
  runtime_lowering_generated: false
  include_contract: ros2_import_json_types_header
  types_include_dir: {types_include_dir}
  next_step: compile this scaffold through ibmw-gen and add a JSON pub/sub runtime smoke

modules:
{chr(10).join(module_lines)}

apps:
{chr(10).join(app_lines)}
"""


def _render_json_namespaced_structs(structs: list[tuple[str, str]]) -> str:
    lines: list[str] = []
    current_package: str | None = None
    for package, struct_text in structs:
        if package != current_package:
            if current_package is not None:
                lines.append(f"}}  // namespace {current_package}")
                lines.append("")
            lines.append(f"namespace {package} {{")
            lines.append("")
            current_package = package
        lines.append(struct_text)
        lines.append("")
    if current_package is not None:
        lines.append(f"}}  // namespace {current_package}")
    return "\n".join(lines).rstrip()


def _json_resolved_message_fields(
    package: str,
    files: list[str],
    *,
    source_root: str | None = None,
) -> list[tuple[str, Ros2MessageIR, list[tuple[str, str]]]]:
    search_roots = _json_dependency_search_roots(source_root)
    messages: dict[str, tuple[str, Ros2MessageIR]] = {}
    selected_canonicals: set[str] = set()
    for path in sorted(files, key=lambda item: (_interface_kind_from_path(item), os.path.basename(item))):
        if _interface_kind_from_path(path) != "msg":
            continue
        ir = parse_interface_file(path, search_roots=search_roots, resolve_dependencies=bool(search_roots))
        message = ir.messages[0]
        if message.fields:
            canonical = f"{package}/msg/{message.name}"
            selected_canonicals.add(canonical)
            messages[canonical] = (package, message)
            for dep in ir.dependencies:
                _json_collect_dependency_messages(dep, messages, search_roots)
    if not messages:
        return []

    available_canonicals = set(messages)
    field_types: dict[str, list[tuple[str, str]]] = {}
    dependencies: dict[str, set[str]] = {}
    for canonical, (message_package, message) in messages.items():
        fields: list[tuple[str, str]] = []
        deps: set[str] = set()
        for field in message.fields:
            cpp_type = _ros2_json_field_cpp_type(
                field,
                package=message_package,
                available_message_canonicals=available_canonicals,
            )
            if cpp_type is None:
                fields = []
                deps = set()
                break
            nested = _ros2_json_message_canonical(field.base_type, message_package, available_canonicals)
            if nested and nested != canonical:
                deps.add(nested)
            fields.append((cpp_type, field.name))
        if fields:
            field_types[canonical] = fields
            dependencies[canonical] = deps

    compatible = set(field_types)
    changed = True
    while changed:
        changed = False
        for name in sorted(compatible):
            if not dependencies[name].issubset(compatible):
                compatible.remove(name)
                changed = True

    ordered: list[str] = []
    visiting: set[str] = set()
    visited: set[str] = set()
    cyclic: set[str] = set()

    def visit(name: str) -> None:
        if name in visited or name in cyclic:
            return
        if name in visiting:
            cyclic.update(visiting)
            return
        visiting.add(name)
        for dep in sorted(dependencies[name]):
            if dep in compatible:
                visit(dep)
        visiting.remove(name)
        if name not in cyclic:
            visited.add(name)
            ordered.append(name)

    for name in sorted(compatible):
        visit(name)
    selected_and_dependency_order = [name for name in ordered if name not in cyclic]
    return [
        (messages[name][0], messages[name][1], field_types[name])
        for name in selected_and_dependency_order
        if name in selected_canonicals or _json_is_dependency_of_selected(name, selected_canonicals, dependencies)
    ]


def _json_dependency_search_roots(source_root: str | None) -> list[str]:
    if not source_root:
        return []
    abs_source = os.path.abspath(source_root)
    package_root = os.path.dirname(abs_source) if os.path.isfile(abs_source) else abs_source
    roots = [os.path.dirname(package_root), package_root]
    return list(dict.fromkeys(roots))


def _json_collect_dependency_messages(
    dep: Ros2Dependency,
    messages: dict[str, tuple[str, Ros2MessageIR]],
    search_roots: list[str],
) -> None:
    if not dep.resolved or dep.kind != "msg" or dep.path is None or dep.canonical in messages:
        return
    ir = parse_interface_file(dep.path, search_roots=search_roots, resolve_dependencies=bool(search_roots))
    message = ir.messages[0]
    if message.fields:
        messages[dep.canonical] = (ir.package, message)
    for nested in ir.dependencies:
        _json_collect_dependency_messages(nested, messages, search_roots)


def _json_is_dependency_of_selected(canonical: str, selected: set[str], dependencies: dict[str, set[str]]) -> bool:
    visited: set[str] = set()

    def reachable(name: str) -> bool:
        if name in visited:
            return False
        visited.add(name)
        for dep in dependencies.get(name, set()):
            if dep == canonical or reachable(dep):
                return True
        return False

    return any(reachable(name) for name in selected)


def _ros2_json_field_cpp_type(
    field: Ros2Field,
    *,
    package: str | None = None,
    available_message_canonicals: set[str] | None = None,
) -> str | None:
    base = field.base_type
    cpp_base = _ROS2_JSON_PRIMITIVE_CPP_TYPES.get(base)
    if cpp_base is None:
        cpp_base = _ros2_json_string_cpp_type(base)
    if cpp_base is None and package is not None and available_message_canonicals is not None:
        cpp_base = _ros2_json_message_cpp_type(base, package, available_message_canonicals)
    if cpp_base is None:
        return None
    array_kind = field.array_kind
    if array_kind is None:
        return cpp_base
    if array_kind in ("bounded", "unbounded"):
        return f"std::vector<{cpp_base}>"
    if array_kind != "fixed":
        return None
    match = _ARRAY_RE.match(field.type)
    if not match:
        return None
    return f"std::array<{cpp_base}, {match.group('bound')}>"


def _ros2_json_string_cpp_type(base: str) -> str | None:
    if base == "string":
        return "std::string"
    if _BOUNDED_STRING_RE.fullmatch(base) and base.startswith("string<="):
        return "std::string"
    return None


def _ros2_json_message_cpp_type(base: str, package: str, available_message_canonicals: set[str]) -> str | None:
    canonical = _ros2_json_message_canonical(base, package, available_message_canonicals)
    if canonical is None:
        return None
    dep_package, _kind, type_name = canonical.split("/")
    if dep_package == package:
        return type_name
    return f"::ibmw::ros2_interfaces::json::{dep_package}::{type_name}"


def _ros2_json_message_canonical(base: str, package: str, available_message_canonicals: set[str]) -> str | None:
    dep_package, kind, type_name, canonical = _parse_external_ref(base, package)
    if kind != "msg":
        return None
    if canonical in available_message_canonicals:
        return canonical
    same_package = f"{package}/msg/{type_name}"
    if same_package in available_message_canonicals:
        return same_package
    return None


_ROS2_JSON_PRIMITIVE_CPP_TYPES = {
    "bool": "bool",
    "byte": "uint8_t",
    "char": "uint8_t",
    "float32": "float",
    "float64": "double",
    "int8": "int8_t",
    "uint8": "uint8_t",
    "int16": "int16_t",
    "uint16": "uint16_t",
    "int32": "int32_t",
    "uint32": "uint32_t",
    "int64": "int64_t",
    "uint64": "uint64_t",
}


def _dds_native_backend_metadata(package: str, files: list[str], *, mode: str, source_root: str) -> dict:
    interfaces = _backend_interface_metadata(package, files, source_root=source_root)
    eligible = [entry["canonical"] for entry in interfaces if entry["capability"]["transports"]["dds_native"] == "yes"]
    degraded = [entry["canonical"] for entry in interfaces if entry["capability"]["transports"]["dds_native"] != "yes"]
    return {
        "schema_version": 1,
        "target": "dds_native",
        "status": "degraded",
        "mode": mode,
        "package": package,
        "source_root": os.path.abspath(source_root),
        "runtime_lowering_generated": False,
        "metadata_only": True,
        "reason": "IBMW native DDS schema metadata emitted; IDL/schema/runtime lowering from ROS2 interface IR is not generated yet",
        "eligible_interfaces": eligible,
        "degraded_interfaces": degraded,
        "interfaces": interfaces,
    }


def _dds_native_lowering_plan(package: str, files: list[str], *, mode: str, source_root: str) -> dict:
    interfaces = _backend_interface_metadata(package, files, source_root=source_root)
    eligible = []
    degraded = []
    for entry in interfaces:
        capability = entry["capability"]
        canonical = entry["canonical"]
        if capability["transports"]["dds_native"] == "yes":
            eligible.append(
                {
                    "canonical": canonical,
                    "kind": entry["kind"],
                    "name": entry["name"],
                    "max_serialized_size_bytes": capability.get("max_serialized_size_bytes"),
                    "future_idl_file": f"idl/{package}/{entry['name']}.idl",
                    "future_schema_type": f"{package}::{entry['name']}",
                    "future_type_header": f"protocol/{entry['name']}.h",
                    "required_gate": "dds_native_schema_idl_compile_smoke_then_runtime_gate",
                }
            )
        else:
            degraded.append(
                {
                    "canonical": canonical,
                    "kind": entry["kind"],
                    "name": entry["name"],
                    "reason": "not fixed-size/plain enough for first native DDS lowering slice",
                    "suggestions": capability.get("suggestions") or [],
                    "contains_heap": capability.get("contains_heap"),
                    "contains_unbounded_sequence": capability.get("contains_unbounded_sequence"),
                    "contains_string": capability.get("contains_string"),
                }
            )
    return {
        "schema_version": 1,
        "target": "dds_native",
        "artifact": "dds_native_lowering_plan",
        "status": "plan_only",
        "mode": mode,
        "package": package,
        "source_root": os.path.abspath(source_root),
        "runtime_lowering_generated": False,
        "metadata_only": True,
        "no_runtime_hot_path_changes": True,
        "size_bound_semantics": "packed_ros2_ir_max_serialized_size_bytes_not_cpp_sizeof",
        "promotion_guard": "metadata_to_schema_idl_type_artifacts_to_compile_smoke_to_runtime_gate_to_status_promotion",
        "eligible_interfaces": eligible,
        "degraded_interfaces": degraded,
        "totals": {
            "eligible": len(eligible),
            "degraded": len(degraded),
        },
    }


def _iceoryx_backend_metadata(
    package: str, files: list[str], *, mode: str, source_root: str, target: str = "iceoryx"
) -> dict:
    interfaces = _backend_interface_metadata(package, files, source_root=source_root)
    eligible = [entry["canonical"] for entry in interfaces if entry["capability"]["zero_copy"][target] == "yes"]
    blocked = [entry["canonical"] for entry in interfaces if entry["capability"]["zero_copy"][target] != "yes"]
    return {
        "schema_version": 1,
        "target": target,
        "status": "degraded",
        "mode": mode,
        "package": package,
        "source_root": os.path.abspath(source_root),
        "runtime_lowering_generated": False,
        "metadata_only": True,
        "reason": f"{target} eligibility metadata emitted; zero-copy runtime/module lowering from ROS2 interface IR is not generated yet",
        "eligible_interfaces": eligible,
        "blocked_interfaces": blocked,
        "interfaces": interfaces,
    }


def _iceoryx_lowering_plan(
    package: str, files: list[str], *, mode: str, source_root: str, target: str = "iceoryx"
) -> dict:
    interfaces = _backend_interface_metadata(package, files, source_root=source_root)
    title = target.capitalize()
    eligible = []
    blocked = []
    for entry in interfaces:
        capability = entry["capability"]
        canonical = entry["canonical"]
        if capability["zero_copy"][target] == "yes":
            eligible.append(
                {
                    "canonical": canonical,
                    "kind": entry["kind"],
                    "name": entry["name"],
                    "max_serialized_size_bytes": capability.get("max_serialized_size_bytes"),
                    "future_type_name": f"{package}::{entry['name']}{title}Type",
                    "future_module_names": {
                        "publisher": f"{entry['name']}{title}PublisherModule",
                        "subscriber": f"{entry['name']}{title}SubscriberModule",
                    },
                    "required_gate": f"{target}_compile_smoke_then_runtime_zero_copy_gate",
                }
            )
        else:
            blocked.append(
                {
                    "canonical": canonical,
                    "kind": entry["kind"],
                    "name": entry["name"],
                    "reason": f"not plain/fixed-size {target} zero-copy eligible",
                    "suggestions": capability.get("suggestions") or [],
                    "contains_heap": capability.get("contains_heap"),
                    "contains_unbounded_sequence": capability.get("contains_unbounded_sequence"),
                    "contains_string": capability.get("contains_string"),
                }
            )
    return {
        "schema_version": 1,
        "target": target,
        "artifact": f"{target}_lowering_plan",
        "status": "plan_only",
        "mode": mode,
        "package": package,
        "source_root": os.path.abspath(source_root),
        "runtime_lowering_generated": False,
        "metadata_only": True,
        "no_runtime_hot_path_changes": True,
        "size_bound_semantics": "packed_ros2_ir_max_serialized_size_bytes_not_cpp_sizeof",
        "promotion_guard": "metadata_to_lowering_plan_to_compile_smoke_to_runtime_zero_copy_gate_to_status_promotion",
        "eligible_interfaces": eligible,
        "blocked_interfaces": blocked,
        "totals": {
            "eligible": len(eligible),
            "blocked": len(blocked),
        },
    }


def _backend_interface_metadata(package: str, files: list[str], *, source_root: str | None = None) -> list[dict]:
    schemas = []
    search_roots = _json_dependency_search_roots(source_root)
    for path in sorted(files, key=lambda item: (_interface_kind_from_path(item), os.path.basename(item))):
        kind = _interface_kind_from_path(path)
        name = os.path.splitext(os.path.basename(path))[0]
        ir = parse_interface_file(path, search_roots=search_roots, resolve_dependencies=bool(search_roots))
        message_entries = []
        for message in ir.messages:
            message_entries.append(
                {
                    "name": message.name,
                    "capability": _message_capability(message),
                    "fields": [
                        {
                            "name": field.name,
                            "type": field.type,
                            "base_type": field.base_type,
                            "array_kind": field.array_kind,
                            "capability": _field_capability(field),
                        }
                        for field in message.fields
                    ],
                    "constants": [
                        {"name": constant.name, "type": constant.type, "value": constant.value}
                        for constant in message.constants
                    ],
                }
            )
        schemas.append(
            {
                "canonical": f"{package}/{kind}/{name}",
                "kind": kind,
                "name": name,
                "capability": _interface_capability(message_entries),
                "messages": message_entries,
            }
        )
    return schemas


def _metadata_files_for_target(metadata_files: list[str], target: str) -> list[str]:
    return [path for path in metadata_files if path.endswith(f"_{target}_backend_metadata.json")]


def _metadata_only_degraded_backend_status(
    target: str,
    metadata_files: list[str],
    *,
    runtime_boundary: str,
    reason: str,
    next_step: str,
) -> dict | None:
    target_metadata_files = _metadata_files_for_target(metadata_files, target)
    if not target_metadata_files:
        return None
    return {
        "target": target,
        "status": "degraded",
        "runtime_boundary": runtime_boundary,
        "reason": reason,
        "generated_artifacts": target_metadata_files,
        "next_step": next_step,
    }


def _target_backend_status_report(targets: list[str], files: list[str], backend_metadata_files: list[str] | None = None) -> dict:
    kinds = sorted({_interface_kind_from_path(path) for path in files})
    has_action = "action" in kinds
    has_srv = "srv" in kinds
    has_msg = "msg" in kinds
    metadata_files = backend_metadata_files or []
    entries = []
    for target in sorted(targets):
        if target == "dds_ros2":
            entries.append(
                {
                    "target": target,
                    "status": "supported",
                    "runtime_boundary": "rcl_free_rosidl_fastrtps",
                    "reason": "ROS2-compatible DDS lane reuses rosidl C++ types and FastRTPS typesupport",
                    "generated_artifacts": ["rosidl_package", "ibmw_wrapper_headers"],
                }
            )
        elif target == "dds_native":
            degraded = _metadata_only_degraded_backend_status(
                target,
                metadata_files,
                runtime_boundary="metadata_only_no_native_dds_lowering",
                reason="IBMW native DDS schema metadata emitted, but IDL/schema/runtime lowering from ROS2 interface IR is not generated yet",
                next_step="consume metadata from the native DDS emitter and add field-level ROS2-to-IBMW type mapping",
            )
            entries.append(
                degraded
                or {
                    "target": target,
                    "status": "blocked",
                    "runtime_boundary": "not_generated",
                    "reason": "IBMW native DDS schema/IDL lowering from ROS2 interface IR is not implemented yet",
                    "next_step": "add native DDS emitter and field-level ROS2-to-IBMW type mapping",
                }
            )
        elif target in ("iceoryx", "shm"):
            reasons = [f"{target} lowering from ROS2 interface IR is not implemented yet"]
            if has_srv or has_action:
                reasons.append("service/action semantics require adapter design beyond fixed-size message lowering")
            if has_msg:
                reasons.append("message lane must prove plain/fixed-size eligibility before zero-copy lowering")
            degraded = _metadata_only_degraded_backend_status(
                target,
                metadata_files,
                runtime_boundary=f"metadata_only_no_{target}_lowering",
                reason=f"{target} eligibility metadata emitted, but zero-copy runtime/module lowering from ROS2 interface IR is not generated yet",
                next_step=f"consume eligibility metadata from the {target} emitter and generate wrappers only for plain/fixed-size messages",
            )
            entries.append(
                degraded
                or {
                    "target": target,
                    "status": "blocked",
                    "runtime_boundary": "not_generated",
                    "reason": "; ".join(reasons),
                    "next_step": f"add plain/fixed-size classifier backed by {target} emitter diagnostics",
                }
            )
        elif target == "json":
            json_metadata_files = _metadata_files_for_target(metadata_files, "json")
            if json_metadata_files:
                entries.append(
                    {
                        "target": target,
                        "status": "degraded",
                        "runtime_boundary": "metadata_only_no_runtime_lowering",
                        "reason": "JSON schema metadata emitted, but JSON runtime/module lowering from ROS2 interface IR is not generated yet",
                        "generated_artifacts": json_metadata_files,
                        "next_step": "consume metadata from the JSON transport emitter and generate runtime wrappers",
                    }
                )
            else:
                entries.append(
                    {
                        "target": target,
                        "status": "blocked",
                        "runtime_boundary": "not_generated",
                        "reason": "JSON lowering from ROS2 interface IR is not implemented yet",
                        "next_step": "add JSON schema emitter and degraded diagnostics for unsupported ROS2 field shapes",
                    }
                )
        else:
            entries.append(
                {
                    "target": target,
                    "status": "blocked",
                    "runtime_boundary": "unknown",
                    "reason": "unknown target backend",
                }
            )
    return {
        "schema_version": 1,
        "interface_kinds": kinds,
        "default_supported_target": "dds_ros2",
        "entries": entries,
        "summary": {entry["target"]: entry["status"] for entry in entries},
    }


def _dependency_closure_entry(dep: Ros2Dependency) -> dict:
    package, kind, type_name, _ = _parse_external_ref(dep.canonical)
    return {
        "canonical": dep.canonical,
        "package": package,
        "kind": kind,
        "name": type_name,
        "path": dep.path,
        "relative_path": f"{kind}/{type_name}.{kind}" if kind in ("msg", "srv", "action") else None,
        "resolved": dep.resolved,
        "plain_compatible": dep.plain_compatible,
        "serialized_size_bytes": dep.serialized_size_bytes,
        "reason": dep.reason,
    }


def _read_package_name_from_xml(root: str) -> str | None:
    path = os.path.join(root, "package.xml")
    if not os.path.isfile(path):
        return None
    try:
        xml_root = ET.parse(path).getroot()
    except ET.ParseError:
        return None
    name = xml_root.findtext("name")
    return name.strip() if name else None


def _render_rosidl_package_xml(package: str, deps: list[str]) -> str:
    lines = [
        '<?xml version="1.0"?>',
        '<package format="3">',
        f"  <name>{package}</name>",
        "  <version>0.0.0</version>",
        "  <description>Generated ROS2 interface package emitted by ibmw-gen.</description>",
        "  <maintainer email=\"ibmw@example.com\">IBMW Generated</maintainer>",
        "  <license>Apache-2.0</license>",
        "  <buildtool_depend>ament_cmake</buildtool_depend>",
        "  <build_depend>rosidl_default_generators</build_depend>",
    ]
    for dep in deps:
        lines.append(f"  <depend>{dep}</depend>")
    lines.extend(
        [
            "  <exec_depend>rosidl_default_runtime</exec_depend>",
            "  <member_of_group>rosidl_interface_packages</member_of_group>",
            "</package>",
            "",
        ]
    )
    return "\n".join(lines)


def _render_rosidl_cmake_lists(package: str, files: list[str], deps: list[str]) -> str:
    lines = [
        "cmake_minimum_required(VERSION 3.8)",
        f"project({package})",
        "",
        "find_package(ament_cmake REQUIRED)",
        "find_package(rosidl_default_generators REQUIRED)",
    ]
    for dep in deps:
        lines.append(f"find_package({dep} REQUIRED)")
    lines.extend(["", "rosidl_generate_interfaces(${PROJECT_NAME}"])
    for rel in files:
        lines.append(f"  \"{rel}\"")
    if deps:
        lines.append("  DEPENDENCIES")
        for dep in deps:
            lines.append(f"  {dep}")
    lines.extend(
        [
            ")",
            "",
            "install(DIRECTORY include/ DESTINATION include)",
            "ament_export_include_directories(include)",
            "ament_export_dependencies(rosidl_default_runtime)",
            "ament_package()",
            "",
        ]
    )
    return "\n".join(lines)


def _write_text(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
