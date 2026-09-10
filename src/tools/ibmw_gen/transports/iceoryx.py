#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# Iceoryx transport plugin with full ownership of code emission.
#
# The function bodies were migrated from module_gen.py; module_gen no longer
# contains iceoryx-specific emission code. This mirrors the DDS plugin layout.
#
# Plugin contract (transports/__init__.py:TransportSpec.emit_module):
#   emit_module(mod, ns, mod_dir) -> list[(rel_path, content)]
#       Returns [.h, .cc] pair for any (mod['type'], "iceoryx")
#       combination this plugin claims, or None to fall through to
#       the legacy _GENERATORS dispatch (no longer reachable for
#       "iceoryx" since the legacy table no longer holds iceoryx rows
#       post-migration).

"""Iceoryx transport plugin for ibmw_gen — owns all iceoryx code emission."""

from generators._common import _snake, _COPYRIGHT
from . import TransportSpec, register


# Type-keys this plugin claims ownership of. Mirrors the iceoryx rows
# that used to live in module_gen._GENERATORS pre-migration.
_ICEORYX_TYPE_KEYS = frozenset({
    "direct_iceoryx_publisher",
    "direct_iceoryx_subscriber",
    "loaned_publisher",
    "loaned_subscriber",
})


def _gen_direct_iceoryx_publisher_header(mod: dict, ns: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]
    ros2_msg_include = mod["ros2_msg_include"]

    return f"""{_COPYRIGHT}
//
// IBMW: NOT TRANSPORT-DECOUPLED — direct iceoryx API.
// R10 escape-hatch template (direct_iceoryx_publisher):
// generated code uses ibmw::transport::publisher_for<T>() directly for
// the iceoryx path. User code is transport-coupled by design — to
// preserve transport-decoupling, prefer the agnostic `publisher` /
// `loaned_publisher` templates when they satisfy the use case.

#pragma once

#include <atomic>
#include <thread>

#include "cpp/module_base.h"
#include "cpp/transport/transport_factory.h"
// IBMW_R1_EXEMPT_BEGIN
#include "iceoryx_ext/iceoryx_transport_driver.h"
// IBMW_R1_EXEMPT_END
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

  std::thread publish_thread_;
  std::atomic<bool> running_{{false}};
  uint32_t publish_count_{{0}};
}};

}}  // namespace {ns}
"""


def _gen_direct_iceoryx_publisher_cc(mod: dict, ns: str, snake_dir: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]
    topic = mod["topic"]

    return f"""{_COPYRIGHT}
#include "{snake_dir}/{_snake(name)}.h"

namespace {ns} {{

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;

  auto publisher = ibmw::transport::publisher_for<{msg_type}>(core_, "{topic}");
  bool success = publisher.RegisterSendType();

  if (!success) {{
    MW_ERROR("Failed to register direct iceoryx publisher type on topic: {topic}");
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
    {msg_type} msg;
    // TODO: populate msg fields here
    publish_count_++;

    auto publisher = ibmw::transport::publisher_for<{msg_type}>(core_, "{topic}");
    publisher.Send(msg);

    MW_INFO("Published direct iceoryx message #{{}}", publish_count_);

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }}
}}

}}  // namespace {ns}
"""


def _gen_direct_iceoryx_subscriber_header(mod: dict, ns: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]
    ros2_msg_include = mod["ros2_msg_include"]

    return f"""{_COPYRIGHT}
//
// IBMW: NOT TRANSPORT-DECOUPLED — direct iceoryx API.
// R10 escape-hatch template (direct_iceoryx_subscriber):
// generated code uses ibmw::transport::subscriber_for<T>() directly for
// the iceoryx path. User code is transport-coupled by design — to
// preserve transport-decoupling, prefer the agnostic `subscriber` /
// `loaned_subscriber` templates when they satisfy the use case.

#pragma once

#include "cpp/module_base.h"
#include "cpp/transport/transport_factory.h"
// IBMW_R1_EXEMPT_BEGIN
#include "iceoryx_ext/iceoryx_transport_driver.h"
// IBMW_R1_EXEMPT_END
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


def _gen_direct_iceoryx_subscriber_cc(mod: dict, ns: str, snake_dir: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]
    topic = mod["topic"]

    return f"""{_COPYRIGHT}
