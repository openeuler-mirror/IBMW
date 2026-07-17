// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <functional>

#include "base/inline_function.h"
#include "engine/transport/message_frame.h"

namespace ibmw::runtime::core::transport {

using FrameworkAsyncTransportHandle = ibmw::util::InlineFunction<void(MessageFrame&)>;
using FrameworkAsyncTransportFilter = ibmw::util::InlineFunction<void(MessageFrame&, FrameworkAsyncTransportHandle&&)>;

class FrameworkAsyncTransportFilterCollector {
 public:
  FrameworkAsyncTransportFilterCollector()
      : final_filter_(
            [](MessageFrame& msg_wrapper, FrameworkAsyncTransportHandle&& h) {
              h(msg_wrapper);
            }) {}
  ~FrameworkAsyncTransportFilterCollector() = default;

  FrameworkAsyncTransportFilterCollector(const FrameworkAsyncTransportFilterCollector&) = delete;
  FrameworkAsyncTransportFilterCollector& operator=(const FrameworkAsyncTransportFilterCollector&) = delete;

  void RegisterFilter(const FrameworkAsyncTransportFilter& filter) {
    final_filter_ =
        [final_filter{std::move(final_filter_)}, &filter](
            MessageFrame& msg_wrapper, FrameworkAsyncTransportHandle&& h) {
          filter(
              msg_wrapper,
              [&final_filter, h{std::move(h)}](MessageFrame& msg_wrapper) mutable {
                final_filter(msg_wrapper, std::move(h));
              });
        };
  }

  void InvokeTransport(FrameworkAsyncTransportHandle&& h, MessageFrame& msg_wrapper) const {
    final_filter_(msg_wrapper, std::move(h));
  }

  void Clear() {
    final_filter_ = FrameworkAsyncTransportFilter();
  }

 private:
  FrameworkAsyncTransportFilter final_filter_;
};

class FrameworkAsyncTransportFilterBus {
 public:
  FrameworkAsyncTransportFilterBus() = default;
  ~FrameworkAsyncTransportFilterBus() = default;

  FrameworkAsyncTransportFilterBus(const FrameworkAsyncTransportFilterBus&) = delete;
  FrameworkAsyncTransportFilterBus& operator=(const FrameworkAsyncTransportFilterBus&) = delete;

  void RegisterFilter(std::string_view name, FrameworkAsyncTransportFilter&& filter) {
    auto emplace_ret = filter_map_.emplace(name, std::move(filter));
    MW_ASSERT(emplace_ret.second, "Register filter {} failed.", name);
  }

  std::vector<std::string> GetAllFiltersName() const {
    std::vector<std::string> result;
    result.reserve(filter_map_.size());
    for (const auto& itr : filter_map_)
      result.emplace_back(itr.first);

    return result;
  }

  void CreateFilterCollector(
      std::string_view topic_name, const std::vector<std::string>& filter_name_vec) {
    if (filter_name_vec.empty()) [[unlikely]]
      return;

    auto collector_ptr = std::make_unique<FrameworkAsyncTransportFilterCollector>();

    for (const auto& name : filter_name_vec) {
      auto find_itr = filter_map_.find(name);
      MW_ASSERT(find_itr != filter_map_.end(), "Can not find filter: {}", name);

      collector_ptr->RegisterFilter(find_itr->second);
    }

    auto emplace_ret = filter_collector_map_.emplace(topic_name, std::move(collector_ptr));

    MW_ASSERT(emplace_ret.second, "Func {} set filter collector failed.", topic_name);

    filter_names_map_.emplace(topic_name, filter_name_vec);
  }

  void CreateFilterCollectorIfNotExist(
      std::string_view topic_name, const std::vector<std::string>& filter_name_vec) {
    if (filter_collector_map_.find(topic_name) == filter_collector_map_.end())
      CreateFilterCollector(topic_name, filter_name_vec);
  }

  const FrameworkAsyncTransportFilterCollector& GetFilterCollector(std::string_view topic_name) const {
    auto find_itr = filter_collector_map_.find(topic_name);
    if (find_itr != filter_collector_map_.end()) {
      return *(find_itr->second);
    }

    return default_filter_collector_;
  }

  std::vector<std::string> GetFilterNameVec(std::string_view topic_name) const {
    auto find_itr = filter_names_map_.find(topic_name);
    if (find_itr != filter_names_map_.end()) {
      return find_itr->second;
    }

    return {};
  }

  void Clear() {
    filter_collector_map_.clear();
    filter_map_.clear();
  }

 private:
  // filter name - filter
  std::unordered_map<std::string, FrameworkAsyncTransportFilter> filter_map_;

  // topic name - filter collector
  using FilterCollectorMap = std::unordered_map<
      std::string,
      std::unique_ptr<FrameworkAsyncTransportFilterCollector>,
      ibmw::common::util::StringHash,
      std::equal_to<>>;
  FilterCollectorMap filter_collector_map_;

  // topic name - filter name vec
  using FilterNameMap = std::unordered_map<
      std::string,
      std::vector<std::string>,
      ibmw::common::util::StringHash,
      std::equal_to<>>;
  FilterNameMap filter_names_map_;

  FrameworkAsyncTransportFilterCollector default_filter_collector_;
};

}  // namespace ibmw::runtime::core::transport
