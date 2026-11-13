#!/usr/bin/env python3
# pyright: reportImplicitRelativeImport=false, reportMissingTypeArgument=false, reportOptionalCall=false, reportArgumentType=false
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Module code generator for ibmw-gen.
# Generates .h and .cc files for each module based on its type and transport.

"""Generate src/<module_dir>/<module>.h and .cc files."""

import importlib

# Shared snake_case and copyright helpers live in generators/_common.py
# so transport plugins (transports/*.py) can use them too.
from generators._common import _snake, _COPYRIGHT  # noqa: F401
from generators.user_code import apply_user_code


# ── Unified publisher skeleton ──────────────────────────────────────────────
#
# Transport-agnostic publisher template used by every plugin that opts in
# via TransportSpec.emit_publisher_typesupport. The user-facing
# <module>.{h,cc} is transport-agnostic (no transport header include,
# no transport-namespace type in the class body; Setup/Run/Stop/
# PublishLoop structure is identical across transports). Everything
# transport-specific is emitted into <module>_typesupport.inc.{h,cc}
# which the main .cc includes verbatim, avoiding any cmake_gen changes.

def _gen_publisher_unified_header(mod: dict, ns: str) -> str:
    """Transport-agnostic publisher header.

    R1/R2/R3/R9: includes only `cpp/module_base.h` and the module's
    own `<name>_typesupport.inc.h`. No dds/ros2/json transport headers
    leak into the user-facing surface.
    """
    name = mod["name"]
    snake = _snake(name)
    return f"""{_COPYRIGHT}
#pragma once

#include <atomic>
#include <optional>
#include <thread>

#include "cpp/module_base.h"

// Transport-specific typesupport lives in the generated inc.h.
// The user-facing template remains transport-agnostic.
#include "{snake}_typesupport.inc.h"

namespace {ns} {{

class {name} : public ibmw::Module {{
 public:
  {name}() = default;
  ~{name}() override = default;

  ibmw::ModuleDescriptor Info() const override {{
    return ibmw::ModuleDescriptor{{.module_name = "{name}"}};
  }}

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();
  auto GetLogger() {{ return core_.GetLogger(); }}

  ibmw::Runtime core_;
  // IBMW_TS_SENDER_ACCESS_MEMBER is defined by the generated inc.h to
  // the transport-specific SenderAccess<T> specialization. User code
  // only ever refers to `access_` through the plugin-provided publish
  // statement.
  IBMW_TS_SENDER_ACCESS_MEMBER access_;
  uint32_t msg_count_ = 0;
  std::atomic<bool> running_{{false}};
  std::thread publish_thread_;
}};

}}  // namespace {ns}
"""


def _gen_publisher_unified_cc(mod: dict, ns: str, snake_dir: str,
                              ts_info: dict) -> str:
    """Transport-agnostic publisher .cc.

    R1/R2: no transport headers here either; all transport-specific
    mechanics live in the included `<name>_typesupport.inc.cc`.
    `ts_info` keys (from plugin emit_publisher_typesupport):
      * msg_type_cpp  — fully-qualified message C++ type
      * register_call — expression returning bool
      * publish_stmt  — statement using local `msg` of type msg_type_cpp
    """
    name = mod["name"]
    snake = _snake(name)
    msg_type_cpp = ts_info["msg_type_cpp"]
    register_call = ts_info["register_call"]
    publish_stmt = ts_info["publish_stmt"]
    return f"""{_COPYRIGHT}
#include "{snake_dir}/{snake}.h"

#include <chrono>
#include <thread>

// Include the transport-specific typesupport translation unit.
// This header is produced by the transport plugin
// and carries every dds/ros2/json-specific symbol referenced below
// (type aliases, RegisterSendType<T>, publisher free functions).
#include "{snake_dir}/{snake}_typesupport.inc.cc"

namespace {ns} {{

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;

  // Plugin-injected: acquire the publisher handle and bind the
  // transport-specific SenderAccess<T>. The macro is defined in
  // <name>_typesupport.inc.h so user code never needs to name a
  // transport-coupled type.
  IBMW_TS_SENDER_ACCESS_INIT(access_, core_);

  if (!({register_call})) {{
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "Register publish type failed!");
    return false;
  }}

  MW_INFO("{name}::Setup succeeded.");
  return true;
}}

bool {name}::Run() {{
  running_ = true;
  publish_thread_ = std::thread([this]() {{ PublishLoop(); }});
  MW_INFO("{name}::Run succeeded.");
  return true;
}}

void {name}::Stop() {{
  running_ = false;
  if (publish_thread_.joinable()) {{
    publish_thread_.join();
  }}
  MW_INFO("{name}::Stop succeeded.");
}}

void {name}::PublishLoop() {{
  while (running_) {{
    {msg_type_cpp} msg;
    // IBMW_PUBSUB_PUBLISHER_BODY_BEGIN
    // IBMW: user code begin (publish_body)
    // TODO: populate msg fields here
    msg_count_++;
    // IBMW: user code end (publish_body)
    // IBMW_PUBSUB_PUBLISHER_BODY_END

    // Plugin-injected publish statement (transport-neutral call
    // surface from the user's perspective).
    {publish_stmt}

    MW_INFO("Published message #{{}}", msg_count_);

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }}
}}

}}  // namespace {ns}
"""


def _gen_publisher_unified(mod: dict, ns: str, mod_dir: str,
                           ts_info: dict) -> list[tuple[str, str]]:
    """Glue: emit all 4 files for a plugin-claimed publisher module.

    Returns:
      [(path_h, content_h), (path_cc, content_cc),
       (path_inc_h, content_inc_h), (path_inc_cc, content_inc_cc)]
    """
    name = mod["name"]
    snake = _snake(name)
    hdr = _gen_publisher_unified_header(mod, ns)
    src = _gen_publisher_unified_cc(mod, ns, mod_dir, ts_info)
    inc_h = ts_info["inc_h_content"]
    inc_cc = ts_info["inc_cc_content"]
    return [
        (f"src/{mod_dir}/{snake}.h", hdr),
        (f"src/{mod_dir}/{snake}.cc", src),
        (f"src/{mod_dir}/{snake}_typesupport.inc.h", inc_h),
        (f"src/{mod_dir}/{snake}_typesupport.inc.cc", inc_cc),
    ]


