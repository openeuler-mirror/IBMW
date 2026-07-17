#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# ibmw-gen migrate-yaml: rewrite v1 .ibmw.yaml files into v2 form.
#
# Migration behavior:
#   - reads a v1 yaml (no `messages:` top-level block; transport-bound
#     fields flat on each module — `dds_type_name`, `ros2_msg_include`,
#     `dds_service`, …)
#   - emits a semantically-equivalent v2 yaml: hoists per-module wire-level
#     fields into a top-level `messages:` block, keyed by `message_type`,
#     with `wire.<transport>` sub-blocks (decision A.a1).
#   - sets `schema_version: 2`.
#
# Round-trip property: the v2 output, when fed
# through `schema._expand_v2_to_v1_internal()`, MUST produce a module
# list that yields byte-equal generator output to the v1 input.
#
# Usage:
#   python3 ibmw_gen.py migrate-yaml <v1.ibmw.yaml> [--in-place|--dry-run]
#   (or invoke `migrate_yaml.py` directly)

"""v1 → v2 .ibmw.yaml in-place migration tool."""

from __future__ import annotations

import argparse
import difflib
import io
import os
import sys
from typing import Optional

# ruamel.yaml is required for round-trip-preserving edits (comments, key
# order, anchor preservation). Falls back to PyYAML with a loud warning.
try:
    from ruamel.yaml import YAML
    from ruamel.yaml.comments import CommentedMap, CommentedSeq
    _HAVE_RUAMEL = True
except ImportError:  # pragma: no cover — ruamel.yaml ships with project
    _HAVE_RUAMEL = False
    import yaml as _pyyaml


# ── v1 → v2 reverse mapping (mirror of schema._V2_WIRE_TO_V1_MODULE) ────────
#
# Keep aligned with ibmw_gen/schema.py:_V2_WIRE_TO_V1_MODULE.
# (transport_name, [(v1_module_field, v2_wire_field), ...])
_V1_MODULE_TO_V2_WIRE = {
    "dds": [
        ("dds_type_name", "type_name"),
        ("dds_namespace_cpp", "namespace_cpp"),
        ("dds_service", "service"),
    ],
    "ros2": [
        ("ros2_msg_include", "include"),
        ("ros2_type_name", "type"),
        ("ros2_package", "package"),
        ("ros2_packages", "packages"),
        ("ros2_service", "service"),
        ("ros2_rpc_gencode_target", "rpc_gencode_target"),
    ],
    "iceoryx": [
        ("iceoryx_msg_type", "msg_type"),
    ],
}

# Module-level fields that semantically belong to the message envelope but
# are NOT transport-bound — kept on the module as-is (W3.3 user-review may
# move them).
_KEEP_ON_MODULE = {
    "name", "type", "transport", "topic", "message_type",
    "zero_copy_config", "custom_config", "subscriber_style",
    "modules", "executor", "ros2_node_name",
}


def _make_yaml() -> "YAML":  # type: ignore[return-value]
    """Return a configured ruamel.yaml YAML round-trip instance."""
    if not _HAVE_RUAMEL:
        raise RuntimeError(
            "ruamel.yaml not installed; install it for round-trip "
            "comment-preserving migration."
        )
    y = YAML(typ="rt")
    y.preserve_quotes = True
    y.width = 4096          # don't auto-wrap long lines
    y.indent(mapping=2, sequence=4, offset=2)
    return y


def _module_implicit_transports(mod: dict) -> set[str]:
    """`direct_ros2_*` and `ros2_interop_*` carry implicit ros2 transport."""
    mtype = mod.get("type") or ""
    if mtype.startswith("direct_ros2_") or mtype.startswith("ros2_interop_"):
        return {"ros2"}
    if mtype.startswith("direct_iceoryx_"):
        return {"iceoryx"}
    return set()


def _module_transports(mod: dict) -> set[str]:
    """Effective transport set for a module (declared + implicit)."""
    out: set[str] = set()
    declared = mod.get("transport")
    if declared:
        out.add(declared)
    out |= _module_implicit_transports(mod)
    return out


