// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "iceoryx_ext/internal/reporter_sidecar.h"
#include "iceoryx_ext/internal/reporter_bridge.h"
#include "iceoryx_ext/iceoryx_bus.h"
#include "engine/transport/message_frame.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>

#ifdef IBMW_ENABLE_REPORTER_V1
namespace ibmw::extensions::iceoryx_extension {
namespace {

struct ReporterCounts {
  std::array<uint64_t, 3> values{};
};

struct SlotRegistrationFixture {
  size_t call = 0;
  size_t fail_at = 9;
  size_t adds = 0;
  size_t prepares = 0;
  size_t commits = 0;
  size_t aborts = 0;
  bool commit_succeeds = true;
  size_t commit_failures = 0;
  bool partial_success = false;
};

ibmw_reporter_slot_v1* RegisterSlot(
    void* owner, const ibmw_reporter_slot_descriptor_v1*) {
  auto& fixture = *static_cast<SlotRegistrationFixture*>(owner);
  const size_t current = fixture.call++;
  return current == fixture.fail_at
             ? nullptr
             : reinterpret_cast<ibmw_reporter_slot_v1*>(current + 1U);
}

void CountAdd(void* owner, ibmw_reporter_slot_v1*, uint64_t) {
  ++static_cast<SlotRegistrationFixture*>(owner)->adds;
}

int PrepareBatch(void* owner, const ibmw_reporter_slot_descriptor_v1*,
                 size_t count, ibmw_reporter_slot_v1** slots,
                 ibmw_reporter_batch_transaction_v1** transaction) {
  auto& fixture = *static_cast<SlotRegistrationFixture*>(owner);
  ++fixture.prepares;
  if (fixture.fail_at < count && !fixture.partial_success) return 0;
  for (size_t i = 0; i < count; ++i)
    slots[i] = fixture.partial_success && i == fixture.fail_at
                  ? nullptr
                  : reinterpret_cast<ibmw_reporter_slot_v1*>(i + 1U);
  *transaction = reinterpret_cast<ibmw_reporter_batch_transaction_v1*>(owner);
  return 1;
}
int CommitBatch(void* owner, ibmw_reporter_batch_transaction_v1*) {
  auto& fixture = *static_cast<SlotRegistrationFixture*>(owner);
  ++fixture.commits;
  if (fixture.commit_failures != 0) {
    --fixture.commit_failures;
    return 0;
  }
  return fixture.commit_succeeds;
}
void AbortBatch(void* owner, ibmw_reporter_batch_transaction_v1*) {
  ++static_cast<SlotRegistrationFixture*>(owner)->aborts;
}

ibmw_reporter_batch_v1 MakeBatch(SlotRegistrationFixture& fixture) {
  return {IBMW_REPORTER_BATCH_ABI_VERSION_V1, sizeof(ibmw_reporter_batch_v1),
          &fixture, PrepareBatch, CommitBatch, AbortBatch};
}

void Add(void* owner, ibmw_reporter_slot_v1* slot, uint64_t value) {
  auto* counts = static_cast<ReporterCounts*>(owner);
  const auto index = reinterpret_cast<uintptr_t>(slot) - 1U;
  counts->values.at(index) += value;
}

DirectReporterBinding MakeBinding(ReporterCounts& counts,
                                  ibmw_reporter_v1& api) {
  api.owner = &counts;
  api.add = Add;
  DirectReporterBinding binding;
  binding.api = &api;
  binding.slots.attempt = reinterpret_cast<ibmw_reporter_slot_v1*>(1U);
  binding.slots.success = reinterpret_cast<ibmw_reporter_slot_v1*>(2U);
  binding.slots.fail = reinterpret_cast<ibmw_reporter_slot_v1*>(3U);
  return binding;
}

TEST(DirectLoanReporterOutcome, UnreportedDoesNotRequireReporter) {
  DirectLoanReporterOutcome<false> outcome(nullptr);
  outcome.MarkSuccess();
}

TEST(DirectPublisherOps, DefaultOpsAreUnreported) {
  const auto ops = MakeUnreportedDirectPublisherOps();
  ASSERT_NE(ops, nullptr);
  EXPECT_FALSE(ops->Reported());
  EXPECT_EQ(ops->raw_loan, &DirectPublisherOps::RawLoanImpl<false>);
  EXPECT_EQ(ops->serialized_loan, &DirectPublisherOps::LoanImpl<false>);
  EXPECT_TRUE((ops->publish == &DirectPublisherOps::CompleteImpl<false, true>));
  EXPECT_TRUE((ops->release == &DirectPublisherOps::CompleteImpl<false, false>));
}

TEST(DirectPublisherOps, BindingApiMarksOpsReported) {
  DirectPublisherOps ops{};
  ibmw_reporter_v1 api{};
  ops.binding.api = &api;
  EXPECT_TRUE(ops.Reported());
}

TEST(DirectLoanReporterOutcome, ReportedSuccessIncrementsAttemptAndSuccess) {
  ReporterCounts counts;
  ibmw_reporter_v1 api{};
  auto binding = MakeBinding(counts, api);
  {
    DirectLoanReporterOutcome<true> outcome(&binding);
    outcome.MarkSuccess();
  }
  EXPECT_EQ(counts.values, (std::array<uint64_t, 3>{1, 1, 0}));
}

TEST(DirectLoanReporterOutcome, ReportedFailureIncrementsAttemptAndFailure) {
  ReporterCounts counts;
  ibmw_reporter_v1 api{};
  auto binding = MakeBinding(counts, api);
  { DirectLoanReporterOutcome<true> outcome(&binding); }
  EXPECT_EQ(counts.values, (std::array<uint64_t, 3>{1, 0, 1}));
}

TEST(ReporterSlots, EveryDirectSlotIsRequired) {
  DirectReporterSlots slots;
  auto* present = reinterpret_cast<ibmw_reporter_slot_v1*>(1U);
  std::array<ibmw_reporter_slot_v1**, 6> fields{
      &slots.attempt, &slots.success, &slots.fail,
      &slots.retry, &slots.publish, &slots.release};
  for (auto** field : fields) *field = present;
  ASSERT_TRUE(DirectReporterSlotsComplete(slots));
  for (auto** field : fields) {
    *field = nullptr;
    EXPECT_FALSE(DirectReporterSlotsComplete(slots));
    *field = present;
  }
}

TEST(ReporterSlots, EveryRetainedSlotIsRequiredAndFallbackDoesNotAdd) {
  for (size_t fail_at = 0; fail_at < 9; ++fail_at) {
    SlotRegistrationFixture fixture{.fail_at = fail_at};
    ibmw_reporter_v1 api{};
    api.owner = &fixture;
    api.register_slot = RegisterSlot;
    api.add = CountAdd;
    auto batch = MakeBatch(fixture);
    IoxReceiveReporter reporter;
    EXPECT_FALSE(RegisterIoxReceiveReporterSlots(
        &api, &batch, reporter, "topic", "type"));
    EXPECT_EQ(fixture.call, 0U);
    EXPECT_EQ(fixture.prepares, 1U);
    EXPECT_EQ(fixture.adds, 0U);
  }

  SlotRegistrationFixture fixture;
  ibmw_reporter_v1 api{};
  api.owner = &fixture;
  api.register_slot = RegisterSlot;
  auto batch = MakeBatch(fixture);
  IoxReceiveReporter reporter;
  EXPECT_TRUE(RegisterIoxReceiveReporterSlots(
      &api, &batch, reporter, "topic", "type"));
  EXPECT_EQ(fixture.call, 0U);
  EXPECT_EQ(fixture.prepares, 1U);
  EXPECT_EQ(fixture.commits, 1U);
}

TEST(ReporterBatchRegistration, CommitFailureAbortsExactlyOnce) {
  SlotRegistrationFixture fixture{.commit_succeeds = false};
  ibmw_reporter_v1 api{};
  api.owner = &fixture;
  auto batch = MakeBatch(fixture);
  {
    ReporterBatchRegistration transaction;
    std::array<ibmw_reporter_slot_v1*, 1> slots{};
    ASSERT_TRUE(transaction.Prepare(&api, &batch, slots, {"count"},
                                    "fake", "topic", "type"));
    EXPECT_FALSE(transaction.Commit());
    EXPECT_TRUE(transaction.Active());
    EXPECT_EQ(fixture.aborts, 0U);
  }
  EXPECT_EQ(fixture.commits, 1U);
  EXPECT_EQ(fixture.aborts, 1U);
}

TEST(ReporterBatchRegistration, CommitFailureCanRetryWithoutInvalidatingSlots) {
  SlotRegistrationFixture fixture{.commit_failures = 1};
  ibmw_reporter_v1 api{};
  api.owner = &fixture;
  auto batch = MakeBatch(fixture);
  ReporterBatchRegistration transaction;
  std::array<ibmw_reporter_slot_v1*, 1> slots{};
  ASSERT_TRUE(transaction.Prepare(&api, &batch, slots, {"count"},
                                  "fake", "topic", "type"));
  ASSERT_FALSE(transaction.Commit());
  ASSERT_TRUE(transaction.Active());
  ASSERT_TRUE(transaction.Commit());
  EXPECT_EQ(fixture.aborts, 0U);
}

TEST(ReporterBatchRegistration, StagedSlotsAcceptAddsBeforeCommit) {
  SlotRegistrationFixture fixture;
  ibmw_reporter_v1 api{};
  api.owner = &fixture;
  api.add = CountAdd;
  auto batch = MakeBatch(fixture);
  ReporterBatchRegistration transaction;
  std::array<ibmw_reporter_slot_v1*, 1> slots{};
  ASSERT_TRUE(transaction.Prepare(&api, &batch, slots, {"count"},
                                  "fake", "topic", "type"));
  api.add(api.owner, slots[0], 1);
  EXPECT_EQ(fixture.adds, 1U);
  EXPECT_TRUE(transaction.Commit());
  EXPECT_EQ(fixture.aborts, 0U);
}

TEST(ReporterBatchRegistration, PartialPrepareIsRejectedAndAborted) {
  for (size_t missing = 0; missing < 9; ++missing) {
    SlotRegistrationFixture fixture{.fail_at = missing, .partial_success = true};
    ibmw_reporter_v1 api{};
    api.owner = &fixture;
    api.add = CountAdd;
    auto batch = MakeBatch(fixture);
    ReporterBatchRegistration transaction;
    std::array<ibmw_reporter_slot_v1*, 9> slots{};
    slots.fill(reinterpret_cast<ibmw_reporter_slot_v1*>(0xfeed));
    EXPECT_FALSE(transaction.Prepare(
        &api, &batch, slots,
        std::array<std::string_view, 9>{"m0", "m1", "m2", "m3", "m4",
                                        "m5", "m6", "m7", "m8"},
        "fake", "topic", "type"));
    EXPECT_FALSE(transaction.Active());
    EXPECT_EQ(fixture.aborts, 1U);
    for (auto* slot : slots) EXPECT_EQ(slot, nullptr);
    EXPECT_EQ(fixture.adds, 0U);
  }
}

TEST(ReporterBridge, ShortReporterAbiDisablesRetainedSetupWithoutPreparing) {
  SlotRegistrationFixture fixture;
  ibmw_reporter_v1 api{};
  api.abi_version = IBMW_REPORTER_ABI_VERSION_V1;
  api.struct_size = offsetof(ibmw_reporter_v1, add_signed);
  api.owner = &fixture;
  api.register_slot = RegisterSlot;
  api.add = CountAdd;
  api.add_signed = [](void*, ibmw_reporter_slot_v1*, int64_t) {};
  auto batch = MakeBatch(fixture);
  DriverReporterState driver{&api, &batch};
  IoxSubscriberRegistrationOptions options;

  EXPECT_FALSE(PrepareSubscriptionReporter(
      &driver, options, "short/topic", "ShortType"));
  EXPECT_EQ(options.reporter_state, nullptr);
  EXPECT_EQ(fixture.prepares, 0U);
}

TEST(ReporterBridge, DuplicatePublisherRegistrationKeepsOneReporterFamily) {
  SlotRegistrationFixture fixture;
  fixture.prepares = 1;
  ibmw_reporter_v1 api{};
  api.owner = &fixture;
  auto batch = MakeBatch(fixture);
  DriverReporterState driver{&api, &batch};
  runtime::core::transport::TransportInfo info;

  EXPECT_TRUE(PreparePublisherReporter(
      &driver, reinterpret_cast<IoxPublisher*>(1U), info, false));
  EXPECT_EQ(fixture.prepares, 1U);
}

TEST(ReporterBridge,
     PublisherPrepareFailureRemovesNewEndpointAndRetryPreparesReplacement) {
  struct Endpoint {
    size_t generation;
  };
  std::unique_ptr<Endpoint> endpoint;
  size_t next_generation = 0;
  SlotRegistrationFixture fixture{.fail_at = 0};
  const auto register_endpoint = [&] {
    bool created = false;
    if (!endpoint) {
      endpoint = std::make_unique<Endpoint>(Endpoint{++next_generation});
      created = true;
    }
    return std::pair{endpoint.get(), created};
  };
  const auto attempt = [&] {
    const auto [publisher, created] = register_endpoint();
    const size_t generation = publisher->generation;
    const bool prepared = internal::PreparePublisherRegistration(
        created,
        [&] {
          std::array<ibmw_reporter_slot_v1*, 6> slots{};
          ibmw_reporter_batch_transaction_v1* transaction = nullptr;
          return PrepareBatch(&fixture, nullptr, slots.size(), slots.data(),
                              &transaction) != 0;
        },
        [&] {
          ASSERT_EQ(endpoint.get(), publisher);
          endpoint.reset();
        });
    return std::pair{prepared, generation};
  };

  const auto first = attempt();
  EXPECT_FALSE(first.first);
  EXPECT_EQ(first.second, 1U);
  EXPECT_EQ(endpoint, nullptr);

  fixture.fail_at = 9;
  const auto retry = attempt();
  EXPECT_TRUE(retry.first);
  EXPECT_EQ(retry.second, 2U);
  EXPECT_EQ(fixture.prepares, 2U);
  ASSERT_NE(endpoint, nullptr);
  EXPECT_EQ(endpoint->generation, 2U);
}

}  // namespace
}  // namespace ibmw::extensions::iceoryx_extension
#endif
