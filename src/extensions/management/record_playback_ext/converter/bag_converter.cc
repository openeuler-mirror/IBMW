// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/converter/bag_converter.h"

#include <exception>
#include <string>
#include <unordered_map>

#include "base/exception.h"
#include "record_playback_ext/storage/bag_reader_factory.h"
#include "record_playback_ext/storage/storage_factory.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

struct RegisteredChannel {
  StorageChannelId id = 0;
  ibmw::transport::TopicDescriptor topic;
};

std::string ChannelKey(const BagChannelInfo& channel) {
  return channel.topic.topic + '\0' + channel.topic.msg_type + '\0' +
         channel.topic.serialization_type + '\0' +
         channel.topic.schema.schema_hash;
}

StorageChannelDescriptor ToStorageChannel(const BagChannelInfo& channel) {
  return StorageChannelDescriptor{.topic = channel.topic};
}

std::string CurrentExceptionMessage(const std::exception_ptr& error) {
  try {
    std::rethrow_exception(error);
  } catch (const std::exception& exception) {
    return exception.what();
  } catch (...) {
    return "unknown conversion error";
  }
}

std::filesystem::path NormalizedPath(const std::filesystem::path& path) {
  return std::filesystem::weakly_canonical(std::filesystem::absolute(path));
}

}  // namespace

BagConvertResult ConvertBag(const BagConvertOptions& options) {
  MW_ASSERT(!options.input_path.empty(), "Bag conversion input cannot be empty.");
  MW_ASSERT(
      !options.output.output_path.empty(),
      "Bag conversion output cannot be empty.");
  MW_ASSERT(
      NormalizedPath(options.input_path) !=
          NormalizedPath(options.output.output_path),
      "Bag conversion input and output must be different paths.");

  auto reader = CreateBagReader(
      options.input_path,
      ReaderOpenOptions{
          .expected_storage_id = options.input_storage_id,
          .storage_config_file = options.input_storage_config_file});
  const auto& input_info = reader->Info();

  auto storage = CreateStorage(options.output.storage_id);
  storage->Open(options.output);

  std::unordered_map<std::string, RegisteredChannel> channel_ids;
  const auto register_channel =
      [&](const BagChannelInfo& channel) -> StorageChannelId {
    const std::string key = ChannelKey(channel);
    const auto existing = channel_ids.find(key);
    if (existing != channel_ids.end()) {
      MW_ASSERT(
          existing->second.topic.serialization_type ==
                  channel.topic.serialization_type &&
              existing->second.topic.schema.schema_hash ==
                  channel.topic.schema.schema_hash,
          "Conflicting Channel descriptor for topic '{}'.",
          channel.topic.topic);
      return existing->second.id;
    }

    const auto schema_id = storage->RegisterSchema(channel.topic.schema);
    const auto channel_id =
        storage->RegisterChannel(ToStorageChannel(channel), schema_id);
    channel_ids.emplace(
        key, RegisteredChannel{.id = channel_id, .topic = channel.topic});
    return channel_id;
  };

  try {
    for (const auto& schema : input_info.schemas) {
      static_cast<void>(storage->RegisterSchema(schema.descriptor));
    }
    for (const auto& channel : input_info.channels) {
      static_cast<void>(register_channel(channel));
    }

    reader->ReadMessages(
        {},
        [&](BagMessage&& input) {
          const auto channel_id = register_channel(input.channel);
          storage->Write(
              channel_id,
              ibmw::transport::SerializedMessage{
                  .topic = input.channel.topic,
                  .identity = {.sequence = input.sequence},
                  .publish_timestamp = input.publish_time,
                  .receive_timestamp = input.log_time,
                  .payload = std::move(input.payload)});
          return true;
        });

    MW_ASSERT(
        storage->Summary().message_count == input_info.message_count,
        "Converted message count {} does not match input message count {}.",
        storage->Summary().message_count, input_info.message_count);
    storage->SetMessageStatistics(
        input_info.dropped_message_count,
        input_info.duplicate_message_count);
    storage->SetCloseStatus(true, {});
    storage->Close();
  } catch (...) {
    const auto error = std::current_exception();
    try {
      storage->SetMessageStatistics(
          input_info.dropped_message_count,
          input_info.duplicate_message_count);
      storage->SetCloseStatus(
          false, "Bag conversion failed: " + CurrentExceptionMessage(error));
      storage->Close();
    } catch (...) {
    }
    std::rethrow_exception(error);
  }

  const auto& summary = storage->Summary();
  return BagConvertResult{
      .input_storage_id = input_info.storage_id,
      .output_storage_id = options.output.storage_id,
      .output_path = summary.output_path,
      .input_message_count = input_info.message_count,
      .output_message_count = summary.message_count,
      .schema_count = input_info.schemas.size(),
      .channel_count = channel_ids.size(),
      .input_closed_cleanly = input_info.closed_cleanly};
}

}  // namespace ibmw::extensions::record_playback_extension
