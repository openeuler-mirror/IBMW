#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# YAML schema parser and validator for .ibmw.yaml project definitions.

"""Parse and validate .ibmw.yaml project definitions."""

import os
import sys
from typing import Any

try:
    import yaml
except ImportError:
    print("Error: PyYAML is required. Install with: pip install pyyaml",
          file=sys.stderr)
    sys.exit(1)


def _error(msg: str):
    print(f"Error: {msg}", file=sys.stderr)
    sys.exit(1)


def _warn(msg: str):
    print(f"Warning: {msg}", file=sys.stderr)


# ── Schema versioning ─────────────────────────────────────────────────────────
# The `schema_version:` top-level key declares which schema dialect a
# .ibmw.yaml file uses. Absence is treated as v1 (back-compat).
#
# v1 vs v2 (see docs/architecture/overview.md):
#   v1 — fields like dds_type_name / iceoryx_msg_type / ros2_msg_include
#        / ros2_package / ros2_packages / ros2_type_name /
#        zero_copy_config sit *flat on each module*. The transport
#        coupling lives in the module entry itself. All current 14 user
#        yamls use this dialect.
#   v2 — message-type information is hoisted into a top-level
#        `messages:` block with per-transport `wire.<transport>` sub-
#        blocks. Modules reference a message by name only:
#           messages:
#             my_msgs::Point:
#               wire:
#                 dds:  { type_name: my_msgs::Point }
#                 ros2: { include: geom/point.hpp, type: geom::Point,
#                         package: geom }
#           modules:
#             - name: Pub
#               type: publisher
#               transport: dds
#               topic: /t
#               message: my_msgs::Point     # ← single ref
#
# IMPLEMENTATION DIRECTION:
#   Validators / enrichers / generators all read the v1 *flat* shape
#   today. The least disruptive path is therefore **v2 → v1 internal
#   expansion**: when a v2 yaml is loaded, hoist `messages.<name>.wire.*`
#   fields back onto every referencing module and continue. v1 yamls
#   are pass-through (no-op). This avoids refactoring every downstream
#   reader.
#   The original design phrased this as "v1→v2"; the actual code moves in
#   the reverse direction. User yamls can be rewritten into v2 source form
#   while the loader still
#   expanding them back to v1 for the existing readers).
#
# The loader therefore performs v2→v1 in-memory expansion, while the
# `ibmw-gen migrate-yaml` CLI converts source files in the other direction.
SUPPORTED_SCHEMA_VERSIONS = (1, 2)
DEFAULT_SCHEMA_VERSION = 1


def _detect_schema_version(data: dict) -> int:
    """Read the optional top-level `schema_version:` key and validate it.

    Returns the integer version. Defaults to DEFAULT_SCHEMA_VERSION when
    absent (preserves v1 back-compat for the 14 existing user yamls).
    """
    if "schema_version" not in data:
        return DEFAULT_SCHEMA_VERSION
    v = data["schema_version"]
    if not isinstance(v, int):
        _error(f"'schema_version' must be an integer, got "
               f"{type(v).__name__}: {v!r}")
    if v not in SUPPORTED_SCHEMA_VERSIONS:
        _error(f"Unsupported schema_version: {v}. "
               f"Supported: {sorted(SUPPORTED_SCHEMA_VERSIONS)}")
    return v


# Auto-derive ros2_msg_include / ros2_package from message_type.
#
# A C++-qualified ROS2 message type like `geometry_msgs::msg::Point` encodes
# everything needed to fill the redundant `ros2_msg_include` / `ros2_package`
# fields mechanically:
#
#     parts        = ["geometry_msgs", "msg", "Point"]
#     ros2_package = parts[0]
#     ros2_msg_include = "/".join(parts[:-1]) + "/" + _camel_to_snake(last) + ".hpp"
#
# The function is a no-op when:
#   - message_type lacks `::` qualifiers (not a canonical ROS2 type — user
#     must still fill ros2_* explicitly, e.g. custom DDS-only types);
#   - the target field is already present (user override preserved).
#
# Called BEFORE the required-field check in loaned_* / direct_ros2_* branches
# so the check sees the derived values. Guards byte-equal: every known yaml
# already carries the derived values as explicit fields, so derivation either
# produces the identical string or is skipped (user override).
def _camel_to_snake(s: str) -> str:
    """CamelCase → snake_case. Matches _common._snake output character-for-
    character for mixed-case words (verified by regen byte-equal)."""
    out = []
    for i, ch in enumerate(s):
        if ch.isupper() and i > 0:
            prev = s[i - 1]
            if prev.islower() or prev.isdigit():
                out.append("_")
            elif i + 1 < len(s) and s[i + 1].islower():
                out.append("_")
        out.append(ch.lower())
    return "".join(out)


def _derive_ros2_fields(mod: dict) -> None:
    """Fill missing ros2_msg_include / ros2_package from message_type in-place.

    No-op if message_type is absent, lacks `::`, or the fields already exist.
    """
    mt = mod.get("message_type")
    if not mt or "::" not in mt:
        return
    parts = mt.split("::")
    if len(parts) < 2:
        return
    if "ros2_package" not in mod:
        mod["ros2_package"] = parts[0]
    if "ros2_msg_include" not in mod:
        last = _camel_to_snake(parts[-1])
        mod["ros2_msg_include"] = "/".join(parts[:-1]) + "/" + last + ".hpp"


# v2 → v1 expansion field map with full coverage of all supported wire fields.
#
# Each entry: (v2 wire.<transport>.<key>, v1 module-level field name).
# Per-transport sub-blocks are flattened onto each module that
# references the message AND has the matching transport.
_V2_WIRE_TO_V1_MODULE: dict[str, list[tuple[str, str]]] = {
    "dds": [
        ("type_name", "dds_type_name"),
        ("namespace_cpp", "dds_namespace_cpp"),
        ("service", "dds_service"),
    ],
    "ros2": [
        ("include", "ros2_msg_include"),
        ("type", "ros2_type_name"),
        ("package", "ros2_package"),
        ("packages", "ros2_packages"),
        ("service", "ros2_service"),
        ("rpc_gencode_target", "ros2_rpc_gencode_target"),
    ],
    "iceoryx": [
        # `msg_type` is redundant — the iceoryx plugin defaults it to
        # `message_type` (the top-level messages: key) when absent. The
        # mapping is kept so legacy yamls that still carry an explicit
        # `wire.iceoryx.msg_type` continue to parse byte-identically.
        ("msg_type", "iceoryx_msg_type"),
    ],
}


