// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <utility>
#include "http_resolver.h"
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include "engine/discovery/service_discovery_protocol.h"

namespace ibmw::discovery {
namespace {
class Provider final : public ServiceDiscoveryProvider {
 public:
  ServiceDiscoverySnapshot Query(const ServiceDiscoveryOptions& options,
      std::chrono::steady_clock::time_point deadline) override {
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace http = beast::http;
    const auto address = service_protocol::ParseServerAddress(options.endpoint, "http");
    const auto endpoints = ResolveHttpEndpoints(address.host, address.port, deadline);
    asio::io_context io;
    beast::tcp_stream stream(io);
    asio::steady_timer timer(io, deadline);
    bool timed_out = false;
    timer.async_wait([&](boost::system::error_code error) {
      if (error) return;
      timed_out = true;
      stream.socket().close(error);
    });
    std::string payload;
    std::exception_ptr failure;
    asio::co_spawn(io, [&]() -> asio::awaitable<std::string> {
      // The timer may already have fired before the coroutine starts. Do not
      // open a new socket after its one-shot cancellation has been consumed.
      if (timed_out || std::chrono::steady_clock::now() >= deadline) {
        timed_out = true;
        throw std::runtime_error("HTTP service discovery timed out.");
      }
      co_await stream.async_connect(endpoints, asio::use_awaitable);
      http::request<http::empty_body> request{http::verb::get, service_protocol::kHttpPath, 11};
      request.set(http::field::host, (address.host.find(':') == std::string::npos
          ? address.host : '[' + address.host + ']') + ':' + address.port);
      request.set(http::field::accept, "application/json");
      co_await http::async_write(stream, request, asio::use_awaitable);
      beast::flat_buffer buffer;
      http::response_parser<http::string_body> parser;
      parser.body_limit(service_protocol::kMaxSnapshotBytes);
      co_await http::async_read(stream, buffer, parser, asio::use_awaitable);
      auto response = parser.release();
      if (response.result() != http::status::ok)
        throw std::runtime_error("HTTP service directory returned " + std::to_string(response.result_int()) +
            "; the target must run an IBMW runtime with service discovery enabled.");
      co_return std::move(response.body());
    }, [&](std::exception_ptr error, std::string value) {
      failure = error; payload = std::move(value); timer.cancel();
      boost::system::error_code ignored; stream.socket().close(ignored);
    });
    io.run();
    if (timed_out) throw std::runtime_error("HTTP service discovery timed out.");
    if (failure) std::rethrow_exception(failure);
    return service_protocol::Decode(payload, DescribeSource(options));
  }
};
}  // namespace
}  // namespace ibmw::discovery

extern "C" IBMW_CORE_EXTENSION_EXPORT ibmw::discovery::ServiceDiscoveryPluginV1 IbmwServiceDiscoveryEntryV1() {
  using namespace ibmw::discovery;
  return {.backend = ServiceDiscoveryBackend::kHttp,
      .create = [] { return static_cast<ServiceDiscoveryProvider*>(new Provider); },
      .destroy = [](ServiceDiscoveryProvider* provider) noexcept { delete provider; }};
}
