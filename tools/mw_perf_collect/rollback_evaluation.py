"""Build and execute a trace-off rollback candidate without packaging evidence."""
from __future__ import annotations

import hashlib
import io
import json
import os
import re
import select
import shlex
import shutil
import signal
import stat
import subprocess
import tarfile
import time
import uuid
from pathlib import Path
from typing import Any, Mapping, Sequence

from .adapters.ibmw import source_identity
from . import core
from .core import _runtime_observations
from .campaign import (build_companion_object_from_snapshot, capture_payload_snapshot,
                       validate_companion_object)

_REQUIRED_ON = (
    "IBMW_BUILD_TESTS", "IBMW_BUILD_RUNTIME", "IBMW_BUILD_PROTOCOLS",
    "IBMW_BUILD_DDS_EXTENSION",
)
# These are computed informational values, not user controls. Keep this list explicit:
# prefix/suffix heuristics could silently omit a newly registered product control.
_DERIVED_READ_ONLY_IBMW_VALUES = frozenset({
    "IBMW_RUNTIME_VERSION", "IBMW_VERSION", "IBMW_VERSION_MAJOR",
    "IBMW_VERSION_MINOR", "IBMW_VERSION_PATCH", "IBMW_VERSION_TWEAK",
})
_PRODUCT_CACHE_TYPES = frozenset({"BOOL", "STRING", "PATH", "FILEPATH"})
_TOOLCHAIN_EXACT = frozenset({
    "CMAKE_BUILD_TYPE", "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER",
    "CMAKE_C_COMPILER_AR", "CMAKE_CXX_COMPILER_AR",
    "CMAKE_C_COMPILER_RANLIB", "CMAKE_CXX_COMPILER_RANLIB",
    "CMAKE_C_COMPILER_TARGET", "CMAKE_CXX_COMPILER_TARGET",
    "CMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN", "CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN",
    "CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER",
    "CMAKE_TOOLCHAIN_FILE", "CMAKE_SYSROOT", "CMAKE_AR", "CMAKE_RANLIB",
    "CMAKE_NM", "CMAKE_READELF", "CMAKE_OBJCOPY", "CMAKE_STRIP",
    "CMAKE_LINKER", "CMAKE_PREFIX_PATH", "CMAKE_FIND_ROOT_PATH",
    "CMAKE_GENERATOR_PLATFORM", "CMAKE_GENERATOR_TOOLSET", "CMAKE_MAKE_PROGRAM",
    "fastcdr_DIR", "fastrtps_DIR", "fastdds_DIR", "FastDDS_DIR",
    "OPENSSL_ROOT_DIR", "OpenSSL_ROOT_DIR", "OPENSSL_INCLUDE_DIR",
    "OPENSSL_CRYPTO_LIBRARY", "OPENSSL_SSL_LIBRARY",
})
_TOOLCHAIN_PATTERN = re.compile(
    r"^CMAKE_(?:C|CXX)_FLAGS(?:_(?:DEBUG|RELEASE|RELWITHDEBINFO|MINSIZEREL))?$|"
    r"^CMAKE_(?:EXE|SHARED|MODULE|STATIC)_LINKER_FLAGS(?:_(?:DEBUG|RELEASE|RELWITHDEBINFO|MINSIZEREL))?$"
)
_COMPILERS = frozenset({"CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER"})
_EXECUTABLE_TOOLCHAIN_CONTROLS = frozenset({
    "CMAKE_C_COMPILER", "CMAKE_CXX_COMPILER",
    "CMAKE_C_COMPILER_LAUNCHER", "CMAKE_CXX_COMPILER_LAUNCHER",
    "CMAKE_LINKER", "CMAKE_AR", "CMAKE_C_COMPILER_AR", "CMAKE_CXX_COMPILER_AR",
    "CMAKE_RANLIB", "CMAKE_C_COMPILER_RANLIB", "CMAKE_CXX_COMPILER_RANLIB",
    "CMAKE_NM", "CMAKE_READELF", "CMAKE_OBJCOPY", "CMAKE_STRIP", "CMAKE_MAKE_PROGRAM",
})
_REQUIRED_SOURCES = {
    "src/extensions/transport/dds_ext/dds_transport_driver.cc": "ibmw_dds_extension.dir",
    "src/extensions/transport/dds_ext/dds_managed_exact_candidate.cc":
        "ibmw_dds_trace_off_business_roundtrip.dir",
}
_REGISTERED_TARGETS = frozenset({
    "ibmw_dds_extension",
    "ibmw_dds_trace_off_business_roundtrip",
})
_FORBIDDEN_SOURCES = frozenset({
    "src/extensions/transport/dds_ext/dds_comm_trace.cc",
    "src/core/observability/comm_trace.cc",
})
_TRACE_SYMBOL = re.compile(
    r"(?:^|::)comm_trace(?:::|$)|ibmw_dds_comm_trace|comm_trace.*hook|hook.*comm_trace",
    re.IGNORECASE,
)
_HANDSHAKE_TIMEOUT = 30.0
_EXIT_TIMEOUT = 30.0
_LINK_RESPONSE_DEPTH = 8
_NINJA_COMMAND_TIMEOUT = 30.0
_MAX_NINJA_COMMAND_OUTPUT = 16 * 1024 * 1024
_MAX_DEPENDENCY_COUNT = 128
_MAX_DEPENDENCY_ENTRIES = 500_000
_MAX_DEPENDENCY_FILE_SIZE = 1024 * 1024 * 1024
_MAX_DEPENDENCY_BYTES = 16 * 1024 * 1024 * 1024
_MAX_DEPENDENCY_FILES = _MAX_DEPENDENCY_ENTRIES
_MAX_DEPENDENCY_DIRECTORIES = _MAX_DEPENDENCY_ENTRIES
_MAX_DEPENDENCY_TOTAL_BYTES = _MAX_DEPENDENCY_BYTES
_MAX_DEPENDENCY_DEPTH = 128
_DEPENDENCY_CHECKPOINTS = (
    "anchor_before_staging", "staged_after_copy", "anchor_after_staging",
    "staged_after_configure", "anchor_after_configure", "staged_after_build",
    "anchor_after_build",
)
_LINK_TARGET_IMAGES = {
    "ibmw_dds_extension": "extension",
    "ibmw_dds_trace_off_business_roundtrip": "executable",
}
_LINK_FILE_RE = re.compile(
    r"(?:\.o|\.obj|\.a|\.lib|\.dylib|\.dll|\.so(?:\.[0-9A-Za-z._-]+)*)$", re.I)
_PATH_ARGUMENT_OPTIONS = frozenset({
    "-I", "-F", "-isystem", "-iquote", "-idirafter", "-include-pch",
    "-include", "-imacros", "-isysroot", "-iframework",
    "-iframeworkwithsysroot", "-iprefix", "-iwithprefix",
    "-iwithprefixbefore", "-B",
    "--include-directory", "--include-directory-after", "--include", "--imacros",
    "--sysroot", "--include-prefix", "--include-with-prefix",
    "--include-with-prefix-before", "--prefix", "--gcc-toolchain",
    "-fplugin", "-specs", "-L", "--library-path", "-R", "-rpath",
    "--rpath", "-rpath-link", "--rpath-link", "-syslibroot",
})
_PATH_ARGUMENT_JOINED_FORMS = tuple(sorted({
    "--include-directory=", "--include-directory-after=", "--include=", "--imacros=",
    "--sysroot=", "--include-prefix=", "--include-with-prefix=",
    "--include-with-prefix-before=", "--prefix=", "--gcc-toolchain=",
    "--library-path=", "--rpath-link=", "--rpath=",
    "-iframeworkwithsysroot", "-iwithprefixbefore", "-include-pch=",
    "-include-pch", "-iframework", "-iwithprefix", "-rpath-link=",
    "-idirafter", "-isystem=", "-isystem", "-isysroot", "-iquote",
    "-imacros", "-iprefix", "-include=", "-include", "-syslibroot=",
    "-fplugin=", "-specs=", "-rpath=", "-I=", "-F=", "-B=", "-L=", "-R=",
    "-I", "-F", "-B", "-L", "-R",
}, key=len, reverse=True))
_FORWARDED_OPTION_MARKERS = frozenset({"-Xclang", "-Xpreprocessor", "-Xlinker"})
_COMMA_FORWARDED_PREFIXES = ("-Wl,", "-Wp,")


def parse_cmake_cache(value: str | Path) -> dict[str, dict[str, str]]:
    """Parse non-comment CMake cache records without interpreting their values."""
    text = value.read_text(encoding="utf-8") if isinstance(value, Path) else value
    parsed: dict[str, dict[str, str]] = {}
    for line in text.splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line or ":" not in line.split("=", 1)[0]:
            continue
        name_type, item = line.split("=", 1)
        name, cache_type = name_type.rsplit(":", 1)
        parsed[name] = {"type": cache_type, "value": item}
    return parsed


def selected_options(cache: Mapping[str, Mapping[str, str]]) -> dict[str, str]:
    return {name: item["value"] for name, item in sorted(cache.items())
            if name.startswith("IBMW_") and item.get("type") in _PRODUCT_CACHE_TYPES
            and name not in _DERIVED_READ_ONLY_IBMW_VALUES}


def toolchain_options(cache: Mapping[str, Mapping[str, str]]) -> dict[str, str]:
    return {name: item["value"] for name, item in sorted(cache.items())
            if (name in _TOOLCHAIN_EXACT or _TOOLCHAIN_PATTERN.fullmatch(name))
            and not item.get("value", "").endswith("-NOTFOUND")}


def option_delta(anchor: Mapping[str, str], fresh: Mapping[str, str]) -> dict[str, dict[str, str | None]]:
    return {name: {"anchor": anchor.get(name), "fresh": fresh.get(name)}
            for name in sorted(set(anchor) | set(fresh)) if anchor.get(name) != fresh.get(name)}


def _canonical_argument_path(raw: str, directory: Path) -> Path:
    candidate = Path(raw)
    if not candidate.is_absolute():
        candidate = directory / candidate
    try:
        return candidate.resolve(strict=False)
    except (OSError, RuntimeError) as exc:
        raise RuntimeError(f"command path cannot be resolved: {raw}") from exc


def _audit_command_tokens(tokens: Sequence[str], *, directory: Path, anchor_base: Path,
                          label: str, response_depth: int = 0,
                          response_stack: tuple[Path, ...] = (),
                          forwarding_depth: int = 0) -> None:
    """Reject command path operands that canonically enter the anchor dependency base."""
    if response_depth > _LINK_RESPONSE_DEPTH:
        raise RuntimeError(f"{label} response-file expansion depth exceeded")
    if forwarding_depth > _LINK_RESPONSE_DEPTH:
        raise RuntimeError(f"{label} forwarded-option expansion depth exceeded")
    directory = directory.resolve(strict=False)
    direct_tokens: list[str] = []
    forwarded: dict[str, list[str]] = {}
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token in _FORWARDED_OPTION_MARKERS:
            if index + 1 >= len(tokens):
                raise RuntimeError(f"{label} contains a forwarding option without an operand")
            forwarded.setdefault(token, []).append(tokens[index + 1])
            index += 2
            continue
        comma_prefix = next((prefix for prefix in _COMMA_FORWARDED_PREFIXES
                             if token.startswith(prefix)), None)
        if comma_prefix is not None:
            components = token[len(comma_prefix):].split(",")
            if not components or any(not component for component in components):
                raise RuntimeError(f"{label} contains an empty forwarded option")
            forwarded.setdefault(comma_prefix, []).extend(components)
        else:
            direct_tokens.append(token)
        index += 1

    pending_path = False
    for token in direct_tokens:
        operands: list[tuple[str, bool]] = []
        if pending_path:
            operands.append((token, False))
            pending_path = False
        elif token in _PATH_ARGUMENT_OPTIONS:
            pending_path = True
            continue
        elif token.startswith("@") and token != "@":
            operands.append((token[1:], True))
        else:
            matched = False
            for prefix in _PATH_ARGUMENT_JOINED_FORMS:
                if token.startswith(prefix) and len(token) > len(prefix):
                    operands.append((token[len(prefix):], False))
                    matched = True
                    break
            if not matched:
                if not token.startswith("-"):
                    operands.append((token, False))
                elif "=" in token:
                    assigned = token.split("=", 1)[1]
                    if assigned and (Path(assigned).is_absolute() or "/" in assigned):
                        operands.append((assigned, False))
        for raw, response in operands:
            if not raw:
                raise RuntimeError(f"{label} contains an empty path operand")
            for path_value in raw.split(os.pathsep) if not response else (raw,):
                canonical = _canonical_argument_path(path_value, directory)
                if _within(canonical, anchor_base):
                    raise RuntimeError(
                        f"{label} references anchor dependency base: {path_value}")
                if not response:
                    continue
                if canonical in response_stack:
                    raise RuntimeError(f"{label} response-file cycle detected")
                try:
                    data, _identity = _read_immutable_file(
                        canonical, f"{label} response file", max_size=16 * 1024 * 1024)
                    response_tokens = shlex.split(data.decode("utf-8"), posix=True)
                except (OSError, UnicodeError, ValueError) as exc:
                    raise RuntimeError(f"{label} response file cannot be audited: {path_value}") from exc
                _audit_command_tokens(
                    response_tokens, directory=directory, anchor_base=anchor_base, label=label,
                    response_depth=response_depth + 1,
                    response_stack=response_stack + (canonical,),
                    forwarding_depth=forwarding_depth)
    if pending_path:
        raise RuntimeError(f"{label} contains a path option without an operand")
    for forwarded_tokens in forwarded.values():
        _audit_command_tokens(
            forwarded_tokens, directory=directory, anchor_base=anchor_base,
            label=label, response_depth=response_depth,
            response_stack=response_stack, forwarding_depth=forwarding_depth + 1)


def _audit_replayed_controls(controls: Mapping[str, str], *, directory: Path,
                             anchor_base: Path) -> None:
    for name, value in sorted(controls.items()):
        try:
            tokens = [component for token in shlex.split(value, posix=True)
                      for component in token.split(";") if component]
        except ValueError as exc:
            raise RuntimeError(f"replayed control {name} has invalid command quoting") from exc
        _audit_command_tokens(tokens, directory=directory, anchor_base=anchor_base,
                              label=f"replayed control {name}")


def parse_compile_inventory(value: str | Path, source_root: str | Path | None = None,
                            build_root: str | Path | None = None,
                            anchor_dependency_base: str | Path | None = None,
                            dependency_source_roots: Mapping[str, str | Path] | None = None
                            ) -> dict[str, Any]:
    text = value.read_text(encoding="utf-8") if isinstance(value, Path) else value
    records = json.loads(text)
    if not isinstance(records, list) or not records:
        raise RuntimeError("compile_commands.json must contain a non-empty array")
    strict_roots = source_root is not None or build_root is not None
    if strict_roots and (source_root is None or build_root is None):
        raise RuntimeError("strict compile inventory validation requires source and build roots")
    source = Path(source_root).resolve(strict=True) if source_root is not None else None
    build = Path(build_root).resolve(strict=True) if build_root is not None else None
    anchor_base = (Path(anchor_dependency_base).resolve(strict=True)
                   if anchor_dependency_base is not None else None)
    raw_dependency_roots = dependency_source_roots or {}
    if (not isinstance(raw_dependency_roots, Mapping)
            or any(not isinstance(name, str)
                   or not re.fullmatch(r"[A-Za-z0-9_+.-]+", name)
                   for name in raw_dependency_roots)):
        raise RuntimeError("compile dependency source name is invalid")
    dependency_roots = {
        name: Path(raw_dependency_roots[name]).resolve(strict=True)
        for name in sorted(raw_dependency_roots)}
    if source_root is not None and Path(source_root) != source:
        raise RuntimeError("source root is not a canonical real path")
    if build_root is not None and Path(build_root) != build:
        raise RuntimeError("build root is not a canonical real path")
    inventory: list[dict[str, Any]] = []
    required_counts = {required: 0 for required in _REQUIRED_SOURCES}
    required_registered_counts = {required: 0 for required in _REQUIRED_SOURCES}
    registered_inventory: list[dict[str, Any]] = []
    other_inventory: list[dict[str, Any]] = []
    for record in records:
        if not isinstance(record, dict) or not isinstance(record.get("file"), str):
            raise RuntimeError("compile_commands.json contains an invalid record")
        command, arguments = record.get("command"), record.get("arguments")
        if isinstance(arguments, list) and all(isinstance(item, str) for item in arguments):
            tokens = arguments
        elif isinstance(command, str):
            try:
                tokens = shlex.split(command)
            except ValueError as exc:
                raise RuntimeError(f"invalid compile command quoting: {exc}") from exc
        else:
            raise RuntimeError("compile command has neither arguments nor command")
        if anchor_base is not None:
            _audit_command_tokens(tokens, directory=Path(record.get("directory", ".")),
                                  anchor_base=anchor_base,
                                  label="fresh compile command")
        if any(token == "-DIBMW_ENABLE_COMM_TRACE" or
               token.startswith("-DIBMW_ENABLE_COMM_TRACE=") for token in tokens):
            raise RuntimeError("trace enable token remains in compile commands")
        raw_directory = Path(record.get("directory", "."))
        directory = raw_directory.resolve(strict=strict_roots)
        if strict_roots and raw_directory != directory:
            raise RuntimeError("compile command directory uses a symlink or '..' alias")
        raw_source_path = Path(record["file"])
        if strict_roots and ".." in raw_source_path.parts:
            raise RuntimeError("compile source uses a '..' alias")
        source_path = raw_source_path
        if not source_path.is_absolute():
            source_path = directory / source_path
        if strict_roots:
            lexical_source_path = Path(os.path.abspath(source_path))
            source_path = source_path.resolve(strict=False)
            if lexical_source_path != source_path:
                raise RuntimeError("compile source uses a symlink alias")
            if not source_path.exists():
                assert build is not None
                try:
                    source_path.relative_to(build)
                except ValueError as exc:
                    raise RuntimeError(
                        f"non-generated compile source is unavailable: {source_path}") from exc
        else:
            source_path = source_path.resolve()
        source_kind = "external"
        if source is not None:
            try:
                source_name = source_path.relative_to(source).as_posix()
                source_kind = "source"
            except ValueError:
                try:
                    assert build is not None
                    source_name = "build/" + source_path.relative_to(build).as_posix()
                    source_kind = "generated"
                except ValueError:
                    matches = []
                    for name, root in dependency_roots.items():
                        try:
                            relative = source_path.relative_to(root)
                        except ValueError:
                            continue
                        matches.append((name, relative))
                    if len(matches) > 1:
                        raise RuntimeError("compile source matches multiple dependency roots")
                    if matches:
                        name, relative = matches[0]
                        if not relative.parts:
                            raise RuntimeError("compile source is a dependency directory")
                        source_name = f"dependency:{name}/{relative.as_posix()}"
                    elif dependency_roots:
                        raise RuntimeError(
                            f"external compile source is outside registered dependency roots: {source_path}")
                    else:
                        source_name = str(source_path)
        elif source_path.name in {"dds_transport_driver.cc", "dds_managed_exact_candidate.cc"}:
            source_name = source_path.name
        else:
            source_name = str(source_path)
        if source_name in _FORBIDDEN_SOURCES or any(
                str(source_path).endswith("/" + suffix) for suffix in _FORBIDDEN_SOURCES):
            raise RuntimeError(f"forbidden trace source remains in compile commands: {source_name}")
        source_requirements = [(required, target_dir) for required, target_dir in _REQUIRED_SOURCES.items()
                               if source_name == required or
                               (source is None and source_name == Path(required).name)]
        output_tokens = []
        for index, token in enumerate(tokens):
            if token == "-o":
                if index + 1 >= len(tokens) or not tokens[index + 1]:
                    raise RuntimeError("compile command has an ambiguous -o output")
                output_tokens.append(tokens[index + 1])
            elif token.startswith("-o") and token != "-o":
                output_tokens.append(token[2:])
        if len(output_tokens) > 1:
            raise RuntimeError("compile command has multiple -o outputs")
        token_output = output_tokens[0] if output_tokens else ""
        record_output = record.get("output")
        if record_output is not None and not isinstance(record_output, str):
            raise RuntimeError("compile record output must be a string")
        output = token_output or (record_output or "")
        if strict_roots and not output:
            raise RuntimeError("compile command has no structurally parsed object output")
        raw_output = Path(output)
        if strict_roots and raw_output.suffix not in {".o", ".obj"}:
            raise RuntimeError(f"compile output is not an object file: {output}")
        output_path = raw_output if raw_output.is_absolute() else directory / raw_output
        if output:
            if strict_roots and ".." in raw_output.parts:
                raise RuntimeError("compile output uses a '..' alias")
            lexical_output = Path(os.path.abspath(output_path))
            canonical_output = output_path.resolve(strict=False)
            if strict_roots and lexical_output != canonical_output:
                raise RuntimeError(f"compile output uses a symlink alias: {output}")
            matched_requirements = [
                (required, target_dir) for required, target_dir in source_requirements
                if any(lexical_output.parts[index:index + 2] == ("CMakeFiles", target_dir)
                       for index in range(len(lexical_output.parts) - 1))]
            output_path = canonical_output
        else:
            matched_requirements = []
            output_path = None
        target = None
        if build is not None and output_path is not None:
            try:
                relative_output = output_path.relative_to(build)
            except ValueError as exc:
                raise RuntimeError(f"compile output is outside fresh build root: {output_path}") from exc
            for index in range(len(relative_output.parts) - 1):
                component = relative_output.parts[index:index + 2]
                if component[0] == "CMakeFiles" and component[1].endswith(".dir"):
                    target = component[1][:-4]
                    break
            if target is None:
                raise RuntimeError(f"compile output has no CMake target directory: {output_path}")
        if build is None:
            matched_requirements = source_requirements
            for required, _target_dir in matched_requirements:
                required_counts[required] += 1
        for required, target_dir in matched_requirements:
            if build is not None and output_path is not None:
                try:
                    relative_output = output_path.relative_to(build)
                except ValueError:
                    raise RuntimeError(f"required compile output is outside fresh build root: {output_path}")
                expected_parts = ("CMakeFiles", target_dir)
                if not any(relative_output.parts[index:index + 2] == expected_parts
                           for index in range(len(relative_output.parts) - 1)):
                    raise RuntimeError(
                        f"required compile output is outside expected CMake target directory: {output_path}")
                if target == target_dir[:-4]:
                    required_registered_counts[required] += 1
        if strict_roots and output_path is not None:
            try:
                assert build is not None
                stored_output = output_path.relative_to(build).as_posix()
            except ValueError as exc:
                raise RuntimeError(f"compile output is outside fresh build root: {output_path}") from exc
        elif build is not None and output_path is not None:
            stored_output = output_path.relative_to(build).as_posix()
        else:
            stored_output = str(output_path) if output_path else ""
        stored_source = source_name
        normalized_tokens = list(tokens)
        if strict_roots:
            logical_roots = [(dependency_root, f"dependency:{name}")
                             for name, dependency_root in dependency_roots.items()]
            assert source is not None and build is not None
            logical_roots.extend(((source, "source"), (build, "build")))
            for token_index, command_token in enumerate(normalized_tokens):
                for root, logical in sorted(logical_roots,
                                            key=lambda item: len(str(item[0])), reverse=True):
                    root_text = str(root)
                    command_token = command_token.replace(
                        root_text + os.sep, logical + "/")
                    if command_token.endswith(root_text):
                        command_token = command_token[:-len(root_text)] + logical
                normalized_tokens[token_index] = command_token
        item = {"source": stored_source, "source_kind": source_kind,
                "output": stored_output, "target": target,
                "command_sha256": hashlib.sha256(
                    "\0".join(normalized_tokens).encode()).hexdigest()}
        inventory.append(item)
        (registered_inventory if target in _REGISTERED_TARGETS else other_inventory).append(item)
    for required, expected_target_dir in _REQUIRED_SOURCES.items():
        expected_target = expected_target_dir[:-4]
        occurrences = [item for item in registered_inventory
                        if item["source"] == required and item["target"] == expected_target]
        wrong_registered = [item for item in registered_inventory
                            if item["source"] == required and item["target"] != expected_target]
        if wrong_registered:
            raise RuntimeError(f"required rollback source appears in the other registered target: {required}")
        required_registered_counts[required] = len(occurrences)
    if build is None:
        duplicate_required = sorted(required for required, count in required_counts.items() if count > 1)
        missing_required = sorted(required for required, count in required_counts.items() if count == 0)
    else:
        duplicate_required = sorted(required for required, count in required_registered_counts.items() if count > 1)
        missing_required = sorted(required for required, count in required_registered_counts.items() if count == 0)
    if duplicate_required:
        raise RuntimeError(f"compile commands duplicate required rollback sources: {duplicate_required}")
    if missing_required:
        raise RuntimeError(f"compile commands omit required rollback sources: {missing_required}")
    inventory = sorted(inventory, key=lambda x: (x["source"], x["output"], x["target"] or ""))
    registered_inventory = sorted(registered_inventory, key=lambda x: (x["source"], x["output"], x["target"] or ""))
    other_inventory = sorted(other_inventory, key=lambda x: (x["source"], x["output"], x["target"] or ""))
    def subset(value: list[dict[str, Any]]) -> dict[str, Any]:
        encoded = json.dumps(value, sort_keys=True, separators=(",", ":"))
        return {"record_count": len(value), "content_sha256": hashlib.sha256(encoded.encode()).hexdigest()}
    return {"record_count": len(inventory), "records": inventory,
            "sources": sorted({item["source"] for item in inventory}),
            "content_sha256": hashlib.sha256(
                core._canonical_json_bytes(inventory)).hexdigest(),
            "registered_records": subset(registered_inventory),
            "other_records": subset(other_inventory),
            "claim": "Registered-target source validation excludes same-source records in other targets; trace-source and trace-define exclusion applies to the whole compile configuration."}


