#!/usr/bin/env python3
# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# shm transport plugin: the self-developed zero-copy shared-memory transport
# (src/extensions/transport/shm_ext, no RouDi daemon). Mirrors
# transports/iceoryx.py module type for module type:
#
#   direct_shm_publisher / direct_shm_subscriber
#       R10 escape hatches emitted by emit_module: publisher_for<T>() /
#       subscriber_for<T>() over the shm driver, with the driver header
#       included inside an IBMW_R1_EXEMPT anchor.
#   loaned_publisher / loaned_subscriber (transport: shm)
#       Unified 4-file layout through emit_loaned_{publisher,subscriber}_
#       typesupport; the user-facing <module>.{h,cc} stay transport-agnostic.
#
# The shm driver keeps the iceoryx URLs, wire format and loan contract, so
# the generated code is the iceoryx code with the shm names.

"""shm transport plugin for ibmw_gen — owns all shm code emission."""

from generators._common import _snake, _COPYRIGHT
from . import TransportSpec, register


_SHM_TYPE_KEYS = frozenset({
    "direct_shm_publisher",
    "direct_shm_subscriber",
    "loaned_publisher",
    "loaned_subscriber",
})


def _gen_direct_shm_publisher_header(mod: dict, ns: str) -> str:
    name = mod["name"]
    ros2_msg_include = mod["ros2_msg_include"]

    return f"""{_COPYRIGHT}
//
// IBMW: NOT TRANSPORT-DECOUPLED — direct shm API.
// R10 escape-hatch template (direct_shm_publisher):
// generated code uses ibmw::transport::publisher_for<T>() directly for
// the shm path. User code is transport-coupled by design — to
// preserve transport-decoupling, prefer the agnostic `publisher` /
// `loaned_publisher` templates when they satisfy the use case.

#pragma once

#include <atomic>
#include <thread>

#include "cpp/module_base.h"
#include "cpp/transport/transport_factory.h"
#include "type_support/ros2/ros2_transport.h"
// IBMW_R1_EXEMPT_BEGIN
#include "shm_ext/shm_transport_driver.h"
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


def _gen_direct_shm_publisher_cc(mod: dict, ns: str, snake_dir: str) -> str:
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
    MW_ERROR("Failed to register direct shm publisher type on topic: {topic}");
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

    MW_INFO("Published direct shm message #{{}}", publish_count_);

    std::this_thread::sleep_for(std::chrono::seconds(1));
  }}
}}

}}  // namespace {ns}
"""


def _gen_direct_shm_subscriber_header(mod: dict, ns: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]
    ros2_msg_include = mod["ros2_msg_include"]

    return f"""{_COPYRIGHT}
//
// IBMW: NOT TRANSPORT-DECOUPLED — direct shm API.
// R10 escape-hatch template (direct_shm_subscriber):
// generated code uses ibmw::transport::subscriber_for<T>() directly for
// the shm path. User code is transport-coupled by design — to
// preserve transport-decoupling, prefer the agnostic `subscriber` /
// `loaned_subscriber` templates when they satisfy the use case.

#pragma once

#include "cpp/module_base.h"
#include "cpp/transport/transport_factory.h"
#include "type_support/ros2/ros2_transport.h"
// IBMW_R1_EXEMPT_BEGIN
#include "shm_ext/shm_transport_driver.h"
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


def _gen_direct_shm_subscriber_cc(mod: dict, ns: str, snake_dir: str) -> str:
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
    MW_ERROR("Failed to subscribe direct shm topic: {topic}");
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

  MW_INFO("Received direct shm message [{{}}]", receive_count_);
  // TODO: process msg fields here

  auto serialization_type = ctx.GetSerializationType();
  MW_DEBUG("Serialization type: {{}}", serialization_type);
}}

}}  // namespace {ns}
"""


# Map (mod['type'], "shm") -> (header_emitter, cc_emitter).
_SHM_GENERATORS = {
    ("direct_shm_publisher", "shm"): (
        _gen_direct_shm_publisher_header, _gen_direct_shm_publisher_cc),
    ("direct_shm_subscriber", "shm"): (
        _gen_direct_shm_subscriber_header, _gen_direct_shm_subscriber_cc),
}


def _emit_module(mod: dict, ns: str, mod_dir: str):
    """Emit the .h / .cc pair for a direct_shm_* module.

    Returns list[(rel_path, content)], or None for any other type:
    loaned_publisher / loaned_subscriber go through the unified path in
    module_gen via the typesupport hooks below.
    """
    pair = _SHM_GENERATORS.get((mod.get("type"), "shm"))
    if pair is None:
        return None

    gen_h, gen_cc = pair
    snake = _snake(mod["name"])
    return [
        (f"src/{mod_dir}/{snake}.h", gen_h(mod, ns)),
        (f"src/{mod_dir}/{snake}.cc", gen_cc(mod, ns, mod_dir)),
    ]


