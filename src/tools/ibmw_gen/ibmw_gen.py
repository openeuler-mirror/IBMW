#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# ibmw-gen: IBMW project code generator.
# Generates a complete, buildable IBMW project from a single .ibmw.yaml file.

"""IBMW project code generator — main entry point."""

import argparse
import os
import sys
import textwrap

# When installed, generators live in lib/ibmw/ibmw_gen/; when run from source,
# they live in the same directory as this script.
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_INSTALLED_LIB = os.path.join(
    os.path.dirname(os.path.dirname(_SCRIPT_DIR)), "lib", "ibmw", "ibmw_gen"
)
if os.path.isdir(os.path.join(_INSTALLED_LIB, "generators")):
    sys.path.insert(0, _INSTALLED_LIB)
else:
    sys.path.insert(0, _SCRIPT_DIR)

from schema import load_and_validate
from generators.json_types_gen import generate_json_types, needs_json_custom_types_header
from generators.dds_idl_gen import generate_dds_idl_files
from generators.dds_types_gen import generate_dds_type_bridges
from generators.dds_rpc_yaml_gen import generate_dds_rpc_yamls
from generators.module_gen import generate_modules
from generators.app_gen import generate_app_mains
from generators.pkg_gen import generate_pkg_mains
from generators.config_gen import generate_configs
from generators.cmake_gen import generate_cmake
from generators.action_gen import generate_experimental_actions

__version__ = "0.1.0"

# ── File writing helpers ─────────────────────────────────────────────────────

ALWAYS_OVERWRITE_PATTERNS = {
    "CMakeLists.txt",
    "cfg/",           # anything under cfg/
    "src/json_types.h",
    "src/json_custom_types.h",
    "schema/",        # anything under schema/
    "protocol/",      # auto-generated CDR type headers
    "src/experimental_actions/",  # private generated proof sources
}


def _is_always_overwrite(rel_path: str) -> bool:
    """Return True if the file should always be overwritten."""
    if rel_path in ("CMakeLists.txt", "src/json_types.h", "src/json_custom_types.h"):
        return True
    if rel_path.startswith("cfg/") or rel_path.startswith("schema/") or rel_path.startswith("protocol/"):
        return True
    if rel_path.startswith("src/experimental_actions/"):
        return True
    # *_dds_types.h, *_cdr_impl.cc
    base = os.path.basename(rel_path)
    if base.endswith("_dds_types.h") or base.endswith("_cdr_impl.cc"):
        return True
    # main.cc, pkg_main.cc
    if base in ("main.cc", "pkg_main.cc"):
        return True
    return False


def _is_user_file(rel_path: str) -> bool:
    """Return True if the file contains user business logic (skip unless --force)."""
    base = os.path.basename(rel_path)
    return base.endswith(".h") or base.endswith(".cc")


def write_file(output_dir: str, rel_path: str, content: str,
               force: bool = False, dry_run: bool = False,
               verbose: bool = False) -> str:
    """Write a generated file, respecting the skip/force policy.

    Returns one of: "overwrite", "skip", "new"
    """
    abs_path = os.path.join(output_dir, rel_path)
    exists = os.path.exists(abs_path)

    if exists and not _is_always_overwrite(rel_path) and _is_user_file(rel_path):
        if not force:
            if verbose or dry_run:
                print(f"  [skip]      {rel_path} (exists, use --force to overwrite)")
            return "skip"

    action = "overwrite" if exists else "new"

    if dry_run:
        print(f"  [{action:9s}] {rel_path}")
        return action

    os.makedirs(os.path.dirname(abs_path), exist_ok=True)
    with open(abs_path, "w", encoding="utf-8") as f:
        f.write(content)

    if verbose:
        print(f"  [{action:9s}] {rel_path}")

    return action


# ── Category filter ──────────────────────────────────────────────────────────

ALL_CATEGORIES = {"types", "modules", "apps", "cmake", "configs", "schema", "actions"}