def parse_nm_symbols(output: str) -> list[str]:
    symbols: list[str] = []
    pattern = re.compile(
        r"^\s*(?:(?:\S+\.(?:a|o)(?:\([^)]*\))?):)?\s*"
        r"(?:[0-9a-fA-F]+\s+)?([A-Za-z?])\s+(.+?)\s*$"
    )
    for line in output.splitlines():
        match = pattern.match(line)
        if match:
            symbols.append(match.group(2))
    return symbols


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _file_identity(path: Path) -> dict[str, Any]:
    canonical = path.resolve(strict=True)
    info = canonical.stat()
    return {"path": str(canonical), "device": info.st_dev, "inode": info.st_ino,
            "size": info.st_size, "sha256": _sha256(canonical)}


def _cache_home(cache: Mapping[str, Mapping[str, str]], source: Path, label: str) -> None:
    raw = cache.get("CMAKE_HOME_DIRECTORY", {}).get("value", "")
    try: resolved = Path(raw).resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise RuntimeError(f"{label} CMAKE_HOME_DIRECTORY is invalid: {raw}") from exc
    if resolved != source:
        raise RuntimeError(f"{label} CMAKE_HOME_DIRECTORY mismatch: {resolved} != {source}")


def _compare_controls(anchor: Mapping[str, str], fresh: Mapping[str, str], label: str,
                      *, compiler_aliases: bool = False) -> None:
    delta = option_delta(anchor, fresh)
    for name in list(delta):
        if compiler_aliases and name in _COMPILERS and anchor.get(name) and fresh.get(name):
            try:
                anchor_id, fresh_id = _file_identity(Path(anchor[name])), _file_identity(Path(fresh[name]))
                if all(anchor_id[key] == fresh_id[key]
                       for key in ("device", "inode", "size", "sha256")):
                    del delta[name]
            except OSError:
                pass
    if delta:
        raise RuntimeError(f"fresh {label} controls differ from anchor: {delta}")


def _run(argv: Sequence[str], log_base: Path, timeout: float, *, env: Mapping[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    result = subprocess.run(list(argv), text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=timeout, check=False, env=env)
    stdout_path = Path(f"{log_base}.stdout.log")
    stderr_path = Path(f"{log_base}.stderr.log")
    stdout_path.write_text(result.stdout, encoding="utf-8")
    stderr_path.write_text(result.stderr, encoding="utf-8")
    if result.returncode != 0:
        tail = "\n".join(result.stderr.splitlines()[-40:])
        raise RuntimeError(f"command failed ({result.returncode}): {argv[0]}; logs: "
                           f"{stdout_path}, {stderr_path}; stderr tail:\n{tail}")
    return result


def _regular_unique(root: Path, name: str) -> Path:
    declared_root = Path(root)
    try:
        root_info = declared_root.lstat()
        canonical_root = declared_root.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise RuntimeError(f"regular {name} search root is unavailable") from exc
    if (stat.S_ISLNK(root_info.st_mode) or not stat.S_ISDIR(root_info.st_mode)
            or canonical_root != declared_root):
        raise RuntimeError(f"regular {name} search root is not a real directory")

    matches: list[Path] = []
    flags = os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_CLOEXEC", 0)
    nofollow = getattr(os, "O_NOFOLLOW", 0)

    # Keep every directory component open while walking.  Path/rglob and
    # os.walk both have a check/use window in which a replaced parent can
    # redirect discovery outside the freshly validated root.
    def walk(directory_fd: int, relative: tuple[str, ...]) -> None:
        try:
            with os.scandir(directory_fd) as entries:
                children = list(entries)
        except OSError as exc:
            raise RuntimeError(f"regular {name} search is unavailable") from exc
        for entry in children:
            try:
                info = os.stat(entry.name, dir_fd=directory_fd, follow_symlinks=False)
            except OSError as exc:
                raise RuntimeError(f"regular {name} match is unavailable or contains a symlink") from exc
            child_relative = relative + (entry.name,)
            if stat.S_ISLNK(info.st_mode):
                raise RuntimeError(f"regular {name} search contains a symlink component")
            if stat.S_ISDIR(info.st_mode):
                try:
                    child_fd = os.open(entry.name, flags | nofollow, dir_fd=directory_fd)
                except OSError as exc:
                    raise RuntimeError(f"regular {name} search contains a symlink component") from exc
                try:
                    walk(child_fd, child_relative)
                finally:
                    os.close(child_fd)
            elif entry.name == name and stat.S_ISREG(info.st_mode):
                try:
                    file_fd = os.open(entry.name, os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | nofollow,
                                      dir_fd=directory_fd)
                    try:
                        opened = os.fstat(file_fd)
                    finally:
                        os.close(file_fd)
                except OSError as exc:
                    raise RuntimeError(f"regular {name} match is unavailable or contains a symlink") from exc
                if not stat.S_ISREG(opened.st_mode):
                    raise RuntimeError(f"regular {name} match is not a regular file")
                matches.append(canonical_root.joinpath(*child_relative))

    try:
        root_fd = os.open(canonical_root, flags | nofollow)
    except OSError as exc:
        raise RuntimeError(f"regular {name} search root is unavailable") from exc
    try:
        walk(root_fd, ())
    finally:
        os.close(root_fd)
    if len(matches) != 1:
        raise RuntimeError(f"expected one regular {name}, found {len(matches)}")
    return matches[0]


def _tool_identity(command: str) -> dict[str, Any]:
    executable = command.split(";", 1)[0]
    path = Path(shutil.which(executable) or executable).resolve(strict=True)
    try:
        result = subprocess.run([str(path), "--version"], text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=10, check=False)
        version_lines = result.stdout.splitlines()
        version = version_lines[0] if version_lines else "unavailable"
    except (OSError, subprocess.SubprocessError):
        version = "unavailable"
    info = path.stat()
    return {"path": str(path), "device": info.st_dev, "inode": info.st_ino,
            "size": info.st_size, "sha256": _sha256(path), "version": version}


def _active_tool_identities(options: Mapping[str, str]) -> dict[str, dict[str, Any]]:
    return {name: _tool_identity(value) for name, value in sorted(options.items())
            if name in _EXECUTABLE_TOOLCHAIN_CONTROLS and value}


def _require_same_tool_identity(anchor: Mapping[str, Any], fresh: Mapping[str, Any],
                                label: str) -> None:
    keys = ("path", "device", "inode", "size", "sha256")
    if any(anchor.get(key) != fresh.get(key) for key in keys):
        raise RuntimeError(f"{label} tool identity differs from anchor")


def _require_same_source_identity(before: Mapping[str, Any], after: Mapping[str, Any]) -> None:
    if after != before:
        raise RuntimeError("source identity changed during rollback evaluation")


def _open_directory_chain(path: Path) -> tuple[int, Path]:
    """Open an absolute directory one no-follow component at a time."""
    declared = Path(path)
    if not declared.is_absolute() or declared != Path(os.path.abspath(declared)):
        raise RuntimeError("external dependency source is not a canonical absolute path")
    flags = os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_CLOEXEC", 0)
    fd = os.open("/", flags)
    try:
        for component in declared.parts[1:]:
            next_fd = os.open(component, flags | getattr(os, "O_NOFOLLOW", 0), dir_fd=fd)
            os.close(fd)
            fd = next_fd
        return fd, declared
    except BaseException:
        os.close(fd)
        raise


def _stable_stat(info: os.stat_result) -> tuple[int, ...]:
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size,
            info.st_mtime_ns, info.st_ctime_ns)


def _dependency_tree(root: Path, destination: Path | None = None, *,
                     max_entries: int = _MAX_DEPENDENCY_ENTRIES,
                     max_files: int = _MAX_DEPENDENCY_FILES,
                     max_directories: int = _MAX_DEPENDENCY_DIRECTORIES,
                     max_bytes: int = _MAX_DEPENDENCY_BYTES) -> dict[str, Any]:
    """Hash, and optionally snapshot, a tree through pinned directory descriptors."""
    try:
        root_fd, canonical = _open_directory_chain(root)
    except OSError as exc:
        raise RuntimeError(f"external dependency source is unavailable or contains a symlink: {root}") from exc
    records: list[dict[str, Any]] = []
    total_bytes = 0
    file_count = 0
    directory_count = 0
    destination_fd: int | None = None
    try:
        root_before = os.fstat(root_fd)
        directory_count += 1
        if max_directories < directory_count:
            raise RuntimeError("external dependency source directory limit exceeded")
        if destination is not None:
            destination.mkdir(mode=stat.S_IMODE(root_before.st_mode))
            destination_fd, _ = _open_directory_chain(destination)
            os.fchmod(destination_fd, stat.S_IMODE(root_before.st_mode))
        records.append({"name": ".", "kind": "directory",
                        "mode": stat.S_IMODE(root_before.st_mode)})

        def walk(source_fd: int, target_fd: int | None, prefix: str, depth: int) -> None:
            nonlocal total_bytes, file_count, directory_count
            if depth > _MAX_DEPENDENCY_DEPTH:
                raise RuntimeError("external dependency source depth limit exceeded")
            try:
                names = sorted(os.listdir(source_fd))
            except OSError as exc:
                raise RuntimeError("external dependency source changed while traversing") from exc
            for name in names:
                if not name or name in {".", ".."} or "/" in name:
                    raise RuntimeError("external dependency source contains an invalid entry name")
                if len(records) >= max_entries:
                    raise RuntimeError("external dependency source entry limit exceeded")
                relative = f"{prefix}/{name}" if prefix else name
                before = os.stat(name, dir_fd=source_fd, follow_symlinks=False)
                mode = stat.S_IMODE(before.st_mode)
                if stat.S_ISLNK(before.st_mode):
                    raise RuntimeError(f"external dependency source contains a symlink: {relative}")
                if stat.S_ISDIR(before.st_mode):
                    if directory_count >= max_directories:
                        raise RuntimeError("external dependency source directory limit exceeded")
                    child_flags = (os.O_RDONLY | os.O_DIRECTORY | getattr(os, "O_CLOEXEC", 0)
                                   | getattr(os, "O_NOFOLLOW", 0))
                    try:
                        child_fd = os.open(name, child_flags, dir_fd=source_fd)
                    except OSError as exc:
                        raise RuntimeError(
                            f"external dependency source changed or contains a symlink: {relative}") from exc
                    target_child = None
                    try:
                        if _stable_stat(os.fstat(child_fd)) != _stable_stat(before):
                            raise RuntimeError(f"external dependency source changed while opening: {relative}")
                        if target_fd is not None:
                            os.mkdir(name, mode, dir_fd=target_fd)
                            target_child = os.open(name, child_flags, dir_fd=target_fd)
                            os.fchmod(target_child, mode)
                        records.append({"name": relative, "kind": "directory", "mode": mode})
                        directory_count += 1
                        walk(child_fd, target_child, relative, depth + 1)
                        linked = os.stat(name, dir_fd=source_fd, follow_symlinks=False)
                        if _stable_stat(os.fstat(child_fd)) != _stable_stat(linked):
                            raise RuntimeError(f"external dependency source changed while copying: {relative}")
                    finally:
                        if target_child is not None:
                            os.close(target_child)
                        os.close(child_fd)
                    continue
                if not stat.S_ISREG(before.st_mode):
                    raise RuntimeError(f"external dependency source contains a special file: {relative}")
                if before.st_size > _MAX_DEPENDENCY_FILE_SIZE:
                    raise RuntimeError(f"external dependency source file size limit exceeded: {relative}")
                if file_count >= max_files:
                    raise RuntimeError("external dependency source file count limit exceeded")
                if total_bytes + before.st_size > max_bytes:
                    raise RuntimeError("external dependency source byte limit exceeded")
                try:
                    source_file = os.open(name, os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
                                          | getattr(os, "O_NOFOLLOW", 0), dir_fd=source_fd)
                except OSError as exc:
                    raise RuntimeError(
                        f"external dependency source changed or contains a symlink: {relative}") from exc
                target_file = None
                digest = hashlib.sha256()
                size = 0
                try:
                    if _stable_stat(os.fstat(source_file)) != _stable_stat(before):
                        raise RuntimeError(f"external dependency source changed while opening: {relative}")
                    if target_fd is not None:
                        target_file = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL
                                              | getattr(os, "O_CLOEXEC", 0)
                                              | getattr(os, "O_NOFOLLOW", 0), mode,
                                              dir_fd=target_fd)
                        os.fchmod(target_file, mode)
                    while True:
                        remaining = min(before.st_size - size, max_bytes - total_bytes - size)
                        chunk = os.read(source_file, min(1024 * 1024, max(0, remaining) + 1))
                        if not chunk:
                            break
                        if len(chunk) > remaining:
                            raise RuntimeError(
                                f"external dependency source grew beyond declared byte budget: {relative}")
                        size += len(chunk); digest.update(chunk)
                        if target_file is not None:
                            view = memoryview(chunk)
                            while view:
                                written = os.write(target_file, view)
                                if written <= 0:
                                    raise OSError("dependency snapshot write made no progress")
                                view = view[written:]
                    linked = os.stat(name, dir_fd=source_fd, follow_symlinks=False)
                    if (_stable_stat(os.fstat(source_file)) != _stable_stat(before)
                            or _stable_stat(linked) != _stable_stat(before)):
                        raise RuntimeError(f"external dependency source changed while copying: {relative}")
                finally:
                    if target_file is not None:
                        os.close(target_file)
                    os.close(source_file)
                total_bytes += size
                if total_bytes > max_bytes:
                    raise RuntimeError("external dependency source byte limit exceeded")
                records.append({"name": relative, "kind": "file", "mode": mode,
                                "size": size, "sha256": digest.hexdigest()})
                file_count += 1

        walk(root_fd, destination_fd, "", 1)
        if _stable_stat(os.fstat(root_fd)) != _stable_stat(root_before):
            raise RuntimeError("external dependency source root changed while traversing")
    finally:
        if destination_fd is not None:
            os.close(destination_fd)
        os.close(root_fd)
    if file_count < 1:
        raise RuntimeError("external dependency source contains no regular files")
    return {"path": str(canonical),
            "file_count": file_count,
            "directory_count": directory_count,
            "byte_count": total_bytes,
            "tree_sha256": hashlib.sha256(core._canonical_json_bytes(records)).hexdigest()}


def _external_source_identity(root: Path) -> dict[str, Any]:
    return _dependency_tree(root)


