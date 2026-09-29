#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# CMakeLists.txt generator for ibmw-gen.

"""Generate CMakeLists.txt for the generated project."""

# ROS2 RPC gencode paths read from ibmw_gen.config so
# integrators can override via IBMW_GEN_ROS2_CMAKE_{PATH,DIR} env vars
# without forking the generator. Defaults preserve historical behavior.
try:
    from typing import Any

    from .. import config as _ibmw_gen_config
    from .type_support_pkg_gen import collect_package_type_support
except ImportError:  # pragma: no cover — direct-script invocation fallback
    from typing import Any

    import sys as _sys
    import os as _os
    _sys.path.insert(0, _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__))))
    import config as _ibmw_gen_config  # type: ignore
    from generators.type_support_pkg_gen import collect_package_type_support


def _snake(name: str) -> str:
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


def _module_dir(mod_name: str) -> str:
    """Compute module directory name from module name."""
    snake = _snake(mod_name)
    return snake if snake.endswith("_module") else snake + "_module"


def _emit_experimental_fibonacci_action(lines: list[str], action: dict[str, Any]) -> None:
    """Emit private DDS-only Fibonacci Action proof targets."""
    roles = set(action.get("roles", []))
    lines.append("# --------------------------------------------------------------------------")
    lines.append("# Experimental private DDS-only Fibonacci Action proof targets.")
    lines.append("# Not a public ROS2 Action runtime or generic .action generator.")
    lines.append("# IBMW_GEN_EXPERIMENTAL_ACTION_FIBONACCI_DDS_ONLY")
    lines.append("# --------------------------------------------------------------------------")
    lines.append("ibmw_find_target_package(action_msgs QUIET FEATURE ibmw-gen-experimental-action-fibonacci)")
    lines.append("ibmw_find_target_package(builtin_interfaces QUIET FEATURE ibmw-gen-experimental-action-fibonacci)")
    lines.append("ibmw_find_target_package(example_interfaces QUIET FEATURE ibmw-gen-experimental-action-fibonacci)")
    lines.append("ibmw_find_target_package(unique_identifier_msgs QUIET FEATURE ibmw-gen-experimental-action-fibonacci)")
    lines.append("")
    lines.append("set(IBMW_GEN_ACTION_FIBONACCI_DDS_ROS2_HEADERS")
    lines.append("  action_msgs::action_msgs__rosidl_generator_cpp")
    lines.append("  builtin_interfaces::builtin_interfaces__rosidl_generator_cpp")
    lines.append("  example_interfaces::example_interfaces__rosidl_generator_cpp")
    lines.append("  unique_identifier_msgs::unique_identifier_msgs__rosidl_generator_cpp)")
    lines.append("set(IBMW_GEN_ACTION_FIBONACCI_DDS_ROS2_LIBS)")
    lines.append("if(CMAKE_CROSSCOMPILING AND IBMW_TARGET_ROS2_TIER STREQUAL \"rosidl\")")
    lines.append("  ibmw_resolve_target_ros2_include_root(IBMW_GEN_ACTION_FIBONACCI_ROS2_INCLUDE_ROOT rmw/rmw/types.h)")
    lines.append("  set(IBMW_GEN_ACTION_FIBONACCI_ROS2_RMW_INCLUDE_DIR ${IBMW_GEN_ACTION_FIBONACCI_ROS2_INCLUDE_ROOT}/rmw)")
    lines.append("  foreach(_ibmw_gen_action_ros2_lib IN ITEMS")
    lines.append("      example_interfaces__rosidl_typesupport_fastrtps_cpp")
    lines.append("      action_msgs__rosidl_typesupport_fastrtps_cpp")
    lines.append("      builtin_interfaces__rosidl_typesupport_fastrtps_cpp")
    lines.append("      unique_identifier_msgs__rosidl_typesupport_fastrtps_cpp")
    lines.append("      rosidl_typesupport_fastrtps_cpp")
    lines.append("      rosidl_runtime_c")
    lines.append("      rcutils)")
    lines.append("    ibmw_resolve_target_ros2_library(_ibmw_gen_action_ros2_lib_path ${_ibmw_gen_action_ros2_lib})")
    lines.append("    list(APPEND IBMW_GEN_ACTION_FIBONACCI_DDS_ROS2_LIBS ${_ibmw_gen_action_ros2_lib_path})")
    lines.append("  endforeach()")
    lines.append("else()")
    lines.append("  list(APPEND IBMW_GEN_ACTION_FIBONACCI_DDS_ROS2_LIBS")
    lines.append("    action_msgs::action_msgs__rosidl_typesupport_fastrtps_cpp")
    lines.append("    builtin_interfaces::builtin_interfaces__rosidl_typesupport_fastrtps_cpp")
    lines.append("    example_interfaces::example_interfaces__rosidl_typesupport_fastrtps_cpp")
    lines.append("    unique_identifier_msgs::unique_identifier_msgs__rosidl_typesupport_fastrtps_cpp)")
    lines.append("endif()")
    lines.append("")
    lines.append("add_library(ibmw_gen_experimental_fibonacci_action_common INTERFACE)")
    lines.append("target_include_directories(ibmw_gen_experimental_fibonacci_action_common")
    lines.append("  INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/experimental_actions/fibonacci/common)")
    lines.append("target_link_libraries(ibmw_gen_experimental_fibonacci_action_common")
    lines.append("  INTERFACE ibmw::base)")
    lines.append("if(TARGET ibmw::extensions::dds_types)")
    lines.append("  target_link_libraries(ibmw_gen_experimental_fibonacci_action_common")
    lines.append("    INTERFACE ibmw::extensions::dds_types)")
    lines.append("endif()")
    lines.append("")

    targets = []
    if "server" in roles:
        targets.append(("ibmw_gen_fibonacci_action_dds_server",
                        "server/fibonacci_action_server.cpp"))
    if "client" in roles:
        targets.append(("ibmw_gen_fibonacci_action_dds_client",
                        "client/fibonacci_action_client.cpp"))
    for target, rel_src in targets:
        src = "${CMAKE_CURRENT_SOURCE_DIR}/src/experimental_actions/fibonacci/" + rel_src
        lines.append(f"set({target}_SRC {src})")
        lines.append("if(TARGET ibmw::extensions::dds_types AND")
        lines.append("   TARGET action_msgs::action_msgs__rosidl_generator_cpp AND")
        lines.append("   TARGET action_msgs::action_msgs__rosidl_typesupport_fastrtps_cpp AND")
        lines.append("   TARGET builtin_interfaces::builtin_interfaces__rosidl_generator_cpp AND")
        lines.append("   TARGET builtin_interfaces::builtin_interfaces__rosidl_typesupport_fastrtps_cpp AND")
        lines.append("   TARGET example_interfaces::example_interfaces__rosidl_generator_cpp AND")
        lines.append("   TARGET example_interfaces::example_interfaces__rosidl_typesupport_fastrtps_cpp AND")
        lines.append("   TARGET unique_identifier_msgs::unique_identifier_msgs__rosidl_generator_cpp AND")
        lines.append("   TARGET unique_identifier_msgs::unique_identifier_msgs__rosidl_typesupport_fastrtps_cpp AND")
        lines.append(f"   EXISTS ${{{target}_SRC}})")
        lines.append(f"  add_executable({target} ${{{target}_SRC}})")
        lines.append(f"  target_include_directories({target}")
        lines.append("    PRIVATE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src/experimental_actions/fibonacci>")
        lines.append("            $<$<BOOL:${IBMW_GEN_ACTION_FIBONACCI_ROS2_RMW_INCLUDE_DIR}>:${IBMW_GEN_ACTION_FIBONACCI_ROS2_RMW_INCLUDE_DIR}>)")
        lines.append(f"  target_link_libraries({target}")
        lines.append("    PRIVATE gflags::gflags")
        lines.append("            ibmw::core::engine")
        lines.append("            ibmw::extensions::dds_types")
        lines.append("            ibmw_gen_experimental_fibonacci_action_common")
        lines.append("            ${IBMW_GEN_ACTION_FIBONACCI_DDS_ROS2_HEADERS}")
        lines.append("            ${IBMW_GEN_ACTION_FIBONACCI_DDS_ROS2_LIBS})")
        lines.append(f"  set_target_properties({target} PROPERTIES OUTPUT_NAME {target})")
        lines.append("else()")
        lines.append(f"  message(STATUS \"Skipping {target}: requires DDS Action proof generated type targets and ${{{target}_SRC}}\")")
        lines.append("endif()")
        lines.append("")