# ── Unified subscriber skeleton ────────────────────────────────────────────
#
# Subscriber counterpart of the publisher unified template. User-facing
# <module>.{h,cc} is transport-agnostic; transport specifics live in
# <module>_typesupport.inc.{h,cc} produced by the plugin.
#
# The established lifetime pattern is preserved: ReceiverAccess<T> is
# a stack local in Setup() because OnMessage() registers the callback
# with the underlying subscriber, which owns the closure for the
# process lifetime. No access_ member is needed.

def _gen_subscriber_unified_header(mod: dict, ns: str) -> str:
    """Transport-agnostic subscriber header.

    R1/R2/R3: includes only `cpp/module_base.h` and the module's own
    `<name>_typesupport.inc.h`. No dds/ros2/json transport headers leak
    into the user-facing surface.
    """
    name = mod["name"]
    snake = _snake(name)
    return f"""{_COPYRIGHT}
#pragma once

#include "cpp/module_base.h"

// Transport-specific typesupport lives in the generated inc.h.
// The user-facing template remains transport-agnostic.
#include "{snake}_typesupport.inc.h"

namespace {ns} {{

class {name} : public ibmw::Module {{
 public:
  {name}() = default;
  ~{name}() override = default;

  ibmw::ModuleDescriptor Info() const override {{
    return ibmw::ModuleDescriptor{{.module_name = "{name}"}};
  }}

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  auto GetLogger() {{ return core_.GetLogger(); }}

  ibmw::Runtime core_;
}};

}}  // namespace {ns}
"""


def _gen_subscriber_unified_cc(mod: dict, ns: str, snake_dir: str,
                               ts_info: dict) -> str:
    """Transport-agnostic subscriber .cc.

    R1/R2: no transport headers here; all transport-specific mechanics
    live in the included `<name>_typesupport.inc.cc`.

    `ts_info` keys (from plugin emit_subscriber_typesupport):
      * msg_type_cpp — fully-qualified message C++ type, used for
                       the lambda parameter type and ReceiverAccess<T>
                       alias (via IBMW_TS_RECEIVER_MSG_TYPE macro)
    """
    name = mod["name"]
    snake = _snake(name)
    return f"""{_COPYRIGHT}
#include "{snake_dir}/{snake}.h"

// Include the transport-specific typesupport translation unit.
// This header is produced by the transport plugin
// and carries every dds/ros2/json-specific symbol referenced below
// (type aliases, ReceiverAccess<T>, subscriber free functions).
#include "{snake_dir}/{snake}_typesupport.inc.cc"

namespace {ns} {{

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;

  // Plugin-injected: produce a stack-local `access` bound to the
  // transport-specific ReceiverAccess<T> specialization. The macro is
  // defined in <name>_typesupport.inc.h so user code never needs to
  // name a transport-coupled type.
  IBMW_TS_RECEIVER_ACCESS_SETUP(core_, access);

  bool ret = access.OnMessage(
      [this](const std::shared_ptr<const IBMW_TS_RECEIVER_MSG_TYPE>& msg) {{
        // IBMW_PUBSUB_SUBSCRIBER_BODY_BEGIN
        // IBMW: user code begin (on_message_body)
        MW_INFO("Received message.");
        // TODO: process msg fields here
        (void)msg;
        // IBMW: user code end (on_message_body)
        // IBMW_PUBSUB_SUBSCRIBER_BODY_END
      }});

  if (!ret) {{
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "Subscribe failed!");
    return false;
  }}

  MW_INFO("{name}::Setup succeeded.");
  return true;
}}

bool {name}::Run() {{
  MW_INFO("{name}::Run succeeded.");
  return true;
}}

void {name}::Stop() {{
  MW_INFO("{name}::Stop succeeded.");
}}

}}  // namespace {ns}
"""


def _gen_subscriber_unified(mod: dict, ns: str, mod_dir: str,
                            ts_info: dict) -> list[tuple[str, str]]:
    """Glue: emit all 4 files for a plugin-claimed subscriber module."""
    name = mod["name"]
    snake = _snake(name)
    hdr = _gen_subscriber_unified_header(mod, ns)
    src = _gen_subscriber_unified_cc(mod, ns, mod_dir, ts_info)
    inc_h = ts_info["inc_h_content"]
    inc_cc = ts_info["inc_cc_content"]
    return [
        (f"src/{mod_dir}/{snake}.h", hdr),
        (f"src/{mod_dir}/{snake}.cc", src),
        (f"src/{mod_dir}/{snake}_typesupport.inc.h", inc_h),
        (f"src/{mod_dir}/{snake}_typesupport.inc.cc", inc_cc),
    ]


# ── Unified loaned_subscriber skeleton ─────────────────────────────────────
#
# Analogous to the unified subscriber, but the OnMessage signature diverges
# between dds and iceoryx plugins (dds: shared_ptr<const T>; iceoryx:
# (const T&, std::size_t size)). We absorb the divergence with
# IBMW_TS_LOANED_SUB_ON_MESSAGE_DECL macro (class-scope virtual decl).
#
# The user-facing OnMessage default impl body lives in inc.cc (per
# plugin) so plugin-specific log fields can differ; the `on_message_body`
# marker is emitted inside that default body.