#include "{snake_dir}/{_snake(name)}.h"

namespace {ns} {{

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;

  auto subscriber = ibmw::transport::subscriber_for<{msg_type}>(core_, "{topic}");
  bool success = subscriber.OnMessage(
      [this](ibmw::transport::ContextRef ctx,
             const std::shared_ptr<const {msg_type}>& msg) {{
        OnMessage(ctx, msg);
      }});

  if (!success) {{
    MW_ERROR("Failed to subscribe direct iceoryx topic: {topic}");
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

  MW_INFO("Received direct iceoryx message [{{}}]", receive_count_);
  // TODO: process msg fields here

  auto serialization_type = ctx.GetSerializationType();
  MW_DEBUG("Serialization type: {{}}", serialization_type);
}}

}}  // namespace {ns}
"""


# ── Loaned Publisher (iceoryx LoanedSample zero-copy SHM API) ───────────────
#
# Generates a publisher module that loans a SHM chunk from the iceoryx driver
# via the framework-level `ibmw::transport::LoanedSample<T>` typed loan API
# (driver-agnostic), writes the message in place, and publishes — true
# zero-copy with no fallback path.
#
# Schema enforces: zero_copy_config == 'strict'. The `iceoryx_msg_type` yaml
# field was removed because the log-only display value now derives from
# `message_type` (these two have always been identical in every known yaml,
# iceoryx_msg_type is only a logging placeholder and future wire-format escape
# hatch). Byte-identical regeneration is preserved.
# Generated code adds a compile-time `static_assert(std::is_trivially_copyable_v<T>)`
# (D2: no runtime blacklist; the compiler rejects non-POD message types early).

def _gen_loaned_subscriber_iceoryx_header(mod: dict, ns: str) -> str:
    name = mod["name"]
    style = mod["subscriber_style"]
    if style != "callback":
        raise NotImplementedError(
            f"Module '{name}' (loaned_subscriber,iceoryx): subscriber_style "
            f"'{style}' is reserved but not yet implemented; only "
            f"'callback' is currently supported")
    msg_type = mod["message_type"]
    ros2_msg_include = mod["ros2_msg_include"]

    return f"""{_COPYRIGHT}
//
// Loaned Subscriber (iceoryx) — receives via the framework
// LoanedSubscriber<T>::AttachBytes() typed zero-copy fast lane. No driver
// injection required (dispatcher routes AttachLoaned to the iceoryx
// driver; iceoryx owns the listener thread + chunk lifetime).
//
// User-facing API: override OnMessage(const T& msg, size_t size). The
// callback fires on the iceoryx Listener thread pool — keep it short
// and lock-free. msg is a const reference into the SHM chunk; do NOT
// retain past return.

#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>

#include "cpp/module_base.h"
#include "cpp/transport/loaned_subscriber.h"
#include "type_support/ros2/ros2_transport.h"
#include <{ros2_msg_include}>

namespace {ns} {{

class {name} : public ibmw::Module {{
 public:
  {name}() = default;
  ~{name}() override = default;

  // Compile-time POD enforcement: LoanedSubscriber<T> requires trivially
  // copyable T (the SHM chunk is reinterpret_cast to const T&).
  static_assert(std::is_trivially_copyable_v<{msg_type}>,
                "iceoryx loaned_subscriber requires a trivially_copyable "
                "message type ({msg_type})");

  ibmw::ModuleDescriptor Info() const override {{
    return ibmw::ModuleDescriptor{{.module_name = "{name}"}};
  }}

  bool Setup(ibmw::Runtime core) override;
  bool Run() override;
  void Stop() override;

 protected:
  // User callback. Default impl logs a counter; subclass and override
  // to do real work. msg is a const-ref into the SHM chunk and dies
  // when the callback returns; size is the loan-time payload byte
  // count (>= sizeof({msg_type})).
  virtual void OnMessage(const {msg_type}& msg, std::size_t size);

  auto GetLogger() {{ return core_.GetLogger(); }}

 private:
  ibmw::Runtime core_;
  std::atomic<uint64_t> received_{{0}};
  std::atomic<uint64_t> dropped_{{0}};
}};

}}  // namespace {ns}
"""


def _gen_loaned_subscriber_iceoryx_cc(mod: dict, ns: str, snake_dir: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]
    topic = mod["topic"]
    iox_msg_type = mod.get("iceoryx_msg_type", msg_type)   # derive from message_type

    return f"""{_COPYRIGHT}
