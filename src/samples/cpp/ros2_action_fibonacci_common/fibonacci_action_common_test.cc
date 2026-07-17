// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "fibonacci_action_client_runtime.h"
#include "fibonacci_action_common.h"
#include "fibonacci_action_fastdds_helpers.h"
#include "fibonacci_action_server_runtime.h"
#include "internal/ros2_action_dds/action_client_observer.h"
#include "internal/ros2_action_dds/action_goal_ledger.h"

namespace ibmw::samples::ros2_action_fibonacci {
namespace {

struct TestUuid {
  std::array<uint8_t, 16> uuid{};
};

struct TestTime {
  int32_t sec = 0;
  uint32_t nanosec = 0;
};

struct TestGoalInfo {
  TestUuid goal_id;
};

struct TestStatus {
  TestGoalInfo goal_info;
  int8_t status = kClientGoalStatusUnknown;
};

using TestClock = std::chrono::steady_clock;
using TestLedger = ibmw::internal::ros2_action_dds::ActionGoalLedger<TestUuid, TestTime, int, TestClock>;
using TestObserver = ibmw::internal::ros2_action_dds::ActionClientObserver<std::string>;
using TestRuntime = FibonacciActionServerRuntime<TestUuid, TestTime, int, TestClock>;
using TestClientRuntime = FibonacciActionClientRuntime;

TestUuid MakeUuid(uint8_t value) {
  TestUuid uuid;
  uuid.uuid[15] = value;
  return uuid;
}

template <typename StatusSnapshotT>
const typename std::decay_t<decltype(std::declval<StatusSnapshotT>().status_list)>::value_type*
FindStatus(const StatusSnapshotT& snapshot, const TestUuid& goal_id) {
  const auto key = UuidToString(goal_id);
  for (const auto& status : snapshot.status_list) {
    if (UuidToString(status.goal_id) == key) {
      return &status;
    }
  }
  return nullptr;
}

TEST(FibonacciActionCommon, BuildsEndpointNamesWithRos2Prefixes) {
  const auto names = MakeEndpointNames("/fibonacci");
  EXPECT_EQ(names.send_goal_request, "rq/fibonacci/_action/send_goalRequest");
  EXPECT_EQ(names.send_goal_reply, "rr/fibonacci/_action/send_goalReply");
  EXPECT_EQ(names.get_result_request, "rq/fibonacci/_action/get_resultRequest");
  EXPECT_EQ(names.get_result_reply, "rr/fibonacci/_action/get_resultReply");
  EXPECT_EQ(names.cancel_goal_request, "rq/fibonacci/_action/cancel_goalRequest");
  EXPECT_EQ(names.cancel_goal_reply, "rr/fibonacci/_action/cancel_goalReply");
  EXPECT_EQ(names.feedback, "rt/fibonacci/_action/feedback");
  EXPECT_EQ(names.status, "rt/fibonacci/_action/status");
}

TEST(FibonacciActionCommon, QosProfilesMatchHumbleFastddsContract) {
  EXPECT_EQ(ServiceQosProfile().history_depth, 10);
  EXPECT_TRUE(ServiceQosProfile().reliable);
  EXPECT_FALSE(ServiceQosProfile().transient_local);
  EXPECT_EQ(FeedbackQosProfile().history_depth, 10);
  EXPECT_TRUE(FeedbackQosProfile().reliable);
  EXPECT_FALSE(FeedbackQosProfile().transient_local);
  EXPECT_EQ(StatusQosProfile().history_depth, 1);
  EXPECT_TRUE(StatusQosProfile().reliable);
  EXPECT_TRUE(StatusQosProfile().transient_local);
}

TEST(FibonacciActionCommon, AppliesFastddsQosProfiles) {
  eprosima::fastdds::dds::DataReaderQos service_reader_qos;
  ApplyServiceEndpointQos(service_reader_qos);
  EXPECT_EQ(service_reader_qos.reliability().kind, eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS);
  EXPECT_EQ(service_reader_qos.durability().kind, eprosima::fastdds::dds::VOLATILE_DURABILITY_QOS);
  EXPECT_EQ(service_reader_qos.history().kind, eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS);
  EXPECT_EQ(service_reader_qos.history().depth, 10);
  EXPECT_EQ(service_reader_qos.data_sharing().kind(), eprosima::fastdds::dds::OFF);

  eprosima::fastdds::dds::DataWriterQos feedback_writer_qos;
  ApplyFeedbackEndpointQos(feedback_writer_qos);
  EXPECT_EQ(feedback_writer_qos.reliability().kind, eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS);
  EXPECT_EQ(feedback_writer_qos.durability().kind, eprosima::fastdds::dds::VOLATILE_DURABILITY_QOS);
  EXPECT_EQ(feedback_writer_qos.history().kind, eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS);
  EXPECT_EQ(feedback_writer_qos.history().depth, 10);
  EXPECT_EQ(feedback_writer_qos.data_sharing().kind(), eprosima::fastdds::dds::OFF);

  eprosima::fastdds::dds::DataReaderQos status_reader_qos;
  ApplyStatusEndpointQos(status_reader_qos);
  EXPECT_EQ(status_reader_qos.reliability().kind, eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS);
  EXPECT_EQ(status_reader_qos.durability().kind, eprosima::fastdds::dds::TRANSIENT_LOCAL_DURABILITY_QOS);
  EXPECT_EQ(status_reader_qos.history().kind, eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS);
  EXPECT_EQ(status_reader_qos.history().depth, 1);
  EXPECT_EQ(status_reader_qos.data_sharing().kind(), eprosima::fastdds::dds::OFF);
}

TEST(FibonacciActionCommon, BuildsServiceRequestWriteParamsWithReplyReaderIdentity) {
  eprosima::fastrtps::rtps::GUID_t reply_reader_guid;
  reply_reader_guid.guidPrefix.value[0] = 0x42;
  reply_reader_guid.entityId.value[3] = 0x07;

  const auto params = MakeServiceRequestWriteParams(reply_reader_guid, 17);
  const auto& related_id = params.related_sample_identity();
  EXPECT_EQ(related_id.writer_guid(), reply_reader_guid);
  EXPECT_EQ(related_id.sequence_number().high, 0);
  EXPECT_EQ(related_id.sequence_number().low, 17);
}

TEST(FibonacciActionCommon, CanonicalizesServiceRequestIdentityWithRelatedWriterGuid) {
  eprosima::fastdds::dds::SampleInfo sample_info;
  sample_info.sample_identity.writer_guid().guidPrefix.value[0] = 0x01;
  sample_info.sample_identity.writer_guid().entityId.value[3] = 0x02;
  sample_info.sample_identity.sequence_number(eprosima::fastrtps::rtps::SequenceNumber_t(0, 31));
  sample_info.related_sample_identity.writer_guid().guidPrefix.value[0] = 0x03;
  sample_info.related_sample_identity.writer_guid().entityId.value[3] = 0x04;

  const auto identity = MakeCanonicalServiceRequestIdentity(sample_info);
  EXPECT_EQ(identity.writer_guid(), sample_info.related_sample_identity.writer_guid());
  EXPECT_EQ(identity.sequence_number().low, 31);
}

TEST(FibonacciActionCommon, KeepsSampleWriterGuidWhenRelatedWriterGuidIsUnknown) {
  eprosima::fastdds::dds::SampleInfo sample_info;
  sample_info.sample_identity.writer_guid().guidPrefix.value[0] = 0x05;
  sample_info.sample_identity.writer_guid().entityId.value[3] = 0x06;
  sample_info.sample_identity.sequence_number(eprosima::fastrtps::rtps::SequenceNumber_t(0, 41));
  sample_info.related_sample_identity.writer_guid() = eprosima::fastrtps::rtps::GUID_t::unknown();

  const auto identity = MakeCanonicalServiceRequestIdentity(sample_info);
  EXPECT_EQ(identity.writer_guid(), sample_info.sample_identity.writer_guid());
  EXPECT_EQ(identity.sequence_number().low, 41);
}

TEST(FibonacciActionCommon, BuildsServiceResponseWriteParamsFromCanonicalRequestIdentity) {
  eprosima::fastrtps::rtps::SampleIdentity request_identity;
  request_identity.writer_guid().guidPrefix.value[0] = 0x09;
  request_identity.writer_guid().entityId.value[3] = 0x0A;
  request_identity.sequence_number(eprosima::fastrtps::rtps::SequenceNumber_t(0, 53));

  const auto params = MakeServiceResponseWriteParams(request_identity);
  EXPECT_EQ(params.related_sample_identity().writer_guid(), request_identity.writer_guid());
  EXPECT_EQ(params.related_sample_identity().sequence_number().low, 53);
}

TEST(FibonacciActionCommon, MatchesServiceRepliesByGuidAndSequence) {
  eprosima::fastrtps::rtps::GUID_t reply_reader_guid;
  reply_reader_guid.guidPrefix.value[0] = 0x0B;
  reply_reader_guid.entityId.value[3] = 0x0C;
  eprosima::fastrtps::rtps::SampleIdentity related_id;
  related_id.writer_guid() = reply_reader_guid;
  related_id.sequence_number(eprosima::fastrtps::rtps::SequenceNumber_t(0, 67));

  EXPECT_TRUE(ServiceReplyMatchesRequest(related_id, reply_reader_guid, 67));

  auto wrong_guid = reply_reader_guid;
  wrong_guid.guidPrefix.value[0] = 0x0D;
  EXPECT_FALSE(ServiceReplyMatchesRequest(related_id, wrong_guid, 67));
  EXPECT_FALSE(ServiceReplyMatchesRequest(related_id, reply_reader_guid, 68));
}

TEST(FibonacciActionCommon, BuildsBoundedFibonacciSequence) {
  EXPECT_TRUE(BuildFibonacciSequence(0).empty());
  EXPECT_EQ(BuildFibonacciSequence(1), std::vector<int32_t>({0}));
  EXPECT_EQ(BuildFibonacciSequence(8), std::vector<int32_t>({0, 1, 1, 2, 3, 5, 8, 13}));
  EXPECT_EQ(BuildFibonacciSequence(47).back(), 1836311903);
}

TEST(FibonacciActionCommon, ValidatesGoalCapacity) {
  EXPECT_TRUE(IsValidGoalOrder(1));
  EXPECT_TRUE(IsValidGoalOrder(kMaxFibonacciOrder));
  EXPECT_FALSE(IsValidGoalOrder(0));
  EXPECT_FALSE(IsValidGoalOrder(kMaxFibonacciOrder + 1));
  EXPECT_TRUE(CanAcceptGoal(3, 31));
  EXPECT_FALSE(CanAcceptGoal(4, 31));
  EXPECT_FALSE(CanAcceptGoal(3, 32));
}

TEST(FibonacciActionCommon, DetectsCancelAllAndSpecificGoalRequests) {
  TestUuid zero;
  TestTime zero_time;
  EXPECT_TRUE(IsZeroUuid(zero));
  EXPECT_TRUE(IsCancelAllActiveRequest(zero, zero_time));

  TestUuid specific;
  specific.uuid[3] = 7;
  EXPECT_FALSE(IsZeroUuid(specific));
  EXPECT_FALSE(IsCancelAllActiveRequest(specific, zero_time));
  EXPECT_EQ(UuidToString(specific).size(), specific.uuid.size());

  TestTime non_zero_time{.sec = 1, .nanosec = 0};
  EXPECT_FALSE(IsCancelAllActiveRequest(zero, non_zero_time));
}

TEST(FibonacciActionCommon, AppliesDeterministicResultRetentionWindow) {
  using Clock = std::chrono::steady_clock;
  const auto terminal_time = Clock::time_point{} + std::chrono::seconds(10);
  EXPECT_FALSE(ResultRetentionExpired<Clock>(terminal_time, terminal_time + std::chrono::seconds(60),
                                            std::chrono::seconds(60)));
  EXPECT_TRUE(ResultRetentionExpired<Clock>(terminal_time, terminal_time + std::chrono::seconds(61),
                                           std::chrono::seconds(60)));
}

TEST(ActionGoalLedger, TracksGenericAcceptanceGetResultAndTerminalRetention) {
  TestLedger ledger;
  const TestTime stamp{.sec = 1, .nanosec = 2};
  const auto now = TestClock::time_point{} + std::chrono::seconds(10);

  const auto first = ledger.AcceptGoal(MakeUuid(61), stamp, kGoalStatusAccepted, 1, 2);
  ASSERT_TRUE(first.accepted);
  EXPECT_EQ(first.code, ibmw::internal::ros2_action_dds::ActionGoalAcceptCode::kAccepted);
  EXPECT_TRUE(ledger.HasGoal(MakeUuid(61)));

  const auto duplicate = ledger.AcceptGoal(MakeUuid(61), stamp, kGoalStatusAccepted, 1, 2);
  EXPECT_FALSE(duplicate.accepted);
  EXPECT_EQ(duplicate.code, ibmw::internal::ros2_action_dds::ActionGoalAcceptCode::kDuplicateGoal);

  const auto over_active_capacity = ledger.AcceptGoal(MakeUuid(62), stamp, kGoalStatusAccepted, 1, 2);
  EXPECT_FALSE(over_active_capacity.accepted);
  EXPECT_EQ(over_active_capacity.code, ibmw::internal::ros2_action_dds::ActionGoalAcceptCode::kCapacityFull);

  const auto pending = ledger.GetResult(MakeUuid(61), 42, kDefaultMaxPendingResultsPerGoal);
  EXPECT_FALSE(pending.immediate);
  EXPECT_EQ(pending.code, ibmw::internal::ros2_action_dds::ActionGetResultCode::kDeferred);
  ASSERT_TRUE(ledger.SetExecuting(first.key, kGoalStatusExecuting));

  const auto terminal = ledger.MarkTerminal(first.key, kGoalStatusSucceeded, now);
  ASSERT_TRUE(terminal.found);
  EXPECT_EQ(terminal.final_status, kGoalStatusSucceeded);
  ASSERT_EQ(terminal.pending_results.size(), 1U);
  EXPECT_EQ(terminal.pending_results[0], 42);

  const auto immediate = ledger.GetResult(MakeUuid(61), 43, kDefaultMaxPendingResultsPerGoal);
  EXPECT_TRUE(immediate.immediate);
  EXPECT_EQ(immediate.status, kGoalStatusSucceeded);
  EXPECT_EQ(immediate.code, ibmw::internal::ros2_action_dds::ActionGetResultCode::kImmediateTerminal);

  const auto retained = ledger.SnapshotStatusAndPrune(now + std::chrono::seconds(60), std::chrono::seconds(60));
  EXPECT_TRUE(retained.pruned_keys.empty());
  ASSERT_EQ(retained.status_list.size(), 1U);
  EXPECT_EQ(retained.status_list[0].status, kGoalStatusSucceeded);

  const auto pruned = ledger.SnapshotStatusAndPrune(now + std::chrono::seconds(61), std::chrono::seconds(60));
  EXPECT_EQ(pruned.pruned_keys, std::vector<std::string>({first.key}));
  EXPECT_TRUE(pruned.status_list.empty());
  EXPECT_FALSE(ledger.HasGoal(MakeUuid(61)));

  const auto after_prune = ledger.AcceptGoal(MakeUuid(62), stamp, kGoalStatusAccepted, 1, 2);
  EXPECT_TRUE(after_prune.accepted);
}

TEST(ActionGoalLedger, BoundsPendingResultsPerGoalAndKeepsQueuedOrder) {
  TestLedger ledger;
  const TestTime stamp{.sec = 1, .nanosec = 2};
  const auto now = TestClock::time_point{} + std::chrono::seconds(10);
  const auto accepted = ledger.AcceptGoal(MakeUuid(63), stamp, kGoalStatusAccepted, 1, 2);
  ASSERT_TRUE(accepted.accepted);

  const auto first = ledger.GetResult(MakeUuid(63), 10, 2);
  EXPECT_FALSE(first.immediate);
  EXPECT_EQ(first.code, ibmw::internal::ros2_action_dds::ActionGetResultCode::kDeferred);
  const auto second = ledger.GetResult(MakeUuid(63), 20, 2);
  EXPECT_FALSE(second.immediate);
  EXPECT_EQ(second.code, ibmw::internal::ros2_action_dds::ActionGetResultCode::kDeferred);

  const auto over_cap = ledger.GetResult(MakeUuid(63), 30, 2);
  EXPECT_TRUE(over_cap.immediate);
  EXPECT_EQ(over_cap.status, kGoalStatusUnknown);
  EXPECT_EQ(over_cap.code, ibmw::internal::ros2_action_dds::ActionGetResultCode::kPendingQueueFull);

  const auto terminal = ledger.MarkTerminal(accepted.key, kGoalStatusSucceeded, now);
  ASSERT_TRUE(terminal.found);
  EXPECT_EQ(terminal.final_status, kGoalStatusSucceeded);
  EXPECT_EQ(terminal.pending_results, std::vector<int>({10, 20}));
}

TEST(ActionGoalLedger, KeepsUnknownAndTerminalGetResultImmediateWithoutEnqueue) {
  TestLedger ledger;
  const TestTime stamp{.sec = 1, .nanosec = 2};
  const auto now = TestClock::time_point{} + std::chrono::seconds(10);

  const auto unknown = ledger.GetResult(MakeUuid(64), 10, 0);
  EXPECT_TRUE(unknown.immediate);
  EXPECT_EQ(unknown.status, kGoalStatusUnknown);
  EXPECT_EQ(unknown.code, ibmw::internal::ros2_action_dds::ActionGetResultCode::kImmediateUnknown);

  const auto accepted = ledger.AcceptGoal(MakeUuid(65), stamp, kGoalStatusAccepted, 1, 2);
  ASSERT_TRUE(accepted.accepted);
  const auto terminal = ledger.MarkTerminal(accepted.key, kGoalStatusSucceeded, now);
  ASSERT_TRUE(terminal.found);
  EXPECT_TRUE(terminal.pending_results.empty());

  const auto immediate = ledger.GetResult(MakeUuid(65), 20, 0);
  EXPECT_TRUE(immediate.immediate);
  EXPECT_EQ(immediate.status, kGoalStatusSucceeded);
  EXPECT_EQ(immediate.code, ibmw::internal::ros2_action_dds::ActionGetResultCode::kImmediateTerminal);

  const auto repeated_terminal = ledger.MarkTerminal(accepted.key, kGoalStatusSucceeded, now);
  ASSERT_TRUE(repeated_terminal.found);
  EXPECT_TRUE(repeated_terminal.pending_results.empty());
}

TEST(ActionGoalLedger, MatchesSpecificCancelAllStopAllAndGoalInfoQueries) {
  TestLedger ledger;
  const TestTime stamp{.sec = 3, .nanosec = 4};
  const auto first = ledger.AcceptGoal(MakeUuid(71), stamp, kGoalStatusAccepted, 4, 32);
  const auto second = ledger.AcceptGoal(MakeUuid(72), stamp, kGoalStatusAccepted, 4, 32);
  ASSERT_TRUE(first.accepted);
  ASSERT_TRUE(second.accepted);

  const auto goal_info = ledger.GoalInfoForKey(first.key);
  ASSERT_TRUE(goal_info.has_value());
  EXPECT_EQ(UuidToString(goal_info->goal_id), UuidToString(MakeUuid(71)));
  EXPECT_EQ(goal_info->stamp.sec, stamp.sec);

  const auto unknown_specific = ledger.CancelGoal(MakeUuid(73), stamp, kGoalStatusCanceling,
                                                  kCancelReturnNone, kCancelReturnUnknownGoalId);
  EXPECT_EQ(unknown_specific.return_code, kCancelReturnUnknownGoalId);
  EXPECT_TRUE(unknown_specific.goals_canceling.empty());

  const auto specific = ledger.CancelGoal(MakeUuid(71), TestTime{}, kGoalStatusCanceling,
                                          kCancelReturnNone, kCancelReturnUnknownGoalId);
  EXPECT_EQ(specific.return_code, kCancelReturnNone);
  ASSERT_EQ(specific.goals_canceling.size(), 1U);
  EXPECT_EQ(UuidToString(specific.goals_canceling[0].goal_id), UuidToString(MakeUuid(71)));
  EXPECT_TRUE(ledger.IsCancelRequested(first.key));

  auto status = ledger.SnapshotStatusAndPrune(TestClock::time_point{}, std::chrono::seconds(60));
  const auto* first_status = FindStatus(status, MakeUuid(71));
  ASSERT_NE(first_status, nullptr);
  EXPECT_EQ(first_status->status, kGoalStatusCanceling);

  ASSERT_TRUE(ledger.MarkTerminal(first.key, kGoalStatusCanceled,
                                  TestClock::time_point{} + std::chrono::seconds(1)).found);

  const auto cancel_all = ledger.CancelGoal(TestUuid{}, TestTime{}, kGoalStatusCanceling,
                                            kCancelReturnNone, kCancelReturnUnknownGoalId);
  EXPECT_EQ(cancel_all.return_code, kCancelReturnNone);
  ASSERT_EQ(cancel_all.goals_canceling.size(), 1U);
  EXPECT_EQ(UuidToString(cancel_all.goals_canceling[0].goal_id), UuidToString(MakeUuid(72)));

  TestLedger stop_all_ledger;
  const auto stop_goal = stop_all_ledger.AcceptGoal(MakeUuid(74), stamp, kGoalStatusAccepted, 4, 32);
  ASSERT_TRUE(stop_goal.accepted);
  stop_all_ledger.RequestStopAll();
  EXPECT_TRUE(stop_all_ledger.IsCancelRequested(stop_goal.key));

  TestLedger empty_ledger;
  const auto empty_cancel_all = empty_ledger.CancelGoal(TestUuid{}, TestTime{}, kGoalStatusCanceling,
                                                        kCancelReturnNone, kCancelReturnUnknownGoalId);
  EXPECT_EQ(empty_cancel_all.return_code, kCancelReturnNone);
  EXPECT_TRUE(empty_cancel_all.goals_canceling.empty());
}

TEST(ActionClientObserver, FiltersActiveGoalAndCapturesLatestGenericFeedback) {
  TestObserver observer;
  observer.ResetActiveGoal(MakeUuid(81));

  EXPECT_FALSE(observer.ObserveFeedback(MakeUuid(82), std::string("ignored")));
  EXPECT_FALSE(observer.HasFeedback());
  EXPECT_TRUE(observer.LatestFeedback().empty());

  EXPECT_TRUE(observer.ObserveFeedback(MakeUuid(81), std::string("first")));
  EXPECT_TRUE(observer.HasFeedback());
  EXPECT_EQ(observer.LatestFeedback(), "first");

  EXPECT_TRUE(observer.ObserveFeedback(MakeUuid(81), std::string("latest")));
  EXPECT_EQ(observer.LatestFeedback(), "latest");
}

TEST(ActionClientObserver, ObservesMatchingStatusCountersMaskAndTerminalSummary) {
  TestObserver observer;
  observer.ResetActiveGoal(MakeUuid(83));

  observer.ObserveStatusList(std::vector<TestStatus>({TestStatus{.goal_info = TestGoalInfo{.goal_id = MakeUuid(84)},
                                                      .status = 2}}));
  auto missing = observer.SummarizeTerminalStatus(4);
  EXPECT_FALSE(missing.available);
  EXPECT_EQ(missing.observed_arrays, 1U);
  EXPECT_EQ(missing.matched_statuses, 0U);

  observer.ObserveStatusList(std::vector<TestStatus>({TestStatus{.goal_info = TestGoalInfo{.goal_id = MakeUuid(83)},
                                                      .status = 2},
                                                     TestStatus{.goal_info = TestGoalInfo{.goal_id = MakeUuid(83)},
                                                      .status = 4}}));
  const auto summary = observer.SummarizeTerminalStatus(4);
  EXPECT_TRUE(summary.available);
  EXPECT_EQ(summary.observed_arrays, 2U);
  EXPECT_EQ(summary.matched_statuses, 2U);
  EXPECT_EQ(summary.latest_status, 4);
  EXPECT_TRUE(summary.saw_terminal);
  EXPECT_NE(summary.observed_status_mask & TestObserver::StatusMaskBit(2), 0U);
  EXPECT_NE(summary.observed_status_mask & TestObserver::StatusMaskBit(4), 0U);
}

TEST(ActionClientObserver, ResetClearsObservationAndCountersSaturate) {
  EXPECT_EQ(TestObserver::SaturatingIncrement(TestObserver::kObservationCap - 1, 10),
            TestObserver::kObservationCap);
  EXPECT_EQ(TestObserver::SaturatingIncrement(TestObserver::kObservationCap, 1),
            TestObserver::kObservationCap);
  EXPECT_EQ(TestObserver::StatusMaskBit(-1), 0U);
  EXPECT_EQ(TestObserver::StatusMaskBit(32), 0U);

  TestObserver observer;
  observer.ResetActiveGoal(MakeUuid(85));
  ASSERT_TRUE(observer.ObserveFeedback(MakeUuid(85), std::string("old")));
  observer.ObserveStatusList(std::vector<TestStatus>({TestStatus{.goal_info = TestGoalInfo{.goal_id = MakeUuid(85)},
                                                    .status = 4}}));

  observer.ResetActiveGoal(MakeUuid(86));
  EXPECT_FALSE(observer.HasFeedback());
  EXPECT_TRUE(observer.LatestFeedback().empty());
  auto reset_summary = observer.SummarizeTerminalStatus(4);
  EXPECT_FALSE(reset_summary.available);
  EXPECT_EQ(reset_summary.observed_arrays, 0U);
  EXPECT_EQ(reset_summary.matched_statuses, 0U);
  EXPECT_FALSE(observer.ObserveFeedback(MakeUuid(85), std::string("stale")));

  for (std::size_t i = 0; i < TestObserver::kObservationCap + 5; ++i) {
    observer.ObserveStatusList(std::vector<TestStatus>({TestStatus{.goal_info = TestGoalInfo{.goal_id = MakeUuid(86)},
                                                      .status = 32}}));
  }
  const auto saturated = observer.SummarizeTerminalStatus(32);
  EXPECT_TRUE(saturated.available);
  EXPECT_EQ(saturated.observed_arrays, TestObserver::kObservationCap);
  EXPECT_EQ(saturated.matched_statuses, TestObserver::kObservationCap);
  EXPECT_EQ(saturated.latest_status, 32);
  EXPECT_EQ(saturated.observed_status_mask, 0U);
  EXPECT_FALSE(saturated.saw_terminal);
}

TEST(FibonacciActionServerRuntime, AcceptsValidGoalsAndRejectsInvalidDuplicateAndActiveCapacity) {
  TestRuntime runtime;
  const TestTime stamp{.sec = 10, .nanosec = 20};

  const auto invalid_low = runtime.AcceptGoal(MakeUuid(1), stamp, 0);
  EXPECT_FALSE(invalid_low.accepted);
  EXPECT_EQ(invalid_low.failure, GoalAcceptanceFailure::kInvalidOrder);

  const auto invalid_high = runtime.AcceptGoal(MakeUuid(2), stamp, kMaxFibonacciOrder + 1);
  EXPECT_FALSE(invalid_high.accepted);
  EXPECT_EQ(invalid_high.failure, GoalAcceptanceFailure::kInvalidOrder);

  const auto first = runtime.AcceptGoal(MakeUuid(3), stamp, 8);
  EXPECT_TRUE(first.accepted);
  EXPECT_EQ(first.failure, GoalAcceptanceFailure::kNone);

  const auto duplicate = runtime.AcceptGoal(MakeUuid(3), stamp, 8);
  EXPECT_FALSE(duplicate.accepted);
  EXPECT_EQ(duplicate.failure, GoalAcceptanceFailure::kDuplicateGoal);

  EXPECT_TRUE(runtime.AcceptGoal(MakeUuid(4), stamp, 8).accepted);
  EXPECT_TRUE(runtime.AcceptGoal(MakeUuid(5), stamp, 8).accepted);
  EXPECT_TRUE(runtime.AcceptGoal(MakeUuid(6), stamp, 8).accepted);

  const auto over_active_capacity = runtime.AcceptGoal(MakeUuid(7), stamp, 8);
  EXPECT_FALSE(over_active_capacity.accepted);
  EXPECT_EQ(over_active_capacity.failure, GoalAcceptanceFailure::kCapacityFull);
}

TEST(FibonacciActionServerRuntime, EnforcesRetainedCapacityAcrossTerminalGoals) {
  TestRuntime runtime;
  const TestTime stamp{.sec = 1, .nanosec = 0};
  const auto now = TestClock::time_point{} + std::chrono::seconds(5);

  for (uint8_t index = 1; index <= kDefaultMaxRetainedGoals; ++index) {
    const auto goal_id = MakeUuid(index);
    const auto accepted = runtime.AcceptGoal(goal_id, stamp, 1);
    ASSERT_TRUE(accepted.accepted) << static_cast<int>(index);
    ASSERT_TRUE(runtime.StartExecuting(accepted.key));
    const auto transition = runtime.AdvanceExecution(accepted.key, now);
    ASSERT_TRUE(transition.terminal);
    ASSERT_EQ(transition.final_status, kGoalStatusSucceeded);
  }

  const auto over_retained_capacity = runtime.AcceptGoal(MakeUuid(99), stamp, 1);
  EXPECT_FALSE(over_retained_capacity.accepted);
  EXPECT_EQ(over_retained_capacity.failure, GoalAcceptanceFailure::kCapacityFull);
}

TEST(FibonacciActionServerRuntime, DefersGetResultUntilTerminalAndThenReturnsImmediateResult) {
  TestRuntime runtime;
  const TestTime stamp{.sec = 1, .nanosec = 2};
  const auto accepted = runtime.AcceptGoal(MakeUuid(8), stamp, 1);
  ASSERT_TRUE(accepted.accepted);

  const auto pending = runtime.GetResult(MakeUuid(8), 42);
  EXPECT_FALSE(pending.immediate);
  ASSERT_TRUE(runtime.StartExecuting(accepted.key));

  const auto transition = runtime.AdvanceExecution(accepted.key,
                                                   TestClock::time_point{} + std::chrono::seconds(10));
  ASSERT_TRUE(transition.terminal);
  EXPECT_EQ(transition.final_status, kGoalStatusSucceeded);
  EXPECT_EQ(transition.final_sequence, std::vector<int32_t>({0}));
  ASSERT_EQ(transition.pending_results.size(), 1U);
  EXPECT_EQ(transition.pending_results[0], 42);

  const auto immediate = runtime.GetResult(MakeUuid(8), 43);
  EXPECT_TRUE(immediate.immediate);
  EXPECT_EQ(immediate.status, kGoalStatusSucceeded);
  EXPECT_EQ(immediate.sequence, std::vector<int32_t>({0}));

  const auto unknown = runtime.GetResult(MakeUuid(9), 44);
  EXPECT_TRUE(unknown.immediate);
  EXPECT_EQ(unknown.status, kGoalStatusUnknown);
  EXPECT_TRUE(unknown.sequence.empty());
}

TEST(FibonacciActionServerRuntime, BoundsPendingResultQueueAndRejectsNewestOverCap) {
  TestRuntime runtime;
  const TestTime stamp{.sec = 1, .nanosec = 2};
  const auto accepted = runtime.AcceptGoal(MakeUuid(14), stamp, 1);
  ASSERT_TRUE(accepted.accepted);

  const auto first = runtime.GetResult(MakeUuid(14), 100, 2);
  EXPECT_FALSE(first.immediate);
  EXPECT_FALSE(first.queue_full);
  const auto second = runtime.GetResult(MakeUuid(14), 200, 2);
  EXPECT_FALSE(second.immediate);
  EXPECT_FALSE(second.queue_full);

  const auto over_cap = runtime.GetResult(MakeUuid(14), 300, 2);
  EXPECT_TRUE(over_cap.immediate);
  EXPECT_EQ(over_cap.status, kGoalStatusUnknown);
  EXPECT_TRUE(over_cap.sequence.empty());
  EXPECT_TRUE(over_cap.queue_full);

  ASSERT_TRUE(runtime.StartExecuting(accepted.key));
  const auto transition = runtime.AdvanceExecution(accepted.key,
                                                   TestClock::time_point{} + std::chrono::seconds(10));
  ASSERT_TRUE(transition.terminal);
  EXPECT_EQ(transition.pending_results, std::vector<int>({100, 200}));
}

TEST(FibonacciActionServerRuntime, KeepsUnknownAndTerminalGetResultImmediateWithoutQueueing) {
  TestRuntime runtime;
  const TestTime stamp{.sec = 1, .nanosec = 2};

  const auto unknown = runtime.GetResult(MakeUuid(15), 100, 0);
  EXPECT_TRUE(unknown.immediate);
  EXPECT_EQ(unknown.status, kGoalStatusUnknown);
  EXPECT_TRUE(unknown.sequence.empty());
  EXPECT_FALSE(unknown.queue_full);

  const auto accepted = runtime.AcceptGoal(MakeUuid(16), stamp, 1);
  ASSERT_TRUE(accepted.accepted);
  ASSERT_TRUE(runtime.StartExecuting(accepted.key));
  const auto terminal = runtime.AdvanceExecution(accepted.key,
                                                 TestClock::time_point{} + std::chrono::seconds(10));
  ASSERT_TRUE(terminal.terminal);
  EXPECT_TRUE(terminal.pending_results.empty());

  const auto immediate = runtime.GetResult(MakeUuid(16), 200, 0);
  EXPECT_TRUE(immediate.immediate);
  EXPECT_EQ(immediate.status, kGoalStatusSucceeded);
  EXPECT_EQ(immediate.sequence, std::vector<int32_t>({0}));
  EXPECT_FALSE(immediate.queue_full);
}

TEST(FibonacciActionServerRuntime, CancelsSpecificGoalsAllGoalsAndIgnoresTerminalGoals) {
  TestRuntime runtime;
  const TestTime stamp{.sec = 3, .nanosec = 4};
  const auto first = runtime.AcceptGoal(MakeUuid(10), stamp, 5);
  const auto second = runtime.AcceptGoal(MakeUuid(11), stamp, 5);
  ASSERT_TRUE(first.accepted);
  ASSERT_TRUE(second.accepted);

  const auto unknown_specific = runtime.CancelGoal(MakeUuid(12), stamp);
  EXPECT_EQ(unknown_specific.return_code, kCancelReturnUnknownGoalId);
  EXPECT_TRUE(unknown_specific.goals_canceling.empty());

  const auto specific = runtime.CancelGoal(MakeUuid(10), TestTime{});
  EXPECT_EQ(specific.return_code, kCancelReturnNone);
  ASSERT_EQ(specific.goals_canceling.size(), 1U);
  EXPECT_EQ(UuidToString(specific.goals_canceling[0].goal_id), UuidToString(MakeUuid(10)));

  auto status = runtime.SnapshotStatusAndPrune(TestClock::time_point{});
  const auto* first_status = FindStatus(status, MakeUuid(10));
  ASSERT_NE(first_status, nullptr);
  EXPECT_EQ(first_status->status, kGoalStatusCanceling);

  ASSERT_TRUE(runtime.StartExecuting(first.key));
  const auto canceled = runtime.AdvanceExecution(first.key, TestClock::time_point{} + std::chrono::seconds(1));
  ASSERT_TRUE(canceled.terminal);
  EXPECT_EQ(canceled.final_status, kGoalStatusCanceled);

  const auto cancel_all = runtime.CancelGoal(TestUuid{}, TestTime{});
  EXPECT_EQ(cancel_all.return_code, kCancelReturnNone);
  ASSERT_EQ(cancel_all.goals_canceling.size(), 1U);
  EXPECT_EQ(UuidToString(cancel_all.goals_canceling[0].goal_id), UuidToString(MakeUuid(11)));

  TestRuntime empty_runtime;
  const auto empty_cancel_all = empty_runtime.CancelGoal(TestUuid{}, TestTime{});
  EXPECT_EQ(empty_cancel_all.return_code, kCancelReturnNone);
  EXPECT_TRUE(empty_cancel_all.goals_canceling.empty());
}

TEST(FibonacciActionServerRuntime, AdvancesExecutionFeedbackStatusAndStrictRetention) {
  TestRuntime runtime;
  const TestTime stamp{.sec = 5, .nanosec = 6};
  const auto accepted = runtime.AcceptGoal(MakeUuid(13), stamp, 3);
  ASSERT_TRUE(accepted.accepted);
  ASSERT_TRUE(runtime.StartExecuting(accepted.key));

  auto feedback = runtime.SnapshotFeedback(accepted.key);
  ASSERT_TRUE(feedback.has_value());
  EXPECT_EQ(feedback->sequence, std::vector<int32_t>({0, 1}));

  const auto non_terminal = runtime.AdvanceExecution(accepted.key,
                                                     TestClock::time_point{} + std::chrono::seconds(10));
  EXPECT_TRUE(non_terminal.found);
  EXPECT_FALSE(non_terminal.terminal);
  feedback = runtime.SnapshotFeedback(accepted.key);
  ASSERT_TRUE(feedback.has_value());
  EXPECT_EQ(feedback->sequence, std::vector<int32_t>({0, 1, 1}));

  const auto terminal_time = TestClock::time_point{} + std::chrono::seconds(20);
  const auto terminal = runtime.AdvanceExecution(accepted.key, terminal_time);
  ASSERT_TRUE(terminal.terminal);
  EXPECT_EQ(terminal.final_status, kGoalStatusSucceeded);
  EXPECT_EQ(terminal.final_sequence, std::vector<int32_t>({0, 1, 1}));

  auto retained = runtime.SnapshotStatusAndPrune(terminal_time + std::chrono::seconds(60));
  EXPECT_TRUE(retained.pruned_keys.empty());
  ASSERT_EQ(retained.status_list.size(), 1U);
  EXPECT_EQ(retained.status_list[0].status, kGoalStatusSucceeded);

  auto pruned = runtime.SnapshotStatusAndPrune(terminal_time + std::chrono::seconds(61));
  EXPECT_EQ(pruned.pruned_keys, std::vector<std::string>({accepted.key}));
  EXPECT_TRUE(pruned.status_list.empty());
  EXPECT_FALSE(runtime.SnapshotFeedback(accepted.key).has_value());
}

TEST(FibonacciActionClientRuntime, ObservesMatchingFeedbackAndValidatesExpectedPrefix) {
  TestClientRuntime runtime;
  runtime.ResetActiveGoal(MakeUuid(21));

  EXPECT_FALSE(runtime.ObserveFeedback(MakeUuid(22), std::vector<int32_t>({9, 9})));
  auto missing = runtime.ValidateFeedbackPrefix(std::vector<int32_t>({0, 1, 1}));
  EXPECT_FALSE(missing.available);
  EXPECT_TRUE(missing.ok);

  EXPECT_TRUE(runtime.ObserveFeedback(MakeUuid(21), std::vector<int32_t>({0, 1})));
  const auto valid = runtime.ValidateFeedbackPrefix(std::vector<int32_t>({0, 1, 1, 2}));
  EXPECT_TRUE(valid.available);
  EXPECT_TRUE(valid.ok);
  EXPECT_EQ(valid.sequence, std::vector<int32_t>({0, 1}));

  EXPECT_TRUE(runtime.ObserveFeedback(MakeUuid(21), std::vector<int32_t>({0, 2})));
  const auto invalid = runtime.ValidateFeedbackPrefix(std::vector<int32_t>({0, 1, 1, 2}));
  EXPECT_TRUE(invalid.available);
  EXPECT_FALSE(invalid.ok);
  EXPECT_EQ(invalid.sequence, std::vector<int32_t>({0, 2}));
}

TEST(FibonacciActionClientRuntime, SummarizesMatchingStatusCountersAndTerminalMask) {
  TestClientRuntime runtime;
  runtime.ResetActiveGoal(MakeUuid(31));

  runtime.ObserveStatusList(std::vector<TestStatus>({TestStatus{.goal_info = TestGoalInfo{.goal_id = MakeUuid(32)},
                                                     .status = 2}}));
  auto missing = runtime.SummarizeTerminalStatus(4);
  EXPECT_FALSE(missing.available);
  EXPECT_EQ(missing.observed_arrays, 1U);
  EXPECT_EQ(missing.matched_statuses, 0U);

  runtime.ObserveStatusList(std::vector<TestStatus>({TestStatus{.goal_info = TestGoalInfo{.goal_id = MakeUuid(31)},
                                                     .status = 2},
                                                    TestStatus{.goal_info = TestGoalInfo{.goal_id = MakeUuid(31)},
                                                     .status = 4}}));
  const auto summary = runtime.SummarizeTerminalStatus(4);
  EXPECT_TRUE(summary.available);
  EXPECT_EQ(summary.observed_arrays, 2U);
  EXPECT_EQ(summary.matched_statuses, 2U);
  EXPECT_EQ(summary.latest_status, 4);
  EXPECT_TRUE(summary.saw_terminal);
  EXPECT_NE(summary.observed_status_mask & TestClientRuntime::StatusMaskBit(2), 0U);
  EXPECT_NE(summary.observed_status_mask & TestClientRuntime::StatusMaskBit(4), 0U);
}

TEST(FibonacciActionClientRuntime, ResetClearsFeedbackAndStatusObservationForNewGoal) {
  TestClientRuntime runtime;
  runtime.ResetActiveGoal(MakeUuid(41));
  ASSERT_TRUE(runtime.ObserveFeedback(MakeUuid(41), std::vector<int32_t>({0, 1})));
  runtime.ObserveStatusList(std::vector<TestStatus>({TestStatus{.goal_info = TestGoalInfo{.goal_id = MakeUuid(41)},
                                                   .status = 4}}));

  runtime.ResetActiveGoal(MakeUuid(42));
  EXPECT_FALSE(runtime.ValidateFeedbackPrefix(std::vector<int32_t>({0, 1})).available);
  auto summary = runtime.SummarizeTerminalStatus(4);
  EXPECT_FALSE(summary.available);
  EXPECT_EQ(summary.observed_arrays, 0U);
  EXPECT_EQ(summary.matched_statuses, 0U);
  EXPECT_FALSE(runtime.ObserveFeedback(MakeUuid(41), std::vector<int32_t>({0, 1})));
  EXPECT_TRUE(runtime.ObserveFeedback(MakeUuid(42), std::vector<int32_t>({0})));
}

TEST(FibonacciActionClientRuntime, SaturatesCountersAndIgnoresOutOfRangeStatusMaskBits) {
  EXPECT_EQ(TestClientRuntime::SaturatingIncrement(TestClientRuntime::kObservationCap - 1, 10),
            TestClientRuntime::kObservationCap);
  EXPECT_EQ(TestClientRuntime::SaturatingIncrement(TestClientRuntime::kObservationCap, 1),
            TestClientRuntime::kObservationCap);
  EXPECT_EQ(TestClientRuntime::StatusMaskBit(-1), 0U);
  EXPECT_EQ(TestClientRuntime::StatusMaskBit(32), 0U);

  TestClientRuntime runtime;
  runtime.ResetActiveGoal(MakeUuid(51));
  for (std::size_t i = 0; i < TestClientRuntime::kObservationCap + 5; ++i) {
    runtime.ObserveStatusList(std::vector<TestStatus>({TestStatus{.goal_info = TestGoalInfo{.goal_id = MakeUuid(51)},
                                                     .status = 32}}));
  }
  const auto summary = runtime.SummarizeTerminalStatus(32);
  EXPECT_TRUE(summary.available);
  EXPECT_EQ(summary.observed_arrays, TestClientRuntime::kObservationCap);
  EXPECT_EQ(summary.matched_statuses, TestClientRuntime::kObservationCap);
  EXPECT_EQ(summary.latest_status, 32);
  EXPECT_EQ(summary.observed_status_mask, 0U);
  EXPECT_FALSE(summary.saw_terminal);
}

}  // namespace
}  // namespace ibmw::samples::ros2_action_fibonacci
