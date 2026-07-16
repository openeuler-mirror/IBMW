// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#ifndef IBMW_API_CPP_RPC_MARKER_H_
#define IBMW_API_CPP_RPC_MARKER_H_

#include <array>

namespace ibmw::rpc::marker {

// Public RPC marker protocol header for ibmw-gen and user-region tooling.
// These canonical strings are stable so generated RPC code can preserve
// round-trip edit anchors across emitter changes.

// Shared anchor grammar: "// <MARKER>_<EDGE>".
inline constexpr char kAnchorFormat[] = "// {marker}_{edge}";
inline constexpr char kBeginEdge[] = "BEGIN";
inline constexpr char kEndEdge[] = "END";

// Method-scoped rpc_server handler-body marker.
inline constexpr char kRpcServerHandlerBody[] = "IBMW_RPC_SERVER_HANDLER_BODY";
inline constexpr char kRpcServerHandlerBodyBegin[] =
    "// IBMW_RPC_SERVER_HANDLER_BODY_BEGIN";
inline constexpr char kRpcServerHandlerBodyEnd[] =
    "// IBMW_RPC_SERVER_HANDLER_BODY_END";

// rpc_client request-parameter build marker.
inline constexpr char kRpcClientRequestParameterBuild[] =
    "IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD";
inline constexpr char kRpcClientRequestParameterBuildBegin[] =
    "// IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_BEGIN";
inline constexpr char kRpcClientRequestParameterBuildEnd[] =
    "// IBMW_RPC_CLIENT_REQUEST_PARAMETER_BUILD_END";

// rpc_client response-parameter consume marker.
inline constexpr char kRpcClientResponseParameterConsume[] =
    "IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME";
inline constexpr char kRpcClientResponseParameterConsumeBegin[] =
    "// IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_BEGIN";
inline constexpr char kRpcClientResponseParameterConsumeEnd[] =
    "// IBMW_RPC_CLIENT_RESPONSE_PARAMETER_CONSUME_END";

// rpc_client return/status handling marker.
inline constexpr char kRpcClientReturnStatusHandling[] =
    "IBMW_RPC_CLIENT_RETURN_STATUS_HANDLING";
inline constexpr char kRpcClientReturnStatusHandlingBegin[] =
    "// IBMW_RPC_CLIENT_RETURN_STATUS_HANDLING_BEGIN";
inline constexpr char kRpcClientReturnStatusHandlingEnd[] =
    "// IBMW_RPC_CLIENT_RETURN_STATUS_HANDLING_END";

inline constexpr std::array<const char*, 1> kRpcServerMarkers = {
    kRpcServerHandlerBody,
};

inline constexpr std::array<const char*, 3> kRpcClientMarkers = {
    kRpcClientRequestParameterBuild,
    kRpcClientResponseParameterConsume,
    kRpcClientReturnStatusHandling,
};

}  // namespace ibmw::rpc::marker

#endif  // IBMW_API_CPP_RPC_MARKER_H_
