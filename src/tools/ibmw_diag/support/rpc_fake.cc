// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "support/rpc_fake.h"

#include <stdexcept>
#include <utility>

namespace ibmw::diag {
RpcFake& Rpc() {
  static RpcFake fake;
  return fake;
}
}  // namespace ibmw::diag

namespace ibmw::cli::service {
CallResult ExecuteCall(CallOptions options, CallTypes types, std::string_view request,
                       std::string_view diagnostic) {
  auto& fake = diag::Rpc();
  fake.requests.push_back({options.backend, types.function, types.serialization,
                           std::string(diagnostic), nlohmann::json::parse(request),
                           options.timeout, options.discovery_timeout});
  if (fake.replies.empty()) {
    ADD_FAILURE() << "Unexpected RPC: " << types.function;
    throw std::logic_error("No fake RPC response configured");
  }
  auto result = std::move(fake.replies.front());
  fake.replies.pop_front();
  return result;
}
}  // namespace ibmw::cli::service