def _expand_v2_messages_block(data: dict) -> None:
    """Hoist v2 `messages:` block fields onto referring modules in-place.

    For each module that uses `message: <name>` (v2 spelling), look up
    `messages[<name>]` and copy its `wire.<transport>` sub-fields onto
    the module using v1 field names. The downstream validator and
    enricher then operate on the familiar v1 flat shape.

    This is a no-op when there is no top-level `messages:` block
    (typical v1 yaml).

    `ros2_packages` (plural list) is kept as a list when expanding because
    downstream `_enrich` and generator code already handle that shape.
    """
    msgs = data.get("messages")
    if not msgs:
        return
    if not isinstance(msgs, dict):
        _error("'messages' must be a mapping of message-name → spec")

    modules = data.get("modules") or []
    for mod in modules:
        if not isinstance(mod, dict):
            continue
        # v2 spelling: `message: <name>`. Skip modules that already use
        # the v1 `message_type` field (mixed-mode yamls are tolerated).
        msg_ref = mod.get("message")
        if not msg_ref:
            continue
        if msg_ref not in msgs:
            _error(f"Module '{mod.get('name', '<unnamed>')}' references "
                   f"unknown message '{msg_ref}'")
        spec = msgs[msg_ref]
        if not isinstance(spec, dict):
            _error(f"messages['{msg_ref}'] must be a mapping")

        # Inject v1-style `message_type` mirror so downstream readers
        # (which look at `message_type`) see the same value.
        mod.setdefault("message_type", msg_ref)

        # Hoist wire.<transport>.* → v1 module-level field for the
        # module's transport (or for *every* listed wire transport if
        # the module declares no `transport` — applies to direct_ros2_*
        # / ros2_interop_* templates that carry implicit transport).
        wire = spec.get("wire") or {}
        if not isinstance(wire, dict):
            _error(f"messages['{msg_ref}'].wire must be a mapping")

        mod_transport = mod.get("transport")
        # When transport is implicit-via-type (direct_ros2_* / ros2_interop_*
        # both carry implicit ros2 transport), apply the
        # ros2 wire sub-block.
        implicit_transports: set[str] = set()
        mod_type = mod.get("type") or ""
        if mod_type.startswith("direct_ros2_") or \
                mod_type.startswith("ros2_interop_"):
            implicit_transports.add("ros2")
        if mod_type.startswith("direct_iceoryx_"):
            implicit_transports.add("iceoryx")

        targets: set[str] = set()
        if mod_transport:
            targets.add(mod_transport)
        targets.update(implicit_transports)

        for tname in targets:
            tblock = wire.get(tname)
            if tblock is None:
                continue
            if not isinstance(tblock, dict):
                _error(f"messages['{msg_ref}'].wire.{tname} must be a "
                       f"mapping")
            mappings = _V2_WIRE_TO_V1_MODULE.get(tname, [])
            for v2key, v1key in mappings:
                if v2key in tblock and v1key not in mod:
                    mod[v1key] = tblock[v2key]


def _expand_v2_to_v1_internal(data: dict) -> None:
    """v2 → v1 in-memory expansion entry point.

    Currently expands the `messages:` block. The top-level `dds_driver:`
    block is already shaped identically in v1 and v2 because it is
    deployment-side configuration, so no transcoding is required.
    """
    _expand_v2_messages_block(data)


# Backwards-named compatibility alias. The implementation moves data from v2
# to the v1 internal flat shape; see the long comment above.
def _migrate_v1_to_v2(data: dict) -> None:  # noqa: D401
    """No-op: see _expand_v2_to_v1_internal for the actual expansion logic."""
    return None


# ── Helpers ──────────────────────────────────────────────────────────────────

def _to_snake_case(name: str) -> str:
    """Convert PascalCase to snake_case."""
    result = []
    for i, ch in enumerate(name):
        if ch.isupper() and i > 0:
            prev = name[i - 1]
            if prev.islower() or prev.isdigit():
                result.append("_")
            elif i + 1 < len(name) and name[i + 1].islower():
                result.append("_")
        result.append(ch.lower())
    return "".join(result)


def _namespace_to_cpp(dotted: str) -> str:
    """Convert dotted namespace (ibmw.protocols.example) to C++ (ibmw::protocols::example)."""
    return "::".join(dotted.split("."))


def _namespace_to_idl_modules(dotted: str) -> list[str]:
    """Convert dotted namespace to list of IDL module names."""
    return dotted.split(".")


# ── Validation ───────────────────────────────────────────────────────────────

VALID_MODULE_TYPES = {
    "bare", "publisher", "subscriber", "rpc_server", "rpc_client",
    "logger",
    # R10 escape-hatch templates: direct_<transport>_* names
    # signal "transport-coupled by design" — user code legitimately
    # accesses the named transport's API directly (e.g. ROS2 typesupport
    # interop). These bypass the transport-decoupling rules R3/R4 by
    # explicit opt-in at module-template selection time.
    "direct_ros2_publisher", "direct_ros2_subscriber",
    "direct_iceoryx_publisher", "direct_iceoryx_subscriber",
    # Deprecated aliases (kept for one release window; normalized to
    # direct_ros2_* below in _normalize_module_type_alias()).
    "ros2_interop_publisher", "ros2_interop_subscriber",
    "loaned_publisher", "loaned_subscriber",
}

# Deprecated module_type aliases → canonical direct_<transport>_* names.
# R10 compatibility: old `ros2_interop_*` names are kept as deprecated
# aliases for one release; ibmw-gen normalizes them to the new canonical
# names and emits a deprecation warning. Remove this map after the
# release window closes.
_MODULE_TYPE_ALIASES = {
    "ros2_interop_publisher": "direct_ros2_publisher",
    "ros2_interop_subscriber": "direct_ros2_subscriber",
}


def _normalize_module_type_alias(mod: dict) -> None:
    """Translate deprecated module_type aliases to canonical names in-place.

    Emits a one-time deprecation warning per (alias, canonical) pair so
    users see clear guidance to migrate. Sets `mod["_orig_type"]` for
    diagnostics.
    """
    t = mod.get("type")
    if t in _MODULE_TYPE_ALIASES:
        new_t = _MODULE_TYPE_ALIASES[t]
        _warn(f"Module '{mod.get('name', '<unnamed>')}': module_type '{t}' "
              f"is deprecated; rename to '{new_t}' (R10 escape-hatch "
              f"naming). The old name is honored for one "
              f"release and will be removed.")
        mod["_orig_type"] = t
        mod["type"] = new_t

# loaned_subscriber API style — controls which user-facing pattern the
# generated module exposes. See docs/contributing/testing.md.
#   callback : user overrides OnMessage(const T&) virtual; framework drives
#              callbacks (default; mirrors existing iox/dds golden samples).
#   tick     : user calls TakeOne(T& out) inside Tick(); zero background thread.
#   borrow   : user explicitly calls BorrowMessage() to get an RAII guard.
VALID_SUBSCRIBER_STYLE = {"callback", "tick", "borrow"}

# Module-level zero-copy mode override (loaned_publisher).
# auto    = best-effort (zero-copy if plain + driver supports it; else fallback)
# strict  = require zero-copy; fail/reject loan when not possible
# compat  = always use traditional path even if zero-copy possible
VALID_ZERO_COPY_CONFIG = {"auto", "strict", "compat"}

# SHM-capable transports for the loaned_{publisher,subscriber} types.
# This is the single source of truth for the generation-time check.
#   - iceoryx is the canonical native shared-memory transport (true zero-copy
#     via SHM loan).
#   - dds is treated as conditionally SHM-capable: FastDDS DataSharing supports
#     a loan API and `loaned_message_demo.ibmw.yaml` is a working sample.
#     It remains conditional until IBMW_HAS_SHM_DDS is concretely defined.
# All other transports (ros2, local, mqtt, net, zenoh, http) lack the
# shared-memory loan path and would force a memcpy — violating the zero-copy
# absolute rule, so validation reports those combinations as errors.
_SHM_CAPABLE_TRANSPORTS = {"iceoryx", "dds"}

# The VALID_TRANSPORTS closed enum was removed. Validation
# now queries the open transport plugin registry — see
# tools/ibmw_gen/transports/__init__.py. Adding a new transport no
# longer requires editing this file.
import transports as _transports  # noqa: E402  (after constants)
VALID_APP_TYPES = {"pubsub", "rpc", "bare"}