def _gen_loaned_subscriber_unified_header(mod: dict, ns: str) -> str:
    """Transport-agnostic loaned_subscriber header.

    R1/R2/R3: includes only cpp/module_base.h + <name>_typesupport.inc.h.
    Class body uses IBMW_TS_LOANED_SUB_* macros to inject the POD
    static_assert (iceoryx) and the virtual OnMessage declaration with
    plugin-specific signature.
    """
    name = mod["name"]
    snake = _snake(name)
    return f"""{_COPYRIGHT}
#pragma once

#include <atomic>
#include <cstdint>

#include "cpp/module_base.h"

// Transport-specific typesupport lives in the generated inc.h.
// The user-facing template remains transport-agnostic.
#include "{snake}_typesupport.inc.h"

namespace {ns} {{

class {name} : public ibmw::Module {{
 public:
  {name}() = default;
  ~{name}() override = default;

  // Plugin-injected: iceoryx expands to static_assert trivially-
  // copyable; dds expands to a no-op.
  IBMW_TS_LOANED_SUB_STATIC_ASSERT;

  ibmw::ModuleDescriptor Info() const override {{
    return ibmw::ModuleDescriptor{{.module_name = "{name}"}};
  }}

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 protected:
  // Plugin-injected: virtual user-callback declaration. Signatures
  // intentionally diverge — dds uses shared_ptr<const T>, iceoryx uses
  // (const T&, std::size_t size). This keeps byte-semantic equivalence
  // with the legacy generated code.
  IBMW_TS_LOANED_SUB_ON_MESSAGE_DECL;

  auto GetLogger() {{ return core_.GetLogger(); }}

 private:
  ibmw::Runtime core_;
  std::atomic<uint64_t> received_{{0}};
  std::atomic<uint64_t> dropped_{{0}};
}};

}}  // namespace {ns}
"""


def _gen_loaned_subscriber_unified_cc(mod: dict, ns: str, snake_dir: str,
                                      ts_info: dict) -> str:
    """Transport-agnostic loaned_subscriber .cc.

    R1/R2: no transport headers here; all transport-specific mechanics
    live in the included <name>_typesupport.inc.cc (which also defines
    the default OnMessage body with the `on_message_body` marker).
    """
    name = mod["name"]
    snake = _snake(name)
    return f"""{_COPYRIGHT}
#include "{snake_dir}/{snake}.h"

// Include the transport-specific typesupport translation unit.
// This carries every dds/iceoryx-specific symbol referenced below
// (subscriber handle binding, attach callback, default OnMessage body).
#include "{snake_dir}/{snake}_typesupport.inc.cc"

namespace {ns} {{

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;

  // Plugin-injected: expands to GetSubscriber + OnMessage<T> /
  // LoanedSubscriber<T>::AttachBytes registration that forwards to
  // the user OnMessage virtual after incrementing received_.
  IBMW_TS_LOANED_SUB_ATTACH(core_);

  MW_INFO("{name}::Setup succeeded.");
  return true;
}}

bool {name}::Run() {{
  // Nothing to do — deliveries already flow through the callback
  // installed in Setup(). No driver pointer needed.
  MW_INFO("{name}::Run started.");
  return true;
}}

void {name}::Stop() {{
  MW_INFO("{name}::Stop succeeded (received={{}}, dropped={{}}).",
          received_.load(), dropped_.load());
}}

// The default OnMessage implementation body is injected by the plugin
// via the transport-specific inc.cc included above (it carries the
// `on_message_body` user-code marker).

}}  // namespace {ns}
"""


def _gen_loaned_subscriber_unified(mod: dict, ns: str, mod_dir: str,
                                   ts_info: dict) -> list[tuple[str, str]]:
    """Glue: emit all 4 files for a plugin-claimed loaned_subscriber."""
    name = mod["name"]
    snake = _snake(name)
    hdr = _gen_loaned_subscriber_unified_header(mod, ns)
    src = _gen_loaned_subscriber_unified_cc(mod, ns, mod_dir, ts_info)
    inc_h = ts_info["inc_h_content"]
    inc_cc = ts_info["inc_cc_content"]
    return [
        (f"src/{mod_dir}/{snake}.h", hdr),
        (f"src/{mod_dir}/{snake}.cc", src),
        (f"src/{mod_dir}/{snake}_typesupport.inc.h", inc_h),
        (f"src/{mod_dir}/{snake}_typesupport.inc.cc", inc_cc),
    ]


# ── Unified loaned_publisher skeleton ─────────────────────────────────────
#
# Both transports use the stable SenderAccess<T> / LoanedSample<T> API. The
# plugin fragment supplies type registration and, where required, capability
# setup without exposing a concrete transport driver to the module.
#
# 7-macro contract from inc.h (plugin emits every one):
#   IBMW_TS_LOANED_PUB_MSG_TYPE                — alias
#   IBMW_TS_LOANED_PUB_STATIC_ASSERT           — class-body top
#   IBMW_TS_LOANED_PUB_MEMBER                  — private:
#   IBMW_TS_LOANED_PUB_SETUP(core_expr)        — Setup body
#   IBMW_TS_LOANED_PUB_RUN_INIT(core_expr)     — Run body, pre-thread
#   IBMW_TS_LOANED_PUB_HOT_LOOP_BODY(loop_var) — entire PublishLoop body
#   IBMW_TS_LOANED_PUB_PRINT_STATS             — PrintStatistics body

def _gen_loaned_publisher_unified_header(mod: dict, ns: str) -> str:
    """Transport-agnostic loaned_publisher header.

    R1/R2/R3: includes only cpp/module_base.h + standard library +
    <name>_typesupport.inc.h. No transport/driver headers; those live
    in the generated inc.h.
    """
    name = mod["name"]
    snake = _snake(name)
    return f"""{_COPYRIGHT}
//
// Loaned Publisher (transport-agnostic). The generated TypeSupport fragment
// binds the stable SenderAccess<T> / LoanedSample<T> API to the message type;
// concrete transport selection remains in runtime configuration.
//
// The user-facing template remains transport-agnostic.

#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>
#include <thread>

#include "cpp/module_base.h"

// Message TypeSupport binding lives in the generated inc.h. It defines the
// seven IBMW_TS_LOANED_PUB_* macros referenced below using only public APIs.
#include "{snake}_typesupport.inc.h"

namespace {ns} {{

class {name} : public ibmw::Module {{
 public:
  {name}() = default;
  ~{name}() override = default;

  // The public loaned API requires an in-place-safe message type.
  IBMW_TS_LOANED_PUB_STATIC_ASSERT;

  ibmw::ModuleDescriptor Info() const override {{
    return ibmw::ModuleDescriptor{{.module_name = "{name}"}};
  }}

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  void PublishLoop();
  void PrintStatistics();
  auto GetLogger() {{ return core_.GetLogger(); }}

  ibmw::Runtime core_;

  // Plugin-injected stable publisher access for the selected message type.
  IBMW_TS_LOANED_PUB_MEMBER;

  std::thread publish_thread_;
  std::atomic<bool> running_{{false}};
  uint64_t publish_count_{{0}};
  uint64_t zero_copy_count_{{0}};
  uint64_t fallback_count_{{0}};
}};

}}  // namespace {ns}
"""


