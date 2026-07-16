// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string_view>

namespace ibmw::extensions::opentelemetry_extension {

static constexpr std::string_view kCtxKeyPrefix = "ibmw_otp-";

static constexpr std::string_view kCtxKeyStartNewTrace = "ibmw_otp-start_new_trace";
static constexpr std::string_view kCtxKeyTraceParent = "ibmw_otp-traceparent";
static constexpr std::string_view kCtxKeyTraceState = "ibmw_otp-tracestate";

}  // namespace ibmw::extensions::opentelemetry_extension