EXPERIMENTAL_ACTION_FIBONACCI = "example_interfaces/action/Fibonacci"
EXPERIMENTAL_ACTION_ROLES = {"server", "client"}

# Module types that don't require transport/topic/message_type
BARE_LIKE_MODULE_TYPES = {"bare", "logger"}

# Valid logging backend types
VALID_LOG_BACKEND_TYPES = {"console", "rotate_file"}

# Valid executor types
VALID_EXECUTOR_TYPES = {
    "thread_pool", "strand", "dedicated", "timer",
    "parallel", "time_manipulator",
}

# Primitive types for DDS
DDS_PRIMITIVE_TYPES = {
    "bool", "int8", "int16", "int32", "int64",
    "uint8", "uint16", "uint32", "uint64",
    "float", "double", "string", "bytes",
}

# JSON C++ types
JSON_CPP_TYPES = {
    "string", "bool",
    "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t",
    "float", "double",
}

# Custom DDS type YAML primitives
CUSTOM_DDS_PRIMITIVE_TYPES = {
    "bool", "int8", "uint8", "int16", "uint16",
    "int32", "uint32", "int64", "uint64",
    "float32", "float64", "string",
}

# Subset of primitives that are plain (no heap allocation)
CUSTOM_DDS_PLAIN_PRIMITIVES = {
    "bool", "int8", "uint8", "int16", "uint16",
    "int32", "uint32", "int64", "uint64",
    "float32", "float64",
}

# Valid underlying types for enums
CUSTOM_DDS_ENUM_UNDERLYING = {
    "uint8", "uint16", "uint32", "uint64",
    "int8", "int16", "int32", "int64",
}


def _validate_project(data: dict):
    """Validate the project section."""
    if "project" not in data:
        _error("Missing required top-level key: 'project'")
    proj = data["project"]
    if not isinstance(proj, dict) or "name" not in proj:
        _error("'project' must have a 'name' field")
    if not proj["name"].replace("_", "").replace("-", "").isalnum():
        _error(f"Invalid project name: '{proj['name']}' "
               "(must be alphanumeric with underscores/hyphens)")
    # Default namespace = project name
    if "namespace" not in proj:
        proj["namespace"] = proj["name"]


def _validate_dds_types(data: dict):
    """Validate the dds_types section."""
    dds = data.get("dds_types")
    if not dds:
        return

    if "namespace" not in dds:
        _error("dds_types must have a 'namespace' field")

    groups = dds.get("groups", [])
    group_names = set()
    all_msg_names = {}  # msg_name -> group_name

    for g in groups:
        if "name" not in g:
            _error("Each dds_types group must have a 'name' field")
        if g["name"] in group_names:
            _error(f"Duplicate dds_types group name: '{g['name']}'")
        group_names.add(g["name"])

        for imp in g.get("imports", []):
            if imp not in group_names:
                _warn(f"Group '{g['name']}' imports '{imp}' which hasn't been "
                      f"defined yet (ensure it appears before '{g['name']}' in the YAML)")

        for msg in g.get("messages", []):
            if "name" not in msg:
                _error(f"Message in group '{g['name']}' must have a 'name' field")
            if "fields" not in msg:
                _error(f"Message '{msg['name']}' in group '{g['name']}' must have 'fields'")
            all_msg_names[msg["name"]] = g["name"]

    # Validate enums
    for enum in dds.get("enums", []):
        if "name" not in enum:
            _error("Each enum must have a 'name' field")
        if "group" not in enum:
            _error(f"Enum '{enum['name']}' must specify a 'group'")
        if "values" not in enum or not enum["values"]:
            _error(f"Enum '{enum['name']}' must have non-empty 'values'")

    # Validate rpc_services
    for svc in dds.get("rpc_services", []):
        if "name" not in svc:
            _error("Each rpc_service must have a 'name' field")
        if "group" not in svc:
            _error(f"RPC service '{svc['name']}' must specify a 'group'")
        if svc["group"] not in group_names:
            _error(f"RPC service '{svc['name']}' references unknown group '{svc['group']}'")
        for method in svc.get("methods", []):
            if "name" not in method:
                _error(f"RPC method in service '{svc['name']}' must have a 'name'")
            if "request" not in method or "response" not in method:
                _error(f"RPC method '{method['name']}' must have 'request' and 'response'")


def _validate_json_types(data: dict):
    """Validate the json_types section."""
    jt = data.get("json_types")
    if not jt:
        return
    for msg in jt.get("messages", []):
        if "name" not in msg:
            _error("Each json_types message must have a 'name' field")
        if "fields" not in msg:
            _error(f"JSON message '{msg['name']}' must have 'fields'")


def _validate_custom_dds_field_type(field_type: str, enum_names: set,
                                      msg_names: set, msg_name: str,
                                      field_name: str):
    """Validate a single field type in custom_dds_types.

    Accepts: primitives, enums, other message names (nested types),
    array<T, N>, vector<T> where T is any valid type.
    """
    if field_type in CUSTOM_DDS_PRIMITIVE_TYPES:
        return
    if field_type in enum_names:
        return
    if field_type in msg_names:
        return
    if field_type.startswith("array<"):
        if not field_type.endswith(">"):
            _error(f"custom_dds_types message '{msg_name}' field '{field_name}': "
                   f"malformed array type '{field_type}' (missing '>')")
        inner = field_type[6:-1]
        last_comma = inner.rfind(",")
        if last_comma < 0:
            _error(f"custom_dds_types message '{msg_name}' field '{field_name}': "
                   f"array type must be 'array<T, N>' (missing comma)")
        inner_type = inner[:last_comma].strip()
        size_str = inner[last_comma + 1:].strip()
        if not size_str:
            _error(f"custom_dds_types message '{msg_name}' field '{field_name}': "
                   f"array type must have a size")
        # Recursively validate inner type
        _validate_custom_dds_field_type(inner_type, enum_names, msg_names,
                                         msg_name, field_name)
        return
    if field_type.startswith("vector<"):
        if not field_type.endswith(">"):
            _error(f"custom_dds_types message '{msg_name}' field '{field_name}': "
                   f"malformed vector type '{field_type}' (missing '>')")
        inner_type = field_type[7:-1].strip()
        _validate_custom_dds_field_type(inner_type, enum_names, msg_names,
                                         msg_name, field_name)
        return
    _error(f"custom_dds_types message '{msg_name}' field '{field_name}': "
           f"unknown type '{field_type}'. Valid types: "
           f"{sorted(CUSTOM_DDS_PRIMITIVE_TYPES)}, defined enums, "
           f"defined messages, array<T, N>, vector<T>")


def _extract_nested_msg_refs(fields: list, msg_names: set) -> set:
    """Extract all message-name references from a message's fields.

    Handles direct references, array<MsgName, N>, and vector<MsgName>.
    """
    refs = set()
    for f in fields:
        t = f["type"]
        if t in msg_names:
            refs.add(t)
        elif t.startswith("array<"):
            inner = t[6:-1]
            last_comma = inner.rfind(",")
            if last_comma >= 0:
                inner_type = inner[:last_comma].strip()
                if inner_type in msg_names:
                    refs.add(inner_type)
        elif t.startswith("vector<"):
            inner_type = t[7:-1].strip()
            if inner_type in msg_names:
                refs.add(inner_type)
    return refs


