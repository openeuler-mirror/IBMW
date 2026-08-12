// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/converter/rosbag2_adapter.h"

#include "base/exception.h"
#include "record_playback_ext/converter/bag_converter.h"

#if defined(IBMW_HAS_ROSBAG2)

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

#include <ament_index_cpp/get_package_share_directory.hpp>
#include <rcutils/allocator.h>
#include <rcutils/types/uint8_array.h>
#include <rosbag2_cpp/reader.hpp>
#include <rosbag2_cpp/writer.hpp>
#include <rosbag2_storage/serialized_bag_message.hpp>
#include <rosbag2_storage/storage_options.hpp>
#include <rosbag2_storage/topic_metadata.hpp>
#include <rosbag2_transport/qos.hpp>

#include "cpp/util/schema_support.h"
#include "mcap/reader.hpp"
#include "type_support/ros2/ros2_schema_support.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

using QosDescriptor = ibmw::transport::QosDescriptor;

std::string QosYaml(const QosDescriptor& descriptor);

uint64_t DurationNanoseconds(const rmw_time_t& value) {
  constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();
  if (value.nsec >= 1000000000ull ||
      value.sec > (kMax - value.nsec) / 1000000000ull) {
    return 0;
  }
  return static_cast<uint64_t>(value.sec) * 1000000000ull + value.nsec;
}

std::string HistoryName(rmw_qos_history_policy_t value) {
  switch (value) {
    case RMW_QOS_POLICY_HISTORY_KEEP_LAST:
      return "keep_last";
    case RMW_QOS_POLICY_HISTORY_KEEP_ALL:
      return "keep_all";
    case RMW_QOS_POLICY_HISTORY_UNKNOWN:
      return "unknown";
    default:
      return "system_default";
  }
}

std::string ReliabilityName(rmw_qos_reliability_policy_t value) {
  switch (value) {
    case RMW_QOS_POLICY_RELIABILITY_RELIABLE:
      return "reliable";
    case RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT:
      return "best_effort";
    case RMW_QOS_POLICY_RELIABILITY_UNKNOWN:
      return "unknown";
    default:
      return "system_default";
  }
}

std::string DurabilityName(rmw_qos_durability_policy_t value) {
  switch (value) {
    case RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL:
      return "transient_local";
    case RMW_QOS_POLICY_DURABILITY_VOLATILE:
      return "volatile";
    case RMW_QOS_POLICY_DURABILITY_UNKNOWN:
      return "unknown";
    default:
      return "system_default";
  }
}

std::string LivelinessName(rmw_qos_liveliness_policy_t value) {
  switch (value) {
    case RMW_QOS_POLICY_LIVELINESS_AUTOMATIC:
      return "automatic";
    case RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC:
      return "manual_by_topic";
    case RMW_QOS_POLICY_LIVELINESS_UNKNOWN:
      return "unknown";
    default:
      return value == static_cast<rmw_qos_liveliness_policy_t>(2)
                 ? "manual_by_node"
                 : "system_default";
  }
}

