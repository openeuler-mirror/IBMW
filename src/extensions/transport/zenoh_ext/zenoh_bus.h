// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "json/json.h"
#include "zenoh.h"
#include "zenoh_ext/global.h"
#include "discovery/zenoh_discovery_responder.h"

namespace ibmw::extensions::zenoh_extension {
class ZenohBus {
 private:
  struct ZenohPubCtx {
    z_owned_publisher_t z_pub;
    bool shm_enabled;
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
  bool RegisterSubscriber(const std::string& keyexpr, MsgHandleFunc handle);
  bool RegisterPublisher(const std::string& keyexpr, bool shm_enabled);

  void RecordDiscoveryEndpoint(ibmw::discovery::EndpointRole role,
                              std::string topic, std::string type, std::string key);
  void StartDiscovery(const std::string& limit_domain);
  void StopDiscovery() noexcept;

  void RegisterRpcNode(const std::string& keyexpr, MsgHandleFunc handle, const std::string& role, bool shm_enabled);

  void Send(const std::string& topic, char* serialized_data_ptr, uint64_t serialized_data_len);

 public:
  z_owned_shm_provider_t shm_provider_;
  z_alloc_alignment_t alignment_ = {0};
  z_publisher_put_options_t z_pub_options_;

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

  std::unordered_map<std::string, std::shared_ptr<ZenohPubCtx>> z_pub_catalog_;
  std::unordered_map<std::string, z_owned_subscriber_t> z_sub_catalog_;

  std::vector<std::shared_ptr<MsgHandleFunc>> msg_handle_vec_;

  z_owned_session_t z_session_;
  z_owned_config_t z_config_;
  ibmw::discovery::ZenohDiscoveryResponder discovery_;
  bool initialized_ = false;

  // shm related
  size_t shm_pool_size_ = 0;
  z_owned_memory_layout_t shm_layout_;

  std::atomic_bool shm_initialized_ = false;

  std::mutex z_catalog_mutex_;
};

}  // namespace ibmw::extensions::zenoh_extension