#include "{snake_dir}/{_snake(name)}.h"

#include <string>

namespace {ns} {{

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;

  // CRITICAL: msg_type string MUST exactly match what the publisher
  // side registers through the public POD loan capability, since the
  // iceoryx driver keys pod_pub_map_ / pod_sub_map_ on (topic, msg_type).
  // We use the same "ros2:<ros2_type_name>" identity that the publisher
  // template uses (see _gen_loaned_publisher_iceoryx_cc). The yaml field
  // `iceoryx_msg_type` is currently informational only — used in log
  // messages — and reserved for a future wire-format escape hatch.
  static const std::string kAttachMsgType =
      std::string("ros2:") + ibmw::ros2::transport::GetRos2TypeName<{msg_type}>();

  auto subscriber = core_.GetTransport().GetSubscriber("{topic}");
  bool ok = ibmw::transport::LoanedSubscriber<{msg_type}>::AttachBytes(
      subscriber, kAttachMsgType,
      [this](const {msg_type}& msg, std::size_t size) {{
        received_.fetch_add(1, std::memory_order_relaxed);
        OnMessage(msg, size);
      }});
  if (!ok) {{
    MW_ERROR("{name}: LoanedSubscriber::AttachBytes failed for "
             "topic='{topic}', iceoryx_msg_type='{iox_msg_type}'");
    return false;
  }}

  MW_INFO("{name}::Setup succeeded (topic='{topic}', "
          "iceoryx_msg_type='{iox_msg_type}', zero-copy SHM receive).");
  return true;
}}

bool {name}::Run() {{
  // Nothing to do — chunks already flow through the AttachBytes
  // handler installed in Setup(). The iceoryx Listener thread pool
  // owns chunk delivery and lifetime; no driver pointer needed.
  MW_INFO("{name}::Run started.");
  return true;
}}

void {name}::Stop() {{
  MW_INFO("{name}::Stop succeeded (received={{}}, dropped={{}}, "
          "true zero-copy SHM).",
          received_.load(), dropped_.load());
}}

void {name}::OnMessage(const {msg_type}& msg, std::size_t size) {{
  // Default impl: log every 5th message. Override in a subclass or
  // edit in place to do real work. The reference points into the
  // iceoryx SHM chunk -- true zero-copy. Do NOT retain `msg` past
  // return; the chunk is released when this callback exits.
  uint64_t count = received_.load();
  if (count <= 3 || (count % 5 == 0)) {{
    MW_INFO("[RECV] {{}} #{{}} size={{}} ptr={{}}",
            "{topic}", count, size,
            static_cast<const void*>(&msg));
  }}
}}

}}  // namespace {ns}
"""


# ── Plugin dispatch table ───────────────────────────────────────────────────

# Map (mod['type'], "iceoryx") -> (header_emitter, cc_emitter). Direct
# replacement for the legacy module_gen._GENERATORS rows.
_ICEORYX_GENERATORS = {
    ("direct_iceoryx_publisher", "iceoryx"): (
        _gen_direct_iceoryx_publisher_header, _gen_direct_iceoryx_publisher_cc),
    ("direct_iceoryx_subscriber", "iceoryx"): (
        _gen_direct_iceoryx_subscriber_header, _gen_direct_iceoryx_subscriber_cc),
    ("loaned_subscriber", "iceoryx"): (
        _gen_loaned_subscriber_iceoryx_header, _gen_loaned_subscriber_iceoryx_cc),
}


def _emit_module(mod: dict, ns: str, mod_dir: str):
    """Emit the .h / .cc pair for an iceoryx-transport module.

    Returns list[(rel_path, content)] or None if this plugin does not
    handle the given (mod['type'], "iceoryx") pair.

    `loaned_subscriber` is handled by the
    unified path in module_gen._gen_loaned_subscriber_unified via
    emit_loaned_subscriber_typesupport below; return None here so the
    dispatcher falls through to the unified emitter.

    `loaned_publisher` is handled the same way
    via emit_loaned_publisher_typesupport.
    """
    mod_type = mod.get("type")
    if mod_type not in _ICEORYX_TYPE_KEYS:
        return None

    # Defer to the unified path in module_gen (see register() below).
    if mod_type in ("loaned_subscriber", "loaned_publisher"):
        return None

    pair = _ICEORYX_GENERATORS.get((mod_type, "iceoryx"))
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


# ── Unified loaned-subscriber typesupport emission ─────────────────────────
def _emit_loaned_subscriber_typesupport(mod: dict, ns: str):
    """Emit transport-specific loaned_subscriber typesupport for iceoryx.

    Byte-semantic equivalent of the legacy
    _gen_loaned_subscriber_iceoryx_{header,cc} path: same includes,
    same static_assert (trivially_copyable), same
    LoanedSubscriber<T>::AttachBytes wiring keyed on
    `"ros2:" + GetRos2TypeName<T>()`, same default OnMessage body
    (log every 5th message with size + pointer).

    Signature: virtual void OnMessage(const T& msg, std::size_t size).
    """
    name = mod["name"]
    style = mod.get("subscriber_style", "callback")
    if style != "callback":
        raise NotImplementedError(
            f"Module '{name}' (loaned_subscriber,iceoryx): subscriber_style "
            f"'{style}' is reserved but not yet implemented; only "
            f"'callback' is currently supported")

    msg_type = mod["message_type"]
    topic = mod["topic"]
    ros2_msg_include = mod["ros2_msg_include"]
    iox_msg_type = mod.get("iceoryx_msg_type", msg_type)  # log-only value

    inc_h = f"""{_COPYRIGHT}
