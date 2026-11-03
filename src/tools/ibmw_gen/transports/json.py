# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# JSON transport plugin with ownership of its generator function bodies.
#
# JSON is the framework-internal serialization format used by the debug
# logger and by some sample modules. It is registered as a transport
# for schema/validator parity with dds/iceoryx/ros2 even though it is
# not a wire transport per se. The code-generator functions moved from
# ``generators/module_gen.py``
# here so that the json plugin is a self-contained owner of its
# type-keys (publisher / subscriber / rpc_server / rpc_client).

"""JSON (serialization) transport plugin for ibmw_gen."""

from importlib import import_module
from typing import Any, Callable

_common = import_module("generators._common")
_COPYRIGHT = str(getattr(_common, "_COPYRIGHT"))


def _snake(name: str) -> str:
    return str(getattr(_common, "_snake")(name))


_marker_preserve = import_module("generators.marker_preserve")
extract_marker_regions = getattr(_marker_preserve, "extract_marker_regions")
preserve_marker_regions = getattr(_marker_preserve, "preserve_marker_regions")
from . import TransportSpec, register


_JSON_RPC_SERVER_MARKERS = ["IBMW_RPC_SERVER_HANDLER_BODY"]
_JSON_RPC_CLIENT_MARKERS = [
    "IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD",
    "IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME",
    "IBMW_RPC_CLIENT_RETURN_STATUS_HANDLING",
]
_JSON_MARKER_ANCHOR_FORMAT = "// {marker}_{edge}"


def _format_marker_anchor(marker: str, edge: str) -> str:
    return _JSON_MARKER_ANCHOR_FORMAT.format(marker=marker, edge=edge)


def _json_types_include(mod: dict[str, Any]) -> str:
    return str(mod.get("ros2_import_json_types_header") or "json_custom_types.h")


def _json_rpc_server_method_marker(method_name: str) -> str:
    return f"{_JSON_RPC_SERVER_MARKERS[0]}_{_snake(method_name).upper()}"


def _json_rpc_client_method_marker(marker: str, method_name: str) -> str:
    return f"{marker}_{_snake(method_name).upper()}"


# ── JSON Publisher ───────────────────────────────────────────────────────────
def _gen_json_publisher_header(mod: dict[str, Any], ns: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]

    return f"""{_COPYRIGHT}
//
// JSON Pub/Sub demo — Publisher module.

#pragma once

#include <atomic>
#include <optional>

#include "cpp/module_base.h"
#include "cpp/util/json_transport.h"
#include "json_custom_types.h"

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


def _gen_json_publisher_cc(mod: dict[str, Any], ns: str, snake_dir: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]
    topic = mod["topic"]

    return f"""{_COPYRIGHT}
#include "{snake_dir}/{_snake(name)}.h"

#include <chrono>
#include <thread>

namespace {ns} {{

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;

  auto publisher = core_.GetTransport().GetPublisher("{topic}");

  // Construct SenderAccess — bundles publisher + type name.
  access_.emplace(publisher);

  if (!access_->RegisterSendType()) {{
    MW_ERROR("Register JSON publish type failed!");
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

    // One-line typed send via SenderAccess
    access_->Send(msg);

    MW_INFO("Published JSON message #{{}}", msg_count_);

    ++msg_count_;
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }}
}}

}}  // namespace {ns}
"""



# ── JSON Subscriber ──────────────────────────────────────────────────────────

def _gen_json_subscriber_header(mod: dict[str, Any], ns: str) -> str:
    name = mod["name"]

    return f"""{_COPYRIGHT}
//
// JSON Pub/Sub demo — Subscriber module.

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
  auto GetLogger() {{ return core_.GetLogger(); }}

  ibmw::Runtime core_;
}};

}}  // namespace {ns}
"""


def _gen_json_subscriber_cc(mod: dict[str, Any], ns: str, snake_dir: str) -> str:
    name = mod["name"]
    msg_type = mod["message_type"]
    topic = mod["topic"]

    return f"""{_COPYRIGHT}