def _canonical_cache_directory(cache: Mapping[str, Mapping[str, str]], name: str,
                               label: str) -> Path:
    raw = cache.get(name, {}).get("value", "")
    if not raw:
        raise RuntimeError(f"{label} {name} is missing or invalid")
    declared = Path(raw)
    try:
        info = declared.lstat()
        canonical = declared.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise RuntimeError(f"{label} {name} is missing or invalid") from exc
    if (not raw or not declared.is_absolute() or stat.S_ISLNK(info.st_mode)
            or not stat.S_ISDIR(info.st_mode) or canonical != declared):
        raise RuntimeError(f"{label} {name} is not a canonical real directory")
    return canonical


def _dependency_override(name: str) -> str:
    normalized = name.upper()
    if not re.fullmatch(r"[A-Z0-9_.+-]+", normalized):
        raise RuntimeError("FetchContent dependency name cannot be normalized safely")
    return "FETCHCONTENT_SOURCE_DIR_" + normalized


def _valid_dependency_relative(value: Any) -> bool:
    if not isinstance(value, str) or not value or "\\" in value or value.startswith("/"):
        return False
    if value == ".":
        return True
    parts = value.split("/")
    return all(part not in {"", ".", ".."}
               and re.fullmatch(r"[A-Za-z0-9_+.-]+", part) is not None
               for part in parts)


def _require_remaining_dependency_budget(files: int, directories: int,
                                         entries: int, byte_count: int) -> None:
    if files >= _MAX_DEPENDENCY_FILES:
        raise RuntimeError("external dependency aggregate file count limit exceeded")
    if directories >= _MAX_DEPENDENCY_DIRECTORIES:
        raise RuntimeError("external dependency aggregate directory count limit exceeded")
    if entries >= _MAX_DEPENDENCY_ENTRIES:
        raise RuntimeError("external dependency aggregate entry limit exceeded")
    if byte_count >= _MAX_DEPENDENCY_TOTAL_BYTES:
        raise RuntimeError("external dependency aggregate byte limit exceeded")


def _anchor_dependency_sources(cache: Mapping[str, Mapping[str, str]]) -> tuple[Path, list[dict[str, Any]]]:
    base = _canonical_cache_directory(cache, "FETCHCONTENT_BASE_DIR", "anchor")
    candidates = sorted((item for item in base.iterdir() if item.name.endswith("-src")),
                        key=lambda item: item.name)
    if not candidates:
        raise RuntimeError("anchor FETCHCONTENT_BASE_DIR contains no dependency source trees")
    if len(candidates) > _MAX_DEPENDENCY_COUNT:
        raise RuntimeError("anchor dependency source count limit exceeded")
    dependencies: list[dict[str, Any]] = []
    overrides: set[str] = set()
    aggregate_files = aggregate_directories = aggregate_entries = aggregate_bytes = 0
    for path in candidates:
        _require_remaining_dependency_budget(
            aggregate_files, aggregate_directories, aggregate_entries, aggregate_bytes)
        name = path.name[:-4]
        override = _dependency_override(name)
        if override in overrides:
            raise RuntimeError("FetchContent dependency override collision")
        overrides.add(override)
        bindings = []
        for variable, item in sorted(cache.items()):
            raw = item.get("value", "")
            if not variable.endswith(("_SOURCE_DIR", "_LOCAL_SOURCE")) or not raw:
                continue
            candidate = Path(raw)
            try:
                resolved = candidate.resolve(strict=True)
                relative = resolved.relative_to(path)
            except (OSError, ValueError):
                continue
            if candidate != resolved:
                raise RuntimeError(f"anchor {variable} is not a canonical source binding")
            bindings.append({"variable": variable, "relative": relative.as_posix()})
        identity = _dependency_tree(
            path,
            max_entries=_MAX_DEPENDENCY_ENTRIES - aggregate_entries,
            max_files=_MAX_DEPENDENCY_FILES - aggregate_files,
            max_directories=_MAX_DEPENDENCY_DIRECTORIES - aggregate_directories,
            max_bytes=_MAX_DEPENDENCY_TOTAL_BYTES - aggregate_bytes)
        aggregate_files += identity["file_count"]
        aggregate_directories += identity["directory_count"]
        aggregate_entries += identity["file_count"] + identity["directory_count"]
        aggregate_bytes += identity["byte_count"]
        dependencies.append({"name": name, "override_variable": override,
                             "cache_bindings": bindings,
                             "anchor_source": str(path),
                             "identity": {"anchor_before_staging": identity}})
    return base, dependencies


def _aggregate_dependency_identity(dependencies: Sequence[Mapping[str, Any]], checkpoint: str
                                   ) -> dict[str, Any]:
    records = []
    for dependency in sorted(dependencies, key=lambda item: str(item["name"])):
        identity = dependency["identity"][checkpoint]
        records.append({"name": dependency["name"],
                        "override_variable": dependency["override_variable"],
                        "cache_bindings": dependency["cache_bindings"],
                        "file_count": identity["file_count"],
                        "directory_count": identity["directory_count"],
                        "byte_count": identity["byte_count"],
                        "tree_sha256": identity["tree_sha256"]})
    aggregate = {"dependency_count": len(records),
                 "file_count": sum(item["file_count"] for item in records),
                 "directory_count": sum(item["directory_count"] for item in records),
                 "byte_count": sum(item["byte_count"] for item in records),
                 "aggregate_sha256": hashlib.sha256(core._canonical_json_bytes(records)).hexdigest()}
    _require_aggregate_dependency_limits(aggregate)
    return aggregate


def _require_aggregate_dependency_limits(identity: Mapping[str, Any]) -> None:
    if identity["file_count"] > _MAX_DEPENDENCY_FILES:
        raise RuntimeError("external dependency aggregate file count limit exceeded")
    if identity["directory_count"] > _MAX_DEPENDENCY_DIRECTORIES:
        raise RuntimeError("external dependency aggregate directory count limit exceeded")
    if identity["file_count"] + identity["directory_count"] > _MAX_DEPENDENCY_ENTRIES:
        raise RuntimeError("external dependency aggregate entry limit exceeded")
    if identity["byte_count"] > _MAX_DEPENDENCY_TOTAL_BYTES:
        raise RuntimeError("external dependency aggregate byte limit exceeded")


def _dependency_content_identity(identity: Mapping[str, Any]) -> dict[str, Any]:
    return {name: identity[name] for name in (
        "file_count", "directory_count", "byte_count", "tree_sha256")}


def _require_dependency_checkpoint(dependencies: Sequence[Mapping[str, Any]], checkpoint: str,
                                   baseline: str = "anchor_before_staging") -> None:
    for dependency in dependencies:
        if (_dependency_content_identity(dependency["identity"][checkpoint]) !=
                _dependency_content_identity(dependency["identity"][baseline])):
            raise RuntimeError(
                f"dependency source identity changed at {checkpoint}: {dependency['name']}")


def _record_dependency_checkpoint(dependencies: list[dict[str, Any]], checkpoint: str,
                                   source_field: str) -> dict[str, Any]:
    aggregate_files = aggregate_directories = aggregate_entries = aggregate_bytes = 0
    for dependency in dependencies:
        _require_remaining_dependency_budget(
            aggregate_files, aggregate_directories, aggregate_entries, aggregate_bytes)
        identity = _dependency_tree(
            Path(dependency[source_field]),
            max_entries=_MAX_DEPENDENCY_ENTRIES - aggregate_entries,
            max_files=_MAX_DEPENDENCY_FILES - aggregate_files,
            max_directories=_MAX_DEPENDENCY_DIRECTORIES - aggregate_directories,
            max_bytes=_MAX_DEPENDENCY_TOTAL_BYTES - aggregate_bytes)
        dependency["identity"][checkpoint] = identity
        aggregate_files += identity["file_count"]
        aggregate_directories += identity["directory_count"]
        aggregate_entries += identity["file_count"] + identity["directory_count"]
        aggregate_bytes += identity["byte_count"]
    return _aggregate_dependency_identity(dependencies, checkpoint)


def _stage_dependency_sources(work: Path, dependencies: list[dict[str, Any]]) -> tuple[Path, Path]:
    source_cache = work / "source-cache"
    fresh_base = work / "build" / "_deps"
    source_cache.mkdir(mode=0o700)
    fresh_base.mkdir(mode=0o700, parents=True)
    aggregate_files = aggregate_directories = aggregate_entries = aggregate_bytes = 0
    for dependency in dependencies:
        _require_remaining_dependency_budget(
            aggregate_files, aggregate_directories, aggregate_entries, aggregate_bytes)
        destination = (source_cache / (dependency["name"] + "-src")).resolve(strict=False)
        copied = _dependency_tree(
            Path(dependency["anchor_source"]), destination,
            max_entries=_MAX_DEPENDENCY_ENTRIES - aggregate_entries,
            max_files=_MAX_DEPENDENCY_FILES - aggregate_files,
            max_directories=_MAX_DEPENDENCY_DIRECTORIES - aggregate_directories,
            max_bytes=_MAX_DEPENDENCY_TOTAL_BYTES - aggregate_bytes)
        aggregate_files += copied["file_count"]
        aggregate_directories += copied["directory_count"]
        aggregate_entries += copied["file_count"] + copied["directory_count"]
        aggregate_bytes += copied["byte_count"]
        _require_aggregate_dependency_limits({"file_count": aggregate_files,
                                              "directory_count": aggregate_directories,
                                              "byte_count": aggregate_bytes})
        if (_dependency_content_identity(copied) != _dependency_content_identity(
                dependency["identity"]["anchor_before_staging"])):
            raise RuntimeError(f"dependency source changed during private snapshot: {dependency['name']}")
        source_after = _external_source_identity(Path(dependency["anchor_source"]))
        if (_dependency_content_identity(source_after) != _dependency_content_identity(copied)):
            raise RuntimeError(f"dependency source changed after private snapshot: {dependency['name']}")
        dependency["staged_source"] = str(destination.resolve(strict=True))
    return source_cache.resolve(strict=True), fresh_base.resolve(strict=True)


def _require_dependency_cache(cache: Mapping[str, Mapping[str, str]], fresh_base: Path,
                              source_cache: Path,
                              dependencies: Sequence[Mapping[str, Any]],
                              anchor_base: Path | None = None) -> None:
    if _canonical_cache_directory(cache, "FETCHCONTENT_BASE_DIR", "fresh") != fresh_base:
        raise RuntimeError("fresh FETCHCONTENT_BASE_DIR cache mismatch")
    if cache.get("FETCHCONTENT_FULLY_DISCONNECTED", {}).get("value") != "ON":
        raise RuntimeError("fresh FETCHCONTENT_FULLY_DISCONNECTED cache mismatch")
    staged_sources: list[Path] = []
    staged_root = source_cache.resolve(strict=True)
    for dependency in dependencies:
        expected = (staged_root / (str(dependency["name"]) + "-src")).resolve(strict=True)
        if (Path(str(dependency["staged_source"])) != expected
                or expected.parent != staged_root):
            raise RuntimeError(
                f"claimed staged source is not canonical for dependency: {dependency['name']}")
        override = str(dependency["override_variable"])
        raw = cache.get(override, {}).get("value", "")
        try:
            resolved = Path(raw).resolve(strict=True)
        except (OSError, RuntimeError) as exc:
            raise RuntimeError(f"fresh {override} cache mismatch") from exc
        if raw != str(expected) or Path(raw) != resolved or resolved != expected:
            raise RuntimeError(f"fresh {override} cache mismatch")
        staged_sources.append(expected)
        for binding in dependency["cache_bindings"]:
            local_name = str(binding["variable"])
            local_expected = expected / str(binding["relative"])
            local = cache.get(local_name, {}).get("value", "")
            try:
                local_resolved = Path(local).resolve(strict=True)
            except (OSError, RuntimeError) as exc:
                raise RuntimeError(f"fresh {local_name} cache mismatch") from exc
            if local != str(local_expected) or Path(local) != local_resolved:
                raise RuntimeError(f"fresh {local_name} cache mismatch")
    if anchor_base is None:
        anchor_base = Path(str(dependencies[0]["anchor_source"])).parent
    for variable, item in sorted(cache.items()):
        if item.get("type") not in {"PATH", "FILEPATH"}:
            continue
        for raw in item.get("value", "").split(";"):
            if not raw or raw.endswith("-NOTFOUND") or not Path(raw).is_absolute():
                continue
            try:
                candidate = Path(raw)
                if candidate != Path(os.path.abspath(candidate)):
                    raise RuntimeError("noncanonical path")
                resolved_candidate = candidate.resolve(strict=False)
            except (OSError, RuntimeError) as exc:
                raise RuntimeError(f"fresh path-valued cache entry cannot be resolved: {variable}") from exc
            if _within(resolved_candidate, anchor_base):
                raise RuntimeError(f"fresh path-valued cache entry references anchor dependency base: {variable}")
            if (candidate != resolved_candidate
                    and (_within(candidate, staged_root)
                         or _within(resolved_candidate, staged_root))):
                raise RuntimeError(f"fresh path-valued cache entry uses an aliased path: {variable}")
            if (_within(resolved_candidate, staged_root)
                    and not any(_within(resolved_candidate, source)
                                for source in staged_sources)):
                raise RuntimeError(f"fresh path-valued cache entry is not an authorized staged source: {variable}")


def _configure_argv(cmake: str, source: Path, build: Path,
                    anchor_cache: Mapping[str, Mapping[str, str]],
                    anchor_toolchain: Mapping[str, str], anchor_selected: Mapping[str, str],
                    fresh_base: Path, dependencies: Sequence[Mapping[str, Any]]) -> list[str]:
    argv = [cmake, "-S", str(source), "-B", str(build)]
    generator = anchor_cache.get("CMAKE_GENERATOR", {}).get("value", "")
    if generator:
        argv.extend(["-G", generator])
    platform = anchor_cache.get("CMAKE_GENERATOR_PLATFORM", {}).get("value", "")
    toolset = anchor_cache.get("CMAKE_GENERATOR_TOOLSET", {}).get("value", "")
    if platform:
        argv.extend(["-A", platform])
    if toolset:
        argv.extend(["-T", toolset])
    for name, value in {**anchor_toolchain, **anchor_selected}.items():
        if name not in {"CMAKE_GENERATOR_PLATFORM", "CMAKE_GENERATOR_TOOLSET"}:
            argv.append(f"-D{name}={value}")
    argv.extend((f"-DFETCHCONTENT_BASE_DIR={fresh_base}",
                 "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"))
    argv.extend(f"-D{item['override_variable']}={item['staged_source']}"
                for item in sorted(dependencies, key=lambda value: str(value["name"])))
    argv.extend(("-DIBMW_ENABLE_COMM_TRACE=OFF",
                 "-DIBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE=OFF",
                 "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"))
    return argv


def _provider_roots(cache: Mapping[str, Mapping[str, str]]) -> list[Path]:
    candidates: list[Path] = []
    source_prefix = cache.get("IBMW_THIRD_PARTY_SOURCE_PREFIX", {}).get("value", "")
    if source_prefix:
        candidates.append(Path(source_prefix))
    for key in ("fastcdr_DIR", "fastdds_DIR", "fastrtps_DIR"):
        raw = cache.get(key, {}).get("value", "")
        if not raw or raw.endswith("-NOTFOUND"):
            continue
        path = Path(raw).resolve()
        for parent in (path, *path.parents):
            if (parent / "lib").is_dir() or (parent / "lib64").is_dir():
                candidates.append(parent)
                break
    paths: list[Path] = []
    for prefix in candidates:
        for library in (prefix / "lib", prefix / "lib64"):
            if library.is_dir() and library.resolve() not in paths:
                paths.append(library.resolve())
    return paths


def _elf_identity(path: Path, readelf: str) -> dict[str, Any]:
    canonical = path.resolve(strict=True)
    info = canonical.stat()
    notes = subprocess.run([readelf, "-n", str(canonical)], text=True, stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=30, check=False)
    dynamic = subprocess.run([readelf, "-d", str(canonical)], text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, timeout=30, check=False)
    build_id = re.search(r"Build ID:\s*([0-9a-fA-F]+)", notes.stdout)
    soname = re.search(r"\(SONAME\).*?\[([^]]+)\]", dynamic.stdout)
    return {"path": str(canonical), "device": info.st_dev, "inode": info.st_ino,
            "sha256": _sha256(canonical), "size": info.st_size,
            "build_id": build_id.group(1).lower() if build_id else None,
            "version": soname.group(1) if soname else canonical.name}


def _provider_identity(cache: Mapping[str, Mapping[str, str]], readelf: str) -> dict[str, Any]:
    roots = _provider_roots(cache)
    discovered: dict[str, list[Path]] = {"dds": [], "cdr": []}
    for root in roots:
        for family, patterns in (("dds", ("libfastdds.so*", "libfastrtps.so*")),
                                 ("cdr", ("libfastcdr.so*",))):
            for pattern in patterns:
                for path in root.glob(pattern):
                    if path.is_file():
                        canonical = path.resolve(strict=True)
                        if canonical not in discovered[family]:
                            discovered[family].append(canonical)
    for family, paths in discovered.items():
        if len(paths) != 1:
            raise RuntimeError(f"expected exactly one fresh-derived {family} provider, found {len(paths)}: {paths}")
    identities: dict[str, Any] = {
        family: _elf_identity(paths[0], readelf) for family, paths in discovered.items()}
    identities["library_paths"] = sorted({str(Path(item["path"]).parent)
                                           for item in identities.values() if isinstance(item, dict)})
    return identities


def _within(path: Path, root: Path) -> bool:
    try:
        path.relative_to(root)
        return True
    except ValueError:
        return False


def _link_file_identity(path: Path, file_class: str, identity: str) -> dict[str, Any]:
    data, opened = _read_immutable_file(path, f"link input {path.name}")
    return {"class": file_class, "identity": identity, "size": len(data),
            "sha256": opened["sha256"]}


def _normalize_rpath(token: str, build: Path, provider_roots: Sequence[Path]) -> str:
    if not token.startswith("-Wl,-rpath,"):
        return token
    values = token.removeprefix("-Wl,-rpath,").split(os.pathsep)
    normalized = []
    for raw in values:
        path = Path(raw)
        if not path.is_absolute():
            normalized.append(raw)
        elif _within(path.resolve(strict=False), build):
            normalized.append("fresh")
        elif any(_within(path.resolve(strict=False), root) for root in provider_roots):
            normalized.append("provider")
        elif _within(path.resolve(strict=False), Path("/usr")) or _within(
                path.resolve(strict=False), Path("/lib")):
            normalized.append("system")
        else:
            raise RuntimeError("link rpath is outside registered roots")
    return "-Wl,-rpath," + os.pathsep.join(normalized)


def _normalize_link_argument(token: str, build: Path,
                             provider_roots: Sequence[Path]) -> str:
    if token.startswith("-Wl,-rpath,"):
        return _normalize_rpath(token, build, provider_roots)
    if token.startswith("-L") and len(token) > 2:
        raw = Path(token[2:])
        if not raw.is_absolute():
            return token
        path = raw.resolve(strict=False)
        if _within(path, build):
            return "-Lfresh"
        if any(_within(path, root) for root in provider_roots):
            return "-Lprovider"
        if (_within(path, Path("/usr/lib")) or _within(path, Path("/lib"))
                or _within(path, Path("/usr/local/lib"))):
            return "-Lsystem"
        raise RuntimeError("link search directory is outside registered roots")
    if token.startswith("/") or re.search(r"(?:^|[=,:])/[A-Za-z0-9_.-]", token):
        raise RuntimeError("unclassified host path in linker argument")
    return token