#pragma once

// Transport-specific typesupport for an iceoryx loaned_subscriber module.
// Auto-generated by ibmw_gen for the iceoryx transport.
//
// Receives via ibmw::transport::LoanedSubscriber<T>::AttachBytes() — the
// framework-level typed zero-copy fast lane. No driver injection
// required (the dispatcher routes AttachLoaned to the iceoryx driver;
// iceoryx owns the Listener thread pool + chunk lifetime).
//
// User-facing API: override OnMessage(const T& msg, size_t size) in
// the user-facing <module>.h class. Default impl below logs every
// 5th message; msg is a const-ref into the SHM chunk — do NOT retain
// past return.

#include <cstddef>
#include <type_traits>

#include "cpp/transport/loaned_subscriber.h"
#include "type_support/ros2/ros2_transport.h"
#include <{ros2_msg_include}>

// Convenience alias for use by the ATTACH macro and the default
// OnMessage body in inc.cc.
#define IBMW_TS_LOANED_SUB_MSG_TYPE {msg_type}

// iceoryx: compile-time POD enforcement. The SHM chunk is
// reinterpret_cast to const T&; non-trivially-copyable T would
// invoke ctors/dtors across process boundaries and is rejected
// early by the compiler (D2: no runtime blacklist).
#define IBMW_TS_LOANED_SUB_STATIC_ASSERT                                      \\
  static_assert(std::is_trivially_copyable_v<IBMW_TS_LOANED_SUB_MSG_TYPE>,    \\
                "iceoryx loaned_subscriber requires a trivially_copyable "    \\
                "message type ({msg_type})")

// Virtual user callback decl. Signature matches legacy iceoryx loaned
// subscriber — (const T&, std::size_t size). The reference points into
// the SHM chunk and dies when the callback returns.
#define IBMW_TS_LOANED_SUB_ON_MESSAGE_DECL                                    \\
  virtual void OnMessage(const IBMW_TS_LOANED_SUB_MSG_TYPE& msg,              \\
                         std::size_t size)