QosDescriptor ParseQosProfiles(
    const std::string& topic,
    const std::string& encoded) {
  if (encoded.empty()) {
    return {};
  }
  std::vector<rosbag2_transport::Rosbag2QoS> profiles;
  try {
    profiles =
        YAML::Load(encoded).as<std::vector<rosbag2_transport::Rosbag2QoS>>();
  } catch (const YAML::Exception&) {
    return {};
  }
  if (profiles.empty()) {
    return {};
  }
  const auto selected = profiles.size() == 1
                            ? profiles.front()
                            : rosbag2_transport::Rosbag2QoS::
                                  adapt_offer_to_recorded_offers(topic, profiles);
  const auto& profile = selected.get_rmw_qos_profile();
  QosDescriptor result;
  result.policies.emplace("history", HistoryName(profile.history));
  result.policies.emplace("depth", std::to_string(profile.depth));
  result.policies.emplace("reliability", ReliabilityName(profile.reliability));
  result.policies.emplace("durability", DurabilityName(profile.durability));
  result.policies.emplace("liveliness", LivelinessName(profile.liveliness));
  result.policies.emplace(
      "deadline_ns", std::to_string(DurationNanoseconds(profile.deadline)));
  result.policies.emplace(
      "lifespan_ns", std::to_string(DurationNanoseconds(profile.lifespan)));
  result.policies.emplace(
      "liveliness_lease_duration_ns",
      std::to_string(DurationNanoseconds(profile.liveliness_lease_duration)));
  result.policies.emplace(
      "avoid_ros_namespace_conventions",
      profile.avoid_ros_namespace_conventions ? "true" : "false");
  return result;
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  MW_ASSERT(input, "Cannot open ROS2 message definition '{}'.", path.string());
  std::stringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::string MessageTypeName(std::string_view type) {
  const auto slash = type.find('/');
  MW_ASSERT(slash != std::string_view::npos, "Invalid ROS2 message type '{}'.", type);
  const auto second = type.find('/', slash + 1);
  MW_ASSERT(second != std::string_view::npos, "Invalid ROS2 message type '{}'.", type);
  MW_ASSERT(type.substr(slash + 1, second - slash - 1) == "msg",
            "ROS2 bag conversion only supports message types, got '{}'.", type);
  return std::string(type);
}

std::filesystem::path MessageDefinitionPath(std::string_view type) {
  const auto canonical = MessageTypeName(type);
  const auto first = canonical.find('/');
  const auto last = canonical.rfind('/');
  const std::string package(canonical.substr(0, first));
  const std::string name(canonical.substr(last + 1));
  return std::filesystem::path(
      ament_index_cpp::get_package_share_directory(package)) /
         "msg" / (name + ".msg");
}

ibmw::util::SchemaDescriptor BuildSchema(std::string_view type) {
  const auto canonical = MessageTypeName(type);
  return ibmw::ros2::BuildRos2SchemaDescriptor(
      canonical, [](std::string_view nested) {
        return ReadTextFile(MessageDefinitionPath(nested));
      });
}

std::vector<std::filesystem::path> RosbagMcapPaths(
    const std::filesystem::path& bag_path,
    const rosbag2_storage::BagMetadata& metadata) {
  std::vector<std::filesystem::path> result;
  if (metadata.storage_identifier != "mcap") {
    return result;
  }
  for (const auto& relative : metadata.relative_file_paths) {
    const auto path = bag_path / relative;
    if (std::filesystem::is_regular_file(path)) {
      result.emplace_back(path);
    }
  }
  return result;
}

std::unordered_map<std::string, ibmw::util::SchemaDescriptor>
ReadMcapSchemas(const std::vector<std::filesystem::path>& paths) {
  std::unordered_map<std::string, ibmw::util::SchemaDescriptor> result;
  for (const auto& path : paths) {
    mcap::McapReader reader;
    const auto open_result = reader.open(path.string());
    MW_ASSERT(open_result.ok(), "Cannot open ROS2 MCAP '{}': {}.", path.string(),
              open_result.message);
    const auto summary_result =
        reader.readSummary(mcap::ReadSummaryMethod::AllowFallbackScan);
    MW_ASSERT(summary_result.ok(), "Cannot read ROS2 MCAP summary '{}': {}.",
              path.string(), summary_result.message);
    const auto schemas = reader.schemas();
    for (const auto& [channel_id, channel] : reader.channels()) {
      static_cast<void>(channel_id);
      if (!channel || channel->schemaId == 0) {
        continue;
      }
      const auto schema = schemas.find(channel->schemaId);
      if (schema == schemas.end() || !schema->second ||
          schema->second->encoding != "ros2msg") {
        continue;
      }
      const auto& value = *schema->second;
      const std::string data(
          reinterpret_cast<const char*>(value.data.data()), value.data.size());
      if (data.empty()) {
        continue;
      }
      const std::string type = value.name;
      auto descriptor = ibmw::util::SchemaDescriptor{
          .type_name = "ros2:" + type,
          .serialization_type = "ros2",
          .schema_name = type,
          .schema_encoding = value.encoding,
          .schema_data = data,
          .schema_hash = ibmw::util::ComputeSchemaHash(data)};
      const auto existing = result.find(channel->topic);
      MW_ASSERT(
          existing == result.end() ||
              (existing->second.schema_name == descriptor.schema_name &&
               existing->second.schema_hash == descriptor.schema_hash),
          "ROS2 MCAP split files contain conflicting Schema for topic '{}'.",
          channel->topic);
      result.insert_or_assign(channel->topic, std::move(descriptor));
    }
    reader.close();
  }
  return result;
}

std::string RosbagTopicKey(const rosbag2_storage::TopicMetadata& topic) {
  return topic.name + '\0' + topic.type + '\0' + topic.serialization_format;
}

/*
 * rosbag2 uses one TopicMetadata entry per topic.  IBMW may have multiple
 * Channels for that topic, but exporting them is valid only when their ROS2
 * type and serialization are identical.
 */
rosbag2_storage::TopicMetadata ToRosbagTopic(const BagChannelInfo& channel) {
  const auto& schema = channel.topic.schema;
  MW_ASSERT(schema.type_name.starts_with("ros2:"),
            "IBMW channel '{}' is not a ROS2 message.", channel.topic.topic);
  MW_ASSERT(schema.schema_encoding == "ros2msg" &&
                (channel.topic.serialization_type == "ros2" ||
                 channel.topic.serialization_type == "cdr"),
            "IBMW channel '{}' does not contain a ROS2 message Schema.",
            channel.topic.topic);
  const std::string ros_type = schema.type_name.substr(5);
  MW_ASSERT(
      channel.topic.msg_type == schema.type_name &&
          schema.schema_name == ros_type && !schema.schema_data.empty() &&
          schema.schema_hash ==
              ibmw::util::ComputeSchemaHash(schema.schema_data),
      "IBMW channel '{}' has an invalid ROS2 Schema.", channel.topic.topic);
  return rosbag2_storage::TopicMetadata{
      .name = channel.topic.topic,
      .type = ros_type,
      .serialization_format = "cdr",
      .offered_qos_profiles = QosYaml(channel.topic.qos)};
}

void MergeQosProfiles(
    std::string& existing,
    const std::string& additional) {
  if (additional.empty()) {
    return;
  }
  if (existing.empty()) {
    existing = additional;
    return;
  }
  auto result = YAML::Load(existing);
  const auto incoming = YAML::Load(additional);
  MW_ASSERT(result.IsSequence() && incoming.IsSequence(),
            "ROS2 QoS profiles must be YAML sequences.");
  std::set<std::string> encoded;
  for (const auto& profile : result) {
    encoded.emplace(YAML::Dump(profile));
  }
  for (const auto& profile : incoming) {
    if (encoded.emplace(YAML::Dump(profile)).second) {
      result.push_back(profile);
    }
  }
  existing = YAML::Dump(result);
}

uint64_t TimestampFromRosbag(const std::chrono::time_point<std::chrono::high_resolution_clock>& value) {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count());
}

