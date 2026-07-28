// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/coordinator/recorder_coordinator.h"

#include <utility>

#include "base/exception.h"

namespace ibmw::extensions::record_playback_extension {

namespace {

std::string DescribeException(const std::exception_ptr& error) {
  if (!error) {
    return {};
  }
  try {
    std::rethrow_exception(error);
  } catch (const std::exception& exception) {
    return exception.what();
  } catch (...) {
    return "unknown Recorder writer failure";
  }
}

}  // namespace

RecorderCoordinator::RecorderCoordinator(
    Options options,
    std::unique_ptr<Storage> storage)
    : options_(std::move(options)),
      storage_(std::move(storage)),
      queue_(options_.queue) {
  MW_ASSERT(storage_ != nullptr, "Recorder storage cannot be null.");
}

RecorderCoordinator::~RecorderCoordinator() {
  try {
    Close();
  } catch (...) {
  }
}

void RecorderCoordinator::Initialize() {
  State expected = State::kCreated;
  MW_ASSERT(
      state_.compare_exchange_strong(expected, State::kInitialized),
      "RecorderCoordinator can only be initialized once.");
  try {
    storage_->Open(options_.storage);
  } catch (...) {
    state_ = State::kClosed;
    throw;
  }
}

void RecorderCoordinator::Start() {
  State expected = State::kInitialized;
  MW_ASSERT(
      state_.compare_exchange_strong(expected, State::kRunning),
      "RecorderCoordinator must be initialized before Start.");
  writer_thread_ = std::thread([this] { RunWriter(); });
}

RecordQueue::PushResult RecorderCoordinator::Accept(
    ibmw::transport::SerializedMessage message) {
  if (state_.load() != State::kRunning) {
    return RecordQueue::PushResult::kClosed;
  }
  {
    std::lock_guard lock(error_mutex_);
    if (writer_error_) {
      return RecordQueue::PushResult::kClosed;
    }
  }
  return queue_.Push(std::move(message));
}

void RecorderCoordinator::RegisterChannel(
    StorageChannelDescriptor channel) {
  MW_ASSERT(
      state_.load() == State::kRunning,
      "Recorder must be running before registering a Channel.");
  std::lock_guard storage_lock(storage_mutex_);
  const auto schema_id = FindOrRegisterSchema(channel.topic);
  static_cast<void>(FindOrRegisterChannel(channel, schema_id));
}

void RecorderCoordinator::Close() {
  const State previous = state_.exchange(State::kClosed);
  if (previous == State::kClosed || previous == State::kCreated) {
    return;
  }

  queue_.Close();
  if (writer_thread_.joinable()) {
    writer_thread_.join();
  }
  std::exception_ptr error;
  {
    std::lock_guard lock(error_mutex_);
    error = writer_error_;
  }
  {
    std::lock_guard storage_lock(storage_mutex_);
    storage_->SetCloseStatus(!error, DescribeException(error));
    storage_->Close();
  }

  if (error) {
    std::rethrow_exception(error);
  }
}

void RecorderCoordinator::RunWriter() {
  try {
    while (auto message = queue_.Pop()) {
      std::lock_guard storage_lock(storage_mutex_);
      const auto schema_id = FindOrRegisterSchema(message->topic);
      const auto channel_id = FindOrRegisterChannel(*message, schema_id);
      storage_->Write(channel_id, *message);
    }
  } catch (...) {
    {
      std::lock_guard lock(error_mutex_);
      writer_error_ = std::current_exception();
    }
    queue_.Close();
  }
}
StorageSchemaId RecorderCoordinator::FindOrRegisterSchema(
    const ibmw::transport::TopicDescriptor& topic) {
  const std::string key = SchemaKey(topic);
  const auto existing = schema_ids_.find(key);
  if (existing != schema_ids_.end()) {
    const auto descriptor = schemas_by_key_.find(key);
    MW_ASSERT(
        descriptor != schemas_by_key_.end(),
        "Recorder Schema index is inconsistent for type '{}'.",
        topic.msg_type);
    MW_ASSERT(
        descriptor->second.schema_hash == topic.schema.schema_hash &&
            descriptor->second.schema_name == topic.schema.schema_name &&
            descriptor->second.schema_encoding ==
                topic.schema.schema_encoding,
        "Conflicting Schema for type '{}' serialization '{}'.",
        topic.msg_type, topic.serialization_type);
    return existing->second;
  }

  MW_ASSERT(
      topic.schema.type_name == topic.msg_type &&
          topic.schema.serialization_type == topic.serialization_type &&
          !topic.schema.schema_name.empty() &&
          !topic.schema.schema_encoding.empty() &&
          !topic.schema.schema_data.empty() &&
          topic.schema.schema_hash ==
              ibmw::util::ComputeSchemaHash(topic.schema.schema_data),
      "Topic '{}' contains an invalid Schema descriptor.", topic.topic);
  const auto id = storage_->RegisterSchema(topic.schema);
  schema_ids_.emplace(key, id);
  schemas_by_key_.emplace(key, topic.schema);
  return id;
}

StorageChannelId RecorderCoordinator::FindOrRegisterChannel(
    const ibmw::transport::SerializedMessage& message,
    StorageSchemaId schema_id) {
  return FindOrRegisterChannel(
      StorageChannelDescriptor{.topic = message.topic}, schema_id);
}

StorageChannelId RecorderCoordinator::FindOrRegisterChannel(
    const StorageChannelDescriptor& descriptor,
    StorageSchemaId schema_id) {
  const std::string key = ChannelKey(descriptor.topic);
  const auto existing = channel_ids_.find(key);
  if (existing != channel_ids_.end()) {
    return existing->second;
  }

  const auto id = storage_->RegisterChannel(descriptor, schema_id);
  channel_ids_.emplace(key, id);
  return id;
}

std::string RecorderCoordinator::SchemaKey(
    const ibmw::transport::TopicDescriptor& topic) {
  return topic.msg_type + '\0' + topic.serialization_type;
}

std::string RecorderCoordinator::ChannelKey(
    const ibmw::transport::TopicDescriptor& topic) {
  return topic.topic + '\0' + topic.msg_type + '\0' +
         topic.serialization_type + '\0' + topic.schema.schema_hash;
}

}  // namespace ibmw::extensions::record_playback_extension