def _expand_link_tokens(plan: Path, *, max_depth: int = _LINK_RESPONSE_DEPTH,
                        plan_data: bytes | None = None, command_dir: Path | None = None,
                        command_tokens: Sequence[str] | None = None
                        ) -> tuple[list[str], list[dict[str, Any]]]:
    plan_dir = (command_dir.resolve(strict=True) if command_dir is not None else
                plan.parent.parent.parent if plan.parent.parent.name == "CMakeFiles" else plan.parent)
    responses: list[dict[str, Any]] = []
    seen_identities: dict[Path, str] = {}

    def parse(data: bytes, label: str) -> list[str]:
        try:
            return shlex.split(data.decode("utf-8"), posix=True)
        except (UnicodeError, ValueError) as exc:
            raise RuntimeError(f"invalid {label} quoting: {exc}") from exc

    def expand(tokens: Sequence[str], depth: int, stack: tuple[Path, ...]) -> list[str]:
        result: list[str] = []
        for token in tokens:
            if not token.startswith("@") or token == "@":
                result.append(token)
                continue
            if depth >= max_depth:
                raise RuntimeError("link response-file expansion depth exceeded")
            raw = Path(token[1:])
            response = (raw if raw.is_absolute() else plan_dir / raw).resolve(strict=True)
            if response in stack:
                raise RuntimeError("link response-file cycle detected")
            data, opened = _read_immutable_file(response, "link response file", max_size=16 * 1024 * 1024)
            identity = seen_identities.get(response)
            if identity is None:
                identity = f"response-{len(responses)}"
                seen_identities[response] = identity
                responses.append({"identity": identity, "size": len(data),
                                  "sha256": opened["sha256"]})
            result.extend(expand(parse(data, "link response file"), depth + 1,
                                 stack + (response,)))
        return result

    if command_tokens is not None:
        if plan_data is not None:
            raise RuntimeError("link command tokens and plan bytes are mutually exclusive")
        initial = list(command_tokens)
    elif plan_data is None:
        raw, _opened = _read_immutable_file(plan, "link plan", max_size=16 * 1024 * 1024)
        initial = parse(raw, "link plan")
    else:
        raw = plan_data
        initial = parse(raw, "link plan")
    return expand(initial, 0, ()), responses


def _run_bounded_command(argv: Sequence[str], *, timeout: float, max_output: int) -> bytes:
    """Run a trusted argv while bounding its combined captured output."""
    process = subprocess.Popen(list(argv), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               start_new_session=True)
    assert process.stdout is not None and process.stderr is not None
    stdout_fd = process.stdout.fileno()
    streams = {stdout_fd, process.stderr.fileno()}
    stdout_chunks: list[bytes] = []
    size = 0
    deadline = time.monotonic() + timeout
    try:
        while streams:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError(f"command timed out: {argv[0]}")
            readable, _, _ = select.select(list(streams), [], [], remaining)
            if not readable:
                continue
            for fd in readable:
                chunk = os.read(fd, min(64 * 1024, max_output - size + 1))
                if not chunk:
                    streams.remove(fd)
                    continue
                size += len(chunk)
                if size > max_output:
                    raise RuntimeError(f"command output exceeds limit: {argv[0]}")
                if fd == stdout_fd:
                    stdout_chunks.append(chunk)
        returncode = process.wait(timeout=max(0.1, deadline - time.monotonic()))
        if returncode != 0:
            raise RuntimeError(f"command failed ({returncode}): {argv[0]}")
        return b"".join(stdout_chunks)
    except BaseException:
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait(timeout=5)
        raise
    finally:
        process.stdout.close()
        process.stderr.close()


def _parse_ninja_command_line(line: str, build: Path) -> list[tuple[list[str], Path, bool]]:
    try:
        lexer = shlex.shlex(line, posix=True, punctuation_chars=";&|<>")
        lexer.whitespace_split = True
        lexer.commenters = ""
        tokens = list(lexer)
    except ValueError as exc:
        raise RuntimeError(f"invalid Ninja command quoting: {exc}") from exc
    segments: list[tuple[list[str], Path, bool]] = []
    command_dir = build
    canonical_command_dir = True
    current: list[str] = []
    for token in tokens + ["&&"]:
        if token == "&&":
            if current:
                if current[0] == "cd":
                    if len(current) != 2:
                        raise RuntimeError("Ninja command has an ambiguous working directory")
                    declared = Path(current[1])
                    if not declared.is_absolute():
                        declared = command_dir / declared
                    try:
                        resolved = declared.resolve(strict=True)
                    except (OSError, RuntimeError) as exc:
                        raise RuntimeError("Ninja command working directory is unavailable") from exc
                    if not resolved.is_dir():
                        raise RuntimeError("Ninja command working directory is unavailable")
                    canonical_command_dir = canonical_command_dir and declared == resolved
                    command_dir = resolved
                elif current != [":"]:
                    segments.append((current, command_dir, canonical_command_dir))
                current = []
            continue
        if token in {";", "&", "|", "||", "<", ">", "<<", ">>"}:
            raise RuntimeError("Ninja command contains unsupported shell control syntax")
        current.append(token)
    return segments


def _command_outputs(tokens: Sequence[str], directory: Path) -> list[Path]:
    outputs: list[Path] = []
    index = 0
    while index < len(tokens):
        token = tokens[index]
        if token == "-o":
            if index + 1 >= len(tokens) or not tokens[index + 1]:
                raise RuntimeError("Ninja command has an incomplete -o")
            outputs.append(_canonical_argument_path(tokens[index + 1], directory))
            index += 2
            continue
        index += 1
    return outputs


def _ninja_link_command(output: bytes, build: Path, expected_output: Path,
                        target: str) -> tuple[list[str], Path]:
    try:
        text = output.decode("utf-8")
    except UnicodeError as exc:
        raise RuntimeError(f"Ninja command output for {target} is not UTF-8") from exc
    matching: list[tuple[list[str], Path]] = []
    for line in text.splitlines():
        if not line.strip():
            continue
        for tokens, directory, canonical_directory in _parse_ninja_command_line(line, build):
            if "-c" in tokens:
                continue
            outputs = _command_outputs(tokens, directory)
            if expected_output not in outputs:
                continue
            if len(outputs) != 1:
                raise RuntimeError(f"Ninja final link command for {target} has multiple outputs")
            if not _within(directory, build):
                raise RuntimeError(
                    f"Ninja final link command for {target} works outside fresh build root")
            if not canonical_directory:
                raise RuntimeError(
                    f"Ninja final link command for {target} uses an aliased working directory")
            matching.append((tokens, directory))
    if len(matching) != 1:
        raise RuntimeError(
            f"expected exactly one Ninja final link command for {target}, found {len(matching)}")
    return matching[0]


def _ninja_plan_source(build: Path, cache: Mapping[str, Mapping[str, str]], target: str,
                       expected_output: Path) -> tuple[Path, bytes, dict[str, Any],
                                                       list[str], Path, list[dict[str, Any]]]:
    raw_tool = cache.get("CMAKE_MAKE_PROGRAM", {}).get("value", "")
    declared = Path(raw_tool)
    try:
        linked = declared.lstat()
        tool = declared.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise RuntimeError("fresh CMAKE_MAKE_PROGRAM is unavailable") from exc
    if (not declared.is_absolute() or declared != tool or stat.S_ISLNK(linked.st_mode)
            or not stat.S_ISREG(linked.st_mode)):
        raise RuntimeError("fresh CMAKE_MAKE_PROGRAM is not a canonical regular tool")
    tool_before = _tool_identity(str(tool))
    authority = build / "build.ninja"
    authority_raw, authority_before = _read_immutable_file(
        authority, "fresh build.ninja", max_size=64 * 1024 * 1024)
    command_output = _run_bounded_command(
        [str(tool), "-C", str(build), "-t", "commands", target],
        timeout=_NINJA_COMMAND_TIMEOUT, max_output=_MAX_NINJA_COMMAND_OUTPUT)
    authority_after_raw, authority_after = _read_immutable_file(
        authority, "fresh build.ninja", max_size=64 * 1024 * 1024)
    tool_after = _tool_identity(str(tool))
    if authority_after_raw != authority_raw or any(
            authority_before[key] != authority_after[key]
            for key in ("device", "inode", "size", "sha256")):
        raise RuntimeError("fresh build.ninja changed during Ninja command export")
    _require_same_tool_identity(tool_before, tool_after, "CMAKE_MAKE_PROGRAM")
    tokens, command_dir = _ninja_link_command(command_output, build, expected_output, target)
    source_evidence = [
        {"identity": "ninja-tool", "size": tool_before["size"],
         "sha256": tool_before["sha256"]},
        {"identity": f"ninja-commands:{target}", "size": len(command_output),
         "sha256": hashlib.sha256(command_output).hexdigest()},
    ]
    return authority, authority_raw, authority_before, tokens, command_dir, source_evidence


def capture_link_closure(build_root: str | Path, anchor_root: str | Path,
                         images: Mapping[str, Mapping[str, Any]],
                         provider: Mapping[str, Any]) -> dict[str, Any]:
    """Capture closed, path-free provenance for the two generated final link plans."""
    build = Path(build_root).resolve(strict=True)
    anchor = Path(anchor_root).resolve(strict=True)
    provider_roots = [Path(item).resolve(strict=True)
                      for item in provider.get("library_paths", [])]
    provider_files = {Path(provider[name]["path"]).resolve(strict=True): name
                      for name in ("dds", "cdr")}
    cache_path = build / "CMakeCache.txt"
    cache_raw: bytes | None = None
    cache_open: dict[str, Any] | None = None
    cache: dict[str, dict[str, str]] = {}
    if cache_path.is_file() and not cache_path.is_symlink():
        cache_raw, cache_open = _read_immutable_file(
            cache_path, "fresh CMakeCache.txt", max_size=16 * 1024 * 1024)
        try:
            cache = parse_cmake_cache(cache_raw.decode("utf-8"))
        except UnicodeError as exc:
            raise RuntimeError("fresh CMakeCache.txt is not UTF-8") from exc
    ninja_generator = cache.get("CMAKE_GENERATOR", {}).get("value", "").startswith("Ninja")
    plans: list[dict[str, Any]] = []
    for target, image_name in _LINK_TARGET_IMAGES.items():
        expected_output = Path(images[image_name]["path"]).resolve(strict=True)
        if ninja_generator:
            (plan, plan_raw, plan_open, command_tokens, plan_dir,
             source_evidence) = _ninja_plan_source(
                 build, cache, target, expected_output)
            tokens, responses = _expand_link_tokens(
                plan, command_tokens=command_tokens, command_dir=plan_dir)
            responses = source_evidence + responses
            plan_identity = "ninja:build.ninja"
        else:
            matches = [path for path in build.rglob("link.txt")
                       if path.parent.name == target + ".dir" and path.is_file()
                       and not path.is_symlink()]
            if len(matches) != 1:
                raise RuntimeError(
                    f"expected one generated link plan for {target}, found {len(matches)}")
            plan = matches[0].resolve(strict=True)
            plan_dir = plan.parent.parent.parent
            plan_raw, plan_open = _read_immutable_file(plan, f"{target} link plan",
                                                       max_size=16 * 1024 * 1024)
            tokens, responses = _expand_link_tokens(plan, plan_data=plan_raw)
            plan_identity = "fresh:" + plan.relative_to(build).as_posix()
        if not tokens:
            raise RuntimeError(f"generated link plan for {target} is empty")
        normalized: list[dict[str, Any]] = []
        inputs: list[dict[str, Any]] = []
        output_seen = False
        output_path: Path | None = None
        index = 0
        while index < len(tokens):
            token = tokens[index]
            if token == "-o":
                if index + 1 >= len(tokens):
                    raise RuntimeError(f"generated link plan for {target} has an incomplete -o")
                raw_output = Path(tokens[index + 1])
                output_path = (raw_output if raw_output.is_absolute() else plan_dir / raw_output).resolve(strict=True)
                normalized.extend(({"kind": "flag", "value": "-o"},
                                   {"kind": "output", "value": f"image:{image_name}"}))
                output_seen = True
                index += 2
                continue
            candidate = Path(token)
            is_file_arg = not token.startswith("-") and bool(_LINK_FILE_RE.search(token))
            resolved: Path | None = None
            if is_file_arg:
                unresolved = candidate if candidate.is_absolute() else plan_dir / candidate
                try:
                    resolved = unresolved.resolve(strict=True)
                except OSError as exc:
                    raise RuntimeError(f"linked input is missing: {token}") from exc
            if index == 0 and candidate.is_absolute():
                driver = candidate.resolve(strict=True)
                identity = _link_file_identity(driver, "toolchain", "linker-driver")
                normalized.append({"kind": "tool", "value": identity["sha256"]})
            elif resolved is not None:
                suffix = resolved.name.lower()
                if _within(resolved, anchor) and not _within(resolved, build):
                    raise RuntimeError(f"linked input is under anchor build: {resolved.name}")
                if suffix.endswith((".o", ".obj")) and not _within(resolved, build):
                    raise RuntimeError(f"linked object is outside fresh build root: {resolved.name}")
                if (suffix.startswith(("libibmw", "ibmw")) and not _within(resolved, build)):
                    raise RuntimeError(f"IBMW linked library is outside fresh build root: {resolved.name}")
                if _within(resolved, build):
                    relative = resolved.relative_to(build).as_posix()
                    file_class = "fresh_object" if suffix.endswith((".o", ".obj")) else "fresh_library"
                    identity_name = "fresh:" + relative
                elif resolved in provider_files:
                    file_class = "provider_library"
                    identity_name = "provider:" + provider_files[resolved]
                elif any(_within(resolved, root) for root in provider_roots):
                    file_class = "external_provider_library"
                    identity_name = "provider-file:" + resolved.name
                elif (_within(resolved, Path("/usr/lib")) or _within(resolved, Path("/lib"))
                      or _within(resolved, Path("/usr/local/lib"))):
                    file_class = "system_library"
                    identity_name = "system:" + resolved.name
                else:
                    raise RuntimeError(f"linked library is not registered: {resolved.name}")
                identity = _link_file_identity(resolved, file_class, identity_name)
                input_index = len(inputs)
                inputs.append(identity)
                normalized.append({"kind": "input", "value": input_index})
            else:
                normalized.append({"kind": "argument",
                                   "value": _normalize_link_argument(token, build, provider_roots)})
            index += 1
        if not output_seen or output_path != expected_output:
            raise RuntimeError(f"generated link plan output mismatch for {target}")
        output_identity = _link_file_identity(expected_output, "fresh_output", f"image:{image_name}")
        if output_identity["sha256"] != images[image_name].get("sha256"):
            raise RuntimeError(f"generated link output identity mismatch for {target}")
        own_objects = [item for item in inputs if item["class"] == "fresh_object"
                       and f"CMakeFiles/{target}.dir/" in item["identity"]]
        if not own_objects:
            raise RuntimeError(f"generated link plan has no fresh target object for {target}")
        plan_value = {"target": target, "image": image_name,
                      "plan": {"identity": plan_identity,
                               "size": len(plan_raw), "sha256": plan_open["sha256"]},
                      "responses": responses, "tokens": normalized, "inputs": inputs,
                      "output": output_identity}
        plan_value["closure_sha256"] = hashlib.sha256(
            core._canonical_json_bytes(plan_value)).hexdigest()
        plans.append(plan_value)
    if cache_raw is not None and cache_open is not None:
        cache_after_raw, cache_after = _read_immutable_file(
            cache_path, "fresh CMakeCache.txt", max_size=16 * 1024 * 1024)
        if cache_after_raw != cache_raw or any(
                cache_open[key] != cache_after[key]
                for key in ("device", "inode", "size", "sha256")):
            raise RuntimeError("fresh CMakeCache.txt changed during link closure capture")
    value = {"schema": "mw_perf.rollback_link_closure.v1", "plans": plans}
    value["content_sha256"] = hashlib.sha256(core._canonical_json_bytes(value)).hexdigest()
    _validate_link_closure(value)
    return value


def _validate_link_closure(value: Any) -> dict[str, Any]:
    schema = json.loads((Path(__file__).parent /
                         "schemas/rollback-link-closure.schema.v1.json").read_bytes())
    if core._schema_errors(value, schema):
        raise ValueError("rollback link closure schema is invalid")
    if not isinstance(value, dict) or set(value) != {"schema", "plans", "content_sha256"}:
        raise ValueError("rollback link closure is malformed")
    if value.get("schema") != "mw_perf.rollback_link_closure.v1":
        raise ValueError("rollback link closure schema is invalid")
    plans = value.get("plans")
    if not isinstance(plans, list) or [item.get("target") for item in plans
                                      if isinstance(item, dict)] != list(_LINK_TARGET_IMAGES):
        raise ValueError("rollback link closure target set is invalid")
    hash_pattern = re.compile(r"[0-9a-f]{64}")
    for plan in plans:
        required = {"target", "image", "plan", "responses", "tokens", "inputs", "output",
                    "closure_sha256"}
        if not isinstance(plan, dict) or set(plan) != required:
            raise ValueError("rollback link closure plan is malformed")
        target = plan["target"]
        if plan["image"] != _LINK_TARGET_IMAGES[target]:
            raise ValueError("rollback link closure image binding is invalid")
        for identity in [plan["plan"], *plan["responses"], *plan["inputs"], plan["output"]]:
            if (not isinstance(identity, dict) or not hash_pattern.fullmatch(str(identity.get("sha256")))
                    or not isinstance(identity.get("size"), int) or identity["size"] < 0):
                raise ValueError("rollback link closure identity is malformed")
        plan_identity = str(plan["plan"].get("identity", ""))
        if (set(plan["plan"]) != {"identity", "size", "sha256"}
                or not (plan_identity.startswith("fresh:")
                        or plan_identity == "ninja:build.ninja")):
            raise ValueError("rollback link plan identity is invalid")
        if any(set(item) != {"identity", "size", "sha256"} for item in plan["responses"]):
            raise ValueError("rollback response identity is malformed")
        if plan_identity == "ninja:build.ninja":
            source = plan["responses"][:2]
            if (len(source) != 2 or source[0]["identity"] != "ninja-tool"
                    or source[1]["identity"] != "ninja-commands:" + target
                    or source[0]["size"] < 1 or source[1]["size"] < 1):
                raise ValueError("rollback Ninja plan source evidence is malformed")
            if any(item["identity"].startswith("ninja-")
                   for item in plan["responses"][2:]):
                raise ValueError("rollback Ninja plan source evidence is ambiguous")
        elif any(item["identity"].startswith("ninja-") for item in plan["responses"]):
            raise ValueError("rollback Makefiles plan has Ninja source evidence")
        allowed_classes = {"fresh_object", "fresh_library", "provider_library",
                           "external_provider_library", "system_library"}
        if any(set(item) != {"class", "identity", "size", "sha256"}
               or item["class"] not in allowed_classes for item in plan["inputs"]):
            raise ValueError("rollback linked input identity is malformed")
        prefixes = {"fresh_object": "fresh:", "fresh_library": "fresh:",
                    "provider_library": "provider:",
                    "external_provider_library": "provider-file:",
                    "system_library": "system:"}
        if any(not item["identity"].startswith(prefixes[item["class"]])
               for item in plan["inputs"]):
            raise ValueError("rollback linked input class identity is inconsistent")
        if any(item["class"] == "fresh_object" and not item["identity"].startswith("fresh:")
               for item in plan["inputs"]):
            raise ValueError("rollback linked object is not fresh")
        if not any(item["class"] == "fresh_object" and
                   f"CMakeFiles/{target}.dir/" in item["identity"] for item in plan["inputs"]):
            raise ValueError("rollback link closure omits a fresh target object")
        output = plan["output"]
        if (set(output) != {"class", "identity", "size", "sha256"}
                or output["class"] != "fresh_output"
                or output["identity"] != "image:" + plan["image"] or output["size"] < 1):
            raise ValueError("rollback link closure output is invalid")
        if not isinstance(plan["tokens"], list) or not plan["tokens"]:
            raise ValueError("rollback normalized link tokens are empty")
        for token in plan["tokens"]:
            if (not isinstance(token, dict) or set(token) != {"kind", "value"}
                    or token["kind"] not in {"tool", "flag", "output", "input", "argument"}):
                raise ValueError("rollback normalized link token is malformed")
            if token["kind"] == "input" and (not isinstance(token["value"], int)
                    or token["value"] < 0 or token["value"] >= len(plan["inputs"])):
                raise ValueError("rollback normalized link input reference is invalid")
            if isinstance(token["value"], str) and (token["value"].startswith("/")
                    or re.match(r"^[A-Za-z]:[\\/]", token["value"]) or "\\" in token["value"]):
                raise ValueError("rollback normalized link token leaks a host path")
        output_tokens = [token for token in plan["tokens"] if token["kind"] == "output"]
        output_flags = [token for token in plan["tokens"]
                        if token["kind"] == "flag" and token["value"] == "-o"]
        if (output_tokens != [{"kind": "output", "value": "image:" + plan["image"]}]
                or len(output_flags) != 1):
            raise ValueError("rollback normalized link output binding is invalid")
        expected_plan_hash = hashlib.sha256(core._canonical_json_bytes(
            {name: plan[name] for name in required - {"closure_sha256"}})).hexdigest()
        if plan["closure_sha256"] != expected_plan_hash:
            raise ValueError("rollback link closure digest is inconsistent")
    ninja_plans = [plan for plan in plans
                   if plan["plan"]["identity"] == "ninja:build.ninja"]
    if ninja_plans:
        if len(ninja_plans) != len(plans):
            raise ValueError("rollback link closure mixes generator plan sources")
        authority = ninja_plans[0]["plan"]
        exporter = ninja_plans[0]["responses"][0]
        if (any(plan["plan"] != authority for plan in ninja_plans[1:])
                or any(plan["responses"][0] != exporter for plan in ninja_plans[1:])):
            raise ValueError("rollback Ninja authority changed between target exports")
    expected = hashlib.sha256(core._canonical_json_bytes(
        {"schema": value["schema"], "plans": plans})).hexdigest()
    if value["content_sha256"] != expected:
        raise ValueError("rollback link closure content digest is inconsistent")
    return value