std::shared_ptr<ibmw::util::BufferArrayView> CopyRosbagPayload(
    const rcutils_uint8_array_t& array) {
  auto bytes = std::make_shared<std::vector<uint8_t>>(array.buffer_length);
  if (array.buffer_length != 0) {
    std::memcpy(bytes->data(), array.buffer, array.buffer_length);
  }
  auto* view = new ibmw::util::BufferArrayView(bytes->data(), bytes->size());
  return std::shared_ptr<ibmw::util::BufferArrayView>(
      view, [bytes](const ibmw::util::BufferArrayView* value) { delete value; });
}

rcutils_uint8_array_t MakeRosbagPayload(std::string_view payload) {
  auto array = rcutils_get_zero_initialized_uint8_array();
  const auto allocator = rcutils_get_default_allocator();
  MW_ASSERT(
      rcutils_uint8_array_init(&array, payload.size(), &allocator) == RCUTILS_RET_OK,
      "Cannot allocate ROS2 serialized message payload.");
  if (!payload.empty()) {
    std::memcpy(array.buffer, payload.data(), payload.size());
  }
  array.buffer_length = payload.size();
  return array;
}

std::string QosYaml(const QosDescriptor& descriptor) {
  if (descriptor.Empty()) {
    return {};
  }
  rosbag2_transport::Rosbag2QoS qos;
  auto& profile = qos.get_rmw_qos_profile();
  const auto find = [&descriptor](std::string_view key) {
    return descriptor.policies.find(std::string(key));
  };
  if (const auto value = find("history"); value != descriptor.policies.end()) {
    profile.history = value->second == "keep_all"
                          ? RMW_QOS_POLICY_HISTORY_KEEP_ALL
                          : value->second == "keep_last"
                                ? RMW_QOS_POLICY_HISTORY_KEEP_LAST
                                : value->second == "unknown"
                                      ? RMW_QOS_POLICY_HISTORY_UNKNOWN
                                      : RMW_QOS_POLICY_HISTORY_SYSTEM_DEFAULT;
  }
  if (const auto value = find("depth"); value != descriptor.policies.end()) {
    profile.depth = std::stoull(value->second);
  }
  if (const auto value = find("reliability"); value != descriptor.policies.end()) {
    profile.reliability = value->second == "best_effort"
                              ? RMW_QOS_POLICY_RELIABILITY_BEST_EFFORT
                              : value->second == "reliable"
                                    ? RMW_QOS_POLICY_RELIABILITY_RELIABLE
                                    : value->second == "unknown"
                                          ? RMW_QOS_POLICY_RELIABILITY_UNKNOWN
                                          : RMW_QOS_POLICY_RELIABILITY_SYSTEM_DEFAULT;
  }
  if (const auto value = find("durability"); value != descriptor.policies.end()) {
    profile.durability = value->second == "transient_local"
                             ? RMW_QOS_POLICY_DURABILITY_TRANSIENT_LOCAL
                             : value->second == "volatile"
                                   ? RMW_QOS_POLICY_DURABILITY_VOLATILE
                                   : value->second == "unknown"
                                         ? RMW_QOS_POLICY_DURABILITY_UNKNOWN
                                         : RMW_QOS_POLICY_DURABILITY_SYSTEM_DEFAULT;
  }
  if (const auto value = find("liveliness"); value != descriptor.policies.end()) {
    profile.liveliness = value->second == "manual_by_topic"
                             ? RMW_QOS_POLICY_LIVELINESS_MANUAL_BY_TOPIC
                             : value->second == "manual_by_node"
                                   ? static_cast<rmw_qos_liveliness_policy_t>(2)
                             : value->second == "automatic"
                                   ? RMW_QOS_POLICY_LIVELINESS_AUTOMATIC
                                   : value->second == "unknown"
                                         ? RMW_QOS_POLICY_LIVELINESS_UNKNOWN
                                         : RMW_QOS_POLICY_LIVELINESS_SYSTEM_DEFAULT;
  }
  const auto set_duration = [&profile, &descriptor](
                                std::string_view name, rmw_time_t& target) {
    const auto value = descriptor.policies.find(std::string(name) + "_ns");
    if (value == descriptor.policies.end()) {
      return;
    }
    const uint64_t ns = std::stoull(value->second);
    target.sec = static_cast<int64_t>(ns / 1000000000ull);
    target.nsec = static_cast<uint64_t>(ns % 1000000000ull);
  };
  set_duration("deadline", profile.deadline);
  set_duration("lifespan", profile.lifespan);
  set_duration("liveliness_lease_duration", profile.liveliness_lease_duration);
  if (const auto value = find("avoid_ros_namespace_conventions");
      value != descriptor.policies.end()) {
    profile.avoid_ros_namespace_conventions =
        value->second == "true" || value->second == "1";
  }
  YAML::Node profiles(YAML::NodeType::Sequence);
  profiles.push_back(qos);
  return YAML::Dump(profiles);
}

}  // namespace