def _gen_loaned_publisher_unified_cc(mod: dict, ns: str, snake_dir: str,
                                     ts_info: dict) -> str:
    """Transport-agnostic loaned_publisher .cc.

    R1/R2: no transport/driver symbols here. Setup / Run / PublishLoop
    / PrintStatistics bodies are all reduced to a single macro call
    so transport divergence is fully absorbed by inc.h.
    """
    name = mod["name"]
    snake = _snake(name)
    return f"""{_COPYRIGHT}
#include "{snake_dir}/{snake}.h"

// Include the transport-specific typesupport translation unit. It
// carries every dds/iceoryx-specific symbol referenced by the macro
// expansions below (publisher handle, driver-aware create call,
// LoanedSample borrow/publish, driver stats). The user-code marker
// `populate_loaned_message` lives inside the inc.h hot-loop macro
// definition so users edit one location regardless of transport.
#include "{snake_dir}/{snake}_typesupport.inc.cc"

namespace {ns} {{

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;

  // Plugin-injected: GetPublisher + RegisterSendType<T> (+ compile-time
  // plain-type log on dds, sizeof log on iceoryx). Returns false on
  // registration failure.
  IBMW_TS_LOANED_PUB_SETUP(core_);

  MW_INFO("{name}::Setup succeeded.");
  return true;
}}

bool {name}::Run() {{
  // Plugin-injected validation of the stable publisher access.
  IBMW_TS_LOANED_PUB_RUN_INIT(core_);

  running_ = true;
  publish_thread_ = std::thread([this]() {{ PublishLoop(); }});
  MW_INFO("{name}::Run succeeded.");
  return true;
}}

void {name}::Stop() {{
  running_ = false;
  if (publish_thread_.joinable()) {{
    publish_thread_.join();
  }}
  PrintStatistics();
  MW_INFO("{name}::Stop succeeded (total published: {{}}).", publish_count_);
}}

void {name}::PublishLoop() {{
  // Plugin-injected strict loan / populate / publish loop.
  while (running_) {{
    IBMW_TS_LOANED_PUB_HOT_LOOP_BODY(loop_iter);
  }}
}}

void {name}::PrintStatistics() {{
  // Plugin-injected transport-neutral counters.
  IBMW_TS_LOANED_PUB_PRINT_STATS;
}}

}}  // namespace {ns}
"""


def _gen_loaned_publisher_unified(mod: dict, ns: str, mod_dir: str,
                                  ts_info: dict) -> list[tuple[str, str]]:
    """Glue: emit all 4 files for a plugin-claimed loaned_publisher."""
    name = mod["name"]
    snake = _snake(name)
    hdr = _gen_loaned_publisher_unified_header(mod, ns)
    src = _gen_loaned_publisher_unified_cc(mod, ns, mod_dir, ts_info)
    inc_h = ts_info["inc_h_content"]
    inc_cc = ts_info["inc_cc_content"]
    return [
        (f"src/{mod_dir}/{snake}.h", hdr),
        (f"src/{mod_dir}/{snake}.cc", src),
        (f"src/{mod_dir}/{snake}_typesupport.inc.h", inc_h),
        (f"src/{mod_dir}/{snake}_typesupport.inc.cc", inc_cc),
    ]


def _resolve_rpc_markerized_typesupport(
    mod: dict[str, object],
    ns: str,
    role: str,
) -> dict[str, str]:
    transport = mod.get("transport")
    if transport not in ("dds", "ros2", "json"):
        raise ValueError(f"unsupported unified rpc_{role} transport: {transport}")

    transport_mod = importlib.import_module(f"transports.{transport}")
    hook_names = {
        ("dds", "server"): "emit_dds_rpc_server_markerized_typesupport",
        ("dds", "client"): "emit_dds_rpc_client_markerized_typesupport",
        ("ros2", "server"): "_gen_ros2_rpc_server_markerized_typesupport",
        ("ros2", "client"): "_gen_ros2_rpc_client_markerized_typesupport",
        ("json", "server"): "_gen_json_rpc_server_markerized_typesupport",
        ("json", "client"): "_gen_json_rpc_client_markerized_typesupport",
    }
    hook_name = hook_names[(str(transport), role)]
    hook = getattr(transport_mod, hook_name)
    ts_info = hook(mod, ns)
    if ts_info is None:
        raise ValueError(
            f"markerized rpc_{role} typesupport hook returned None for transport {transport}")
    return ts_info


def _gen_rpc_server_unified_header(mod: dict[str, object], ns: str,
                                   ts_info: dict[str, str]) -> str:
    """Emit the marker-driven thin rpc_server shell."""
    _ = ts_info
    name = str(mod["name"])
    snake = _snake(name)
    return f"""{_COPYRIGHT}
#pragma once

#include "cpp/module_base.h"
#include "{snake}_typesupport.inc.h"

namespace ibmw::context {{
class Context;
}}

namespace {ns} {{

class {name} : public ibmw::Module {{
 public:
  {name}() = default;
  ~{name}() override = default;

  ibmw::ModuleDescriptor Info() const override {{
    return ibmw::ModuleDescriptor{{.module_name = "{name}"}};
  }}

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

  IBMW_TS_RPC_SERVER_HANDLER_DECL

 private:
  auto GetLogger() {{ return core_.GetLogger(); }}

  ibmw::Runtime core_;
  std::shared_ptr<ibmw::context::Context> ctx_ptr_;
}};

}}  // namespace {ns}
"""