def _link_closure_summary(value: Any) -> dict[str, Any]:
    closure = _validate_link_closure(value)
    return {"content_sha256": closure["content_sha256"],
            "targets": {plan["target"]: {
                "image": plan["image"], "closure_sha256": plan["closure_sha256"],
                "output_sha256": plan["output"]["sha256"],
                "input_count": len(plan["inputs"]),
                "response_count": sum(item["identity"].startswith("response-")
                                      for item in plan["responses"])}
                         for plan in closure["plans"]}}


def _validate_link_inventory_binding(closure: Any, inventory: Any) -> None:
    link_value = _validate_link_closure(closure)
    compile_value = _validate_packaged_inventory(inventory)
    outputs = {item["output"]: item for item in compile_value["records"]}
    for plan in link_value["plans"]:
        target_objects = []
        for item in plan["inputs"]:
            if item["class"] != "fresh_object":
                continue
            logical = item["identity"].removeprefix("fresh:")
            record = outputs.get(logical)
            if record is None:
                raise ValueError("rollback linked object is absent from compile inventory")
            if record.get("target") == plan["target"]:
                target_objects.append(logical)
        if not target_objects:
            raise ValueError("rollback final link lacks a compile-bound target object")


def _validate_link_tool_binding(closure: Any, tools: Any) -> None:
    if not isinstance(tools, dict):
        raise ValueError("rollback link tool binding is unavailable")
    registered = {item.get("sha256") for item in tools.values() if isinstance(item, dict)}
    for plan in _validate_link_closure(closure)["plans"]:
        linked = [item["value"] for item in plan["tokens"] if item["kind"] == "tool"]
        if len(linked) != 1 or linked[0] not in registered:
            raise ValueError("rollback linker driver is not a registered toolchain identity")
        if plan["plan"]["identity"] == "ninja:build.ninja":
            make_program = tools.get("CMAKE_MAKE_PROGRAM")
            if (not isinstance(make_program, dict)
                    or plan["responses"][0]["sha256"] != make_program.get("sha256")):
                raise ValueError(
                    "rollback Ninja exporter is not the registered CMAKE_MAKE_PROGRAM")


def _audit_image(path: Path, logs: Path, nm: str, readelf: str, expected: re.Pattern[str]) -> dict[str, Any]:
    audits: dict[str, Any] = {}
    all_symbols: list[str] = []
    for label, argv in (("nm_all", [nm, "-a", "-C", str(path)]),
                        ("nm_dynamic", [nm, "-D", "-C", str(path)])):
        result = _run(argv, logs / f"{path.name}.{label}", 60)
        symbols = parse_nm_symbols(result.stdout)
        if not symbols:
            raise RuntimeError(f"{label} yielded no parsed symbols for {path.name}; image may be stripped or output is unsupported")
        all_symbols.extend(symbols)
        rejected = sorted(symbol for symbol in symbols if _TRACE_SYMBOL.search(symbol))
        if rejected:
            raise RuntimeError(f"trace symbols found in {path.name}: {rejected[:3]}")
        audits[label] = {"symbol_count": len(symbols), "stdout_sha256": _sha256(logs / f"{path.name}.{label}.stdout.log")}
    if not any(expected.search(symbol) for symbol in all_symbols):
        raise RuntimeError(f"required ordinary symbol is absent from {path.name}")
    notes = _run([readelf, "-n", str(path)], logs / f"{path.name}.readelf_notes", 60)
    match = re.search(r"Build ID:\s*([0-9a-fA-F]+)", notes.stdout)
    if not match:
        raise RuntimeError(f"ELF build ID unavailable for {path.name}")
    return {"path": str(path), "sha256": _sha256(path), "size": path.stat().st_size,
            "build_id": match.group(1).lower(), "audits": audits,
            "readelf_stdout_sha256": _sha256(logs / f"{path.name}.readelf_notes.stdout.log")}


def _execute_candidate(candidate: Path, extension: Path, build_dir: Path,
                       provider: Mapping[str, Any], logs: Path) -> dict[str, Any]:
    ready_r, ready_w = os.pipe()
    release_r, release_w = os.pipe()
    stdout_path, stderr_path = logs / "candidate.stdout.log", logs / "candidate.stderr.log"
    env = dict(os.environ)
    env["MW_PERF_ROLLBACK_READY_FD"] = str(ready_w)
    env["MW_PERF_ROLLBACK_RELEASE_FD"] = str(release_r)
    env["LD_LIBRARY_PATH"] = os.pathsep.join([str(build_dir), *provider["library_paths"]])
    process: subprocess.Popen[bytes] | None = None
    try:
        with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
            process = subprocess.Popen([str(candidate)], stdout=stdout, stderr=stderr, env=env,
                                       pass_fds=(ready_w, release_r), start_new_session=True)
            os.close(ready_w); ready_w = -1
            os.close(release_r); release_r = -1
            if not select.select([ready_r], [], [], _HANDSHAKE_TIMEOUT)[0] or os.read(ready_r, 1) != b"R":
                raise RuntimeError("candidate did not provide rollback ready byte R")
            observations = _runtime_observations(process.pid, "rollback-evaluation-" + uuid.uuid4().hex)
            candidate_sha, extension_sha = _sha256(candidate), _sha256(extension)
            executable = [item for item in observations if item["source"] == "exe" and item["resolved_path"] == str(candidate) and item["sha256"] == candidate_sha]
            mapped = [item for item in observations if item["source"] == "maps" and item["resolved_path"] == str(extension) and item["sha256"] == extension_sha]
            if not executable or not mapped:
                raise RuntimeError("runtime executable/extension image registration was not proven")
            provider_matches: dict[str, list[dict[str, Any]]] = {}
            expected_provider_paths = {item["path"] for key, item in provider.items()
                                       if key in {"dds", "cdr"}}
            for family in ("dds", "cdr"):
                identity = provider[family]
                matches = [item for item in observations if item["source"] == "maps"
                           and item["device"] == identity["device"]
                           and item["inode"] == identity["inode"]
                           and item["sha256"] == identity["sha256"]]
                if not matches:
                    raise RuntimeError(f"runtime {family} provider mapping does not match registered identity")
                provider_matches[family] = matches
            alternate = [item for item in observations if item["source"] == "maps"
                         and re.search(r"/lib(?:fastdds|fastrtps|fastcdr)\.so", item["resolved_path"])
                         and item["resolved_path"] not in expected_provider_paths]
            if alternate:
                raise RuntimeError(f"unexpected alternate DDS provider mappings: {[x['resolved_path'] for x in alternate]}")
            if os.write(release_w, b"G") != 1:
                raise RuntimeError("candidate release byte write failed")
            returncode = process.wait(timeout=_EXIT_TIMEOUT)
            if returncode != 0:
                raise RuntimeError(f"candidate exited with status {returncode}")
        return {"pid": process.pid, "returncode": returncode, "observations": observations,
                "executable_matches": executable, "extension_matches": mapped,
                "provider_matches": provider_matches,
                "stdout_sha256": _sha256(stdout_path), "stderr_sha256": _sha256(stderr_path)}
    finally:
        for fd in (ready_r, ready_w, release_r, release_w):
            if fd >= 0:
                try: os.close(fd)
                except OSError: pass
        if process is not None and process.poll() is None:
            try: os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            process.wait(timeout=5)


def execute_trace_off_build(source_root: str | Path, anchor_build_dir: str | Path,
                            work_root: str | Path) -> dict[str, Any]:
    """Configure, audit, and handshake-run a fresh COMM_TRACE=OFF build."""
    source = Path(source_root).resolve(strict=True)
    anchor = Path(anchor_build_dir).resolve(strict=True)
    work = Path(work_root)
    if work.exists():
        raise RuntimeError("rollback work_root must not exist")
    try:
        work.mkdir(mode=0o700, parents=False)
        logs = work / "logs"; logs.mkdir(mode=0o700)
        build = work / "build"; build.mkdir(mode=0o700)
        anchor_cache_path = anchor / "CMakeCache.txt"
        anchor_cache_identity = _file_identity(anchor_cache_path)
        anchor_cache = parse_cmake_cache(anchor_cache_path)
        _cache_home(anchor_cache, source, "anchor")
        anchor_selected = selected_options(anchor_cache)
        anchor_toolchain = toolchain_options(anchor_cache)
        if anchor_selected.get("IBMW_ENABLE_COMM_TRACE") != "ON":
            raise RuntimeError("anchor IBMW_ENABLE_COMM_TRACE must be ON")
        for option in _REQUIRED_ON:
            if anchor_selected.get(option) != "ON":
                raise RuntimeError(f"anchor {option} must be ON")
        source_before = source_identity(source)
        anchor_dependency_base, dependencies = _anchor_dependency_sources(anchor_cache)
        aggregate_identity = {"anchor_before_staging": _aggregate_dependency_identity(
            dependencies, "anchor_before_staging")}
        source_cache, fresh_dependency_base = _stage_dependency_sources(work, dependencies)
        aggregate_identity["staged_after_copy"] = _record_dependency_checkpoint(
            dependencies, "staged_after_copy", "staged_source")
        _require_dependency_checkpoint(dependencies, "staged_after_copy")
        aggregate_identity["anchor_after_staging"] = _record_dependency_checkpoint(
            dependencies, "anchor_after_staging", "anchor_source")
        _require_dependency_checkpoint(dependencies, "anchor_after_staging")

        cmake = anchor_cache.get("CMAKE_COMMAND", {}).get("value", "")
        if not cmake:
            raise RuntimeError("anchor CMAKE_COMMAND is unavailable")
        anchor_cmake_identity = _tool_identity(cmake)
        anchor_tool_identities = _active_tool_identities(anchor_toolchain)
        _audit_replayed_controls(
            {"CMAKE_GENERATOR": anchor_cache.get("CMAKE_GENERATOR", {}).get("value", ""),
             **anchor_toolchain, **anchor_selected}, directory=build,
            anchor_base=anchor_dependency_base)
        argv = _configure_argv(cmake, source, build, anchor_cache, anchor_toolchain,
                               anchor_selected, fresh_dependency_base, dependencies)
        _run(argv, logs / "configure", 600)
        aggregate_identity["staged_after_configure"] = _record_dependency_checkpoint(
            dependencies, "staged_after_configure", "staged_source")
        _require_dependency_checkpoint(dependencies, "staged_after_configure")
        aggregate_identity["anchor_after_configure"] = _record_dependency_checkpoint(
            dependencies, "anchor_after_configure", "anchor_source")
        _require_dependency_checkpoint(dependencies, "anchor_after_configure")
        configured_cache = parse_cmake_cache(build / "CMakeCache.txt")
        _require_dependency_cache(configured_cache, fresh_dependency_base, source_cache, dependencies,
                                  anchor_dependency_base)
        _run([cmake, "--build", str(build), "--target", "ibmw_dds_extension",
              "ibmw_dds_trace_off_business_roundtrip", "--", "-j2"], logs / "build", 1800)
        aggregate_identity["staged_after_build"] = _record_dependency_checkpoint(
            dependencies, "staged_after_build", "staged_source")
        _require_dependency_checkpoint(dependencies, "staged_after_build")
        aggregate_identity["anchor_after_build"] = _record_dependency_checkpoint(
            dependencies, "anchor_after_build", "anchor_source")
        _require_dependency_checkpoint(dependencies, "anchor_after_build")

        fresh_cache = parse_cmake_cache(build / "CMakeCache.txt")
        _require_dependency_cache(fresh_cache, fresh_dependency_base, source_cache, dependencies,
                                  anchor_dependency_base)
        _cache_home(fresh_cache, source, "fresh")
        fresh_selected = selected_options(fresh_cache)
        fresh_toolchain = toolchain_options(fresh_cache)
        fresh_cmake = fresh_cache.get("CMAKE_COMMAND", {}).get("value", "")
        if not fresh_cmake:
            raise RuntimeError("fresh CMAKE_COMMAND is unavailable")
        if fresh_cmake != cmake:
            raise RuntimeError("fresh CMAKE_COMMAND differs from anchor")
        fresh_cmake_identity = _tool_identity(fresh_cmake)
        _require_same_tool_identity(anchor_cmake_identity, fresh_cmake_identity, "CMAKE_COMMAND")
        fresh_tool_identities = _active_tool_identities(fresh_toolchain)
        if set(fresh_tool_identities) != set(anchor_tool_identities):
            raise RuntimeError("fresh executable toolchain controls differ from anchor")
        for name in anchor_tool_identities:
            _require_same_tool_identity(anchor_tool_identities[name], fresh_tool_identities[name], name)
        if fresh_selected.get("IBMW_ENABLE_COMM_TRACE") != "OFF":
            raise RuntimeError("fresh IBMW_ENABLE_COMM_TRACE is not OFF")
        delta = option_delta(anchor_selected, fresh_selected)
        expected_delta = {"IBMW_ENABLE_COMM_TRACE"}
        if anchor_selected.get("IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE") == "ON":
            expected_delta.add("IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE")
        if set(delta) != expected_delta:
            raise RuntimeError(
                "fresh selected option delta is not limited to communication trace and the "
                f"separate managed-candidate build control: {sorted(delta)}")
        expected_product = dict(anchor_selected)
        expected_product["IBMW_ENABLE_COMM_TRACE"] = "OFF"
        expected_product["IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE"] = "OFF"
        _compare_controls(expected_product, fresh_selected, "product")
        _compare_controls(anchor_toolchain, fresh_toolchain, "toolchain", compiler_aliases=True)
        evidence_control = {
            "name": "CMAKE_EXPORT_COMPILE_COMMANDS", "forced": "ON",
            "fresh": fresh_cache.get("CMAKE_EXPORT_COMPILE_COMMANDS", {}).get("value")}
        if evidence_control["fresh"] != "ON":
            raise RuntimeError("fresh CMAKE_EXPORT_COMPILE_COMMANDS is not ON")
        dependency_source_control = {
            "provider": "anchor_fetchcontent_source_cache",
            "reuse_scope": "source_only",
            "cache_verified": True,
            "anchor_base_dir": str(anchor_dependency_base),
            "fresh_base_dir": str(fresh_dependency_base),
            "staged_source_cache": str(source_cache),
            "dependencies": dependencies,
            "aggregate_identity": aggregate_identity,
        }
        inventory = parse_compile_inventory(
            build / "compile_commands.json", source, build, anchor_dependency_base,
            {item["name"]: item["staged_source"] for item in dependencies})
        extension = _regular_unique(build, "libibmw_dds_transport.so")
        candidate = _regular_unique(build, "ibmw_dds_trace_off_business_roundtrip")
        nm = anchor_cache.get("CMAKE_NM", {}).get("value") or shutil.which("nm")
        readelf = anchor_cache.get("CMAKE_READELF", {}).get("value") or shutil.which("readelf")
        if not nm or not readelf:
            raise RuntimeError("nm and readelf are required")
        images = {"extension": _audit_image(extension, logs, nm, readelf,
                                               re.compile(r"DdsTransportDriver|(?:init|Init).*extension", re.I)),
                  "executable": _audit_image(candidate, logs, nm, readelf,
                                                re.compile(r"(?:^|\s)main(?:\(|$)"))}
        provider = _provider_identity(fresh_cache, readelf)
        link_closure = capture_link_closure(build, anchor, images, provider)
        runtime = _execute_candidate(candidate, extension, build, provider, logs)
        if capture_link_closure(build, anchor, images, provider) != link_closure:
            raise RuntimeError("generated link closure changed after runtime validation")
        source_after = source_identity(source)
        _require_same_source_identity(source_before, source_after)
        log_hashes = {str(path.relative_to(work)): _sha256(path) for path in sorted(logs.iterdir()) if path.is_file()}
        return {"work_root": str(work.resolve()), "build_dir": str(build.resolve()),
                "selected_options": fresh_selected, "product_options": {
                    "anchor": anchor_selected, "fresh": fresh_selected, "delta": delta},
                "toolchain_options": {"anchor": anchor_toolchain, "fresh": fresh_toolchain},
                "delta": delta, "evidence_control": evidence_control,
                "dependency_source_control": dependency_source_control,
                "compile_inventory": inventory, "link_closure": link_closure, "images": images,
                "runtime": runtime, "provider_identity": provider,
                "provider_library_paths": provider["library_paths"],
                "source_identity": {"before": source_before, "after": source_after},
                "anchor_cache_identity": anchor_cache_identity,
                "logs": log_hashes, "tools": {"cmake": fresh_cmake_identity,
                    "active_toolchain": fresh_tool_identities}}
    except BaseException:
        shutil.rmtree(work, ignore_errors=True)
        raise


# Packaging is intentionally separate from execute_trace_off_build.  The runner's
# dictionaries are evidence claims, never authority for bytes or identities.
_ROLLBACK_SCHEMA = "mw_perf.rollback_trace_off.v2"
_ROLLBACK_MANIFEST_SCHEMA = "mw_perf.rollback_evidence_manifest.v1"
_ROLLBACK_CONTRACT = "mw_perf.p0a_contract.v8"
_IMAGE_NAMES = {"extension": "images/extension", "executable": "images/executable"}
_ROLLBACK_PAYLOAD_ROLES = {
    "build_identity.json": "build_identity",
    "build_plan.json": "build_plan",
    "compile_source_inventory.json": "compile_inventory",
    "images/executable": "executable_image",
    "images/extension": "extension_image",
    "link_closure.json": "link_closure",
    "provider_identity.json": "provider_identity",
    "rollback_trace_off.json": "report",
    "runtime_resolution.json": "runtime_resolution",
    "symbol_audit.json": "symbol_audit",
}
_SOURCE_FIELDS = frozenset({"root", "commit", "branch", "dirty", "diff_sha256",
                            "untracked_tree_sha256", "untracked_count", "working_tree_sha256"})


def _path_free(value: Any, *, key: str = "") -> Any:
    if isinstance(value, dict):
        result = {}
        for name, item in sorted(value.items()):
            if (not isinstance(name, str) or "path" in name.lower()
                    or name.lower().endswith(("_dir", "_root"))):
                raise ValueError("rollback evidence contains an unknown path-like key")
            result[name] = _path_free(item, key=name)
        return result
    if isinstance(value, list):
        return [_path_free(item, key=key) for item in value]
    if isinstance(value, str) and (value.startswith(("/", "\\\\"))
                                   or re.match(r"^[A-Za-z]:[\\/]", value)
                                   or "\\" in value):
        raise ValueError("rollback evidence contains a path")
    return value


