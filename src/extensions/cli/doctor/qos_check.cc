// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "qos_check.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <fnmatch.h>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>

#include <nlohmann/json.hpp>

namespace ibmw::cli::doctor {
namespace {
using Policies = std::map<std::string, std::string>;
using Status = CheckStatus;
using Endpoint = discovery::DiscoveredEndpoint;
using Role = discovery::EndpointRole;
using Backend = discovery::DiscoveryBackend;

std::string Value(const Policies& policies, const std::string& key) {
  if (key == "resource_limits") {
    std::string summary;
    for (const auto* field : {"max_samples", "max_instances", "max_samples_per_instance"}) {
      const auto it = policies.find("resource_limits." + std::string(field));
      if (it != policies.end()) summary += (summary.empty() ? "" : ", ") + std::string(field) + '=' + it->second;
    }
    if (!summary.empty()) return summary;
  }
  if (key == "type_consistency") {
    std::string summary;
    for (const auto* field : {"type_information", "type_complete", "type_minimal", "type_identifier"}) {
      const auto it = policies.find(field);
      if (it != policies.end()) summary += (summary.empty() ? "" : ", ") + std::string(field) + '=' + it->second;
    }
    if (!summary.empty()) return summary;
  }
  const auto it = policies.find(key);
  return it == policies.end() ? "<unavailable>" : it->second;
}

std::optional<int64_t> Integer(std::string_view value) {
  int64_t result = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
  if (error != std::errc{} || end != value.data() + value.size()) return {};
  return result;
}

// Compare seconds/nanoseconds separately: legal durations need not fit in int64 nanoseconds.
using Duration = std::pair<uint64_t, uint32_t>;
std::optional<Duration> Time(const std::string& value, bool ros) {
  if (ros && value == "default") {
    // RMW distinguishes its all-zero default sentinel from an explicit infinite duration.
    return Duration{std::numeric_limits<uint64_t>::max(), 1};
  }
  if (value == "infinite") {
    return Duration{std::numeric_limits<uint64_t>::max(), 0};
  }
  std::istringstream input(value);
  std::string seconds, s, nanos, ns, extra;
  if (!(input >> seconds >> s >> nanos >> ns) || input >> extra || s != "s" || ns != "ns") return {};
  const auto sec = Integer(seconds), nsec = Integer(nanos);
  if (!sec || !nsec || *sec < 0 || *nsec < 0 || *nsec >= 1000000000) return {};
  return Duration{static_cast<uint64_t>(*sec), static_cast<uint32_t>(*nsec)};
}

std::optional<int> Rank(const std::string& value, std::initializer_list<const char*> order) {
  int rank = 0;
  for (const auto* name : order) {
    if (value == name) return rank;
    ++rank;
  }
  return {};
}

bool Fingerprint(const std::string& value, bool with_size) {
  if (value.size() < 31 || (value.substr(0, 3) != "f1:" && value.substr(0, 3) != "f2:")) return false;
  for (size_t index = 3; index < 31; ++index) {
    const char digit = value[index];
    if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) return false;
  }
  if (!with_size) return value.size() == 31;
  if (value.size() < 33 || value[31] != ':') return false;
  const auto size = Integer(std::string_view(value).substr(32));
  return size && *size >= 0 && *size <= UINT32_MAX;
}

std::optional<std::vector<std::string>> Names(const std::string& value) {
  const auto json = nlohmann::json::parse(value, nullptr, false);
  if (!json.is_array()) return {};
  std::vector<std::string> names;
  for (const auto& name : json) {
    if (!name.is_string()) return {};
    auto text = name.get<std::string>();
    if (text.find('\0') != std::string::npos) return {};
    names.push_back(std::move(text));
  }
  return names;
}

std::string Topic(const Endpoint& endpoint) {
  return endpoint.topic_name.empty() ? endpoint.wire_topic_name : endpoint.topic_name;
}
std::string Type(const Endpoint& endpoint) {
  return endpoint.type_name.empty() ? endpoint.wire_type_name : endpoint.type_name;
}
std::string Identity(const Endpoint& endpoint, size_t index) {
  std::string id = endpoint.participant_id + "/" +
      (endpoint.endpoint_id.empty() ? "snapshot-" + std::to_string(index) : endpoint.endpoint_id);
  if (!endpoint.module_name.empty()) id += " module=" + endpoint.module_name;
  if (!endpoint.node_name.empty()) id += " node=" + endpoint.node_namespace + "/" + endpoint.node_name;
  return id;
}

struct View {
  const Endpoint* endpoint;
  const Policies* policies;
  std::string driver;
  std::string identity;
};

class Checks {
 public:
  Checks(std::vector<QosCheckResult>& results, const View& writer, const View* reader = nullptr)
      : results_(results), a_(*writer.policies), b_(reader ? *reader->policies : empty_),
        ros_(writer.driver == "ros2") {
    base_.source = writer.endpoint->source;
    base_.driver = writer.driver;
    base_.topic = Topic(*writer.endpoint);
    base_.type = Type(*writer.endpoint);
    if (writer.endpoint->role == Role::kPublisher) base_.publisher = writer.identity;
    else base_.subscriber = writer.identity;
    if (reader) base_.subscriber = reader->identity;
  }