void Rosbag2BagReader::Open(
    const std::filesystem::path& bag_path,
    const ReaderOpenOptions& options) {
  const auto absolute = std::filesystem::absolute(bag_path);
  const bool direct_file = std::filesystem::is_regular_file(absolute);
  MW_ASSERT(direct_file || std::filesystem::is_directory(absolute),
            "rosbag2 input '{}' must be a bag directory or data file.",
            absolute.string());
  if (!direct_file) {
    const auto metadata_path = absolute / "metadata.yaml";
    MW_ASSERT(std::filesystem::is_regular_file(metadata_path),
              "rosbag2 metadata '{}' does not exist.", metadata_path.string());
  }
  rosbag2_cpp::Reader reader;
  rosbag2_storage::StorageOptions storage_options;
  storage_options.uri = absolute.string();
  storage_options.storage_config_uri =
      options.storage_config_file.string();
  if (direct_file) {
    const auto extension = absolute.extension().string();
    MW_ASSERT(extension == ".mcap" || extension == ".db3",
              "Unsupported standalone rosbag2 data file '{}'.",
              absolute.string());
    storage_options.storage_id = extension == ".mcap" ? "mcap" : "sqlite3";
  }
  reader.open(storage_options);
  const auto& metadata = reader.get_metadata();
  info_ = {};
  info_.bag_path = std::filesystem::canonical(absolute);
  info_.version = static_cast<uint32_t>(metadata.version);
  info_.storage_id = "rosbag2:" + metadata.storage_identifier;
  info_.message_count = metadata.message_count;
  info_.start_timestamp = TimestampFromRosbag(metadata.starting_time);
  info_.end_timestamp = info_.start_timestamp + metadata.duration.count();
  info_.closed_cleanly = true;
  info_.compression_mode = metadata.compression_mode.empty() ? "none" : metadata.compression_mode;
  info_.compression_format = metadata.compression_format;
  for (const auto& file : metadata.files) {
    const auto path = direct_file ? absolute : info_.bag_path / file.path;
    info_.files.emplace_back(BagFileInfo{
        .path = path,
        .start_timestamp = TimestampFromRosbag(file.starting_time),
        .end_timestamp = TimestampFromRosbag(file.starting_time) + file.duration.count(),
        .message_count = file.message_count,
        .size = std::filesystem::is_regular_file(path) ? std::filesystem::file_size(path) : 0});
    info_.total_size += info_.files.back().size;
  }
  const auto embedded_schemas =
      (direct_file && absolute.extension() == ".mcap")
          ? ReadMcapSchemas({absolute})
          : ReadMcapSchemas(RosbagMcapPaths(info_.bag_path, metadata));
  const auto topics = metadata.topics_with_message_count;
  for (size_t index = 0; index < topics.size(); ++index) {
    const auto& topic = topics[index].topic_metadata;
    MW_ASSERT(
        topic.serialization_format == "cdr",
        "ROS2 topic '{}' uses unsupported serialization format '{}'.",
        topic.name, topic.serialization_format);
    const auto embedded = embedded_schemas.find(topic.name);
    const auto schema = embedded == embedded_schemas.end()
                            ? BuildSchema(topic.type)
                            : embedded->second;
    MW_ASSERT(
        schema.schema_name == topic.type,
        "ROS2 MCAP topic '{}' Schema type '{}' does not match metadata type '{}'.",
        topic.name, schema.schema_name, topic.type);
    const uint16_t schema_id = static_cast<uint16_t>(info_.schemas.size());
    const uint16_t channel_id = static_cast<uint16_t>(info_.channels.size());
    info_.schemas.emplace_back(BagSchemaInfo{.id = schema_id, .descriptor = schema, .hash_valid = true});
    info_.channels.emplace_back(BagChannelInfo{
        .id = channel_id,
        .schema_id = schema_id,
        .topic = {.topic = topic.name,
                  .msg_type = schema.type_name,
                  .serialization_type = "ros2",
                  .schema = schema,
                  .qos = ParseQosProfiles(
                      topic.name, topic.offered_qos_profiles)},
        .message_count = topics[index].message_count});
  }
  bag_path_ = direct_file ? absolute : info_.bag_path;
  storage_id_ = metadata.storage_identifier;
  storage_config_file_ = options.storage_config_file;
  reader.close();
  MW_ASSERT(options.expected_storage_id.empty() ||
                options.expected_storage_id == "rosbag2" ||
                options.expected_storage_id == storage_id_ ||
                options.expected_storage_id == info_.storage_id,
            "Requested input format '{}' is not rosbag2.", options.expected_storage_id);
}