def _validate_build_plan(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError("rollback build plan is malformed")
    schema = json.loads(
        (Path(__file__).parent / "schemas/rollback-build-plan.schema.v1.json").read_bytes())
    if core._schema_errors(value, schema):
        raise ValueError("rollback build plan schema is invalid")
    # These two schema-defined fields contain logical archive/build references.
    _path_free({name: item for name, item in value.items() if name != "compile"})
    for record in value["compile"]["records"]:
        for name in ("source", "output"):
            reference = record[name]
            if (reference.startswith("/") or "\\" in reference
                    or any(part in {".", ".."} for part in reference.split("/"))):
                raise ValueError("rollback build plan logical reference is invalid")
    compile_value = value["compile"]
    if (compile_value["record_count"] != len(compile_value["records"])
            or compile_value["record_count"] < 2):
        raise ValueError("rollback build plan compile count is inconsistent")
    for reference in value["logs"]:
        if (not reference or reference.startswith("/") or "\\" in reference
                or any(part in {"", ".", ".."} for part in reference.split("/"))):
            raise ValueError("rollback build plan log reference is invalid")
    return value


def _identity_without_paths(value: Any) -> Any:
    if not isinstance(value, dict) or not set(value) <= _SOURCE_FIELDS or "root" not in value:
        raise ValueError("identity must be an object")
    return _path_free({name: item for name, item in value.items() if name != "root"})


def _portable_file_identity(value: Any, label: str, *, require_build_id: bool = False) -> dict[str, Any]:
    if not isinstance(value, dict) or not isinstance(value.get("path"), str):
        raise ValueError(f"{label} identity is incomplete")
    path = Path(value["path"])
    if not path.is_file() or path.is_symlink() or value.get("sha256") != _sha256(path):
        raise ValueError(f"{label} identity does not match bytes")
    selected = {name: value.get(name) for name in ("sha256", "size", "version")}
    if require_build_id:
        selected["build_id"] = value.get("build_id")
        if not isinstance(selected["build_id"], str) or not selected["build_id"]:
            raise ValueError(f"{label} build ID is unavailable")
    return selected


def _portable_tool(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != {
            "path", "device", "inode", "size", "sha256", "version"}:
        raise ValueError(f"{label} tool identity has unknown or missing fields")
    return {key: value[key] for key in ("sha256", "size", "version")}


def _portable_provider(value: Any, label: str) -> dict[str, Any]:
    if not isinstance(value, dict) or set(value) != {
            "path", "device", "inode", "size", "sha256", "build_id", "version"}:
        raise ValueError(f"{label} provider identity has unknown or missing fields")
    result = {key: value[key] for key in ("sha256", "size", "version", "build_id")}
    if not isinstance(result["build_id"], str) or not result["build_id"]:
        raise ValueError(f"{label} provider build ID is unavailable")
    return result


def _portable_dependency_source(value: Any) -> dict[str, Any]:
    required = {"provider", "reuse_scope", "cache_verified", "anchor_base_dir",
                "fresh_base_dir", "staged_source_cache", "dependencies",
                "aggregate_identity"}
    if not isinstance(value, dict) or set(value) != required:
        raise ValueError("rollback dependency source control is incomplete")
    if (value.get("provider") != "anchor_fetchcontent_source_cache"
            or value.get("reuse_scope") != "source_only"
            or value.get("cache_verified") is not True):
        raise ValueError("rollback dependency source control is invalid")
    dependencies = value.get("dependencies")
    if (not isinstance(dependencies, list) or not dependencies
            or len(dependencies) > _MAX_DEPENDENCY_COUNT
            or dependencies != sorted(dependencies, key=lambda item: str(item.get("name", "")))):
        raise ValueError("rollback dependency source inventory is invalid")
    portable_dependencies = []
    seen_names: set[str] = set()
    seen_overrides: set[str] = set()
    identity_fields = {"path", "file_count", "directory_count", "byte_count", "tree_sha256"}
    for dependency in dependencies:
        if not isinstance(dependency, dict) or set(dependency) != {
                "name", "override_variable", "cache_bindings", "anchor_source",
                "staged_source", "identity"}:
            raise ValueError("rollback dependency source entry is malformed")
        name, override = dependency.get("name"), dependency.get("override_variable")
        if (not isinstance(name, str) or not name or name in seen_names
                or not isinstance(override, str) or override != _dependency_override(name)
                or override in seen_overrides):
            raise ValueError("rollback dependency source name or override is invalid")
        seen_names.add(name); seen_overrides.add(override)
        bindings = dependency.get("cache_bindings")
        if (not isinstance(bindings, list)
                or bindings != sorted(bindings, key=lambda item: str(item.get("variable", "")))
                or any(not isinstance(item, dict) or set(item) != {"variable", "relative"}
                       or not isinstance(item["variable"], str)
                       or not item["variable"].endswith(("_SOURCE_DIR", "_LOCAL_SOURCE"))
                       or not _valid_dependency_relative(item["relative"])
                       for item in bindings)):
            raise ValueError("rollback dependency cache bindings are invalid")
        identities = dependency.get("identity")
        if not isinstance(identities, dict) or set(identities) != set(_DEPENDENCY_CHECKPOINTS):
            raise ValueError("rollback dependency source checkpoints are incomplete")
        baseline = None
        for checkpoint in _DEPENDENCY_CHECKPOINTS:
            identity = identities.get(checkpoint)
            expected_path = (dependency["anchor_source"] if checkpoint.startswith("anchor_")
                             else dependency["staged_source"])
            if (not isinstance(identity, dict) or set(identity) != identity_fields
                    or identity.get("path") != expected_path
                    or not re.fullmatch(r"[0-9a-f]{64}", str(identity.get("tree_sha256")))
                    or not isinstance(identity.get("file_count"), int)
                    or identity["file_count"] < 1
                    or not isinstance(identity.get("directory_count"), int)
                    or identity["directory_count"] < 0
                    or not isinstance(identity.get("byte_count"), int)
                    or identity["byte_count"] < 1):
                raise ValueError("rollback dependency source identity is malformed")
            content = _dependency_content_identity(identity)
            baseline = content if baseline is None else baseline
            if content != baseline:
                raise ValueError("rollback dependency source identity changed")
        assert baseline is not None
        portable_dependencies.append({"name": name, "override_variable": override,
                                      "cache_bindings": bindings, **baseline})
    aggregate = value.get("aggregate_identity")
    if not isinstance(aggregate, dict) or set(aggregate) != set(_DEPENDENCY_CHECKPOINTS):
        raise ValueError("rollback aggregate dependency checkpoints are incomplete")
    expected_aggregates = {checkpoint: _aggregate_dependency_identity(dependencies, checkpoint)
                           for checkpoint in _DEPENDENCY_CHECKPOINTS}
    if aggregate != expected_aggregates or len({core._canonical_json_bytes(item)
                                                for item in aggregate.values()}) != 1:
        raise ValueError("rollback aggregate dependency identity changed")
    baseline_aggregate = aggregate["anchor_before_staging"]
    return {"provider": value["provider"], "reuse_scope": value["reuse_scope"],
            "cache_verified": True, "dependency_count": len(portable_dependencies),
            "file_count": baseline_aggregate["file_count"],
            "directory_count": baseline_aggregate["directory_count"],
            "byte_count": baseline_aggregate["byte_count"],
            "aggregate_sha256": baseline_aggregate["aggregate_sha256"],
            "mutation_checkpoints": list(_DEPENDENCY_CHECKPOINTS),
            "checkpoint_digests": {name: aggregate[name]["aggregate_sha256"]
                                   for name in _DEPENDENCY_CHECKPOINTS},
            "dependencies": portable_dependencies}


def _validate_v2_dependency_summary(value: Any) -> None:
    """Recompute all v2 dependency summary authority from packaged entries."""
    if not isinstance(value, dict):
        raise ValueError("rollback v2 dependency summary is malformed")
    dependencies = value.get("dependencies")
    if (not isinstance(dependencies, list) or not dependencies
            or len(dependencies) > _MAX_DEPENDENCY_COUNT):
        raise ValueError("rollback v2 dependency inventory is malformed")
    records: list[dict[str, Any]] = []
    names: set[str] = set()
    overrides: set[str] = set()
    required = {"name", "override_variable", "cache_bindings", "file_count",
                "directory_count", "byte_count", "tree_sha256"}
    for dependency in dependencies:
        if not isinstance(dependency, dict) or set(dependency) != required:
            raise ValueError("rollback v2 dependency entry is malformed")
        name, override = dependency.get("name"), dependency.get("override_variable")
        if (not isinstance(name, str) or not name or name in names
                or not isinstance(override, str) or override != _dependency_override(name)
                or override in overrides):
            raise ValueError("rollback v2 dependency identity is invalid")
        names.add(name); overrides.add(override)
        bindings = dependency.get("cache_bindings")
        if (not isinstance(bindings, list)
                or bindings != sorted(bindings, key=lambda item: str(item.get("variable", "")))
                or any(not isinstance(item, dict) or set(item) != {"variable", "relative"}
                       or not isinstance(item.get("variable"), str)
                       or not item["variable"]
                       or not item["variable"].endswith(("_SOURCE_DIR", "_LOCAL_SOURCE"))
                       or not _valid_dependency_relative(item.get("relative"))
                       for item in bindings)):
            raise ValueError("rollback v2 dependency bindings are invalid")
        file_count = dependency.get("file_count")
        directory_count = dependency.get("directory_count")
        byte_count = dependency.get("byte_count")
        if (not isinstance(file_count, int) or isinstance(file_count, bool)
                or not isinstance(directory_count, int) or isinstance(directory_count, bool)
                or not isinstance(byte_count, int) or isinstance(byte_count, bool)
                or file_count < 1 or file_count > _MAX_DEPENDENCY_FILES
                or directory_count < 0 or directory_count > _MAX_DEPENDENCY_DIRECTORIES
                or file_count + directory_count > _MAX_DEPENDENCY_ENTRIES
                or byte_count < 1 or byte_count > _MAX_DEPENDENCY_TOTAL_BYTES
                or not re.fullmatch(r"[0-9a-f]{64}", str(dependency.get("tree_sha256")))):
            raise ValueError("rollback v2 dependency resource identity is invalid")
        records.append({key: dependency[key] for key in (
            "name", "override_variable", "cache_bindings", "file_count",
            "directory_count", "byte_count", "tree_sha256")})
    expected = {
        "dependency_count": len(records),
        "file_count": sum(item["file_count"] for item in records),
        "directory_count": sum(item["directory_count"] for item in records),
        "byte_count": sum(item["byte_count"] for item in records),
        "aggregate_sha256": hashlib.sha256(core._canonical_json_bytes(records)).hexdigest(),
    }
    if records != sorted(records, key=lambda item: item["name"]):
        raise ValueError("rollback v2 dependency inventory is not canonically ordered")
    try:
        _require_aggregate_dependency_limits(expected)
    except RuntimeError as exc:
        raise ValueError("rollback v2 dependency aggregate exceeds resource limits") from exc
    if any(value.get(key) != item for key, item in expected.items()):
        raise ValueError("rollback v2 dependency aggregate is inconsistent")
    if value.get("mutation_checkpoints") != list(_DEPENDENCY_CHECKPOINTS):
        raise ValueError("rollback v2 dependency checkpoints are inconsistent")
    checkpoint_digests = value.get("checkpoint_digests")
    if (not isinstance(checkpoint_digests, dict)
            or set(checkpoint_digests) != set(_DEPENDENCY_CHECKPOINTS)
            or any(checkpoint_digests.get(checkpoint) != expected["aggregate_sha256"]
                   for checkpoint in _DEPENDENCY_CHECKPOINTS)):
        raise ValueError("rollback v2 dependency checkpoint digest is inconsistent")


def _portable_observation(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError("runtime observation is malformed")
    allowed = {"run_id", "pid", "start_time_ticks", "source", "resolved_path", "path",
               "device", "inode", "size", "sha256", "identity_source", "mapping",
               "observation_id"}
    if not set(value) <= allowed:
        raise ValueError("runtime observation has unknown fields")
    required = {"source", "device", "inode", "size", "sha256"}
    if not required <= set(value):
        raise ValueError("runtime observation identity is incomplete")
    result = {key: value[key] for key in ("source", "device", "inode", "size", "sha256")}
    for key in ("identity_source", "mapping"):
        if key in value:
            result[key] = _path_free(value[key], key=key)
    return result


def _portable_runtime(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ValueError("rollback runtime evidence is incomplete")
    required = {"returncode", "observations", "executable_matches", "extension_matches",
                "provider_matches", "stdout_sha256", "stderr_sha256"}
    if not required <= set(value) or not set(value) <= required | {"pid"}:
        raise ValueError("rollback runtime evidence is incomplete")
    observations = sorted((_portable_observation(item) for item in value["observations"]),
                          key=lambda item: core._canonical_json_bytes(item))
    matches = {
        "executable": sorted((_portable_observation(item) for item in value["executable_matches"]),
                             key=lambda item: core._canonical_json_bytes(item)),
        "extension": sorted((_portable_observation(item) for item in value["extension_matches"]),
                            key=lambda item: core._canonical_json_bytes(item)),
        "providers": {family: sorted((_portable_observation(item) for item in value["provider_matches"][family]),
                                     key=lambda item: core._canonical_json_bytes(item))
                      for family in ("dds", "cdr")},
    }
    return {"returncode": value["returncode"], "observations": observations, "matches": matches,
            "stdout_sha256": value["stdout_sha256"], "stderr_sha256": value["stderr_sha256"]}


def _required_runtime_hashes(value: Any) -> dict[str, Any]:
    matches = _portable_runtime(value)["matches"]
    return {"executable": sorted(item["sha256"] for item in matches["executable"]),
            "extension": sorted(item["sha256"] for item in matches["extension"]),
            "providers": {family: sorted(item["sha256"] for item in matches["providers"][family])
                          for family in ("dds", "cdr")}}


def _runtime_projection(value: Any) -> dict[str, Any]:
    """Return the closed, path-free runtime authority published in the bundle."""
    portable = _portable_runtime(value)
    matches = portable["matches"]
    observations = portable["observations"]
    return {"returncode": portable["returncode"],
            "observations_sha256": hashlib.sha256(
                core._canonical_json_bytes(observations)).hexdigest(),
            "observations_count": len(observations),
            "executable_matches": sorted(item["sha256"] for item in matches["executable"]),
            "extension_matches": sorted(item["sha256"] for item in matches["extension"]),
            "provider_matches": {family: sorted(item["sha256"] for item in matches["providers"][family])
                                 for family in ("dds", "cdr")},
            "executable_sha256": matches["executable"][0]["sha256"],
            "extension_sha256": matches["extension"][0]["sha256"],
            "dds_provider_sha256": matches["providers"]["dds"][0]["sha256"],
            "cdr_provider_sha256": matches["providers"]["cdr"][0]["sha256"],
            "stdout_sha256": portable["stdout_sha256"],
            "stderr_sha256": portable["stderr_sha256"]}


def _validate_packaged_inventory(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict) or not isinstance(value.get("records"), list):
        raise ValueError("rollback compile inventory is incomplete")
    if set(value) != {"record_count", "records", "sources", "content_sha256",
                      "registered_records", "other_records", "claim"}:
        raise ValueError("rollback compile inventory has unknown or missing fields")
    records = value["records"]
    if value.get("record_count") != len(records) or len(records) < 2:
        raise ValueError("rollback compile inventory count is inconsistent")
    required = {name: 0 for name in _REQUIRED_SOURCES}
    for item in records:
        if (not isinstance(item, dict) or set(item) != {
                "source", "source_kind", "output", "target", "command_sha256"}
                or not isinstance(item.get("source"), str)):
            raise ValueError("rollback compile inventory record is malformed")
        source = item["source"]
        if source in _FORBIDDEN_SOURCES:
            raise ValueError("rollback compile inventory contains a forbidden trace source")
        if source in required and item.get("target") == _REQUIRED_SOURCES[source][:-4]:
            required[source] += 1
        if not re.fullmatch(r"[0-9a-f]{64}", str(item.get("command_sha256"))):
            raise ValueError("rollback compile command hash is malformed")
    if any(count != 1 for count in required.values()):
        raise ValueError("rollback compile inventory does not bind required sources")
    if records != sorted(records, key=lambda x: (x["source"], x["output"], x["target"] or "")):
        raise ValueError("rollback compile inventory is not canonically ordered")
    if value.get("sources") != sorted({item["source"] for item in records}):
        raise ValueError("rollback compile inventory source set is inconsistent")
    registered = [item for item in records if item["target"] in _REGISTERED_TARGETS]
    other = [item for item in records if item["target"] not in _REGISTERED_TARGETS]
    def subset(items: list[dict[str, Any]]) -> dict[str, Any]:
        return {"record_count": len(items), "content_sha256": hashlib.sha256(
            core._canonical_json_bytes(items)).hexdigest()}
    if (value.get("content_sha256") != hashlib.sha256(
            core._canonical_json_bytes(records)).hexdigest()
            or value.get("registered_records") != subset(registered)
            or value.get("other_records") != subset(other)):
        raise ValueError("rollback compile inventory digest is inconsistent")
    _path_free(value)
    return value


def _reported_hash(item: Any, label: str) -> str:
    if not isinstance(item, dict) or not isinstance(item.get("path"), str):
        raise ValueError(f"{label} image path is missing")
    path = Path(item["path"])
    if not path.is_file() or path.is_symlink():
        raise ValueError(f"{label} image is unavailable")
    digest = _sha256(path)
    if item.get("sha256") != digest:
        raise ValueError(f"{label} image hash does not match bytes")
    return digest


def _write_exclusive(path: Path, data: bytes) -> None:
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                 getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0), 0o600)
    try:
        view = memoryview(data)
        while view:
            written = os.write(fd, view)
            if written <= 0:
                raise OSError("rollback payload write made no progress")
            view = view[written:]
        os.fsync(fd)
    finally:
        os.close(fd)


def _read_immutable_file(path: Path, label: str, *, max_size: int | None = None) -> tuple[bytes, dict[str, Any]]:
    # Do not resolve/open the pathname in one operation.  Pin every directory
    # component first, then open the leaf through its pinned parent.  This is
    # the Python equivalent of RESOLVE_NO_SYMLINKS/openat2 for the platforms
    # supported by this collector (Linux and other POSIX systems with *at()).
    absolute = Path(os.path.abspath(path))
    directory_fds: list[tuple[int, Path, os.stat_result]] = []
    current_fd = os.open(os.sep, os.O_RDONLY | os.O_DIRECTORY |
                         getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0))
    current = Path(os.sep)
    try:
        for component in absolute.parts[1:-1]:
            next_fd = os.open(component, os.O_RDONLY | os.O_DIRECTORY |
                              getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0),
                              dir_fd=current_fd)
            os.close(current_fd)
            current_fd = next_fd
            current /= component
            directory_fds.append((current_fd, current, os.fstat(current_fd)))
        flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_CLOEXEC", 0)
        fd = os.open(absolute.name, flags, dir_fd=current_fd)
    except Exception:
        os.close(current_fd)
        raise
    try:
        before = os.fstat(fd)
        if not stat.S_ISREG(before.st_mode):
            raise ValueError(f"{label} is not a regular file")
        if max_size is not None and before.st_size > max_size:
            raise ValueError(f"{label} exceeds the size limit")
        chunks: list[bytes] = []
        remaining = max_size + 1 if max_size is not None else None
        while True:
            chunk = os.read(fd, min(1024 * 1024, remaining) if remaining is not None else 1024 * 1024)
            if not chunk:
                break
            chunks.append(chunk)
            if remaining is not None:
                remaining -= len(chunk)
                if remaining == 0:
                    raise ValueError(f"{label} exceeds the size limit")
        data = b"".join(chunks)
        after = os.fstat(fd)
        linked = os.stat(absolute, follow_symlinks=False)
        identity = lambda item: (item.st_dev, item.st_ino, item.st_size,
                                 item.st_mtime_ns, item.st_ctime_ns)
        if identity(before) != identity(after) or identity(after) != identity(linked):
            raise ValueError(f"{label} changed while being read")
        return data, {"device": after.st_dev, "inode": after.st_ino, "size": len(data),
                      "sha256": hashlib.sha256(data).hexdigest()}
    finally:
        os.close(fd)
        os.close(current_fd)


