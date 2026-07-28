# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# ROS 2 transport plugin with ownership of its generator function bodies.
#
# This module holds every code-generator function that produces ROS 2
# transport-coupled C++ modules. These functions moved from
# ``generators/module_gen.py`` so that the ros2
# plugin is a self-contained owner of its type-keys (publisher / subscriber /
# rpc_server / rpc_client).

"""ROS 2 transport plugin for ibmw_gen."""

from generators._common import _snake, _COPYRIGHT
from generators.marker_preserve import (
    extract_marker_regions,
    preserve_marker_regions,
)
from . import TransportSpec, register


_ROS2_RPC_SERVER_MARKERS = ["IBMW_RPC_SERVER_HANDLER_BODY"]
_ROS2_RPC_CLIENT_MARKERS = [
    "IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD",
    "IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME",
    "IBMW_RPC_CLIENT_RETURN_STATUS_HANDLING",
]
_ROS2_MARKER_ANCHOR_FORMAT = "// {marker}_{edge}"


def _format_marker_anchor(marker: str, edge: str) -> str:
    return _ROS2_MARKER_ANCHOR_FORMAT.format(marker=marker, edge=edge)


def _ros2_rpc_server_method_marker(method_name: str) -> str:
    return f"{_ROS2_RPC_SERVER_MARKERS[0]}_{_snake(method_name).upper()}"


def _ros2_rpc_client_method_marker(marker: str, method_name: str) -> str:
    return f"{marker}_{_snake(method_name).upper()}"


# ── ROS2 Publisher ───────────────────────────────────────────────────────────
def _gen_ros2_publisher_header(mod: dict, ns: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]  # e.g. "example_ros2::msg::RosTestMsg"

    return f"""{_COPYRIGHT}
//
// ROS2 Pub/Sub demo — Publisher module.

#pragma once

#include <atomic>
#include <optional>

#include "cpp/module_base.h"
#include "type_support/ros2/ros2_transport.h"

#include "{mod.get('ros2_msg_include', '')}"

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
  std::optional<ibmw::transport::SenderAccess<{msg_type}>> access_;
  uint32_t msg_count_ = 0;
  std::atomic<bool> running_{{false}};
}};

}}  // namespace {ns}
"""


def _gen_ros2_publisher_cc(mod: dict, ns: str, snake_dir: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]
    topic = mod["topic"]

    return f"""{_COPYRIGHT}
#include "{snake_dir}/{_snake(name)}.h"

#include <chrono>
#include <thread>

namespace {ns} {{

using {msg_type};

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;

  auto publisher = core_.GetTransport().GetPublisher("{topic}");

  // Construct SenderAccess — bundles publisher + type name.
  access_.emplace(publisher);

  if (!access_->RegisterSendType()) {{
    MW_ERROR("Register ROS2 publish type failed!");
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
    auto msg = {msg_type.rsplit('::', 1)[-1] if '::' in msg_type else msg_type}();
    // TODO: populate msg fields here

    // One-line typed send via SenderAccess
    access_->Send(msg);

    MW_INFO("Published ROS2 message #{{}}", msg_count_);

    ++msg_count_;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }}
}}

}}  // namespace {ns}
"""


def _gen_ros2_subscriber_header(mod: dict, ns: str) -> str:
    name = mod["name"]

    return f"""{_COPYRIGHT}
//
// ROS2 Pub/Sub demo — Subscriber module.

#pragma once

#include "cpp/module_base.h"
#include "type_support/ros2/ros2_transport.h"

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


def _gen_ros2_subscriber_cc(mod: dict, ns: str, snake_dir: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]
    topic = mod["topic"]
    # Short type name for local using
    short_type = msg_type.rsplit("::", 1)[-1] if "::" in msg_type else msg_type

    return f"""{_COPYRIGHT}
#include "{snake_dir}/{_snake(name)}.h"

#include "{mod.get('ros2_msg_include', '')}"

namespace {ns} {{

using {msg_type};

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;

  auto subscriber = core_.GetTransport().GetSubscriber("{topic}");

  // ReceiverAccess<T> wraps subscribe + type dispatch in one object.
  ibmw::transport::ReceiverAccess<{short_type}> access(subscriber);

  bool ret = access.OnMessage(
      [this](const std::shared_ptr<const {short_type}>& msg) {{
        MW_INFO("Received ROS2 message.");
        // TODO: process msg fields here
      }});