def _gen_rpc_server_unified_cc(mod: dict[str, object], ns: str,
                               snake_dir: str,
                               ts_info: dict[str, str]) -> str:
    """Emit the marker-driven thin rpc_server implementation shell."""
    _ = ts_info
    name = str(mod["name"])
    snake = _snake(name)
    transport = str(mod.get("transport"))
    if transport == "dds":
        run_body = f"""  try {{
    if (ctx_ptr_) {{
      ctx_ptr_->LetMe();
    }}
    MW_INFO(\"{name}::Run succeeded.\");
  }} catch (const std::exception& e) {{
    MW_ERROR(\"{name}::Run failed: {{}}\", e.what());
    return false;
  }}
  return true;"""
        stop_body = f"""  try {{
    if (ctx_ptr_) {{
      ctx_ptr_->StopRunning();
    }}
    MW_INFO(\"{name}::Stop succeeded.\");
  }} catch (const std::exception& e) {{
    MW_ERROR(\"{name}::Stop failed: {{}}\", e.what());
  }}"""
    else:
        run_body = f"""  MW_INFO(\"{name}::Run succeeded.\");
  return true;"""
        stop_body = f"""  MW_INFO(\"{name}::Stop succeeded.\");"""
    return f"""{_COPYRIGHT}
#include "{snake_dir}/{snake}.h"

#include "{snake_dir}/{snake}_typesupport.inc.cc"

namespace {ns} {{

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;
  IBMW_TS_RPC_SERVER_SETUP(core);
  return true;
}}

bool {name}::Run() {{
{run_body}
}}

void {name}::Stop() {{
{stop_body}
}}

IBMW_TS_RPC_SERVER_HANDLER_IMPL({name})

}}  // namespace {ns}
"""


def _gen_rpc_client_setup_body(mod: dict[str, object]) -> str:
    transport = str(mod.get("transport"))
    name = str(mod["name"])
    if transport == "dds":
        return f"""  ctx_ptr_ = std::make_shared<ibmw::context::Context>(core);
  ctx_ptr_->LetMe();

  try {{
    time_executor_ = ctx_ptr_->CreateExecutor(\"time_schedule_executor\");
    client_.Init(ctx_ptr_);
    MW_INFO(\"{name}::Setup succeeded.\");
  }} catch (const std::exception& e) {{
    MW_ERROR(\"{name}::Setup failed: {{}}\", e.what());
    return false;
  }}

  return true;"""
    if transport == "ros2":
        pkg = str(mod["ros2_package"])
        svc = str(mod["ros2_service"])
        if bool(mod.get("ros2_imported_srv_adapter")):
            register_client = f"ibmw::ros2_interfaces::{pkg}::Register{svc}ClientFunc"
            access_type = f"ibmw::ros2_interfaces::{pkg}::{svc}SyncClientAdapter"
        else:
            register_client = f"{pkg}::srv::Register{svc}ClientFunc"
            access_type = f"{pkg}::srv::{svc}SyncAccess"
        return f"""  core_ = core;

  try {{
    std::string file_path = std::string(core_.GetConfigurator().GetConfigFilePath());
    if (!file_path.empty()) {{
      YAML::Node cfg_node = YAML::LoadFile(file_path);
      if (cfg_node[\"{name}\"] && cfg_node[\"{name}\"][\"rpc_frq\"]) {{
        rpc_frq_ = cfg_node[\"{name}\"][\"rpc_frq\"].as<double>();
      }}
    }}

    auto rpc_handle = core_.GetService();
    MW_CHECK_LOG_THROW(core_.GetLogger(), rpc_handle,
        ibmw::common::util::kLogLevelError, \"Get service handle failed.\");

    bool ret = {register_client}(rpc_handle);
    MW_REQUIRE(ret, \"Register ROS2 RPC client failed.\");

    access_ = std::make_shared<{access_type}>(rpc_handle);

    MW_INFO(\"{name}::Setup succeeded.\");
  }} catch (const std::exception& e) {{
    MW_ERROR(\"Setup failed: {{}}\", e.what());
    return false;
  }}

  return true;"""
    if transport == "json":
        service_name = str(mod["service_name"])
        methods = mod.get("methods", [])
        if not isinstance(methods, list):
            methods = []
        registrations: list[str] = []
        for method in methods:
            if not isinstance(method, dict):
                continue
            method_name = str(method["name"])
            req_type = str(method["request"])
            rsp_type = str(method["response"])
            method_var = _snake(method_name)
            registrations.append(
                f"""  bool ret_{method_var} = ibmw::json::rpc::RegisterClientFunc<{req_type}, {rsp_type}>(
      service_ref_, kServiceName, \"{method_name}\");
  if (!ret_{method_var}) {{
    MW_ERROR(\"Failed to register client func for {{}}/{method_name}\", kServiceName);
    return false;
  }}"""
            )
        registration_block = "\n\n".join(registrations)
        if registration_block:
            registration_block = f"\n{registration_block}\n"
        return f"""  core_ = core;
  service_ref_ = core_.GetService();{registration_block}
  MW_INFO(\"{name}::Setup succeeded.\");
  return true;"""
    raise ValueError(f"unsupported unified rpc_client transport: {transport}")


def _gen_rpc_client_run_body(mod: dict[str, object]) -> str:
    transport = str(mod.get("transport"))
    name = str(mod["name"])
    if transport == "dds":
        return f"""  try {{
    auto executor = ibmw::co::IbmwExecutor(time_executor_);
    scope_.spawn(ibmw::co::On(executor, RpcLoop()));
    MW_INFO(\"{name}::Run succeeded.\");
  }} catch (const std::exception& e) {{
    MW_ERROR(\"{name}::Run failed: {{}}\", e.what());
    return false;
  }}
  return true;"""
    if transport == "ros2":
        return f"""  loop_thread_ = std::thread([this]() {{ RpcLoop(); }});

  MW_INFO(\"{name}::Run succeeded.\");
  return true;"""
    if transport == "json":
        return f"""  running_ = true;
  std::thread([this]() {{ RpcLoop(); }}).detach();
  MW_INFO(\"{name}::Run — client loop started.\");
  return true;"""
    raise ValueError(f"unsupported unified rpc_client transport: {transport}")