def _extract_wire_fields(mod: dict, transport: str) -> dict:
    """Pull v1 wire-level fields off a module → v2 wire sub-block dict."""
    wire: dict = {}
    for v1key, v2key in _V1_MODULE_TO_V2_WIRE.get(transport, []):
        if v1key in mod:
            wire[v2key] = mod[v1key]
    return wire


def _strip_wire_fields(mod, transport: str) -> None:
    """Remove v1 wire-level fields that have been hoisted to messages: block."""
    for v1key, _v2key in _V1_MODULE_TO_V2_WIRE.get(transport, []):
        if v1key in mod:
            del mod[v1key]


def _message_key_for_module(mod: dict) -> Optional[str]:
    """Pick a stable message-block key name for this module.

    Priority:
      1. explicit `message_type` (most v1 yamls)
      2. `dds_service` / `ros2_service` (RPC modules — keyed by service)
      3. None — module has no transport-bound payload, skip
    """
    if mod.get("message_type"):
        return str(mod["message_type"])
    # RPC modules: prefer dds_service then ros2_service as the message key
    for key in ("dds_service", "ros2_service"):
        if mod.get(key):
            return str(mod[key])
    return None


def _detect_v1(data) -> bool:
    """Return True if the document is in v1 form (no messages: block)."""
    if not isinstance(data, dict):
        return False
    sv = data.get("schema_version", 1)
    return sv == 1 and not data.get("messages")


def _stamp_schema_version(data) -> None:
    """Insert/replace schema_version: 2 at the top of the document."""
    if hasattr(data, "insert"):
        if "schema_version" in data:
            del data["schema_version"]
        data.insert(0, "schema_version", 2)
    else:
        data["schema_version"] = 2


def migrate(data):
    """In-place v1 → v2 migration of a parsed yaml document.

    Returns the same `data` object (for chaining). Raises ValueError if
    the document is already v2 or otherwise unsuitable.
    """
    if not _detect_v1(data):
        raise ValueError(
            "Document is already v2 (has 'messages:' block or "
            "schema_version >= 2). Migration is a no-op; aborting."
        )

    modules = data.get("modules") or []
    if not modules:
        # Nothing to hoist; just stamp version and return.
        _stamp_schema_version(data)
        return data

    # ── Pass 1: collect per-message-key wire blocks across all modules ──
    # message_key → { transport_name → wire_dict }   (wire_dict may be empty
    # — empty wire is still meaningful: it tells Pass 2 the module owns a
    # message reference even when no wire-level overrides are present.)
    messages: dict[str, dict[str, dict]] = {}

    for mod in modules:
        if not isinstance(mod, dict):
            continue
        msg_key = _message_key_for_module(mod)
        if not msg_key:
            continue
        slot = messages.setdefault(msg_key, {})
        for transport in _module_transports(mod):
            wire = _extract_wire_fields(mod, transport)
            existing = slot.get(transport)
            if existing is None:
                slot[transport] = wire
            else:
                # Merge: last-write-wins per key, but warn on conflict.
                for k, v in wire.items():
                    if k in existing and existing[k] != v:
                        print(f"warning: message '{msg_key}' wire.{transport}.{k} "
                              f"conflict ({existing[k]!r} vs {v!r}); keeping first",
                              file=sys.stderr)
                    else:
                        existing[k] = v

    # ── Pass 2: rewrite modules ── strip hoisted fields, rename message_type → message
    for mod in modules:
        if not isinstance(mod, dict):
            continue
        msg_key = _message_key_for_module(mod)
        if not msg_key or msg_key not in messages:
            continue
        for transport in _module_transports(mod):
            _strip_wire_fields(mod, transport)
        # Replace `message_type` with `message` (v2 spelling), preserving
        # position in the mapping.
        if "message_type" in mod:
            # ruamel CommentedMap supports insert(pos, key, value)
            if hasattr(mod, "insert"):
                pos = list(mod.keys()).index("message_type")
                val = mod["message_type"]
                del mod["message_type"]
                mod.insert(pos, "message", val)
            else:
                val = mod.pop("message_type")
                mod["message"] = val
        elif msg_key:
            # RPC-style: no message_type was present; insert message at end.
            if hasattr(mod, "insert"):
                mod.insert(len(mod), "message", msg_key)
            else:
                mod["message"] = msg_key

    # ── Pass 3: build top-level messages: block ──
    if not messages:
        # No transport-bound modules at all (pure bare/logger); just stamp.
        _stamp_schema_version(data)
        return data

    if _HAVE_RUAMEL:
        msg_block = CommentedMap()
        for msg_key, transports in messages.items():
            msg_entry = CommentedMap()
            # Drop empty wire dicts; only emit wire: when there is at least
            # one transport with at least one field (defaults stay implicit).
            non_empty = {t: w for t, w in transports.items() if w}
            if non_empty:
                wire_block = CommentedMap()
                for tname, wire in non_empty.items():
                    wire_block[tname] = CommentedMap(wire)
                msg_entry["wire"] = wire_block
            msg_block[msg_key] = msg_entry
    else:
        msg_block = {}
        for msg_key, transports in messages.items():
            non_empty = {t: dict(w) for t, w in transports.items() if w}
            entry: dict = {}
            if non_empty:
                entry["wire"] = non_empty
            msg_block[msg_key] = entry

    # Insert messages: block after `project:` (or at top if no project).
    if hasattr(data, "insert"):
        keys = list(data.keys())
        anchor = "project" if "project" in keys else None
        pos = (keys.index(anchor) + 1) if anchor else 0
        data.insert(pos, "messages", msg_block)
    else:
        # PyYAML fallback: order not preserved
        data["messages"] = msg_block

    _stamp_schema_version(data)
    return data