  void Add(const std::string& key, Status status, std::string message) {
    auto result = base_;
    result.policy = key;
    result.offered = Value(a_, key);
    result.requested = Value(b_, key);
    result.status = status;
    result.message = std::move(message);
    results_.push_back(std::move(result));
  }

  void Ordered(const std::string& key, std::initializer_list<const char*> order, bool equal = false) {
    const auto a = Rank(Value(a_, key), order), b = Rank(Value(b_, key), order);
    if (!a || !b) return Add(key, Status::kUnknown, "Missing, default, unknown or invalid policy value.");
    const bool compatible = equal ? *a == *b : *a >= *b;
    Add(key, compatible ? Status::kOk : Status::kWarning,
        compatible ? "This policy is compatible." : "Offered policy does not satisfy this subscriber.");
  }

  void DurationPair(const std::string& key) {
    const auto a = Time(Value(a_, key), ros_), b = Time(Value(b_, key), ros_);
    if (!a || !b) return Add(key, Status::kUnknown, "Duration was not observed or cannot be interpreted.");
    Add(key, *a <= *b ? Status::kOk : Status::kWarning,
        *a <= *b ? "Offered duration is within the requested duration."
                 : "Offered duration exceeds the requested duration; this pair is incompatible.");
  }

  void CommonPair() {
    Ordered("reliability", {"best_effort", "reliable"});
    if (ros_) Ordered("durability", {"volatile", "transient_local"});
    else Ordered("durability", {"volatile", "transient_local", "transient", "persistent"});
    if (ros_) Ordered("liveliness", {"automatic", "manual_by_topic"});
    else Ordered("liveliness", {"automatic", "manual_by_participant", "manual_by_topic"});
    DurationPair("deadline");
    DurationPair("liveliness_lease_duration");
  }

  void DdsPair(const View& writer, const View& reader) {
    Ordered("ownership", {"shared", "exclusive"}, true);
    const auto wa = Rank(Value(a_, "disable_positive_acks"), {"false", "true"});
    const auto ra = Rank(Value(b_, "disable_positive_acks"), {"false", "true"});
    if (!wa || !ra) Add("disable_positive_acks", Status::kUnknown, "ACK policy was not observed.");
    else Add("disable_positive_acks", *wa >= *ra ? Status::kOk : Status::kWarning,
             *wa >= *ra ? "ACK policies are compatible." : "Reader suppresses ACKs required by the writer.");
    Partitions();
    Representation();
    Types(writer, reader);
    const auto wk = Value(a_, "topic_kind"), rk = Value(b_, "topic_kind");
    if (!Rank(wk, {"keyed", "unkeyed"}) || !Rank(rk, {"keyed", "unkeyed"})) {
      Add("topic_kind", Status::kUnknown, "Keyed/unkeyed endpoint metadata is unavailable.");
    } else Add("topic_kind", wk == rk ? Status::kOk : Status::kWarning,
               wk == rk ? "Topic key kinds agree." : "Keyed and unkeyed endpoints cannot match.");
  }

