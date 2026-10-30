// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <string_view>

#include "c/service/rpc_status_base.h"

namespace ibmw::extensions::grpc_extension::grpc {

// Status mapping for failed calls, see https://grpc.io/docs/guides/status-codes/.

// The "grpc-status" a server sends for a failed ibmw rpc status.
inline const char* ToGrpcStatus(uint32_t code) noexcept {
  switch (code) {
    case IBMW_RPC_STATUS_TIMEOUT:
      return "4";  // DEADLINE_EXCEEDED
    case IBMW_RPC_STATUS_SVR_NOT_IMPLEMENTED:
    case IBMW_RPC_STATUS_SVR_NOT_FOUND:
      return "12";  // UNIMPLEMENTED
    case IBMW_RPC_STATUS_SVR_DRIVER_INTERNAL_ERROR:
    case IBMW_RPC_STATUS_SVR_SERIALIZATION_FAILED:
      return "13";  // INTERNAL
    case IBMW_RPC_STATUS_SVR_NOT_READY:
      return "14";  // UNAVAILABLE
    default:
      return "2";  // UNKNOWN
  }
}

// The ibmw rpc status a client reports for a non-zero "grpc-status".
inline uint32_t FromGrpcStatus(std::string_view grpc_status) noexcept {
  if (grpc_status == "4") return IBMW_RPC_STATUS_TIMEOUT;
  if (grpc_status == "12") return IBMW_RPC_STATUS_SVR_NOT_IMPLEMENTED;
  if (grpc_status == "13") return IBMW_RPC_STATUS_SVR_DRIVER_INTERNAL_ERROR;
  if (grpc_status == "14") return IBMW_RPC_STATUS_SVR_NOT_READY;
  return IBMW_RPC_STATUS_CLI_UNKNOWN;
}

}  // namespace ibmw::extensions::grpc_extension::grpc
