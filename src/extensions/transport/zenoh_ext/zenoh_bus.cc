// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "zenoh_ext/zenoh_bus.h"

#include <chrono>
#include <future>
#include <stdexcept>
#include <utility>

namespace ibmw::extensions::zenoh_extension {

namespace {

// Set while a subscriber callback runs on this thread. Zenoh may then hold
// other callbacks of the same sample on this thread, so none can be awaited.
thread_local bool in_bus_callback = false;

// A registered subscriber's callback; zenoh owns it and signals when it drops it.
struct SubscriberCallback {
  ZenohBus::MsgHandleFunc handle;
  std::promise<void> released;
};

}  // namespace

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
  // Subscribers first: a running callback may still publish.
  std::unordered_map<std::string, ZenohSubCtx> subs;
  {
    std::lock_guard lock(z_catalog_mutex_);
    subs.swap(z_sub_catalog_);
  }
  for (auto &ptr : subs) {
    DropSubscriber(ptr.first, ptr.second);
  }

  for (auto &ptr : z_pub_catalog_) {
    z_drop(z_move(ptr.second->z_pub));
  }

  if (initialized_) z_close(z_loan_mut(z_session_), nullptr);
  z_pub_catalog_.clear();

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
            [](z_loaned_sample_t* sample, void* context) {
              const bool outer = std::exchange(in_bus_callback, true);  // local puts nest callbacks
              (*static_cast<MsgHandleFunc*>(context))(sample);
              in_bus_callback = outer;
            },
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
  auto* callback = new SubscriberCallback{.handle = std::move(handle), .released = {}};
  std::future<void> released = callback->released.get_future();
  z_closure(
      &z_callback,
      [](z_loaned_sample_t *sample, void *arg) {
        const bool outer = std::exchange(in_bus_callback, true);  // local puts nest callbacks
        static_cast<SubscriberCallback *>(arg)->handle(sample);
        in_bus_callback = outer;
      },
      [](void *arg) {
        // The handler is destroyed before the waiter resumes.
        auto* owned = static_cast<SubscriberCallback *>(arg);
        auto promise = std::move(owned->released);
        delete owned;
        promise.set_value();
      },
      callback);

  z_owned_subscriber_t z_sub;

  // On failure zenoh still owns the closure and drops it, which deletes the callback.
  if (z_declare_subscriber(z_loan(z_session_), &z_sub, z_loan(key), z_move(z_callback), nullptr) < 0) {
    MW_ERROR("Unable to declare Subscriber!");
    return false;
  }

  z_sub_catalog_.emplace(keyexpr, ZenohSubCtx{z_sub, std::move(released)});

  MW_TRACE("Subscriber with keyexpr: {} registered successfully.", keyexpr.c_str());
  return true;
}

bool ZenohBus::UnregisterSubscriber(const std::string &keyexpr) {
  ZenohSubCtx sub;
  {
    std::lock_guard lock(z_catalog_mutex_);
    auto iter = z_sub_catalog_.find(keyexpr);
    if (iter == z_sub_catalog_.end()) return false;
    sub = std::move(iter->second);
    z_sub_catalog_.erase(iter);
  }
  DropSubscriber(keyexpr, sub);
  return true;
}

void ZenohBus::DropSubscriber(const std::string &keyexpr, ZenohSubCtx &sub) {
  z_drop(z_move(sub.z_sub));
  // Callbacks already running keep zenoh's reference to the callback.
  if (in_bus_callback) [[unlikely]] {
    MW_WARN("Subscriber {} is removed from a zenoh callback; its running callbacks are not awaited.", keyexpr);
    return;
  }
  // Not given up: the callback would then run into whatever it uses.
  while (sub.released.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
    MW_WARN("Subscriber {} waits for a running zenoh callback.", keyexpr);
  }
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