  void EndpointConfiguration(bool publisher) {
    const auto history = Value(a_, "history");
    const auto depth = Integer(Value(a_, "depth"));
    if (!a_.contains("history")) Add("history", Status::kSkipped, "History is not advertised by this source.");
    else if (!Rank(history, {"keep_last", "keep_all"})) Add("history", Status::kUnknown, "History is default, unknown or invalid.");
    else Add("history", Status::kOk, "Recognized endpoint policy; histories need not be equal across endpoints.");
    if (history == "keep_all") Add("depth", Status::kSkipped, "Depth does not constrain KEEP_ALL history.");
    else if (history == "keep_last") {
      Add("depth", !depth ? Status::kUnknown : *depth > 0 ? Status::kOk : Status::kWarning,
          "KEEP_LAST requires a positive endpoint depth; different endpoint depths are allowed.");
    } else Add("depth", Status::kSkipped, "Cannot validate depth without an observed history policy.");
    if (!publisher) Add("lifespan", Status::kSkipped, "Lifespan controls writer sample expiration, not reader matching.");
    else if (!a_.contains("lifespan")) Add("lifespan", Status::kSkipped, "Writer lifespan is not advertised by this source.");
    else Add("lifespan", Time(Value(a_, "lifespan"), ros_) ? Status::kOk : Status::kUnknown,
             "Writer expiration setting only; actual expiration requires traffic observations.");
    if (ros_) {
      Add("avoid_ros_namespace_conventions", Status::kSkipped,
          "Name-mapping option; endpoint names determine pairing, not equality of this flag.");
      return;
    }

    Resources(history, depth);
    if (publisher && a_.contains("ownership_strength")) {
      const auto strength = Integer(Value(a_, "ownership_strength"));
      Add("ownership_strength", strength && *strength >= INT32_MIN && *strength <= INT32_MAX
          ? Status::kOk : Status::kUnknown,
          "Ownership strength selects an exclusive writer; endpoint strengths need not be equal.");
    } else Add("ownership_strength", Status::kSkipped, "Writer selection policy; not a pair compatibility requirement.");

    for (const char* policy : {"presentation", "destination_order", "latency_budget", "time_based_filter",
                               "durability_service", "transport_priority"}) {
      Add(policy, Status::kSkipped, "Not enforced as a matching policy by the supported Fast DDS 2.6/3.2 implementations.");
    }
    for (const char* policy : {"user_data", "topic_data", "group_data", "entity_factory",
                               "writer_data_lifecycle", "reader_data_lifecycle", "publish_mode",
                               "data_sharing", "reliability_max_blocking_time", "liveliness_announcement_period",
                               "disable_positive_acks_duration"}) {
      if (a_.contains(policy) && (std::string_view(policy).ends_with("time") ||
                                std::string_view(policy).ends_with("period") ||
                                std::string_view(policy).ends_with("duration"))) {
        Add(policy, Time(Value(a_, policy), false) ? Status::kOk : Status::kUnknown,
            "Endpoint-local timing policy; not a requested/offered matching requirement.");
      } else Add(policy, Status::kSkipped, "Endpoint-local or informational policy; not a requested/offered mismatch check.");
    }
  }