def _anchor_binding(anchor_run: Path) -> dict[str, Any]:
    anchor = Path(anchor_run)
    try:
        anchor_info = anchor.lstat()
        anchor_real = anchor.resolve(strict=True)
    except (OSError, RuntimeError) as exc:
        raise ValueError("anchor_run must be a real directory") from exc
    if (stat.S_ISLNK(anchor_info.st_mode) or not stat.S_ISDIR(anchor_info.st_mode)
            or anchor_real != anchor):
        raise ValueError("anchor_run must be a real directory")

    def read_anchor(relative: str, label: str) -> tuple[bytes, dict[str, Any]]:
        path = anchor / relative
        try:
            # _read_immutable_file pins the complete absolute directory chain;
            # retain this explicit containment check so an anchor replacement
            # fails closed rather than becoming a different evidence root.
            path.resolve(strict=True).relative_to(anchor_real)
            data, identity = _read_immutable_file(path, label)
            if anchor.lstat().st_ino != anchor_info.st_ino or anchor.lstat().st_dev != anchor_info.st_dev:
                raise ValueError(f"{label} anchor root changed")
            return data, identity
        except (OSError, RuntimeError, ValueError) as exc:
            if isinstance(exc, ValueError) and str(exc).startswith(label):
                raise
            raise ValueError("anchor v8 evidence is unavailable") from exc

    try:
        manifest_raw, _manifest_identity = read_anchor("manifest.json", "anchor manifest")
        evidence_raw, _evidence_identity = read_anchor(
            "capability/p0a_evidence_manifest.json", "anchor evidence")
        evidence = json.loads(evidence_raw)
        manifest = json.loads(manifest_raw)
        contract_version = evidence.get("registration_contract_version")
        contract_raw, _contract_identity = read_anchor(
            "registration/contract.json", "anchor contract")
        trusted, trusted_raw = core._trusted_p0a_contract(contract_version)
    except (OSError, ValueError, TypeError, json.JSONDecodeError) as exc:
        raise ValueError("anchor v8 evidence is unavailable") from exc
    try:
        validation_raw, _validation_identity = read_anchor("validation.json", "anchor validation")
        validation = json.loads(validation_raw)
        resolution_raw, _resolution_identity = read_anchor(
            "registration/resolution_declarations.json", "anchor resolution")
        resolution = json.loads(resolution_raw)
        trusted_resolution_raw, _trusted_resolution_identity = read_anchor(
            "capability/resolution.json", "anchor trusted resolution")
        trusted_resolution = json.loads(trusted_resolution_raw)
    except (OSError, ValueError, TypeError, json.JSONDecodeError) as exc:
        raise ValueError("anchor v8 evidence is unavailable") from exc
    fresh_validation = core.validate_dir(anchor, profile="p0a")
    nested = validation.get("p0a_evidence", {})
    nested_codes = nested.get("failure_codes", []) if isinstance(nested, dict) else []
    if (core._canonical_json_bytes(validation, newline=True) !=
            core._canonical_json_bytes(fresh_validation, newline=True)
            or contract_version != _ROLLBACK_CONTRACT
            or contract_raw != trusted_raw or manifest.get("run_status") != "completed"
            or manifest.get("candidate_only") is not True
            or manifest.get("metric_availability") != "unsupported"
            or manifest.get("dds_comm_trace", {}).get("status") != "exact_correlated"
            or manifest.get("dds_comm_trace", {}).get("registration_contract_version") != contract_version
            or manifest.get("measurement_kind") != "exact_correlated"
            or validation.get("valid_for_collection") is not True
            or validation.get("valid_for_metric") is not False
            or validation.get("valid_for_comparison") is not False
            or nested.get("valid") is not False or nested_codes != ["P0A_ARTIFACT"]
            or "P0A_DDS_COMM_TRACE" in nested_codes):
        raise ValueError("anchor is not a valid v8 candidate")
    expected = evidence.get("registration_contract_sha256")
    contract_hash = hashlib.sha256(contract_raw).hexdigest()
    if (expected != contract_hash or evidence.get("run_id") != manifest.get("run_id")
            or evidence.get("registration_contract_version") != contract_version):
        raise ValueError("anchor contract hash is stale")
    declarations = resolution.get("declarations") if isinstance(resolution, dict) else None
    if (resolution.get("schema") != "mw_perf.p0a.resolution_declarations.v1"
            or resolution.get("registration_contract_version") != _ROLLBACK_CONTRACT
            or resolution.get("run_id") != manifest.get("run_id")
            or not isinstance(declarations, list)
            or resolution.get("declaration_sha256") != hashlib.sha256(
                core._canonical_json_bytes(declarations)).hexdigest()):
        raise ValueError("anchor production build identity is invalid")
    if not isinstance(trusted_resolution, dict):
        raise ValueError("anchor trusted resolution is malformed")
    trusted_declarations = trusted_resolution.get("declarations")
    declaration_fields = {"role", "name", "source", "expected_path", "sha256",
                          "expected_device", "expected_inode", "expected_size"}
    if (trusted_resolution.get("schema") != "mw_perf.p0a.resolution.v2"
            or trusted_resolution.get("run_id") != manifest.get("run_id")
            or trusted_resolution.get("resolved") is not True
            or trusted_resolution.get("conclusion") != "pass"
            or declarations != trusted_declarations
            or resolution.get("declaration_sha256") != trusted_resolution.get("declaration_sha256")
            or len(declarations) != 2
            or any(set(item) != declaration_fields for item in declarations)
            or declarations != sorted(declarations, key=lambda item: tuple(
                str(item[field]).encode("utf-8")
                for field in ("role", "name", "source", "expected_path", "sha256")))
            or len({(item["role"], item["name"]) for item in declarations}) != 2
            or len({(item["source"], item["expected_path"]) for item in declarations}) != 2):
        raise ValueError("anchor resolution declarations are not exact and trusted")
    selected = [{name: item.get(name) for name in ("role", "name", "source", "sha256")}
                for item in declarations if isinstance(item, dict)
                and (item.get("role"), item.get("source")) in {("sut", "exe"), ("extension", "maps")}]
    selected.sort(key=lambda item: tuple(str(item[name]) for name in ("role", "name", "source", "sha256")))
    if ({(item["role"], item["source"]) for item in selected} !=
            {("sut", "exe"), ("extension", "maps")}
            or len(selected) != 2 or any(not re.fullmatch(r"[0-9a-f]{64}", str(item["sha256"]))
                                         for item in selected)):
        raise ValueError("anchor production images are invalid")
    production_hash = hashlib.sha256(core._canonical_json_bytes({
        "schema": "mw_perf.p0a.fault_build_identity.v1", "runtime_images": selected})).hexdigest()
    return {"run_id": manifest.get("run_id"),
            "root_manifest_sha256": hashlib.sha256(manifest_raw).hexdigest(),
            "evidence_manifest_sha256": hashlib.sha256(evidence_raw).hexdigest(),
            "registration_contract": {"id": contract_version,
                                       "sha256": hashlib.sha256(contract_raw).hexdigest()},
            "production_build_identity_sha256": production_hash}


def _revalidate_packaging(result: Mapping[str, Any], anchor_run: Path, logs: Path) -> tuple[dict[str, Any], dict[str, bytes]]:
    """Rebuild all package authority from runner-provided locations only."""
    work = Path(result.get("work_root", "")).resolve(strict=True)
    build = Path(result.get("build_dir", "")).resolve(strict=True)
    if build != work / "build":
        raise ValueError("rollback build directory is not the canonical work build")
    source_hint = ((result.get("source_identity") or {}).get("before") or {}).get("root")
    anchor_cache_hint = ((result.get("anchor_cache_identity") or {}).get("path"))
    anchor_cache_path = Path(anchor_cache_hint).resolve(strict=True) if anchor_cache_hint else (
        Path(anchor_run).resolve(strict=True) / "CMakeCache.txt")
    anchor_cache = parse_cmake_cache(anchor_cache_path)
    source = Path(source_hint).resolve(strict=True) if source_hint else Path(
        anchor_cache.get("CMAKE_HOME_DIRECTORY", {}).get("value", "")).resolve(strict=True)
    _cache_home(anchor_cache, source, "anchor")
    source_before = source_identity(source)
    fresh_cache = parse_cmake_cache(build / "CMakeCache.txt")
    claimed_dependency = result.get("dependency_source_control")
    _portable_dependency_source(claimed_dependency)
    if not isinstance(claimed_dependency, dict):
        raise ValueError("rollback dependency source control is incomplete")
    anchor_base, anchor_dependencies = _anchor_dependency_sources(anchor_cache)
    try:
        claimed_anchor_base = Path(claimed_dependency["anchor_base_dir"]).resolve(strict=True)
        fresh_base = Path(claimed_dependency["fresh_base_dir"]).resolve(strict=True)
        source_cache = Path(claimed_dependency["staged_source_cache"]).resolve(strict=True)
    except (OSError, RuntimeError, KeyError, TypeError) as exc:
        raise ValueError("rollback dependency cache paths are invalid") from exc
    if (claimed_anchor_base != anchor_base or fresh_base != build / "_deps"
            or source_cache != work / "source-cache"):
        raise ValueError("rollback dependency cache roots differ from independent validation")
    dependencies = claimed_dependency["dependencies"]
    if [(item["name"], item["override_variable"], item["cache_bindings"],
         item["anchor_source"])
            for item in dependencies] != [(item["name"], item["override_variable"],
                                           item["cache_bindings"], item["anchor_source"])
                                          for item in anchor_dependencies]:
        raise ValueError("claimed dependency inventory differs from anchor cache")
    for claimed, current in zip(dependencies, anchor_dependencies):
        anchor_identity = current["identity"]["anchor_before_staging"]
        expected_staged = (source_cache / (str(claimed["name"]) + "-src")).resolve(strict=True)
        if (Path(str(claimed["staged_source"])) != expected_staged
                or expected_staged.parent != source_cache):
            raise ValueError("claimed staged source differs from canonical source cache")
        staged_identity = _external_source_identity(expected_staged)
        baseline = claimed["identity"]["anchor_before_staging"]
        if (_dependency_content_identity(anchor_identity) != _dependency_content_identity(baseline)
                or _dependency_content_identity(staged_identity) !=
                _dependency_content_identity(baseline)):
            raise ValueError("claimed dependency source identity differs from independent validation")
    _require_dependency_cache(fresh_cache, fresh_base, source_cache, dependencies, anchor_base)
    _cache_home(fresh_cache, source, "fresh")
    source_current = source_identity(source)
    _require_same_source_identity(source_before, source_current)
    anchor_selected, fresh_selected = selected_options(anchor_cache), selected_options(fresh_cache)
    if anchor_selected.get("IBMW_ENABLE_COMM_TRACE") != "ON":
        raise ValueError("anchor IBMW_ENABLE_COMM_TRACE must be ON")
    if any(anchor_selected.get(option) != "ON" for option in _REQUIRED_ON):
        raise ValueError("anchor required product controls are not ON")
    expected_product = dict(anchor_selected)
    expected_product["IBMW_ENABLE_COMM_TRACE"] = "OFF"
    expected_product["IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE"] = "OFF"
    _compare_controls(expected_product, fresh_selected, "product")
    anchor_toolchain, fresh_toolchain = toolchain_options(anchor_cache), toolchain_options(fresh_cache)
    _compare_controls(anchor_toolchain, fresh_toolchain, "toolchain", compiler_aliases=True)
    tools = {"cmake": _tool_identity(fresh_cache.get("CMAKE_COMMAND", {}).get("value", "")),
             "active_toolchain": _active_tool_identities(fresh_toolchain)}
    anchor_tools = {"cmake": _tool_identity(anchor_cache.get("CMAKE_COMMAND", {}).get("value", "")),
                    "active_toolchain": _active_tool_identities(anchor_toolchain)}
    _require_same_tool_identity(anchor_tools["cmake"], tools["cmake"], "CMAKE_COMMAND")
    if fresh_cache.get("CMAKE_EXPORT_COMPILE_COMMANDS", {}).get("value") != "ON":
        raise ValueError("fresh CMAKE_EXPORT_COMPILE_COMMANDS is not ON")
    if set(anchor_tools["active_toolchain"]) != set(tools["active_toolchain"]):
        raise ValueError("fresh executable toolchain controls differ from anchor")
    for name in tools["active_toolchain"]:
        _require_same_tool_identity(anchor_tools["active_toolchain"][name], tools["active_toolchain"][name], name)
    inventory = parse_compile_inventory(
        build / "compile_commands.json", source, build, anchor_base,
        {item["name"]: item["staged_source"] for item in dependencies})
    extension = _regular_unique(build, "libibmw_dds_transport.so")
    candidate = _regular_unique(build, "ibmw_dds_trace_off_business_roundtrip")
    nm = fresh_cache.get("CMAKE_NM", {}).get("value") or shutil.which("nm")
    readelf = fresh_cache.get("CMAKE_READELF", {}).get("value") or shutil.which("readelf")
    if not nm or not readelf:
        raise ValueError("nm and readelf are required")
    audit_logs = logs / "packaging-audit"; audit_logs.mkdir(mode=0o700, parents=True, exist_ok=True)
    extension_bytes, extension_open = _read_immutable_file(extension, "extension image")
    candidate_bytes, candidate_open = _read_immutable_file(candidate, "executable image")
    image_bytes = {"extension": extension_bytes, "executable": candidate_bytes}
    image_open = {"extension": extension_open, "executable": candidate_open}
    images = {"extension": _audit_image(extension, audit_logs, nm, readelf,
                                         re.compile(r"DdsTransportDriver|(?:init|Init).*extension", re.I)),
              "executable": _audit_image(candidate, audit_logs, nm, readelf,
                                           re.compile(r"(?:^|\s)main(?:\(|$)"))}
    provider = _provider_identity(fresh_cache, readelf)
    link_closure = capture_link_closure(build, anchor_cache_path.parent, images, provider)
    if result.get("link_closure") != link_closure:
        raise ValueError("claimed link closure differs from independent validation")
    runtime_logs = logs / "packaging-runtime"
    runtime_logs.mkdir(mode=0o700, parents=True, exist_ok=True)
    runtime = _execute_candidate(candidate, extension, build, provider, runtime_logs)
    # A path-based run must be bound to the exact bytes that will be published.
    for name, path in (("extension", extension), ("executable", candidate)):
        after = _file_identity(path)
        if (path.read_bytes() != image_bytes[name]
                or any(after[key] != image_open[name][key]
                       for key in ("device", "inode", "size", "sha256"))
                or after["sha256"] != images[name]["sha256"]):
            raise ValueError("rollback image changed after independent runtime validation")
    provider_after = _provider_identity(fresh_cache, readelf)
    if provider_after != provider:
        raise ValueError("rollback provider changed after independent runtime validation")
    if capture_link_closure(build, anchor_cache_path.parent, images, provider) != link_closure:
        raise ValueError("rollback link plan or input changed after independent validation")
    source_after = source_identity(source)
    _require_same_source_identity(source_before, source_after)
    claimed_source = result.get("source_identity")
    if (not isinstance(claimed_source, dict) or claimed_source.get("before") != source_before
            or claimed_source.get("after") != source_after or source_current != source_after):
        raise ValueError("claimed source identity differs from independent validation")
    claimed_product = result.get("product_options")
    fresh_product = {"anchor": anchor_selected, "fresh": fresh_selected,
                     "delta": option_delta(anchor_selected, fresh_selected)}
    if claimed_product != fresh_product or result.get("delta") != fresh_product["delta"]:
        raise ValueError("claimed product controls differ from independent validation")
    if result.get("compile_inventory") != inventory:
        raise ValueError("claimed compile inventory differs from independent validation")
    claimed_tools = result.get("tools")
    if claimed_tools != tools:
        raise ValueError("claimed tool identity differs from independent validation")
    claimed_images = result.get("images")
    if not isinstance(claimed_images, dict) or any(claimed_images.get(name) != images[name]
                                                   for name in _IMAGE_NAMES):
        raise ValueError("claimed image audit differs from independent validation")
    claimed_provider = result.get("provider_identity")
    if claimed_provider != provider:
        raise ValueError("claimed provider identity differs from independent validation")
    claimed = result.get("runtime")
    if not isinstance(claimed, dict) or claimed.get("returncode") != runtime.get("returncode"):
        raise ValueError("rollback runtime differs from independent validation")
    claimed_runtime, fresh_runtime = _portable_runtime(claimed), _portable_runtime(runtime)
    if claimed_runtime["returncode"] != fresh_runtime["returncode"]:
        raise ValueError("claimed runtime result differs from independent validation")
    for family in ("extension_matches", "executable_matches"):
        expected_name = "extension" if family == "extension_matches" else "executable"
        if (not claimed.get(family)
                or any(item.get("sha256") != images[expected_name]["sha256"]
                       for item in claimed[family])):
            raise ValueError("rollback runtime image identity mismatch")
    claimed_provider_matches = claimed.get("provider_matches", {})
    for family in ("dds", "cdr"):
        if (not claimed_provider_matches.get(family)
                or any(item.get("sha256") != provider[family]["sha256"]
                       for item in claimed_provider_matches[family])):
            raise ValueError("claimed runtime provider identity mismatch")
    if _required_runtime_hashes(claimed) != _required_runtime_hashes(runtime):
        raise ValueError("claimed runtime required identity differs from independent validation")
    authority = dict(result)
    packaging_log_hashes = {path.relative_to(logs).as_posix(): _sha256(path)
                            for path in sorted(logs.rglob("*")) if path.is_file()}
    authority.update({"source_identity": {"before": source_before, "after": source_after},
                      "product_options": fresh_product,
                      "delta": option_delta(anchor_selected, fresh_selected), "compile_inventory": inventory,
                      "link_closure": link_closure,
                      "images": images, "provider_identity": provider, "runtime": runtime, "tools": tools,
                      "logs": packaging_log_hashes,
                      "build_dir": str(build), "work_root": str(work), "evidence_control": {
                          "name": "CMAKE_EXPORT_COMPILE_COMMANDS", "forced": "ON",
                          "fresh": fresh_cache.get("CMAKE_EXPORT_COMPILE_COMMANDS", {}).get("value")},
                      "dependency_source_control": claimed_dependency})
    return authority, image_bytes


