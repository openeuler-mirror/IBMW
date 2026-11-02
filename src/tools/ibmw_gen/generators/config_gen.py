#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# YAML config file generator for ibmw-gen.

"""Generate cfg/<app_name>.yaml and cfg/<pkg_name>.yaml config files."""


def _has_transport(modules: list[dict], module_map: dict, transport: str, mod_type: str) -> bool:
    """Check if any module in the list has the given transport and type."""
    for mod_name in modules:
        mod = module_map.get(mod_name, {})
        if mod.get("transport") == transport and mod["type"] == mod_type:
            return True
    return False


def _has_type(modules: list[str], module_map: dict, mod_type: str) -> bool:
    """Check if any module in the list has the given type."""
    for mod_name in modules:
        mod = module_map.get(mod_name, {})
        if mod["type"] == mod_type:
            return True
    return False


# ── Module type taxonomy (messaging) ────────────────────────────────────────

# direct_shm_* publish through the shm messaging driver, so their topics
# need the messaging block like any other messaging module.
_MSG_TYPES_ALL = {"publisher", "subscriber",
                  "ros2_interop_publisher", "ros2_interop_subscriber",
                  "direct_ros2_publisher", "direct_ros2_subscriber",
                  "direct_shm_publisher", "direct_shm_subscriber",
                  "loaned_publisher", "loaned_subscriber"}
_MSG_PUB_TYPES = {"publisher", "ros2_interop_publisher",
                  "direct_ros2_publisher", "direct_shm_publisher",
                  "loaned_publisher"}
_MSG_SUB_TYPES = {"subscriber", "ros2_interop_subscriber",
                  "direct_ros2_subscriber", "direct_shm_subscriber",
                  "loaned_subscriber"}


def _collect_topics(modules: list[str], module_map: dict) -> dict[str, set[str]]:
    """Collect unique topics and their roles (publisher/subscriber)."""
    topics = {}  # topic -> set of "publisher" | "subscriber"
    for mod_name in modules:
        mod = module_map.get(mod_name, {})
        if mod["type"] in _MSG_TYPES_ALL and "topic" in mod:
            topic = mod["topic"]
            if topic not in topics:
                topics[topic] = set()
            if mod["type"] in _MSG_PUB_TYPES:
                topics[topic].add("publisher")
            else:
                topics[topic].add("subscriber")
    return topics


def _needs_rpc(modules: list[str], module_map: dict) -> bool:
    """Check if any module needs RPC service infrastructure."""
    for mod_name in modules:
        mod = module_map.get(mod_name, {})
        if mod["type"] in ("rpc_server", "rpc_client"):
            return True
    return False


def _to_ros2_snake_case(name: str) -> str:
    """Convert a ROS2 CamelCase service/type name to snake_case."""
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


def _ros2_imported_srv_service_name(mod: dict) -> str | None:
    """Return the friendly ROS2 service name for an imported .srv module."""
    if mod.get("transport") != "ros2" or not mod.get("ros2_imported_srv_adapter"):
        return None
    if mod.get("type") not in ("rpc_server", "rpc_client"):
        return None
    service = mod.get("ros2_service")
    if not service:
        return None
    friendly = str(mod.get("ros2_service_name") or f"/{_to_ros2_snake_case(str(service))}")
    if not friendly.startswith("/"):
        friendly = f"/{friendly}"
    return friendly


def _collect_ros2_rpc_remaps(modules: list[str], module_map: dict, mod_type: str) -> list[tuple[str, str]]:
    """Collect (canonical_func_regex, friendly_ros2_name) remaps for imported .srv modules."""
    remaps: list[tuple[str, str]] = []
    seen: set[tuple[str, str]] = set()
    for mod_name in modules:
        mod = module_map.get(mod_name, {})
        if mod.get("type") != mod_type:
            continue
        friendly = _ros2_imported_srv_service_name(mod)
        if friendly is None:
            continue
        package = mod.get("ros2_package")
        service = mod.get("ros2_service")
        if not package or not service:
            continue
        # ServiceBase/RegisterClientFunc uses GetFullFuncName("ros2", "<pkg>/srv", "<Service>"),
        # i.e. "ros2:/<pkg>/srv/<Service>". Remapping this to "/set_mode" lets the ROS2
        # driver expose the stock service name instead of the encoded canonical path.
        func_regex = f"^ros2:/{package}/srv/{service}$"
        item = (func_regex, friendly)
        if item not in seen:
            seen.add(item)
            remaps.append(item)
    return remaps


