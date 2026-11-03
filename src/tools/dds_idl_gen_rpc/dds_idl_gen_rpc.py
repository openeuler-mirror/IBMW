#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.
#
# dds_idl_gen_rpc.py - Generate C++ RPC wrapper code from DDS IDL service definitions
#
# This tool generates ibmw RPC service/access C++ classes from a YAML service
# definition file that references DDS IDL message types.
#
# Usage:
#   python3 dds_idl_gen_rpc.py \
#     --service_file=path/to/ExampleService.yaml \
#     --output_path=path/to/output/dir \
#     [--idl_gencode_path=path/to/generated/idl/headers]
#
# The YAML service definition format:
#   module: ibmw.protocols.example
#   services:
#     - name: ExampleService
#       methods:
#         - name: GetFooData
#           request: GetFooDataReq
#           response: GetFooDataRsp

import os
import re
import sys
import yaml


def module_to_namespace(module_str):
    """Convert dotted module path to C++ namespace. e.g. 'ibmw.protocols.example' -> 'ibmw::protocols::example'"""
    return module_str.replace(".", "::")


def module_to_namespace_begin(module_str):
    """Generate opening namespace declarations."""
    parts = module_str.split(".")
    return "\n".join(f"namespace {p} {{" for p in parts)


def module_to_namespace_end(module_str):
    """Generate closing namespace declarations."""
    parts = module_str.split(".")
    return "\n".join(f"}}  // namespace {p}" for p in reversed(parts))