def build_trace_off_rollback_companion(result: dict, anchor_run: Path, payload_root: Path,
                                       object_store: Path, campaign_id: str,
                                       companion_id: str) -> dict[str, Any]:
    """Package an already completed trace-off runner result, without building."""
    if not isinstance(result, dict):
        raise ValueError("rollback result must be an object")
    anchor = _anchor_binding(Path(anchor_run))
    work_hint = Path(result.get("work_root", ""))
    packaging_logs = work_hint / ("packaging-" + uuid.uuid4().hex)
    packaging_logs.mkdir(mode=0o700, parents=False)
    result, authoritative_image_bytes = _revalidate_packaging(result, Path(anchor_run), packaging_logs)
    source = result.get("source_identity")
    if (not isinstance(source, dict) or source.get("before") != source.get("after")
            or not isinstance(source.get("before"), dict)):
        raise ValueError("rollback source identity drift or missing identity")
    product = result.get("product_options")
    delta = result.get("delta", product.get("delta") if isinstance(product, dict) else None)
    if not isinstance(product, dict) or not isinstance(delta, dict):
        raise ValueError("rollback product evidence is incomplete")
    allowed_delta = {"IBMW_ENABLE_COMM_TRACE"}
    candidate_delta = delta.get("IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE")
    if candidate_delta is not None:
        if candidate_delta != {"anchor": "ON", "fresh": "OFF"}:
            raise ValueError("rollback managed exact candidate delta is not ON-to-OFF")
        allowed_delta.add("IBMW_BUILD_DDS_MANAGED_EXACT_CANDIDATE")
    if set(delta) != allowed_delta:
        raise ValueError(
            "rollback product delta is not limited to communication trace and the separate "
            "managed-candidate build control")
    if delta["IBMW_ENABLE_COMM_TRACE"].get("fresh") != "OFF":
        raise ValueError("rollback trace option is not OFF")
    if (set(product) != {"anchor", "fresh", "delta"}
            or not isinstance(product["anchor"], dict) or not isinstance(product["fresh"], dict)
            or product["delta"] != delta or option_delta(product["anchor"], product["fresh"]) != delta
            or delta["IBMW_ENABLE_COMM_TRACE"].get("anchor") != "ON"):
        raise ValueError(
            "rollback product evidence does not bind the communication-trace delta and optional "
            "managed-candidate target-build delta")
    runtime = result.get("runtime")
    if (not isinstance(runtime, dict) or runtime.get("returncode") != 0
            or not runtime.get("executable_matches") or not runtime.get("extension_matches")):
        raise ValueError("rollback runtime evidence is incomplete")
    images = result.get("images")
    if not isinstance(images, dict):
        raise ValueError("rollback images are missing")
    image_hashes = {name: _reported_hash(images.get(name), name) for name in _IMAGE_NAMES}
    for name, matches in (("extension", runtime["extension_matches"]),
                          ("executable", runtime["executable_matches"])):
        if any(item.get("sha256") != image_hashes[name] for item in matches):
            raise ValueError("rollback runtime image identity mismatch")
    for name in _IMAGE_NAMES:
        item = images[name]
        audits = item.get("audits") if isinstance(item, dict) else None
        if (not isinstance(audits, dict) or set(audits) != {"nm_all", "nm_dynamic"} or any(
                not isinstance(a.get("symbol_count"), int) or a["symbol_count"] <= 0
                for a in audits.values() if isinstance(a, dict))
                or any(not isinstance(a, dict) for a in audits.values())):
            raise ValueError("rollback symbol audit is empty")
    provider = result.get("provider_identity")
    if not isinstance(provider, dict) or any(not isinstance(provider.get(f), dict)
                                              for f in ("dds", "cdr")):
        raise ValueError("rollback provider identity is incomplete")
    matches = runtime.get("provider_matches")
    if not isinstance(matches, dict) or any(not matches.get(f) for f in ("dds", "cdr")):
        raise ValueError("rollback provider runtime matching is incomplete")
    for family in ("dds", "cdr"):
        expected = provider[family].get("sha256")
        if not expected or any(item.get("sha256") != expected for item in matches[family]):
            raise ValueError("rollback provider runtime identity mismatch")
    compile_inventory = _validate_packaged_inventory(result.get("compile_inventory"))
    link_closure = _validate_link_closure(result.get("link_closure"))
    _validate_link_inventory_binding(link_closure, compile_inventory)
    link_summary = _link_closure_summary(link_closure)
    for plan in link_closure["plans"]:
        if (plan["output"]["sha256"] != image_hashes[plan["image"]]
                or plan["output"]["size"] != len(authoritative_image_bytes[plan["image"]])):
            raise ValueError("rollback link closure output image mismatch")
    normalized_source = _identity_without_paths(source["before"])
    tools = result.get("tools")
    if (not isinstance(tools, dict) or set(tools) != {"cmake", "active_toolchain"}
            or not isinstance(tools.get("active_toolchain"), dict)):
        raise ValueError("rollback tool identities are incomplete")
    normalized_tools = {"cmake": _portable_tool(tools.get("cmake"), "cmake")}
    normalized_tools["active_toolchain"] = {
        name: _portable_tool(identity, name)
        for name, identity in sorted(tools["active_toolchain"].items())}
    _validate_link_tool_binding(link_closure, normalized_tools["active_toolchain"])
    normalized_provider = {family: _portable_provider(provider[family], family)
                           for family in ("dds", "cdr")}
    normalized_dependency = _portable_dependency_source(result.get("dependency_source_control"))
    normalized_runtime = _runtime_projection(runtime)
    build_identity = _path_free({"schema": "mw_perf.rollback_build_identity.v1",
        "production_build_identity_sha256": anchor["production_build_identity_sha256"],
        "source": normalized_source, "product": {"delta": delta},
        "tools": normalized_tools, "providers": normalized_provider,
        "dependency_source_control": normalized_dependency,
        "compile": result.get("compile_inventory"), "link_closure": link_closure, "images": {
            name: {"sha256": image_hashes[name], "build_id": images[name].get("build_id"),
                   "audits": images[name].get("audits")} for name in _IMAGE_NAMES},
        "runtime": normalized_runtime})
    build_hash = hashlib.sha256(core._canonical_json_bytes(build_identity)).hexdigest()
    selected_delta_claim = "communication trace ON-to-OFF"
    if candidate_delta is not None:
        selected_delta_claim += " and managed-candidate build ON-to-OFF"
    report = {"schema": _ROLLBACK_SCHEMA, "classification": {
                  "classification": "candidate", "metric_availability": "unsupported",
                  "valid_for_comparison": False, "failure_code": "P0A_ARTIFACT"},
              "claim": ("trace-off rollback candidate matches selected controls, registered-source "
                        "inventory, final-link closure, audited images, providers, and one runtime "
                        f"roundtrip under selected-control deltas ({selected_delta_claim}); the "
                        "managed-candidate build option controls only construction, audit, and "
                        "installation of a separate target, while audited rollback product targets "
                        "retain independent compile and final-link closure"),
              "limitations": ["candidate-only rollback evidence; no whole-product equivalence or performance claim",
                              "FetchContent dependencies were configured from identified, private source-only copies; their compilation is irrelevant to audited output links and is not image-consumption proof"],
              "anchor": anchor, "source": normalized_source, "product_delta": _path_free(delta),
              "evidence_control": _path_free(result.get("evidence_control", {})),
              "dependency_source_control": normalized_dependency,
              "compile_out": {name: compile_inventory.get(name) for name in (
                  "record_count", "content_sha256", "registered_records", "other_records", "claim")},
              "link_closure": link_summary,
              "image_summaries": {name: {"sha256": image_hashes[name], "build_id": images[name].get("build_id")}
                                  for name in _IMAGE_NAMES},
              "symbol_summary": {name: {audit: images[name]["audits"][audit]["symbol_count"]
                                         for audit in ("nm_all", "nm_dynamic")} for name in _IMAGE_NAMES},
              "provider_summary": normalized_provider,
              "runtime_summary": normalized_runtime,
              "rollback_build_identity_sha256": build_hash}
    report_schema = json.loads((Path(__file__).parent / "schemas/rollback-trace-off-result.schema.v2.json").read_bytes())
    if core._schema_errors(report, report_schema):
        raise ValueError("rollback report schema validation failed")
    if Path(payload_root).exists():
        raise ValueError("payload_root must not already exist")
    root = Path(payload_root); root.mkdir(mode=0o700, parents=False)
    try:
        files: dict[str, bytes] = {}
        build_plan = {"schema": "mw_perf.rollback_build_plan.v1",
                      "build_identity_sha256": build_hash,
                      "anchor": {"run_id": anchor["run_id"],
                                  "root_manifest_sha256": anchor["root_manifest_sha256"],
                                  "evidence_manifest_sha256": anchor["evidence_manifest_sha256"],
                                  "contract_sha256": anchor["registration_contract"]["sha256"]},
                      "compile": {"records": compile_inventory["records"],
                                  "record_count": compile_inventory["record_count"],
                                  "content_sha256": compile_inventory["content_sha256"]},
                      "link_closure": link_summary,
                      "logs": _path_free(result.get("logs", {}))}
        _validate_build_plan(build_plan)
        for relative, value in (("rollback_trace_off.json", report),
                                ("build_plan.json", build_plan),
                                ("compile_source_inventory.json", _path_free(result.get("compile_inventory"))),
                                 ("link_closure.json", link_closure),
                                ("build_identity.json", build_identity),
                                ("symbol_audit.json", _path_free(report["symbol_summary"])),
                                ("runtime_resolution.json", report["runtime_summary"]),
                                ("provider_identity.json", normalized_provider)):
            files[relative] = core._canonical_json_bytes(value, newline=True)
        for name, relative in _IMAGE_NAMES.items():
            files[relative] = authoritative_image_bytes[name]
            if hashlib.sha256(files[relative]).hexdigest() != image_hashes[name]:
                raise ValueError("rollback image changed during packaging")
        expected_files = {name: {"sha256": hashlib.sha256(files[name]).hexdigest(),
                                 "size": len(files[name]), "role": role}
                          for name, role in _ROLLBACK_PAYLOAD_ROLES.items()}
        manifest = {"schema": _ROLLBACK_MANIFEST_SCHEMA, "expected_files": expected_files}
        manifest_schema = json.loads((Path(__file__).parent / "schemas/rollback-evidence-manifest.schema.v1.json").read_bytes())
        if core._schema_errors(manifest, manifest_schema):
            raise ValueError("rollback evidence manifest schema validation failed")
        files["rollback_evidence_manifest.json"] = core._canonical_json_bytes(manifest, newline=True)
        for relative, data in files.items():
            _write_exclusive(root / relative, data)
        snapshot = capture_payload_snapshot(root)
        metadata = {"campaign_id": campaign_id, "role": "rollback_trace_off",
                    "companion_id": companion_id,
                    "run_id": "rollback-" + hashlib.sha256((campaign_id + "\0" + companion_id + "\0" + build_hash).encode()).hexdigest(),
                    "evidence_contract": anchor["registration_contract"],
                    "root_manifest_sha256": hashlib.sha256(files["rollback_trace_off.json"]).hexdigest(),
                    "evidence_manifest_sha256": hashlib.sha256(files["rollback_evidence_manifest.json"]).hexdigest(),
                    "build_identity_sha256": build_hash}
        descriptor = build_companion_object_from_snapshot(snapshot, Path(object_store), metadata)
        object_path = Path(object_store) / "sha256" / descriptor["object_sha256"][:2] / (descriptor["object_sha256"] + ".tar")
        if not validate_companion_object(object_path, descriptor)["valid"]:
            raise ValueError("rollback companion failed descriptor validation")
        if not validate_trace_off_rollback_companion(object_path, descriptor)["valid"]:
            raise ValueError("rollback companion failed semantic validation")
        shutil.rmtree(packaging_logs, ignore_errors=True)
        return {"report": report, "descriptor": descriptor}
    except BaseException:
        shutil.rmtree(root, ignore_errors=True)
        shutil.rmtree(packaging_logs, ignore_errors=True)
        raise


def _strict_json_object(data: bytes, label: str) -> dict[str, Any]:
    def pairs(items: list[tuple[str, Any]]) -> dict[str, Any]:
        value: dict[str, Any] = {}
        for key, item in items:
            if key in value:
                raise ValueError(f"{label} contains a duplicate key")
            value[key] = item
        return value
    value = json.loads(data.decode("utf-8"), object_pairs_hook=pairs)
    if not isinstance(value, dict) or data != core._canonical_json_bytes(value, newline=True):
        raise ValueError(f"{label} is not a canonical object")
    return value


def validate_trace_off_rollback_companion(
        object_path: Path, expected_descriptor: dict[str, Any] | None = None) -> dict[str, Any]:
    """Validate rollback-specific semantics directly from companion tar bytes."""
    if expected_descriptor is None:
        return {"valid": False, "failure_codes": ["ROLLBACK_EXPECTED_DESCRIPTOR"],
                "descriptor": None}
    generic = validate_companion_object(Path(object_path), expected_descriptor)
    codes = set(generic.get("failure_codes", []))
    if codes:
        return {"valid": False, "failure_codes": sorted(codes), "descriptor": generic.get("descriptor")}
    payloads: dict[str, bytes] = {}
    anchor_binding: dict[str, Any] = {}
    try:
        raw, _object_identity = _read_immutable_file(
            Path(object_path), "rollback companion", max_size=256 * 1024 * 1024)
        digest = hashlib.sha256(raw).hexdigest()
        if (expected_descriptor is not None
                and expected_descriptor.get("object_sha256") != digest):
            raise ValueError("rollback companion object hash differs from descriptor")
        with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as archive:
            for member in archive:
                if member.isreg() and member.name.startswith("payload/"):
                    stream = archive.extractfile(member)
                    if stream is None:
                        raise ValueError("unreadable payload")
                    payloads[member.name.removeprefix("payload/")] = stream.read()
        expected_set = set(_ROLLBACK_PAYLOAD_ROLES) | {"rollback_evidence_manifest.json"}
        if set(payloads) != expected_set:
            raise ValueError("rollback payload member set is not exact")
        manifest = _strict_json_object(payloads["rollback_evidence_manifest.json"], "rollback manifest")
        schema = json.loads((Path(__file__).parent / "schemas/rollback-evidence-manifest.schema.v1.json").read_bytes())
        if core._schema_errors(manifest, schema):
            raise ValueError("rollback manifest schema is invalid")
        expected_files = manifest.get("expected_files")
        if not isinstance(expected_files, dict) or set(expected_files) != set(_ROLLBACK_PAYLOAD_ROLES):
            raise ValueError("rollback manifest member set is not exact")
        for relative, role in _ROLLBACK_PAYLOAD_ROLES.items():
            entry, data = expected_files[relative], payloads[relative]
            if (not isinstance(entry, dict) or entry.get("role") != role
                    or entry.get("size") != len(data)
                    or entry.get("sha256") != hashlib.sha256(data).hexdigest()):
                raise ValueError("rollback manifest file binding is invalid")
        report = _strict_json_object(payloads["rollback_trace_off.json"], "rollback report")
        report_schemas: dict[str, str] = {
            "mw_perf.rollback_trace_off.v1": "rollback-trace-off-result.schema.v1.json",
            "mw_perf.rollback_trace_off.v2": "rollback-trace-off-result.schema.v2.json",
        }
        report_schema_id = report.get("schema")
        schema_name = report_schemas.get(report_schema_id) if isinstance(
            report_schema_id, str) else None
        if schema_name is None:
            raise ValueError("rollback report schema ID is unsupported")
        report_schema = json.loads((Path(__file__).parent / "schemas" / schema_name).read_bytes())
        if core._schema_errors(report, report_schema):
            raise ValueError("rollback report schema is invalid")
        if report.get("schema") == "mw_perf.rollback_trace_off.v2":
            _validate_v2_dependency_summary(report.get("dependency_source_control"))
        build_identity = _strict_json_object(payloads["build_identity.json"], "build identity")
        build_plan = _strict_json_object(payloads["build_plan.json"], "build plan")
        _validate_build_plan(build_plan)
        inventory = _strict_json_object(payloads["compile_source_inventory.json"], "compile inventory")
        link_closure = _strict_json_object(payloads["link_closure.json"], "link closure")
        providers = _strict_json_object(payloads["provider_identity.json"], "provider identity")
        runtime = _strict_json_object(payloads["runtime_resolution.json"], "runtime resolution")
        symbols = _strict_json_object(payloads["symbol_audit.json"], "symbol audit")
        _path_free(report); _path_free(build_identity); _path_free(inventory); _path_free(link_closure)
        _path_free(providers); _path_free(runtime); _path_free(symbols)
        _validate_packaged_inventory(inventory)
        _validate_link_closure(link_closure)
        _validate_link_inventory_binding(link_closure, inventory)
        _validate_link_tool_binding(link_closure, build_identity.get("tools", {}).get(
            "active_toolchain"))
        runtime_schema = json.loads((Path(__file__).parent / "schemas/rollback-runtime-resolution.schema.v1.json").read_bytes())
        if core._schema_errors(runtime, runtime_schema):
            raise ValueError("rollback runtime resolution schema is invalid")
        archive_descriptor = generic.get("descriptor")
        if not isinstance(archive_descriptor, dict):
            raise ValueError("rollback archive descriptor is unavailable")
        identity_product = build_identity.get("product")
        if (not isinstance(identity_product, dict)
                or report.get("product_delta") != identity_product.get("delta")):
            raise ValueError("rollback product delta binding is inconsistent")
        build_hash = hashlib.sha256(core._canonical_json_bytes(build_identity)).hexdigest()
        plan_compile = build_plan.get("compile", {})
        expected_plan_records = inventory["records"]
        declared_build_hash = build_identity.get("build_identity_sha256")
        if (report.get("rollback_build_identity_sha256") != build_hash
                or build_plan.get("build_identity_sha256") != build_hash
                or (declared_build_hash is not None and declared_build_hash != build_hash)
                or archive_descriptor.get("build_identity_sha256") != build_hash
                or plan_compile.get("records") != expected_plan_records
                or plan_compile.get("record_count") != inventory.get("record_count")
                or plan_compile.get("content_sha256") != inventory.get("content_sha256")
                or build_identity.get("compile") != inventory
                or build_identity.get("link_closure") != link_closure
                or build_plan.get("link_closure") != _link_closure_summary(link_closure)
                or build_identity.get("source") != report.get("source")
                or build_identity.get("providers") != providers
                or build_identity.get("dependency_source_control") !=
                   report.get("dependency_source_control")):
            raise ValueError("rollback build identity hash is invalid")
        _trusted_contract, trusted_raw = core._trusted_p0a_contract(_ROLLBACK_CONTRACT)
        trusted_hash = hashlib.sha256(trusted_raw).hexdigest()
        trusted_binding = {"id": _ROLLBACK_CONTRACT, "sha256": trusted_hash}
        anchor = report.get("anchor", {})
        plan_anchor = build_plan.get("anchor", {})
        if (anchor.get("registration_contract") != trusted_binding
                or plan_anchor.get("contract_sha256") != trusted_hash
                or archive_descriptor.get("evidence_contract") != trusted_binding):
            raise ValueError("rollback anchor contract binding is invalid")
        anchor_binding = {
            "run_id": anchor.get("run_id"),
            "root_manifest_sha256": anchor.get("root_manifest_sha256"),
            "evidence_manifest_sha256": anchor.get("evidence_manifest_sha256"),
            "evidence_contract": anchor.get("registration_contract"),
            "build_identity_sha256": anchor.get("production_build_identity_sha256"),
        }
        if (plan_anchor.get("run_id") != anchor.get("run_id")
                or plan_anchor.get("root_manifest_sha256") != anchor.get("root_manifest_sha256")
                or plan_anchor.get("evidence_manifest_sha256") != anchor.get("evidence_manifest_sha256")):
            raise ValueError("rollback build plan anchor binding is invalid")
        if (archive_descriptor.get("root_manifest_sha256") !=
                hashlib.sha256(payloads["rollback_trace_off.json"]).hexdigest()
                or archive_descriptor.get("evidence_manifest_sha256") !=
                hashlib.sha256(payloads["rollback_evidence_manifest.json"]).hexdigest()):
            raise ValueError("rollback descriptor archive binding is invalid")
        image_hashes = {name: hashlib.sha256(payloads[relative]).hexdigest()
                        for name, relative in _IMAGE_NAMES.items()}
        if any(report["image_summaries"][name].get("sha256") != image_hashes[name]
               for name in _IMAGE_NAMES):
            raise ValueError("rollback report image binding is invalid")
        if report.get("link_closure") != _link_closure_summary(link_closure):
            raise ValueError("rollback report link closure summary is inconsistent")
        for plan in link_closure["plans"]:
            if (plan["output"]["sha256"] != image_hashes[plan["image"]]
                    or plan["output"]["size"] != len(payloads[_IMAGE_NAMES[plan["image"]]])):
                raise ValueError("rollback link closure output binding is invalid")
        for name in _IMAGE_NAMES:
            identity_image = build_identity.get("images", {}).get(name, {})
            if (identity_image.get("sha256") != image_hashes[name]
                    or identity_image.get("build_id") != report["image_summaries"][name].get("build_id")
                    or {audit: value.get("symbol_count")
                        for audit, value in identity_image.get("audits", {}).items()} != symbols.get(name)):
                raise ValueError("rollback build image semantics are inconsistent")
        expected_compile = {name: inventory.get(name) for name in (
            "record_count", "content_sha256", "registered_records", "other_records", "claim")}
        if report.get("compile_out") != expected_compile:
            raise ValueError("rollback report compile summary is inconsistent")
        if symbols != report.get("symbol_summary") or providers != report.get("provider_summary"):
            raise ValueError("rollback report evidence summaries are inconsistent")
        if (runtime != report.get("runtime_summary")
                or runtime != build_identity.get("runtime")):
            raise ValueError("rollback report runtime summary is inconsistent")
        if (runtime.get("executable_sha256") != image_hashes["executable"]
                or runtime.get("extension_sha256") != image_hashes["extension"]
                or runtime.get("dds_provider_sha256") != providers.get("dds", {}).get("sha256")
                or runtime.get("cdr_provider_sha256") != providers.get("cdr", {}).get("sha256")
                or set(runtime.get("executable_matches", [])) != {image_hashes["executable"]}
                or set(runtime.get("extension_matches", [])) != {image_hashes["extension"]}
                or set(runtime.get("provider_matches", {}).get("dds", [])) != {
                    providers.get("dds", {}).get("sha256")}
                or set(runtime.get("provider_matches", {}).get("cdr", [])) != {
                    providers.get("cdr", {}).get("sha256")}):
            raise ValueError("rollback runtime byte binding is invalid")
    except (OSError, tarfile.TarError, UnicodeError, json.JSONDecodeError, KeyError,
            TypeError, ValueError):
        codes.add("ROLLBACK_OBJECT_SEMANTICS")
    return {"valid": not codes, "failure_codes": sorted(codes),
            "descriptor": generic.get("descriptor"), "anchor": anchor_binding}
