# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

"""Optional per-module `user_code:` snippets spliced into user-code regions.

A module may carry ``user_code: {<region>: <C++ snippet>}``. Each snippet
replaces the default body (TODO placeholder and stub statements) of the
matching user-code region in the generated module files. Modules without
``user_code`` are emitted unchanged.
"""

from __future__ import annotations

import re
import textwrap

from generators._common import _snake

# Region base names accepted per module type. Bases marked as per-method
# take a ``.<Method>`` suffix naming the RPC method (e.g.
# ``rpc_server_handler_body.AddTwoInts``).
_PUBLISHER = ("publish_body",)
_SUBSCRIBER = ("on_message_body",)
ALLOWED_REGIONS: dict[str, tuple[str, ...]] = {
    "publisher": _PUBLISHER,
    "direct_ros2_publisher": _PUBLISHER,
    "direct_iceoryx_publisher": _PUBLISHER,
    "subscriber": _SUBSCRIBER,
    "direct_ros2_subscriber": _SUBSCRIBER,
    "direct_iceoryx_subscriber": _SUBSCRIBER,
    "loaned_subscriber": _SUBSCRIBER,
    "loaned_publisher": ("populate_loaned_message",),
    "rpc_server": ("rpc_server_handler_body",),
    "rpc_client": (
        "rpc_client_request_parameter_build",
        "rpc_client_response_parameter_consume",
        "rpc_client_return_status_handling",
    ),
}
_PER_METHOD_REGIONS = {
    "rpc_server_handler_body",
    "rpc_client_request_parameter_build",
    "rpc_client_response_parameter_consume",
    "rpc_client_return_status_handling",
}
# direct_* templates carry a bare placeholder line instead of a marked
# region; user code replaces that line and is wrapped in region markers.
_DIRECT_PLACEHOLDERS = {
    "publish_body": "// TODO: populate msg fields here",
    "on_message_body": "// TODO: process msg fields here",
}
# Regions emitted inside a backslash-continued macro definition.
_MACRO_REGIONS = {"populate_loaned_message"}

_ANCHOR_LINE = re.compile(
    r"^(?://|/\*)\s*IBMW(?::\s*user code\s+(?:begin|end)\b|_\w*_(?:BEGIN|END)\b)")
_BAD_CHARS = re.compile(r"[\x00-\x08\x0b-\x1f\x7f]")
_METHOD = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


class UserCodeError(ValueError):
    """Raised for an invalid or unplaceable `user_code` entry."""


def _region_list(mod_type: str) -> str:
    return ", ".join(
        f"{base}.<Method>" if base in _PER_METHOD_REGIONS else base
        for base in ALLOWED_REGIONS.get(mod_type, ()))


def validate_user_code(mod: dict) -> None:
    """Raise UserCodeError if the module's `user_code` mapping is invalid."""
    if "user_code" not in mod:
        return
    name = mod.get("name")
    mod_type = mod.get("type")
    user_code = mod["user_code"]
    if not isinstance(user_code, dict):
        raise UserCodeError(f"Module '{name}': 'user_code' must be a mapping of "
                         f"region name -> C++ code")
    if mod_type not in ALLOWED_REGIONS:
        raise UserCodeError(f"Module '{name}': 'user_code' is not supported for "
                         f"module type '{mod_type}'. Supported types: "
                         f"{sorted(ALLOWED_REGIONS)}")
    for region, code in user_code.items():
        region_s = str(region)
        base, _, method = region_s.partition(".")
        per_method = base in _PER_METHOD_REGIONS
        if (base not in ALLOWED_REGIONS[mod_type]
                or per_method != bool(method)
                or (method and not _METHOD.match(method))):
            raise UserCodeError(
                f"Module '{name}' (type={mod_type}): unknown user_code region "
                f"'{region_s}'. Allowed regions: {_region_list(mod_type)}")
        if not isinstance(code, str) or not code.strip():
            raise UserCodeError(f"Module '{name}': user_code '{region_s}' must be "
                             f"a non-empty string of C++ code")
        if _BAD_CHARS.search(code) or "\r" in code:
            raise UserCodeError(f"Module '{name}': user_code '{region_s}' must not "
                             f"contain control characters")
        for line in code.splitlines():
            stripped = line.strip()
            if _ANCHOR_LINE.match(stripped):
                raise UserCodeError(
                    f"Module '{name}': user_code '{region_s}' must not contain "
                    f"IBMW marker anchor lines: {stripped!r}")
            if base in _MACRO_REGIONS and (
                    "//" in stripped or stripped.startswith("#")
                    or stripped.endswith("\\")):
                raise UserCodeError(
                    f"Module '{name}': user_code '{region_s}' is emitted inside "
                    f"a macro body; use /* */ comments and no preprocessor "
                    f"directives or line continuations: {stripped!r}")