 private:
  void Resources(const std::string& history, std::optional<int64_t> depth) {
    const auto samples = Integer(Value(a_, "resource_limits.max_samples"));
    const auto instances = Integer(Value(a_, "resource_limits.max_instances"));
    const auto per_instance = Integer(Value(a_, "resource_limits.max_samples_per_instance"));
    if (!a_.contains("resource_limits.max_samples") && !a_.contains("resource_limits.max_instances") &&
        !a_.contains("resource_limits.max_samples_per_instance")) {
      return Add("resource_limits", Status::kSkipped, "Resource limits are local and not advertised in endpoint discovery.");
    }
    if (!samples || !instances || !per_instance) {
      return Add("resource_limits", Status::kUnknown, "Incomplete or malformed resource limits.");
    }
    const auto valid = [](int64_t value) { return value == -1 || (value > 0 && value <= INT32_MAX); };
    bool ok = valid(*samples) && valid(*instances) && valid(*per_instance);
    if (ok && Value(a_, "topic_kind") == "keyed" && *samples != -1) {
      // Fast DDS enforces the aggregate allocation bound for keyed endpoints only.
      // Each operand fits int32; their product safely fits int64.
      ok &= *instances != -1 && *per_instance != -1 && *instances * *per_instance <= *samples;
    }
    if (history == "keep_last" && depth) {
      ok &= *depth > 0 && (*per_instance == -1 || *depth <= *per_instance);
    }
    Add("resource_limits", ok ? Status::kOk : Status::kWarning,
        ok ? "Local history/resource limits are consistent." : "Local history depth and resource limits are inconsistent.");
  }

  void Partitions() {
    const auto a = Names(Value(a_, "partition")), b = Names(Value(b_, "partition"));
    if (!a || !b) return Add("partition", Status::kUnknown, "Missing or invalid partition list.");
    bool matched = false;
    if (a->empty()) matched = b->empty() || std::find(b->begin(), b->end(), "") != b->end();
    else if (b->empty()) matched = std::find(a->begin(), a->end(), "") != a->end();
    else for (const auto& writer : *a) for (const auto& reader : *b) {
      matched |= fnmatch(writer.c_str(), reader.c_str(), FNM_NOESCAPE) == 0 || fnmatch(reader.c_str(), writer.c_str(), FNM_NOESCAPE) == 0;
    }
    Add("partition", matched ? Status::kOk : Status::kWarning,
        matched ? "A partition matches." : "No partition matches; these endpoints will not match.");
  }

  void Representation() {
    const auto profile = Value(a_, "dds_profile");
    if (profile == "fastdds2") return Add("data_representation", Status::kSkipped,
        "Fast DDS 2.6 endpoint matching does not enforce this policy; encoding is reported without claiming delivery.");
    if (profile != "fastdds3" || Value(b_, "dds_profile") != profile) {
      return Add("data_representation", Status::kUnknown, "DDS representation matching profile is unavailable.");
    }
    const auto a = Names(Value(a_, "data_representation")), b = Names(Value(b_, "data_representation"));
    if (!a || !b) return Add("data_representation", Status::kUnknown, "Missing or invalid representation list.");
    const auto writer = a->empty() ? "xcdr1" : a->front();
    if (!Rank(writer, {"xcdr1", "xcdr2", "xml"}) ||
        std::any_of(b->begin(), b->end(), [](const auto& item) { return !Rank(item, {"xcdr1", "xcdr2", "xml"}); })) {
      return Add("data_representation", Status::kUnknown, "Unknown encoding identifier.");
    }
    const bool match = writer != "xml" && ((b->empty() && writer == "xcdr1") ||
        std::find(b->begin(), b->end(), writer) != b->end());
    Add("data_representation", match ? Status::kOk : Status::kWarning,
        match ? "Reader accepts the writer's selected representation." : "Reader cannot accept the writer's selected representation.");
  }

