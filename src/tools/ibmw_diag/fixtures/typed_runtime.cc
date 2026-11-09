// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// A real application using production type packages and transport/service APIs.
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

#include "cpp/module_base.h"
#include "cpp/service/rpc_handle.h"
#include "engine/engine.h"
#include "engine/runtime_graph/type_support_registry.h"
#include "nlohmann/json.hpp"

namespace {
using namespace std::chrono_literals;
using Json = nlohmann::json;
volatile std::sig_atomic_t stopped = 0;
void Stop(int) { stopped = 1; }

class Application final : public ibmw::Module {
 public:
  Application(Json spec, std::filesystem::path directory)
      : spec_(std::move(spec)), directory_(std::move(directory)) {
    types_.Load(spec_.at("types").get<std::string>());
    message_ = types_.Find(spec_.at("message_type").get<std::string>());
    if (!message_) throw std::runtime_error("Application message type is unavailable");
    value_ = message_.CreateSharedPtr();
    Decode(message_, spec_.at("payload"), value_.get());
  }
  ibmw::ModuleDescriptor Info() const override { return {.module_name = "NetworkFixture"}; }
  void Record(const char* file, const Json& value) {
    std::lock_guard lock(mutex_);
    std::ofstream(directory_ / file, std::ios::app) << value.dump() << '\n';
  }
  bool Setup(ibmw::Runtime runtime) override {
    auto transport = runtime.GetTransport();
    if (spec_.value("publish", true)) {
      publisher_ = transport.GetPublisher(spec_.value("publish_topic", "/diag/data"));
      if (!publisher_.RegisterSendType(message_.NativeHandle())) return false;
    }
    if (spec_.value("subscribe", true)) {
      auto subscriber = transport.GetSubscriber(spec_.value("subscribe_topic", "/diag/input"));
      if (!subscriber.OnMessage(message_.NativeHandle(), ibmw::transport::MessageCallback(
          [this](const ibmw_transport_context_base_t*, const void* message,
                 ibmw_function_base_t* release) {
            ibmw::transport::SubscriberReleaseCallback release_message(release);
            try {
              Record("received.jsonl", Json::parse(message_.SimpleSerialize("json", message)));
            } catch (const std::exception& error) {
              Record("errors.jsonl", {{"error", error.what()}});
            }
            release_message();
          }))) return false;
    }
    if (spec_.contains("service")) {
      request_ = types_.Find(spec_.at("request_type").get<std::string>());
      response_ = types_.Find(spec_.at("response_type").get<std::string>());
      if (!request_ || !response_) return false;
      handler_ = ibmw::rpc::ServiceFunc(
          [this](const ibmw_service_context_base_t*, const void* request, void* response,
                 ibmw_function_base_t* callback) {
            uint32_t code = 0;
            try {
              const auto input = Json::parse(request_.SimpleSerialize("json", request));
              Record("rpc.jsonl", input);
              const bool header = input.contains("frame_id");
              const auto marker = header ? input.at("frame_id").get<std::string>() : "";
              const auto delay = header ? (marker == "delay" ? 800 : 0) : input.value("delay_ms", 0);
              std::this_thread::sleep_for(std::chrono::milliseconds(delay));
              code = header ? (marker == "fail" ? 1008u : 0u) : input.value("status", 0u);
              const Json output = header
                  ? Json{{"code", input.at("sequence_num")}, {"msg", marker}}
                  : Json{{"sum", input.at("a").get<int64_t>() + input.at("b").get<int64_t>()}};
              Decode(response_, output, response);
              Record("rpc-done.jsonl", input);
            } catch (const std::exception& error) {
              Record("errors.jsonl", {{"error", error.what()}});
              code = IBMW_RPC_STATUS_SVR_HANDLE_FAILED;
            }
            const auto* ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
            ops->invoker(callback->object_buf, code);
          });
      const auto* service = runtime.GetService().NativeHandle();
      const auto name = spec_.at("service").get<std::string>();
      if (!service->register_service_func(service->ctx, ibmw::util::ToIbmwStringView(name), nullptr,
          request_.NativeHandle(), response_.NativeHandle(), handler_.NativeHandle())) return false;
    }
    return true;
  }
  bool Run() override { return true; }
  void Stop() override {}
  void Publish() {
    if (!publisher_) return;
    ibmw::transport::Context context;
    const auto serialization = spec_.value("serialization", "json");
    context.SetSerializationType(serialization);
    publisher_.Send(message_.TypeName(), context, value_.get());
    Record("sent.jsonl", {{"time_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()},
        {"bytes", message_.SimpleSerialize(serialization, value_.get()).size()}});
  }
 private:
  static void Decode(ibmw::util::TypeSupportRef type, const Json& value, void* output) {
    const auto text = value.dump();
    const ibmw_buffer_view_t buffer{.data = text.data(), .len = text.size()};
    if (!type.Deserialize("json", {.data = &buffer, .len = 1}, output))
      throw std::runtime_error("Application JSON could not be decoded");
  }
  Json spec_;
  std::filesystem::path directory_;
  std::mutex mutex_;
  ibmw::runtime::core::runtime_graph::TypeSupportRegistry types_;
  ibmw::util::TypeSupportRef message_, request_, response_;
  std::shared_ptr<void> value_;
  ibmw::transport::SenderRef publisher_;
  ibmw::rpc::ServiceFunc handler_;
};
}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  try {
    namespace core = ibmw::runtime::core;
    Application application(Json::parse(std::ifstream(argv[2])), argv[3]);
    core::Engine engine;
    engine.GetModuleBus().RegisterModule("diag-network", application.NativeHandle());
    std::atomic_bool ready = false;
    engine.RegisterHook(core::AfterPhase(core::Phase::kStart), [&] { ready = true; });
    engine.Initialize({.cfg_file_path = argv[1]});
    std::signal(SIGINT, Stop);
    std::signal(SIGTERM, Stop);
    auto running = engine.AsyncStart();
    while (!ready && running.wait_for(10ms) != std::future_status::ready) {}
    if (!ready) { running.get(); throw std::runtime_error("Application did not start"); }
    std::cout << "READY" << std::endl;
    auto next = std::chrono::steady_clock::now();
    while (!stopped) {
      application.Publish();
      next += 40ms;
      std::this_thread::sleep_until(next);
    }
    engine.Shutdown();
    running.get();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
