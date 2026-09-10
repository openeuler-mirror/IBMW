# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Transport plugin registry for ibmw_gen.
#
# Each transport (dds / iceoryx / ros2 / json) lives in its own module
# under this package and exposes a `register()` callable that returns a
# TransportSpec. This package auto-discovers and loads all sibling
# modules at import time, populating REGISTRY.
#
# Design intent (see docs/architecture/overview.md):
#   * Replace the closed enum schema.VALID_TRANSPORTS with an open
#     plugin registry so adding a new transport never touches the
#     framework core.
#   * Each plugin owns schema validation hooks, code-emission entry points,
#     and any transport-specific helpers.
#   * Schema readers use `transports.is_registered(name)` so adding a plugin
#     does not require modifying a closed transport enum.

"""Transport plugin registry for ibmw_gen."""

from __future__ import annotations

import importlib
import pkgutil
from dataclasses import dataclass, field
from typing import Callable


AnyCallable = Callable[..., object]


@dataclass(frozen=True)
class TransportSpec:
    """Descriptor returned by a transport plugin's register().

    Attributes:
        name:
            Canonical lowercase identifier used in user yaml
            (`transport: dds` etc.) and in the generator dispatch map.
        validate_module:
            Optional callable invoked during schema validation when
            `mod['transport'] == self.name`. Receives the module dict
            (already with name/type/transport keys present) and the
            full parsed yaml `data`. May call schema._error/_warn.
            Stubs return None, adding no checks beyond what
            schema._validate_modules already does. Full transport-specific
            validators belong here.
        emit_module:
            Optional callable invoked by the generator to emit
            the transport-specific portion of a module. Receives the
            module dict and a generator context.
    """

    name: str
    validate_module: AnyCallable | None = None
    emit_module: AnyCallable | None = None

    # ── Unified publisher typesupport hook ───────────────────────────────
    # Fine-grained emit hooks that support the unified-template path in
    # module_gen._gen_publisher_unified / _gen_subscriber_unified / ...
    #
    # When present, the generator prefers these over emit_module so
    # user-facing <module>.{h,cc} stays transport-agnostic (R1/R2/R3/
    # R9/R11) and transport-specific details move into the generated
    # <module>_typesupport.inc.{h,cc}.
    #
    # emit_publisher_typesupport: called for (type='publisher',
    #   transport=self.name). Returns a dict with keys:
    #     * "msg_type_cpp"       - e.g. "ibmw::protocols::example::HelloMsg"
    #     * "inc_h_content"      - full text for <module>_typesupport.inc.h
    #     * "inc_cc_content"     - full text for <module>_typesupport.inc.cc
    #     * "register_call"      - C++ expression injected in Setup() to
    #                              publish-side register (e.g.
    #                              "access_->RegisterSendType()")
    #     * "publish_stmt"       - C++ statement injected in PublishLoop
    #                              loop body (e.g. "access_->Send(msg);")
    #   OR returns None to indicate the unified path is not supported
    #   for this (mod) and the legacy emit_module path should be used.
    emit_publisher_typesupport: AnyCallable | None = None

    # ── Unified subscriber typesupport hook ──────────────────────────────
    # emit_subscriber_typesupport: called for (type='subscriber',
    #   transport=self.name). Returns a dict with keys:
    #     * "msg_type_cpp"       - fully-qualified message C++ type, used
    #                              both for the lambda parameter typing
    #                              and for the ReceiverAccess<T> alias
    #     * "inc_h_content"      - full text for <module>_typesupport.inc.h
    #                              (transport includes + IBMW_TS_RECEIVER_*
    #                              macros)
    #     * "inc_cc_content"     - full text for <module>_typesupport.inc.cc
    #                              (empty stub today; reserved for future
    #                              transport-specific TU code)
    #   OR returns None to defer to the legacy emit_module path.
    #
    # Unlike publisher, subscriber's ReceiverAccess<T> is kept as a
    # stack-local in Setup(), preserving the established lifetime pattern.
    # The inc.h provides:
    #   * IBMW_TS_RECEIVER_MSG_TYPE   — C++ type used for the lambda param
    #   * IBMW_TS_RECEIVER_ACCESS_SETUP(core_expr, access_var)
    #                                 — defines `access_var` (on the stack)
    #                                   bound to the configured topic
    emit_subscriber_typesupport: AnyCallable | None = None

    # ── Unified loaned-subscriber typesupport hook ───────────────────────
    # emit_loaned_subscriber_typesupport: called for
    # (type='loaned_subscriber', transport=self.name). Returns a dict
    # with keys:
    #     * "msg_type_cpp"       - fully-qualified message C++ type
    #     * "inc_h_content"      - full text for <module>_typesupport.inc.h
    #                              (transport includes + IBMW_TS_LOANED_SUB_*
    #                              macros)
    #     * "inc_cc_content"     - full text for <module>_typesupport.inc.cc
    #                              (default OnMessage impl body — contains
    #                              the `on_message_body` user-code marker)
    #   OR returns None to defer to the legacy emit_module path.
    #
    # Signatures intentionally diverge between plugins:
    #   * dds:     virtual void OnMessage(const std::shared_ptr<const T>& msg)
    #   * iceoryx: virtual void OnMessage(const T& msg, std::size_t size)
    # The inc.h provides macros to absorb the signature difference:
    #   * IBMW_TS_LOANED_SUB_STATIC_ASSERT        — iceoryx: POD check;
    #                                               dds: empty
    #   * IBMW_TS_LOANED_SUB_ON_MESSAGE_DECL      — virtual method decl
    #   * IBMW_TS_LOANED_SUB_ATTACH(core_expr)    — Setup body binding
    emit_loaned_subscriber_typesupport: AnyCallable | None = None

    # ── Unified loaned-publisher typesupport hook ────────────────────────
    # emit_loaned_publisher_typesupport: called for
    # (type='loaned_publisher', transport=self.name). Returns a dict
    # with keys:
    #     * "msg_type_cpp"       - fully-qualified message C++ type
    #     * "inc_h_content"      - full text for <module>_typesupport.inc.h
    #                              (public TypeSupport binding + the seven
    #                              IBMW_TS_LOANED_PUB_* macros below)
    #     * "inc_cc_content"     - full text for <module>_typesupport.inc.cc
    #                              (today: namespace stub + populate marker
    #                              comment; reserved for future out-of-line
    #                              transport TU code)
    #   OR returns None to defer to the legacy emit_module path.
    #
    # Plugin contract — the inc.h MUST `#define` all seven macros below.
    # User-facing <module>.{h,cc} only references these macros; it never
    # names a transport/driver type directly (R1/R2/R3/R9/R11).
    #
    #   * IBMW_TS_LOANED_PUB_MSG_TYPE
    #         inc.h-private alias for the chosen C++ message type.
    #   * IBMW_TS_LOANED_PUB_STATIC_ASSERT
    #         expands at top of class body and enforces trivially-copyable
    #         storage for the strict in-place loan contract.
    #   * IBMW_TS_LOANED_PUB_MEMBER
    #         expands inside `private:` and declares a public
    #         SenderAccess<T> handle.
    #   * IBMW_TS_LOANED_PUB_SETUP(core_expr)
    #         expands inside Setup() body. Performs GetPublisher,
    #         RegisterSendType<T>, and capability setup where required.
    #   * IBMW_TS_LOANED_PUB_RUN_INIT(core_expr)
    #         expands inside Run() before the publish thread is launched.
    #         Validates that the public publisher handle was initialized.
    #   * IBMW_TS_LOANED_PUB_HOT_LOOP_BODY(loop_var)
    #         expands as the WHOLE body of the PublishLoop while-loop
    #         through SenderAccess<T>::Loan/Publish. The user-code marker
    #         `populate_loaned_message`
    #         (a double-comment marker hint pair) sits
    #         inside the macro body just before the publish call.
    #   * IBMW_TS_LOANED_PUB_PRINT_STATS
    #         expands inside PrintStatistics body with transport-neutral
    #         strict-loan counters.
    emit_loaned_publisher_typesupport: AnyCallable | None = None

    # ── Unified RPC typesupport hooks ────────────────────────────────────
    # emit_rpc_server_typesupport: called for (type='rpc_server',
    #   transport=self.name). Returns a dict with keys:
    #     * "msg_type_cpp"   - fully-qualified request/response type set
    #                          (per-method or service-level, plugin choice)
    #     * "inc_h_content"  - full text for <module>_typesupport.inc.h
    #                          (transport includes + IBMW_TS_RPC_SERVER_*
    #                          macros: SETUP(core_expr), HANDLER_DECL,
    #                          HANDLER_IMPL(name))
    #     * "inc_cc_content" - full text for <module>_typesupport.inc.cc
    #                          (out-of-line handler bodies)
    #   OR returns None to defer to the legacy emit_module path
    #   (e.g. ROS2 skeleton fallback when ros2_service is absent).
    #
    # Plugin contract — the inc.h MUST `#define` the following macros so
    # the user-facing <module>.{h,cc} stays transport-agnostic
    # (R1/R2/R3/R9/R11):
    #
    #   * IBMW_TS_RPC_SERVER_SETUP(core_expr)
    #         expands inside Setup() body. Performs service registration
    #         (DDS: register_service<Req,Rsp>; ROS2: CreateService;
    #         JSON: bind handler to topic) using `core_expr` for the
    #         core handle.
    #   * IBMW_TS_RPC_SERVER_HANDLER_DECL
    #         expands inside `private:` and declares the per-method
    #         handler signature(s). Transport-specific request/response
    #         types are absorbed inside the macro expansion.
    #   * IBMW_TS_RPC_SERVER_HANDLER_IMPL(name)
    #         expands at namespace scope in <module>.cc to provide the
    #         out-of-line handler body. `name` is the C++ class name.
    emit_rpc_server_typesupport: AnyCallable | None = None

    # emit_rpc_client_typesupport: called for (type='rpc_client',
    #   transport=self.name). Returns a dict with keys:
    #     * "msg_type_cpp"   - fully-qualified request/response type set
    #     * "inc_h_content"  - inc.h with IBMW_TS_RPC_CLIENT_LOOP_DECL
    #                          (DDS: coroutine form `co::Task<void> RpcLoop()`;
    #                           ROS2/JSON: `void RpcLoop()` thread form).
    #                          Transport private-member declarations
    #                          (client handle, executor binding) live in
    #                          the same macro expansion.
    #     * "inc_cc_content" - inc.cc with IBMW_TS_RPC_CLIENT_LOOP_IMPL(name)
    #                          out-of-line definition. The macro absorbs
    #                          the coroutine-vs-thread divergence so the
    #                          user-facing .cc only writes
    #                          `IBMW_TS_RPC_CLIENT_LOOP_IMPL(MyClient)`.
    #   OR returns None to defer to legacy emit_module.
    #
    # Plugin contract — the inc.h MUST `#define`:
    #
    #   * IBMW_TS_RPC_CLIENT_LOOP_DECL
    #         expands inside `public:` (DDS) or `private:` (ROS2/JSON)
    #         and declares the loop method with the transport-appropriate
    #         signature. Plugin chooses access level via the macro
    #         expansion site comment.
    #   * IBMW_TS_RPC_CLIENT_LOOP_IMPL(name)
    #         expands at namespace scope in <module>.cc. Body composes
    #         the request, calls the transport client primitive
    #         (DDS: co_await client->call_async; ROS2/JSON:
    #         client.SyncCall + sleep), and logs the response.
    emit_rpc_client_typesupport: AnyCallable | None = None

    # ── Marker-preservation metadata ─────────────────────────────────────
    # The scaffold freezes the cross-transport RPC marker grammar. Defaults
    # remain empty so generators without marker support stay byte-stable.
    #
    # Canonical marker stems are listed in src/api/cpp/rpc_marker.h.
    # Emitters combine a marker stem with marker_anchor_format to form the
    # BEGIN/END anchor pair they will preserve round-trip.
    rpc_server_markers: list[str] = field(default_factory=list)
    rpc_client_markers: list[str] = field(default_factory=list)
    marker_anchor_format: str = "// {marker}_{edge}"

    # Free-form descriptor for plugin metadata (version, capability
    # flags, etc.). Kept as a plain dict for extension headroom.
    meta: dict[str, object] = field(default_factory=dict)


