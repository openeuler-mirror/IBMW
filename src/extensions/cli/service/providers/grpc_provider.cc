// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "grpc_ext/client/client.h"
#include "engine/discovery/service_discovery_protocol.h"

namespace ibmw::discovery {
namespace {
class Provider final : public ServiceDiscoveryProvider {
 public:
  ServiceDiscoverySnapshot Query(const ServiceDiscoveryOptions& options,
      std::chrono::steady_clock::time_point deadline) override {
    namespace asio = boost::asio;
    namespace grpc = ibmw::extensions::grpc_extension;
    const auto address = service_protocol::ParseServerAddress(options.endpoint, "grpc");
    auto io = std::make_shared<asio::io_context>();
    auto client = std::make_shared<grpc::client::AsioHttp2Client>(io);
    client->Initialize({.host = address.host, .service = address.port}); client->Start();
    asio::steady_timer timer(*io, deadline);
    bool timed_out = false;
    timer.async_wait([&](boost::system::error_code error) {
      if (!error) { timed_out = true; client->Shutdown(); }
    });
    std::string payload;
    std::exception_ptr failure;
    asio::co_spawn(*io, [&]() -> asio::awaitable<std::string> {
      auto request = std::make_shared<grpc::http2::Request>();
      request->SetUrl({.protocol = "http", .host = address.host, .service = address.port,
                       .path = service_protocol::kGrpcPath});
      request->SetMethod("POST");
      request->AddHeader("content-type", "application/grpc+json");
      request->AddHeader("te", "trailers");
      request->Write(service_protocol::GrpcFrame("{}"));
      const auto remaining = deadline - std::chrono::steady_clock::now();
      if (remaining <= std::chrono::steady_clock::duration::zero()) throw std::runtime_error("gRPC discovery timed out.");
      auto response = co_await client->HttpSendRecvCo(request, remaining);
      if (response->GetHttpStatus() != grpc::http2::HttpStatus::kOk)
        throw std::runtime_error("gRPC service directory is unavailable; upgrade the target IBMW runtime.");
      const auto found = response->GetTrailers().find("grpc-status");
      if (found == response->GetTrailers().end() || found->second != "0")
        throw std::runtime_error("gRPC service directory returned an error status.");
      co_return std::string(service_protocol::GrpcPayload(response->GetBody().GetStringView()));
    }, [&](std::exception_ptr error, std::string value) {
      failure = error; payload = std::move(value); timer.cancel(); client->Shutdown();
    });
    io->run();
    if (timed_out) throw std::runtime_error("gRPC service discovery timed out.");
    if (failure) std::rethrow_exception(failure);
    return service_protocol::Decode(payload, DescribeSource(options));
  }
};
}  // namespace
}  // namespace ibmw::discovery

extern "C" IBMW_CORE_EXTENSION_EXPORT ibmw::discovery::ServiceDiscoveryPluginV1 IbmwServiceDiscoveryEntryV1() {
  using namespace ibmw::discovery;
  return {.backend = ServiceDiscoveryBackend::kGrpc,
      .create = [] { return static_cast<ServiceDiscoveryProvider*>(new Provider); },
      .destroy = [](ServiceDiscoveryProvider* provider) noexcept { delete provider; }};
}