def generate_cmake(project: dict) -> str:
    """Generate the full CMakeLists.txt content."""
    proj = project["project"]
    proj_name = proj["name"]
    ns = proj["_namespace_cpp"]
    module_map = project["_module_map"]
    modules = project.get("modules", [])
    apps = project.get("apps", [])
    dds = project.get("dds_types")
    json_types = project.get("json_types")
    json_messages = (json_types or {}).get("messages", [])
    cdt = project.get("custom_dds_types")
    experimental_actions = project.get("experimental_actions", [])

    # Determine what features are used
    has_dds = any(m.get("transport") == "dds" for m in modules)
    has_json = (any(m.get("transport") == "json" for m in modules)
                or bool(json_messages))
    has_ros2 = any(m.get("transport") == "ros2" for m in modules)
    imported_srv_adapter_packages = sorted({
        str(m.get("ros2_package"))
        for m in modules
        if m.get("transport") == "ros2" and m.get("ros2_imported_srv_adapter") and m.get("ros2_package")
    })
    has_dds_rpc = any(
        m.get("transport") == "dds" and m["type"] in ("rpc_server", "rpc_client")
        for m in modules
    )
    has_custom_dds = bool(cdt)
    needs_fastcdr = project.get("_needs_fastcdr", False)
    has_ros2_interop = project.get("_has_ros2_interop", False)
    ros2_interop_packages = project.get("_ros2_interop_packages", [])
    # Loaned modules use public header-only TypeSupport adapters. Their ROS2
    # packages are still discovered here, but they do not need transport
    # extension libraries or ros2_types static registrars.
    loaned_publisher_packages = project.get("_loaned_publisher_packages", [])
    # loaned_subscriber project-level signals mirror loaned_publisher.
    loaned_subscriber_packages = project.get("_loaned_subscriber_packages", [])
    has_direct_iceoryx = project.get("_has_direct_iceoryx", False)
    direct_iceoryx_packages = project.get("_direct_iceoryx_packages", [])
    needs_ros2_whole_archive = has_ros2_interop or has_direct_iceoryx
    ros2_msg_packages = sorted(set(ros2_interop_packages)
                               | set(loaned_publisher_packages)
                               | set(loaned_subscriber_packages)
                               | set(direct_iceoryx_packages))
    # Only explicit direct iceoryx modules compile against the implementation.
    needs_iceoryx_posh_top = has_direct_iceoryx

    lines = []

    # Header
    lines.append("# This file was generated by ibmw-gen. Do not edit.")
    lines.append("#")
    lines.append(f"# Project: {proj_name}")
    lines.append("#")
    lines.append("# Usage:")
    lines.append("#   mkdir build && cd build")
    lines.append('#   cmake .. -DCMAKE_PREFIX_PATH="$IBMW_INSTALL_PREFIX"')
    lines.append("#   make")
    lines.append("")
    lines.append("cmake_minimum_required(VERSION 3.24)")
    lines.append(f"project({proj_name} LANGUAGES CXX)")
    lines.append("")
    lines.append("set(CMAKE_CXX_STANDARD 20)")
    lines.append("set(CMAKE_CXX_STANDARD_REQUIRED ON)")
    lines.append("set(CMAKE_CXX_EXTENSIONS OFF)")
    lines.append("if(CMAKE_CROSSCOMPILING)")
    lines.append("  set(CMAKE_SKIP_RPATH ON)")
    lines.append("  set(CMAKE_SKIP_BUILD_RPATH ON)")
    lines.append("  set(CMAKE_SKIP_INSTALL_RPATH ON)")
    lines.append("  set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF)")
    lines.append("endif()")
    lines.append("")

    # Find IBMW
    lines.append("# --------------------------------------------------------------------------")
    lines.append("# Find IBMW (also loads IbmwHelpers.cmake: ibmw_add_module/pkg/app/etc.)")
    lines.append("# --------------------------------------------------------------------------")
    lines.append("find_package(ibmw 0.1.0 REQUIRED)")
    lines.append("")

    # Whole-archive helper for ROS2 interop and explicit direct modules
    # (static type registrars in ros2_types_impl must be force-loaded).
    if needs_ros2_whole_archive:
        lines.append("# --------------------------------------------------------------------------")
        lines.append("# Helper: force whole-archive link for ros2_types_impl static registrars.")
        lines.append("# Used by ros2_interop_* and direct_iceoryx_* modules.")
        lines.append("# --------------------------------------------------------------------------")
        lines.append("function(ibmw_whole_archive_link TARGET LIB)")
        lines.append("  if(APPLE)")
        lines.append("    target_link_libraries(${TARGET} PUBLIC -Wl,-force_load ${LIB})")
        lines.append("  elseif(UNIX)")
        lines.append("    target_link_libraries(${TARGET} PUBLIC")
        lines.append("      -Wl,--whole-archive ${LIB} -Wl,--no-whole-archive)")
        lines.append("  else()")
        lines.append("    target_link_libraries(${TARGET} PUBLIC ${LIB})")
        lines.append("  endif()")
        lines.append("endfunction()")
        lines.append("")

    # ROS2 message packages (e.g. geometry_msgs, sensor_msgs) needed by
    # ros2_interop / loaned_* / direct_iceoryx_* modules.
    if ros2_msg_packages:
        lines.append("# --------------------------------------------------------------------------")
        lines.append("# ROS2 message packages required by ros2_interop / loaned_* / direct_iceoryx_* modules.")
        lines.append("# --------------------------------------------------------------------------")
        lines.append("find_package(ament_cmake REQUIRED)")
        for pkg in ros2_msg_packages:
            lines.append(f"find_package({pkg} REQUIRED)")
        lines.append("")

    # iceoryx_posh — required by loaned_* / direct_iceoryx_* modules using
    # iceoryx transport (iceoryx_ext links it PRIVATE; downstream must re-find).
    # Note: iceoryx_poshTargets.cmake transitively references the `cpptoml`
    # import target. Upstream cpptoml ships an empty cpptomlConfig.cmake
    # (find_package(cpptoml) succeeds but creates no target), so we must
    # explicitly include cpptomlTargets.cmake to materialize it before
    # find_package(iceoryx_posh) — otherwise iceoryx_posh's config aborts
    # with "missing imported targets: cpptoml".
    if needs_iceoryx_posh_top:
        lines.append("# --------------------------------------------------------------------------")
        lines.append("# iceoryx_posh — required by loaned_publisher / loaned_subscriber modules")
        lines.append("# with transport: iceoryx.")
        lines.append("# --------------------------------------------------------------------------")
        lines.append("# Materialize the `cpptoml` INTERFACE import target (transitively required")
        lines.append("# by iceoryx_poshTargets.cmake; upstream cpptomlConfig.cmake is empty).")
        lines.append("find_package(cpptoml QUIET)")
        lines.append("if(NOT TARGET cpptoml)")
        lines.append("  find_file(_ibmw_cpptoml_targets cpptomlTargets.cmake")
        lines.append("    PATHS ${CMAKE_PREFIX_PATH} ${ibmw_DIR}/../../..")
        lines.append("    PATH_SUFFIXES lib/cmake/cpptoml")
        lines.append("    NO_DEFAULT_PATH)")
        lines.append("  if(_ibmw_cpptoml_targets)")
        lines.append("    include(${_ibmw_cpptoml_targets})")
        lines.append("  else()")
        lines.append("    message(FATAL_ERROR \"cpptomlTargets.cmake not found; iceoryx_posh requires it. \"")
        lines.append("      \"Add the directory containing lib/cmake/cpptoml/ to CMAKE_PREFIX_PATH.\")")
        lines.append("  endif()")
        lines.append("endif()")
        lines.append("find_package(iceoryx_posh REQUIRED)")
        lines.append("")

    # fastcdr for custom DDS types
    if needs_fastcdr:
        lines.append("# --------------------------------------------------------------------------")
        lines.append("# FastCDR — required for custom DDS type serialization.")
        lines.append("# --------------------------------------------------------------------------")
        lines.append("find_package(fastcdr REQUIRED)")
        lines.append("")

    # The installed IBMW SDK supplies nlohmann_json::nlohmann_json with its
    # public TypeSupport target. Do not fetch a second copy in JSON projects.

    # ROS2 find_package if needed
    if has_ros2:
        # Collect unique ROS2 package names from modules
        ros2_packages = set()
        for m in modules:
            if m.get("transport") == "ros2":
                pkg = m.get("ros2_package")
                if pkg:
                    ros2_packages.add(pkg)
        for pkg in sorted(ros2_packages):
            lines.append(f"find_package({pkg} REQUIRED)")
        if imported_srv_adapter_packages:
            lines.append("find_package(rosidl_typesupport_cpp REQUIRED)")
        if ros2_packages:
            lines.append("")
        for pkg in imported_srv_adapter_packages:
            target_name = f"{_snake(pkg)}_ibmw_imported_srv_glue"
            lines.append(f"ibmw_add_rosidl_interface_target({target_name}")
            lines.append(f"  PACKAGE_NAME {pkg}")
            lines.append(f"  DEPS {pkg}::{pkg}__rosidl_typesupport_cpp")
            lines.append("       rosidl_typesupport_cpp::rosidl_typesupport_cpp)")
            lines.append("")

    # DDS messages if needed (IDL-based, not custom_dds_types)
    if dds:
        groups = dds.get("groups", [])
        rpc_services = dds.get("rpc_services", [])

        # Determine which groups/services need local gencode generation
        # (i.e., not provided by external pre-shipped targets)
        local_groups = [g for g in groups if not g.get("idl_gencode_target")
                        and g.get("messages")]
        local_rpc_services = [s for s in rpc_services
                              if not s.get("rpc_gencode_target")]

        # Include cmake modules if we need to generate IDL/RPC code
        if local_groups or local_rpc_services:
            lines.append("# --------------------------------------------------------------------------")
            lines.append("# DDS IDL / RPC code generation for user-defined types.")
            lines.append("# --------------------------------------------------------------------------")
            lines.append("if(DEFINED IBMW_HELPERS_DIR)")
            lines.append("  list(APPEND CMAKE_MODULE_PATH \"${IBMW_HELPERS_DIR}\")")
            lines.append("else()")
            lines.append("  list(APPEND CMAKE_MODULE_PATH \"${ibmw_DIR}/../../../cmake\")")
            lines.append("endif()")
            if local_groups:
                lines.append("include(DdsIdlGenCode)")
            if local_rpc_services:
                lines.append("if(DEFINED IBMW_DDS_IDL_RPC_GEN_TOOL_PATH)")
                lines.append("  set_property(GLOBAL PROPERTY DDS_IDL_RPC_GEN_CODE_TOOL_PATH_PROPERTY"
                             " \"${IBMW_DDS_IDL_RPC_GEN_TOOL_PATH}\")")
                lines.append("else()")
                lines.append("  set_property(GLOBAL PROPERTY DDS_IDL_RPC_GEN_CODE_TOOL_PATH_PROPERTY"
                             " \"${ibmw_DIR}/../../../bin\")")
                lines.append("endif()")
                lines.append("include(DdsIdlRpcGenCode)")
            lines.append("")

        # Build lookup for group -> idl_gencode_target
        group_idl_target = {}  # group_name -> target name
        for g in groups:
            if g.get("idl_gencode_target"):
                group_idl_target[g["name"]] = g["idl_gencode_target"]
            else:
                group_idl_target[g["name"]] = f"{_snake(g['name'])}_idl_gencode"

        # Generate IDL gencode targets for local groups
        for g in local_groups:
            group_name = g["name"]
            snake = _snake(group_name)
            idl_target = group_idl_target[group_name]

            lines.append(f"# IDL gencode: {group_name}")
            lines.append(f"add_dds_idl_gencode_target(")
            lines.append(f"  TARGET_NAME {idl_target}")
            lines.append(f"  IDL_FILES ${{CMAKE_CURRENT_SOURCE_DIR}}/schema/{group_name}.idl")
            lines.append(f"  GENCODE_PATH ${{CMAKE_CURRENT_BINARY_DIR}}/gencode)")
            lines.append("")

        # Generate RPC gencode targets for local services
        rpc_gencode_targets = {}  # svc_name -> target name
        for svc in local_rpc_services:
            svc_name = svc["name"]
            group_name = svc["group"]
            snake_svc = _snake(svc_name)
            rpc_target = f"{snake_svc}_rpc_gencode"
            rpc_gencode_targets[svc_name] = rpc_target
            idl_target = group_idl_target.get(group_name,
                f"{_snake(group_name)}_idl_gencode")

            lines.append(f"# RPC gencode: {svc_name}")
            lines.append(f"add_dds_idl_rpc_gencode_target(")
            lines.append(f"  TARGET_NAME {rpc_target}")
            lines.append(f"  SERVICE_FILE ${{CMAKE_CURRENT_SOURCE_DIR}}/schema/{svc_name}.yaml")
            lines.append(f"  GENCODE_PATH ${{CMAKE_CURRENT_BINARY_DIR}}/gencode")
            lines.append(f"  IDL_FILE_STEM {group_name}")
            lines.append(f"  DEP_IDL_TARGETS {idl_target})")
            lines.append("")

        # DDS messages (non-RPC groups)
        for g in local_groups:
            group_name = g["name"]
            snake = _snake(group_name)

            # Skip groups used by RPC services (handled by ibmw_add_dds_rpc)
            is_rpc_group = any(svc["group"] == group_name for svc in rpc_services)
            if is_rpc_group:
                continue

            idl_target = group_idl_target[group_name]
            lines.append(f"# DDS messages: {group_name}")
            lines.append(f"ibmw_add_dds_messages({snake}_cdr")
            lines.append(f"  IDL_GENCODE_TARGET {idl_target}")
            lines.append(f"  CDR_IMPLS src/{snake}_cdr_impl.cc)")
            lines.append("")

        # DDS RPC
        for svc in rpc_services:
            svc_name = svc["name"]
            group_name = svc["group"]
            snake_group = _snake(group_name)
            snake_svc = _snake(svc_name)

            # Skip services with external targets
            if svc.get("rpc_gencode_target"):
                continue

            idl_target = group_idl_target.get(group_name,
                f"{_snake(group_name)}_idl_gencode")
            rpc_target = rpc_gencode_targets.get(svc_name,
                f"{snake_svc}_rpc_gencode")

            lines.append(f"# DDS RPC: {svc_name}")
            lines.append(f"ibmw_add_dds_rpc({snake_svc}_cdr")
            lines.append(f"  IDL_GENCODE_TARGET {idl_target}")
            lines.append(f"  RPC_GENCODE_TARGET {rpc_target}")
            lines.append(f"  CDR_IMPLS src/{snake_group}_cdr_impl.cc)")
            lines.append("")

    # ROS2 RPC gencode from .srv files (when srv_file is specified)
    # Collect unique (ros2_package, srv_file, ros2_service) tuples
    ros2_rpc_gencode_targets = {}  # (pkg, svc) -> cmake target name
    for mod in modules:
        if (mod.get("transport") == "ros2" and
                mod["type"] in ("rpc_server", "rpc_client") and
                mod.get("srv_file") and not mod.get("ros2_rpc_gencode_target")):
            pkg = mod["ros2_package"]
            svc = mod["ros2_service"]
            key = (pkg, svc)
            if key not in ros2_rpc_gencode_targets:
                target_name = f"{_snake(pkg)}_{_snake(svc)}_ibmw_rpc_gencode"
                ros2_rpc_gencode_targets[key] = target_name

    if ros2_rpc_gencode_targets:
        lines.append("# --------------------------------------------------------------------------")
        lines.append("# ROS2 RPC gencode — generate IBMW RPC wrappers from .srv files.")
        lines.append("# --------------------------------------------------------------------------")
        lines.append("find_package(Python3 REQUIRED COMPONENTS Interpreter)")
        lines.append(f"include({_ibmw_gen_config.ros2_rpc_gen_cmake_path()})")
        lines.append("set_property(GLOBAL PROPERTY ROS2_IBMW_RPC_GEN_CODE_TOOL_PATH_PROPERTY")
        lines.append(f"  {_ibmw_gen_config.ros2_rpc_gen_dir()})")
        lines.append("")
        for (pkg, svc), target in ros2_rpc_gencode_targets.items():
            srv_file = None
            for mod in modules:
                if (mod.get("ros2_package") == pkg and
                        mod.get("ros2_service") == svc and mod.get("srv_file")):
                    srv_file = mod["srv_file"]
                    break
            lines.append(f"add_ros2_ibmw_rpc_gencode_target_for_one_file(")
            lines.append(f"  TARGET_NAME {target}")
            lines.append(f"  PACKAGE_NAME {pkg}")
            lines.append(f"  PROTO_FILE ${{CMAKE_CURRENT_SOURCE_DIR}}/{srv_file}")
            lines.append(f"  GENCODE_PATH ${{CMAKE_CURRENT_BINARY_DIR}}/gencode)")
            lines.append("")
        # Store for dep resolution
        for (pkg, svc), target in ros2_rpc_gencode_targets.items():
            for mod in modules:
                if (mod.get("ros2_package") == pkg and
                        mod.get("ros2_service") == svc and
                        not mod.get("ros2_rpc_gencode_target")):
                    mod["_auto_ros2_rpc_gencode_target"] = target

    json_ts_target = None
    if json_messages:
        json_ts_target = f"{_snake(proj_name)}_json_ts"
        lines.append("# --------------------------------------------------------------------------")
        lines.append("# JSON TypeSupport Package for Bag schema discovery and playback.")
        lines.append("# --------------------------------------------------------------------------")
        lines.append(f"add_library({json_ts_target} SHARED")
        lines.append("  src/json_type_support_pkg_main.cc)")
        lines.append(f"target_include_directories({json_ts_target} PRIVATE")
        lines.append("  ${CMAKE_CURRENT_SOURCE_DIR}/src)")
        lines.append(f"target_link_libraries({json_ts_target} PRIVATE")
        lines.append("  ibmw::api::type_support")
        lines.append("  ibmw::api::cpp")
        lines.append("  nlohmann_json::nlohmann_json)")
        lines.append(f"set_target_properties({json_ts_target} PROPERTIES")
        lines.append(f"  OUTPUT_NAME \"{_snake(proj_name)}_json_ts\")")
        lines.append(f"install(TARGETS {json_ts_target}")
        lines.append("  LIBRARY DESTINATION lib")
        lines.append("  RUNTIME DESTINATION bin)")
        lines.append("")

    for action in experimental_actions:
        if action.get("name") == "fibonacci":
            _emit_experimental_fibonacci_action(lines, action)

    # Group modules by their app membership for section comments
    section_num = 0

    # Process each module
    module_cmake_targets = {}  # module_name -> cmake target name

    for mod in modules:
        mod_name = mod["name"]
        mod_type = mod["type"]
        transport = mod.get("transport")
        snake_name = _snake(mod_name)
        mod_dir = _module_dir(mod_name)

        # Determine SRCS and DEPS
        srcs = f"src/{mod_dir}/{snake_name}.cc"
        deps = []
        extra_include_dirs = []

        if transport == "dds":
            if (mod.get("_is_loaned_publisher") or
                    mod.get("_is_loaned_subscriber")):
                deps.append("ibmw::api::ros2_type_support")
                for pkg in mod.get("ros2_packages", []):
                    deps.append(f"${{{pkg}_TARGETS}}")
            elif mod.get("_is_ros2_interop"):
                # ros2_interop_publisher / ros2_interop_subscriber modules
                # need ros2_types + per-package targets (whole-archive link
                # added later for static registrars).
                deps.append("ibmw::extensions::ros2_types")
                for pkg in mod.get("ros2_packages", []):
                    deps.append(f"${{{pkg}_TARGETS}}")
            else:
                deps.append("ibmw::extensions::dds_types")

            # Check if this module uses custom_dds_types
            uses_custom = mod.get("_uses_custom_dds_types", False)

            if uses_custom and has_custom_dds:
                # Custom DDS types — link fastcdr, add protocol/ include dir
                deps.append("fastcdr")
                extra_include_dirs.append("${CMAKE_CURRENT_SOURCE_DIR}")
            elif mod_type in ("publisher", "subscriber"):
                # IDL-based DDS types
                if dds:
                    msg_name = mod.get("message_type", "")
                    group_name = dds.get("_msg_to_group", {}).get(msg_name)
                    if group_name:
                        # Find the group dict
                        grp = None
                        for g in dds.get("groups", []):
                            if g["name"] == group_name:
                                grp = g
                                break
                        if grp and grp.get("idl_gencode_target"):
                            # External target — use it directly, no CDR
                            deps.append(grp["idl_gencode_target"])
                        else:
                            snake_group = _snake(group_name)
                            deps.append(f"{snake_group}_idl_gencode")
                            deps.append(f"{snake_group}_cdr")
            elif mod_type in ("rpc_server", "rpc_client"):
                svc_ref = mod.get("dds_service", "")
                if dds:
                    for svc in dds.get("rpc_services", []):
                        if svc["name"] == svc_ref:
                            if svc.get("rpc_gencode_target"):
                                # External target — use it directly, no CDR
                                deps.append(svc["rpc_gencode_target"])
                            else:
                                snake_svc = _snake(svc_ref)
                                deps.append(f"{snake_svc}_rpc_gencode")
                                deps.append(f"{snake_svc}_cdr")
                            break

        elif transport == "iceoryx":
            # Only loaned_publisher modules use the iceoryx transport at
            # the codegen level (regular pub/sub still go through dds today).
            # Schema enforces this: VALID_TRANSPORTS allows 'iceoryx' and
            # the loaned_publisher branch is the only consumer.
            if mod.get("_is_direct_iceoryx"):
                deps.append("ibmw::extensions::iceoryx_ext")
                deps.append("iceoryx_posh::iceoryx_posh")
                deps.append("ibmw::extensions::ros2_types")
                for pkg in mod.get("ros2_packages", []):
                    deps.append(f"${{{pkg}_TARGETS}}")
            elif (mod.get("_is_loaned_publisher") or
                  mod.get("_is_loaned_subscriber")):
                deps.append("ibmw::api::ros2_type_support")
                for pkg in mod.get("ros2_packages", []):
                    deps.append(f"${{{pkg}_TARGETS}}")
            # else: no other module type uses iceoryx today; future module
            # types should add their own branch here.

        elif transport == "json":
            deps.append("nlohmann_json::nlohmann_json")
            if mod.get("ros2_import_json_types_header") and mod.get("ros2_import_json_types_include_dir"):
                extra_include_dirs.append(str(mod["ros2_import_json_types_include_dir"]))
            elif not mod.get("ros2_import_json_types_header"):
                extra_include_dirs.append("${CMAKE_CURRENT_SOURCE_DIR}/src")

        elif transport == "ros2":
            deps.append("ibmw::extensions::ros2_types")
            ros2_pkg = mod.get("ros2_package")
            if ros2_pkg:
                deps.append(f"{ros2_pkg}::{ros2_pkg}__rosidl_generator_cpp")
            if mod.get("ros2_imported_srv_adapter") and ros2_pkg:
                deps.append(f"{_snake(str(ros2_pkg))}_ibmw_imported_srv_glue")
            # ROS2 RPC may need gencode targets
            if mod_type in ("rpc_server", "rpc_client"):
                ros2_rpc_target = mod.get("ros2_rpc_gencode_target") or mod.get("_auto_ros2_rpc_gencode_target")
                if ros2_rpc_target:
                    deps.append(ros2_rpc_target)

        # Generate ibmw_add_module
        lines.append(f"ibmw_add_module({snake_name}")
        lines.append(f"  SRCS {srcs}")
        if deps:
            lines.append(f"  DEPS {' '.join(deps)})")
        else:
            lines.append(")")

        # Add target_include_directories for custom DDS types (PUBLIC so
        # app/pkg targets that link this module can also find protocol/ headers)
        if extra_include_dirs:
            for inc_dir in extra_include_dirs:
                lines.append(f"target_include_directories({snake_name} PUBLIC {inc_dir})")
        if mod.get("ros2_imported_srv_adapter") and mod.get("ros2_imported_srv_adapter_include_dir"):
            lines.append(
                f"target_include_directories({snake_name} PUBLIC {mod['ros2_imported_srv_adapter_include_dir']})"
            )

        # Only interop and explicitly direct modules rely on the static
        # ros2_types registrar library.
        if mod.get("_is_ros2_interop") or mod.get("_is_direct_iceoryx"):
            lines.append(f"ibmw_whole_archive_link({snake_name} ibmw_extensions_ros2_types_impl)")

        lines.append("")

        module_cmake_targets[mod_name] = snake_name

    # Process each app
    for app in apps:
        app_name = app["_snake_name"]
        pkg_name = app["_pkg_name"]
        mod_names = app["modules"]
        app_type = app.get("type", "pubsub")

        module_targets = [module_cmake_targets.get(m, _snake(m)) for m in mod_names]
        package_type_support = collect_package_type_support(project, mod_names)

        # Pkg
        lines.append(f"ibmw_add_pkg({pkg_name}")
        lines.append(f"  SRCS src/{pkg_name}/pkg_main.cc")
        lines.append(f"  MODULES {' '.join(module_targets)})")
        lines.append("")

        if package_type_support.entries:
            ts_target = f"{pkg_name}_ts"
            lines.append(f"add_library({ts_target} SHARED")
            lines.append(f"  src/{pkg_name}/type_support_pkg_main.cc)")
            lines.append(f"target_include_directories({ts_target} PRIVATE")
            lines.append("  ${CMAKE_CURRENT_SOURCE_DIR}")
            lines.append("  ${CMAKE_CURRENT_SOURCE_DIR}/src)")
            lines.append(f"target_link_libraries({ts_target} PRIVATE")
            lines.append("  " + "\n  ".join(package_type_support.cmake_dependencies()) + ")")
            lines.append(f"set_target_properties({ts_target} PROPERTIES")
            lines.append(f"  OUTPUT_NAME \"{ts_target}\")")
            lines.append(f"install(TARGETS {ts_target}")
            lines.append("  LIBRARY DESTINATION lib")
            lines.append("  RUNTIME DESTINATION bin)")
            lines.append("")

        # App
        if app_type == "bare":
            # Bare app uses add_executable directly (no modules)
            lines.append(f"add_executable({app_name} src/{app_name}/main.cc)")
            lines.append(f"target_link_libraries({app_name} PRIVATE ibmw::core::engine)")
        else:
            lines.append(f"ibmw_add_app({app_name}")
            lines.append(f"  SRCS src/{app_name}/main.cc")
            lines.append(f"  MODULES {' '.join(module_targets)})")
        lines.append("")

    # Copy config files. Pure experimental Action proof projects do not emit cfg/.
    if apps:
        lines.append("# --------------------------------------------------------------------------")
        lines.append("# Copy config files to build directory")
        lines.append("# --------------------------------------------------------------------------")
        lines.append("file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/cfg DESTINATION ${CMAKE_CURRENT_BINARY_DIR})")
        lines.append("")

    # Summary messages
    lines.append('message(STATUS "")')
    lines.append(f'message(STATUS "{proj_name} configured successfully!")')
    lines.append('message(STATUS "")')

    for i, app in enumerate(apps, 1):
        app_name = app["_snake_name"]
        pkg_name = app["_pkg_name"]
        lines.append(f'message(STATUS "  {i}. {app_name}:")')
        lines.append(f'message(STATUS "       Pkg:  ibmw run cfg/{pkg_name}.yaml")')
        lines.append(f'message(STATUS "       App:  ./{app_name} cfg/{app_name}.yaml")')
        lines.append('message(STATUS "")')

    return "\n".join(lines)
