#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# DDS transport plugin with full ownership of DDS code emission.
#
# The function bodies were migrated from module_gen.py after an intermediate
# emit_module() shim verified byte-identical output. module_gen no longer
# contains DDS-specific emission code.
#
# Plugin contract (transports/__init__.py:TransportSpec.emit_module):
#   emit_module(mod, ns, mod_dir) -> list[(rel_path, content)]
#       Returns [.h, .cc] pair for any (mod['type'], "dds") combination
#       this plugin claims, or None to fall through to the legacy
#       _GENERATORS dispatch (no longer reachable for "dds" since the
#       legacy table no longer holds dds rows post-migration).

"""DDS transport plugin for ibmw_gen — owns all dds code emission."""

from generators._common import _snake, _COPYRIGHT
from generators.marker_preserve import (
    extract_marker_regions,
    preserve_marker_regions,
)
from . import TransportSpec, register


# Type-keys this plugin claims ownership of. Mirrors the dds rows that
# used to live in module_gen._GENERATORS pre-migration.
_DDS_TYPE_KEYS = frozenset({
    "publisher",
    "subscriber",
    "rpc_server",
    "rpc_client",
    "loaned_publisher",
    "loaned_subscriber",
    "direct_ros2_publisher",
    "direct_ros2_subscriber",
    # Deprecated aliases (schema._normalize_module_type_alias normally
    # rewrites ros2_interop_* -> direct_ros2_* upfront).
    "ros2_interop_publisher",
    "ros2_interop_subscriber",
})

_DDS_RPC_SERVER_MARKERS = ["IBMW_RPC_SERVER_HANDLER_BODY"]
_DDS_RPC_CLIENT_MARKERS = [
    "IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD",
    "IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME",
    "IBMW_RPC_CLIENT_RETURN_STATUS_HANDLING",
]
_DDS_MARKER_ANCHOR_FORMAT = "// {marker}_{edge}"


def _format_marker_anchor(marker: str, edge: str) -> str:
    return _DDS_MARKER_ANCHOR_FORMAT.format(marker=marker, edge=edge)


def _dds_rpc_server_method_marker(method_name: str) -> str:
    return f"{_DDS_RPC_SERVER_MARKERS[0]}_{_snake(method_name).upper()}"


def _dds_rpc_client_method_marker(marker: str, method_name: str) -> str:
    return f"{marker}_{_snake(method_name).upper()}"


# ── Migrated _gen_* functions ───────────────────────────────────────────────
def _gen_dds_publisher_header(mod: dict, ns: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]  # e.g. "ExampleEventMsg"
    # Find the DDS type bridge header
    dds_types_header = mod.get("_dds_types_header", "example_event_dds_types.h")
    dds_ns = mod.get("_dds_namespace_cpp", "ibmw::protocols::example")

    return f"""{_COPYRIGHT}
#pragma once

#include <atomic>
#include <optional>

#include "cpp/module_base.h"
#include "type_support/dds/dds_transport.h"
#include "{dds_types_header}"

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
  // SenderAccess<T> bundles publisher + type info into one object.
  std::optional<ibmw::transport::SenderAccess<{dds_ns}::{msg_type}>> access_;
  uint32_t msg_count_ = 0;
  std::atomic<bool> running_{{false}};
}};

}}  // namespace {ns}
"""


def _gen_dds_publisher_cc(mod: dict, ns: str, snake_dir: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]
    topic = mod["topic"]
    dds_types_header = mod.get("_dds_types_header", "example_event_dds_types.h")
    dds_ns = mod.get("_dds_namespace_cpp", "ibmw::protocols::example")

    return f"""{_COPYRIGHT}
#include "{snake_dir}/{_snake(name)}.h"
#include "{dds_types_header}"

#include <chrono>
#include <thread>

using {dds_ns}::{msg_type};

namespace {ns} {{

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;

  auto publisher = core_.GetTransport().GetPublisher("{topic}");

  // Construct SenderAccess — bundles publisher + type name.
  access_.emplace(publisher);

  if (!access_->RegisterSendType()) {{
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "Register publish type failed!");
    return false;
  }}

  MW_INFO("{name}::Setup succeeded.");
  return true;
}}

bool {name}::Run() {{
  running_ = true;
  std::thread([this]() {{ PublishLoop(); }}).detach();
  MW_INFO("{name}::Run succeeded.");
  return true;
}}

void {name}::Stop() {{
  running_ = false;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  MW_INFO("{name}::Stop succeeded.");
}}

void {name}::PublishLoop() {{
  while (running_) {{
    {msg_type} msg;
    // TODO: populate msg fields here
    msg_count_++;

    // One-line typed send via SenderAccess
    access_->Send(msg);

    MW_INFO("Published DDS message #{{}}", msg_count_);

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }}
}}

}}  // namespace {ns}
"""


def _gen_dds_subscriber_header(mod: dict, ns: str) -> str:
    name = mod["name"]
    return f"""{_COPYRIGHT}
#pragma once

#include "cpp/module_base.h"
#include "type_support/dds/dds_transport.h"

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


def _gen_dds_subscriber_cc(mod: dict, ns: str, snake_dir: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]
    topic = mod["topic"]
    dds_types_header = mod.get("_dds_types_header", "example_event_dds_types.h")
    dds_ns = mod.get("_dds_namespace_cpp", "ibmw::protocols::example")

    return f"""{_COPYRIGHT}
#include "{snake_dir}/{_snake(name)}.h"
#include "{dds_types_header}"

using {dds_ns}::{msg_type};

namespace {ns} {{

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;

  auto subscriber = core_.GetTransport().GetSubscriber("{topic}");

  // ReceiverAccess<T> wraps subscribe + type dispatch in one object.
  ibmw::transport::ReceiverAccess<{msg_type}> access(subscriber);

