// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "pub_command.h"

#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include "command_lifecycle.h"
#include "engine/engine.h"
#include "engine/runtime_graph/type_support_registry.h"
#include "message_text.h"
#include "pub_options.h"

namespace ibmw::cli::topic {
namespace {

using Clock = std::chrono::steady_clock;
namespace core = ibmw::runtime::core;

void WaitUntil(Clock::time_point deadline) {
  while (!stop_requested && Clock::now() < deadline)
    std::this_thread::sleep_until(std::min(deadline, Clock::now() + std::chrono::milliseconds(20)));
}

int RunTopicPubCommandImpl(const Invocation& invocation) {
  auto publication = ParsePublicationOptions(invocation);
  const auto& driver = publication.driver;
  const auto& topic = publication.topic;
  const auto& type_name = publication.type_name;
  auto& config = publication.root;
  auto runtime = config["ibmw"];

  const auto packages = invocation.options.find("--type-support");
  if (packages == invocation.options.end()) throw std::invalid_argument("Provide --type-support <library> for the message type.");
  core::runtime_graph::TypeSupportRegistry types;
  for (const auto& package : packages->second) {
    if (package.empty()) throw std::invalid_argument("--type-support requires a nonempty library path.");
    types.Load(package);
    runtime["runtime_graph"]["type_support_pkgs"].push_back(package);
  }
  const auto type = types.Find(type_name);
  if (!type) throw std::invalid_argument("No TypeSupport for '" + type_name + "' in the supplied libraries.");
  if (!type.CheckSerializationTypeSupported("json"))
    throw std::invalid_argument("TypeSupport for '" + type_name + "' does not support JSON message input.");
  const auto documents = YAML::LoadAll(invocation.positional_args.at(2));
  if (documents.size() != 1) throw std::invalid_argument("Message contents must be one JSON/YAML document.");
  const auto text = FromYaml(documents.front()).dump();
  auto object = type.CreateSharedPtr();
  const ibmw_buffer_view_t buffer{.data = text.data(), .len = text.size()};
  if (!object || !type.Deserialize("json", {.data = &buffer, .len = 1}, object.get()))
    throw std::invalid_argument("Invalid fields, values or bounds for message type '" + type_name + "'.");
  // Serialize before starting any transport: input/conversion failures must not create a publisher.
  const auto serialization = std::string(type.DefaultSerializationType());
  const auto bytes = type.SimpleSerialize(serialization, object.get());
  if (bytes.empty()) throw std::runtime_error("Message serialization failed for '" + type_name + "'.");
  if (driver == "dds" && !type_name.starts_with("dds:") && !type_name.starts_with("ros2:"))
    throw std::invalid_argument("DDS publishing requires a dds: or ros2: message type.");

  LoggerLevelGuard logger;
  core::Engine engine;
  auto& bus = engine.GetTransportBus();
  const auto package = types.PackagePath(type_name);
  const std::string module = "ibmw-topic-pub";
  const core::transport::SendTypeWrapper* registered = nullptr;
  // Register before RuntimeGraph snapshots the catalog, just as modules do.
  engine.RegisterHook(core::After(core::Phase::kInitialize, core::Subsystem::kTransport), [&] {
    core::transport::SendTypeWrapper registration;
    registration.info = {.msg_type = type_name, .topic_name = topic, .pkg_path = package,
                         .module_name = module, .msg_type_support_ref = type};
    if (!bus.RegisterSendType(std::move(registration))) throw std::runtime_error("Cannot register publisher.");
    registered = bus.GetTransportCatalog()->GetSendTypeWrapperPtr(type_name, topic, package, module);
    if (!registered || registered->cached_drivers.empty()) throw std::runtime_error("No driver selected for publisher.");
  });
  engine.Initialize({.config_node = config});
  // During transport initialization (e.g. waiting for RouDi), keep the default
  // signal behavior so blocking third-party initialization can be interrupted.
  SignalGuard signals;
  auto running = engine.AsyncStart();
  struct StopGuard {
    core::Engine& engine;
    std::future<void>& running;
    ~StopGuard() { engine.Shutdown(); if (running.valid()) running.wait(); }
  } cleanup{engine, running};
  std::cerr << "Publishing " << topic << " [" << type_name << "] via " << driver << '\n';
  WaitUntil(Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(publication.wait)));
  const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1 / publication.rate));
  auto next = Clock::now();
  uint64_t sent = 0;
  while (!stop_requested && sent < publication.count) {
    ibmw::transport::Context context;
    context.SetSerializationType(serialization);
    core::transport::MessageFrame frame{.info = registered->info, .msg_ptr = object.get(),
                                       .ctx_ref = context, .msg_cache_ptr = object};
    bus.Send(std::move(frame));
    ++sent;
    if (sent == publication.count) break;
    next += period;
    if (next < Clock::now()) next = Clock::now() + period;
    WaitUntil(next);
  }
  if (!stop_requested)
    WaitUntil(Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(publication.keep_alive)));
  std::cerr << "Published " << sent << " message(s).\n";
  engine.Shutdown();
  running.get();
  return 0;
}

}  // namespace

void AddPubCommand(Command& topic) {
  auto& pub = topic.AddSubcommand(
      "pub", "Publish JSON/YAML message contents using the runtime transport");
  pub.SetHandler(RunTopicPubCommandImpl);
  AddPublicationOptions(pub);
}

}  // namespace ibmw::cli::topic