def _snippet_lines(code: str, indent: str, macro_col: int | None) -> list[str]:
    body = textwrap.dedent(code).strip("\n").splitlines()
    out = []
    for raw in body:
        text = (indent + raw.rstrip()) if raw.strip() else ""
        if macro_col is not None:
            text = text.ljust(macro_col) + "\\" if len(text) < macro_col else text + " \\"
        out.append(text + "\n")
    return out


def _find_region(files: list[list[str]], begin, end, label: str):
    """Return (file_idx, begin_idx, end_idx) of the unique region, or None."""
    hits = []
    for fi, lines in enumerate(files):
        for bi, line in enumerate(lines):
            if begin(line):
                ei = next((j for j in range(bi + 1, len(lines)) if end(lines[j])), None)
                if ei is not None:
                    hits.append((fi, bi, ei))
    if len(hits) > 1:
        raise UserCodeError(f"{label} appears {len(hits)} times in the generated sources")
    return hits[0] if hits else None


def apply_user_code(mod: dict, files: list[tuple[str, str]]) -> list[tuple[str, str]]:
    """Splice `mod['user_code']` snippets into the module's generated files."""
    user_code = mod.get("user_code")
    if not user_code:
        return files
    name = mod.get("name")
    texts = [content.splitlines(keepends=True) for _, content in files]
    for region, code in user_code.items():
        base, _, method = str(region).partition(".")
        if method:
            stem = f"IBMW_{base.upper()}_{_snake(method).upper()}"
            begin = lambda line, s=stem: line.strip() == f"// {s}_BEGIN"
            end = lambda line, s=stem: line.strip() == f"// {s}_END"
        else:
            pat_b = re.compile(rf"^\s*(?://|/\*) IBMW: user code begin \({base}\)")
            pat_e = re.compile(rf"^\s*(?://|/\*) IBMW: user code end \({base}\)")
            begin, end = pat_b.match, pat_e.match
        label = f"Module '{name}': user_code region '{region}'"
        hit = _find_region(texts, begin, end, label)
        if hit is not None:
            fi, bi, ei = hit
            first = texts[fi][bi]
            indent = first[:len(first) - len(first.lstrip())]
            macro_col = None
            if first.rstrip().endswith("\\"):
                macro_col = len(first.rstrip()) - 1
            texts[fi][bi + 1:ei] = _snippet_lines(code, indent, macro_col)
            continue
        placeholder = _DIRECT_PLACEHOLDERS.get(base)
        if str(mod.get("type", "")).startswith("direct_") and placeholder:
            hit = _find_region(texts, lambda line: line.strip() == placeholder,
                               lambda line: True, label)
            if hit is not None:
                fi, bi, _ = hit
                first = texts[fi][bi]
                indent = first[:len(first) - len(first.lstrip())]
                texts[fi][bi:bi + 1] = (
                    [f"{indent}// IBMW: user code begin ({base})\n"]
                    + _snippet_lines(code, indent, None)
                    + [f"{indent}// IBMW: user code end ({base})\n"])
                continue
        raise UserCodeError(f"{label} was not found in the generated module sources")
    return [(path, "".join(lines)) for (path, _), lines in zip(files, texts)]
