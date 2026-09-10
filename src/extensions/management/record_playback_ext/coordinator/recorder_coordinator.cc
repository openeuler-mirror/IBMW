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
  message.identity.source_id.clear();
  message.identity.publisher_id.clear();
  message.source_driver.clear();
  if (!options_.snapshot_mode) {
    return queue_.Push(std::move(message));
  }
  std::lock_guard snapshot_input_lock(snapshot_input_mutex_);
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

void RecorderCoordinator::TriggerSnapshot() {
  MW_ASSERT(options_.snapshot_mode, "Recorder is not in snapshot mode.");
  uint64_t request_id = 0;
  {
    std::lock_guard snapshot_input_lock(snapshot_input_mutex_);
    MW_ASSERT(
        state_.load() == State::kRunning,
        "Recorder is not running.");
    const uint64_t queue_target = queue_.GetStatistics().pushed;
    std::lock_guard snapshot_lock(snapshot_mutex_);
    request_id = next_snapshot_id_++;
    snapshot_requests_.push_back(
        SnapshotRequest{.id = request_id, .queue_target = queue_target});
  }
  queue_.WakeConsumer();

  std::unique_lock snapshot_lock(snapshot_mutex_);
  snapshot_cv_.wait(snapshot_lock, [this, request_id] {
    return completed_snapshot_id_ >= request_id ||
           snapshot_writer_finished_;
  });
  if (completed_snapshot_id_ >= request_id) {
    return;
  }
  snapshot_lock.unlock();
  std::exception_ptr error;
  {
    std::lock_guard error_lock(error_mutex_);
    error = writer_error_;
  }
  if (error) {
    std::rethrow_exception(error);
  }
  MW_ASSERT(false, "Recorder stopped before completing snapshot {}.", request_id);
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
  const auto statistics = QueueStatistics();
  std::exception_ptr error;
  {
    std::lock_guard lock(error_mutex_);
    error = writer_error_;
  }
  uint64_t abandoned_messages = 0;
  if (error) {
    abandoned_messages = statistics.queued_messages;
    abandoned_messages += options_.snapshot_mode
                              ? snapshot_buffer_.size()
                              : writer_failed_messages_.load();
  }
  {
    std::lock_guard storage_lock(storage_mutex_);
    storage_->SetMessageStatistics(
        statistics.dropped + snapshot_dropped_ +
            external_dropped_.load() + abandoned_messages,
        0);
    storage_->SetCloseStatus(!error, DescribeException(error));
    storage_->Close();
  }

  if (error) {
    std::rethrow_exception(error);
  }
}

RecordQueue::Statistics RecorderCoordinator::QueueStatistics() const {
  return queue_.GetStatistics();
}

void RecorderCoordinator::RunWriter() {
  try {
    while (true) {
      auto message = options_.snapshot_mode
                         ? queue_.PopFor(std::chrono::milliseconds(50))
                         : queue_.Pop();
      const auto statistics = queue_.GetStatistics();
      if (options_.snapshot_mode) {
        const uint64_t popped_before_message =
            statistics.popped - (message ? 1 : 0);
        FlushReadySnapshots(popped_before_message);
        if (message) {
          BufferSnapshot(std::move(*message), statistics.popped);
        }
        FlushReadySnapshots(statistics.popped);
      } else if (message) {
        WriteMessage(*message);
      }
      if (!message && statistics.closed && statistics.queued_messages == 0) {
        break;
      }
    }
  } catch (...) {
    {
      std::lock_guard lock(error_mutex_);
      writer_error_ = std::current_exception();
    }
    queue_.Close();
  }
  {
    std::lock_guard snapshot_lock(snapshot_mutex_);
    snapshot_writer_finished_ = true;
  }
  snapshot_cv_.notify_all();
}

void RecorderCoordinator::WriteMessage(
    const ibmw::transport::SerializedMessage& message) {
  try {
    std::lock_guard storage_lock(storage_mutex_);
    const auto schema_id = FindOrRegisterSchema(message.topic);
    const auto channel_id = FindOrRegisterChannel(message, schema_id);
    storage_->Write(channel_id, message);
  } catch (...) {
    ++writer_failed_messages_;
    throw;
  }
}

void RecorderCoordinator::BufferSnapshot(
    ibmw::transport::SerializedMessage message,
    uint64_t queue_sequence) {
  const size_t payload_size = message.PayloadSize();
  const size_t max_bytes = options_.queue.max_bytes;
  if (max_bytes > 0 && payload_size > max_bytes) {
    ++snapshot_dropped_;
    return;
  }
  snapshot_buffer_bytes_ += payload_size;
  snapshot_buffer_.push_back(
      SnapshotBufferedMessage{
          .queue_sequence = queue_sequence,
          .message = std::move(message)});
  while (!snapshot_buffer_.empty() &&
         ((options_.queue.max_messages > 0 &&
           snapshot_buffer_.size() > options_.queue.max_messages) ||
          (max_bytes > 0 && snapshot_buffer_bytes_ > max_bytes))) {
    snapshot_buffer_bytes_ -=
        snapshot_buffer_.front().message.PayloadSize();
    snapshot_buffer_.pop_front();
    ++snapshot_dropped_;
  }
}

void RecorderCoordinator::FlushSnapshot(uint64_t queue_target) {
  while (!snapshot_buffer_.empty() &&
         snapshot_buffer_.front().queue_sequence <= queue_target) {
    WriteMessage(snapshot_buffer_.front().message);
    snapshot_buffer_bytes_ -=
        snapshot_buffer_.front().message.PayloadSize();
    snapshot_buffer_.pop_front();
  }
}

void RecorderCoordinator::FlushReadySnapshots(uint64_t popped_messages) {
  while (true) {
    SnapshotRequest request;
    {
      std::lock_guard snapshot_lock(snapshot_mutex_);
      if (snapshot_requests_.empty() ||
          snapshot_requests_.front().queue_target > popped_messages) {
        return;
      }
      request = snapshot_requests_.front();
      snapshot_requests_.pop_front();
    }

    FlushSnapshot(request.queue_target);
    {
      std::lock_guard storage_lock(storage_mutex_);
      storage_->Flush();
    }

    {
      std::lock_guard snapshot_lock(snapshot_mutex_);
      completed_snapshot_id_ = request.id;
    }
    snapshot_cv_.notify_all();
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