  if (!ret) {{
    MW_ERROR("Subscribe to ROS2 type failed!");
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


_ROS2_GENERATORS = {
    "publisher": (_gen_ros2_publisher_header, _gen_ros2_publisher_cc),
    "subscriber": (_gen_ros2_subscriber_header, _gen_ros2_subscriber_cc),
    # rpc_server/rpc_client rows are handled by the unified RPC path
    # in module_gen._gen_rpc_{server,client}_unified handles all RPC emission
    # via _gen_*_rpc_*_markerized_typesupport sidecars.
}


def _emit_module(mod: dict, ns: str, mod_dir):
    """Emit the .h / .cc pair for a ROS 2 transport module.

    Returns ``list[(rel_path, content)]`` or ``None`` if this plugin does
    not handle the given ``(mod['type'], "ros2")`` pair. Called by
    ``generate_modules()`` via the plugin registry. The skeleton fallback
    (when a ros2 rpc module has no ros2_service configured) is handled
    inside the main rpc generators above.

    `publisher` is handled by the unified path
    in module_gen._gen_publisher_unified via
    emit_publisher_typesupport below; return None here so the upper
    dispatcher picks the unified route.

    `subscriber` is handled similarly via
    emit_subscriber_typesupport.
    """
    mtype = mod.get("type")
    if mtype in ("publisher", "subscriber"):
        return None
    gens = _ROS2_GENERATORS.get(mtype)
    if gens is None:
        return None
    header_fn, cc_fn = gens
    snake = _snake(mod["name"])
    hdr = header_fn(mod, ns)
    src = cc_fn(mod, ns, mod_dir)
    return [
        (f"src/{mod_dir}/{snake}.h", hdr),
        (f"src/{mod_dir}/{snake}.cc", src),
    ]


# ── Unified publisher typesupport emission ─────────────────────────────────
def _emit_publisher_typesupport(mod: dict, ns: str):
    """Emit transport-specific publisher typesupport for ros2.

    Byte-semantic equivalent of the legacy _gen_ros2_publisher_{header,cc}
    path: same includes (ros2_types transport bridge + ros2 msg header),
    same SenderAccess<MsgType> binding, same Send() hot path.
    """
    msg_type = mod["message_type"]  # e.g. "example_ros2::msg::RosTestMsg"
    topic = mod["topic"]
    msg_include = mod.get("ros2_msg_include", "")

    inc_h = f"""{_COPYRIGHT}
#pragma once

// Transport-specific typesupport for a ros2 publisher module.
// Auto-generated by ibmw_gen for the ROS 2 transport.

#include <optional>

#include "type_support/ros2/ros2_transport.h"
#include "{msg_include}"

#define IBMW_TS_SENDER_ACCESS_MEMBER \\
  std::optional<ibmw::transport::SenderAccess<{msg_type}>>

#define IBMW_TS_SENDER_ACCESS_INIT(access_expr, core_expr)        \\
  do {{                                                            \\
    auto _ibmw_pub = (core_expr).GetTransport().GetPublisher(     \\
        "{topic}");                                                \\
    (access_expr).emplace(_ibmw_pub);                             \\
  }} while (0)
"""

    inc_cc = f"""{_COPYRIGHT}
// Transport-specific typesupport TU fragment for a ros2 publisher.
// Auto-generated by ibmw_gen for the ROS 2 transport. Intentionally empty
// — all required symbols live in the inc.h header for ros2 today.
"""

    return {
        "msg_type_cpp": msg_type,
        "inc_h_content": inc_h,
        "inc_cc_content": inc_cc,
        "register_call": "access_->RegisterSendType()",
        "publish_stmt": "access_->Send(msg);",
    }


# ── Unified subscriber typesupport emission ────────────────────────────────
def _emit_subscriber_typesupport(mod: dict, ns: str):
    """Emit transport-specific subscriber typesupport for ros2.

    Byte-semantic equivalent of the legacy _gen_ros2_subscriber_{header,cc}
    path: same includes (ros2_types transport bridge + ros2 msg header),
    same ReceiverAccess<MsgType> stack-local, same OnMessage wiring.
    """
    msg_type = mod["message_type"]  # e.g. "example_ros2::msg::RosTestMsg"
    topic = mod["topic"]
    msg_include = mod.get("ros2_msg_include", "")

    inc_h = f"""{_COPYRIGHT}
#pragma once

// Transport-specific typesupport for a ros2 subscriber module.
// Auto-generated by ibmw_gen for the ROS 2 transport.

#include "type_support/ros2/ros2_transport.h"
#include "{msg_include}"

#define IBMW_TS_RECEIVER_MSG_TYPE {msg_type}

#define IBMW_TS_RECEIVER_ACCESS_SETUP(core_expr, access_var)        \\
  auto _ibmw_sub = (core_expr).GetTransport().GetSubscriber(        \\
      "{topic}");                                                   \\
  ibmw::transport::ReceiverAccess<{msg_type}> access_var(_ibmw_sub)
"""

    inc_cc = f"""{_COPYRIGHT}
// Transport-specific typesupport TU fragment for a ros2 subscriber.
// Auto-generated by ibmw_gen for the ROS 2 transport. Intentionally empty
// — all required symbols live in the inc.h header for ros2 today.
"""

    return {
        "msg_type_cpp": msg_type,
        "inc_h_content": inc_h,
        "inc_cc_content": inc_cc,
    }


def _gen_ros2_rpc_server_markerized_typesupport(
    mod: dict,
    ns: str,
    previous_inc_cc: str | None = None,
):
    if not mod.get("_has_ros2_service_info") or not mod.get("ros2_service"):
        return None

    name = mod["name"]
    pkg = mod["ros2_package"]
    svc = mod["ros2_service"]
    # Opt-in path for artifact-imported .srv adapters. When enabled, the
    # sidecar inherits the import-generated *SyncServiceAdapter from
    # <pkg>/ibmw_rpc_bridge_runtime.hpp. When absent/false, keep the legacy
    # ros2_gen_rpc "<Service>.ibmw_rpc.srv.h" path unchanged.
    use_imported_srv_adapter = bool(mod.get("ros2_imported_srv_adapter"))
    imported_ns = f"ibmw::ros2_interfaces::{pkg}"
    service_base_cpp = (
        f"{imported_ns}::{svc}SyncServiceAdapter"
        if use_imported_srv_adapter
        else f"{pkg}::srv::{svc}SyncService"
    )
    request_cpp = (
        f"{imported_ns}::{svc}RpcBridgeRuntime::Request"
        if use_imported_srv_adapter
        else f"{pkg}::srv::{svc}_Request"
    )
    response_cpp = (
        f"{imported_ns}::{svc}RpcBridgeRuntime::Response"
        if use_imported_srv_adapter
        else f"{pkg}::srv::{svc}_Response"
    )
    service_include = (
        f"<{pkg}/ibmw_rpc_bridge_runtime.hpp>"
        if use_imported_srv_adapter
        else f'"{svc}.ibmw_rpc.srv.h"'
    )
    override_name = f"Handle{svc}" if use_imported_srv_adapter else svc
    helper_name = f"_IbmwRos2RpcServer{name}{svc}SyncServiceImpl"
    handler_name = f"Handle{svc}"
    handler_marker = _ros2_rpc_server_method_marker(svc)
    handler_begin = _format_marker_anchor(handler_marker, "BEGIN")
    handler_end = _format_marker_anchor(handler_marker, "END")
    marshalling_helper_name = (
        f"_ibmw_ros2_rpc_server_marshalling_helper_{_snake(svc)}"
    )
    if use_imported_srv_adapter:
        default_handler_body = """  // Keep the generated default field-agnostic so imported .srv adapters work
  // for primitive, string, nested, array, and sequence service shapes.
  return ibmw::rpc::Status(IBMW_RPC_STATUS_SVR_NOT_IMPLEMENTED);"""
    else:
        default_handler_body = """  // Example: echo back request data
  rsp.code = 0;
  return ibmw::rpc::Status();"""

    inc_h = f"""{_COPYRIGHT}
#pragma once

#include <memory>

#include {service_include}

namespace {ns} {{

class {name};

class {helper_name} : public {service_base_cpp} {{
 public:
  explicit {helper_name}({name}* owner) : owner_(owner) {{}}
  ~{helper_name}() override = default;

  ibmw::rpc::Status {override_name}(
      ibmw::rpc::ContextRef ctx,
      const {request_cpp}& req,
      {response_cpp}& rsp) override;

 private:
  {name}* owner_;
}};

}}  // namespace {ns}

#define IBMW_TS_RPC_SERVER_SETUP(core_expr)                                   \\
  do {{                                                                       \\
    core_ = (core_expr);                                                      \\
    try {{                                                                    \\
      service_ptr_ = std::make_shared<{helper_name}>(this);                   \\
      bool ret = core_.GetService().RegisterService(service_ptr_.get());      \\
      MW_REQUIRE(ret, "Register ROS2 RPC service failed.");                  \\
      MW_INFO("Register service succeeded.");                                \\
    }} catch (const std::exception& e) {{                                     \\
      MW_ERROR("Setup failed: {{}}", e.what());                              \\
      return false;                                                           \\
    }}                                                                        \\
  }} while (0)

#define IBMW_TS_RPC_SERVER_HANDLER_DECL                                        \\
  ibmw::rpc::Status {handler_name}(                                           \\
      ibmw::rpc::ContextRef ctx,                                              \\
      const {request_cpp}& req,                                               \\
      {response_cpp}& rsp);                                                   \\
  std::shared_ptr<{helper_name}> service_ptr_;

#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(name) /* defined in inc.cc */
"""

    method_markers = [handler_marker]
    template_inc_cc = f"""{_COPYRIGHT}
namespace {ns} {{

template <typename ServerT>
inline ibmw::rpc::Status {marshalling_helper_name}(
    ServerT* self,
    ibmw::rpc::ContextRef ctx,
    const {request_cpp}& req,
    {response_cpp}& rsp) {{
  (void)self;
  (void)ctx;
  (void)req;
  (void)rsp;
  {handler_begin}
  // TODO: implement your service logic here
{default_handler_body}
  {handler_end}
}}

ibmw::rpc::Status {helper_name}::{override_name}(
    ibmw::rpc::ContextRef ctx,
    const {request_cpp}& req,
    {response_cpp}& rsp) {{
  return owner_->{handler_name}(ctx, req, rsp);
}}

}}  // namespace {ns}

#undef IBMW_TS_RPC_SERVER_HANDLER_IMPL

#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(name)                                  \\
  ibmw::rpc::Status name::{handler_name}(                                     \\
      ibmw::rpc::ContextRef ctx,                                              \\
      const {request_cpp}& req,                                               \\
      {response_cpp}& rsp) {{                                                 \\
    return {marshalling_helper_name}(this, ctx, req, rsp);                    \\
  }}
"""
    if previous_inc_cc and method_markers:
        preserved = extract_marker_regions(previous_inc_cc, method_markers)
        inc_cc = preserve_marker_regions(template_inc_cc, preserved)
    else:
        inc_cc = template_inc_cc

    return {
        "msg_type_cpp": service_base_cpp,
        "inc_h_content": inc_h,
        "inc_cc_content": inc_cc,
    }


def emit_rpc_server_typesupport(mod: dict, ns: str):
    return _gen_ros2_rpc_server_markerized_typesupport(mod, ns)


def _gen_ros2_rpc_client_markerized_typesupport(
    mod: dict,
    ns: str,
    previous_inc_cc: str | None = None,
):
    if not mod.get("_has_ros2_service_info") or not mod.get("ros2_service"):
        return None

    pkg = mod["ros2_package"]
    svc = mod["ros2_service"]
    use_imported_srv_adapter = bool(mod.get("ros2_imported_srv_adapter"))
    imported_ns = f"ibmw::ros2_interfaces::{pkg}"
    request_cpp = (
        f"{imported_ns}::{svc}RpcBridgeRuntime::Request"
        if use_imported_srv_adapter
        else f"{pkg}::srv::{svc}_Request"
    )
    response_cpp = (
        f"{imported_ns}::{svc}RpcBridgeRuntime::Response"
        if use_imported_srv_adapter
        else f"{pkg}::srv::{svc}_Response"
    )
    access_cpp = (
        f"{imported_ns}::{svc}SyncClientAdapter"
        if use_imported_srv_adapter
        else f"{pkg}::srv::{svc}SyncAccess"
    )
    service_include = (
        f"<{pkg}/ibmw_rpc_bridge_runtime.hpp>"
        if use_imported_srv_adapter
        else f'"{svc}.ibmw_rpc.srv.h"'
    )
    request_marker = _ros2_rpc_client_method_marker(
        _ROS2_RPC_CLIENT_MARKERS[0], svc)
    response_marker = _ros2_rpc_client_method_marker(
        _ROS2_RPC_CLIENT_MARKERS[1], svc)
    status_marker = _ros2_rpc_client_method_marker(
        _ROS2_RPC_CLIENT_MARKERS[2], svc)
    method_markers = [request_marker, response_marker, status_marker]
    request_begin = _format_marker_anchor(request_marker, "BEGIN")
    request_end = _format_marker_anchor(request_marker, "END")
    response_begin = _format_marker_anchor(response_marker, "BEGIN")
    response_end = _format_marker_anchor(response_marker, "END")
    status_begin = _format_marker_anchor(status_marker, "BEGIN")
    status_end = _format_marker_anchor(status_marker, "END")

    helper_block = f"""template <typename SelfT>
inline void _ibmw_ros2_rpc_client_loop_body_impl(SelfT* self) {{
  MW_INFO("Start RpcLoop.");

  uint32_t count = 0;
  while (self->run_flag_) {{
    std::this_thread::sleep_for(
        std::chrono::milliseconds(static_cast<uint32_t>(1000 / self->rpc_frq_)));
    count++;

    MW_INFO("Loop count : {{}} -------------------------", count);

    {request_cpp} req;
    {response_cpp} rsp;

    {request_begin}
    // TODO: populate req fields here
    {request_end}

    auto ctx_ptr = self->access_->NewContextSharedPtr();
    ctx_ptr->SetTimeout(std::chrono::seconds(3));

    MW_INFO("start new service call. req:\\n{{}}", {pkg}::srv::to_yaml(req));

    auto status = self->access_->{svc}(ctx_ptr, req, rsp);

    if (status.OK()) {{
      {response_begin}
      MW_INFO("Client get rpc ret, status: {{}}, rsp: {{}}",
              status.ToString(), {pkg}::srv::to_yaml(rsp));
      {response_end}
    }} else {{
      {status_begin}
      MW_ERROR("Client get rpc error ret, status: {{}}",
               status.ToString());
      {status_end}
    }}
  }}

  MW_INFO("Exit RpcLoop.");
}}"""

    inc_h = f"""{_COPYRIGHT}
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include {service_include}

// Thread-form client loop uses std::this_thread::sleep_for in the
// transport-owned helper emitted in the companion inc.cc.

#define IBMW_TS_RPC_CLIENT_LOOP_DECL                                           \\
  void RpcLoop();                                                              \\
                                                                                  \\
 private:                                                                      \\
  template <typename SelfT>                                                    \\
  friend void _ibmw_ros2_rpc_client_loop_body_impl(SelfT*);                    \\
  auto GetLogger() {{ return core_.GetLogger(); }}                             \\
  std::shared_ptr<{access_cpp}> access_;                                        \\
  std::thread loop_thread_;                                                    \\
  std::atomic_bool run_flag_{{true}};                                          \\
  double rpc_frq_{{1.0}};

#define IBMW_TS_RPC_CLIENT_LOOP_IMPL(name)                                     \\
  void name::RpcLoop() {{                                                      \\
    _ibmw_ros2_rpc_client_loop_body_impl(this);                                \\
  }}
"""

    template_inc_cc = f"""{_COPYRIGHT}
// Transport-specific typesupport TU fragment for a ros2 rpc_client.
// Auto-generated by ibmw_gen. The thread-form RpcLoop helper
// and IBMW_TS_RPC_CLIENT_LOOP_* macro pair live in the companion inc.h so the
// ROS2 path stays distinct from the DDS coroutine form.

namespace {ns} {{

{helper_block}

}}  // namespace {ns}
"""
    if previous_inc_cc and method_markers:
        preserved = extract_marker_regions(previous_inc_cc, method_markers)
        inc_cc = preserve_marker_regions(template_inc_cc, preserved)
    else:
        inc_cc = template_inc_cc

    return {
        "msg_type_cpp": access_cpp,
        "inc_h_content": inc_h,
        "inc_cc_content": inc_cc,
    }


def emit_rpc_client_typesupport(mod: dict, ns: str):
    return _gen_ros2_rpc_client_markerized_typesupport(mod, ns)


register(TransportSpec(
    name="ros2",
    emit_module=_emit_module,
    emit_publisher_typesupport=_emit_publisher_typesupport,
    emit_subscriber_typesupport=_emit_subscriber_typesupport,
    emit_rpc_server_typesupport=emit_rpc_server_typesupport,
    emit_rpc_client_typesupport=emit_rpc_client_typesupport,
    rpc_server_markers=list(_ROS2_RPC_SERVER_MARKERS),
    rpc_client_markers=list(_ROS2_RPC_CLIENT_MARKERS),
    marker_anchor_format=_ROS2_MARKER_ANCHOR_FORMAT,
    meta={"owns_type_keys": ["publisher", "subscriber", "rpc_server", "rpc_client"]},
))