def _needs_messaging(modules: list[str], module_map: dict) -> bool:
    """Check if any module needs messaging infrastructure."""
    for mod_name in modules:
        mod = module_map.get(mod_name, {})
        if mod["type"] in _MSG_TYPES_ALL:
            return True
    return False


def _has_any_transport(modules: list[str], module_map: dict, transport: str) -> bool:
    """Check if any module uses the given transport."""
    for mod_name in modules:
        mod = module_map.get(mod_name, {})
        if mod.get("transport") == transport:
            return True
    return False


def _get_messaging_driver(modules: list[str], module_map: dict) -> str:
    """Determine the primary messaging driver for an app's modules.

    Priority: iceoryx > shm > dds > local. The driver chosen here is wired into
    every topic's `enable_drivers:` list. (For mixed-transport apps the
    user must hand-edit the generated cfg or use multiple apps.)

    iceoryx and shm in one app is an error: the shm driver serves the same
    URLs as iceoryx (it is its drop-in replacement), and the priority would
    silently move the shm modules onto iceoryx.
    """
    drivers = {module_map.get(m, {}).get("transport") for m in modules}
    if "iceoryx" in drivers and "shm" in drivers:
        by_transport = {
            t: sorted(m for m in modules if module_map.get(m, {}).get("transport") == t)
            for t in ("iceoryx", "shm")
        }
        raise ValueError(
            "An app cannot mix iceoryx and shm modules (iceoryx: "
            f"{', '.join(by_transport['iceoryx'])}; shm: {', '.join(by_transport['shm'])}). "
            "The shm transport replaces iceoryx on the same topics; put the modules "
            "in separate apps or move them to one transport.")
    if "iceoryx" in drivers:
        return "iceoryx"
    if "shm" in drivers:
        return "shm"
    if "dds" in drivers:
        return "dds"
    return "local"


# ── Default DDS QoS config ──────────────────────────────────────────────────

_DEFAULT_DDS_QOS = {
    "history": "keep_last",
    "depth": 10,
    "reliability": "reliable",
    "durability": "volatile",
}


def _gen_dds_qos_block(qos: dict, indent: str) -> list[str]:
    """Generate a QoS YAML block at the given indentation level."""
    lines = []
    lines.append(f"{indent}qos:")
    for key in ("history", "depth", "reliability", "durability", "data_sharing"):
        if key in qos:
            lines.append(f"{indent}  {key}: {qos[key]}")
    return lines


# ── Logging section ─────────────────────────────────────────────────────────

def _gen_logging_section(project: dict, variant: dict | None = None) -> list[str]:
    """Generate the ibmw.logging section.

    Uses the project-level 'logging' section if present, otherwise defaults
    to console-only at INFO level (v1 behavior).

    Args:
        project: The project dict.
        variant: Optional per-app logging override (for multi-config demos).
    """
    log_cfg = variant if variant else project.get("logging")

    lines = []
    lines.append("  logging:")

    if not log_cfg:
        # Default: console-only at INFO
        lines.append("    core_level: INFO")
        lines.append("    backends:")
        lines.append("      - type: console")
        return lines

    core_level = log_cfg.get("core_level", "INFO")
    lines.append(f"    core_level: {core_level}")

    backends = log_cfg.get("backends", [{"type": "console"}])
    lines.append("    backends:")
    for be in backends:
        lines.append(f"      - type: {be['type']}")
        opts = be.get("options")
        if opts:
            lines.append("        options:")
            for k, v in opts.items():
                # Handle special quoting for pattern strings
                if k == "pattern" and isinstance(v, str):
                    lines.append(f'          {k}: "{v}"')
                else:
                    lines.append(f"          {k}: {v}")

    return lines


