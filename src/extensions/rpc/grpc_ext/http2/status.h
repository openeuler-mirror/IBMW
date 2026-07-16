// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>

namespace ibmw::extensions::grpc_extension::http2 {
// The following source code is from trpc-cpp.
// Copied and modified from
// https://github.com/trpc-group/trpc-cpp/blob/v1.2.0/trpc/util/http/status.h

enum class HttpStatus : uint32_t {
  kContinue = 100,
  kSwitchingProtocols = 101,
  kProcessing = 102,
  kEarlyHints = 103,
  kOk = 200,
  kCreated = 201,
  kAccepted = 202,
  kNonAuthoritativeInfo = 203,
  kNoContent = 204,
  kResetContent = 205,
  kPartialContent = 206,
  kMultiStatus = 207,
  kAlreadyReported = 208,
  kIAmUsed = 226,
  kMultipleChoices = 300,
  kMovedPermanently = 301,
  kFound = 302,
  kSeeOther = 303,
  kNotModified = 304,
  kUseProxy = 305,
  kUnused306 = 306,
  kTemporaryRedirect = 307,
  kPermanentRedirect = 308,
  kBadRequest = 400,
  kUnauthorized = 401,
  kPaymentRequired = 402,
  kForbidden = 403,
  kNotFound = 404,
  kMethodNotAllowed = 405,
  kNotAcceptable = 406,
  kProxyAuthRequired = 407,
  kRequestTimeout = 408,
  kConflict = 409,
  kGone = 410,
  kLengthRequired = 411,
  kPreconditionFailed = 412,
  kRequestEntityTooLarge = 413,
  kRequestUriTooLong = 414,
  kUnsupportedMediaType = 415,
  kRequestedRangeNotSatisfiable = 416,
  kExpectationFailed = 417,
  kIAmATeapot = 418,
  kMisdirectedRequest = 421,
  kUnprocessableEntity = 422,
  kLocked = 423,
  kFailedDependency = 424,
  kTooEarly = 425,
  kUpgradeRequired = 426,
  kPreconditionRequired = 428,
  kTooManyRequests = 429,
  kRequestHeaderFieldsTooLarge = 431,
  kUnavailableForLegalReasons = 451,
  kClientClosedRequest = 499,
  kInternalServerError = 500,
  kNotImplemented = 501,
  kBadGateway = 502,
  kServiceUnavailable = 503,
  kGatewayTimeout = 504,
  kHttpVersionNotSupported = 505,
  kVariantAlsoNegotiates = 506,
  kInsufficientStorage = 507,
  kLoopDetected = 508,
  kNotExtended = 510,
  kNetworkAuthenticationRequired = 511,
};
// End of source code from trpc-cpp.

}  // namespace ibmw::extensions::grpc_extension::http2