  bool ret = access.OnMessage(
      [this](const std::shared_ptr<const {msg_type}>& msg) {{
        MW_INFO("Received DDS message.");
        // TODO: process msg fields here
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


def _gen_direct_ros2_publisher_header(mod: dict, ns: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]  # e.g. "std_msgs::msg::String"
    ros2_msg_include = mod["ros2_msg_include"]

    return f"""{_COPYRIGHT}
//
// IBMW: NOT TRANSPORT-DECOUPLED — direct ros2 API.
// R10 escape-hatch template (direct_ros2_publisher):
// generated code uses ibmw::ros2::transport API directly for ROS2 DDS
// wire-compatibility. User code is transport-coupled by design — to
// preserve transport-decoupling, prefer the agnostic `publisher` /
// `loaned_publisher` templates with SenderAccess<T>/LoanedSample<T>.

#pragma once

#include <atomic>
#include <thread>

#include "cpp/module_base.h"
#include "type_support/ros2/ros2_transport.h"
#include <{ros2_msg_include}>

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
  ibmw::transport::SenderRef publisher_;

  std::thread publish_thread_;
  std::atomic<bool> running_{{false}};
  uint32_t publish_count_{{0}};
}};

}}  // namespace {ns}
"""


def _gen_direct_ros2_publisher_cc(mod: dict, ns: str, snake_dir: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]
    topic = mod["topic"]
    ros2_type_name = mod["ros2_type_name"]
    # Short type name for local variable construction
    short_type = msg_type.rsplit("::", 1)[-1] if "::" in msg_type else msg_type

    return f"""{_COPYRIGHT}
#include "{snake_dir}/{_snake(name)}.h"

namespace {ns} {{

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;

  // Get publisher handle
  publisher_ = core_.GetTransport().GetPublisher("{topic}");

  // Register ROS2 message type for publishing
  bool success = ibmw::ros2::transport::RegisterSendType<{msg_type}>(
      publisher_, "{ros2_type_name}");

  if (!success) {{
    MW_ERROR("Failed to register ROS2 type: {ros2_type_name}");
    return false;
  }}

  MW_INFO("{name}::Setup succeeded.");
  return true;
}}

bool {name}::Run() {{
  running_ = true;

  // Start publish thread (joined in Stop)
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
    {msg_type} msg;
    // TODO: populate msg fields here
    publish_count_++;

    ibmw::ros2::transport::Send(
        publisher_, "{ros2_type_name}", msg);

    MW_INFO("Published ROS2 interop message #{{}}", publish_count_);

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }}
}}

}}  // namespace {ns}
"""


def _gen_direct_ros2_subscriber_header(mod: dict, ns: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]
    ros2_msg_include = mod["ros2_msg_include"]

    return f"""{_COPYRIGHT}
//
// IBMW: NOT TRANSPORT-DECOUPLED — direct ros2 API.
// R10 escape-hatch template (direct_ros2_subscriber):
// generated code uses ibmw::ros2::transport API directly for ROS2 DDS
// wire-compatibility. User code is transport-coupled by design — to
// preserve transport-decoupling, prefer the agnostic `subscriber` /
// `loaned_subscriber` templates with ReceiverAccess<T>/LoanedSubscriber<T>.

#pragma once

#include "cpp/module_base.h"
#include "type_support/ros2/ros2_transport.h"
#include <{ros2_msg_include}>

namespace {ns} {{

class {name} : public ibmw::Module {{
 public:
  {name}() = default;
  ~{name}() override = default;

  ibmw::ModuleDescriptor Info() const override {{
    return ibmw::ModuleDescriptor{{.module_name = "{name}"}};
  }}

  bool Setup(ibmw::Runtime core) override;
  bool Run() override {{ return true; }}
  void Stop() override;

 private:
  void OnMessage(
      ibmw::transport::ContextRef ctx,
      const std::shared_ptr<const {msg_type}>& msg);

  auto GetLogger() {{ return core_.GetLogger(); }}

  ibmw::Runtime core_;
  uint32_t receive_count_{{0}};
}};

}}  // namespace {ns}
"""


def _gen_direct_ros2_subscriber_cc(mod: dict, ns: str, snake_dir: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]
    topic = mod["topic"]
    ros2_type_name = mod["ros2_type_name"]

    return f"""{_COPYRIGHT}
#include "{snake_dir}/{_snake(name)}.h"

#include <cstdio>

namespace {ns} {{

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;

  // Get subscriber handle
  auto subscriber = core_.GetTransport().GetSubscriber("{topic}");

  // Subscribe to ROS2 messages with context callback
  bool success = ibmw::ros2::transport::OnMessage<{msg_type}>(
      subscriber,
      "{ros2_type_name}",
      [this](ibmw::transport::ContextRef ctx,
             const std::shared_ptr<const {msg_type}>& msg) {{
        OnMessage(ctx, msg);
      }});

  if (!success) {{
    MW_ERROR("Failed to subscribe ROS2 type: {ros2_type_name}");
    return false;
  }}

  MW_INFO("{name}::Setup succeeded.");
  return true;
}}

void {name}::Stop() {{
  MW_INFO("{name}::Stop, total received: {{}}", receive_count_);
}}

void {name}::OnMessage(
    ibmw::transport::ContextRef ctx,
    const std::shared_ptr<const {msg_type}>& msg) {{

  receive_count_++;

  // Synchronous harness evidence: callback-thread MW_INFO can be buffered by
  // the logger while the live interop test is already checking process logs.
  std::fprintf(stderr, "Received ROS2 interop message [%u]\\n", receive_count_);
  MW_INFO("Received ROS2 interop message [{{}}]", receive_count_);
  // TODO: process msg fields here

  auto serialization_type = ctx.GetSerializationType();
  MW_DEBUG("Serialization type: {{}}", serialization_type);
}}

}}  // namespace {ns}
"""


def _gen_loaned_subscriber_dds_header(mod: dict, ns: str) -> str:
    name = mod["name"]
    style = mod["subscriber_style"]
    if style != "callback":
        raise NotImplementedError(
            f"Module '{name}' (loaned_subscriber,dds): subscriber_style "
            f"'{style}' is reserved but not yet implemented; only "
            f"'callback' is currently supported")
    msg_type = mod["message_type"]
    ros2_msg_include = mod["ros2_msg_include"]

    return f"""{_COPYRIGHT}
//
// Loaned Subscriber (DDS) — receives via ibmw::ros2::transport::OnMessage<T>
// callback. No driver injection required (framework abstraction layer
// auto-dispatches to the dds driver).
//
// User-facing API: override OnMessage(const T& msg) below to consume each
// delivered message. The callback fires on the engine's MessageHandler
// thread; keep it short and lock-free.

#pragma once

#include <atomic>
#include <memory>

#include "cpp/module_base.h"
#include "type_support/ros2/ros2_transport.h"
#include <{ros2_msg_include}>

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

 protected:
  // User callback — invoked once per delivered message. Default impl
  // logs a counter; subclass and override (or edit in place) to do
  // real work. The shared_ptr is owned by the engine until OnMessage
  // returns; do NOT retain past return without copying.
  virtual void OnMessage(const std::shared_ptr<const {msg_type}>& msg);

  auto GetLogger() {{ return core_.GetLogger(); }}

 private:
  ibmw::Runtime core_;
  std::atomic<uint64_t> received_{{0}};
  std::atomic<uint64_t> dropped_{{0}};
}};

}}  // namespace {ns}
"""


def _gen_loaned_subscriber_dds_cc(mod: dict, ns: str, snake_dir: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]
    topic = mod["topic"]
    ros2_type_name = mod["ros2_type_name"]

    return f"""{_COPYRIGHT}
#include "{snake_dir}/{_snake(name)}.h"

namespace {ns} {{

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;

  // Standard receive-side stack: GetSubscriber + ros2 OnMessage<T>.
  // Must be wired in Setup() because the transport bus rejects new
  // OnMessage calls "after start". The dds driver routes deliveries
  // through the engine MessageHandler -- no driver pointer needed.
  auto subscriber = core_.GetTransport().GetSubscriber("{topic}");
  bool ok = ibmw::ros2::transport::OnMessage<{msg_type}>(
      subscriber, "{ros2_type_name}",
      [this](const std::shared_ptr<const {msg_type}>& msg) {{
        if (!msg) {{
          dropped_.fetch_add(1, std::memory_order_relaxed);
          return;
        }}
        received_.fetch_add(1, std::memory_order_relaxed);
        OnMessage(msg);
      }});
  if (!ok) {{
    MW_ERROR("{name}: OnMessage<{msg_type}> registration failed for "
             "topic '{topic}' (ros2_type='{ros2_type_name}')");
    return false;
  }}

  MW_INFO("{name}::Setup succeeded (topic='{topic}', "
          "ros2_type='{ros2_type_name}').");
  return true;
}}

bool {name}::Run() {{
  // Nothing to do — deliveries already flow through the OnMessage
  // handler installed in Setup(). No driver pointer needed.
  MW_INFO("{name}::Run started.");
  return true;
}}

void {name}::Stop() {{
  MW_INFO("{name}::Stop succeeded (received={{}}, dropped={{}}).",
          received_.load(), dropped_.load());
}}

void {name}::OnMessage(
    const std::shared_ptr<const {msg_type}>& msg) {{
  // Default impl: log every 5th message. Override in a subclass or
  // edit in place to do real work. msg is non-null on entry.
  uint64_t count = received_.load();
  if (count <= 3 || (count % 5 == 0)) {{
    MW_INFO("[RECV] {{}} #{{}} ptr={{}}",
            "{topic}", count, static_cast<const void*>(msg.get()));
  }}
}}

}}  // namespace {ns}
"""


# ── Plugin dispatch table ───────────────────────────────────────────────────

# Map (mod['type'], "dds") -> (header_emitter, cc_emitter). Direct
# replacement for the legacy module_gen._GENERATORS rows.
_DDS_GENERATORS = {
    ("publisher", "dds"): (_gen_dds_publisher_header, _gen_dds_publisher_cc),
    ("subscriber", "dds"): (_gen_dds_subscriber_header, _gen_dds_subscriber_cc),
    # rpc_server/rpc_client rows are handled by the unified RPC path
    # in module_gen._gen_rpc_{server,client}_unified handles all RPC emission
    # via _gen_*_rpc_*_markerized_typesupport sidecars.
    ("direct_ros2_publisher", "dds"): (
        _gen_direct_ros2_publisher_header, _gen_direct_ros2_publisher_cc),
    ("direct_ros2_subscriber", "dds"): (
        _gen_direct_ros2_subscriber_header, _gen_direct_ros2_subscriber_cc),
    # Deprecated aliases reuse the direct_ros2 emitters.
    ("ros2_interop_publisher", "dds"): (
        _gen_direct_ros2_publisher_header, _gen_direct_ros2_publisher_cc),
    ("ros2_interop_subscriber", "dds"): (
        _gen_direct_ros2_subscriber_header, _gen_direct_ros2_subscriber_cc),
    ("loaned_subscriber", "dds"): (
        _gen_loaned_subscriber_dds_header, _gen_loaned_subscriber_dds_cc),
}


def _emit_module(mod: dict, ns: str, mod_dir: str):
    """Emit the .h / .cc pair for a dds-transport module.

    Returns list[(rel_path, content)] or None if this plugin does not
    handle the given (mod['type'], "dds") pair.

    `publisher` is handled by the unified path
    in module_gen._gen_publisher_unified via
    emit_publisher_typesupport below; return None here so the upper
    dispatcher picks the unified route.

    `subscriber` is handled by the unified path
    in module_gen._gen_subscriber_unified via
    emit_subscriber_typesupport below; return None here too.

    `loaned_subscriber` is handled by the
    unified path in module_gen._gen_loaned_subscriber_unified via
    emit_loaned_subscriber_typesupport below; return None here too.

    `loaned_publisher` is handled by the
    unified path in module_gen._gen_loaned_publisher_unified via
    emit_loaned_publisher_typesupport below; return None here too.

    Every other type-key (rpc_*, direct_ros2_*) keeps the legacy
    per-type emitter.
    """
    mod_type = mod.get("type")
    if mod_type in ("publisher", "subscriber", "loaned_subscriber",
                    "loaned_publisher"):
        # Defer to module_gen.generate_modules's unified branches.
        return None
    if mod_type not in _DDS_TYPE_KEYS:
        return None

    pair = _DDS_GENERATORS.get((mod_type, "dds"))
    if pair is None:
        return None

    gen_h, gen_cc = pair
    snake = _snake(mod["name"])
    hdr = gen_h(mod, ns)
    src = gen_cc(mod, ns, mod_dir)
    return [
        (f"src/{mod_dir}/{snake}.h", hdr),
        (f"src/{mod_dir}/{snake}.cc", src),
    ]


# ── Unified publisher typesupport emission ─────────────────────────────────
def _emit_publisher_typesupport(mod: dict, ns: str):
    """Emit transport-specific publisher typesupport (inc.h / inc.cc).

    Produces the transport-specific half of the unified publisher
    template. The returned dict is consumed by
    module_gen._gen_publisher_unified to assemble all 4 files.

    Design:
      * inc.h carries: dds transport header, dds type bridge header,
        IBMW_TS_SENDER_ACCESS_MEMBER (std::optional<SenderAccess<T>>),
        and IBMW_TS_SENDER_ACCESS_INIT (topic bind + emplace).
      * inc.cc carries: no-op (headers do the heavy lifting);
        retained so the main .cc can unconditionally
        `#include "<name>_typesupport.inc.cc"` regardless of whether
        the plugin needs a separate TU.
    """
    msg_type = mod["message_type"]
    topic = mod["topic"]
    dds_types_header = mod.get(
        "_dds_types_header", "example_event_dds_types.h")
    dds_ns = mod.get("_dds_namespace_cpp", "ibmw::protocols::example")
    msg_type_cpp = f"{dds_ns}::{msg_type}"

    inc_h = f"""{_COPYRIGHT}
#pragma once

// Transport-specific typesupport for a dds publisher module.
// Auto-generated by ibmw_gen for the DDS transport.
// This translation-unit fragment is included BOTH by the user-facing
// <module>.h and <module>.cc; it defines the transport-coupled symbols
// the unified publisher template depends on.

#include <optional>

#include "type_support/dds/dds_transport.h"
#include "{dds_types_header}"

// Fully-qualified message type used by the unified template's
// PublishLoop (template body constructs `{msg_type_cpp} msg`).

// Member declaration macro: expands inside the user-facing class body
// as `IBMW_TS_SENDER_ACCESS_MEMBER access_;`, producing a
// std::optional<SenderAccess<T>> bound to the dds-bridge C++ type.
#define IBMW_TS_SENDER_ACCESS_MEMBER \\
  std::optional<ibmw::transport::SenderAccess<{msg_type_cpp}>>

// Setup-time initializer macro: expands inside Setup(ibmw::Runtime
// core) to bind the publisher handle for the configured topic and
// construct the SenderAccess. Kept as a macro so the user-facing
// <module>.cc stays transport-agnostic.
#define IBMW_TS_SENDER_ACCESS_INIT(access_expr, core_expr)        \\
  do {{                                                            \\
    auto _ibmw_pub = (core_expr).GetTransport().GetPublisher(     \\
        "{topic}");                                                \\
    (access_expr).emplace(_ibmw_pub);                             \\
  }} while (0)
"""

    inc_cc = f"""{_COPYRIGHT}
// Transport-specific typesupport TU fragment for a dds publisher.
// Auto-generated by ibmw_gen for the DDS transport. Included verbatim by
// the unified publisher .cc so dds-specific implementation code (if
// any is needed in future) has a dedicated home without requiring
// cmake_gen changes.
//
// Currently dds's publisher path is fully inline (headers + macros in
// inc.h suffice); this file is intentionally otherwise empty.
"""

    return {
        "msg_type_cpp": msg_type_cpp,
        "inc_h_content": inc_h,
        "inc_cc_content": inc_cc,
        "register_call": "access_->RegisterSendType()",
        "publish_stmt": "access_->Send(msg);",
    }


# ── Unified subscriber typesupport emission ────────────────────────────────
def _emit_subscriber_typesupport(mod: dict, ns: str):
    """Emit transport-specific subscriber typesupport (inc.h / inc.cc).

    Design:
      * inc.h carries: dds transport header, dds type bridge header,
        IBMW_TS_RECEIVER_MSG_TYPE (the dds-bridge C++ type),
        IBMW_TS_RECEIVER_ACCESS_SETUP (GetSubscriber + construct
        ReceiverAccess<T> stack-local).
      * inc.cc is an empty stub — dds subscriber path is fully inline
        via headers + macros; kept so the main .cc can
        unconditionally `#include ".../<name>_typesupport.inc.cc"`.
    """
    msg_type = mod["message_type"]
    topic = mod["topic"]
    dds_types_header = mod.get(
        "_dds_types_header", "example_event_dds_types.h")
    dds_ns = mod.get("_dds_namespace_cpp", "ibmw::protocols::example")
    msg_type_cpp = f"{dds_ns}::{msg_type}"

    inc_h = f"""{_COPYRIGHT}
#pragma once

// Transport-specific typesupport for a dds subscriber module.
// Auto-generated by ibmw_gen for the DDS transport.
// This translation-unit fragment is included BOTH by the user-facing
// <module>.h and <module>.cc; it defines the transport-coupled symbols
// the unified subscriber template depends on.

#include "type_support/dds/dds_transport.h"
#include "{dds_types_header}"

// Message C++ type used by the unified template's OnMessage lambda.
#define IBMW_TS_RECEIVER_MSG_TYPE {msg_type_cpp}

// Setup-time initializer: expands inside Setup(ibmw::Runtime core) to
// bind the subscriber handle for the configured topic and construct a
// stack-local ReceiverAccess<T>. Kept as a macro so the user-facing
// <module>.cc stays transport-agnostic.
#define IBMW_TS_RECEIVER_ACCESS_SETUP(core_expr, access_var)        \\
  auto _ibmw_sub = (core_expr).GetTransport().GetSubscriber(        \\
      "{topic}");                                                   \\
  ibmw::transport::ReceiverAccess<{msg_type_cpp}> access_var(_ibmw_sub)
"""

    inc_cc = f"""{_COPYRIGHT}
// Transport-specific typesupport TU fragment for a dds subscriber.
// Auto-generated by ibmw_gen for the DDS transport. Included verbatim by
// the unified subscriber .cc so dds-specific implementation code (if
// any is needed in future) has a dedicated home without requiring
// cmake_gen changes.
//
// Currently dds's subscriber path is fully inline (headers + macros in
// inc.h suffice); this file is intentionally otherwise empty.
"""

    return {
        "msg_type_cpp": msg_type_cpp,
        "inc_h_content": inc_h,
        "inc_cc_content": inc_cc,
    }


# ── Unified loaned-subscriber typesupport emission ─────────────────────────

def _emit_loaned_subscriber_typesupport(mod: dict, ns: str):
    """Emit transport-specific loaned_subscriber typesupport for dds.

    Byte-semantic equivalent of the legacy _gen_loaned_subscriber_dds_{
    header,cc} path: same ros2_types transport + ros2 msg headers, same
    ibmw::ros2::transport::OnMessage<T> attachment wiring, same default
    OnMessage body (log every 5th message).

    Signature: virtual void OnMessage(const std::shared_ptr<const T>& msg).
    """
    name = mod["name"]
    style = mod.get("subscriber_style", "callback")
    if style != "callback":
        raise NotImplementedError(
            f"Module '{name}' (loaned_subscriber,dds): subscriber_style "
            f"'{style}' is reserved but not yet implemented; only "
            f"'callback' is currently supported")

    msg_type = mod["message_type"]
    topic = mod["topic"]
    ros2_type_name = mod["ros2_type_name"]
    ros2_msg_include = mod["ros2_msg_include"]

    inc_h = f"""{_COPYRIGHT}
#pragma once

// Transport-specific typesupport for a dds loaned_subscriber module.
// Auto-generated by ibmw_gen for the DDS transport.
//
// Receives via ibmw::ros2::transport::OnMessage<T> — framework
// abstraction layer auto-dispatches to the dds driver. No driver
// injection required on the subscriber side.
//
// User-facing API: override OnMessage(const shared_ptr<const T>& msg)
// in the user-facing <module>.h class. Default impl below logs
// every 5th message.

#include <memory>

#include "type_support/ros2/ros2_transport.h"
#include <{ros2_msg_include}>

// Convenience alias for use by the ATTACH macro and the default
// OnMessage body in inc.cc.
#define IBMW_TS_LOANED_SUB_MSG_TYPE {msg_type}

// dds: no POD constraint (the ros2 OnMessage path handles non-plain
// types via serialization). Expands to a no-op.
#define IBMW_TS_LOANED_SUB_STATIC_ASSERT /* no-op (dds accepts any) */

// Virtual user callback decl. Signature matches legacy dds loaned
// subscriber — shared_ptr<const T>. The engine MessageHandler thread
// owns the shared_ptr until the callback returns.
#define IBMW_TS_LOANED_SUB_ON_MESSAGE_DECL                                    \\
  virtual void OnMessage(                                                     \\
      const std::shared_ptr<const IBMW_TS_LOANED_SUB_MSG_TYPE>& msg)

// Setup body: GetSubscriber + ros2 OnMessage<T> attach. The lambda
// forwards to the user OnMessage virtual after incrementing stats.
#define IBMW_TS_LOANED_SUB_ATTACH(core_expr)                                  \\
  do {{                                                                        \\
    auto _ibmw_sub = (core_expr).GetTransport().GetSubscriber("{topic}");     \\
    bool _ibmw_ok = ibmw::ros2::transport::OnMessage<                         \\
        IBMW_TS_LOANED_SUB_MSG_TYPE>(                                         \\
        _ibmw_sub, "{ros2_type_name}",                                        \\
        [this](const std::shared_ptr<                                         \\
                   const IBMW_TS_LOANED_SUB_MSG_TYPE>& msg) {{                 \\
          if (!msg) {{                                                         \\
            dropped_.fetch_add(1, std::memory_order_relaxed);                 \\
            return;                                                           \\
          }}                                                                   \\
          received_.fetch_add(1, std::memory_order_relaxed);                  \\
          OnMessage(msg);                                                     \\
        }});                                                                   \\
    if (!_ibmw_ok) {{                                                          \\
      MW_ERROR("OnMessage<{msg_type}> registration failed for "               \\
               "topic '{topic}' (ros2_type='{ros2_type_name}')");             \\
      return false;                                                           \\
    }}                                                                         \\
  }} while (0)
"""

    inc_cc = f"""{_COPYRIGHT}
// Transport-specific typesupport TU fragment for a dds loaned_subscriber.
// Auto-generated by ibmw_gen for the DDS transport.
//
// Defines the default OnMessage() body so the user-facing <module>.cc
// stays transport-agnostic. The `on_message_body` marker delimits the
// user-editable region.

namespace {ns} {{

void {name}::OnMessage(
    const std::shared_ptr<const {msg_type}>& msg) {{
  // IBMW_PUBSUB_LOANED_SUBSCRIBER_BODY_BEGIN
  // IBMW: user code begin (on_message_body)
  // Default impl: log every 5th message. Override in a subclass or
  // edit in place to do real work. msg is non-null on entry.
  uint64_t count = received_.load();
  if (count <= 3 || (count % 5 == 0)) {{
    MW_INFO("[RECV] {{}} #{{}} ptr={{}}",
            "{topic}", count, static_cast<const void*>(msg.get()));
  }}
  // IBMW: user code end (on_message_body)
  // IBMW_PUBSUB_LOANED_SUBSCRIBER_BODY_END
}}

}}  // namespace {ns}
"""

    return {
        "msg_type_cpp": msg_type,
        "inc_h_content": inc_h,
        "inc_cc_content": inc_cc,
    }


# ── Unified loaned-publisher typesupport emission ──────────────────────────
#
# The emitted module binds only to SenderAccess<T>/LoanedSample<T>. Concrete
# transport selection and initialization remain runtime concerns.
def _emit_loaned_publisher_typesupport(mod: dict, ns: str):
    """Bind a DDS-declared loaned publisher to the public loan API."""
    name = mod["name"]
    msg_type = mod["message_type"]
    topic = mod["topic"]
    ros2_msg_include = mod["ros2_msg_include"]

    inc_h = f"""{_COPYRIGHT}
#pragma once

// Public TypeSupport binding for a loaned publisher. The generated module is
// independent of the concrete transport selected by runtime configuration.

#include <chrono>
#include <optional>
#include <type_traits>
#include <utility>

#include "cpp/transport/transport_factory.h"
#include "type_support/ros2/ros2_transport.h"
#include <{ros2_msg_include}>

#define IBMW_TS_LOANED_PUB_MSG_TYPE {msg_type}

#define IBMW_TS_LOANED_PUB_STATIC_ASSERT                                      \\
  static_assert(std::is_trivially_copyable_v<IBMW_TS_LOANED_PUB_MSG_TYPE>,    \\
                "loaned_publisher requires a trivially_copyable message "    \\
                "type ({msg_type})")

#define IBMW_TS_LOANED_PUB_MEMBER                                             \\
  std::optional<ibmw::transport::SenderAccess<                                \\
      IBMW_TS_LOANED_PUB_MSG_TYPE>> publisher_

#define IBMW_TS_LOANED_PUB_SETUP(core_expr)                                   \\
  do {{                                                                        \\
    publisher_.emplace(                                                       \\
        (core_expr).GetTransport().GetPublisher("{topic}"));                  \\
    if (!publisher_->RegisterSendType()) {{                                    \\
      MW_ERROR("{name}: RegisterSendType failed for topic '{topic}'");        \\
      return false;                                                           \\
    }}                                                                         \\
    MW_INFO("{name}: public loaned publisher ready on '{topic}' ({{}} bytes)", \\
            sizeof(IBMW_TS_LOANED_PUB_MSG_TYPE));                             \\
  }} while (0)

#define IBMW_TS_LOANED_PUB_RUN_INIT(core_expr)                                \\
  do {{                                                                        \\
    (void)(core_expr);                                                        \\
    if (!publisher_) {{                                                        \\
      MW_ERROR("{name}: loaned publisher was not initialized");              \\
      return false;                                                           \\
    }}                                                                         \\
  }} while (0)

#define IBMW_TS_LOANED_PUB_HOT_LOOP_BODY(loop_var)                            \\
  do {{                                                                        \\
    auto loop_var = publisher_->Loan();                                       \\
    if (!loop_var.is_valid()) {{                                               \\
      ++fallback_count_;                                                      \\
      MW_ERROR("{name}: loan failed for topic '{topic}'");                    \\
      std::this_thread::sleep_for(std::chrono::milliseconds(100));            \\
      break;                                                                  \\
    }}                                                                         \\
    ++zero_copy_count_;                                                       \\
    /* IBMW_PUBSUB_LOANED_PUBLISHER_BODY_BEGIN */                             \\
    /* IBMW: user code begin (populate_loaned_message) */                     \\
    /* TODO: populate `*loop_var` (or `loop_var->field`) here. */             \\
    /* IBMW: user code end (populate_loaned_message) */                       \\
    /* IBMW_PUBSUB_LOANED_PUBLISHER_BODY_END */                               \\
    if ((publish_count_ % 50) == 0) {{                                         \\
      MW_INFO("[SEND] {{}} #{{}} loaned=true", "{topic}", publish_count_);    \\
    }}                                                                         \\
    bool _ibmw_pub_ok = publisher_->Publish(std::move(loop_var));             \\
    if (_ibmw_pub_ok) {{                                                       \\
      ++publish_count_;                                                       \\
    }} else {{                                                                  \\
      MW_ERROR("{name}: loaned publish failed");                             \\
    }}                                                                         \\
    std::this_thread::sleep_for(std::chrono::milliseconds(100));              \\
  }} while (0)

#define IBMW_TS_LOANED_PUB_PRINT_STATS                                        \\
  do {{                                                                        \\
    uint64_t _ibmw_total = zero_copy_count_ + fallback_count_;                \\
    double _ibmw_ratio = _ibmw_total > 0                                      \\
        ? static_cast<double>(zero_copy_count_) / _ibmw_total * 100.0 : 0.0;  \\
    MW_INFO("========== Loaned Publisher Statistics ==========");             \\
    MW_INFO("Total published : {{}}", publish_count_);                          \\
    MW_INFO("Zero-copy       : {{}} ({{:.1f}}%)", zero_copy_count_, _ibmw_ratio); \\
    MW_INFO("Loan failures   : {{}} (strict mode)", fallback_count_);          \\
    MW_INFO("=================================================");             \\
  }} while (0)
"""

    inc_cc = f"""{_COPYRIGHT}
// Transport-specific typesupport TU fragment for a dds loaned_publisher.
// Auto-generated by ibmw_gen for the DDS transport.
//
// All hot-path mechanics for dds loaned_publisher expand inline from
// the IBMW_TS_LOANED_PUB_* macros in the companion inc.h. This TU
// stub exists to (a) preserve the 4-file unified layout, (b) carry
// the namespace context so the compiler associates the user-facing
// symbols with the right ns, and (c) reserve a place for any future
// out-of-line dds-specific transport code.
//
// The user-code marker pair (`populate_loaned_message`) lives inside
// the IBMW_TS_LOANED_PUB_HOT_LOOP_BODY macro definition in inc.h;
// users edit it there.

#include <chrono>

namespace {ns} {{

// (no out-of-line definitions today — all hooks expand inline via
// macros in {_snake(name)}_typesupport.inc.h)

}}  // namespace {ns}
"""

    return {
        "msg_type_cpp": msg_type,
        "inc_h_content": inc_h,
        "inc_cc_content": inc_cc,
    }


def emit_dds_rpc_server_markerized_typesupport(
    mod: dict,
    ns: str,
    previous_inc_cc: str | None = None,
):
    """Emit a marker-driven dds rpc_server sidecar template.

    Step C keeps the legacy `_gen_*_rpc_server_dds*` path untouched and leaves
    module_gen.py on the legacy dispatcher. This sidecar emitter exists in
    parallel so tests can lock the marker protocol before Step G switches the
    unified RPC path over.
    """
    svc = mod.get("_dds_service_info", {})
    name = mod["name"]
    dds_ns = mod.get("_dds_namespace_cpp", "ibmw::protocols::example")
    rpc_header = mod.get("_rpc_header", "ExampleRpc.ibmw_rpc.dds.h")
    svc_name = svc.get("name", "ExampleService")
    methods = svc.get("methods", [])

    setup_lines: list[str] = []
    decl_lines: list[str] = []
    helper_lines: list[str] = []
    impl_lines: list[str] = []
    method_markers: list[str] = []

    for method in methods:
        method_name = method["name"]
        req_type = f"{dds_ns}::{method['request']}"
        rsp_type = f"{dds_ns}::{method['response']}"
        method_marker = _dds_rpc_server_method_marker(method_name)
        helper_name = (
            f"_ibmw_dds_rpc_server_marshalling_helper_"
            f"{_snake(name)}_{_snake(method_name)}"
        )
        method_markers.append(method_marker)

        setup_lines.extend([
            f"    server_handle->{method_name}.ServeInline(",
            "        [this](ibmw::rpc::ContextRef ctx,",
            f"               const {req_type}& req,",
            f"               {rsp_type}& rsp) {{",
            f"          return {helper_name}(this, ctx, req, rsp);",
            "        });",
            "",
        ])
        decl_lines.extend([
            f"  ibmw::rpc::Status On{method_name}(",
            "      ibmw::rpc::ContextRef ctx,",
            f"      const {req_type}& req,",
            f"      {rsp_type}& rsp);",
        ])
        helper_lines.extend([
            "template <typename ServerT>",
            f"inline ibmw::rpc::Status {helper_name}(",
            "    ServerT* self,",
            "    ibmw::rpc::ContextRef ctx,",
            f"    const {req_type}& req,",
            f"    {rsp_type}& rsp) {{",
            "  // Transport-owned dispatch/marshalling scaffold stays outside",
            "  // the user marker region; only On<Method>() is user-editable.",
            f"  return self->On{method_name}(ctx, req, rsp);",
            "}",
            "",
        ])
        impl_lines.extend([
            f"ibmw::rpc::Status {name}::On{method_name}(",
            "    ibmw::rpc::ContextRef ctx,",
            f"    const {req_type}& req,",
            f"    {rsp_type}& rsp) {{",
            "  (void)ctx;",
            "  (void)req;",
            "  (void)rsp;",
            f"  {_format_marker_anchor(method_marker, 'BEGIN')}",
            f'  MW_INFO("{method_name} called");',
            "  // TODO: implement handler logic, then remove this default return.",
            "  return ibmw::rpc::Status(IBMW_RPC_STATUS_SVR_NOT_IMPLEMENTED);",
            f"  {_format_marker_anchor(method_marker, 'END')}",
            "  return ibmw::rpc::Status();",
            "}",
            "",
        ])

    def _macro_body(lines: list[str]) -> str:
        if not lines:
            return "  /* no handlers */"
        return "\n".join(
            f"{line} \\" if line else "\\" for line in lines
        ).rstrip()

    setup_body = _macro_body([
        "  do {",
        "    ctx_ptr_ = std::make_shared<ibmw::context::Context>((core_expr));",
        "    ctx_ptr_->LetMe();",
        "    try {",
        f"      auto server_handle = ctx_ptr_->CreateServer<{dds_ns}::{svc_name}CoServer>();",
        *setup_lines,
        f'      MW_INFO("{name}::Setup succeeded.");',
        "    } catch (const std::exception& e) {",
        f'      MW_ERROR("{name}::Setup failed: {{}}", e.what());',
        "      return false;",
        "    }",
        "  } while (0)",
    ])
    handler_decl = _macro_body(decl_lines)

    template_inc_cc = f"""{_COPYRIGHT}
// Transport-specific marker-driven typesupport TU fragment for a dds rpc_server.
// Auto-generated by ibmw_gen as an RPC server typesupport sidecar.

#undef IBMW_TS_RPC_SERVER_HANDLER_IMPL

#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(name)

namespace {ns} {{

{chr(10).join(helper_lines + impl_lines)}

}}  // namespace {ns}"""
    if previous_inc_cc and method_markers:
        preserved = extract_marker_regions(previous_inc_cc, method_markers)
        inc_cc = preserve_marker_regions(template_inc_cc, preserved)
    else:
        inc_cc = template_inc_cc

    inc_h = f"""{_COPYRIGHT}
#pragma once

#include <memory>

#include "cpp/ibmw_sdk.h"
#include "{rpc_header}"

#define IBMW_TS_RPC_SERVER_SETUP(core_expr)                                   \
{setup_body}

#define IBMW_TS_RPC_SERVER_HANDLER_DECL                                        \
{handler_decl}

#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(name) /* defined in inc.cc */
"""

    return {
        "msg_type_cpp": f"{dds_ns}::{svc_name}CoServer",
        "inc_h_content": inc_h,
        "inc_cc_content": inc_cc,
    }

def emit_dds_rpc_client_markerized_typesupport(
    mod: dict,
    ns: str,
    previous_inc_cc: str | None = None,
):
    """Emit a marker-driven dds rpc_client sidecar template.

    Step D keeps the legacy `_gen_*_rpc_client_dds*` path untouched and leaves
    module_gen.py on the legacy dispatcher. This sidecar emitter exists in
    parallel so tests can lock the coroutine client marker protocol before
    Step G switches the unified RPC path over.
    """
    svc = mod.get("_dds_service_info", {})
    dds_ns = mod.get("_dds_namespace_cpp", "ibmw::protocols::example")
    rpc_header = mod.get("_rpc_header", "ExampleRpc.ibmw_rpc.dds.h")
    svc_name = svc.get("name", "ExampleService")
    methods = svc.get("methods", [])
    name = mod.get("name", "RpcClientModule")
    helper_name = f"_ibmw_dds_rpc_client_loop_body_impl_{_snake(str(name))}"

    helper_lines = [
        "template <typename SelfT>",
        f"inline ibmw::co::Task<void> {helper_name}(SelfT* self) {{",
        "  try {",
        '    MW_INFO("Start RPC loop.");',
        "",
        "    uint32_t counter = 0;",
        "    while (self->ctx_ptr_->Running()) {",
        "      self->ctx_ptr_->LetMe();",
        "",
    ]
    method_markers: list[str] = []

    for method in methods:
        method_name = method["name"]
        req_type = f"{dds_ns}::{method['request']}"
        rsp_type = f"{dds_ns}::{method['response']}"
        request_marker = _dds_rpc_client_method_marker(
            _DDS_RPC_CLIENT_MARKERS[0], method_name
        )
        response_marker = _dds_rpc_client_method_marker(
            _DDS_RPC_CLIENT_MARKERS[1], method_name
        )
        status_marker = _dds_rpc_client_method_marker(
            _DDS_RPC_CLIENT_MARKERS[2], method_name
        )
        method_markers.extend([request_marker, response_marker, status_marker])

        helper_lines.extend([
            f"      // --- Call {method_name} ---",
            "      {",
            f"        {req_type} req;",
            f"        {rsp_type} rsp;",
            f"        {_format_marker_anchor(request_marker, 'BEGIN')}",
            "        // TODO: populate req fields",
            f"        {_format_marker_anchor(request_marker, 'END')}",
            "",
            f"        auto status = co_await self->client_.{method_name}.Call(req, rsp);",
            "        self->ctx_ptr_->LetMe();",
            "",
            "        if (status.OK()) {",
            "          (void)rsp;",
            f"          {_format_marker_anchor(response_marker, 'BEGIN')}",
            f'          MW_INFO("{method_name} OK");',
            "          // TODO: consume rsp fields",
            f"          {_format_marker_anchor(response_marker, 'END')}",
            "        }",
            "",
            f"        {_format_marker_anchor(status_marker, 'BEGIN')}",
            "        if (!status.OK()) {",
            f'          MW_WARN("{method_name} FAILED: {{}}", status.ToString());',
            "        }",
            f"        {_format_marker_anchor(status_marker, 'END')}",
            "      }",
            "",
        ])

    helper_lines.extend([
        "      ++counter;",
        "",
        "      // Wait 2 seconds before next round",
        "      co_await ibmw::co::ScheduleAfter(",
        "          ibmw::co::IbmwExecutor(self->time_executor_),",
        "          std::chrono::seconds(2));",
        "    }",
        "",
        '    MW_INFO("Exit RPC loop.");',
        "  } catch (const std::exception& e) {",
        '    MW_ERROR("RpcLoop failed: {}", e.what());',
        "  }",
        "",
        "  co_return;",
        "}",
    ])
    helper_block = "\n".join(helper_lines)

    template_inc_cc = f"""{_COPYRIGHT}
// Transport-specific marker-driven typesupport TU fragment for a dds rpc_client.
// Auto-generated by ibmw_gen as an RPC client typesupport sidecar.

namespace {ns} {{

{helper_block}

}}  // namespace {ns}
"""
    if previous_inc_cc and method_markers:
        preserved = extract_marker_regions(previous_inc_cc, method_markers)
        inc_cc = preserve_marker_regions(template_inc_cc, preserved)
    else:
        inc_cc = template_inc_cc

    inc_h = f"""{_COPYRIGHT}
#pragma once

#include <chrono>
#include <memory>

#include "cpp/ibmw_sdk.h"
#include "cpp/co/async_scope.h"
#include "cpp/co/ibmw_context.h"
#include "cpp/co/on.h"
#include "cpp/co/schedule.h"
#include "cpp/co/sync_wait.h"
#include "{rpc_header}"

#define IBMW_TS_RPC_CLIENT_LOOP_DECL                                           \
  ibmw::co::Task<void> RpcLoop();                                              \
                                                                                \
 private:                                                                      \
  template <typename SelfT>                                                    \
  friend ibmw::co::Task<void> {helper_name}(SelfT*);                          \
  auto GetLogger() const {{ return ctx_ptr_->GetLogger(); }}                   \
  std::shared_ptr<ibmw::context::Context> ctx_ptr_;                            \
  {dds_ns}::{svc_name}CoClient client_;                                        \
  ibmw::executor::ExecutorHandle time_executor_;                               \
  ibmw::co::AsyncScope scope_;

#define IBMW_TS_RPC_CLIENT_LOOP_IMPL(name)                                     \
  ibmw::co::Task<void> name::RpcLoop() {{                                      \
    co_await {helper_name}(this);                                              \
    co_return;                                                                 \
  }}
"""

    return {
        "msg_type_cpp": f"{dds_ns}::{svc_name}CoClient",
        "inc_h_content": inc_h,
        "inc_cc_content": inc_cc,
    }


def emit_rpc_server_typesupport(mod: dict, ns: str):
    svc = mod.get("_dds_service_info", {})
    name = mod["name"]
    dds_ns = mod.get("_dds_namespace_cpp", "ibmw::protocols::example")
    rpc_header = mod.get("_rpc_header", "ExampleRpc.ibmw_rpc.dds.h")
    svc_name = svc.get("name", "ExampleService")
    methods = svc.get("methods", [])

    setup_lines = []
    decl_lines = []
    impl_lines = []
    for m in methods:
        method_name = m["name"]
        req_type = f"{dds_ns}::{m['request']}"
        rsp_type = f"{dds_ns}::{m['response']}"
        marker = f"{_snake(method_name)}_handler"

        setup_lines.extend([
            f"    server_handle->{method_name}.ServeInline(",
            "        [this](ibmw::rpc::ContextRef ctx,",
            f"               const {req_type}& req,",
            f"               {rsp_type}& rsp) {{",
            f"          return On{method_name}(ctx, req, rsp);",
            "        });",
            "",
        ])
        decl_lines.extend([
            f"  ibmw::rpc::Status On{method_name}(",
            "      ibmw::rpc::ContextRef ctx,",
            f"      const {req_type}& req,",
            f"      {rsp_type}& rsp);",
        ])
        impl_lines.extend([
            f"ibmw::rpc::Status {name}::On{method_name}(",
            "    ibmw::rpc::ContextRef ctx,",
            f"    const {req_type}& req,",
            f"    {rsp_type}& rsp) {{",
            "  (void)ctx;",
            "  (void)req;",
            "  (void)rsp;",
            f"  // IBMW: user code begin ({marker})",
            f"  MW_INFO(\"{method_name} called\");",
            "  // TODO: implement handler logic, then remove this default return.",
            "  return ibmw::rpc::Status(IBMW_RPC_STATUS_SVR_NOT_IMPLEMENTED);",
            f"  // IBMW: user code end ({marker})",
            "  return ibmw::rpc::Status();",
            "}",
            "",
        ])

    def _macro_body(lines):
        if not lines:
            return "  /* no handlers */"
        return "\n".join(f"{line} \\\\" if line else "\\" for line in lines).rstrip()

    setup_body = _macro_body([
        "  do {",
        "    ctx_ptr_ = std::make_shared<ibmw::context::Context>((core_expr));",
        "    ctx_ptr_->LetMe();",
        "    try {",
        f"      auto server_handle = ctx_ptr_->CreateServer<{dds_ns}::{svc_name}CoServer>();",
        *setup_lines,
        f"      MW_INFO(\"{name}::Setup succeeded.\");",
        "    } catch (const std::exception& e) {",
        f"      MW_ERROR(\"{name}::Setup failed: {{}}\", e.what());",
        "      return false;",
        "    }",
        "  } while (0)",
    ])
    handler_decl = _macro_body(decl_lines)
    handler_impl = _macro_body(impl_lines)

    inc_h = f"""{_COPYRIGHT}
#pragma once

#include <memory>

#include "cpp/ibmw_sdk.h"
#include "{rpc_header}"

#define IBMW_TS_RPC_SERVER_SETUP(core_expr)                                   \\
{setup_body}

#define IBMW_TS_RPC_SERVER_HANDLER_DECL                                        \\
{handler_decl}

#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(name) /* defined in inc.cc */
"""

    inc_cc = f"""{_COPYRIGHT}
#undef IBMW_TS_RPC_SERVER_HANDLER_IMPL

#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(name)
{handler_impl}
"""

    return {
        "msg_type_cpp": f"{dds_ns}::{svc_name}CoServer",
        "inc_h_content": inc_h,
        "inc_cc_content": inc_cc,
    }


def emit_rpc_client_typesupport(mod: dict, ns: str):
    svc = mod.get("_dds_service_info", {})
    name = mod.get("name", "RpcClientModule")
    dds_ns = mod.get("_dds_namespace_cpp", "ibmw::protocols::example")
    rpc_header = mod.get("_rpc_header", "ExampleRpc.ibmw_rpc.dds.h")
    svc_name = svc.get("name", "ExampleService")
    methods = svc.get("methods", [])
    helper_name = f"_ibmw_dds_rpc_client_loop_body_impl_{_snake(str(name))}"

    helper_lines = [
        "template <typename SelfT>",
        f"inline ibmw::co::Task<void> {helper_name}(SelfT* self) {{",
        "  try {",
        '    MW_INFO("Start RPC loop.");',
        "",
        "    uint32_t counter = 0;",
        "    while (self->ctx_ptr_->Running()) {",
        "      self->ctx_ptr_->LetMe();",
        "",
    ]
    for m in methods:
        method_name = m["name"]
        req_type = f"{dds_ns}::{m['request']}"
        rsp_type = f"{dds_ns}::{m['response']}"
        helper_lines.extend([
            f"      // --- Call {method_name} ---",
            "      {",
            f"        {req_type} req;",
            f"        {rsp_type} rsp;",
            "        // TODO: populate req fields",
            "",
            f"        auto status = co_await self->client_.{method_name}.Call(req, rsp);",
            "        self->ctx_ptr_->LetMe();",
            "",
            "        if (status.OK()) {",
            f'          MW_INFO("{method_name} OK");',
            "        } else {",
            f'          MW_WARN("{method_name} FAILED: {{}}", status.ToString());',
            "        }",
            "      }",
            "",
        ])
    helper_lines.extend([
        "      ++counter;",
        "",
        "      // Wait 2 seconds before next round",
        "      co_await ibmw::co::ScheduleAfter(",
        "          ibmw::co::IbmwExecutor(self->time_executor_),",
        "          std::chrono::seconds(2));",
        "    }",
        "",
        '    MW_INFO("Exit RPC loop.");',
        "  } catch (const std::exception& e) {",
        '    MW_ERROR("RpcLoop failed: {}", e.what());',
        "  }",
        "",
        "  co_return;",
        "}",
    ])
    helper_block = "\n".join(helper_lines)

    inc_h = f"""{_COPYRIGHT}
#pragma once

#include <chrono>
#include <memory>

#include "cpp/ibmw_sdk.h"
#include "cpp/co/async_scope.h"
#include "cpp/co/ibmw_context.h"
#include "cpp/co/on.h"
#include "cpp/co/schedule.h"
#include "cpp/co/sync_wait.h"
#include "{rpc_header}"

#define IBMW_TS_RPC_CLIENT_LOOP_DECL                                           \\
  ibmw::co::Task<void> RpcLoop();                                              \\
                                                                                \\
 private:                                                                      \\
  template <typename SelfT>                                                    \\
  friend ibmw::co::Task<void> {helper_name}(SelfT*);                          \\
  auto GetLogger() const {{ return ctx_ptr_->GetLogger(); }}                   \\
  std::shared_ptr<ibmw::context::Context> ctx_ptr_;                            \\
  {dds_ns}::{svc_name}CoClient client_;                                        \\
  ibmw::executor::ExecutorHandle time_executor_;                               \\
  ibmw::co::AsyncScope scope_;

#define IBMW_TS_RPC_CLIENT_LOOP_IMPL(name)                                     \\
  ibmw::co::Task<void> name::RpcLoop() {{                                      \\
    co_await {helper_name}(this);                                              \\
    co_return;                                                                 \\
  }}

namespace {ns} {{

{helper_block}

}}  // namespace {ns}
"""

    inc_cc = f"""{_COPYRIGHT}
// Transport-specific typesupport TU fragment for a dds rpc_client.
// Auto-generated by ibmw_gen. The coroutine loop helper and
// IBMW_TS_RPC_CLIENT_LOOP_* macro pair live in the companion inc.h so the
// coroutine contract is locked in one place for the unified dispatcher.
"""

    return {
        "msg_type_cpp": f"{dds_ns}::{svc_name}CoClient",
        "inc_h_content": inc_h,
        "inc_cc_content": inc_cc,
    }


register(TransportSpec(
    name="dds",
    emit_module=_emit_module,
    emit_publisher_typesupport=_emit_publisher_typesupport,
    emit_subscriber_typesupport=_emit_subscriber_typesupport,
    emit_loaned_subscriber_typesupport=_emit_loaned_subscriber_typesupport,
    emit_loaned_publisher_typesupport=_emit_loaned_publisher_typesupport,
    emit_rpc_server_typesupport=emit_rpc_server_typesupport,
    emit_rpc_client_typesupport=emit_rpc_client_typesupport,
    rpc_server_markers=list(_DDS_RPC_SERVER_MARKERS),
    rpc_client_markers=list(_DDS_RPC_CLIENT_MARKERS),
    meta={
        "is_minimal_viable_target": True,
        "stage": "unified-loaned-publisher",
        "owns_type_keys": sorted(_DDS_TYPE_KEYS),
    },
))
