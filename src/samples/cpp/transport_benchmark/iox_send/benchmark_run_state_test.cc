#include "benchmark_run_state.h"

#include "gtest/gtest.h"

namespace ibmw::samples::transport_benchmark::iox_send {
namespace {

TEST(SequenceAccounting, DurationModeAcceptsSamplesWithoutFixedCountState) {
  SequenceAccounting state(0, 2);
  EXPECT_EQ(state.Observe(0), SequenceResult::kWarmup);
  EXPECT_EQ(state.Observe(1), SequenceResult::kWarmup);
  EXPECT_EQ(state.Observe(2), SequenceResult::kAccepted);
  EXPECT_EQ(state.Observe(3), SequenceResult::kAccepted);
  EXPECT_EQ(state.Received(), 2U);
  EXPECT_EQ(state.Unique(), 2U);
  EXPECT_EQ(state.Missing(), 0U);
}

TEST(SequenceAccounting, FixedCountDetectsDuplicateMissingAndOrderErrors) {
  SequenceAccounting state(3, 1);
  EXPECT_EQ(state.Observe(1), SequenceResult::kAccepted);
  EXPECT_EQ(state.Observe(2), SequenceResult::kAccepted);
  EXPECT_EQ(state.Observe(2), SequenceResult::kDuplicate);
  EXPECT_EQ(state.Received(), 3U);
  EXPECT_EQ(state.Unique(), 2U);
  EXPECT_EQ(state.Missing(), 1U);
  EXPECT_EQ(state.Duplicate(), 1U);
  EXPECT_EQ(state.OutOfOrder(), 1U);
  EXPECT_FALSE(state.StrictRunValid(0, 0));
}

TEST(SequenceAccounting, ExactFixedCountRunIsValid) {
  SequenceAccounting state(3, 1);
  EXPECT_EQ(state.Observe(1), SequenceResult::kAccepted);
  EXPECT_EQ(state.Observe(2), SequenceResult::kAccepted);
  EXPECT_EQ(state.Observe(3), SequenceResult::kAccepted);
  EXPECT_TRUE(state.StrictRunValid(0, 0));
  EXPECT_FALSE(state.StrictRunValid(1, 0));
}

TEST(SubscriberAccounting, CorruptCopyDoesNotConsumeSequence) {
  SubscriberAccounting state(2, 1);
  EXPECT_EQ(state.Observe(1, true, false), SequenceResult::kOutOfRange);
  EXPECT_EQ(state.Received(), 0U);
  EXPECT_EQ(state.Unique(), 0U);
  EXPECT_EQ(state.Observe(1, true, true), SequenceResult::kAccepted);
  EXPECT_EQ(state.Observe(2, true, true), SequenceResult::kAccepted);
  EXPECT_EQ(state.Received(), 2U);
  EXPECT_EQ(state.Unique(), 2U);
  EXPECT_EQ(state.Missing(), 0U);
  EXPECT_EQ(state.InvalidPayload(), 1U);
  EXPECT_FALSE(state.StrictRunValid());
}

TEST(SubscriberAccounting, OutOfRangeCopyIsInvalidAndUnaccounted) {
  SubscriberAccounting state(2, 1);
  EXPECT_EQ(state.Observe(3, true, true), SequenceResult::kOutOfRange);
  EXPECT_EQ(state.Received(), 0U);
  EXPECT_EQ(state.Unique(), 0U);
  EXPECT_EQ(state.InvalidPayload(), 1U);
  EXPECT_FALSE(state.StrictRunValid());
}

TEST(SubscriberAccounting, WarmupFillerDoesNotInvalidateMeasuredRun) {
  SubscriberAccounting state(1, 1);
  EXPECT_EQ(state.Observe(0, true, false), SequenceResult::kWarmup);
  EXPECT_EQ(state.Observe(1, true, true), SequenceResult::kAccepted);
  EXPECT_EQ(state.InvalidPayload(), 0U);
  EXPECT_TRUE(state.StrictRunValid());
}

TEST(PublisherRunStatus, DurationCompletionDoesNotCompareAttemptsToZero) {
  const auto status = MakePublisherRunStatus(0, 2, 12, true);
  EXPECT_EQ(status.attempted_total, 12U);
  EXPECT_EQ(status.attempted_measured, 10U);
  EXPECT_TRUE(status.completion);
  EXPECT_TRUE(status.valid);
}

TEST(PublisherRunStatus, FixedCountRequiresExactAttempts) {
  EXPECT_TRUE(MakePublisherRunStatus(3, 2, 5, false).valid);
  EXPECT_FALSE(MakePublisherRunStatus(3, 2, 4, false).valid);
  EXPECT_FALSE(MakePublisherRunStatus(3, 2, 6, false).valid);
}

TEST(CsvCompletion, DurationModeRetainsHistoricalAppendBehavior) {
  EXPECT_TRUE(ShouldAppendCsv(0, false));
  EXPECT_TRUE(ShouldAppendCsv(3, true));
  EXPECT_FALSE(ShouldAppendCsv(3, false));
}

}  // namespace
}  // namespace ibmw::samples::transport_benchmark::iox_send