void Rosbag2BagReader::ReadMessages(
    const BagReadOptions& options,
    const MessageCallback& callback) const {
  MW_ASSERT(callback, "Bag message callback cannot be empty.");
  rosbag2_cpp::Reader reader;
  rosbag2_storage::StorageOptions storage_options;
  storage_options.uri = bag_path_.string();
  storage_options.storage_id = storage_id_;
  storage_options.storage_config_uri = storage_config_file_.string();
  reader.open(storage_options);
  std::unordered_map<std::string, BagChannelInfo> channels;
  for (const auto& channel : info_.channels) {
    channels.emplace(channel.topic.topic, channel);
  }
  const std::set<std::string> selected_topics(options.topics.begin(), options.topics.end());
  while (reader.has_next()) {
    const auto message = reader.read_next();
    if (!message || !message->serialized_data) {
      continue;
    }
    const uint64_t timestamp = message->time_stamp;
    if (timestamp < options.start_timestamp ||
        (options.end_timestamp != 0 && timestamp >= options.end_timestamp) ||
        (!selected_topics.empty() && !selected_topics.contains(message->topic_name))) {
      continue;
    }
    const auto channel = channels.find(message->topic_name);
    MW_ASSERT(channel != channels.end(), "rosbag2 message references unknown topic '{}'.",
              message->topic_name);
    BagMessage converted{
        .channel = channel->second,
        .log_time = timestamp,
        .publish_time = timestamp,
        .sequence = 0,
        .payload = CopyRosbagPayload(*message->serialized_data)};
    if (!callback(std::move(converted))) {
      break;
    }
  }
  reader.close();
}