def _detect_circular_message_refs(messages: list, msg_names: set):
    """Detect circular references among custom_dds_types messages.

    Circular references (including self-references) are rejected because
    FastCDR serialization would recurse infinitely on value-type structs.
    """
    # Build adjacency: msg_name -> set of msg_names it references
    deps = {}
    for msg in messages:
        deps[msg["name"]] = _extract_nested_msg_refs(msg["fields"], msg_names)

    # DFS-based cycle detection
    WHITE, GRAY, BLACK = 0, 1, 2
    color = {name: WHITE for name in msg_names}

    def dfs(node, path):
        color[node] = GRAY
        path.append(node)
        for dep in deps.get(node, set()):
            if color[dep] == GRAY:
                # Found a cycle
                cycle_start = path.index(dep)
                cycle = path[cycle_start:]
                cycle_str = " -> ".join(cycle + [dep])
                _error(f"custom_dds_types: circular reference detected: "
                       f"{cycle_str}. Value-type structs cannot be "
                       f"self-referential or mutually recursive.")
            if color[dep] == WHITE:
                dfs(dep, path)
        path.pop()
        color[node] = BLACK

    for name in msg_names:
        if color[name] == WHITE:
            dfs(name, [])


def _validate_custom_dds_types(data: dict):
    """Validate the custom_dds_types section."""
    cdt = data.get("custom_dds_types")
    if not cdt:
        return

    if not isinstance(cdt, dict):
        _error("custom_dds_types must be a mapping")

    messages = cdt.get("messages")
    if not messages:
        _error("custom_dds_types must have at least one message in 'messages'")

    # Collect enum names for type reference validation
    enum_names = set()
    for enum_def in cdt.get("enums", []):
        if "name" not in enum_def:
            _error("Each custom_dds_types enum must have a 'name' field")
        if enum_def["name"] in enum_names:
            _error(f"Duplicate custom_dds_types enum name: '{enum_def['name']}'")
        enum_names.add(enum_def["name"])

        if "values" not in enum_def or not enum_def["values"]:
            _error(f"custom_dds_types enum '{enum_def['name']}' must have non-empty 'values'")

        ut = enum_def.get("underlying_type", "uint32")
        if ut not in CUSTOM_DDS_ENUM_UNDERLYING:
            _error(f"custom_dds_types enum '{enum_def['name']}' has invalid "
                   f"underlying_type '{ut}'. Valid: {sorted(CUSTOM_DDS_ENUM_UNDERLYING)}")

        for v in enum_def["values"]:
            if "name" not in v:
                _error(f"Enum '{enum_def['name']}' value must have a 'name'")
            if "value" not in v:
                _error(f"Enum '{enum_def['name']}' value '{v['name']}' must have a 'value'")

    # Validate constants
    const_names = set()
    for const_def in cdt.get("constants", []):
        if "name" not in const_def:
            _error("Each custom_dds_types constant must have a 'name' field")
        if "type" not in const_def:
            _error(f"custom_dds_types constant '{const_def['name']}' must have a 'type'")
        if "value" not in const_def:
            _error(f"custom_dds_types constant '{const_def['name']}' must have a 'value'")
        if const_def["type"] not in CUSTOM_DDS_PRIMITIVE_TYPES:
            _error(f"custom_dds_types constant '{const_def['name']}' has invalid "
                   f"type '{const_def['type']}'")
        const_names.add(const_def["name"])

    # Validate messages: first pass — collect names and check structure
    msg_names = set()
    for msg in messages:
        if "name" not in msg:
            _error("Each custom_dds_types message must have a 'name' field")
        if msg["name"] in msg_names:
            _error(f"Duplicate custom_dds_types message name: '{msg['name']}'")
        msg_names.add(msg["name"])

        if "fields" not in msg or not msg["fields"]:
            _error(f"custom_dds_types message '{msg['name']}' must have non-empty 'fields'")

    # Validate messages: second pass — validate field types (now msg_names is complete)
    for msg in messages:
        field_names = set()
        for f in msg["fields"]:
            if "name" not in f:
                _error(f"Field in message '{msg['name']}' must have a 'name'")
            if "type" not in f:
                _error(f"Field '{f.get('name', '?')}' in message '{msg['name']}' "
                       f"must have a 'type'")
            if f["name"] in field_names:
                _error(f"Duplicate field name '{f['name']}' in message '{msg['name']}'")
            field_names.add(f["name"])
            _validate_custom_dds_field_type(f["type"], enum_names, msg_names,
                                             msg["name"], f["name"])

    # Detect circular references among messages
    _detect_circular_message_refs(messages, msg_names)


def _validate_extensions(data: dict):
    """Validate the optional extensions section."""
    ext = data.get("extensions")
    if not ext:
        return
    if not isinstance(ext, dict):
        _error("extensions must be a mapping")
    items = ext.get("items")
    if items is not None:
        if not isinstance(items, list):
            _error("extensions.items must be a list")
        for item in items:
            if not isinstance(item, dict):
                _error("Each extensions.items entry must be a mapping")
            if "name" not in item:
                _error("Each extensions.items entry must have a 'name'")
            if "path" not in item:
                _error(f"Extension '{item['name']}' must have a 'path'")


def _validate_dds_driver(data: dict):
    """Validate the optional dds_driver section (QoS defaults + ROS2 interop)."""
    dd = data.get("dds_driver")
    if not dd:
        return
    if not isinstance(dd, dict):
        _error("dds_driver must be a mapping")
    # Validate known fields
    valid_fields = {
        "history", "depth", "reliability", "durability", "data_sharing",
        "domain_id",
        "ros2_compatible_mode", "ros2_topic_prefix",
    }
    for key in dd:
        if key not in valid_fields:
            _warn(f"dds_driver: unknown field '{key}'")


def _validate_logging(data: dict):
    """Validate the optional logging section."""
    log = data.get("logging")
    if not log:
        return
    if not isinstance(log, dict):
        _error("logging must be a mapping")

    # core_level is optional (default: INFO)
    cl = log.get("core_level", "INFO")
    valid_levels = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"}
    if cl not in valid_levels:
        _error(f"logging.core_level '{cl}' is invalid. "
               f"Valid: {sorted(valid_levels)}")

    # backends is optional; when present, must be a list
    backends = log.get("backends")
    if backends is not None:
        if not isinstance(backends, list):
            _error("logging.backends must be a list")
        for i, be in enumerate(backends):
            if not isinstance(be, dict):
                _error(f"logging.backends[{i}] must be a mapping")
            if "type" not in be:
                _error(f"logging.backends[{i}] must have a 'type'")
            if be["type"] not in VALID_LOG_BACKEND_TYPES:
                _error(f"logging.backends[{i}] has invalid type '{be['type']}'. "
                       f"Valid: {sorted(VALID_LOG_BACKEND_TYPES)}")
            # Options are free-form, type-specific — just check it's a dict
            opts = be.get("options")
            if opts is not None and not isinstance(opts, dict):
                _error(f"logging.backends[{i}].options must be a mapping")

            # Cross-validate executor references
            if opts:
                for exec_key in ("log_executor_name", "sync_executor_name"):
                    exec_name = opts.get(exec_key)
                    if exec_name:
                        executors = data.get("executors", [])
                        exec_names = {e["name"] for e in executors if "name" in e}
                        if exec_names and exec_name not in exec_names:
                            _warn(f"logging.backends[{i}].options.{exec_key} "
                                  f"references executor '{exec_name}' which is "
                                  f"not defined in executors section")


