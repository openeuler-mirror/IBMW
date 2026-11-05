// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "call_client.h"

#include <atomic>
#include <csignal>
#include <future>
#include <iostream>
#include <thread>
#include "base/log_util.h"
#include "cpp/service/rpc_handle.h"
#include "cpp/util/buffer.h"
#include "engine/engine.h"
#include "extension_api/ibmw_service_client_readiness.h"

namespace ibmw::cli::service {
namespace {
volatile std::sig_atomic_t call_signal = 0;
void StopCall(int signal) { call_signal = signal; }
class Signals {
 public:
  Signals() {
    call_signal = 0;
    struct sigaction action{}; action.sa_handler = StopCall; ::sigemptyset(&action.sa_mask);
    if (::sigaction(SIGINT, &action, &interrupt_) != 0) throw std::runtime_error("Cannot install call SIGINT handler.");
    if (::sigaction(SIGTERM, &action, &terminate_) != 0) {
      ::sigaction(SIGINT, &interrupt_, nullptr); throw std::runtime_error("Cannot install call SIGTERM handler.");
    }
  }
  ~Signals() { ::sigaction(SIGINT, &interrupt_, nullptr); ::sigaction(SIGTERM, &terminate_, nullptr); }
 private:
  struct sigaction interrupt_{}, terminate_{};
};
// Own every object referenced by asynchronous drivers until Engine shutdown completes.
struct PendingCall {
  std::shared_ptr<void> request, response;
  ibmw::rpc::Context context;
  std::promise<ibmw::rpc::Status> result;
  std::atomic_bool completed = false;
  void Complete(ibmw::rpc::Status status) { if (!completed.exchange(true)) result.set_value(status); }
};

}  // namespace

CallResult ExecuteCall(CallOptions options, CallTypes types, std::string_view text,
                       std::string_view diagnostic_name) {
  auto pending = std::make_shared<PendingCall>();
  pending->request = types.request.CreateSharedPtr();
  pending->response = types.response.CreateSharedPtr();
  const ibmw_buffer_view_t buffer{.data = text.data(), .len = text.size()};
  if (!pending->request || !pending->response || !types.request.Deserialize("json", {.data = &buffer, .len = 1}, pending->request.get()))
    throw std::invalid_argument("Invalid request fields, values or bounds for '" + std::string(types.request.TypeName()) + "'.");
  {
    ibmw::util::BufferArray buffers;
    if (!types.request.Serialize(types.serialization, pending->request.get(),
                                 buffers.AllocatorNativeHandle(), buffers.BufferArrayNativeHandle()))
      throw std::invalid_argument("Request wire serialization failed.");
  }
  auto extension = options.root["ibmw"]["extensions"]["items"][0];
  if (extension["path"].as<std::string>().empty()) extension["path"] = CallLibrary("libibmw_" + std::string(options.backend == "http" ? "net" : options.backend) + "_transport.so");
  namespace core = ibmw::runtime::core;
  struct LoggerGuard {
    uint32_t previous = ibmw::common::util::SimpleLogger::GetLogLevel();
    LoggerGuard() { ibmw::common::util::SimpleLogger::SetLogLevel(ibmw::common::util::kLogLevelWarn); }
    ~LoggerGuard() { ibmw::common::util::SimpleLogger::SetLogLevel(previous); }
  } logger;
  core::Engine engine;
  ibmw::rpc::ServiceRef client;
  const ibmw::extension::ServiceClientReadinessV1* readiness = nullptr;
  engine.RegisterHook(core::After(core::Phase::kInitialize, core::Subsystem::kService), [&] {
    client = ibmw::rpc::ServiceRef(engine.GetServiceDispatcher().GetRpcAccess("ibmw-service-call").NativeHandle());
    if (!client.RegisterClientFunc(types.function, types.custom_service, types.request.NativeHandle(), types.response.NativeHandle()))
      throw std::runtime_error("Cannot register RPC client with the selected backend.");
    if (engine.GetServiceDispatcher().GetUsedRpcDriver().empty()) throw std::runtime_error("No RPC driver selected.");
    for (const auto* driver : engine.GetServiceDispatcher().GetUsedRpcDriver())
      if (driver->Name() == options.backend) readiness = dynamic_cast<const ibmw::extension::ServiceClientReadinessV1*>(driver);
  });
  engine.Initialize({.config_node = options.root});
  Signals signals;
  auto running = engine.AsyncStart();
  struct Cleanup {
    core::Engine& engine; std::future<void>& running;
    ~Cleanup() { engine.Shutdown(); if (running.valid()) running.wait(); }
  } cleanup{engine, running};
  if (options.backend == "dds" || options.backend == "ros2" || options.backend == "mqtt" || options.backend == "zenoh") {
    if (!readiness) throw std::runtime_error("The selected RPC extension lacks client readiness support; rebuild the extension with this CLI.");
    std::cerr << "Waiting for client readiness via " << options.backend << '\n';
    const auto ready_deadline = std::chrono::steady_clock::now() + options.discovery_timeout;
    while (!call_signal && !readiness->IsClientReady("ibmw-service-call", types.function)) {
      if (std::chrono::steady_clock::now() >= ready_deadline) throw std::runtime_error("Client readiness timed out; no request was sent.");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  if (call_signal) {
    const auto interrupted = call_signal;
    engine.Shutdown(); running.get();
    std::cerr << diagnostic_name << ": interrupted before sending; no request was sent.\n";
    return {128 + interrupted, {}};
  }
  auto result = pending->result.get_future();
  pending->context.SetTimeout(options.timeout);
  pending->context.SetSerializationType(types.serialization);
  std::cerr << "Calling " << options.service << " via " << options.backend << '\n';
  if (!call_signal) client.Call(types.function, pending->context, pending->request.get(), pending->response.get(),
      ibmw::rpc::ClientCallback([pending](uint32_t status) { pending->Complete(ibmw::rpc::Status(status)); }));
  const auto deadline = std::chrono::steady_clock::now() + options.timeout;
  while (!call_signal && result.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) {
    if (std::chrono::steady_clock::now() >= deadline) { pending->Complete(ibmw::rpc::Status(IBMW_RPC_STATUS_TIMEOUT)); break; }
  }
  const int interrupted = call_signal;
  engine.Shutdown(); running.get();
  if (interrupted) { std::cerr << diagnostic_name << ": interrupted; the request may already have executed.\n"; return {128 + interrupted, {}}; }
  const auto status = result.get();
  if (!status.OK()) { std::cerr << diagnostic_name << ": " << status.ToString() << '\n'; return {1, {}}; }
  const auto response = types.response.SimpleSerialize("json", pending->response.get());
  if (response.empty()) throw std::runtime_error("Response JSON serialization failed.");
  return {0, response};
}
}  // namespace ibmw::cli::service