BagConvertResult ConvertBagToRosbag2(
    const BagReader& reader,
    const StorageOptions& options) {
  MW_ASSERT(options.storage_id == "mcap" || options.storage_id == "sqlite3",
            "ROS2 output storage must be mcap or sqlite3.");
  MW_ASSERT(options.compression_mode == "none",
            "Compression options are not supported for rosbag2 output.");
  std::map<std::string, rosbag2_storage::TopicMetadata> registered;
  for (const auto& channel : reader.Info().channels) {
    auto metadata = ToRosbagTopic(channel);
    const auto existing = registered.find(metadata.name);
    if (existing == registered.end()) {
      registered.emplace(metadata.name, std::move(metadata));
      continue;
    }
    MW_ASSERT(
        RosbagTopicKey(existing->second) == RosbagTopicKey(metadata),
        "ROS2 topic '{}' is associated with multiple message types or "
        "serialization formats.",
        metadata.name);
    MergeQosProfiles(
        existing->second.offered_qos_profiles,
        metadata.offered_qos_profiles);
  }
  MW_ASSERT(!std::filesystem::exists(options.output_path) ||
                (std::filesystem::is_directory(options.output_path) &&
                 std::filesystem::is_empty(options.output_path)),
            "ROS2 output '{}' must not already contain files.", options.output_path.string());
  rosbag2_cpp::Writer writer;
  rosbag2_storage::StorageOptions storage_options;
  storage_options.uri = std::filesystem::absolute(options.output_path).string();
  storage_options.storage_id = options.storage_id;
  storage_options.max_bagfile_size = options.max_bag_size;
  storage_options.max_bagfile_duration = options.max_bag_duration_ns / 1000000000ull;
  storage_options.storage_preset_profile = options.storage_preset_profile;
  storage_options.storage_config_uri = options.storage_config_file.string();
  writer.open(storage_options);
  for (const auto& [topic, metadata] : registered) {
    static_cast<void>(topic);
    writer.create_topic(metadata);
  }
  uint64_t count = 0;
  reader.ReadMessages({}, [&](BagMessage&& message) {
    const auto payload = message.payload->JoinToString();
    auto output = std::make_shared<rosbag2_storage::SerializedBagMessage>();
    output->topic_name = message.channel.topic.topic;
    output->time_stamp = message.log_time;
    output->serialized_data = std::shared_ptr<rcutils_uint8_array_t>(
        new rcutils_uint8_array_t(MakeRosbagPayload(payload)),
        [](rcutils_uint8_array_t* value) {
          const rcutils_ret_t result = rcutils_uint8_array_fini(value);
          static_cast<void>(result);
          delete value;
        });
    writer.write(output);
    ++count;
    return true;
  });
  writer.close();
  return BagConvertResult{
      .input_storage_id = reader.Info().storage_id,
      .output_storage_id = "rosbag2:" + options.storage_id,
      .output_path = std::filesystem::absolute(options.output_path),
      .input_message_count = reader.Info().message_count,
      .output_message_count = count,
      .schema_count = reader.Info().schemas.size(),
      .channel_count = registered.size(),
      .input_closed_cleanly = reader.Info().closed_cleanly};
}

}  // namespace ibmw::extensions::record_playback_extension

#else

namespace ibmw::extensions::record_playback_extension {
void Rosbag2BagReader::Open(const std::filesystem::path&, const ReaderOpenOptions&) {
  MW_ASSERT(false, "IBMW was built without rosbag2 support.");
}
void Rosbag2BagReader::ReadMessages(const BagReadOptions&, const MessageCallback&) const {
  MW_ASSERT(false, "IBMW was built without rosbag2 support.");
}
BagConvertResult ConvertBagToRosbag2(const BagReader&, const StorageOptions&) {
  MW_ASSERT(false, "IBMW was built without rosbag2 support.");
  return {};
}
}  // namespace ibmw::extensions::record_playback_extension

#endif