def _validate_executors(data: dict):
    """Validate the optional executors section."""
    execs = data.get("executors")
    if not execs:
        return
    if not isinstance(execs, list):
        _error("executors must be a list")

    names = set()
    for i, ex in enumerate(execs):
        if not isinstance(ex, dict):
            _error(f"executors[{i}] must be a mapping")
        if "name" not in ex:
            _error(f"executors[{i}] must have a 'name'")
        if "type" not in ex:
            _error(f"executors[{i}] ('{ex.get('name', '?')}') must have a 'type'")
        if ex["type"] not in VALID_EXECUTOR_TYPES:
            _error(f"executors[{i}] ('{ex['name']}') has invalid type '{ex['type']}'. "
                   f"Valid: {sorted(VALID_EXECUTOR_TYPES)}")
        if ex["name"] in names:
            _error(f"Duplicate executor name: '{ex['name']}'")
        names.add(ex["name"])

        # Options are type-specific — just check it's a dict if present
        opts = ex.get("options")
        if opts is not None and not isinstance(opts, dict):
            _error(f"executors[{i}] ('{ex['name']}').options must be a mapping")


def _validate_experimental_actions(data: dict[str, Any]) -> None:
    """Validate the private experimental Action generator lane.

    This lane is intentionally restricted to a single DDS-only proof for
    example_interfaces/action/Fibonacci. It is not part of module type
    validation and does not accept arbitrary ROS2 Action names.
    """
    actions = data.get("experimental_actions")
    if actions is None:
        return
    if not isinstance(actions, list):
        _error("experimental_actions must be a list")
    seen = set()
    for i, action in enumerate(actions):
        if not isinstance(action, dict):
            _error(f"experimental_actions[{i}] must be a mapping")
        name = action.get("name", "fibonacci")
        if name != "fibonacci":
            _error("experimental_actions is currently Fibonacci-only; "
                   "name must be 'fibonacci'")
        action_name = action.get("action", EXPERIMENTAL_ACTION_FIBONACCI)
        if action_name != EXPERIMENTAL_ACTION_FIBONACCI:
            _error("experimental_actions is currently restricted to "
                   f"'{EXPERIMENTAL_ACTION_FIBONACCI}'")
        transport = action.get("transport", "dds")
        if transport != "dds":
            _error("experimental_actions Fibonacci proof supports only "
                   "transport: dds")
        roles = action.get("roles", ["server", "client"])
        if not isinstance(roles, list) or not roles:
            _error("experimental_actions Fibonacci proof requires a non-empty "
                   "roles list")
        if not all(isinstance(role, str) for role in roles):
            _error("experimental_actions Fibonacci proof roles must be strings")
        unknown_roles = set(roles) - EXPERIMENTAL_ACTION_ROLES
        if unknown_roles:
            _error("experimental_actions Fibonacci proof roles must be one or "
                   f"both of {sorted(EXPERIMENTAL_ACTION_ROLES)}; got "
                   f"{sorted(unknown_roles)}")
        if len(set(roles)) != len(roles):
            _error("experimental_actions Fibonacci proof roles must be unique")
        if name in seen:
            _error("Duplicate experimental_actions entry: 'fibonacci'")
        seen.add(name)
        action["name"] = name
        action["action"] = action_name
        action["transport"] = transport
        action["roles"] = roles


