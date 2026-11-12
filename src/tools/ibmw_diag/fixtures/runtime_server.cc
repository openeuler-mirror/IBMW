// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Real managed modules. Side-channel journals record effects independently of CLI output.
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

#include "messages.h"
#include "cpp/module_base.h"
#include "cpp/util/json_rpc_helpers.h"
#include "cpp/util/json_transport.h"
#include "engine/engine.h"

namespace {
using namespace std::chrono_literals;
using namespace ibmw::diag::fixture;
volatile std::sig_atomic_t stopped = 0;
void Stop(int) { stopped = 1; }

class Journal {
 public:
  explicit Journal(std::filesystem::path directory) : directory_(std::move(directory)) {}
  void Append(const char* file, const nlohmann::json& value) {
    std::lock_guard lock(mutex_);
    std::ofstream(directory_ / file, std::ios::app) << value.dump() << '\n';
  }
  void Snapshot(const nlohmann::json& value) {
    const auto pending = directory_ / "observed.pending";
    std::ofstream(pending) << value.dump();
    std::filesystem::rename(pending, directory_ / "observed.json");
  }
 private:
  std::filesystem::path directory_;
  std::mutex mutex_;
};

class TestModule final : public ibmw::Module {
 public:
  TestModule(std::string name, Journal& journal, bool orphan)
      : name_(std::move(name)), journal_(journal), orphan_(orphan) {}
  ibmw::ModuleDescriptor Info() const override { return {.module_name = name_}; }
  bool Setup(ibmw::Runtime runtime) override {
    journal_.Append("lifecycle.jsonl", {{"module", name_}, {"phase", "setup"}});
    params_ = runtime.GetParams();
    logger_ = runtime.GetLogger();
    params_.SetParameter("speed", name_ == "Producer" ? "0.2" : "7");
    params_.SetParameter("controller.gain", "2");
    auto transport = runtime.GetTransport();
    if (name_ == "Peer") {
      return ibmw::json::transport::OnMessage<Message>(transport.GetSubscriber("/diag/data"),
          [this](const std::shared_ptr<const Message>& message) {
            journal_.Append("local-received.jsonl", *message);
          });
    }
    publisher_ = transport.GetPublisher("/diag/data");
    if (!ibmw::json::transport::RegisterSendType<Message>(publisher_)) return false;
    if (orphan_) {
      auto orphan = transport.GetPublisher("/diag/orphan");
      if (!ibmw::json::transport::RegisterSendType<Message>(orphan)) return false;
    }
    if (!ibmw::json::transport::OnMessage<Message>(transport.GetSubscriber("/diag/input"),
        [this](const std::shared_ptr<const Message>& message) {
          journal_.Append("received.jsonl", *message);
        })) return false;
    service_ = std::make_unique<ibmw::rpc::ServiceBase>("json", "diag/math");
    ibmw::json::rpc::RegisterServiceFunc<SumRequest, SumResponse>(*service_, "Sum",
        [this](const SumRequest& request, SumResponse& response) -> uint32_t {
          journal_.Append("rpc.jsonl", request);
          std::this_thread::sleep_for(std::chrono::milliseconds(request.delay_ms));
          response.sum = request.a + request.b;
          journal_.Append("rpc-done.jsonl", request);
          return request.status;
        });
    return runtime.GetService().RegisterService(service_.get());
  }
  bool Run() override {
    journal_.Append("lifecycle.jsonl", {{"module", name_}, {"phase", "run"}});
    return true;
  }
  void Stop() override {
    journal_.Append("lifecycle.jsonl", {{"module", name_}, {"phase", "stop"}});
  }
  void Publish() {
    ibmw::json::transport::Send(publisher_, Message{42});
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    journal_.Append("sent.jsonl", {{"time_ns", std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()},
                                 {"bytes", nlohmann::json(Message{42}).dump().size()}});
  }
  std::string Speed() const { return params_.GetParameter("speed"); }
  void Log(unsigned level, const std::string& message) {
    logger_.Log(level, __LINE__, __FILE__, __func__, message.data(), message.size());
  }
 private:
  std::string name_;
  Journal& journal_;
  bool orphan_;
  ibmw::parameter::ParamsRef params_;
  ibmw::logger::LoggerRef logger_;
  ibmw::transport::SenderRef publisher_;
  std::unique_ptr<ibmw::rpc::ServiceBase> service_;
};
}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) return 2;
  try {
    namespace core = ibmw::runtime::core;
    const std::filesystem::path directory = argv[2];
    Journal journal(directory);
    TestModule producer("Producer", journal, std::string(argv[3]) == "orphan");
    TestModule peer("Peer", journal, false);
    core::Engine engine;
    engine.GetModuleBus().RegisterModule("diag-fixture", producer.NativeHandle());
    engine.GetModuleBus().RegisterModule("diag-fixture", peer.NativeHandle());
    std::atomic_bool ready = false;
    engine.RegisterHook(core::AfterPhase(core::Phase::kStart), [&] { ready = true; });
    engine.Initialize({.cfg_file_path = argv[1]});
    std::signal(SIGINT, Stop);
    std::signal(SIGTERM, Stop);
    auto running = engine.AsyncStart();
    while (!ready && running.wait_for(10ms) != std::future_status::ready) {}
    if (!ready) { running.get(); throw std::runtime_error("Engine did not start."); }
    std::cout << "READY" << std::endl;
    auto next = std::chrono::steady_clock::now();
    std::string previous_probe;
    while (!stopped) {
      producer.Publish();
      journal.Snapshot({{"producer_speed", producer.Speed()}, {"peer_speed", peer.Speed()}});
      std::string probe;
      std::getline(std::ifstream(directory / "probe.txt"), probe);
      if (!probe.empty() && probe != previous_probe) {
        for (unsigned level = 0; level < IBMW_LOG_LEVEL_OFF; ++level)
          producer.Log(level, probe + ":" + std::to_string(level));
        peer.Log(IBMW_LOG_LEVEL_FATAL, probe + ":END");
        previous_probe = probe;
      }
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
