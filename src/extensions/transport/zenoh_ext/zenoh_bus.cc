// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "zenoh_ext/zenoh_bus.h"

#include <utility>
#include <stdexcept>

namespace ibmw::extensions::zenoh_extension {

ZenohBus::ZenohBus() {
  z_internal_null(&z_session_);
  z_internal_null(&z_config_);
  z_internal_null(&shm_provider_);
  z_internal_null(&shm_layout_);
  z_publisher_put_options_default(&z_pub_options_);
}

void ZenohBus::Initialize(const std::string &native_cfg_path, size_t shm_pool_size) {
  if (initialized_) throw std::logic_error("Zenoh bus is already initialized.");
  if (!native_cfg_path.empty()) {
    if (zc_config_from_file(&z_config_, native_cfg_path.c_str()) != Z_OK) {
      MW_ERROR("Unable to load configuration file: {}", native_cfg_path);
      throw std::runtime_error("Unable to load Zenoh configuration: " + native_cfg_path);
    }
    PrintZenohCgf(z_config_);
  } else {
    z_config_default(&z_config_);
  }

  if (z_open(&z_session_, z_move(z_config_), NULL) < 0) {
    MW_ERROR("Unable to open zenoh session!");
    throw std::runtime_error("Unable to open Zenoh session.");
  }

  shm_pool_size_ = shm_pool_size;
  initialized_ = true;
}

void ZenohBus::Shutdown() {
  StopServiceDiscovery();
  StopDiscovery();
  for (auto &ptr : z_pub_catalog_) {
    z_drop(z_move(ptr.second->z_pub));
  }

  for (auto &ptr : z_sub_catalog_) {
    z_drop(z_move(ptr.second));
  }

  if (initialized_) z_close(z_loan_mut(z_session_), nullptr);
  msg_handle_vec_.clear();
  z_pub_catalog_.clear();
  z_sub_catalog_.clear();

  z_drop(z_move(z_session_));
  z_drop(z_move(z_config_));
  z_drop(z_move(shm_provider_));
  z_drop(z_move(shm_layout_));
  shm_initialized_ = false;
  frozen_ = false;
  initialized_ = false;
  discovery_.Clear();
}

void ZenohBus::Freeze() noexcept {
  std::lock_guard lock(z_catalog_mutex_);
  frozen_.store(true);
}

bool ZenohBus::RegisterPublisher(const std::string &keyexpr, bool shm_enabled) {
  if (!initialized_) return false;
  std::lock_guard<std::mutex> lock(z_catalog_mutex_);
  if (frozen_.load()) [[unlikely]] {
    MW_ERROR("Publisher {} is registered after the zenoh bus started.", keyexpr);
    return false;
  }
  if (z_pub_catalog_.contains(keyexpr)) return true;
  z_view_keyexpr_t key;
  if (z_view_keyexpr_from_str(&key, keyexpr.c_str()) < 0) return false;

  z_owned_publisher_t z_pub;
  if (z_declare_publisher(z_loan(z_session_), &z_pub, z_loan(key), nullptr) < 0) {
    MW_ERROR("Unable to declare Publisher!");
    return false;
  }

  z_pub_catalog_.emplace(keyexpr, std::make_shared<ZenohPubCtx>(ZenohPubCtx{z_pub, shm_enabled}));
  MW_TRACE("Publisher with keyexpr: {} registered successfully.", keyexpr.c_str());

  // Create shared memory provider
  if (!shm_initialized_.load() && shm_enabled) {
    z_memory_layout_new(&shm_layout_, shm_pool_size_, alignment_);
    z_posix_shm_provider_new(&shm_provider_, z_loan(shm_layout_));
    shm_initialized_.store(true);
  }
  return true;
}

bool ZenohBus::CreateSubscriber(const std::string& keyexpr, MsgHandleFunc handle,
                                z_owned_subscriber_t& subscriber) {
  std::lock_guard lock(z_catalog_mutex_);
  if (!initialized_) return false;
  z_view_keyexpr_t key;
  if (z_view_keyexpr_from_str(&key, keyexpr.c_str()) < 0) return false;
  z_owned_closure_sample_t closure;
  z_closure(&closure,
            [](z_loaned_sample_t* sample, void* context) { (*static_cast<MsgHandleFunc*>(context))(sample); },
            [](void* context) { delete static_cast<MsgHandleFunc*>(context); },
            new MsgHandleFunc(std::move(handle)));
  return z_declare_subscriber(z_loan(z_session_), &subscriber, z_loan(key), z_move(closure), nullptr) >= 0;
}

bool ZenohBus::RegisterSubscriber(const std::string &keyexpr, MsgHandleFunc handle) {
  if (!initialized_) return false;
  std::lock_guard<std::mutex> lock(z_catalog_mutex_);
  if (frozen_.load()) [[unlikely]] {
    MW_ERROR("Subscriber {} is registered after the zenoh bus started.", keyexpr);
    return false;
  }
  if (z_sub_catalog_.contains(keyexpr)) return false;
  z_view_keyexpr_t key;
  if (z_view_keyexpr_from_str(&key, keyexpr.c_str()) < 0) return false;
  z_owned_closure_sample_t z_callback;
  auto function_ptr = std::make_shared<MsgHandleFunc>(std::move(handle));
  z_closure(
      &z_callback,
      [](z_loaned_sample_t *sample, void *arg) { (*reinterpret_cast<MsgHandleFunc *>(arg))(sample); },

      nullptr,

      function_ptr.get());

  z_owned_subscriber_t z_sub;

  if (z_declare_subscriber(z_loan(z_session_), &z_sub, z_loan(key), z_move(z_callback), nullptr) < 0) {
    MW_ERROR("Unable to declare Subscriber!");
    return false;
  }

  z_sub_catalog_.emplace(keyexpr, z_sub);
  msg_handle_vec_.emplace_back(std::move(function_ptr));

  MW_TRACE("Subscriber with keyexpr: {} registered successfully.", keyexpr.c_str());
  return true;
}

void ZenohBus::RecordDiscoveryEndpoint(ibmw::discovery::EndpointRole role,
    std::string topic, std::string type, std::string key) {
  discovery_.Record(role, std::move(topic), std::move(type), std::move(key));
}

void ZenohBus::StartDiscovery(const std::string& limit_domain) {
  if (!initialized_) throw std::logic_error("Zenoh bus is not initialized.");
  discovery_.Start(z_loan(z_session_), limit_domain);
}

void ZenohBus::StopDiscovery() noexcept { discovery_.Stop(); }

bool ZenohBus::RegisterRpcNode(const std::string &keyexpr, MsgHandleFunc handle, const std::string &role, bool shm_enabled) {
  std::string pub_keyexpr;
  std::string sub_keyexpr;

  if (role == "client") {
    pub_keyexpr = "req/" + keyexpr;
    sub_keyexpr = "rsp/" + keyexpr;
  } else if (role == "server") {
    pub_keyexpr = "rsp/" + keyexpr;
    sub_keyexpr = "req/" + keyexpr;
  } else {
    MW_ERROR("Invalid role: {}", role);
    return false;
  }

  if (!RegisterPublisher(pub_keyexpr, shm_enabled)) {
    MW_ERROR("Unable to register the publisher of {} {}.", role, keyexpr);
    return false;
  }
  // A client's handler finds the call by request id, so one subscriber serves
  // all clients of the function. Requests go to the first server only.
  {
    std::lock_guard lock(z_catalog_mutex_);
    if (z_sub_catalog_.contains(sub_keyexpr)) {
      if (role == "server")
        MW_WARN("Server {} is registered again; zenoh requests keep going to the first one.", keyexpr);
      return true;
    }
  }
  if (!RegisterSubscriber(sub_keyexpr, std::move(handle))) {
    MW_ERROR("Unable to register the subscriber of {} {}.", role, keyexpr);
    return false;
  }
  MW_INFO("{} with keyexpr: {} ,role: {} , shm_enabled: {}  is registered successfully.", role, keyexpr.c_str(), role, shm_enabled);
  return true;
}

void ZenohBus::Send(const std::string &topic, char *serialized_data_ptr, uint64_t serialized_data_len) {
  auto z_pub_iter = z_pub_catalog_.find(topic);
  if (z_pub_iter == z_pub_catalog_.end()) [[unlikely]] {
    MW_ERROR("Url: {} is not registered!", topic);
    return;
  }

  z_owned_bytes_t z_payload;

  z_bytes_from_buf(&z_payload, reinterpret_cast<uint8_t *>(serialized_data_ptr), serialized_data_len, nullptr, nullptr);
  z_publisher_put(z_loan(z_pub_iter->second->z_pub), z_move(z_payload), &z_pub_options_);
}

}  // namespace ibmw::extensions::zenoh_extension