# ── Unified loaned-subscriber typesupport emission ─────────────────────────
def _emit_loaned_subscriber_typesupport(mod: dict, ns: str):
    """Emit the loaned_subscriber typesupport for shm.

    LoanedSubscriber<T>::AttachBytes() keyed on "ros2:" + GetRos2TypeName<T>(),
    the same key the loaned publisher registers. Signature of the user
    callback: virtual void OnMessage(const T& msg, std::size_t size).
    """
    name = mod["name"]
    style = mod.get("subscriber_style", "callback")
    if style != "callback":
        raise NotImplementedError(
            f"Module '{name}' (loaned_subscriber,shm): subscriber_style "
            f"'{style}' is reserved but not yet implemented; only "
            f"'callback' is currently supported")

    msg_type = mod["message_type"]
    topic = mod["topic"]
    ros2_msg_include = mod["ros2_msg_include"]
    shm_msg_type = mod.get("shm_msg_type", msg_type)  # log-only value

    inc_h = f"""{_COPYRIGHT}
#pragma once

// Transport-specific typesupport for a shm loaned_subscriber module.
// Auto-generated by ibmw_gen for the shm transport.
//
// Receives via ibmw::transport::LoanedSubscriber<T>::AttachBytes() — the
// framework-level typed zero-copy fast lane. No driver injection
// required (the dispatcher routes AttachLoaned to the shm driver, which
// owns the listener thread + chunk lifetime).
//
// User-facing API: override OnMessage(const T& msg, size_t size) in
// the user-facing <module>.h class. Default impl below logs every
// 5th message; msg is a const-ref into the shm chunk — do NOT retain
// past return.

#include <cstddef>
#include <type_traits>

#include "cpp/transport/loaned_subscriber.h"
#include "type_support/ros2/ros2_transport.h"
#include <{ros2_msg_include}>

// Convenience alias for use by the ATTACH macro and the default
// OnMessage body in inc.cc.
#define IBMW_TS_LOANED_SUB_MSG_TYPE {msg_type}

// shm: compile-time POD enforcement. The chunk is reinterpret_cast to
// const T&; non-trivially-copyable T would invoke ctors/dtors across
// process boundaries and is rejected early by the compiler.
#define IBMW_TS_LOANED_SUB_STATIC_ASSERT                                      \\
  static_assert(std::is_trivially_copyable_v<IBMW_TS_LOANED_SUB_MSG_TYPE>,    \\
                "shm loaned_subscriber requires a trivially_copyable "        \\
                "message type ({msg_type})")

// Virtual user callback decl: (const T&, std::size_t size). The reference
// points into the shm chunk and dies when the callback returns.
#define IBMW_TS_LOANED_SUB_ON_MESSAGE_DECL                                    \\
  virtual void OnMessage(const IBMW_TS_LOANED_SUB_MSG_TYPE& msg,              \\
                         std::size_t size)

// Setup body: GetSubscriber + LoanedSubscriber<T>::AttachBytes. The
// attach-key MUST exactly match what the publisher side registers through
// the public POD loan capability, since the shm driver keys its POD topics
// on (topic, msg_type). The yaml field `shm_msg_type` is log-only.
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
               "topic='{topic}', shm_msg_type='{shm_msg_type}'");             \\
      return false;                                                           \\
    }}                                                                         \\
  }} while (0)
"""

    inc_cc = f"""{_COPYRIGHT}
// Transport-specific typesupport TU fragment for a shm loaned_subscriber.
// Auto-generated by ibmw_gen for the shm transport.
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
  // shm chunk -- true zero-copy. Do NOT retain `msg` past
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
# Same stable SenderAccess<T>/LoanedSample<T> surface as the dds and iceoryx
# declarations; only the log text names the transport.
#
def _emit_loaned_publisher_typesupport(mod: dict, ns: str):
    """Bind a shm-declared loaned publisher to the public loan API."""
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
    /* writes directly into the shm chunk -- true zero-copy. */              \\
    /* The subscriber sees these bytes verbatim. */                           \\
    /* IBMW: user code end (populate_loaned_message) */                       \\
    /* IBMW_PUBSUB_LOANED_PUBLISHER_BODY_END */                               \\
    if ((publish_count_ % 50) == 0) {{                                         \\
      MW_INFO("[SEND] {{}} #{{}} zero_copy=true (shm)",                          \\
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
    MW_INFO("============ Loaned Publisher (shm) Statistics ============"); \\
    MW_INFO("Total published : {{}}", publish_count_);                          \\
    MW_INFO("Zero-copy       : {{}} ({{:.1f}}%)", zero_copy_count_, _ibmw_ratio); \\
    MW_INFO("Borrow failures : {{}} (strict mode -- no copy fallback)",         \\
            fallback_count_);                                                 \\
    MW_INFO("==========================================================="); \\
  }} while (0)
"""

    inc_cc = f"""{_COPYRIGHT}
// Transport-specific typesupport TU fragment for a shm loaned_publisher.
// Auto-generated by ibmw_gen for the shm transport.
//
// All hot-path mechanics expand inline from the IBMW_TS_LOANED_PUB_*
// macros in the companion inc.h. This TU stub keeps the 4-file unified
// layout and carries the namespace context.
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
    name="shm",
    emit_module=_emit_module,
    emit_loaned_subscriber_typesupport=_emit_loaned_subscriber_typesupport,
    emit_loaned_publisher_typesupport=_emit_loaned_publisher_typesupport,
    meta={
        "stage": "unified-loaned-publisher",
        "owns_type_keys": sorted(_SHM_TYPE_KEYS),
    },
))
