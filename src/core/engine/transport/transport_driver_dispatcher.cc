// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/transport/transport_driver_dispatcher.h"

#include <regex>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "base/log_util.h"

namespace ibmw::runtime::core::transport {

namespace {

bool IsTransportShutdownProfileEnabled() {
  const char* value = std::getenv("IBMW_TRANSPORT_SHUTDOWN_PROFILE");
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

}  // namespace

void TransportDriverDispatcher::Initialize() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Transport driver bus can only be initialized once.");
}

void TransportDriverDispatcher::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");

  for (auto& driver : transport_driver_index_vec_) {
    MW_TRACE("Start transport driver '{}'.", driver->Name());
    driver->Start();
  }
}

void TransportDriverDispatcher::Shutdown() {
  if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
    return;

  const bool shutdown_profile = IsTransportShutdownProfileEnabled();
  for (auto& driver : transport_driver_index_vec_) {
    if (shutdown_profile) {
      MW_INFO("Transport shutdown profile [driver-start]: {}", driver->Name());
    }
    MW_TRACE("Shutdown transport driver '{}'.", driver->Name());
    driver->Shutdown();
    if (shutdown_profile) {
      MW_INFO("Transport shutdown profile [driver-done]: {}", driver->Name());
    }
  }
}

void TransportDriverDispatcher::SetTransportCatalog(TransportCatalog* transport_catalog_ptr) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  transport_catalog_ptr_ = transport_catalog_ptr;
}

void TransportDriverDispatcher::SetPublishFrameworkAsyncTransportFilterBus(
    FrameworkAsyncTransportFilterBus* ptr) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  publish_filter_bus_ptr_ = ptr;
}

void TransportDriverDispatcher::SetSubscribeFrameworkAsyncTransportFilterBus(
    FrameworkAsyncTransportFilterBus* ptr) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  subscribe_filter_bus_ptr_ = ptr;
}

void TransportDriverDispatcher::SetPublishFiltersRules(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& rules) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  publish_filters_rules_ = rules;
}

void TransportDriverDispatcher::SetSubscribeFiltersRules(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& rules) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  subscribe_filters_rules_ = rules;
}

void TransportDriverDispatcher::SetPubTopicsDriversRules(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& rules) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  pub_topics_drivers_rules_ = rules;
}

void TransportDriverDispatcher::SetSubTopicsDriversRules(
    const std::vector<std::pair<std::string, std::vector<std::string>>>& rules) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  sub_topics_drivers_rules_ = rules;
}

void TransportDriverDispatcher::RegisterTransportDriver(
    TransportDriver* transport_driver_ptr) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  transport_driver_index_vec_.emplace_back(transport_driver_ptr);
}

TransportDriverDispatcher::TopicDriverInfoMap TransportDriverDispatcher::GetPubTopicDriverInfo() const {
  std::unordered_map<std::string_view, std::vector<std::string_view>> result;
  for (const auto& itr : pub_topics_driver_index_map_) {
    std::vector<std::string_view> drivers_name;
    drivers_name.reserve(itr.second.size());
    for (const auto& item : itr.second)
      drivers_name.emplace_back(item->Name());

    result.emplace(itr.first, std::move(drivers_name));
  }

  return result;
}

TransportDriverDispatcher::TopicDriverInfoMap TransportDriverDispatcher::GetSubTopicDriverInfo() const {
  std::unordered_map<std::string_view, std::vector<std::string_view>> result;
  for (const auto& itr : sub_topics_driver_index_map_) {
    std::vector<std::string_view> drivers_name;
    drivers_name.reserve(itr.second.size());
    for (const auto& item : itr.second)
      drivers_name.emplace_back(item->Name());

    result.emplace(itr.first, std::move(drivers_name));
  }

  return result;
}

std::vector<TransportDriver*> TransportDriverDispatcher::GetDriversByRules(
    std::string_view topic_name,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& rules) {
  for (const auto& item : rules) {
    const auto& topic_regex = item.first;
    const auto& enable_drivers = item.second;

    try {
      if (std::regex_match(topic_name.begin(), topic_name.end(), std::regex(topic_regex, std::regex::ECMAScript))) {
        std::vector<TransportDriver*> driver_ptr_vec;

        for (const auto& driver_name : enable_drivers) {
          auto itr = std::find_if(
              transport_driver_index_vec_.begin(), transport_driver_index_vec_.end(),
              [&driver_name](const TransportDriver* driver_ptr) -> bool {
                return driver_ptr->Name() == driver_name;
              });

          if (itr == transport_driver_index_vec_.end()) [[unlikely]] {
            MW_WARN("Can not find '{}' in driver list.", driver_name);
            continue;
          }

          driver_ptr_vec.emplace_back(*itr);
        }

        return driver_ptr_vec;
      }
    } catch (const std::exception& e) {
      MW_WARN("Regex get exception, expr: {}, string: {}, exception info: {}",
                 topic_regex, topic_name, e.what());
    }
  }

  return {};
}

std::vector<std::string> TransportDriverDispatcher::GetFilterRules(
    std::string_view topic_name,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& rules) {
  for (const auto& item : rules) {
    const auto& topic_regex = item.first;
    const auto& filters = item.second;

    try {
      if (std::regex_match(topic_name.begin(), topic_name.end(), std::regex(topic_regex, std::regex::ECMAScript))) {
        return filters;
      }
    } catch (const std::exception& e) {
      MW_WARN("Regex get exception, expr: {}, string: {}, exception info: {}",
                 topic_regex, topic_name, e.what());
    }
  }

  return {};
}
}  // namespace ibmw::runtime::core::transport