# ── Extensions section ──────────────────────────────────────────────────────

def _gen_extensions_section(project: dict, mod_names: list[str],
                            module_map: dict) -> list[str]:
    """Generate the ibmw.extensions section if needed.

    Automatically adds DDS / iceoryx / shm extension when any module uses the
    corresponding transport. Also merges user-specified extensions from
    the YAML. ROS2-compatible DDS topic/type options are emitted under
    ibmw.messaging.drivers[].options, which is consumed by DdsTransportDriver.
    """
    lines = []
    has_dds = _has_any_transport(mod_names, module_map, "dds")
    has_iceoryx = _has_any_transport(mod_names, module_map, "iceoryx")
    has_shm = _has_any_transport(mod_names, module_map, "shm")

    # Check if any module in this app uses ros2 RPC transport
    has_ros2_rpc = any(
        module_map.get(m, {}).get("transport") == "ros2" and
        module_map.get(m, {}).get("type") in ("rpc_server", "rpc_client")
        for m in mod_names
    )

    # Collect user-specified extensions
    user_ext = project.get("extensions", {}).get("items", [])

    # Auto-add extensions if not already specified by user
    dds_ext_needed = has_dds
    iceoryx_ext_needed = has_iceoryx
    shm_ext_needed = has_shm
    dds_ext_found = False
    iceoryx_ext_found = False
    shm_ext_found = False
    ros2_ext_found = False
    for ext in user_ext:
        if ext.get("name") == "dds_extension":
            dds_ext_found = True
        if ext.get("name") == "iceoryx_extension":
            iceoryx_ext_found = True
        if ext.get("name") == "shm_extension":
            shm_ext_found = True
        if ext.get("name") == "ros2_extension":
            ros2_ext_found = True

    if (not dds_ext_needed and not iceoryx_ext_needed and not shm_ext_needed
            and not has_ros2_rpc and not user_ext):
        return lines

    items = list(user_ext)
    if dds_ext_needed and not dds_ext_found:
        dds_driver = project.get("dds_driver", {})
        domain_id = dds_driver.get("domain_id", 0)
        ext_opts = {"domain_id": domain_id}
        items.insert(0, {
            "name": "dds_extension",
            "path": "libibmw_dds_transport.so",
            "options": ext_opts,
        })

    # Auto-add the iceoryx extension so the user
    # is responsible for starting RouDi externally; the cfg surfaces this
    # via the file-header usage note (added by _gen_config). Default
    # options omitted — iceoryx_extension defaults are reasonable.
    if iceoryx_ext_needed and not iceoryx_ext_found:
        iceoryx_driver = project.get("iceoryx_driver", {})
        ext_opts = dict(iceoryx_driver.get("extension_options", {}))
        items.insert(0, {
            "name": "iceoryx_extension",
            "path": "libibmw_iceoryx_transport.so",
            "options": ext_opts if ext_opts else None,
        })

    # Auto-add the shm extension. No daemon is needed; options come from
    # project shm_driver.extension_options (shm_extension keys such as
    # shm_init_size, domain, max_bytes_per_publisher), defaults otherwise.
    if shm_ext_needed and not shm_ext_found:
        shm_driver = project.get("shm_driver", {})
        ext_opts = dict(shm_driver.get("extension_options", {}))
        items.insert(0, {
            "name": "shm_extension",
            "path": "libibmw_shm_transport.so",
            "options": ext_opts if ext_opts else None,
        })

    # Auto-add ROS2 extension for ros2 RPC modules
    if has_ros2_rpc and not ros2_ext_found:
        ros2_ext_opts = {"node_name": project["project"]["_namespace_cpp"]}
        items.append({
            "name": "ros2_extension",
            "path": "libibmw_ros2_transport.so",
            "options": ros2_ext_opts,
        })

    if items:
        lines.append("")
        lines.append("  extensions:")
        lines.append("    items:")
        for ext in items:
            lines.append(f"      - name: {ext['name']}")
            lines.append(f"        path: {ext['path']}")
            opts = ext.get("options")
            if opts:
                lines.append("        options:")
                for k, v in opts.items():
                    if isinstance(v, bool):
                        lines.append(f"          {k}: {'true' if v else 'false'}")
                    elif isinstance(v, str):
                        lines.append(f'          {k}: "{v}"')
                    else:
                        lines.append(f"          {k}: {v}")

    return lines