def _validate_modules(data: dict):
    """Validate the modules section."""
    if "modules" not in data:
        _error("Missing required top-level key: 'modules'")

    # Collect valid message names from dds_types and custom_dds_types
    dds_msg_names = set()
    dds = data.get("dds_types")
    if dds:
        for g in dds.get("groups", []):
            for msg in g.get("messages", []):
                dds_msg_names.add(msg["name"])
    cdt = data.get("custom_dds_types")
    cdt_msg_names = set()
    if cdt:
        for msg in cdt.get("messages", []):
            cdt_msg_names.add(msg["name"])

    module_names = set()
    for mod in data["modules"]:
        if "name" not in mod:
            _error("Each module must have a 'name' field")
        if "type" not in mod:
            _error(f"Module '{mod['name']}' must have a 'type' field")
        if mod["type"] not in VALID_MODULE_TYPES:
            _error(f"Module '{mod['name']}' has invalid type '{mod['type']}'. "
                   f"Valid types: {sorted(VALID_MODULE_TYPES)}")
        # R10 alias normalization: translate deprecated
        # ros2_interop_* → direct_ros2_* in-place + emit deprecation warn.
        _normalize_module_type_alias(mod)
        # R10 advisory: direct_<transport>_* templates are explicit
        # escape hatches that couple user code to a named transport. Emit
        # a one-time advisory so users know they are opting out of the
        # transport-decoupling rules R3/R4 by design.
        if mod["type"].startswith("direct_"):
            _warn(f"Module '{mod['name']}' (type={mod['type']}): direct_* "
                  f"templates are R10 escape hatches — generated code is "
                  f"transport-coupled by design. Prefer transport-agnostic "
                  f"templates (publisher/subscriber/loaned_*) when the typed "
                  f"API suffices. See skills/transport_decoupling.md R10.")
        if mod["name"] in module_names:
            _error(f"Duplicate module name: '{mod['name']}'")
        module_names.add(mod["name"])

        if mod["type"] in ("publisher", "subscriber"):
            if "transport" not in mod:
                _error(f"Module '{mod['name']}' (type={mod['type']}) requires 'transport'")
            if mod["transport"] not in _transports.names():
                _error(f"Module '{mod['name']}' has invalid transport "
                       f"'{mod['transport']}'. Registered transports: "
                       f"{sorted(_transports.names())}")
            if "topic" not in mod:
                _error(f"Module '{mod['name']}' (type={mod['type']}) requires 'topic'")
            if "message_type" not in mod:
                _error(f"Module '{mod['name']}' (type={mod['type']}) requires 'message_type'")

            # For DDS transport, validate message_type against dds_types or custom_dds_types
            if mod["transport"] == "dds":
                msg_type = mod["message_type"]
                if msg_type not in dds_msg_names and msg_type not in cdt_msg_names:
                    if dds_msg_names or cdt_msg_names:
                        _warn(f"Module '{mod['name']}': message_type '{msg_type}' "
                              f"not found in dds_types or custom_dds_types")
                # Tag whether this module uses custom_dds_types
                if msg_type in cdt_msg_names:
                    mod["_uses_custom_dds_types"] = True

        # Bare-like types (bare, logger) — no transport requirements
        if mod["type"] in BARE_LIKE_MODULE_TYPES:
            pass  # No additional validation needed

        # Validate log_level if present
        log_level = mod.get("log_level")
        if log_level:
            valid_levels = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "OFF"}
            if log_level not in valid_levels:
                _error(f"Module '{mod['name']}' has invalid log_level '{log_level}'. "
                       f"Valid: {sorted(valid_levels)}")

        if mod["type"] in ("rpc_server", "rpc_client"):
            transport = mod.get("transport", "")
            if transport == "dds":
                if "dds_service" not in mod:
                    _error(f"Module '{mod['name']}' (DDS RPC) requires 'dds_service'")
            elif transport == "json":
                if "service_name" not in mod:
                    _error(f"Module '{mod['name']}' (JSON RPC) requires 'service_name'")
                if "methods" not in mod or not mod["methods"]:
                    _error(f"Module '{mod['name']}' (JSON RPC) requires 'methods'")
            elif transport == "ros2":
                if "ros2_package" not in mod:
                    _error(f"Module '{mod['name']}' (ROS2 RPC) requires 'ros2_package'")
                # ros2_service is optional; if provided, real code is generated
                # instead of TODO skeletons
                if "ros2_service" in mod:
                    mod["_has_ros2_service_info"] = True
                else:
                    mod["_has_ros2_service_info"] = False

        # direct_* publishers/subscribers — R10 escape hatches.
        # direct_ros2_* use low-level ros2::transport API over DDS;
        # direct_iceoryx_* use publisher_for<T>/subscriber_for<T> over the
        # iceoryx transport path. The legacy aliases ros2_interop_* were
        # normalized above by _normalize_module_type_alias().
        if mod["type"] in ("direct_ros2_publisher", "direct_ros2_subscriber",
                           "ros2_interop_publisher", "ros2_interop_subscriber",
                           "direct_iceoryx_publisher", "direct_iceoryx_subscriber"):
            if "topic" not in mod:
                _error(f"Module '{mod['name']}' (type={mod['type']}) requires 'topic'")
            # Derive ros2_msg_include / ros2_package from message_type
            # before the required-field checks below.
            _derive_ros2_fields(mod)
            if "ros2_msg_include" not in mod:
                _error(f"Module '{mod['name']}' (type={mod['type']}) requires "
                       "'ros2_msg_include' (e.g. 'std_msgs/msg/string.hpp')")
            if "message_type" not in mod:
                _error(f"Module '{mod['name']}' (type={mod['type']}) requires "
                       "'message_type' (e.g. 'std_msgs::msg::String')")
            if mod["type"] in ("direct_ros2_publisher", "direct_ros2_subscriber",
                               "ros2_interop_publisher", "ros2_interop_subscriber"):
                if "ros2_type_name" not in mod:
                    _error(f"Module '{mod['name']}' (type={mod['type']}) requires "
                           "'ros2_type_name' (e.g. 'std_msgs/msg/String')")
                # Transport is always DDS for direct_ros2_* (at config level)
                if mod.get("transport") and mod["transport"] != "dds":
                    _warn(f"Module '{mod['name']}' (type={mod['type']}): transport "
                          f"should be 'dds' (got '{mod['transport']}'); forcing 'dds'")
                mod["transport"] = "dds"
                # Mark for ros2 interop enrichment (cmake_gen / config_gen
                # consume this flag; renamed users can keep depending on it
                # since cmake link logic is identical).
                mod["_is_ros2_interop"] = True
            else:
                if "ros2_package" not in mod:
                    _error(f"Module '{mod['name']}' (type={mod['type']}) requires "
                           "'ros2_package' (e.g. 'geometry_msgs')")
                if mod.get("transport") and mod["transport"] != "iceoryx":
                    _warn(f"Module '{mod['name']}' (type={mod['type']}): transport "
                          f"should be 'iceoryx' (got '{mod['transport']}'); forcing 'iceoryx'")
                mod["transport"] = "iceoryx"
                if "ros2_packages" not in mod:
                    mod["ros2_packages"] = [mod["ros2_package"]]
                mod["_is_direct_iceoryx"] = True

        if mod["type"] in {"loaned_publisher", "loaned_subscriber"}:
            _t = mod.get("transport", "dds")
            if _t not in _SHM_CAPABLE_TRANSPORTS:
                _yp = data.get("_yaml_path", "<unknown>")
                sys.stderr.write(
                    f"[R8.2-error] yaml=<{_yp}> module={mod['name']} "
                    f"transport={_t}: loaned_{mod['type'].split('_')[1]} "
                    f"requires an SHM-capable transport "
                    f"({sorted(_SHM_CAPABLE_TRANSPORTS)}). Found: {_t}.\n"
                )

        # loaned_publisher uses the public SenderAccess<T> + LoanedSample<T>
        # API for both supported transports. It is strict-only and requires a
        # trivially copyable message type, enforced by generated code.
        if mod["type"] == "loaned_publisher":
            if "topic" not in mod:
                _error(f"Module '{mod['name']}' (loaned_publisher) requires 'topic'")
            if "message_type" not in mod:
                _error(f"Module '{mod['name']}' (loaned_publisher) requires "
                       "'message_type' (e.g. 'geometry_msgs::msg::Point')")
            # Auto-derive ros2 fields from message_type so the user
            # yaml stays minimal. No-op if they're already explicit.
            _derive_ros2_fields(mod)
            if "ros2_msg_include" not in mod:
                _error(f"Module '{mod['name']}' (loaned_publisher) requires "
                       "'ros2_msg_include' (e.g. 'geometry_msgs/msg/point.hpp')")
            if "ros2_package" not in mod:
                _error(f"Module '{mod['name']}' (loaned_publisher) requires "
                       "'ros2_package' (e.g. 'geometry_msgs')")

            transport = mod.get("transport", "dds")
            if transport not in {"dds", "iceoryx"}:
                _error(f"Module '{mod['name']}' (loaned_publisher): unsupported "
                       f"transport '{transport}' (valid: 'dds', 'iceoryx')")
            mod["transport"] = transport

            if transport == "dds":
                # ros2_type_name default: derive from message_type using ROS2 DDS
                # naming convention. User supplies wire-format string
                # ("pkg/msg/Type"); if absent, derive from "pkg::msg::Type".
                if "ros2_type_name" not in mod:
                    mt = mod["message_type"]
                    parts = mt.split("::")
                    if len(parts) >= 3 and parts[-2] == "msg":
                        mod["ros2_type_name"] = "/".join(parts)
                    else:
                        _error(f"Module '{mod['name']}' (loaned_publisher,dds): "
                               f"cannot derive ros2_type_name from message_type "
                               f"'{mt}'; please specify 'ros2_type_name' "
                               f"(e.g. 'geometry_msgs/msg/Point')")
                # dds_type_name default: same as ros2_type_name (DDS wire format).
                if "dds_type_name" not in mod:
                    mod["dds_type_name"] = mod["ros2_type_name"]
                zc = mod.get("zero_copy_config", "strict")
                if zc != "strict":
                    _error(f"Module '{mod['name']}' (loaned_publisher,dds): "
                           f"zero_copy_config must be 'strict' (got '{zc}'); "
                           "the public loaned API has no copy fallback")
                mod["zero_copy_config"] = "strict"
                # NOTE: loaned_publisher does NOT set _is_ros2_interop. cmake_gen
                # has its own dedicated _is_loaned_publisher branch which links
                # public ROS2 TypeSupport + ${pkg}_TARGETS deps. Concrete
                # transport selection remains in runtime configuration.
            else:  # transport == "iceoryx"
                # iceoryx loaned_publisher: `iceoryx_msg_type` was removed from
                # required fields because it is only a logging placeholder (the wire key is derived from
                # ros2_type_name via "ros2:" prefix). iceoryx plugin now falls
                # back to message_type when the yaml omits this field.
                # zero_copy_config: iceoryx ONLY supports 'strict' (true zero-copy
                # via SHM loan; no fallback path exists in the iceoryx driver).
                # Reject auto/compat at schema time — surface intent mismatch
                # early instead of silently degrading.
                zc = mod.get("zero_copy_config", "strict")
                if zc != "strict":
                    _error(f"Module '{mod['name']}' (loaned_publisher,iceoryx): "
                           f"zero_copy_config must be 'strict' (got '{zc}'); "
                           f"iceoryx transport supports only true zero-copy SHM "
                           f"loan with no fallback path. Use transport='dds' if "
                           f"you need '{zc}' semantics.")
                mod["zero_copy_config"] = "strict"
                # NOTE: iceoryx branch does NOT set _is_ros2_interop. cmake_gen
                # has its own public TypeSupport dependency branch.

            mod["_is_loaned_publisher"] = True
            # ros2_packages list (cmake_gen iterates it for ${pkg}_TARGETS)
            # — used by both dds and iceoryx branches to bring in the message
            # struct (.hpp) via find_package(geometry_msgs) + rosidl deps.
            if "ros2_packages" not in mod:
                mod["ros2_packages"] = [mod["ros2_package"]]

        # loaned_subscriber — receive-side counterpart of loaned_publisher.
        # Two transports supported (mirrors loaned_publisher); zero driver
        # injection required because the framework's high-level abstractions
        # already dispatch to the right driver:
        #   - iceoryx : ibmw::transport::LoanedSubscriber<T>::AttachBytes()
        #   - dds     : ibmw::ros2::transport::OnMessage<T>(...)
        # See docs/contributing/architecture-rules.md for the design rationale.
        if mod["type"] == "loaned_subscriber":
            if "topic" not in mod:
                _error(f"Module '{mod['name']}' (loaned_subscriber) requires 'topic'")
            if "message_type" not in mod:
                _error(f"Module '{mod['name']}' (loaned_subscriber) requires "
                       "'message_type' (e.g. 'geometry_msgs::msg::Point')")
            # Derive ros2 fields from message_type (same rules as
            # loaned_publisher; keep paired modules consistent).
            _derive_ros2_fields(mod)
            if "ros2_msg_include" not in mod:
                _error(f"Module '{mod['name']}' (loaned_subscriber) requires "
                       "'ros2_msg_include' (e.g. 'geometry_msgs/msg/point.hpp')")
            if "ros2_package" not in mod:
                _error(f"Module '{mod['name']}' (loaned_subscriber) requires "
                       "'ros2_package' (e.g. 'geometry_msgs')")

            transport = mod.get("transport", "dds")
            if transport not in {"dds", "iceoryx"}:
                _error(f"Module '{mod['name']}' (loaned_subscriber): unsupported "
                       f"transport '{transport}' (valid: 'dds', 'iceoryx')")
            mod["transport"] = transport

            # subscriber_style — controls user-facing API shape. Default
            # 'callback' mirrors the existing iox_loan_v2 / dds_loaned
            # subscriber golden samples (override OnMessage(const T&)).
            #
            # Only 'callback' is implemented end-to-end. 'tick'
            # and 'borrow' are reserved style names — schema accepts them
            # so users can experiment but module_gen.py raises
            # NotImplementedError until the modes are wired up.
            style = mod.get("subscriber_style", "callback")
            if style not in VALID_SUBSCRIBER_STYLE:
                _error(f"Module '{mod['name']}' (loaned_subscriber): invalid "
                       f"subscriber_style '{style}' "
                       f"(valid: {sorted(VALID_SUBSCRIBER_STYLE)})")
            mod["subscriber_style"] = style

            if transport == "dds":
                # ros2_type_name default — same derivation rule as
                # loaned_publisher,dds. Required because OnMessage<T> is
                # registered with the ROS2 typesupport name.
                if "ros2_type_name" not in mod:
                    mt = mod["message_type"]
                    parts = mt.split("::")
                    if len(parts) >= 3 and parts[-2] == "msg":
                        mod["ros2_type_name"] = "/".join(parts)
                    else:
                        _error(f"Module '{mod['name']}' (loaned_subscriber,dds): "
                               f"cannot derive ros2_type_name from message_type "
                               f"'{mt}'; please specify 'ros2_type_name' "
                               f"(e.g. 'geometry_msgs/msg/Point')")
            else:  # transport == "iceoryx"
                # iceoryx_msg_type is not required (see the loaned_publisher
                # branch above for rationale). No validation
                # needed here — plugin falls back to message_type.
                pass

            mod["_is_loaned_subscriber"] = True
            if "ros2_packages" not in mod:
                mod["ros2_packages"] = [mod["ros2_package"]]