def _gen_rpc_client_stop_body(mod: dict[str, object]) -> str:
    transport = str(mod.get("transport"))
    name = str(mod["name"])
    if transport == "dds":
        return f"""  try {{
    ctx_ptr_->StopRunning();
    ibmw::co::SyncWait(scope_.complete());
    MW_INFO(\"{name}::Stop succeeded.\");
  }} catch (const std::exception& e) {{
    MW_ERROR(\"{name}::Stop failed: {{}}\", e.what());
  }}"""
    if transport == "ros2":
        return f"""  run_flag_ = false;
  if (loop_thread_.joinable()) {{
    loop_thread_.join();
  }}
  MW_INFO(\"{name}::Stop succeeded.\");"""
    if transport == "json":
        return f"""  running_ = false;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  MW_INFO(\"{name}::Stop.\");"""
    raise ValueError(f"unsupported unified rpc_client transport: {transport}")


def _gen_rpc_client_unified_header(mod: dict[str, object], ns: str,
                                   ts_info: dict[str, str]) -> str:
    """Emit the marker-driven thin rpc_client shell."""
    _ = ts_info
    name = str(mod["name"])
    snake = _snake(name)
    return f"""{_COPYRIGHT}
#pragma once

#include "cpp/module_base.h"
#include "{snake}_typesupport.inc.h"

namespace {ns} {{

class {name} : public ibmw::Module {{
 public:
  {name}() = default;
  ~{name}() override = default;

  ibmw::ModuleDescriptor Info() const override {{
    return ibmw::ModuleDescriptor{{.module_name = "{name}"}};
  }}

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

  IBMW_TS_RPC_CLIENT_LOOP_DECL
  ibmw::Runtime core_;
}};

}}  // namespace {ns}
"""


def _gen_rpc_client_unified_cc(mod: dict[str, object], ns: str,
                               snake_dir: str,
                               ts_info: dict[str, str]) -> str:
    """Emit the marker-driven thin rpc_client implementation shell."""
    _ = ts_info
    name = str(mod["name"])
    snake = _snake(name)
    transport = str(mod.get("transport"))
    extra_includes = {
        "dds": "#include \"cpp/co/ibmw_context.h\"\n#include \"cpp/co/on.h\"\n#include \"cpp/co/schedule.h\"\n#include \"cpp/co/sync_wait.h\"",
        "ros2": "#include \"yaml-cpp/yaml.h\"",
        "json": "",
    }[transport]
    include_block = f"{extra_includes}\n\n" if extra_includes else ""
    return f"""{_COPYRIGHT}
#include "{snake_dir}/{snake}.h"

{include_block}#include "{snake_dir}/{snake}_typesupport.inc.cc"

namespace {ns} {{

bool {name}::Setup(ibmw::Runtime core) {{
{_gen_rpc_client_setup_body(mod)}
}}

bool {name}::Run() {{
{_gen_rpc_client_run_body(mod)}
}}

void {name}::Stop() {{
{_gen_rpc_client_stop_body(mod)}
}}

IBMW_TS_RPC_CLIENT_LOOP_IMPL({name})

}}  // namespace {ns}
"""


def _gen_rpc_server_unified(mod: dict[str, object], ns: str, mod_dir: str,
                            ts_info: dict[str, str]) -> list[tuple[str, str]]:
    """Glue: emit all 4 files for a marker-driven rpc_server."""
    _ = ts_info
    ts_info = _resolve_rpc_markerized_typesupport(mod, ns, "server")
    name = str(mod["name"])
    snake = _snake(name)
    hdr = _gen_rpc_server_unified_header(mod, ns, ts_info)
    src = _gen_rpc_server_unified_cc(mod, ns, mod_dir, ts_info)
    inc_h = ts_info["inc_h_content"]
    inc_cc = ts_info["inc_cc_content"]
    return [
        (f"src/{mod_dir}/{snake}.h", hdr),
        (f"src/{mod_dir}/{snake}.cc", src),
        (f"src/{mod_dir}/{snake}_typesupport.inc.h", inc_h),
        (f"src/{mod_dir}/{snake}_typesupport.inc.cc", inc_cc),
    ]


def _gen_rpc_client_unified(mod: dict[str, object], ns: str, mod_dir: str,
                            ts_info: dict[str, str]) -> list[tuple[str, str]]:
    """Glue: emit all 4 files for a marker-driven rpc_client."""
    _ = ts_info
    ts_info = _resolve_rpc_markerized_typesupport(mod, ns, "client")
    name = str(mod["name"])
    snake = _snake(name)
    hdr = _gen_rpc_client_unified_header(mod, ns, ts_info)
    src = _gen_rpc_client_unified_cc(mod, ns, mod_dir, ts_info)
    inc_h = ts_info["inc_h_content"]
    inc_cc = ts_info["inc_cc_content"]
    return [
        (f"src/{mod_dir}/{snake}.h", hdr),
        (f"src/{mod_dir}/{snake}.cc", src),
        (f"src/{mod_dir}/{snake}_typesupport.inc.h", inc_h),
        (f"src/{mod_dir}/{snake}_typesupport.inc.cc", inc_cc),
    ]


# ── Bare module ──────────────────────────────────────────────────────────────

def _gen_bare_header(mod: dict, ns: str) -> str:
    name = mod["name"]
    return f"""{_COPYRIGHT}
#pragma once

#include "cpp/module_base.h"

namespace {ns} {{

class {name} : public ibmw::Module {{
 public:
  {name}() = default;
  ~{name}() override = default;

  ibmw::ModuleDescriptor Info() const override {{
    return ibmw::ModuleDescriptor{{.module_name = "{name}"}};
  }}

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  // Required by MW_INFO/MW_WARN/... macros (they expand to GetLogger())
  auto GetLogger() {{ return core_.GetLogger(); }}

  ibmw::Runtime core_;
}};

}}  // namespace {ns}
"""


def _gen_bare_cc(mod: dict, ns: str, snake_dir: str) -> str:
    name = mod["name"]
    return f"""{_COPYRIGHT}
#include "{snake_dir}/{_snake(name)}.h"

namespace {ns} {{

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("{name}::Setup succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}}

bool {name}::Run() {{
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("{name}::Run succeeded.");
  return true;
  // IBMW_PUBSUB_BARE_BODY_END
}}

void {name}::Stop() {{
  // IBMW_PUBSUB_BARE_BODY_BEGIN
  MW_INFO("{name}::Stop succeeded.");
  // IBMW_PUBSUB_BARE_BODY_END
}}

}}  // namespace {ns}
"""