# ── Scheduling / Executors section ──────────────────────────────────────────

def _gen_executors_section(project: dict, mod_names: list[str],
                           module_map: dict,
                           app: dict | None = None) -> list[str]:
    """Generate the ibmw.scheduling.executors section.

    If the app has an explicit 'executors' section, use it (per-app override).
    Else if the project has an explicit 'executors' section, use it verbatim.
    Otherwise, auto-derive executors from module requirements (v1 behavior).
    """
    explicit_execs = None
    if app:
        explicit_execs = app.get("executors")
    if not explicit_execs:
        explicit_execs = project.get("executors")

    if explicit_execs:
        # Emit the explicit executor list
        lines = []
        lines.append("")
        lines.append("  scheduling:")
        lines.append("    executors:")
        for ex in explicit_execs:
            lines.append(f"      - name: {ex['name']}")
            lines.append(f"        type: {ex['type']}")
            opts = ex.get("options")
            if opts:
                lines.append("        options:")
                for k, v in opts.items():
                    # Handle list values (e.g. thread_bind_cpu: [0, 1])
                    if isinstance(v, list):
                        lines.append(f"          {k}: {v}")
                    elif isinstance(v, str) and ":" in v:
                        # e.g. thread_sched_policy: "SCHED_FIFO:80"
                        lines.append(f'          {k}: "{v}"')
                    else:
                        lines.append(f"          {k}: {v}")
        return lines

    # Auto-derive executors from module requirements (v1 behavior)
    needs_msg = _needs_messaging(mod_names, module_map)
    needs_rpc = _needs_rpc(mod_names, module_map)

    if not needs_msg and not needs_rpc:
        return []

    lines = []
    lines.append("")
    lines.append("  scheduling:")
    lines.append("    executors:")

    if needs_rpc:
        # DDS RPC and JSON RPC need time_schedule_executor;
        # ROS2 RPC does not.
        has_dds_rpc = any(
            module_map[m].get("transport") == "dds" and
            module_map[m]["type"] in ("rpc_server", "rpc_client")
            for m in mod_names
        )
        has_json_rpc = any(
            module_map[m].get("transport") == "json" and
            module_map[m]["type"] in ("rpc_server", "rpc_client")
            for m in mod_names
        )
        if has_dds_rpc or has_json_rpc:
            lines.append("      - name: time_schedule_executor")
            lines.append("        type: thread_pool")
            lines.append("        options:")
            lines.append("          thread_num: 1")

        lines.append("      - name: work_thread_pool")
        lines.append("        type: thread_pool")
        lines.append("        options:")
        # ROS2 RPC needs 4 threads, others need 2
        has_ros2_rpc = any(
            module_map[m].get("transport") == "ros2" and
            module_map[m]["type"] in ("rpc_server", "rpc_client")
            for m in mod_names
        )
        if has_ros2_rpc:
            lines.append("          thread_num: 4")
        else:
            lines.append("          thread_num: 2")

        lines.append("      - name: timeout_handle")
        lines.append("        type: timer")
        lines.append("        options:")
        lines.append("          bind_executor: work_thread_pool")
    else:
        lines.append("      - name: work_thread_pool")
        lines.append("        type: thread_pool")
        lines.append("        options:")
        lines.append("          thread_num: 2")

    return lines