def _dump_yaml(data) -> str:
    """Round-trip-dump `data` to a string."""
    if _HAVE_RUAMEL:
        y = _make_yaml()
        buf = io.StringIO()
        y.dump(data, buf)
        return buf.getvalue()
    else:  # pragma: no cover
        return _pyyaml.safe_dump(data, sort_keys=False, allow_unicode=True)


def _load_yaml(path: str):
    if _HAVE_RUAMEL:
        y = _make_yaml()
        with open(path, encoding="utf-8") as f:
            return y.load(f)
    else:  # pragma: no cover
        with open(path, encoding="utf-8") as f:
            return _pyyaml.safe_load(f)


def run_cli(argv: Optional[list[str]] = None) -> int:
    """Entry point usable both standalone and as `ibmw-gen migrate-yaml`."""
    parser = argparse.ArgumentParser(
        prog="ibmw-gen migrate-yaml",
        description="Migrate a v1 .ibmw.yaml file to the v2 schema "
                    "(adds top-level `messages:` block; rewrites "
                    "module wire-level fields).",
    )
    parser.add_argument("yaml_file", help="Path to v1 .ibmw.yaml input")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--in-place", action="store_true",
                      help="Overwrite the input file with v2 output "
                           "(a .v1.bak backup is written alongside).")
    mode.add_argument("--dry-run", action="store_true",
                      help="Print a unified diff (v1 → v2) to stdout "
                           "without modifying anything (default).")
    parser.add_argument("-o", "--output", default=None,
                        help="Write v2 output to this path instead of stdout/in-place.")

    args = parser.parse_args(argv)

    if not os.path.isfile(args.yaml_file):
        print(f"error: file not found: {args.yaml_file}", file=sys.stderr)
        return 2

    with open(args.yaml_file, encoding="utf-8") as f:
        original = f.read()

    data = _load_yaml(args.yaml_file)

    try:
        migrate(data)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    new_text = _dump_yaml(data)

    if args.in_place:
        bak = args.yaml_file + ".v1.bak"
        with open(bak, "w", encoding="utf-8") as f:
            f.write(original)
        with open(args.yaml_file, "w", encoding="utf-8") as f:
            f.write(new_text)
        print(f"migrated: {args.yaml_file}  (backup: {bak})", file=sys.stderr)
        return 0

    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(new_text)
        print(f"wrote: {args.output}", file=sys.stderr)
        return 0

    # default: dry-run unified diff
    diff = difflib.unified_diff(
        original.splitlines(keepends=True),
        new_text.splitlines(keepends=True),
        fromfile=f"{args.yaml_file} (v1)",
        tofile=f"{args.yaml_file} (v2)",
    )
    sys.stdout.writelines(diff)
    return 0


if __name__ == "__main__":
    sys.exit(run_cli())