# ── Registry ────────────────────────────────────────────────────────────────

_REGISTRY: dict[str, TransportSpec] = {}


def register(spec: object) -> None:
    """Register a transport plugin. Called by each plugin module."""
    if not isinstance(spec, TransportSpec):
        raise TypeError(
            f"register() expects TransportSpec, got {type(spec).__name__}"
        )
    if spec.name in _REGISTRY:
        # Allow re-registration only if identical (defensive against
        # double-import in tests). Otherwise treat as user error.
        existing = _REGISTRY[spec.name]
        if existing is spec:
            return
        raise ValueError(
            f"Transport '{spec.name}' already registered with a different spec object; "
            + "refuse to overwrite"
        )
    _REGISTRY[spec.name] = spec


def is_registered(name: str) -> bool:
    """Return True iff a transport plugin with this name is loaded."""
    return name in _REGISTRY


def get(name: str) -> TransportSpec | None:
    """Look up a transport spec by name; returns None if absent."""
    return _REGISTRY.get(name)


def names() -> set[str]:
    """Return the set of currently registered transport names."""
    return set(_REGISTRY.keys())


def _autoload() -> None:
    """Import every sibling module so each can call register()."""
    for mod in pkgutil.iter_modules(__path__):
        if mod.name.startswith("_"):
            continue
        _ = importlib.import_module(f"{__name__}.{mod.name}")


_autoload()
