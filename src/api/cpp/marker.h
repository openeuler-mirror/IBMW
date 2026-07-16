// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#ifndef IBMW_API_CPP_MARKER_H_
#define IBMW_API_CPP_MARKER_H_

#include <array>

#include "cpp/rpc_marker.h"

namespace ibmw::marker {

// Public marker protocol header for ibmw-gen and user-region tooling.
// This header unifies the marker grammar across all five non-RPC user-
// region kinds (bare app body + 4 transport publish/subscribe variants)
// and re-imports the four pre-existing RPC markers from rpc_marker.h
// so a single header gives generators and round-trip
// editors a complete, canonical marker dictionary.

enum class Kind {
  Bare,
  Publisher,
  Subscriber,
  LoanedPublisher,
  LoanedSubscriber,
  RpcServer,
  RpcClient,
};

enum class Edge { Begin, End };

enum class Path { Header, Cc, MarkerizedTypesupport };

// Shared anchor grammar: "// <MARKER>_<EDGE>".
inline constexpr char kAnchorFormat[] = "// {marker}_{edge}";
inline constexpr char kBeginEdge[] = "BEGIN";
inline constexpr char kEndEdge[] = "END";

// Bare-app user-region body marker (generators/app_gen.py).
inline constexpr char kPubsubBareBody[] = "IBMW_PUBSUB_BARE_BODY";
inline constexpr char kPubsubBareBodyBegin[] =
    "// IBMW_PUBSUB_BARE_BODY_BEGIN";
inline constexpr char kPubsubBareBodyEnd[] =
    "// IBMW_PUBSUB_BARE_BODY_END";

// Publisher user-region body marker (transports/{dds,ros2,json,iceoryx}.py).
inline constexpr char kPubsubPublisherBody[] = "IBMW_PUBSUB_PUBLISHER_BODY";
inline constexpr char kPubsubPublisherBodyBegin[] =
    "// IBMW_PUBSUB_PUBLISHER_BODY_BEGIN";
inline constexpr char kPubsubPublisherBodyEnd[] =
    "// IBMW_PUBSUB_PUBLISHER_BODY_END";

// Subscriber user-region body marker (transports/{dds,ros2,json,iceoryx}.py).
inline constexpr char kPubsubSubscriberBody[] = "IBMW_PUBSUB_SUBSCRIBER_BODY";
inline constexpr char kPubsubSubscriberBodyBegin[] =
    "// IBMW_PUBSUB_SUBSCRIBER_BODY_BEGIN";
inline constexpr char kPubsubSubscriberBodyEnd[] =
    "// IBMW_PUBSUB_SUBSCRIBER_BODY_END";

// LoanedPublisher user-region body marker (zero-copy loaned variants).
inline constexpr char kPubsubLoanedPublisherBody[] =
    "IBMW_PUBSUB_LOANED_PUBLISHER_BODY";
inline constexpr char kPubsubLoanedPublisherBodyBegin[] =
    "// IBMW_PUBSUB_LOANED_PUBLISHER_BODY_BEGIN";
inline constexpr char kPubsubLoanedPublisherBodyEnd[] =
    "// IBMW_PUBSUB_LOANED_PUBLISHER_BODY_END";

// LoanedSubscriber user-region body marker (zero-copy loaned variants).
inline constexpr char kPubsubLoanedSubscriberBody[] =
    "IBMW_PUBSUB_LOANED_SUBSCRIBER_BODY";
inline constexpr char kPubsubLoanedSubscriberBodyBegin[] =
    "// IBMW_PUBSUB_LOANED_SUBSCRIBER_BODY_BEGIN";
inline constexpr char kPubsubLoanedSubscriberBodyEnd[] =
    "// IBMW_PUBSUB_LOANED_SUBSCRIBER_BODY_END";

inline constexpr std::array<const char*, 5> kPubsubMarkers = {
    kPubsubBareBody,
    kPubsubPublisherBody,
    kPubsubSubscriberBody,
    kPubsubLoanedPublisherBody,
    kPubsubLoanedSubscriberBody,
};

// Re-export the four pre-existing RPC markers from rpc_marker.h so callers
// get the full canonical dictionary
// from a single include.  The names are aliases — the source of truth
// stays in ::ibmw::rpc::marker so downstream code remains unchanged.
inline constexpr const char* kRpcServerHandlerBody =
    ::ibmw::rpc::marker::kRpcServerHandlerBody;
inline constexpr const char* kRpcClientRequestParameterBuild =
    ::ibmw::rpc::marker::kRpcClientRequestParameterBuild;
inline constexpr const char* kRpcClientResponseParameterConsume =
    ::ibmw::rpc::marker::kRpcClientResponseParameterConsume;
inline constexpr const char* kRpcClientReturnStatusHandling =
    ::ibmw::rpc::marker::kRpcClientReturnStatusHandling;

}  // namespace ibmw::marker

#endif  // IBMW_API_CPP_MARKER_H_
