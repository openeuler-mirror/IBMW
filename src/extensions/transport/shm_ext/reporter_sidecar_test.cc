// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Reporter V1 sidecar of the shm transport against a fake reporter, the
// counterpart of iceoryx_ext/reporter_sidecar_test.cc. The real host is
// covered by ibmw_shm_reporter_host_boundary_test.

#include "shm_ext/internal/reporter_sidecar.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "shm_ext/internal/reporter_bridge.h"
#include "shm_ext/shm_bus.h"

#ifdef IBMW_ENABLE_REPORTER_V1
namespace ibmw::extensions::shm_extension {
namespace {

// Fake host: slots are 1-based indexes into `values`.
struct FakeReporter {
  std::array<int64_t, 16> values{};
  size_t prepares = 0;
  size_t commits = 0;
  size_t aborts = 0;
  size_t commit_failures = 0;
  bool prepare_fails = false;
  size_t missing_slot = SIZE_MAX;
  std::vector<std::string> kinds;
  std::vector<std::string> metrics;
  std::vector<std::string> drivers;
};

FakeReporter& Fake(void* owner) { return *static_cast<FakeReporter*>(owner); }
size_t Index(ibmw_reporter_slot_v1* slot) { return reinterpret_cast<uintptr_t>(slot) - 1U; }
std::string Str(ibmw_reporter_string_view_v1 v) { return std::string(v.data, v.size); }

ibmw_reporter_slot_v1* RegisterSlot(void*, const ibmw_reporter_slot_descriptor_v1*) { return nullptr; }
void Add(void* owner, ibmw_reporter_slot_v1* slot, uint64_t value) {
  Fake(owner).values.at(Index(slot)) += static_cast<int64_t>(value);
}
void AddSigned(void* owner, ibmw_reporter_slot_v1* slot, int64_t value) { Fake(owner).values.at(Index(slot)) += value; }

int Prepare(void* owner, const ibmw_reporter_slot_descriptor_v1* descriptors, size_t count,
            ibmw_reporter_slot_v1** slots, ibmw_reporter_batch_transaction_v1** transaction) {
  auto& f = Fake(owner);
  ++f.prepares;
  if (f.prepare_fails) return 0;
  for (size_t i = 0; i < count; ++i) {
    f.kinds.push_back(Str(descriptors[i].kind));
    f.metrics.push_back(Str(descriptors[i].metric));
    f.drivers.push_back(Str(descriptors[i].driver));
    slots[i] = i == f.missing_slot ? nullptr : reinterpret_cast<ibmw_reporter_slot_v1*>(i + 1U);
  }
  *transaction = reinterpret_cast<ibmw_reporter_batch_transaction_v1*>(owner);
  return 1;
}
int Commit(void* owner, ibmw_reporter_batch_transaction_v1*) {
  auto& f = Fake(owner);
  ++f.commits;
  if (f.commit_failures != 0) {
    --f.commit_failures;
    return 0;
  }
  return 1;
}
void Abort(void* owner, ibmw_reporter_batch_transaction_v1*) { ++Fake(owner).aborts; }

struct Host {
  FakeReporter fake;
  ibmw_reporter_v1 api{IBMW_REPORTER_ABI_VERSION_V1, sizeof(ibmw_reporter_v1), &fake, RegisterSlot, Add, {},
                       AddSigned};
  ibmw_reporter_batch_v1 batch{IBMW_REPORTER_BATCH_ABI_VERSION_V1, sizeof(ibmw_reporter_batch_v1), &fake, Prepare,
                               Commit, Abort};
};

ibmw_reporter_slot_v1* Slot(size_t index) { return reinterpret_cast<ibmw_reporter_slot_v1*>(index + 1U); }

DirectLoanReporter MakeDirect(Host& host) {
  DirectLoanReporter r;
  r.api = &host.api;
  r.slots = {Slot(0), Slot(1), Slot(2), Slot(3), Slot(4), Slot(5)};
  return r;
}

TEST(ShmDirectLoanReporterOutcome, UnreportedPublisherIsANoOp) {
  DirectLoanReporterOutcome outcome(nullptr);
  EXPECT_FALSE(outcome.Reported());
  outcome.retries = 3;
  outcome.MarkSuccess();
  ReportDirectLoanReturn(nullptr, true, false);
}

TEST(ShmDirectLoanReporterOutcome, SuccessFailureAndRetries) {
  Host host;
  const auto r = MakeDirect(host);
  {
    DirectLoanReporterOutcome outcome(&r);
    EXPECT_TRUE(outcome.Reported());
    outcome.retries = 3;
    outcome.MarkSuccess();
  }
  { DirectLoanReporterOutcome outcome(&r); }
  // attempt, success, fail, retry, publish, release
  EXPECT_EQ((std::array<int64_t, 6>{host.fake.values[0], host.fake.values[1], host.fake.values[2],
                                    host.fake.values[3], host.fake.values[4], host.fake.values[5]}),
            (std::array<int64_t, 6>{2, 1, 1, 3, 0, 0}));
}

TEST(ShmDirectLoanReporterOutcome, ReturnCountsPublishOrReleaseNeverBoth) {
  Host host;
  const auto r = MakeDirect(host);
  ReportDirectLoanReturn(&r, true, false);
  ReportDirectLoanReturn(&r, false, true);
  ReportDirectLoanReturn(&r, false, false);  // stale token
  EXPECT_EQ(host.fake.values[4], 1);
  EXPECT_EQ(host.fake.values[5], 1);
}

TEST(ShmReporterSlots, EveryDirectSlotIsRequired) {
  for (size_t missing = 0; missing < 6; ++missing) {
    DirectReporterSlots slots{Slot(0), Slot(1), Slot(2), Slot(3), Slot(4), Slot(5)};
    ibmw_reporter_slot_v1** fields[] = {&slots.attempt, &slots.success, &slots.fail,
                                        &slots.retry,   &slots.publish, &slots.release};
    EXPECT_TRUE(DirectReporterSlotsComplete(slots));
    *fields[missing] = nullptr;
    EXPECT_FALSE(DirectReporterSlotsComplete(slots)) << missing;
  }
}

TEST(ShmReporterSlots, DirectFamilyCarriesKindMetricsAndDriverLabel) {
  Host host;
  ReporterBatchRegistration transaction;
  DirectLoanReporter r;
  ASSERT_TRUE(PrepareDirectLoanReporterSlots(&host.api, &host.batch, transaction, r, "topic", "type", "iceoryx"));
  EXPECT_TRUE(DirectReporterSlotsComplete(r.slots));
  EXPECT_EQ(host.fake.metrics, (std::vector<std::string>{"logical_attempt", "logical_success", "logical_fail",
                                                         "internal_retry", "publish", "release"}));
  EXPECT_EQ(host.fake.kinds, std::vector<std::string>(6, "direct_shm_loan"));
  EXPECT_EQ(host.fake.drivers, std::vector<std::string>(6, "iceoryx"));
  EXPECT_TRUE(transaction.Commit());
  EXPECT_FALSE(transaction.Active());
  EXPECT_EQ(host.fake.aborts, 0u);
}

TEST(ShmReporterSlots, EveryRetainedSlotIsRequired) {
  for (size_t missing = 0; missing < 9; ++missing) {
    Host host;
    host.fake.missing_slot = missing;
    ReporterBatchRegistration transaction;
    ShmReceiveReporter r;
    EXPECT_FALSE(PrepareShmReceiveReporterSlots(&host.api, &host.batch, transaction, r, "t", "ty", "shm"))
        << missing;
    EXPECT_FALSE(transaction.Active());
    EXPECT_EQ(host.fake.aborts, 1u) << missing;
  }
}

TEST(ShmReporterSlots, RetainedFamilyCarriesKindMetricsAndDriverLabel) {
  Host host;
  ReporterBatchRegistration transaction;
  ShmReceiveReporter r;
  ASSERT_TRUE(PrepareShmReceiveReporterSlots(&host.api, &host.batch, transaction, r, "t", "ty", "shm"));
  EXPECT_EQ(host.fake.metrics,
            (std::vector<std::string>{"receive_delivered", "callback_entered", "callback_completed",
                                      "callback_threw_std", "callback_threw_nonstd", "receive_released",
                                      "outstanding_current", "outstanding_peak", "limit_drop"}));
  EXPECT_EQ(host.fake.kinds, std::vector<std::string>(9, "retained_shm_receive"));
  EXPECT_EQ(host.fake.drivers, std::vector<std::string>(9, "shm"));
}

TEST(ShmReporterBatch, FailedPrepareAndForeignOwnerAreRejected) {
  Host host;
  host.fake.prepare_fails = true;
  ReporterBatchRegistration transaction;
  DirectLoanReporter r;
  EXPECT_FALSE(PrepareDirectLoanReporterSlots(&host.api, &host.batch, transaction, r, "t", "ty", "shm"));
  EXPECT_FALSE(transaction.Active());

  Host other;
  host.fake.prepare_fails = false;
  EXPECT_FALSE(PrepareDirectLoanReporterSlots(&host.api, &other.batch, transaction, r, "t", "ty", "shm"));
  EXPECT_EQ(other.fake.prepares, 0u);
  auto short_batch = host.batch;
  short_batch.struct_size = offsetof(ibmw_reporter_batch_v1, abort);
  EXPECT_FALSE(PrepareDirectLoanReporterSlots(&host.api, &short_batch, transaction, r, "t", "ty", "shm"));
  EXPECT_EQ(host.fake.prepares, 1u);
}

TEST(ShmReporterBatch, FailedCommitStaysPreparedAndCanBeRetried) {
  Host host;
  host.fake.commit_failures = 1;
  {
    ReporterBatchRegistration transaction;
    DirectLoanReporter r;
    ASSERT_TRUE(PrepareDirectLoanReporterSlots(&host.api, &host.batch, transaction, r, "t", "ty", "shm"));
    EXPECT_FALSE(transaction.Commit());
    EXPECT_TRUE(transaction.Active());
    EXPECT_TRUE(transaction.Commit());
  }
  EXPECT_EQ(host.fake.commits, 2u);
  EXPECT_EQ(host.fake.aborts, 0u);
  {
    // Never committed: aborted exactly once by the destructor.
    ReporterBatchRegistration transaction;
    DirectLoanReporter r;
    ASSERT_TRUE(PrepareDirectLoanReporterSlots(&host.api, &host.batch, transaction, r, "t", "ty", "shm"));
  }
  EXPECT_EQ(host.fake.aborts, 1u);
}

TEST(ShmReceiveReporter, LeasesTrackOutstandingAndPeak) {
  Host host;
  SubscriptionReporterState state;
  ASSERT_TRUE(
      PrepareShmReceiveReporterSlots(&host.api, &host.batch, state.transaction, state.reporter, "t", "ty", "shm"));
  // receive_delivered 0, callback_entered 1, receive_released 5,
  // outstanding_current 6, outstanding_peak 7, limit_drop 8
  ReportLeaseLocked(&state, true, 1);
  ReportLeaseLocked(&state, true, 2);
  ReportLeaseLocked(&state, false, 1);
  ReportLeaseLocked(&state, true, 2);
  ReportLeaseLocked(&state, false, 1);
  ReportLeaseLocked(&state, false, 0);
  ReportReceive(&state, &ShmReceiveReporter::callback_entered);
  ReportReceive(&state, &ShmReceiveReporter::limit_drop);
  ReportReceive(nullptr, &ShmReceiveReporter::limit_drop);
  ReportLeaseLocked(nullptr, true, 1);
  EXPECT_EQ(host.fake.values[0], 3);
  EXPECT_EQ(host.fake.values[1], 1);
  EXPECT_EQ(host.fake.values[5], 3);
  EXPECT_EQ(host.fake.values[6], 0);
  EXPECT_EQ(host.fake.values[7], 2);
  EXPECT_EQ(host.fake.values[8], 1);
}

// This binary is no reporter host: the driver turns reporting off.
TEST(ShmReporterBridge, NoHostMeansNoDriverState) { EXPECT_EQ(CreateDriverReporterState("shm"), nullptr); }

TEST(ShmReporterBridge, SubscriptionStagesCommitsAndLabels) {
  Host host;
  DriverReporterState driver{&host.api, &host.batch, "iceoryx"};
  ShmSubscriptionOptions options;
  ASSERT_TRUE(PrepareSubscriptionReporter(&driver, options, "t", "ty"));
  ASSERT_NE(options.reporter_state, nullptr);
  EXPECT_EQ(host.fake.commits, 0u);
  EXPECT_EQ(host.fake.drivers, std::vector<std::string>(9, "iceoryx"));
  EXPECT_TRUE(CommitSubscriptionReporter(options));
  EXPECT_EQ(host.fake.commits, 1u);
  EXPECT_TRUE(CommitSubscriptionReporter(options));  // already committed
  EXPECT_EQ(host.fake.commits, 1u);

  ShmSubscriptionOptions off;
  EXPECT_TRUE(PrepareSubscriptionReporter(nullptr, off, "t", "ty"));
  EXPECT_EQ(off.reporter_state, nullptr);
  EXPECT_TRUE(CommitSubscriptionReporter(off));
}

TEST(ShmReporterBridge, SubscriptionCommitFailureAborts) {
  Host host;
  host.fake.commit_failures = 1;
  DriverReporterState driver{&host.api, &host.batch, "shm"};
  ShmSubscriptionOptions options;
  ASSERT_TRUE(PrepareSubscriptionReporter(&driver, options, "t", "ty"));
  EXPECT_FALSE(CommitSubscriptionReporter(options));
  EXPECT_EQ(host.fake.aborts, 1u);
  options.reporter_state.reset();
  EXPECT_EQ(host.fake.aborts, 1u);
}

// outstanding_current needs add_signed: an older ABI cannot report it.
TEST(ShmReporterBridge, SubscriptionWithoutAddSignedFails) {
  Host host;
  host.api.struct_size = offsetof(ibmw_reporter_v1, add_signed);
  DriverReporterState driver{&host.api, &host.batch, "shm"};
  ShmSubscriptionOptions options;
  EXPECT_FALSE(PrepareSubscriptionReporter(&driver, options, "t", "ty"));
  EXPECT_EQ(host.fake.prepares, 0u);
  EXPECT_EQ(options.reporter_state, nullptr);
}

}  // namespace
}  // namespace ibmw::extensions::shm_extension
#endif