# ── DDS messaging driver config ─────────────────────────────────────────────

def _gen_dds_driver_options(project: dict, mod_names: list[str],
                            module_map: dict) -> list[str]:
    """Generate the DDS driver config block for ibmw.messaging.drivers.

    When the app contains ros2_interop or loaned_publisher modules using
    DDS, the ROS2-compatible mode (rt/ topic prefix + ROS2 wire format)
    is required for interop with the ros2 wire used by Ros2StylePublisher.
    """
    dds_driver = project.get("dds_driver", {})
    pub_qos = dict(_DEFAULT_DDS_QOS)
    sub_qos = dict(_DEFAULT_DDS_QOS)

    # Override defaults from dds_driver section
    for key in ("history", "depth", "reliability", "durability", "data_sharing"):
        if key in dds_driver:
            pub_qos[key] = dds_driver[key]
            sub_qos[key] = dds_driver[key]

    # ros2_compatible is needed for direct/legacy ros2 interop and DDS
    # loaned_publisher paths (they use ROS2 wire format).
    needs_ros2_compat = any(
        module_map.get(m, {}).get("_is_ros2_interop")
        or module_map.get(m, {}).get("type") in ("direct_ros2_publisher", "direct_ros2_subscriber")
        or module_map.get(m, {}).get("type") in ("loaned_publisher", "loaned_subscriber")
        for m in mod_names
    )
    ros2_compat = dds_driver.get("ros2_compatible_mode", needs_ros2_compat)
    ros2_prefix = dds_driver.get("ros2_topic_prefix", "rt")

    lines = []
    lines.append("      - type: dds")
    lines.append("        options:")
    if ros2_compat:
        lines.append(f"          ros2_compatible: true")
        lines.append(f'          ros2_topic_prefix: "{ros2_prefix}"')
    lines.append("          publishers:")
    lines.append('            - topic: "(.*)"')
    lines.extend(_gen_dds_qos_block(pub_qos, "              "))
    lines.append("          subscribers:")
    lines.append('            - topic: "(.*)"')
    lines.extend(_gen_dds_qos_block(sub_qos, "              "))
    return lines


# ── Custom config passthrough ───────────────────────────────────────────────

def _gen_custom_config_sections(app: dict, module_map: dict) -> list[str]:
    """Generate root-level custom config sections for modules.

    Modules with a 'custom_config' dict in their YAML definition get a
    top-level YAML block keyed by module name, outside the ibmw: tree.
    This is read by the module via core_.GetConfigurator().GetConfigFilePath().
    """
    lines = []
    for mod_name in app["modules"]:
        mod = module_map.get(mod_name, {})
        cc = mod.get("custom_config")
        if cc and isinstance(cc, dict):
            lines.append("")
            lines.append(f"{mod_name}:")
            for k, v in cc.items():
                if isinstance(v, list):
                    lines.append(f"  {k}: {v}")
                elif isinstance(v, str):
                    lines.append(f'  {k}: "{v}"')
                else:
                    lines.append(f"  {k}: {v}")
    return lines


# ── Module section with log_level passthrough ───────────────────────────────

def _gen_modules_section(app: dict, module_map: dict, is_pkg: bool) -> list[str]:
    """Generate the ibmw.modules section."""
    lines = []
    lines.append("")
    lines.append("  modules:")

    if is_pkg:
        pkg_name = app["_pkg_name"]
        lines.append("    packages:")
        lines.append(f"      - path: lib{pkg_name}.so")

        # enable_modules: list of module names
        mod_names = app["modules"]
        lines.append(f"        enable_modules: [{', '.join(mod_names)}]")

    lines.append("    modules:")
    for mod_name in app["modules"]:
        mod = module_map.get(mod_name, {})
        lines.append(f"      - name: {mod_name}")
        # Use module-level log_level if specified, otherwise default to INFO
        log_level = mod.get("log_level", "INFO")
        lines.append(f"        log_level: {log_level}")

    return lines


