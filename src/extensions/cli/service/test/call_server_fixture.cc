// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include <csignal>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include "cpp/service/rpc_handle.h"
#include "engine/engine.h"
#include "engine/runtime_graph/type_support_registry.h"
#include "nlohmann/json.hpp"
namespace { volatile std::sig_atomic_t stop = 0; void Stop(int) { stop = 1; } }
int main(int argc, char** argv) {
  if (argc != 8) return 2;
  try {
    namespace core = ibmw::runtime::core;
    core::runtime_graph::TypeSupportRegistry types;
    types.Load(argv[2]);
    const auto request = types.Find(argv[3]), response = types.Find(argv[4]);
    if (!request || !response) throw std::runtime_error("Missing fixture types.");
    std::ofstream counter(argv[6]);
    std::mutex counter_mutex;
    std::string custom_name = argv[7];
    ibmw::rpc::ServiceFunc handler([&](const ibmw_service_context_base_t*, const void* req, void* rsp, ibmw_function_base_t* callback) {
      uint32_t code = 0;
      try {
        const auto input = nlohmann::json::parse(request.SimpleSerialize("json", req));
        { std::lock_guard lock(counter_mutex); counter << input.dump() << std::endl; }
        std::this_thread::sleep_for(std::chrono::milliseconds(input.value("delay_ms", 0)));
        code = input.value("status", 0u);
        nlohmann::json result;
        if (input.contains("frame_id")) result = {{"code", input.at("sequence_num")}, {"msg", input.at("frame_id")}};
        else result = {{"sum", input.value("a", int64_t{0}) + input.value("b", int64_t{0})}};
        const auto text = result.dump();
        const ibmw_buffer_view_t buffer{.data = text.data(), .len = text.size()};
        if (!response.Deserialize("json", {.data = &buffer, .len = 1}, rsp)) code = IBMW_RPC_STATUS_SVR_HANDLE_FAILED;
      } catch (...) { code = IBMW_RPC_STATUS_SVR_HANDLE_FAILED; }
      const auto* ops = static_cast<const ibmw_function_service_callback_ops_t*>(callback->ops);
      ops->invoker(callback->object_buf, code);
    });
    core::Engine engine;
    engine.RegisterHook(core::After(core::Phase::kInitialize, core::Subsystem::kService), [&] {
      const auto* service = engine.GetServiceDispatcher().GetRpcAccess("call-fixture").NativeHandle();
      if (!service->register_service_func(service->ctx, ibmw::util::ToIbmwStringView(argv[5]),
          custom_name.empty() ? nullptr : custom_name.c_str(), request.NativeHandle(), response.NativeHandle(), handler.NativeHandle()))
        throw std::runtime_error("Cannot register fixture service.");
    });
    engine.Initialize({.cfg_file_path = argv[1]});
    std::signal(SIGINT, Stop); std::signal(SIGTERM, Stop);
    auto running = engine.AsyncStart();
    std::cout << "READY" << std::endl;
    while (!stop) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    engine.Shutdown(); running.get();
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