# ── Logger module ────────────────────────────────────────────────────────────

def _gen_logger_header(mod: dict, ns: str) -> str:
    name = mod["name"]
    executor = mod.get("executor_name", "work_executor")
    return f"""{_COPYRIGHT}
#pragma once

#include <atomic>
#include <future>

#include "cpp/module_base.h"

namespace {ns} {{

class {name} : public ibmw::Module {{
 public:
  {name}() = default;
  ~{name}() override = default;

  ibmw::ModuleDescriptor Info() const override {{
    return ibmw::ModuleDescriptor{{.module_name = "{name}"}};
  }}

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 private:
  auto GetLogger() {{ return core_.GetLogger(); }}

  ibmw::Runtime core_;
  ibmw::executor::ExecutorHandle executor_;
  std::atomic_bool run_flag_{{false}};
  std::promise<void> stop_promise_;
}};

}}  // namespace {ns}
"""


def _gen_logger_cc(mod: dict, ns: str, snake_dir: str) -> str:
    name = mod["name"]
    executor = mod.get("executor_name", "work_executor")
    return f"""{_COPYRIGHT}
#include "{snake_dir}/{_snake(name)}.h"

#include <chrono>
#include <thread>

namespace {ns} {{

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;

  executor_ = core_.GetExecutor().GetExecutor("{executor}");
  MW_CHECK_LOG_THROW(
      core_.GetLogger(), executor_,
      ibmw::common::util::kLogLevelError,
      "Get executor '{executor}' failed.");

  MW_INFO("{name}::Setup succeeded.");
  return true;
}}

bool {name}::Run() {{
  run_flag_ = true;

  executor_.Dispatch([this]() {{
    auto GetLogger = [this]() {{ return core_.GetLogger(); }};

    uint64_t n = 0;
    while (run_flag_) {{
      std::string s = "log_msg_" + std::to_string(n);

      MW_TRACE("Test trace log, s = {{}}, n = {{}}.", s, n);
      MW_DEBUG("Test debug log, s = {{}}, n = {{}}.", s, n);
      MW_INFO("Test info log, s = {{}}, n = {{}}.", s, n);
      MW_WARN("Test warn log, s = {{}}, n = {{}}.", s, n);
      MW_ERROR("Test error log, s = {{}}, n = {{}}.", s, n);
      MW_FATAL("Test fatal log, s = {{}}, n = {{}}.", s, n);

      ++n;
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }}

    stop_promise_.set_value();
  }});

  MW_INFO("{name}::Run succeeded.");
  return true;
}}

void {name}::Stop() {{
  run_flag_ = false;
  auto f = stop_promise_.get_future();
  f.wait();
  MW_INFO("{name}::Stop succeeded.");
}}

}}  // namespace {ns}
"""



# The former `_gen_json_*` generator functions (publisher /
# subscriber / rpc_server / rpc_client) have moved to
# tools/ibmw_gen/transports/json.py. The plugin path in generate_modules()
# now routes every (type, "json") module via _transports.get("json").




# The former `_gen_ros2_*` generator functions (publisher /
# subscriber / rpc_server / rpc_client main + skeleton) have moved to
# tools/ibmw_gen/transports/ros2.py. The plugin path in generate_modules()
# now routes every (type, "ros2") module via _transports.get("ros2").






# The former `_gen_loaned_{publisher,subscriber}_iceoryx_{header,cc}`
# functions have moved to tools/ibmw_gen/transports/iceoryx.py. The plugin path in
# generate_modules() now routes every (type, "iceoryx") module via
# _transports.get("iceoryx").

# ── Dispatch ─────────────────────────────────────────────────────────────────

def _resolve_module_metadata(mod: dict, project: dict):
    """Add computed metadata to a module dict for code generation."""
    dds = project.get("dds_types")
    cdt = project.get("custom_dds_types")

    # For DDS pub/sub modules: find the type bridge header
    if mod.get("transport") == "dds" and mod["type"] in ("publisher", "subscriber"):
        # Check if this module uses custom_dds_types (auto-generated CDR headers)
        if mod.get("_uses_custom_dds_types") and cdt:
            mod["_dds_namespace_cpp"] = cdt["_namespace_cpp"]
            msg_name = mod["message_type"]
            msg_info = cdt.get("_msg_map", {}).get(msg_name)
            if msg_info:
                mod["_dds_types_header"] = f"protocol/{msg_info['_snake_name']}.h"
            else:
                mod["_dds_types_header"] = f"protocol/{_snake(msg_name)}.h"
        elif dds:
            mod["_dds_namespace_cpp"] = dds["_namespace_cpp"]
            # Find which group contains the message type
            msg_name = mod["message_type"]
            group_name = dds.get("_msg_to_group", {}).get(msg_name)
            if group_name:
                mod["_dds_types_header"] = f"{_snake(group_name)}_dds_types.h"
            else:
                mod["_dds_types_header"] = f"{_snake(msg_name)}_dds_types.h"

    # For DDS RPC modules: find the RPC header and service info
    if mod.get("transport") == "dds" and mod["type"] in ("rpc_server", "rpc_client"):
        if dds:
            mod["_dds_namespace_cpp"] = dds["_namespace_cpp"]
            svc_ref = mod.get("dds_service", "")
            for svc in dds.get("rpc_services", []):
                if svc["name"] == svc_ref:
                    mod["_dds_service_info"] = svc
                    # Use _rpc_header from schema enrichment if available
                    mod["_rpc_header"] = svc.get("_rpc_header", f"{svc['name']}.ibmw_rpc.dds.h")
                    break
            else:
                mod["_rpc_header"] = f"{svc_ref}.ibmw_rpc.dds.h"
                mod["_dds_service_info"] = {"name": svc_ref, "methods": []}


