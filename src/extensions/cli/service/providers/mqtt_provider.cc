// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <MQTTAsync.h>
#include <algorithm>
#include <condition_variable>
#include <map>
#include <mutex>
#include "engine/discovery/service_discovery_protocol.h"

namespace ibmw::discovery {
namespace {
using namespace std::chrono_literals;
class Provider final : public ServiceDiscoveryProvider {
  struct State {
    std::mutex mutex;
    std::condition_variable changed;
    bool completed = false;
    std::string error;
    ServiceDiscoverySource source;
    std::map<std::string, ServiceDiscoverySnapshot> snapshots;
  };
  struct Client {
    MQTTAsync value = nullptr;
    ~Client() {
      if (!value) return;
      MQTTAsync_disconnectOptions options = MQTTAsync_disconnectOptions_initializer;
      options.timeout = 0;
      MQTTAsync_disconnect(value, &options);
      MQTTAsync_destroy(&value);
    }
  };
  static void Success(void* context, MQTTAsync_successData*) {
    auto& state = *static_cast<State*>(context);
    std::lock_guard lock(state.mutex);
    state.completed = true; state.changed.notify_all();
  }
  static void Failure(void* context, MQTTAsync_failureData* data) {
    auto& state = *static_cast<State*>(context);
    std::lock_guard lock(state.mutex);
    state.error = "MQTT discovery operation failed (" + std::to_string(data ? data->code : -1) + ").";
    state.changed.notify_all();
  }
  static void Lost(void* context, char*) {
    auto& state = *static_cast<State*>(context);
    std::lock_guard lock(state.mutex);
    state.error = "MQTT broker connection was lost."; state.changed.notify_all();
  }
  static int Arrived(void* context, char* topic, int, MQTTAsync_message* message) {
    auto& state = *static_cast<State*>(context);
    try {
      if (message->retained) throw std::runtime_error("Unexpected retained MQTT service directory.");
      auto snapshot = service_protocol::Decode(
          {static_cast<const char*>(message->payload), static_cast<size_t>(message->payloadlen)}, state.source);
      if (!snapshot.services.empty()) {
        const auto instance = snapshot.services.front().runtime_id;
        std::lock_guard lock(state.mutex);
        state.snapshots.insert_or_assign(instance, std::move(snapshot));
      }
    } catch (const std::exception& error) {
      std::lock_guard lock(state.mutex); state.error = error.what();
    }
    state.changed.notify_all();
    MQTTAsync_freeMessage(&message); MQTTAsync_free(topic);
    return 1;
  }
  static void Check(int code) {
    if (code != MQTTASYNC_SUCCESS) throw std::runtime_error("MQTT discovery operation failed (" + std::to_string(code) + ").");
  }
  static void Wait(State& state, std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(state.mutex);
    if (!state.changed.wait_until(lock, deadline, [&] { return state.completed || !state.error.empty(); }))
      throw std::runtime_error("MQTT discovery timed out.");
    if (!state.error.empty()) throw std::runtime_error(state.error);
    state.completed = false;
  }
 public:
  ServiceDiscoverySnapshot Query(const ServiceDiscoveryOptions& options,
      std::chrono::steady_clock::time_point deadline) override {
    if (!options.endpoint.starts_with("tcp://") && !options.endpoint.starts_with("ssl://"))
      throw std::invalid_argument("MQTT broker must use tcp://host:port or ssl://host:port.");
    const bool tls = options.endpoint.starts_with("ssl://");
    const auto& tls_options = options.mqtt_tls;
    if (tls_options.client_cert.empty() != tls_options.client_key.empty())
      throw std::invalid_argument("MQTT client certificate and key must be supplied together.");
    if (!tls && !tls_options.Empty())
      throw std::invalid_argument("MQTT TLS credentials require an ssl:// broker.");
    service_protocol::ParseServerAddress(options.endpoint, tls ? "ssl" : "tcp");
    State state;
    state.source = DescribeSource(options);
    Client client; // Destroy callbacks before their State.
    const auto id = "ibmw-service-" + service_protocol::NewInstanceId();
    const auto reply_topic = std::string(service_protocol::kMqttReplyPrefix) + service_protocol::NewInstanceId();
    Check(MQTTAsync_create(&client.value, options.endpoint.c_str(), id.c_str(), MQTTCLIENT_PERSISTENCE_NONE, nullptr));
    Check(MQTTAsync_setCallbacks(client.value, &state, Lost, Arrived, nullptr));
    MQTTAsync_connectOptions connect = MQTTAsync_connectOptions_initializer;
    MQTTAsync_SSLOptions ssl = MQTTAsync_SSLOptions_initializer;
    ssl.enableServerCertAuth = 1;
    ssl.verify = 1;
    ssl.trustStore = tls_options.truststore.empty() ? nullptr : tls_options.truststore.c_str();
    ssl.keyStore = tls_options.client_cert.empty() ? nullptr : tls_options.client_cert.c_str();
    ssl.privateKey = tls_options.client_key.empty() ? nullptr : tls_options.client_key.c_str();
    connect.ssl = tls ? &ssl : nullptr;
    connect.context = &state; connect.onSuccess = Success; connect.onFailure = Failure;
    connect.cleansession = 1; connect.keepAliveInterval = 10;
    connect.connectTimeout = 1 + std::max<int64_t>(0, std::chrono::duration_cast<std::chrono::seconds>(deadline - std::chrono::steady_clock::now()).count());
    Check(MQTTAsync_connect(client.value, &connect)); Wait(state, deadline);
    MQTTAsync_responseOptions subscribe = MQTTAsync_responseOptions_initializer;
    subscribe.context = &state; subscribe.onSuccess = Success; subscribe.onFailure = Failure;
    Check(MQTTAsync_subscribe(client.value, reply_topic.c_str(), 1, &subscribe)); Wait(state, deadline);
    while (std::chrono::steady_clock::now() < deadline) {
      MQTTAsync_message request = MQTTAsync_message_initializer;
      request.payload = const_cast<char*>(reply_topic.data()); request.payloadlen = static_cast<int>(reply_topic.size());
      request.qos = 1;
      Check(MQTTAsync_sendMessage(client.value, service_protocol::kMqttRequestTopic, &request, nullptr));
      std::unique_lock lock(state.mutex);
      state.changed.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + 200ms), [&] { return !state.error.empty(); });
      if (!state.error.empty()) break;
    }
    std::lock_guard lock(state.mutex);
    ServiceDiscoverySnapshot result;
    for (auto& [id, snapshot] : state.snapshots)
      result.services.insert(result.services.end(), snapshot.services.begin(), snapshot.services.end());
    result.sources.push_back({.source = state.source,
        .status = state.error.empty() ? DiscoveryStatus::kReady : DiscoveryStatus::kError, .message = state.error});
    return result;
  }
};
}  // namespace
}  // namespace ibmw::discovery

extern "C" IBMW_CORE_EXTENSION_EXPORT ibmw::discovery::ServiceDiscoveryPluginV1 IbmwServiceDiscoveryEntryV1() {
  using namespace ibmw::discovery;
  return {.backend = ServiceDiscoveryBackend::kMqtt,
      .create = [] { return static_cast<ServiceDiscoveryProvider*>(new Provider); },
      .destroy = [](ServiceDiscoveryProvider* provider) noexcept { delete provider; }};
}