  void Types(const View& writer, const View& reader) {
    if (Type(*writer.endpoint).empty() || Type(*reader.endpoint).empty()) {
      return Add("type_consistency", Status::kUnknown, "Endpoint type name is unavailable.");
    }
    const auto a = Value(a_, "type_information"), b = Value(b_, "type_information");
    if (a == "present" && b == "present") {
      const auto profile = Value(a_, "dds_profile");
      if ((profile != "fastdds2" && profile != "fastdds3") || Value(b_, "dds_profile") != profile) {
        Add("type_consistency", Status::kUnknown, "Native type matching profile is missing or differs between endpoints.");
        return;
      }
      bool comparable = false, equal = false;
      for (const auto* kind : {"type_complete", "type_minimal", "type_identifier"}) {
        if (a_.contains(kind) && b_.contains(kind)) {
          if (!Fingerprint(a_.at(kind), profile == "fastdds3") || !Fingerprint(b_.at(kind), profile == "fastdds3")) {
            Add("type_consistency", Status::kUnknown, "Malformed type fingerprint; structural compatibility cannot be determined.");
            return;
          }
          comparable = true;
          equal |= a_.at(kind) == b_.at(kind);
          // Fast DDS 2 prefers complete, then minimal, then legacy TypeIdentifier.
          // Fast DDS 3 accepts either identical complete or identical minimal information.
          if (Value(a_, "dds_profile") == "fastdds2") break;
        }
      }
      const bool needs_objects = Value(a_, "dds_profile") == "fastdds2" && !equal;
      Add("type_consistency", !comparable || needs_objects ? Status::kUnknown : equal ? Status::kOk : Status::kWarning,
          !comparable ? "TypeObjects/identifiers were advertised but cannot be compared from this snapshot."
          : needs_objects ? "Different identifiers require Fast DDS 2 TypeObject consistency checks; this snapshot has only fingerprints."
          : equal ? "Advertised type identifiers agree (strict Fast DDS matching profile)."
                  : "Advertised type identifiers differ under the strict Fast DDS matching profile.");
    } else if ((a == "none" || a == "present") && (b == "none" || b == "present")) {
      const auto profile = Value(b_, "dds_profile");
      const auto force = Rank(Value(b_, "type_consistency.force_type_validation"), {"false", "true"});
      if (profile == "fastdds2" && !force) Add("type_consistency", Status::kUnknown, "Reader type-validation requirement was not observed.");
      else if (profile != "fastdds2" && profile != "fastdds3") Add("type_consistency", Status::kUnknown, "Native type matching profile is unavailable.");
      else {
        const bool ok = !(profile == "fastdds2" && force == 1) && Type(*writer.endpoint) == Type(*reader.endpoint);
        Add("type_consistency", ok ? Status::kOk : Status::kWarning,
            ok ? "Native name fallback applies because type information is absent; structural equality was not verified."
               : "Type validation cannot match: missing required type information or different type names.");
      }
    } else Add("type_consistency", Status::kUnknown, "Type information presence was not observed; names alone do not prove structural equality.");
    for (const auto* flag : {"kind", "force_type_validation", "ignore_sequence_bounds", "ignore_string_bounds", "ignore_member_names", "prevent_type_widening"}) {
      Add(std::string("type_consistency.") + flag, Status::kSkipped,
          "Reported reader setting; supported Fast DDS versions use their strict native type matching rather than general coercion.");
    }
  }

  std::vector<QosCheckResult>& results_;
  Policies empty_;
  const Policies& a_;
  const Policies& b_;
  bool ros_;
  QosCheckResult base_;
};
}  // namespace

