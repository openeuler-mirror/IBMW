// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/recorder/record_queue.h"

#include <chrono>
#include <future>
#include <string>

#include "gtest/gtest.h"

namespace ibmw::extensions::record_playback_extension {
namespace {

using ibmw::runtime::core::transport::SerializedMessage;

SerializedMessage MakeMessage(size_t payload_size) {
  const std::string bytes(payload_size, 'x');
  const ibmw::util::BufferArrayView view(bytes.data(), bytes.size());
  SerializedMessage message;
  message.payload =
      ibmw::runtime::core::transport::CopySerializedPayload(view);
  return message;
}

TEST(RecordQueueTest, PushAndPopUpdateQueueStatistics) {
  RecordQueue queue({
      .max_messages = 2,
      .max_bytes = 16,
      .full_policy = RecordQueue::FullPolicy::kDropNewest});

  EXPECT_EQ(queue.Push(MakeMessage(4)), RecordQueue::PushResult::kPushed);
  EXPECT_EQ(queue.Push(MakeMessage(6)), RecordQueue::PushResult::kPushed);

  auto statistics = queue.GetStatistics();
  EXPECT_EQ(statistics.pushed, 2);
  EXPECT_EQ(statistics.popped, 0);
  EXPECT_EQ(statistics.queued_messages, 2);
  EXPECT_EQ(statistics.queued_bytes, 10);

  auto first = queue.TryPop();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->PayloadSize(), 4);
  auto second = queue.TryPop();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->PayloadSize(), 6);
  EXPECT_FALSE(queue.TryPop().has_value());

  statistics = queue.GetStatistics();
  EXPECT_EQ(statistics.popped, 2);
  EXPECT_EQ(statistics.queued_messages, 0);
  EXPECT_EQ(statistics.queued_bytes, 0);
}

TEST(RecordQueueTest, DropNewestRejectsFullOversizedAndInvalidMessages) {
  RecordQueue queue({
      .max_messages = 1,
      .max_bytes = 8,
      .full_policy = RecordQueue::FullPolicy::kDropNewest});

  EXPECT_EQ(queue.Push(MakeMessage(8)), RecordQueue::PushResult::kPushed);
  EXPECT_EQ(queue.Push(MakeMessage(1)), RecordQueue::PushResult::kDropped);
  EXPECT_EQ(queue.Push(MakeMessage(9)), RecordQueue::PushResult::kDropped);
  EXPECT_EQ(queue.Push(SerializedMessage{}),
            RecordQueue::PushResult::kInvalid);

  const auto statistics = queue.GetStatistics();
  EXPECT_EQ(statistics.pushed, 1);
  EXPECT_EQ(statistics.dropped, 3);
  EXPECT_EQ(statistics.queued_messages, 1);
  EXPECT_EQ(statistics.queued_bytes, 8);
}

TEST(RecordQueueTest, CloseUnblocksAWaitingProducerAndDrainsQueuedData) {
  using namespace std::chrono_literals;

  RecordQueue queue({
      .max_messages = 1,
      .max_bytes = 0,
      .full_policy = RecordQueue::FullPolicy::kBlock});
  ASSERT_EQ(queue.Push(MakeMessage(4)), RecordQueue::PushResult::kPushed);

  std::promise<void> started;
  auto started_future = started.get_future();
  auto blocked_push = std::async(std::launch::async, [&queue, &started] {
    started.set_value();
    return queue.Push(MakeMessage(4));
  });
  started_future.wait();
  EXPECT_EQ(blocked_push.wait_for(20ms), std::future_status::timeout);

  queue.Close();
  EXPECT_EQ(blocked_push.get(), RecordQueue::PushResult::kClosed);

  auto queued = queue.Pop();
  ASSERT_TRUE(queued.has_value());
  EXPECT_EQ(queued->PayloadSize(), 4);
  EXPECT_FALSE(queue.Pop().has_value());
  EXPECT_TRUE(queue.GetStatistics().closed);
}

}  // namespace
}  // namespace ibmw::extensions::record_playback_extension