# ── Main config generator ───────────────────────────────────────────────────

def _gen_config(app: dict, project: dict, is_pkg: bool,
                logging_variant: dict | None = None,
                config_suffix: str = "") -> str:
    """Generate a YAML config file for an app.

    Args:
        app: The app dict from the project.
        project: The full project dict.
        is_pkg: Whether to generate Pkg mode config.
        logging_variant: Optional logging config override for multi-config demos.
        config_suffix: Optional suffix for the config file comment.
    """
    module_map = project["_module_map"]
    mod_names = app["modules"]
    app_name = app["_snake_name"]

    if is_pkg:
        pkg_name = app["_pkg_name"]
        usage_line = f"# Usage: ibmw run cfg/{pkg_name}.yaml"
    else:
        usage_line = f"# Usage: ./{app_name} cfg/{app_name}.yaml"

    desc = f"{'Pkg' if is_pkg else 'App'} mode"
    if config_suffix:
        desc = f"{desc}, {config_suffix}"

    # Detect iceoryx transport so we can surface the RouDi prerequisite
    # in the file header, keeping the generated configuration self-describing.
    has_iceoryx = _has_any_transport(mod_names, module_map, "iceoryx")

    lines = []
    lines.append(f"# IBMW config for {app_name} ({desc})")
    lines.append("#")
    lines.append(usage_line)
    if has_iceoryx:
        lines.append("#")
        lines.append("# Prerequisite: iceoryx RouDi must be running first, e.g.:")
        lines.append("#   nohup ./iox-roudi > /tmp/roudi.log 2>&1 &")
        lines.append("# (kill via:  ps -ef | grep iox-roudi  +  kill -9 <PID>)")
    lines.append("")
    lines.append("ibmw:")

    # Logging section
    lines.extend(_gen_logging_section(project, logging_variant))

    # Extensions section (DDS extension, etc.)
    lines.extend(_gen_extensions_section(project, mod_names, module_map))

    # Scheduling / Executors section
    lines.extend(_gen_executors_section(project, mod_names, module_map, app))

    # Messaging section
    needs_msg = _needs_messaging(mod_names, module_map)
    needs_rpc = _needs_rpc(mod_names, module_map)
    driver = _get_messaging_driver(mod_names, module_map)

    if needs_msg:
        topics = _collect_topics(mod_names, module_map)
        lines.append("")
        lines.append("  messaging:")
        lines.append("    drivers:")

        if driver == "dds":
            lines.extend(_gen_dds_driver_options(project, mod_names, module_map))
        elif driver == "iceoryx":
            # Declare the type only; iceoryx driver defaults
            # are reasonable. Users can hand-edit options when needed.
            lines.append("      - type: iceoryx")
        elif driver == "shm":
            # Declare the type only, as for iceoryx; the driver name is
            # shm_extension's default driver_name.
            lines.append("      - type: shm")
        else:
            lines.append("      - type: local")

        # Publishers
        pub_topics = [t for t, roles in topics.items() if "publisher" in roles]
        if pub_topics:
            lines.append("    publishers:")
            for t in pub_topics:
                lines.append(f'      - topic: "{t}"')
                lines.append(f"        enable_drivers: [{driver}]")

        # Subscribers
        sub_topics = [t for t, roles in topics.items() if "subscriber" in roles]
        if sub_topics:
            lines.append("    subscribers:")
            for t in sub_topics:
                lines.append(f'      - topic: "{t}"')
                lines.append(f"        enable_drivers: [{driver}]")

    # Service section
    if needs_rpc:
        # Determine which service drivers are needed
        has_ros2_rpc = any(
            module_map[m].get("transport") == "ros2" and
            module_map[m]["type"] in ("rpc_server", "rpc_client")
            for m in mod_names
        )
        has_dds_rpc = any(
            module_map[m].get("transport") == "dds" and
            module_map[m]["type"] in ("rpc_server", "rpc_client")
            for m in mod_names
        )

        lines.append("")
        lines.append("  service:")
        lines.append("    drivers:")

        if has_dds_rpc:
            dds_driver_cfg = project.get("dds_driver", {})
            ros2_compat = dds_driver_cfg.get("ros2_compatible_mode", True)
            lines.append("      - type: dds")
            lines.append("        options:")
            lines.append(f"          ros2_compatible: {'true' if ros2_compat else 'false'}")
            lines.append("          timeout_executor: timeout_handle")

        if has_ros2_rpc:
            ros2_client_remaps = _collect_ros2_rpc_remaps(mod_names, module_map, "rpc_client")
            ros2_server_remaps = _collect_ros2_rpc_remaps(mod_names, module_map, "rpc_server")
            lines.append("      - type: ros2")
            lines.append("        options:")
            lines.append("          timeout_executor: timeout_handle")
            if ros2_client_remaps:
                lines.append("          clients:")
                for func_name, remapping_rule in ros2_client_remaps:
                    lines.append(f'            - func_name: "{func_name}"')
                    lines.append(f'              remapping_rule: "{remapping_rule}"')
            if ros2_server_remaps:
                lines.append("          servers:")
                for func_name, remapping_rule in ros2_server_remaps:
                    lines.append(f'            - func_name: "{func_name}"')
                    lines.append(f'              remapping_rule: "{remapping_rule}"')

        lines.append("      - type: local")
        lines.append("        options:")
        lines.append("          timeout_executor: timeout_handle")

        # Build enable_drivers list
        rpc_drivers = []
        if has_dds_rpc:
            rpc_drivers.append("dds")
        if has_ros2_rpc:
            rpc_drivers.append("ros2")
        rpc_drivers.append("local")
        driver_list = ", ".join(rpc_drivers)

        lines.append("    clients:")
        lines.append('      - func_name: "(.*)"')
        lines.append(f"        enable_drivers: [{driver_list}]")
        lines.append("    servers:")
        lines.append('      - func_name: "(.*)"')
        lines.append(f"        enable_drivers: [{driver_list}]")

    # Modules section
    lines.extend(_gen_modules_section(app, module_map, is_pkg))

    # Custom config sections (root-level, outside ibmw:)
    lines.extend(_gen_custom_config_sections(app, module_map))

    lines.append("")
    return "\n".join(lines)


