// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/runtime_graph/topic_graph.h"

#include <algorithm>

#include "base/exception.h"

namespace ibmw::runtime::core::runtime_graph {

void TopicGraph::Clear() {
  topics_.clear();
}

void TopicGraph::MergeRuntime(const RuntimeRegistration& runtime) {
  for (auto advertisement : runtime.topics) {
    advertisement.runtime_id = runtime.runtime_id;
    advertisement.control_socket = runtime.control_socket;
    auto& entries = topics_[advertisement.topic.topic];
    for (const auto& existing : entries) {
      if (existing.topic.msg_type == advertisement.topic.msg_type &&
          existing.topic.serialization_type ==
              advertisement.topic.serialization_type) {
        const bool existing_has_schema =
            !existing.topic.schema.schema_data.empty();
        const bool advertisement_has_schema =
            !advertisement.topic.schema.schema_data.empty();
        if (existing_has_schema && advertisement_has_schema) {
          MW_ASSERT(
              existing.topic.schema.schema_hash ==
                      advertisement.topic.schema.schema_hash &&
                  existing.topic.schema.schema_data ==
                      advertisement.topic.schema.schema_data,
              "Conflicting Schema advertisements for topic '{}' and type '{}'.",
              advertisement.topic.topic, advertisement.topic.msg_type);
        }
      }
    }

    const auto duplicate = std::find_if(
        entries.begin(), entries.end(), [&](const auto& existing) {
          return existing.runtime_id == advertisement.runtime_id &&
                 existing.publisher_id == advertisement.publisher_id;
        });
    if (duplicate == entries.end()) {
      entries.emplace_back(std::move(advertisement));
    } else {
      *duplicate = std::move(advertisement);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
      return std::tie(lhs.runtime_id, lhs.publisher_id) <
             std::tie(rhs.runtime_id, rhs.publisher_id);
    });
  }
}

void TopicGraph::ReplaceRuntimes(
    const std::vector<RuntimeRegistration>& runtimes) {
  Clear();
  for (const auto& runtime : runtimes) {
    MergeRuntime(runtime);
  }
}

std::vector<TopicAdvertisement> TopicGraph::Find(
    std::string_view topic_name) const {
  const auto found = topics_.find(std::string(topic_name));
  return found == topics_.end() ? std::vector<TopicAdvertisement>{}
                               : found->second;
}

std::vector<std::string> TopicGraph::Topics() const {
  std::vector<std::string> result;
  result.reserve(topics_.size());
  for (const auto& [topic, entries] : topics_) {
    static_cast<void>(entries);
    result.emplace_back(topic);
  }
  std::sort(result.begin(), result.end());
  return result;
}

}  // namespace ibmw::runtime::core::runtime_graph