def parse_categories(only_str: str | None) -> set[str]:
    if only_str is None or only_str.strip().lower() == "all":
        return ALL_CATEGORIES
    parts = {s.strip().lower() for s in only_str.split(",")}
    unknown = parts - ALL_CATEGORIES
    if unknown:
        print(f"Error: unknown categories: {unknown}", file=sys.stderr)
        print(f"Valid categories: {sorted(ALL_CATEGORIES)}", file=sys.stderr)
        sys.exit(1)
    return parts


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    # Pre-dispatch `migrate-yaml` because it has its own argument shape.
    # Backward-compatible: legacy `ibmw-gen <yaml> [opts]` keeps working
    # because we only branch on an explicit subcommand keyword.
    if len(sys.argv) >= 2 and sys.argv[1] == "migrate-yaml":
        from migrate_yaml import run_cli
        sys.exit(run_cli(sys.argv[2:]))
    if len(sys.argv) >= 2 and sys.argv[1] == "diagnose-interface":
        from ros2_interface_import import (
            InterfaceParseError,
            audit_target_artifacts,
            build_ros2_compatibility_lock,
            collect_ros2_compatibility,
            diagnose_interface,
            diagnose_interface_json,
            format_diagnosis_json,
            format_ros2_compatibility_lock_yaml,
        )
        diag_parser = argparse.ArgumentParser(
            prog="ibmw-gen diagnose-interface",
            description="Diagnose a local ROS 2 .msg, .srv, or .action interface file.",
        )
        diag_parser.add_argument("path", help="Path to a .msg, .srv, or .action interface")
        diag_parser.add_argument(
            "--search-root",
            action="append",
            default=[],
            help="Root containing ROS 2 package interface directories; repeatable",
        )
        diag_parser.add_argument(
            "--format",
            choices=("text", "json"),
            default="text",
            help="Diagnosis output format (default: text)",
        )
        diag_parser.add_argument(
            "--ros2-prefix",
            default=None,
            help="Optional ROS 2 prefix used as both host tools and target runtime compatibility prefix",
        )
        diag_parser.add_argument(
            "--ros2-distro",
            default=None,
            help="Optional ROS 2 distro name for compatibility reporting (defaults to ROS_DISTRO)",
        )
        diag_parser.add_argument(
            "--host-tools-prefix",
            default=None,
            help="Optional host-native ROS 2 generator/tools prefix for cross-compile compatibility reporting",
        )
        diag_parser.add_argument(
            "--target-runtime-prefix",
            default=None,
            help="Optional target ROS 2/FastDDS runtime prefix for cross-compile compatibility reporting",
        )
        diag_parser.add_argument(
            "--target-runtime-distro",
            default=None,
            help="Optional target runtime ROS 2 distro (defaults to --ros2-distro/ROS_DISTRO)",
        )
        diag_parser.add_argument(
            "--compatibility-lock-output",
            default=None,
            help="Optional path to write ros2_compatibility.lock.yaml from collected compatibility data",
        )
        diag_parser.add_argument(
            "--require-known-good",
            action="store_true",
            help="Fail if collected ROS2/FastDDS/FastCDR versions do not match the known-good matrix (default when a complete combo is collected)",
        )
        diag_parser.add_argument(
            "--allow-unknown-ros2-dds-combo",
            action="store_true",
            help="Allow a complete ROS2/FastDDS/FastCDR combo that is not registered in the known-good matrix",
        )
        diag_parser.add_argument(
            "--target-artifact",
            action="append",
            default=[],
            help="Target artifact to audit for host-prefix leaks and ELF architecture; repeatable",
        )
        diag_parser.add_argument(
            "--forbid-prefix",
            action="append",
            default=[],
            help="Prefix that must not appear in target artifacts; repeatable",
        )
        diag_parser.add_argument(
            "--expected-target-arch",
            choices=("x86", "x86_64", "arm", "aarch64"),
            default=None,
            help="Expected ELF architecture for --target-artifact audit",
        )
        diag_parser.add_argument(
            "--require-target-audit-pass",
            action="store_true",
            help="Fail if --target-artifact audit finds forbidden prefixes or architecture mismatches",
        )
        diag_args = diag_parser.parse_args(sys.argv[2:])
        try:
            compatibility = collect_ros2_compatibility(
                ros2_prefix=diag_args.ros2_prefix,
                ros2_distro=diag_args.ros2_distro,
                host_tools_prefix=diag_args.host_tools_prefix,
                target_runtime_prefix=diag_args.target_runtime_prefix,
                target_runtime_distro=diag_args.target_runtime_distro,
            )
            host_prefix_for_audit = diag_args.host_tools_prefix or diag_args.ros2_prefix
            forbidden_prefixes = list(diag_args.forbid_prefix)
            if host_prefix_for_audit:
                forbidden_prefixes.append(os.path.abspath(host_prefix_for_audit))
            target_artifact_audit = audit_target_artifacts(
                diag_args.target_artifact,
                forbidden_prefixes=sorted(set(forbidden_prefixes)),
                expected_arch=diag_args.expected_target_arch,
            )
            if diag_args.compatibility_lock_output:
                lock_yaml = format_ros2_compatibility_lock_yaml(
                    build_ros2_compatibility_lock(compatibility, target_artifact_audit)
                )
                lock_dir = os.path.dirname(os.path.abspath(diag_args.compatibility_lock_output))
                os.makedirs(lock_dir, exist_ok=True)
                with open(diag_args.compatibility_lock_output, "w", encoding="utf-8") as f:
                    f.write(lock_yaml)
            unknown_complete_combo = (
                compatibility["status"] == "complete"
                and compatibility["matrix_status"] == "unknown_combo"
            )
            if (
                (diag_args.require_known_good and compatibility["known_good"] != "yes")
                or (unknown_complete_combo and not diag_args.allow_unknown_ros2_dds_combo)
            ):
                raise InterfaceParseError(
                    "ROS2 compatibility combo is not known-good: "
                    + "; ".join(compatibility["reasons"])
                )
            if diag_args.require_target_audit_pass and target_artifact_audit["status"] != "pass":
                raise InterfaceParseError(
                    "target artifact audit did not pass: "
                    + "; ".join(target_artifact_audit["reasons"] or [target_artifact_audit["status"]])
                )
            if diag_args.format == "json":
                print(
                    format_diagnosis_json(
                        diagnose_interface_json(
                            diag_args.path,
                            search_roots=diag_args.search_root,
                            ros2_prefix=diag_args.ros2_prefix,
                            ros2_distro=diag_args.ros2_distro,
                            host_tools_prefix=diag_args.host_tools_prefix,
                            target_runtime_prefix=diag_args.target_runtime_prefix,
                            target_runtime_distro=diag_args.target_runtime_distro,
                            target_artifacts=diag_args.target_artifact,
                            forbidden_prefixes=diag_args.forbid_prefix,
                            expected_target_arch=diag_args.expected_target_arch,
                        )
                    ),
                    end="",
                )
            else:
                print(
                    diagnose_interface(
                        diag_args.path,
                        search_roots=diag_args.search_root,
                        ros2_prefix=diag_args.ros2_prefix,
                        ros2_distro=diag_args.ros2_distro,
                        host_tools_prefix=diag_args.host_tools_prefix,
                        target_runtime_prefix=diag_args.target_runtime_prefix,
                        target_runtime_distro=diag_args.target_runtime_distro,
                        target_artifacts=diag_args.target_artifact,
                        forbidden_prefixes=diag_args.forbid_prefix,
                        expected_target_arch=diag_args.expected_target_arch,
                    ),
                    end="",
                )
        except InterfaceParseError as exc:
            if diag_args.format == "json":
                print(format_diagnosis_json({"ok": False, "error": str(exc)}), end="", file=sys.stderr)
            else:
                print(f"Error: {exc}", file=sys.stderr)
            sys.exit(1)
        sys.exit(0)

    if len(sys.argv) >= 2 and sys.argv[1] == "import":
        from ros2_interface_import import (
            InterfaceParseError,
            emit_reference_existing_discovery_report,
            emit_reference_existing_interface_report,
            emit_workspace_interface_artifact_package,
            emit_workspace_interface_discovery_report,
            emit_rosidl_artifact_package,
            format_diagnosis_json,
            resolve_reference_existing_prefix_from_environment,
        )
        import_parser = argparse.ArgumentParser(
            prog="ibmw-gen import",
            description="Import ROS 2 interfaces and emit IBMW/ROS2 artifacts.",
        )
        import_parser.add_argument(
            "--ros2-src",
            help="Path to a ROS 2 interface package source directory containing msg/, srv/, or action/",
        )
        import_parser.add_argument(
            "--ros2-prefix",
            default=None,
            help="ROS 2 install prefix for reference_existing package/type discovery",
        )
        import_parser.add_argument(
            "--ros2-workspace",
            default=None,
            help="ROS 2 workspace/source tree to scan for interface package/type discovery",
        )
        import_parser.add_argument(
            "--ros2-package",
            default=None,
            help="Installed ROS 2 interface package name to reference from --ros2-prefix",
        )
        import_parser.add_argument(
            "--list-ros2-interfaces",
            action="store_true",
            help="List installed ROS 2 interface packages/types from --ros2-prefix using share-tree scanning only",
        )
        import_parser.add_argument(
            "--ros2-type",
            action="append",
            default=[],
            help="Installed interface type selector for reference_existing mode; repeatable. Accepts Type, msg/Type, srv/Type, action/Type, or <package>/<kind>/<Type>",
        )
        import_parser.add_argument(
            "--host-tools-prefix",
            default=None,
            help="Host ROS 2 prefix used only for generator-side discovery/version reporting in reference_existing mode",
        )
        import_parser.add_argument(
            "--target-runtime-prefix",
            default=None,
            help="Target runtime/sysroot ROS 2 prefix to record for cross-compile audits; never used for host discovery",
        )
        import_parser.add_argument(
            "--target-artifact",
            action="append",
            default=[],
            help="Target artifact to audit for host-prefix leaks and ELF architecture in reference_existing reports; repeatable",
        )
        import_parser.add_argument(
            "--forbid-prefix",
            action="append",
            default=[],
            help="Prefix that must not appear in target artifacts; repeatable",
        )
        import_parser.add_argument(
            "--expected-target-arch",
            choices=("x86", "x86_64", "arm", "aarch64"),
            default=None,
            help="Expected ELF architecture for --target-artifact audit",
        )
        import_parser.add_argument(
            "--require-target-audit-pass",
            action="store_true",
            help="Fail reference_existing import if --target-artifact audit finds forbidden prefixes or architecture mismatches",
        )
        import_parser.add_argument(
            "--artifact-only",
            action="store_true",
            help="Emit only interface artifacts; do not generate IBMW runtime wrappers",
        )
        import_parser.add_argument(
            "--output-dir",
            "-o",
            default=None,
            help="Output directory for the generated artifact package",
        )
        import_parser.add_argument(
            "--package-name",
            default=None,
            help="Override generated ROS 2 package name (defaults to package.xml name or source directory basename)",
        )
        import_parser.add_argument(
            "--target",
            action="append",
            default=[],
            choices=("dds_ros2", "dds_native", "iceoryx", "json"),
            help="Artifact target to record in the report; repeatable (default: dds_ros2)",
        )
        import_parser.add_argument(
            "--format",
            choices=("text", "json"),
            default="text",
            help="Import report output format (default: text)",
        )
        import_args = import_parser.parse_args(sys.argv[2:])
        try:
            if import_args.list_ros2_interfaces:
                if import_args.ros2_src:
                    raise InterfaceParseError("--list-ros2-interfaces cannot be combined with --ros2-src")
                if import_args.ros2_workspace and import_args.ros2_prefix:
                    raise InterfaceParseError("choose either --ros2-prefix installed discovery or --ros2-workspace source discovery, not both")
                if import_args.artifact_only:
                    raise InterfaceParseError("--list-ros2-interfaces does not emit artifacts; remove --artifact-only")
                if import_args.ros2_type:
                    raise InterfaceParseError("--ros2-type is only valid for artifact imports, not --list-ros2-interfaces")
                if import_args.output_dir:
                    raise InterfaceParseError("--output-dir is not used with --list-ros2-interfaces")
                if import_args.package_name:
                    raise InterfaceParseError("--package-name is only valid with --ros2-src; use --ros2-package to filter discovery")
                if import_args.target or import_args.target_artifact or import_args.forbid_prefix or import_args.expected_target_arch or import_args.require_target_audit_pass:
                    raise InterfaceParseError("artifact target/audit options are not used with --list-ros2-interfaces")
                if import_args.ros2_workspace:
                    report = emit_workspace_interface_discovery_report(
                        import_args.ros2_workspace,
                        package_name=import_args.ros2_package,
                    )
                    if import_args.format == "json":
                        print(format_diagnosis_json(report), end="")
                    else:
                        print(f"Discovered ROS2 workspace interface packages: {report['package_count']}")
                        print(f"Workspace root: {report['workspace_root']}")
                        for package in report["packages"]:
                            print(f"  - {package['package']} ({package['type_count']} types)")
                            for entry in package["available_types"]:
                                print(f"      {entry['selector']}")
                    sys.exit(0)
                ros2_prefix = import_args.ros2_prefix
                ros2_prefix_source = "explicit"
                if not ros2_prefix:
                    if not import_args.ros2_package:
                        raise InterfaceParseError("--list-ros2-interfaces requires --ros2-prefix unless --ros2-package can be resolved from sourced ROS2 prefix environment")
                    resolved = resolve_reference_existing_prefix_from_environment(import_args.ros2_package)
                    ros2_prefix = resolved["prefix"]
                    ros2_prefix_source = resolved["source"]
                report = emit_reference_existing_discovery_report(
                    ros2_prefix,
                    package_name=import_args.ros2_package,
                    ros2_prefix_source=ros2_prefix_source,
                )
                if import_args.format == "json":
                    print(format_diagnosis_json(report), end="")
                else:
                    print(f"Discovered ROS2 interface packages: {report['package_count']}")
                    print(f"ROS2 prefix: {report['ros2_prefix']}")
                    for package in report["packages"]:
                        print(f"  - {package['package']} ({package['type_count']} types)")
                        for entry in package["available_types"]:
                            print(f"      {entry['selector']}")
                sys.exit(0)
            if not import_args.artifact_only:
                raise InterfaceParseError("ibmw-gen import currently requires --artifact-only")
            if not import_args.output_dir:
                raise InterfaceParseError("ibmw-gen import requires --output-dir unless --list-ros2-interfaces is used")
            if import_args.ros2_workspace:
                if import_args.ros2_src or import_args.ros2_prefix:
                    raise InterfaceParseError("choose only one of --ros2-src, --ros2-prefix, or --ros2-workspace")
                if not import_args.ros2_package:
                    raise InterfaceParseError("--ros2-workspace artifact import requires --ros2-package")
                if import_args.ros2_type:
                    raise InterfaceParseError("--ros2-type is only valid with reference_existing installed package imports")
                if import_args.target_artifact or import_args.forbid_prefix or import_args.expected_target_arch or import_args.require_target_audit_pass:
                    raise InterfaceParseError("target artifact audit options are only valid with --ros2-prefix/--ros2-package reference_existing")
                report = emit_workspace_interface_artifact_package(
                    import_args.ros2_workspace,
                    import_args.ros2_package,
                    import_args.output_dir,
                    artifact_package_name=import_args.package_name,
                    targets=import_args.target or None,
                )
            elif import_args.ros2_prefix or import_args.ros2_package:
                if import_args.ros2_src:
                    raise InterfaceParseError("choose either --ros2-src generate_package or --ros2-prefix/--ros2-package reference_existing, not both")
                if import_args.package_name:
                    raise InterfaceParseError("--package-name is only valid with --ros2-src; use --ros2-package for reference_existing")
                if not import_args.ros2_package:
                    raise InterfaceParseError("reference_existing import requires --ros2-package")
                ros2_prefix = import_args.ros2_prefix
                ros2_prefix_source = "explicit"
                if not ros2_prefix:
                    resolved = resolve_reference_existing_prefix_from_environment(import_args.ros2_package)
                    ros2_prefix = resolved["prefix"]
                    ros2_prefix_source = resolved["source"]
                report = emit_reference_existing_interface_report(
                    ros2_prefix,
                    import_args.ros2_package,
                    import_args.output_dir,
                    targets=import_args.target or None,
                    selected_types=import_args.ros2_type or None,
                    host_tools_prefix=import_args.host_tools_prefix,
                    target_runtime_prefix=import_args.target_runtime_prefix,
                    target_artifacts=import_args.target_artifact,
                    forbidden_prefixes=import_args.forbid_prefix,
                    expected_target_arch=import_args.expected_target_arch,
                    require_target_audit_pass=import_args.require_target_audit_pass,
                    ros2_prefix_source=ros2_prefix_source,
                )
            else:
                if import_args.ros2_type:
                    raise InterfaceParseError("--ros2-type is only valid with --ros2-prefix/--ros2-package reference_existing")
                if import_args.target_artifact or import_args.forbid_prefix or import_args.expected_target_arch or import_args.require_target_audit_pass:
                    raise InterfaceParseError("target artifact audit options are only valid with --ros2-prefix/--ros2-package reference_existing")
                if not import_args.ros2_src:
                    raise InterfaceParseError("ibmw-gen import requires --ros2-src or --ros2-prefix/--ros2-package")
                report = emit_rosidl_artifact_package(
                    import_args.ros2_src,
                    import_args.output_dir,
                    package_name=import_args.package_name,
                    targets=import_args.target or None,
                )
            if import_args.format == "json":
                print(format_diagnosis_json(report), end="")
            else:
                if report.get("mode") == "reference_existing":
                    print(f"Referenced existing ROS2 interface package: {report['package']}")
                else:
                    print(f"Generated ROS2 artifact package: {report['package']}")
                print(f"Output: {report['output_dir']}")
                print("Files:")
                for rel in report["generated_files"]:
                    print(f"  - {rel}")
        except InterfaceParseError as exc:
            if import_args.format == "json":
                print(format_diagnosis_json({"ok": False, "error": str(exc)}), end="", file=sys.stderr)
            else:
                print(f"Error: {exc}", file=sys.stderr)
            sys.exit(1)
        sys.exit(0)

    parser = argparse.ArgumentParser(
        prog="ibmw-gen",
        description="Generate a complete IBMW project from a .ibmw.yaml file.",
    )
    parser.add_argument("yaml_file", help="Path to .ibmw.yaml project definition")
    parser.add_argument("-o", "--output-dir", default=None,
                        help="Output directory (default: ./<project.name>/)")
    parser.add_argument("--force", action="store_true",
                        help="Overwrite ALL files including user-edited modules")
    parser.add_argument("--dry-run", action="store_true",
                        help="Print what would be generated without writing")
    parser.add_argument("--only", default=None,
                        help="Only generate specific categories: "
                             "types,modules,apps,cmake,configs,schema,actions,all")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="Verbose output")
    parser.add_argument("--version", action="version", version=f"ibmw-gen {__version__}")

    args = parser.parse_args()

    # Load and validate YAML
    project = load_and_validate(args.yaml_file)

    # Determine output directory
    output_dir = args.output_dir or os.path.join(".", project["project"]["name"])
    output_dir = os.path.abspath(output_dir)

    categories = parse_categories(args.only)

    if args.verbose or args.dry_run:
        print(f"ibmw-gen v{__version__}")
        print(f"  Input:  {os.path.abspath(args.yaml_file)}")
        print(f"  Output: {output_dir}")
        print(f"  Categories: {sorted(categories)}")
        print()

    stats = {"new": 0, "overwrite": 0, "skip": 0}

    def emit(rel_path: str, content: str):
        action = write_file(output_dir, rel_path, content,
                            force=args.force, dry_run=args.dry_run,
                            verbose=args.verbose)
        stats[action] += 1

    # Copy input YAML to output (for re-generation reference)
    import shutil
    yaml_basename = os.path.basename(args.yaml_file)
    if not args.dry_run:
        os.makedirs(output_dir, exist_ok=True)
        shutil.copy2(args.yaml_file, os.path.join(output_dir, yaml_basename))

    # ── Generate files by category ──

    if "types" in categories or "schema" in categories:
        # JSON types
        if needs_json_custom_types_header(project):
            content = generate_json_types(project)
            emit("src/json_custom_types.h", content)

        # DDS IDL files
        if project.get("dds_types"):
            for rel_path, content in generate_dds_idl_files(project):
                emit(rel_path, content)

            # DDS type bridge headers + CDR impls
            for rel_path, content in generate_dds_type_bridges(project):
                emit(rel_path, content)

            # DDS RPC service YAML
            if project["dds_types"].get("rpc_services"):
                for rel_path, content in generate_dds_rpc_yamls(project):
                    emit(rel_path, content)

        # Custom DDS types (auto-generated CDR type headers)
        if project.get("custom_dds_types"):
            from generators.custom_dds_types_gen import generate_custom_dds_type_files
            for rel_path, content in generate_custom_dds_type_files(project):
                emit(rel_path, content)

    if "modules" in categories:
        for rel_path, content in generate_modules(project):
            emit(rel_path, content)

    if "apps" in categories:
        for rel_path, content in generate_app_mains(project):
            emit(rel_path, content)
        for rel_path, content in generate_pkg_mains(project):
            emit(rel_path, content)

    if "configs" in categories:
        for rel_path, content in generate_configs(project):
            emit(rel_path, content)

    if "actions" in categories:
        for rel_path, content in generate_experimental_actions(project):
            emit(rel_path, content)

    if "cmake" in categories:
        content = generate_cmake(project)
        emit("CMakeLists.txt", content)

    # ── Summary ──
    total = stats["new"] + stats["overwrite"] + stats["skip"]
    print()
    if args.dry_run:
        print(f"Dry run complete: {total} files "
              f"({stats['new']} new, {stats['overwrite']} overwrite, {stats['skip']} skip)")
    else:
        print(f"Generation complete: {total} files "
              f"({stats['new']} new, {stats['overwrite']} overwrite, {stats['skip']} skip)")
        print(f"Output: {output_dir}")


if __name__ == "__main__":
    main()