def gen_h_file(module_str, services, idl_file_stem, idl_gencode_path):
    """Generate the .ibmw_rpc.dds.h header file content."""
    ns = module_to_namespace(module_str)
    ns_begin = module_to_namespace_begin(module_str)
    ns_end = module_to_namespace_end(module_str)

    # Build includes for IDL-generated types
    # Each request/response type needs its corresponding .hpp and PubSubTypes.hpp
    type_includes = set()
    for svc in services:
        for method in svc["methods"]:
            type_includes.add(method["request"])
            type_includes.add(method["response"])

    # Include the IDL gencode type definitions in the public RPC header.
    # CdrAux.ipp contains non-inline FastCDR specializations, so it must stay in
    # the generated .cc to avoid duplicate symbols in every including TU.
    idl_include = f'#include "{idl_file_stem}.hpp"'

    # Build service class declarations
    sync_services = []
    async_services = []
    co_services = []
    register_funcs = []
    sync_proxies = []
    async_proxies = []
    future_proxies = []
    co_proxies = []
    co_clients = []
    co_servers = []

    for svc in services:
        svc_name = svc["name"]

        # SyncService
        methods_sync = []
        for m in svc["methods"]:
            methods_sync.append(f"""
  virtual ibmw::rpc::Status {m["name"]}(
      ibmw::rpc::ContextRef ctx_ref,
      const {m["request"]}& req,
      {m["response"]}& rsp) {{
    return ibmw::rpc::Status(IBMW_RPC_STATUS_SVR_NOT_IMPLEMENTED);
  }}""")

        sync_services.append(f"""class {svc_name}SyncService : public ibmw::rpc::ServiceBase {{
 public:
  {svc_name}SyncService();
  ~{svc_name}SyncService() override = default;
{"".join(methods_sync)}
}};""")

        # AsyncService
        methods_async = []
        for m in svc["methods"]:
            methods_async.append(f"""
  virtual void {m["name"]}(
      ibmw::rpc::ContextRef ctx_ref,
      const {m["request"]}& req,
      {m["response"]}& rsp,
      std::function<void(ibmw::rpc::Status)>&& callback) {{
    callback(ibmw::rpc::Status(IBMW_RPC_STATUS_SVR_NOT_IMPLEMENTED));
  }}""")

        async_services.append(f"""class {svc_name}AsyncService : public ibmw::rpc::ServiceBase {{
 public:
  {svc_name}AsyncService();
  ~{svc_name}AsyncService() override = default;
{"".join(methods_async)}
}};""")

        # CoService
        methods_co = []
        for m in svc["methods"]:
            methods_co.append(f"""
  virtual ibmw::co::Task<ibmw::rpc::Status> {m["name"]}(
      ibmw::rpc::ContextRef ctx_ref,
      const {m["request"]}& req,
      {m["response"]}& rsp) {{
    co_return ibmw::rpc::Status(IBMW_RPC_STATUS_SVR_NOT_IMPLEMENTED);
  }}""")

        co_services.append(f"""class {svc_name}CoService : public ibmw::rpc::CoServiceBase {{
 public:
  {svc_name}CoService();
  ~{svc_name}CoService() override = default;
{"".join(methods_co)}
}};""")

        # RegisterClientFunc declarations
        register_funcs.append(
            f"bool Register{svc_name}ClientFunc(ibmw::rpc::ServiceRef service_ref, std::string_view service_name);\n"
            f"bool Register{svc_name}ClientFunc(ibmw::rpc::ServiceRef service_ref);")

        # SyncAccess
        methods_sync_access = []
        for m in svc["methods"]:
            methods_sync_access.append(f"""
  ibmw::rpc::Status {m["name"]}(
      ibmw::rpc::ContextRef ctx_ref,
      const {m["request"]}& req,
      {m["response"]}& rsp);

  ibmw::rpc::Status {m["name"]}(
      const {m["request"]}& req,
      {m["response"]}& rsp) {{
    return {m["name"]}(ibmw::rpc::ContextRef(), req, rsp);
  }}""")

        sync_proxies.append(f"""class {svc_name}SyncAccess : public ibmw::rpc::SyncAccessBase {{
 public:
  explicit {svc_name}SyncAccess(ibmw::rpc::ServiceRef service_ref);
  explicit {svc_name}SyncAccess(ibmw::rpc::ServiceRef service_ref, std::string_view service_name);
  ~{svc_name}SyncAccess() = default;

  static bool RegisterClientFunc(ibmw::rpc::ServiceRef service_ref) {{
    return Register{svc_name}ClientFunc(service_ref);
  }}

  static bool RegisterClientFunc(ibmw::rpc::ServiceRef service_ref, std::string_view service_name) {{
    return Register{svc_name}ClientFunc(service_ref, service_name);
  }}
{"".join(methods_sync_access)}
}};""")

        # AsyncAccess
        methods_async_access = []
        for m in svc["methods"]:
            methods_async_access.append(f"""
  void {m["name"]}(
      ibmw::rpc::ContextRef ctx_ref,
      const {m["request"]}& req,
      {m["response"]}& rsp,
      std::function<void(ibmw::rpc::Status)>&& callback);

  void {m["name"]}(
      const {m["request"]}& req,
      {m["response"]}& rsp,
      std::function<void(ibmw::rpc::Status)>&& callback) {{
    {m["name"]}(ibmw::rpc::ContextRef(), req, rsp, std::move(callback));
  }}

  // Keeps req and rsp alive until the call completes.
  void {m["name"]}(
      ibmw::rpc::ContextRef ctx_ref,
      std::shared_ptr<const {m["request"]}> req,
      std::shared_ptr<{m["response"]}> rsp,
      std::function<void(ibmw::rpc::Status)>&& callback);

  void {m["name"]}(
      std::shared_ptr<const {m["request"]}> req,
      std::shared_ptr<{m["response"]}> rsp,
      std::function<void(ibmw::rpc::Status)>&& callback) {{
    {m["name"]}(ibmw::rpc::ContextRef(), std::move(req), std::move(rsp), std::move(callback));
  }}""")

        async_proxies.append(f"""class {svc_name}AsyncAccess : public ibmw::rpc::AsyncAccessBase {{
 public:
  explicit {svc_name}AsyncAccess(ibmw::rpc::ServiceRef service_ref);
  explicit {svc_name}AsyncAccess(ibmw::rpc::ServiceRef service_ref, std::string_view service_name);
  ~{svc_name}AsyncAccess() = default;

  static bool RegisterClientFunc(ibmw::rpc::ServiceRef service_ref) {{
    return Register{svc_name}ClientFunc(service_ref);
  }}

  static bool RegisterClientFunc(ibmw::rpc::ServiceRef service_ref, std::string_view service_name) {{
    return Register{svc_name}ClientFunc(service_ref, service_name);
  }}
{"".join(methods_async_access)}
}};""")

        # FutureAccess
        methods_future_access = []
        for m in svc["methods"]:
            methods_future_access.append(f"""
  std::future<ibmw::rpc::Status> {m["name"]}(
      ibmw::rpc::ContextRef ctx_ref,
      const {m["request"]}& req,
      {m["response"]}& rsp);

  std::future<ibmw::rpc::Status> {m["name"]}(
      const {m["request"]}& req,
      {m["response"]}& rsp) {{
    return {m["name"]}(ibmw::rpc::ContextRef(), req, rsp);
  }}

  // Keeps req and rsp alive until the call completes.
  std::future<ibmw::rpc::Status> {m["name"]}(
      ibmw::rpc::ContextRef ctx_ref,
      std::shared_ptr<const {m["request"]}> req,
      std::shared_ptr<{m["response"]}> rsp);

  std::future<ibmw::rpc::Status> {m["name"]}(
      std::shared_ptr<const {m["request"]}> req,
      std::shared_ptr<{m["response"]}> rsp) {{
    return {m["name"]}(ibmw::rpc::ContextRef(), std::move(req), std::move(rsp));
  }}""")

        future_proxies.append(f"""class {svc_name}FutureAccess : public ibmw::rpc::FutureAccessBase {{
 public:
  explicit {svc_name}FutureAccess(ibmw::rpc::ServiceRef service_ref);
  explicit {svc_name}FutureAccess(ibmw::rpc::ServiceRef service_ref, std::string_view service_name);
  ~{svc_name}FutureAccess() = default;

  static bool RegisterClientFunc(ibmw::rpc::ServiceRef service_ref) {{
    return Register{svc_name}ClientFunc(service_ref);
  }}

  static bool RegisterClientFunc(ibmw::rpc::ServiceRef service_ref, std::string_view service_name) {{
    return Register{svc_name}ClientFunc(service_ref, service_name);
  }}
{"".join(methods_future_access)}
}};""")

        # CoAccess
        methods_co_access = []
        for m in svc["methods"]:
            methods_co_access.append(f"""
  ibmw::co::Task<ibmw::rpc::Status> {m["name"]}(
      ibmw::rpc::ContextRef ctx_ref,
      const {m["request"]}& req,
      {m["response"]}& rsp);

  ibmw::co::Task<ibmw::rpc::Status> {m["name"]}(
      const {m["request"]}& req,
      {m["response"]}& rsp) {{
    return {m["name"]}(ibmw::rpc::ContextRef(), req, rsp);
  }}""")

        co_proxies.append(f"""class {svc_name}CoAccess : public ibmw::rpc::CoAccessBase {{
 public:
  explicit {svc_name}CoAccess(ibmw::rpc::ServiceRef service_ref);
  explicit {svc_name}CoAccess(ibmw::rpc::ServiceRef service_ref, std::string_view service_name);
  ~{svc_name}CoAccess() = default;

  static bool RegisterClientFunc(ibmw::rpc::ServiceRef service_ref) {{
    return Register{svc_name}ClientFunc(service_ref);
  }}

  static bool RegisterClientFunc(ibmw::rpc::ServiceRef service_ref, std::string_view service_name) {{
    return Register{svc_name}ClientFunc(service_ref, service_name);
  }}
{"".join(methods_co_access)}
}};""")

        # CoClient
        client_members = []
        for m in svc["methods"]:
            client_members.append(
                f'  ::ibmw::context::res::Client<{m["request"]}, {m["response"]}> {m["name"]};')

        co_clients.append(f"""class {svc_name}CoClient {{
 public:
  /**
   * @brief Initialize the service resource based on ibmw Context.
   */
  void Init(const std::shared_ptr<::ibmw::context::Context> & ctx_ptr, std::string service_name = "");

 public:
{chr(10).join(client_members)}

 public:
  static std::vector<std::string> GetMethodNames(std::string service_name = {{}});

}};""")

        # CoServer + IbmwImpl
        server_members = []
        for m in svc["methods"]:
            server_members.append(
                f'  ::ibmw::context::res::Server<{m["request"]}, {m["response"]}> {m["name"]};')

        impl_methods = []
        for m in svc["methods"]:
            impl_methods.append(f"""
  ibmw::co::Task<ibmw::rpc::Status> {m["name"]}(
      ibmw::rpc::ContextRef ctx_ref,
      const {m["request"]}& req,
      {m["response"]}& rsp) override;
""")

        co_servers.append(f"""// Forward declaration
class {svc_name}IbmwImpl;

class {svc_name}CoServer : public std::enable_shared_from_this<{svc_name}CoServer> {{
 public:
  void Init(const std::shared_ptr<::ibmw::context::Context> & ctx_ptr, std::string service_name = "");

 public:
{chr(10).join(server_members)}

 public:
  static std::vector<std::string> GetMethodNames(std::string service_name = {{}});

 private:
  std::weak_ptr<::ibmw::context::Context> ctx_weak_ptr_;
  std::shared_ptr<{svc_name}IbmwImpl> impl_;
}};

class {svc_name}IbmwImpl final : public {svc_name}CoService {{
 public:
  explicit {svc_name}IbmwImpl(
      std::weak_ptr<::ibmw::context::Context> weak_ctx_ptr,
      std::shared_ptr<{svc_name}CoServer> res);
{"".join(impl_methods)}
 private:
  std::weak_ptr<::ibmw::context::Context> weak_ctx_ptr_;
  std::shared_ptr<{svc_name}CoServer> res_;
}};""")

    # Assemble the header file
    header = f"""/**
 * @file {idl_file_stem}.ibmw_rpc.dds.h
 * @brief This file was generated by dds_idl_gen_rpc, do not edit it!!!
 */
#pragma once

#include <future>
#include <memory>

#include "cpp/service/rpc_handle.h"
#include "cpp/service/rpc_status.h"
#include "cpp/util/version.h"
#include "cpp/co/task.h"
#include "cpp/context/context.h"

#include "cpp/ibmw_sdk.h"

{idl_include}

static_assert(1000 <= IBMW_RUNTIME_VERSION_INT,
              "IBMW_RUNTIME_VERSION is older than generated code version 0.1.0");
static_assert(IBMW_MIN_GENCODE_VERSION_INT <= 1000,
              "IBMW_MIN_GENCODE_VERSION is greater than generated code version 0.1.0");

{ns_begin}

{chr(10).join(sync_services)}

{chr(10).join(async_services)}

{chr(10).join(co_services)}

{chr(10).join(register_funcs)}

{chr(10).join(sync_proxies)}

{chr(10).join(async_proxies)}

{chr(10).join(future_proxies)}

{chr(10).join(co_proxies)}

{chr(10).join(co_clients)}

{chr(10).join(co_servers)}

{ns_end}
"""
    return header


