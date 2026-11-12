// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "topic_check.h"

#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace ibmw::cli::doctor {

std::vector<TopicCheckResult> CheckTopics(
    const discovery::DiscoverySnapshot& snapshot, bool include_hidden) {
  using discovery::EndpointRole;
  using Name = std::pair<bool, std::string>;  // true means a wire name
  using Key = std::pair<Name, Name>;         // topic, type
  std::vector<TopicCheckResult> results;
  if (snapshot.sources.empty()) {
    results.push_back({.status = CheckStatus::kUnknown,
                       .message = "No discovery source status was returned."});
    return results;
  }
  for (const auto& source : snapshot.sources) {
    if (source.status != discovery::DiscoveryStatus::kReady) {
      results.push_back({.source = source.source, .status = CheckStatus::kUnknown,
                         .message = source.message.empty()
                             ? "Discovery did not complete." : source.message});
      continue;  // Partial/stale endpoints cannot establish an absent subscriber.
    }
    std::map<Key, TopicCheckResult> groups;
    std::map<Key, std::set<std::string>> publisher_participants;
    std::set<std::tuple<std::string, std::string, EndpointRole>> seen;
    std::set<Name> unknown_subscriber_types;
    std::map<Name, size_t> subscriber_totals;
    bool unnamed_subscriber = false;
    bool unnamed_endpoint = false;
    for (const auto& endpoint : snapshot.endpoints) {
      if (endpoint.source != source.source || (endpoint.hidden && !include_hidden)) continue;
      if (!endpoint.endpoint_id.empty() &&
          !seen.emplace(endpoint.participant_id, endpoint.endpoint_id, endpoint.role).second) continue;
      const Name topic{endpoint.topic_name.empty(), endpoint.topic_name.empty()
          ? endpoint.wire_topic_name : endpoint.topic_name};
      const Name type{endpoint.type_name.empty(), endpoint.type_name.empty()
          ? endpoint.wire_type_name : endpoint.type_name};
      if (topic.second.empty()) {
        unnamed_endpoint = true;
        unnamed_subscriber |= endpoint.role == EndpointRole::kSubscriber;
        continue;
      }
      auto& group = groups[{topic, type}];
      group.source = source.source;
      group.topic = topic.second;
      group.type = type.second;
      if (endpoint.role == EndpointRole::kPublisher) {
        ++group.publishers;
        publisher_participants[{topic, type}].insert(endpoint.participant_id);
      } else {
        ++group.subscribers;
        ++subscriber_totals[topic];
        if (type.second.empty()) unknown_subscriber_types.insert(topic);
      }
    }
    if (unnamed_endpoint) {
      results.push_back({.source = source.source, .status = CheckStatus::kUnknown,
                         .message = "An endpoint has no topic name; the check is incomplete."});
    }
    for (auto& [key, group] : groups) {
      if (group.publishers == 0) {
        group.status = CheckStatus::kSkipped;
        group.message = "No publisher observed; subscriber-only topics are outside this check.";
      } else if ((key.second.second.empty() && subscriber_totals[key.first] > 0) ||
                 (group.subscribers == 0 &&
                  (unnamed_subscriber || unknown_subscriber_types.contains(key.first)))) {
        group.status = CheckStatus::kUnknown;
        group.message = "Incomplete endpoint metadata prevents determining subscriber presence.";
      } else if (group.subscribers == 0) {
        // An RPC server's reply writer has readers only while clients exist.
        // Every publisher in the group must have its own request reader;
        // one healthy server must not hide a separate orphan reply writer.
        const auto& participants = publisher_participants.at(key);
        const bool idle_rpc_reply = source.source.backend == discovery::DiscoveryBackend::kDds &&
            group.topic.starts_with("ibmw_rpc/rr/") && !group.type.empty() &&
            std::all_of(participants.begin(), participants.end(), [&](const auto& participant) {
              return !participant.empty() && std::any_of(snapshot.endpoints.begin(), snapshot.endpoints.end(),
                  [&](const auto& request) {
                    return request.source == source.source && (!request.hidden || include_hidden) &&
                        request.role == EndpointRole::kSubscriber && request.participant_id == participant &&
                        (request.topic_name.empty() ? request.wire_topic_name : request.topic_name) ==
                            "ibmw_rpc/rq/" + group.topic.substr(12);
                  });
            });
        group.status = idle_rpc_reply ? CheckStatus::kSkipped : CheckStatus::kWarning;
        group.message = idle_rpc_reply
            ? "Idle RPC reply publisher has a request subscriber in the same participant; no active client is required."
            : "Publisher without subscriber observed in this source for this topic/type.";
      } else {
        group.status = CheckStatus::kOk;
        group.message = "Publisher and subscriber observed; QoS and delivery were not checked.";
      }
      results.push_back(std::move(group));
    }
    if (groups.empty() && !unnamed_endpoint) {
      results.push_back({.source = source.source, .status = CheckStatus::kSkipped,
                         .message = "No visible topic endpoints observed in this source."});
    }
  }
  return results;
}

}  // namespace ibmw::cli::doctor
