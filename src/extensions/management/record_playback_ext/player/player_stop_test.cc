// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "record_playback_ext/player/player.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

std::shared_ptr<ibmw::util::BufferArrayView> PlayerTestPayload() {
  auto storage = std::make_shared<std::string>("payload");
  return std::shared_ptr<ibmw::util::BufferArrayView>(
      new ibmw::util::BufferArrayView(storage->data(), storage->size()),
      [storage](const ibmw::util::BufferArrayView* view) { delete view; });
}

class ControlledBagReader final : public BagReader {
 public:
  enum class Mode {
    kEmptyUntilStopped,
    kProduceMessages,
  };

  ControlledBagReader(Mode mode, const PlayerControl& control)
      : mode_(mode), control_(control) {
    info_.bag_path = "controlled-player-test";
    info_.start_timestamp = 100;
    info_.end_timestamp = 103;
    info_.message_count = 4;
    info_.channels.emplace_back(BagChannelInfo{
        .id = 1,
        .schema_id = 1,
        .topic =
            {.topic = "/player_stop",
             .msg_type = "test:PlayerMessage",
             .serialization_type = "cdr"},
        .message_count = 4});
  }

  void Open(const std::filesystem::path&, const ReaderOpenOptions&) override {}

  void ReadMessages(
      const BagReadOptions&,
      const MessageCallback& callback) const override {
    {
      std::lock_guard lock(mutex_);
      entered_ = true;
    }
    cv_.notify_all();
    if (mode_ == Mode::kEmptyUntilStopped) {
      while (!control_.IsStopped()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      return;
    }

    for (uint32_t sequence = 0; sequence < 4; ++sequence) {
      {
        std::lock_guard lock(mutex_);
        ++callback_attempts_;
      }
      cv_.notify_all();
      BagMessage message{
          .channel = info_.channels.front(),
          .log_time = info_.start_timestamp + sequence,
          .publish_time = info_.start_timestamp + sequence,
          .sequence = sequence,
          .payload = PlayerTestPayload()};
      if (!callback(std::move(message))) {
        return;
      }
    }
  }

  const BagInfo& Info() const noexcept override { return info_; }

  bool WaitUntilEntered(std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return entered_; });
  }

  bool WaitForCallbackAttempts(
      size_t count,
      std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    return cv_.wait_for(
        lock, timeout,
        [this, count] { return callback_attempts_ >= count; });
  }

 private:
  Mode mode_;
  const PlayerControl& control_;
  BagInfo info_;
  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  mutable bool entered_ = false;
  mutable size_t callback_attempts_ = 0;
};

BagPlayer MakePlayer(
    const BagReader& reader,
    PlayerControl& control,
    PlayerOptions options) {
  return BagPlayer(
      reader, std::move(options), control, {},
      [](const TopicAdvertisement&) { return true; },
      [](const TopicAdvertisement&,
         const ibmw::transport::SerializedMessage&) {});
}

TEST(BagPlayerStopTest, StopCompletesWhileReadAheadQueueIsEmpty) {
  PlayerControl control(false);
  ControlledBagReader reader(
      ControlledBagReader::Mode::kEmptyUntilStopped, control);
  auto player = MakePlayer(reader, control, PlayerOptions{});
  auto play = std::async(std::launch::async, [&] { return player.Play(); });
  ASSERT_TRUE(reader.WaitUntilEntered(std::chrono::seconds(1)));

  control.Stop();

  ASSERT_EQ(
      play.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_EQ(play.get(), 0U);
}

TEST(BagPlayerStopTest, StopCompletesWhileReadAheadQueueIsFull) {
  PlayerControl control(true);
  ControlledBagReader reader(
      ControlledBagReader::Mode::kProduceMessages, control);
  auto player = MakePlayer(
      reader, control,
      PlayerOptions{.read_ahead_queue_size = 1, .start_paused = true});
  auto play = std::async(std::launch::async, [&] { return player.Play(); });
  ASSERT_TRUE(
      reader.WaitForCallbackAttempts(3, std::chrono::seconds(1)));

  control.Stop();

  ASSERT_EQ(
      play.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_EQ(play.get(), 0U);
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