std::vector<QosCheckResult> CheckQos(const discovery::DiscoverySnapshot& snapshot, bool include_hidden) {
  std::vector<QosCheckResult> results;
  for (const auto& source : snapshot.sources) {
    if (source.status != discovery::DiscoveryStatus::kReady) {
      results.push_back({.source = source.source, .policy = "discovery", .status = Status::kUnknown,
                         .message = "QoS check incomplete: " + source.message});
      continue;
    }
    std::vector<View> views;
    std::set<std::tuple<std::string, std::string, Role>> seen;
    for (size_t index = 0; index < snapshot.endpoints.size(); ++index) {
      const auto& endpoint = snapshot.endpoints[index];
      if (endpoint.source != source.source || (endpoint.hidden && !include_hidden)) continue;
      if (!endpoint.endpoint_id.empty() && !seen.emplace(endpoint.participant_id, endpoint.endpoint_id, endpoint.role).second) continue;
      if (source.source.backend == Backend::kRegistry) {
        for (const auto* driver : {"dds", "ros2"}) {
          const auto it = endpoint.driver_qos.find(driver);
          if (it != endpoint.driver_qos.end()) views.push_back({&endpoint, &it->second.policies, driver, Identity(endpoint, index)});
          else if (std::find(endpoint.available_drivers.begin(), endpoint.available_drivers.end(), driver) != endpoint.available_drivers.end()) {
            static const Policies missing;
            views.push_back({&endpoint, &missing, driver, Identity(endpoint, index)});
          }
        }
      } else if (source.source.backend == Backend::kDds || source.source.backend == Backend::kRos2) {
        views.push_back({&endpoint, &endpoint.qos.policies, std::string(discovery::BackendName(source.source.backend)), Identity(endpoint, index)});
      }
    }
    if (views.empty()) {
      results.push_back({.source = source.source, .policy = "scope", .status = Status::kSkipped,
                         .message = "No DDS/ROS 2 endpoints in this source; other transports are outside QoS checks."});
      continue;
    }
    for (const auto& view : views) {
      Checks checks(results, view);
      checks.EndpointConfiguration(view.endpoint->role == Role::kPublisher);
      if (Topic(*view.endpoint).empty()) checks.Add("scope", Status::kUnknown, "Endpoint topic name is unavailable.");
    }
    size_t pairs = 0;
    for (const auto& writer : views) {
      if (writer.endpoint->role != Role::kPublisher || Topic(*writer.endpoint).empty()) continue;
      for (const auto& reader : views) {
        if (reader.endpoint->role != Role::kSubscriber || writer.driver != reader.driver ||
            Topic(*writer.endpoint) != Topic(*reader.endpoint) ||
            writer.endpoint->topic_name.empty() != reader.endpoint->topic_name.empty()) continue;
        if (writer.driver == "ros2" && Type(*writer.endpoint) != Type(*reader.endpoint)) continue;
        Checks checks(results, writer, &reader);
        if (source.source.backend == Backend::kRegistry) {
          const auto wd = Value(*writer.policies, "domain_id"), rd = Value(*reader.policies, "domain_id");
          if (Integer(wd) && Integer(rd) && wd != rd) {
            checks.Add("domain_id", Status::kSkipped, "Different domains; these endpoints are not a candidate pair.");
            continue;
          }
          if (!Integer(wd) || !Integer(rd)) checks.Add("domain_id", Status::kUnknown,
              "Registry lacks native domain metadata; subsequent results compare configuration only.");
          const auto wt = Value(*writer.policies, "wire_topic_name"), rt = Value(*reader.policies, "wire_topic_name");
          if (writer.policies->contains("wire_topic_name") && reader.policies->contains("wire_topic_name") && wt != rt) {
            checks.Add("wire_topic_name", Status::kSkipped, "Different native topic names; these endpoints are not a candidate pair.");
            continue;
          }
        }
        ++pairs;
        if (Type(*writer.endpoint).empty() || Type(*reader.endpoint).empty()) checks.Add("type_name", Status::kUnknown,
            "Endpoint type metadata is incomplete; policy comparisons alone cannot establish a compatible pair.");
        checks.CommonPair();
        if (writer.driver == "dds") checks.DdsPair(writer, reader);
      }
    }
    if (pairs == 0) results.push_back({.source = source.source, .policy = "pairing", .status = Status::kSkipped,
                                     .message = "No candidate publisher/subscriber pairs; endpoint configuration checks only."});
  }
  if (snapshot.sources.empty()) results.push_back({.policy = "discovery", .status = Status::kUnknown,
                                                  .message = "No discovery source status was returned."});
  return results;
}
}  // namespace ibmw::cli::doctor