# ── Multi-config variant support ────────────────────────────────────────────

def _gen_config_variants(app: dict, project: dict, is_pkg: bool) -> list[tuple[str, str]]:
    """Generate config variants if the app has logging_variants defined.

    Returns list of (rel_path, content) tuples.
    """
    variants = app.get("config_variants")
    if not variants:
        return []

    results = []
    for var in variants:
        suffix = var.get("suffix", "")
        logging_override = var.get("logging")

        if is_pkg:
            base = app["_pkg_name"]
        else:
            base = app["_snake_name"]

        filename = f"{base}_{suffix}.yaml" if suffix else f"{base}.yaml"

        cfg = _gen_config(app, project, is_pkg,
                          logging_variant=logging_override,
                          config_suffix=suffix)
        results.append((f"cfg/{filename}", cfg))

    return results


def generate_configs(project: dict) -> list[tuple[str, str]]:
    """Generate YAML config files for all apps (app + pkg variants).

    Returns list of (rel_path, content) tuples.
    """
    results = []

    for app in project.get("apps", []):
        app_name = app["_snake_name"]
        pkg_name = app["_pkg_name"]

        # App config (default)
        app_cfg = _gen_config(app, project, is_pkg=False)
        results.append((f"cfg/{app_name}.yaml", app_cfg))

        # Pkg config (default)
        pkg_cfg = _gen_config(app, project, is_pkg=True)
        results.append((f"cfg/{pkg_name}.yaml", pkg_cfg))

        # Config variants (if defined)
        results.extend(_gen_config_variants(app, project, is_pkg=False))
        results.extend(_gen_config_variants(app, project, is_pkg=True))

    return results