def gen_cc_file(module_str, services, idl_file_stem, idl_gencode_path):
    """Generate the .ibmw_rpc.dds.cc implementation file content."""
    ns = module_to_namespace(module_str)
    ns_begin = module_to_namespace_begin(module_str)
    ns_end = module_to_namespace_end(module_str)

    # Build type support calls - we use GetDdsMessageTypeSupport<T>() with FastCDR
    # CdrAux provides serialize/deserialize, no PubSubType needed
    # When dds_type_name is specified (for ROS2 interop), we use
    # GetDdsMessageTypeSupportByName<T>() to register the ROS2-compatible DDS type name.

    idl_cdraux_include = f'#include "{idl_file_stem}CdrAux.ipp"'
    dep_cdraux_includes = []
    if idl_gencode_path:
        type_hpp_path = os.path.join(idl_gencode_path, f"{idl_file_stem}.hpp")
        if os.path.exists(type_hpp_path):
            with open(type_hpp_path, 'r') as f:
                type_hpp_content = f.read()
            for dep_match in re.finditer(r'#include\s+"(\w+)\.hpp"', type_hpp_content):
                dep_name = dep_match.group(1)
                if dep_name != idl_file_stem:
                    dep_ipp = f"{dep_name}CdrAux.ipp"
                    dep_ipp_path = os.path.join(idl_gencode_path, dep_ipp)
                    if os.path.exists(dep_ipp_path):
                        dep_cdraux_includes.append(f'#include "{dep_ipp}"')
    dep_cdraux_text = '\n'.join(dep_cdraux_includes)
    if dep_cdraux_text:
        dep_cdraux_text += '\n'

    def _ts_call(cpp_type, dds_type_name=None):
        """Generate the GetDdsMessageTypeSupport call for a type."""
        if dds_type_name:
            return f'ibmw::dds::GetDdsMessageTypeSupportByName<{cpp_type}>("{dds_type_name}")'
        else:
            return f'ibmw::dds::GetDdsMessageTypeSupport<{cpp_type}>()'

    def _ros2_sn_ptr(svc):
        """Generate the custom_type_support_ptr expression for ROS2 service name.

        When ros2_service_name is set, we pass a static string pointer so that
        DdsRpcDriver can use it for topic naming (ROS2-compatible topic names).
        Otherwise nullptr (standard IBMW topic naming).
        """
        ros2_sn = svc.get("ros2_service_name")
        if ros2_sn:
            return f"kRos2ServiceName_{svc['name']}"
        return "nullptr"

    # Build service implementations
    sections = []

    # Static constants
    sections.append(f'static constexpr std::string_view kRpcType = "dds";')
    sections.append("")

    for svc in services:
        svc_name = svc["name"]
        sections.append(f'static constexpr std::string_view k{svc_name}Name = "{module_str}.{svc_name}";')
        ros2_sn = svc.get("ros2_service_name")
        if ros2_sn:
            sections.append(f'static constexpr char kRos2ServiceName_{svc_name}[] = "{ros2_sn}";')

    sections.append("")

    for svc in services:
        svc_name = svc["name"]

        # SyncService constructor
        sync_ctor_body = []
        for m in svc["methods"]:
            req = m["request"]
            rsp = m["response"]
            req_dds_tn = m.get("req_dds_type_name")
            rsp_dds_tn = m.get("rsp_dds_type_name")
            sync_ctor_body.append(f"""  {{
    ibmw::rpc::ServiceFunc service_callback(
        [this](const ibmw_service_context_base_t* ctx, const void* req, void* rsp, ibmw_function_base_t* result_callback_ptr) {{
          ibmw::rpc::ServiceCallback result_callback(result_callback_ptr);

          ibmw::rpc::ContextRef ctx_ref(ctx);

          auto status = {m["name"]}(
              ctx_ref,
              *static_cast<const {req}*>(req),
              *static_cast<{rsp}*>(rsp));

          result_callback(status.Code());
        }});
    RegisterServiceFunc(
        "{m["name"]}",
        {_ros2_sn_ptr(svc)},
        {_ts_call(req, req_dds_tn)},
        {_ts_call(rsp, rsp_dds_tn)},
        std::move(service_callback));
  }}""")

        sections.append(f"""{svc_name}SyncService::{svc_name}SyncService() : ibmw::rpc::ServiceBase(kRpcType, k{svc_name}Name) {{
{chr(10).join(sync_ctor_body)}
}}""")

    sections.append("")

    for svc in services:
        svc_name = svc["name"]

        # AsyncService constructor
        async_ctor_body = []
        for m in svc["methods"]:
            req = m["request"]
            rsp = m["response"]
            req_dds_tn = m.get("req_dds_type_name")
            rsp_dds_tn = m.get("rsp_dds_type_name")
            async_ctor_body.append(f"""  {{
    ibmw::rpc::ServiceFunc service_callback(
        [this](const ibmw_service_context_base_t* ctx, const void* req, void* rsp, ibmw_function_base_t* result_callback_ptr) {{
          auto result_callback_func_ptr = std::make_shared<ibmw::rpc::ServiceCallback>(result_callback_ptr);

          ibmw::rpc::ContextRef ctx_ref(ctx);

          {m["name"]}(
              ctx_ref,
              *static_cast<const {req}*>(req),
              *static_cast<{rsp}*>(rsp),
              [result_callback_func_ptr{{std::move(result_callback_func_ptr)}}](ibmw::rpc::Status status) {{
                (*result_callback_func_ptr)(status.Code());
              }});
        }});
    RegisterServiceFunc(
        "{m["name"]}",
        {_ros2_sn_ptr(svc)},
        {_ts_call(req, req_dds_tn)},
        {_ts_call(rsp, rsp_dds_tn)},
        std::move(service_callback));
  }}""")

        sections.append(f"""{svc_name}AsyncService::{svc_name}AsyncService() : ibmw::rpc::ServiceBase(kRpcType, k{svc_name}Name) {{
{chr(10).join(async_ctor_body)}
}}""")

    sections.append("")

    for svc in services:
        svc_name = svc["name"]

        # CoService constructor
        co_ctor_body = []
        for m in svc["methods"]:
            req = m["request"]
            rsp = m["response"]
            req_dds_tn = m.get("req_dds_type_name")
            rsp_dds_tn = m.get("rsp_dds_type_name")
            co_ctor_body.append(f"""  {{
    ibmw::rpc::ServiceFunc service_callback(
        [this](const ibmw_service_context_base_t* ctx, const void* req, void* rsp, ibmw_function_base_t* result_callback_ptr) {{
          auto handle_ptr = std::make_shared<ibmw::rpc::CoRpcHandle>(
              [this](ibmw::rpc::ContextRef ctx_ref, const void* req_ptr, void* rsp_ptr)
                  -> ibmw::co::Task<ibmw::rpc::Status> {{
                return {m["name"]}(
                    ctx_ref,
                    *static_cast<const {req}*>(req_ptr),
                    *static_cast<{rsp}*>(rsp_ptr));
              }});

          auto result_callback = std::make_shared<ibmw::rpc::ServiceCallback>(result_callback_ptr);

          auto ctx_owner = std::make_shared<ibmw::rpc::Context>(ibmw::rpc::ContextRef(ctx));
          ibmw::rpc::ContextRef ctx_ref(ctx_owner->NativeHandle());
          auto req_owner = std::make_shared<{req}>(*static_cast<const {req}*>(req));
          auto rsp_owner = std::make_shared<{rsp}>(*static_cast<{rsp}*>(rsp));

          ibmw::co::StartDetached(
              ibmw::co::On(
                  ibmw::co::InlineExecutor(),
                  filter_mgr_.InvokeRpc(handle_ptr, ctx_ref, req_owner.get(), rsp_owner.get())) |
              ibmw::co::Then(
                  [handle_ptr, ctx_owner, rsp, req_owner, rsp_owner,
                   result_callback{{std::move(result_callback)}}](ibmw::rpc::Status status) {{
                    *static_cast<{rsp}*>(rsp) = *rsp_owner;
                    (*result_callback)(status.Code());
                  }}));
        }});
    RegisterServiceFunc(
        "{m["name"]}",
        {_ros2_sn_ptr(svc)},
        {_ts_call(req, req_dds_tn)},
        {_ts_call(rsp, rsp_dds_tn)},
        std::move(service_callback));
  }}""")

        sections.append(f"""{svc_name}CoService::{svc_name}CoService() : ibmw::rpc::CoServiceBase(kRpcType, k{svc_name}Name) {{
{chr(10).join(co_ctor_body)}
}}""")

    sections.append("")

    # RegisterClientFunc implementations
    for svc in services:
        svc_name = svc["name"]

        reg_body = []
        for m in svc["methods"]:
            req = m["request"]
            rsp = m["response"]
            req_dds_tn = m.get("req_dds_type_name")
            rsp_dds_tn = m.get("rsp_dds_type_name")
            reg_body.append(f"""  if (!(service_ref.RegisterClientFunc(
          kRpcType,
          service_name,
          "{m["name"]}",
          {_ros2_sn_ptr(svc)},
          {_ts_call(req, req_dds_tn)},
          {_ts_call(rsp, rsp_dds_tn)})))
    return false;""")

        sections.append(f"""bool Register{svc_name}ClientFunc(ibmw::rpc::ServiceRef service_ref, std::string_view service_name) {{
{chr(10).join(reg_body)}
  return true;
}}
bool Register{svc_name}ClientFunc(ibmw::rpc::ServiceRef service_ref) {{
  return Register{svc_name}ClientFunc(service_ref, k{svc_name}Name);
}}""")

    sections.append("")

    # SyncAccess implementations
    for svc in services:
        svc_name = svc["name"]

        sections.append(f"""{svc_name}SyncAccess::{svc_name}SyncAccess(ibmw::rpc::ServiceRef service_ref)
    : ibmw::rpc::SyncAccessBase(service_ref, kRpcType, k{svc_name}Name) {{}}

{svc_name}SyncAccess::{svc_name}SyncAccess(ibmw::rpc::ServiceRef service_ref, std::string_view service_name)
    : ibmw::rpc::SyncAccessBase(service_ref, kRpcType, service_name) {{}}
""")

        for m in svc["methods"]:
            req = m["request"]
            rsp = m["response"]
            sections.append(f"""ibmw::rpc::Status {svc_name}SyncAccess::{m["name"]}(
    ibmw::rpc::ContextRef ctx_ref,
    const {req}& req,
    {rsp}& rsp) {{
  const std::string& full_func_name = ibmw::rpc::GetFullFuncName(rpc_type_, service_name_, "{m["name"]}");
  return Call(full_func_name, ctx_ref, req, rsp);
}}""")

    sections.append("")

    # AsyncAccess implementations
    for svc in services:
        svc_name = svc["name"]

        sections.append(f"""{svc_name}AsyncAccess::{svc_name}AsyncAccess(ibmw::rpc::ServiceRef service_ref)
    : ibmw::rpc::AsyncAccessBase(service_ref, kRpcType, k{svc_name}Name) {{}}

{svc_name}AsyncAccess::{svc_name}AsyncAccess(ibmw::rpc::ServiceRef service_ref, std::string_view service_name)
    : ibmw::rpc::AsyncAccessBase(service_ref, kRpcType, service_name) {{}}
""")

        for m in svc["methods"]:
            req = m["request"]
            rsp = m["response"]
            sections.append(f"""void {svc_name}AsyncAccess::{m["name"]}(
    ibmw::rpc::ContextRef ctx_ref,
    const {req}& req,
    {rsp}& rsp,
    std::function<void(ibmw::rpc::Status)>&& callback) {{
  const std::string& full_func_name = ibmw::rpc::GetFullFuncName(rpc_type_, service_name_, "{m["name"]}");
  Call(full_func_name, ctx_ref, req, rsp, std::move(callback));
}}

void {svc_name}AsyncAccess::{m["name"]}(
    ibmw::rpc::ContextRef ctx_ref,
    std::shared_ptr<const {req}> req,
    std::shared_ptr<{rsp}> rsp,
    std::function<void(ibmw::rpc::Status)>&& callback) {{
  const std::string& full_func_name = ibmw::rpc::GetFullFuncName(rpc_type_, service_name_, "{m["name"]}");
  Call(full_func_name, ctx_ref, std::move(req), std::move(rsp), std::move(callback));
}}""")

    sections.append("")

    # FutureAccess implementations
    for svc in services:
        svc_name = svc["name"]

        sections.append(f"""{svc_name}FutureAccess::{svc_name}FutureAccess(ibmw::rpc::ServiceRef service_ref)
    : ibmw::rpc::FutureAccessBase(service_ref, kRpcType, k{svc_name}Name) {{}}

{svc_name}FutureAccess::{svc_name}FutureAccess(ibmw::rpc::ServiceRef service_ref, std::string_view service_name)
    : ibmw::rpc::FutureAccessBase(service_ref, kRpcType, service_name) {{}}
""")

        for m in svc["methods"]:
            req = m["request"]
            rsp = m["response"]
            sections.append(f"""std::future<ibmw::rpc::Status> {svc_name}FutureAccess::{m["name"]}(
    ibmw::rpc::ContextRef ctx_ref,
    const {req}& req,
    {rsp}& rsp) {{
  const std::string& full_func_name = ibmw::rpc::GetFullFuncName(rpc_type_, service_name_, "{m["name"]}");
  return Call(full_func_name, ctx_ref, req, rsp);
}}

std::future<ibmw::rpc::Status> {svc_name}FutureAccess::{m["name"]}(
    ibmw::rpc::ContextRef ctx_ref,
    std::shared_ptr<const {req}> req,
    std::shared_ptr<{rsp}> rsp) {{
  const std::string& full_func_name = ibmw::rpc::GetFullFuncName(rpc_type_, service_name_, "{m["name"]}");
  return Call(full_func_name, ctx_ref, std::move(req), std::move(rsp));
}}""")

    sections.append("")

    # CoAccess implementations
    for svc in services:
        svc_name = svc["name"]

        sections.append(f"""{svc_name}CoAccess::{svc_name}CoAccess(ibmw::rpc::ServiceRef service_ref)
    : ibmw::rpc::CoAccessBase(service_ref, kRpcType, k{svc_name}Name) {{}}

{svc_name}CoAccess::{svc_name}CoAccess(ibmw::rpc::ServiceRef service_ref, std::string_view service_name)
    : ibmw::rpc::CoAccessBase(service_ref, kRpcType, service_name) {{}}
""")

        for m in svc["methods"]:
            req = m["request"]
            rsp = m["response"]
            sections.append(f"""ibmw::co::Task<ibmw::rpc::Status> {svc_name}CoAccess::{m["name"]}(
    ibmw::rpc::ContextRef ctx_ref,
    const {req}& req,
    {rsp}& rsp) {{
  const std::string& full_func_name = ibmw::rpc::GetFullFuncName(rpc_type_, service_name_, "{m["name"]}");
  co_return co_await Call(full_func_name, ctx_ref, req, rsp);
}}""")

    sections.append("")

    # CoClient implementations
    for svc in services:
        svc_name = svc["name"]

        init_methods = []
        for m in svc["methods"]:
            req = m["request"]
            rsp = m["response"]
            init_methods.append(f"""
  this->{m["name"]} = ctx_ptr->cli().Init<{req}, {rsp}>(
    kRpcType.data() + std::string(":/") + service_name + "/{m["name"]}",
    [access](ibmw::rpc::ContextRef ctx_ref, const {req}& req, {rsp}& rsp)
      -> ::ibmw::co::Task<::ibmw::rpc::Status>
    {{
      co_return co_await access->{m["name"]}(ctx_ref, req, rsp);
    }}
  );
""")

        method_names = []
        for m in svc["methods"]:
            method_names.append(f'    std::string(kRpcType) + ":/" + service_name + "/{m["name"]}",')

        sections.append(f"""void {svc_name}CoClient::Init(const std::shared_ptr<::ibmw::context::Context> & ctx_ptr, std::string service_name) {{
  const ::ibmw::rpc::ServiceRef rpc_handle = ::ibmw::context::Context::GetRawRef(*ctx_ptr).GetService();

  if (not service_name.empty())
    Register{svc_name}ClientFunc(rpc_handle, service_name);
  else {{
    Register{svc_name}ClientFunc(rpc_handle);
    service_name = "{module_str}.{svc_name}";
  }}

  auto access = std::make_shared<{svc_name}CoAccess>(rpc_handle);
  access->SetServiceName(service_name);
{"".join(init_methods)}}}

std::vector<std::string> {svc_name}CoClient::GetMethodNames(std::string service_name) {{
  if (service_name.empty())
    service_name = "{module_str}.{svc_name}";

  return {{
{chr(10).join(method_names)}
  }};
}}""")

    sections.append("")

    # CoServer implementations
    for svc in services:
        svc_name = svc["name"]

        server_init_methods = []
        for m in svc["methods"]:
            req = m["request"]
            rsp = m["response"]
            server_init_methods.append(f"""
  this->{m["name"]} = ctx_ptr->srv().Init<{req}, {rsp}>(
    kRpcType.data() + std::string(":/") + service_name + "/{m["name"]}"
  );
""")

        server_method_names = []
        for m in svc["methods"]:
            server_method_names.append(f'    std::string(kRpcType) + ":/" + service_name + "/{m["name"]}",')

        impl_method_impls = []
        for m in svc["methods"]:
            req = m["request"]
            rsp = m["response"]
            impl_method_impls.append(f"""
ibmw::co::Task<ibmw::rpc::Status> {svc_name}IbmwImpl::{m["name"]}(
    ibmw::rpc::ContextRef ctx_ref,
    const {req}& req,
    {rsp}& rsp) {{
  auto ctx_ptr = weak_ctx_ptr_.lock();

  ctx_ptr->LetMe();
  co_return co_await ctx_ptr->srv().Serving(res_->{m["name"]}, ctx_ref, req, rsp);
}}
""")

        sections.append(f"""void {svc_name}CoServer::Init(const std::shared_ptr<::ibmw::context::Context> & ctx_ptr, std::string service_name) {{
  if (service_name.empty())
    service_name = "{module_str}.{svc_name}";

  ctx_weak_ptr_ = ctx_ptr;

  impl_ = std::make_shared<{svc_name}IbmwImpl>(
      std::weak_ptr<::ibmw::context::Context>(ctx_ptr), shared_from_this());
{"".join(server_init_methods)}
  ::ibmw::context::Context::GetRawRef(*ctx_ptr).GetService().RegisterService(service_name, impl_.get());
}}

std::vector<std::string> {svc_name}CoServer::GetMethodNames(std::string service_name) {{
  if (service_name.empty())
    service_name = "{module_str}.{svc_name}";

  return {{
{chr(10).join(server_method_names)}
  }};
}}

{svc_name}IbmwImpl::{svc_name}IbmwImpl(
    std::weak_ptr<::ibmw::context::Context> weak_ctx_ptr,
    std::shared_ptr<{svc_name}CoServer> res)
    : weak_ctx_ptr_(std::move(weak_ctx_ptr)), res_(std::move(res)) {{}}
{"".join(impl_method_impls)}""")

    # Assemble the cc file
    cc = f"""/**
 * @file {idl_file_stem}.ibmw_rpc.dds.cc
 * @brief This file was generated by dds_idl_gen_rpc, do not edit it!!!
 */

#include "{idl_file_stem}.ibmw_rpc.dds.h"

{dep_cdraux_text}{idl_cdraux_include}

#include "cpp/co/inline_executor.h"
#include "cpp/co/on.h"
#include "cpp/co/start_detached.h"
#include "cpp/co/then.h"
#include "dds_types/util/dds_type_support.h"

#include "cpp/ibmw_sdk.h"

{ns_begin}

{chr(10).join(sections)}

{ns_end}
"""
    return cc