// Setup body: GetSubscriber + LoanedSubscriber<T>::AttachBytes. The
// attach-key MUST exactly match what the publisher side registers through
// the public POD loan capability, since the selected transport
// keys pod_pub_map_ / pod_sub_map_ on (topic, msg_type). We use the
// same "ros2:<ros2_type_name>" identity the publisher template uses
// (see _gen_loaned_publisher_iceoryx_cc). The yaml field
// `iceoryx_msg_type` is currently log-only and reserved for a future
// wire-format escape hatch.
#define IBMW_TS_LOANED_SUB_ATTACH(core_expr)                                  \\
  do {{                                                                        \\
    static const std::string _ibmw_attach_msg_type =                          \\
        std::string("ros2:") +                                                \\
        ibmw::ros2::transport::GetRos2TypeName<                               \\
            IBMW_TS_LOANED_SUB_MSG_TYPE>();                                   \\
    auto _ibmw_sub = (core_expr).GetTransport().GetSubscriber("{topic}");     \\
    bool _ibmw_ok = ibmw::transport::LoanedSubscriber<                        \\
        IBMW_TS_LOANED_SUB_MSG_TYPE>::AttachBytes(                            \\
        _ibmw_sub, _ibmw_attach_msg_type,                                     \\
        [this](const IBMW_TS_LOANED_SUB_MSG_TYPE& msg,                        \\
               std::size_t size) {{                                            \\
          received_.fetch_add(1, std::memory_order_relaxed);                  \\
          OnMessage(msg, size);                                               \\
        }});                                                                   \\
    if (!_ibmw_ok) {{                                                          \\
      MW_ERROR("{name}: LoanedSubscriber::AttachBytes failed for "            \\
               "topic='{topic}', iceoryx_msg_type='{iox_msg_type}'");         \\
      return false;                                                           \\
    }}                                                                         \\
  }} while (0)
"""

    inc_cc = f"""{_COPYRIGHT}
// Transport-specific typesupport TU fragment for an iceoryx loaned_subscriber.
// Auto-generated by ibmw_gen for the iceoryx transport.
//
// Defines the default OnMessage() body so the user-facing <module>.cc
// stays transport-agnostic. The `on_message_body` marker delimits the
// user-editable region.

#include <string>

