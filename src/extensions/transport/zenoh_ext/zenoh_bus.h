// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <future>
#include <string>
#include <unordered_map>

#include "json/json.h"
#include "zenoh.h"
#include "zenoh_ext/global.h"
#include "discovery/zenoh_discovery_responder.h"
#include "discovery/zenoh_service_discovery_responder.h"

namespace ibmw::extensions::zenoh_extension {
class ZenohBus {
 private:
  struct ZenohPubCtx {
    z_owned_publisher_t z_pub;
    bool shm_enabled;
  };
  struct ZenohSubCtx {
    z_owned_subscriber_t z_sub;
    std::future<void> released;  // ready once zenoh dropped the callback
  };

 public:
  using MsgHandleFunc = std::function<void(const z_loaned_sample_t* message)>;
  ZenohBus();
  ~ZenohBus() { Shutdown(); }

  ZenohBus(const ZenohBus&) = delete;
  ZenohBus& operator=(const ZenohBus&) = delete;

  void Initialize(const std::string& native_cfg_path, size_t shm_pool_size);
  void Shutdown();

  // Caller owns the native handle. Its closure owns the callback until Zenoh
  // releases all in-flight references, including after undeclaration.
  bool CreateSubscriber(const std::string& keyexpr, MsgHandleFunc handle,
                        z_owned_subscriber_t& subscriber);
  // Registration is refused after Freeze(); CreateSubscriber is not.
  bool RegisterSubscriber(const std::string& keyexpr, MsgHandleFunc handle);
  // Undeclares a registered subscriber and waits until zenoh released its
  // callback, so whatever the callback uses may be destroyed afterwards. Called
  // from inside a bus callback it cannot wait for that callback: it logs and
  // returns without waiting.
  bool UnregisterSubscriber(const std::string& keyexpr);
  bool RegisterPublisher(const std::string& keyexpr, bool shm_enabled);

  // Ends registration; the drivers call it when they start. Sends read the
  // publisher catalog without a lock, so they may only come from a driver in
  // state Start: by then every registration is done.
  void Freeze() noexcept;

  void RecordDiscoveryEndpoint(ibmw::discovery::EndpointRole role,
                              std::string topic, std::string type, std::string key);
  void StartDiscovery(const std::string& limit_domain);
  void StopDiscovery() noexcept;
  void RecordService(std::string_view name, std::string_view request, std::string_view response) {
    service_discovery_.Catalog().Add(name, request, response);
  }
  void StartServiceDiscovery(std::string_view domain) {
    if (domain.starts_with('/')) domain.remove_prefix(1);
    service_discovery_.Start(z_loan(z_session_), domain);
  }
  void StopServiceDiscovery() noexcept { service_discovery_.Stop(); }

  // Clients of one function share its answer subscriber. A function has one
  // server: a later one is accepted with a warning and never called.
  bool RegisterRpcNode(const std::string& keyexpr, MsgHandleFunc handle, const std::string& role, bool shm_enabled);

  // Only from a driver in state Start, see Freeze().
  void Send(const std::string& topic, char* serialized_data_ptr, uint64_t serialized_data_len);

 public:
  z_owned_shm_provider_t shm_provider_;
  z_alloc_alignment_t alignment_ = {0};
  z_publisher_put_options_t z_pub_options_;  // shared by all puts, set once

  size_t ShmPoolSize() const noexcept { return shm_pool_size_; }

  // Read without the lock: only from a driver in state Start, see Freeze().
  const std::unordered_map<std::string, std::shared_ptr<ZenohPubCtx>>& GetPublisherRegisterMap() const {
    return z_pub_catalog_;
  }

 private:
  static void PrintZenohCgf(z_owned_config_t z_config) {
    z_owned_string_t out_config_string;
    zc_config_to_string(z_loan(z_config), &out_config_string);

    Json::CharReaderBuilder reader_builder;
    Json::Value json_data;

    std::istringstream s(z_string_data(z_loan(out_config_string)));
    Json::parseFromStream(reader_builder, s, &json_data, nullptr);
    Json::StreamWriterBuilder writer_builder;
    writer_builder["indentation"] = "    ";
    std::string pretty_json = Json::writeString(writer_builder, json_data);

    MW_INFO("Using custom zenoh native configuration: {}", pretty_json);

    z_drop(z_move(out_config_string));
  }

  // Undeclares the subscriber and waits for its callback to be released.
  static void DropSubscriber(const std::string& keyexpr, ZenohSubCtx& sub);

  std::unordered_map<std::string, std::shared_ptr<ZenohPubCtx>> z_pub_catalog_;
  std::unordered_map<std::string, ZenohSubCtx> z_sub_catalog_;

  z_owned_session_t z_session_;
  z_owned_config_t z_config_;
  ibmw::discovery::ZenohDiscoveryResponder discovery_;
  ibmw::discovery::ZenohServiceDiscoveryResponder service_discovery_;
  bool initialized_ = false;

  // shm related
  size_t shm_pool_size_ = 0;
  z_owned_memory_layout_t shm_layout_;

  std::atomic_bool shm_initialized_ = false;
  std::atomic_bool frozen_ = false;

  std::mutex z_catalog_mutex_;
};

}  // namespace ibmw::extensions::zenoh_extension
