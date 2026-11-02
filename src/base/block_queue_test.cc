// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <string>
#include <thread>

#include "base/block_queue.h"

namespace ibmw::common::util {
namespace {

// API-044: items queued before Stop() are drained, then Dequeue throws.
TEST(BlockQueueTest, StopDrainsQueuedItems) {
  BlockQueue<std::string> queue;
  queue.Enqueue("a");
  queue.Enqueue(std::string("b"));
  queue.Stop();
  EXPECT_THROW(queue.Enqueue("c"), BlockQueueStoppedException);
  EXPECT_EQ(queue.Dequeue(), "a");
  EXPECT_EQ(queue.TryDequeue(), "b");
  EXPECT_EQ(queue.TryDequeue(), std::nullopt);
  EXPECT_THROW(queue.Dequeue(), BlockQueueStoppedException);
}

TEST(BlockQueueTest, StopWakesBlockedConsumer) {
  BlockQueue<int> queue;
  std::thread consumer([&] { EXPECT_THROW(queue.Dequeue(), BlockQueueStoppedException); });
  queue.Stop();
  consumer.join();
}

}  // namespace
}  // namespace ibmw::common::util