_GENERATORS = {
    ("bare", None): (_gen_bare_header, _gen_bare_cc),
    ("logger", None): (_gen_logger_header, _gen_logger_cc),
    # All dds-transport generators live in
    # tools/ibmw_gen/transports/dds.py now. The plugin path in
    # generate_modules() routes (type, "dds") via _transports.get("dds").
    # All json-transport generators live in
    # tools/ibmw_gen/transports/json.py now. The plugin path in
    # generate_modules() routes (type, "json") via _transports.get("json").
    # All ros2-transport generators live in
    # tools/ibmw_gen/transports/ros2.py now. The plugin path in
    # generate_modules() routes (type, "ros2") via _transports.get("ros2").
    # All iceoryx-transport generators live in
    # tools/ibmw_gen/transports/iceoryx.py now. The plugin path in
    # generate_modules() routes (type, "iceoryx") via
    # _transports.get("iceoryx").
}


class ModuleGenError(ValueError):
    """A module the generator cannot emit."""


def generate_modules(project: dict) -> list[tuple[str, str]]:
    """Generate module .h and .cc files, then splice optional `user_code`."""
    results = []
    for mod in project.get("modules", []):
        results.extend(apply_user_code(mod, _generate_module_files(project, [mod])))
    return results


def _generate_module_files(project: dict, modules: list) -> list[tuple[str, str]]:
    """Generate module .h and .cc files.

    Returns list of (rel_path, content) tuples.

    Dispatch order:
      1. If a transport plugin is registered for `mod['transport']`
         AND its `emit_module` callable claims the module (returns a
         non-None list), use the plugin output.
      2. Otherwise fall back to the legacy `_GENERATORS` table.

    Transport plugins own their emission bodies. A plugin may decline a
    module by returning None, allowing the legacy table to handle it.
    """
    # Lazy import to avoid module_gen <-> transports <-> schema cycles.
    # Uses the same flat-path style as schema.py (`import transports`),
    # because ibmw_gen is run with `tools/ibmw_gen/` on sys.path rather
    # than as an installed package.
    import transports as _transports

    ns = project["project"]["_namespace_cpp"]
    results = []

    for mod in modules:
        _resolve_module_metadata(mod, project)

        mod_type = mod["type"]
        transport = mod.get("transport")

        snake_name = _snake(mod["name"])
        snake_dir = snake_name + "_module" if not snake_name.endswith("_module") else snake_name
        mod_dir = mod.get("dir", snake_dir)

        # 1) Unified publisher path.
        #    Preferred over emit_module when the plugin implements the
        #    fine-grained emit_publisher_typesupport hook. Keeps the
        #    user-facing <module>.{h,cc} transport-agnostic.
        if transport and mod_type == "publisher":
            spec = _transports.get(transport)
            if spec is not None and getattr(
                    spec, "emit_publisher_typesupport", None) is not None:
                ts_info = spec.emit_publisher_typesupport(mod, ns)
                if ts_info is not None:
                    results.extend(
                        _gen_publisher_unified(mod, ns, mod_dir, ts_info))
                    continue

        # 1b) Unified subscriber path. Same priority
        #     rule as the publisher branch above.
        if transport and mod_type == "subscriber":
            spec = _transports.get(transport)
            if spec is not None and getattr(
                    spec, "emit_subscriber_typesupport", None) is not None:
                ts_info = spec.emit_subscriber_typesupport(mod, ns)
                if ts_info is not None:
                    results.extend(
                        _gen_subscriber_unified(mod, ns, mod_dir, ts_info))
                    continue

        # 1c) Unified loaned_subscriber path. Same
        #     priority rule — plugin opt-in via
        #     emit_loaned_subscriber_typesupport.
        if transport and mod_type == "loaned_subscriber":
            spec = _transports.get(transport)
            if spec is not None and getattr(
                    spec, "emit_loaned_subscriber_typesupport",
                    None) is not None:
                ts_info = spec.emit_loaned_subscriber_typesupport(mod, ns)
                if ts_info is not None:
                    results.extend(
                        _gen_loaned_subscriber_unified(
                            mod, ns, mod_dir, ts_info))
                    continue

        # 1d) Unified loaned_publisher path. Same
        #     priority rule — plugin opt-in via
        #     emit_loaned_publisher_typesupport.
        if transport and mod_type == "loaned_publisher":
            spec = _transports.get(transport)
            if spec is not None and getattr(
                    spec, "emit_loaned_publisher_typesupport",
                    None) is not None:
                ts_info = spec.emit_loaned_publisher_typesupport(mod, ns)
                if ts_info is not None:
                    results.extend(
                        _gen_loaned_publisher_unified(
                            mod, ns, mod_dir, ts_info))
                    continue

        # 1e) Unified rpc_server path. Same
        #     priority rule — plugin opt-in via
        #     emit_rpc_server_typesupport.
        if transport and mod_type == "rpc_server":
            spec = _transports.get(transport)
            hook = None if spec is None else getattr(
                spec, "emit_rpc_server_typesupport", None)
            if hook is not None:
                ts_info = hook(mod, ns)
                if ts_info is not None:
                    results.extend(
                        _gen_rpc_server_unified(mod, ns, mod_dir, ts_info))
                    continue

        # 1f) Unified rpc_client path. Same
        #     priority rule — plugin opt-in via
        #     emit_rpc_client_typesupport.
        if transport and mod_type == "rpc_client":
            spec = _transports.get(transport)
            hook = None if spec is None else getattr(
                spec, "emit_rpc_client_typesupport", None)
            if hook is not None:
                ts_info = hook(mod, ns)
                if ts_info is not None:
                    results.extend(
                        _gen_rpc_client_unified(mod, ns, mod_dir, ts_info))
                    continue

        # 2) Plugin emit_module path.
        if transport:
            spec = _transports.get(transport)
            if spec is not None and spec.emit_module is not None:
                plugin_out = spec.emit_module(mod, ns, mod_dir)
                if plugin_out is not None:
                    results.extend(plugin_out)
                    continue

        # 3) Legacy dispatch.
        key = (mod_type, transport)
        if key not in _GENERATORS:
            # The project CMake still lists the module's sources.
            raise ModuleGenError(
                f"no generator for module '{mod['name']}' "
                f"(type={mod_type}, transport={transport})")

        gen_h, gen_cc = _GENERATORS[key]
        hdr = gen_h(mod, ns)
        src = gen_cc(mod, ns, mod_dir)

        results.append((f"src/{mod_dir}/{snake_name}.h", hdr))
        results.append((f"src/{mod_dir}/{snake_name}.cc", src))

    return results