namespace {ns} {{

void {name}::OnMessage(const {msg_type}& msg, std::size_t size) {{
  // IBMW_PUBSUB_LOANED_SUBSCRIBER_BODY_BEGIN
  // IBMW: user code begin (on_message_body)
  // Default impl: log every 5th message. Override in a subclass or
  // edit in place to do real work. The reference points into the
  // iceoryx SHM chunk -- true zero-copy. Do NOT retain `msg` past
  // return; the chunk is released when this callback exits.
  uint64_t count = received_.load();
  if (count <= 3 || (count % 5 == 0)) {{
    MW_INFO("[RECV] {{}} #{{}} size={{}} ptr={{}}",
            "{topic}", count, size,
            static_cast<const void*>(&msg));
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
# This path uses the same stable SenderAccess<T>/LoanedSample<T> surface as
# the DDS declaration. Only the public POD capability preparation differs.
#
def _emit_loaned_publisher_typesupport(mod: dict, ns: str):
    """Bind an iceoryx-declared loaned publisher to the public loan API."""
    name = mod["name"]
    msg_type = mod["message_type"]
    topic = mod["topic"]
    ros2_msg_include = mod["ros2_msg_include"]

    inc_h = f"""{_COPYRIGHT}
#pragma once

// Public TypeSupport binding for a loaned publisher. No concrete transport
// implementation type is visible to the generated application module.

#include <chrono>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>

#include "cpp/transport/transport_factory.h"
#include "type_support/ros2/ros2_transport.h"
#include <{ros2_msg_include}>

#define IBMW_TS_LOANED_PUB_MSG_TYPE {msg_type}

#define IBMW_TS_LOANED_PUB_STATIC_ASSERT                                      \\
  static_assert(std::is_trivially_copyable_v<IBMW_TS_LOANED_PUB_MSG_TYPE>,    \\
                "loaned_publisher requires a trivially_copyable "             \\
                "message type ({msg_type})")

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
    if (!publisher_->RegisterLoanedPodTopic(                                  \\
            sizeof(IBMW_TS_LOANED_PUB_MSG_TYPE),                              \\
            alignof(IBMW_TS_LOANED_PUB_MSG_TYPE))) {{                         \\
      MW_ERROR("{name}: configured transport rejected POD loan setup for "    \\
               "topic '{topic}'");                                            \\
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

// Whole PublishLoop while-body: Borrow + strict mode counter +
// populate user-code marker pair + Publish + sleep.
// Single macro is mandatory — see plan §3.a.
#define IBMW_TS_LOANED_PUB_HOT_LOOP_BODY(loop_var)                            \\
  do {{                                                                        \\
    auto loop_var = publisher_->Loan();                                       \\
    if (!loop_var.is_valid()) {{                                               \\
      /* Strict mode: failed Borrow IS the failure mode (no copy */           \\
      /* fallback). Log and back off briefly. */                              \\
      ++fallback_count_;                                                      \\
      MW_ERROR("{name}: LoanedSample::Borrow failed (topic='{topic}')");      \\
      std::this_thread::sleep_for(std::chrono::milliseconds(100));            \\
      break;                                                                  \\
    }}                                                                         \\
    ++zero_copy_count_;                                                       \\
    /* IBMW_PUBSUB_LOANED_PUBLISHER_BODY_BEGIN */                             \\
    /* IBMW: user code begin (populate_loaned_message) */                     \\
    /* TODO: populate `*loop_var` (or `loop_var->field`) here. This */        \\
    /* writes directly into the iceoryx SHM chunk -- true zero-copy. */      \\
    /* The subscriber sees these bytes verbatim. */                           \\
    /* IBMW: user code end (populate_loaned_message) */                       \\
    /* IBMW_PUBSUB_LOANED_PUBLISHER_BODY_END */                               \\
    if ((publish_count_ % 50) == 0) {{                                         \\
      MW_INFO("[SEND] {{}} #{{}} zero_copy=true (iceoryx SHM)",                  \\
              "{topic}", publish_count_);                                     \\
    }}                                                                         \\
    bool _ibmw_pub_ok = publisher_->Publish(std::move(loop_var));             \\
    if (_ibmw_pub_ok) {{                                                       \\
      ++publish_count_;                                                       \\
    }} else {{                                                                  \\
      MW_ERROR("{name}: LoanedSample::Publish failed");                       \\
    }}                                                                         \\
    std::this_thread::sleep_for(std::chrono::milliseconds(100));              \\
  }} while (0)

// PrintStatistics body: 4 lines + strict-mode comment line + separator.
#define IBMW_TS_LOANED_PUB_PRINT_STATS                                        \\
  do {{                                                                        \\
    uint64_t _ibmw_total = zero_copy_count_ + fallback_count_;                \\
    double _ibmw_ratio = _ibmw_total > 0                                      \\
        ? static_cast<double>(zero_copy_count_) / _ibmw_total * 100.0 : 0.0;  \\
    MW_INFO("========== Loaned Publisher (iceoryx) Statistics =========="); \\
    MW_INFO("Total published : {{}}", publish_count_);                          \\
    MW_INFO("Zero-copy       : {{}} ({{:.1f}}%)", zero_copy_count_, _ibmw_ratio); \\
    MW_INFO("Borrow failures : {{}} (strict mode -- no copy fallback)",         \\
            fallback_count_);                                                 \\
    MW_INFO("==========================================================="); \\
  }} while (0)
"""

    inc_cc = f"""{_COPYRIGHT}
// Transport-specific typesupport TU fragment for an iceoryx loaned_publisher.
// Auto-generated by ibmw_gen for the iceoryx transport.
//
// All hot-path mechanics for iceoryx loaned_publisher expand inline
// from the IBMW_TS_LOANED_PUB_* macros in the companion inc.h. This
// TU stub exists to (a) preserve the 4-file unified layout, (b) carry
// the namespace context, and (c) reserve a place for any future
// out-of-line iceoryx-specific transport code.
//
// The user-code marker pair (`populate_loaned_message`) lives inside
// the IBMW_TS_LOANED_PUB_HOT_LOOP_BODY macro definition in inc.h;
// users edit it there.

namespace {ns} {{

// (no out-of-line definitions today -- all hooks expand inline via
// macros in {_snake(name)}_typesupport.inc.h)

}}  // namespace {ns}
"""

    return {
        "msg_type_cpp": msg_type,
        "inc_h_content": inc_h,
        "inc_cc_content": inc_cc,
    }


register(TransportSpec(
    name="iceoryx",
    emit_module=_emit_module,
    emit_loaned_subscriber_typesupport=_emit_loaned_subscriber_typesupport,
    emit_loaned_publisher_typesupport=_emit_loaned_publisher_typesupport,
    meta={
        "stage": "unified-loaned-publisher",
        "owns_type_keys": sorted(_ICEORYX_TYPE_KEYS),
    },
))