def main():
    service_file = ""
    output_path = ""
    idl_gencode_path = ""

    for arg in sys.argv[1:]:
        if "=" in arg:
            key, value = arg.split("=", 1)
        else:
            continue

        if key == "--service_file":
            service_file = value
        elif key == "--output_path":
            output_path = value
        elif key == "--idl_gencode_path":
            idl_gencode_path = value

    if not service_file:
        print("Error: --service_file is required", file=sys.stderr)
        sys.exit(1)
    if not output_path:
        print("Error: --output_path is required", file=sys.stderr)
        sys.exit(1)

    # Read YAML service definition
    with open(service_file, "r") as f:
        config = yaml.safe_load(f)

    module_str = config["module"]
    services = config["services"]

    # Determine IDL file stem for the output filenames
    if "idl_file" in config:
        # Only the file name: the outputs stay inside output_path.
        idl_file_stem = os.path.splitext(os.path.basename(config["idl_file"]))[0]
        if not re.fullmatch(r"[A-Za-z0-9_][A-Za-z0-9_.-]*", idl_file_stem):
            print(f"Error: invalid idl_file '{config['idl_file']}'", file=sys.stderr)
            sys.exit(1)
    else:
        # Fallback: use the service file stem
        idl_file_stem = os.path.splitext(os.path.basename(service_file))[0]

    # Generate output files
    os.makedirs(output_path, exist_ok=True)

    h_file_path = os.path.join(output_path, f"{idl_file_stem}.ibmw_rpc.dds.h")
    cc_file_path = os.path.join(output_path, f"{idl_file_stem}.ibmw_rpc.dds.cc")

    h_content = gen_h_file(module_str, services, idl_file_stem, idl_gencode_path)
    cc_content = gen_cc_file(module_str, services, idl_file_stem, idl_gencode_path)

    with open(h_file_path, "w") as f:
        f.write(h_content)

    with open(cc_file_path, "w") as f:
        f.write(cc_content)

    print(f"Generated: {h_file_path}")
    print(f"Generated: {cc_file_path}")


if __name__ == "__main__":
    main()