def _validate_apps(data: dict):
    """Validate the apps section."""
    if "apps" not in data:
        _error("Missing required top-level key: 'apps'")

    module_names = {m["name"] for m in data.get("modules", [])}
    app_names = set()

    for app in data["apps"]:
        if "name" not in app:
            _error("Each app must have a 'name' field")
        if app["name"] in app_names:
            _error(f"Duplicate app name: '{app['name']}'")
        app_names.add(app["name"])

        if "modules" not in app or not app["modules"]:
            _error(f"App '{app['name']}' must have a non-empty 'modules' list")

        for mod_name in app["modules"]:
            if mod_name not in module_names:
                _error(f"App '{app['name']}' references unknown module '{mod_name}'")

        # Default app type
        if "type" not in app:
            app["type"] = "pubsub"
        if app["type"] not in VALID_APP_TYPES:
            _error(f"App '{app['name']}' has invalid type '{app['type']}'")


def _enrich(data: dict):
    """Add computed helper fields to the parsed data."""
    proj = data["project"]
    proj["_namespace_cpp"] = proj["namespace"].replace("-", "_")

    # Build module lookup
    module_map = {}
    for mod in data.get("modules", []):
        mod["_snake_name"] = _to_snake_case(mod["name"])
        module_map[mod["name"]] = mod
    data["_module_map"] = module_map

    # DDS types enrichment
    dds = data.get("dds_types")
    if dds:
        dds["_namespace_cpp"] = _namespace_to_cpp(dds["namespace"])
        dds["_modules"] = _namespace_to_idl_modules(dds["namespace"])

        # Build message-to-group lookup
        msg_to_group = {}
        for g in dds.get("groups", []):
            g["_snake_name"] = _to_snake_case(g["name"])
            g["_external"] = bool(g.get("idl_gencode_target"))
            for msg in g.get("messages", []):
                msg_to_group[msg["name"]] = g["name"]
        dds["_msg_to_group"] = msg_to_group

        # Build group lookup for RPC services
        group_map = {g["name"]: g for g in dds.get("groups", [])}
        for svc in dds.get("rpc_services", []):
            svc["_external"] = bool(svc.get("rpc_gencode_target"))

            # If 'srv' is specified (e.g. "example_interfaces/srv/AddTwoInts"),
            # auto-compute req_dds_type_name / rsp_dds_type_name for each method
            # using the ROS2 DDS type naming convention:
            #   <package>::srv::dds_::<ServiceName>_Request_
            #   <package>::srv::dds_::<ServiceName>_Response_
            srv_field = svc.get("srv")
            if srv_field:
                parts = srv_field.split("/")
                if len(parts) != 3 or parts[1] != "srv":
                    _error(f"RPC service '{svc['name']}': 'srv' must be "
                           f"'<package>/srv/<ServiceName>', got '{srv_field}'")
                ros2_pkg, _, ros2_srv_name = parts
                for method in svc.get("methods", []):
                    if not method.get("req_dds_type_name"):
                        method["req_dds_type_name"] = (
                            f"{ros2_pkg}::srv::dds_::{ros2_srv_name}_Request_")
                    if not method.get("rsp_dds_type_name"):
                        method["rsp_dds_type_name"] = (
                            f"{ros2_pkg}::srv::dds_::{ros2_srv_name}_Response_")

                # Auto-compute ros2_service_name if not explicitly set.
                # Converts CamelCase to snake_case:
                #   "AddTwoInts" -> "add_two_ints"
                if not svc.get("ros2_service_name"):
                    import re
                    snake = re.sub(r'(?<=[a-z0-9])([A-Z])', r'_\1',
                                   re.sub(r'([A-Z]+)([A-Z][a-z])', r'\1_\2',
                                          ros2_srv_name)).lower()
                    svc["ros2_service_name"] = f"/{snake}"

            # Derive RPC header name from rpc_gencode_target or group name
            if svc.get("rpc_header"):
                pass  # user-specified
            elif svc.get("rpc_gencode_target"):
                # External target — RPC header follows the group's IDL name
                svc["_rpc_header"] = f"{svc['group']}.ibmw_rpc.dds.h"
            else:
                # Use group name (= IDL file stem) when available,
                # since cmake_gen passes group name as IDL_FILE_STEM
                stem = svc.get("group", svc["name"])
                svc["_rpc_header"] = f"{stem}.ibmw_rpc.dds.h"

    # Custom DDS types enrichment
    cdt = data.get("custom_dds_types")
    if cdt:
        # Parse the namespace from the user-specified C++ namespace string
        cdt_ns_raw = cdt.get("namespace", proj["_namespace_cpp"])
        cdt["_namespace_cpp"] = cdt_ns_raw
        # Build msg name -> snake_name lookup for header paths
        cdt_msg_map = {}
        for msg in cdt.get("messages", []):
            msg["_snake_name"] = _to_snake_case(msg["name"])
            cdt_msg_map[msg["name"]] = msg
        cdt["_msg_map"] = cdt_msg_map

    # Compute project-level feature flags
    has_dds = any(m.get("transport") == "dds" for m in data.get("modules", []))
    has_custom_dds = bool(cdt)
    has_dds_ext = has_dds  # DDS transport requires dds_extension
    has_ros2_interop = any(m.get("_is_ros2_interop") for m in data.get("modules", []))
    has_ros2_rpc = any(
        m.get("transport") == "ros2" and m["type"] in ("rpc_server", "rpc_client")
        for m in data.get("modules", [])
    )
    data["_has_dds"] = has_dds
    data["_has_custom_dds"] = has_custom_dds
    data["_has_dds_ext"] = has_dds_ext
    data["_needs_fastcdr"] = has_custom_dds
    data["_has_ros2_interop"] = has_ros2_interop
    data["_has_ros2_rpc"] = has_ros2_rpc

    # Collect unique ROS2 packages needed by ros2_interop modules (e.g. geometry_msgs).
    # std_msgs is a transitive dep of ibmw::extensions::ros2_types so not needed.
    ros2_interop_packages = set()
    for m in data.get("modules", []):
        if m.get("_is_ros2_interop"):
            for pkg in m.get("ros2_packages", []):
                ros2_interop_packages.add(pkg)
    data["_ros2_interop_packages"] = sorted(ros2_interop_packages)

    # Independent project-level signals for loaned_publisher modules no longer
    # piggyback on _is_ros2_interop. cmake_gen.py
    # uses these together with the ros2_interop signals to decide whether
    # to emit the whole-archive helper, find_package(<pkg>) calls, and to
    # link ros2_types into loaned-publisher modules (which use the ROS2
    # typesupport via RegisterSendType<T>).
    has_loaned_publisher = any(
        m.get("_is_loaned_publisher") for m in data.get("modules", []))
    data["_has_loaned_publisher"] = has_loaned_publisher
    loaned_publisher_packages = set()
    for m in data.get("modules", []):
        if m.get("_is_loaned_publisher"):
            for pkg in m.get("ros2_packages", []):
                loaned_publisher_packages.add(pkg)
    data["_loaned_publisher_packages"] = sorted(loaned_publisher_packages)

    # Project-level signal for iceoryx-transport
    # loaned_publisher modules. cmake_gen.py needs this to emit
    # find_package(iceoryx_posh REQUIRED) at the top level (the iceoryx_ext
    # SHARED links iceoryx_posh PRIVATE so downstream consumers must locate
    # the import target themselves).
    data["_has_iceoryx_loaned_publisher"] = any(
        m.get("_is_loaned_publisher") and m.get("transport") == "iceoryx"
        for m in data.get("modules", []))

    # Project-level signals for loaned_subscriber mirror the
    # loaned_publisher signals so cmake_gen.py can emit the same set of
    # ros2_types / iceoryx_posh deps for the receive-side modules.
    has_loaned_subscriber = any(
        m.get("_is_loaned_subscriber") for m in data.get("modules", []))
    data["_has_loaned_subscriber"] = has_loaned_subscriber
    loaned_subscriber_packages = set()
    for m in data.get("modules", []):
        if m.get("_is_loaned_subscriber"):
            for pkg in m.get("ros2_packages", []):
                loaned_subscriber_packages.add(pkg)
    data["_loaned_subscriber_packages"] = sorted(loaned_subscriber_packages)
    data["_has_iceoryx_loaned_subscriber"] = any(
        m.get("_is_loaned_subscriber") and m.get("transport") == "iceoryx"
        for m in data.get("modules", []))

    # Project-level signals for direct_iceoryx_* templates.
    # These modules use the iceoryx transport path but also require ROS2
    # message package deps because publisher_for<T>/subscriber_for<T> bind the
    # ros2 SenderAccess/ReceiverAccess specializations for T.
    has_direct_iceoryx = any(
        m.get("_is_direct_iceoryx") for m in data.get("modules", []))
    data["_has_direct_iceoryx"] = has_direct_iceoryx
    direct_iceoryx_packages = set()
    for m in data.get("modules", []):
        if m.get("_is_direct_iceoryx"):
            for pkg in m.get("ros2_packages", []):
                direct_iceoryx_packages.add(pkg)
    data["_direct_iceoryx_packages"] = sorted(direct_iceoryx_packages)


    # App enrichment
    for app in data.get("apps", []):
        app["_snake_name"] = app["name"].replace("-", "_")
        # Determine pkg name
        app_modules = app["modules"]
        if len(app_modules) == 1:
            mod = module_map[app_modules[0]]
            app["_pkg_name"] = f"{mod['_snake_name']}_pkg"
        else:
            app["_pkg_name"] = f"{app['_snake_name']}_pkg"
            # Try a shorter name: replace _app suffix with _pkg
            if app["name"].endswith("_app"):
                app["_pkg_name"] = app["name"][:-4] + "_pkg"


# ── Public API ───────────────────────────────────────────────────────────────

def load_and_validate(yaml_path: str) -> dict:
    """Load a .ibmw.yaml file, validate it, and return the enriched data."""
    if not os.path.exists(yaml_path):
        _error(f"File not found: {yaml_path}")

    with open(yaml_path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f)

    if not isinstance(data, dict):
        _error(f"Invalid YAML: expected a mapping, got {type(data).__name__}")

    data["_yaml_path"] = yaml_path

    # Detect the schema version and run v2→v1 in-memory
    # expansion so all downstream readers see the v1 flat shape.
    schema_v = _detect_schema_version(data)
    data["_schema_version"] = schema_v
    # W3.1: hoist `messages:` block fields onto referring modules. This
    # is a no-op when the yaml has no `messages:` block (typical v1).
    _expand_v2_to_v1_internal(data)

    _validate_project(data)
    _validate_dds_types(data)
    _validate_json_types(data)
    _validate_custom_dds_types(data)
    _validate_extensions(data)
    _validate_dds_driver(data)
    _validate_logging(data)
    _validate_executors(data)
    _validate_experimental_actions(data)
    _validate_modules(data)
    _validate_apps(data)
    _enrich(data)

    return data