#include "{snake_dir}/{_snake(name)}.h"

#include "cpp/util/json_transport.h"
#include "json_custom_types.h"

namespace {ns} {{

bool {name}::Setup(ibmw::Runtime core) {{
  core_ = core;

  auto subscriber = core_.GetTransport().GetSubscriber("{topic}");

  // ReceiverAccess<T> wraps subscribe + type dispatch in one object.
  ibmw::transport::ReceiverAccess<{msg_type}> access(subscriber);

  bool ret = access.OnMessage(
      [this](const std::shared_ptr<const {msg_type}>& msg) {{
        MW_INFO("Received JSON message.");
        // TODO: process msg fields here
      }});

  if (!ret) {{
    MW_ERROR("Subscribe to JSON type failed!");
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



# ── JSON RPC Server ──────────────────────────────────────────────────────────

def _gen_json_rpc_server_markerized_typesupport(
    mod: dict[str, Any],
    ns: str,
    previous_inc_cc: str | None = None,
) -> dict[str, str]:
    name = mod["name"]
    service_name = mod["service_name"]
    methods = mod.get("methods", [])
    helper_name = f"_IbmwJsonRpcServer{name}{service_name}Impl"
    types_include = _json_types_include(mod)

    registration_lines: list[str] = []
    decl_lines: list[str] = []
    impl_lines: list[str] = []
    msg_types: list[str] = []
    method_markers: list[str] = []

    for m in methods:
        req_type = m["request"]
        rsp_type = m["response"]
        method_name = m["name"]
        method_marker = _json_rpc_server_method_marker(method_name)
        method_markers.append(method_marker)

        msg_types.extend([req_type, rsp_type])
        registration_lines.extend([
            f"    ibmw::json::rpc::RegisterServiceFunc<{req_type}, {rsp_type}>(",
            f"        *this, \"{method_name}\",",
            f"        [this](const {req_type}& req, {rsp_type}& rsp) -> uint32_t {{",
            f"          return owner_->Handle{method_name}(req, rsp);",
            "        });",
            "",
        ])
        decl_lines.append(
            f"  uint32_t Handle{method_name}(const {req_type}& req, {rsp_type}& rsp);")
        impl_lines.extend([
            f"uint32_t name::Handle{method_name}(const {req_type}& req, {rsp_type}& rsp) {{",
            "  (void)req;",
            "  (void)rsp;",
            f"  {_format_marker_anchor(method_marker, 'BEGIN')}",
            "  // TODO: implement handler logic, then remove this default return.",
            "  return IBMW_RPC_STATUS_SVR_NOT_IMPLEMENTED;",
            f"  {_format_marker_anchor(method_marker, 'END')}",
            "  return 0;",
            "}",
            "",
        ])

    def _macro_body(lines: list[str]) -> str:
        while lines and not lines[-1]:
            lines = lines[:-1]
        if not lines:
            return "  /* no handlers */"
        macro_lines: list[str] = []
        last_index = len(lines) - 1
        for index, line in enumerate(lines):
            if index == last_index:
                macro_lines.append(line)
            elif line:
                macro_lines.append(f"{line} \\")
            else:
                macro_lines.append("\\")
        return "\n".join(macro_lines)

    registration_body = "\n".join(registration_lines).rstrip()
    handler_decl = _macro_body([
        *decl_lines,
        "  std::unique_ptr<ibmw::rpc::ServiceBase> service_;",
    ])
    handler_impl = _macro_body(impl_lines)

    inc_h = f"""{_COPYRIGHT}
#pragma once

#include <memory>

#include "cpp/service/rpc_handle.h"
#include "cpp/util/json_rpc_helpers.h"
#include "{types_include}"

namespace {ns} {{

class {name};

class {helper_name} : public ibmw::rpc::ServiceBase {{
 public:
  explicit {helper_name}({name}* owner);

  ~{helper_name}() override = default;

 private:
  {name}* owner_;
}};

}}  // namespace {ns}

#define IBMW_TS_RPC_SERVER_SETUP(core_expr)                                   \
  do {{                                                                       \
    core_ = (core_expr);                                                      \
    service_ = std::make_unique<{helper_name}>(this);                         \
    auto rpc_handle = core_.GetService();                                     \
    bool ret = rpc_handle.RegisterService(service_.get());                    \
    if (!ret) {{                                                              \
      MW_ERROR("Failed to register {service_name}!");                        \
      return false;                                                           \
    }}                                                                        \
    MW_INFO("{name}::Setup succeeded — {service_name} registered.");         \
  }} while (0)

#define IBMW_TS_RPC_SERVER_HANDLER_DECL                                        \
{handler_decl}

#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(name) /* defined in inc.cc */
"""

    handler_impl_define = "#define IBMW_TS_RPC_SERVER_HANDLER_IMPL(name) " + chr(92)
    template_inc_cc = f"""{_COPYRIGHT}
#undef IBMW_TS_RPC_SERVER_HANDLER_IMPL

namespace {ns} {{

{helper_name}::{helper_name}({name}* owner)
    : ibmw::rpc::ServiceBase("json", "{ns}.{service_name}"), owner_(owner) {{
{registration_body}
}}

}}  // namespace {ns}

{handler_impl_define}
{handler_impl}
"""
    if previous_inc_cc and method_markers:
        preserved = extract_marker_regions(previous_inc_cc, method_markers)
        inc_cc = preserve_marker_regions(template_inc_cc, preserved)
    else:
        inc_cc = template_inc_cc

    return {
        "msg_type_cpp": ", ".join(dict.fromkeys(msg_types)) or "ibmw::rpc::ServiceBase",
        "inc_h_content": inc_h,
        "inc_cc_content": inc_cc,
    }


def emit_rpc_server_typesupport(mod: dict[str, Any], ns: str) -> dict[str, str]:
    return _gen_json_rpc_server_markerized_typesupport(mod, ns)



# ── JSON RPC Client ──────────────────────────────────────────────────────────

def _gen_json_rpc_client_markerized_typesupport(
    mod: dict[str, Any],
    ns: str,
    previous_inc_cc: str | None = None,
) -> dict[str, str]:
    service_name = mod["service_name"]
    methods = mod.get("methods", [])
    types_include = _json_types_include(mod)

    helper_lines = [
        "template <typename SelfT>",
        "inline void _ibmw_json_rpc_client_loop_body_impl(SelfT* self) {",
        "  uint32_t round = 0;",
        "",
        "  while (self->running_) {",
    ]
    method_markers: list[str] = []
    for m in methods:
        req_type = m["request"]
        rsp_type = m["response"]
        method_name = m["name"]
        request_marker = _json_rpc_client_method_marker(
            _JSON_RPC_CLIENT_MARKERS[0], method_name)
        response_marker = _json_rpc_client_method_marker(
            _JSON_RPC_CLIENT_MARKERS[1], method_name)
        status_marker = _json_rpc_client_method_marker(
            _JSON_RPC_CLIENT_MARKERS[2], method_name)
        method_markers.extend([request_marker, response_marker, status_marker])
        helper_lines.extend([
            "      {",
            f"        {req_type} req;",
            f"        {rsp_type} rsp;",
            f"        {_format_marker_anchor(request_marker, 'BEGIN')}",
            "        // TODO: populate req fields",
            f"        {_format_marker_anchor(request_marker, 'END')}",
            "",
            f"        auto status = ibmw::json::rpc::Call<{req_type}, {rsp_type}>(",
            "            self->service_ref_, kServiceName,",
            f"            \"{method_name}\", req, rsp);",
            "",
            "        if (status.OK()) {",
            "          (void)rsp;",
            f"          {_format_marker_anchor(response_marker, 'BEGIN')}",
            f'          MW_LOG(self->GetLogger(), ibmw::common::util::kLogLevelInfo, "[Round {{}}] {method_name} => status={{}}", round, status.Code());',
            "          // TODO: consume rsp fields",
            f"          {_format_marker_anchor(response_marker, 'END')}",
            "        }",
            "",
            f"        {_format_marker_anchor(status_marker, 'BEGIN')}",
            "        if (!status.OK()) {",
            f'          MW_LOG(self->GetLogger(), ibmw::common::util::kLogLevelWarn, "[Round {{}}] {method_name} FAILED => status={{}}", round, status.Code());',
            "        }",
            f"        {_format_marker_anchor(status_marker, 'END')}",
            "",
            "        std::this_thread::sleep_for(std::chrono::milliseconds(500));",
            "      }",
            "",
        ])
    helper_lines.extend([
        "    ++round;",
        "    std::this_thread::sleep_for(std::chrono::seconds(1));",
        "  }",
        "}",
    ])
    helper_block = "\n".join(helper_lines)

    inc_h = f"""{_COPYRIGHT}
#pragma once

#include <atomic>
#include <chrono>
#include <thread>

#include "cpp/service/rpc_handle.h"
#include "cpp/util/json_rpc_helpers.h"
#include "{types_include}"

namespace {ns} {{

static constexpr const char* kServiceName = "{ns}.{service_name}";

}}  // namespace {ns}

// Thread-form client loop uses std::this_thread::sleep_for in the
// transport-owned helper emitted in the companion inc.cc.

#define IBMW_TS_RPC_CLIENT_LOOP_DECL                                           \
  void RpcLoop();                                                              \
                                                                                 \
 private:                                                                      \
  template <typename SelfT>                                                    \
  friend void _ibmw_json_rpc_client_loop_body_impl(SelfT*);                    \
  auto GetLogger() {{ return core_.GetLogger(); }}                             \
  ibmw::rpc::ServiceRef service_ref_;                                          \
  std::atomic<bool> running_{{false}};

#define IBMW_TS_RPC_CLIENT_LOOP_IMPL(name)                                     \
  void name::RpcLoop() {{                                                      \
    _ibmw_json_rpc_client_loop_body_impl(this);                                \
  }}
"""

    template_inc_cc = f"""{_COPYRIGHT}
// Transport-specific typesupport TU fragment for a json rpc_client.
// Auto-generated by ibmw_gen as an RPC typesupport sidecar.
// The thread-form RpcLoop helper lives here so JSON stays distinct from
// the DDS coroutine form.

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
        "msg_type_cpp": "ibmw::rpc::ServiceRef",
        "inc_h_content": inc_h,
        "inc_cc_content": inc_cc,
    }


def emit_rpc_client_typesupport(mod: dict[str, Any], ns: str) -> dict[str, str]:
    return _gen_json_rpc_client_markerized_typesupport(mod, ns)


_JSON_GENERATORS: dict[str, tuple[
    Callable[[dict[str, Any], str], str],
    Callable[[dict[str, Any], str, str], str],
]] = {
    "publisher": (_gen_json_publisher_header, _gen_json_publisher_cc),
    "subscriber": (_gen_json_subscriber_header, _gen_json_subscriber_cc),
    # rpc_server/rpc_client rows are handled by the unified RPC path
    # in module_gen._gen_rpc_{server,client}_unified handles all RPC emission
    # via _gen_*_rpc_*_markerized_typesupport sidecars.
}


def _emit_module(
    mod: dict[str, Any], ns: str, mod_dir: str
) -> list[tuple[str, str]] | None:
    """Emit the .h / .cc pair for a JSON transport module.

    Returns ``list[(rel_path, content)]`` or ``None`` if this plugin does
    not handle the given ``(mod['type'], "json")`` pair. Called by
    ``generate_modules()`` via the plugin registry.

    `publisher` is handled by the unified path
    in module_gen._gen_publisher_unified via
    emit_publisher_typesupport below; return None here so the upper
    dispatcher picks the unified route.

    `subscriber` is handled similarly via
    emit_subscriber_typesupport.
    """
    mtype = mod.get("type")
    if not isinstance(mtype, str):
        return None
    if mtype in ("publisher", "subscriber"):
        return None
    gens = _JSON_GENERATORS.get(mtype)
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
def _emit_publisher_typesupport(mod: dict[str, Any], ns: str) -> dict[str, str]:
    """Emit transport-specific publisher typesupport for json.

    Byte-semantic equivalent of the legacy _gen_json_publisher_{header,cc}
    path: same json util transport + custom types headers, same
    SenderAccess<MsgType> binding.
    """
    msg_type = mod["message_type"]
    topic = mod["topic"]
    types_include = _json_types_include(mod)

    inc_h = f"""{_COPYRIGHT}
#pragma once

// Transport-specific typesupport for a json publisher module.
// Auto-generated by ibmw_gen for the JSON transport.

#include <optional>

#include "{types_include}"
#include "cpp/util/json_transport.h"

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
// Transport-specific typesupport TU fragment for a json publisher.
// Auto-generated by ibmw_gen for the JSON transport. Intentionally empty.
"""

    return {
        "msg_type_cpp": msg_type,
        "inc_h_content": inc_h,
        "inc_cc_content": inc_cc,
        "register_call": "access_->RegisterSendType()",
        "publish_stmt": "access_->Send(msg);",
    }


# ── Unified subscriber typesupport emission ────────────────────────────────
def _emit_subscriber_typesupport(mod: dict[str, Any], ns: str) -> dict[str, str]:
    """Emit transport-specific subscriber typesupport for json.

    Byte-semantic equivalent of the legacy _gen_json_subscriber_{header,cc}
    path: same json util transport + custom types headers, same
    ReceiverAccess<MsgType> stack-local construction in Setup.
    """
    msg_type = mod["message_type"]
    topic = mod["topic"]
    types_include = _json_types_include(mod)

    inc_h = f"""{_COPYRIGHT}
#pragma once

// Transport-specific typesupport for a json subscriber module.
// Auto-generated by ibmw_gen for the JSON transport.

#include "{types_include}"
#include "cpp/util/json_transport.h"

// Message C++ type used by the unified template's OnMessage lambda.
#define IBMW_TS_RECEIVER_MSG_TYPE {msg_type}

// Setup-time initializer: expands inside Setup(ibmw::Runtime core) to
// bind the subscriber handle for the configured topic and construct a
// stack-local ReceiverAccess<T>.
#define IBMW_TS_RECEIVER_ACCESS_SETUP(core_expr, access_var)        \\
  auto _ibmw_sub = (core_expr).GetTransport().GetSubscriber(        \\
      "{topic}");                                                   \\
  ibmw::transport::ReceiverAccess<{msg_type}> access_var(_ibmw_sub)
"""

    inc_cc = f"""{_COPYRIGHT}
// Transport-specific typesupport TU fragment for a json subscriber.
// Auto-generated by ibmw_gen for the JSON transport. Intentionally empty.
"""

    return {
        "msg_type_cpp": msg_type,
        "inc_h_content": inc_h,
        "inc_cc_content": inc_cc,
    }


register(TransportSpec(
    name="json",
    emit_module=_emit_module,
    emit_publisher_typesupport=_emit_publisher_typesupport,
    emit_subscriber_typesupport=_emit_subscriber_typesupport,
    emit_rpc_server_typesupport=emit_rpc_server_typesupport,
    emit_rpc_client_typesupport=emit_rpc_client_typesupport,
    rpc_server_markers=list(_JSON_RPC_SERVER_MARKERS),
    rpc_client_markers=list(_JSON_RPC_CLIENT_MARKERS),
    marker_anchor_format=_JSON_MARKER_ANCHOR_FORMAT,
    meta={
        "kind": "serialization",
        "owns_type_keys": ["publisher", "subscriber", "rpc_server", "rpc_client"],
    },
))
