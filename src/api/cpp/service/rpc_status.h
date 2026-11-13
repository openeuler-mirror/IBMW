// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

#include "c/service/rpc_status_base.h"

namespace ibmw::rpc {

class Status {
 public:
  Status() = default;
  ~Status() = default;

  explicit Status(ibmw_rpc_status_code_t code) : code_(code) {}
  explicit Status(uint32_t code) : code_(code) {}

  bool OK() const {
    return code_ == ibmw_rpc_status_code_t::IBMW_RPC_STATUS_OK;
  }

  explicit operator bool() const { return OK(); }

  void SetCode(uint32_t code) { code_ = code; }
  void SetCode(ibmw_rpc_status_code_t code) { code_ = code; }

  uint32_t Code() const { return code_; }

  std::string ToString() const {
    return std::string(OK() ? "suc" : "fail") +
           ", code " + std::to_string(code_) +
           ", msg: " + std::string(GetCodeMsg(code_));
  }

  static std::string_view GetCodeMsg(uint32_t code) {
    static const std::unordered_map<uint32_t, std::string_view> kCodeMsgMap{
        {IBMW_RPC_STATUS_OK, "OK"},
        {IBMW_RPC_STATUS_UNKNOWN, "Unknown error"},
        {IBMW_RPC_STATUS_TIMEOUT, "Timeout"},
        // svr side
        {IBMW_RPC_STATUS_SVR_UNKNOWN, "Server side unknown error"},
        {IBMW_RPC_STATUS_SVR_DRIVER_INTERNAL_ERROR, "Server side driver internal error"},
        {IBMW_RPC_STATUS_SVR_NOT_IMPLEMENTED, "Server not implemented"},
        {IBMW_RPC_STATUS_SVR_NOT_FOUND, "Server not found"},
        {IBMW_RPC_STATUS_SVR_INVALID_SERIALIZATION_TYPE, "Server side invalid serialization type"},
        {IBMW_RPC_STATUS_SVR_SERIALIZATION_FAILED, "Server side serialization failed"},
        {IBMW_RPC_STATUS_SVR_INVALID_DESERIALIZATION_TYPE, "Server side invalid deserialization type"},
        {IBMW_RPC_STATUS_SVR_DESERIALIZATION_FAILED, "Server side deserialization failed"},
        {IBMW_RPC_STATUS_SVR_HANDLE_FAILED, "Server handle failed"},
        {IBMW_RPC_STATUS_SVR_NOT_READY, "Server not ready"},
        // cli side
        {IBMW_RPC_STATUS_CLI_UNKNOWN, "Client side unknown error"},
        {IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR, "Client side driver internal error"},
        {IBMW_RPC_STATUS_CLI_INVALID_CONTEXT, "Client side invalid context"},
        {IBMW_RPC_STATUS_CLI_INVALID_ADDR, "Client side invalid address"},
        {IBMW_RPC_STATUS_CLI_INVALID_SERIALIZATION_TYPE, "Client side invalid serialization type"},
        {IBMW_RPC_STATUS_CLI_SERIALIZATION_FAILED, "Client side serialization failed"},
        {IBMW_RPC_STATUS_CLI_INVALID_DESERIALIZATION_TYPE, "Client side invalid deserialization type"},
        {IBMW_RPC_STATUS_CLI_DESERIALIZATION_FAILED, "Client side deserialization failed"},
        {IBMW_RPC_STATUS_CLI_NO_DRIVER_TO_HANDLE, "Client side no driver to handle"},
        {IBMW_RPC_STATUS_CLI_SEND_REQ_FAILED, "Client side send req failed"},
        {IBMW_RPC_STATUS_CLI_FUNC_NOT_REGISTERED, "Client side function not registered"},
        {IBMW_RPC_STATUS_CLI_NOT_READY, "Client side not ready"},
    };

    auto finditr = kCodeMsgMap.find(code);
    return (finditr != kCodeMsgMap.end()) ? finditr->second : "Unknown code";
  }

 private:
  uint32_t code_ = ibmw_rpc_status_code_t::IBMW_RPC_STATUS_OK;
};

}  // namespace ibmw::rpc
