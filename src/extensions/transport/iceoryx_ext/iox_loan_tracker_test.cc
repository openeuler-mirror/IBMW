// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <chrono>
#include <future>

#include "iceoryx_ext/iceoryx_bus.h"

namespace ibmw::extensions::iceoryx_extension {
namespace {

class ScopedTrackedLoanRelease {
 public:
  ScopedTrackedLoanRelease(IoxLoanTracker& tracker, int& releases)
      : tracker_(tracker), releases_(releases) {}
  ~ScopedTrackedLoanRelease() {
    tracker_.CompleteTrackedLoan([this] { ++releases_; });
  }

 private:
  IoxLoanTracker& tracker_;
  int& releases_;
};

TEST(IoxLoanTracker, ClosingRejectsNewLoansAndAllowsRetryAfterRelease) {
  IoxLoanTracker tracker;
  int loan_attempts = 0;

  ASSERT_TRUE(tracker.TryTrackLoan([&] {
    ++loan_attempts;
    return true;
  }));
  EXPECT_EQ(tracker.Outstanding(), 1U);

  tracker.BeginShutdown();
  EXPECT_TRUE(tracker.IsClosing());
  EXPECT_FALSE(tracker.TryTrackLoan([&] {
    ++loan_attempts;
    return true;
  }));
  EXPECT_EQ(loan_attempts, 1);
  EXPECT_FALSE(tracker.WaitForLoans(std::chrono::milliseconds(1)));

  int releases = 0;
  tracker.CompleteTrackedLoan([&] { ++releases; });
  EXPECT_EQ(releases, 1);
  EXPECT_EQ(tracker.Outstanding(), 0U);
  EXPECT_TRUE(tracker.WaitForLoans(std::chrono::milliseconds(0)));
}

TEST(IoxLoanTracker, FailedLoanIsNotOutstanding) {
  IoxLoanTracker tracker;
  EXPECT_FALSE(tracker.TryTrackLoan([] { return false; }));
  EXPECT_EQ(tracker.Outstanding(), 0U);
  tracker.BeginShutdown();
  EXPECT_TRUE(tracker.WaitForLoans(std::chrono::milliseconds(0)));
}

TEST(IoxLoanTracker, FailedGrowthKeepsOldLoanOutstandingUntilRaiiRelease) {
  IoxLoanTracker tracker;
  ASSERT_TRUE(tracker.TryTrackLoan([] { return true; }));
  int releases = 0;
  {
    ScopedTrackedLoanRelease raii_release(tracker, releases);
    const size_t oversized =
        static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1U;

    EXPECT_FALSE(tracker.ReplaceTrackedLoan([&] {
      return NextIoxShmSize(64U, oversized).has_value();
    }));
    EXPECT_EQ(tracker.Outstanding(), 1U);

    tracker.BeginShutdown();
    EXPECT_FALSE(tracker.WaitForLoans(std::chrono::milliseconds(1)));
  }

  EXPECT_EQ(releases, 1);
  EXPECT_EQ(tracker.Outstanding(), 0U);
  EXPECT_TRUE(tracker.WaitForLoans(std::chrono::milliseconds(0)));
}

TEST(IoxLoanTracker, SuccessfulReplacementKeepsOneOutstandingLoan) {
  IoxLoanTracker tracker;
  ASSERT_TRUE(tracker.TryTrackLoan([] { return true; }));

  EXPECT_TRUE(tracker.ReplaceTrackedLoan([] { return true; }));
  EXPECT_EQ(tracker.Outstanding(), 1U);

  tracker.CompleteTrackedLoan([] {});
  EXPECT_EQ(tracker.Outstanding(), 0U);
}

TEST(IoxLoanTracker, PublisherStopAndTimeoutRemainBoundedDuringBlockedPublish) {
  IoxLoanTracker tracker;
  ASSERT_TRUE(tracker.TryTrackLoan([] { return true; }));
  std::promise<void> publish_entered_promise;
  auto publish_entered = publish_entered_promise.get_future();
  std::promise<void> consumer_unblocks_promise;
  auto consumer_unblocks = consumer_unblocks_promise.get_future().share();
  auto publish = std::async(std::launch::async, [&] {
    tracker.CompleteTrackedLoan([&] {
      publish_entered_promise.set_value();
      consumer_unblocks.wait();
    });
  });
  ASSERT_EQ(publish_entered.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  const auto begin = std::chrono::steady_clock::now();
  tracker.BeginShutdown();
  EXPECT_FALSE(tracker.WaitForLoans(std::chrono::milliseconds(10)));
  EXPECT_LT(std::chrono::steady_clock::now() - begin,
            std::chrono::milliseconds(200));
  EXPECT_TRUE(tracker.IsClosing());

  consumer_unblocks_promise.set_value();
  EXPECT_EQ(publish.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  EXPECT_TRUE(tracker.WaitForLoans(std::chrono::milliseconds(10)));
}

TEST(IoxOperationGate, CloseIsIrreversibleAndDrainsEstablishedLease) {
  IoxOperationGate gate;
  auto lease = gate.TryAcquire();
  ASSERT_TRUE(lease);
  EXPECT_EQ(gate.Active(), 1U);

  gate.Close();
  EXPECT_FALSE(gate.Accepting());
  EXPECT_FALSE(gate.TryAcquire());
  EXPECT_FALSE(gate.WaitForQuiescence(std::chrono::milliseconds(0)));

  lease = {};
  EXPECT_TRUE(gate.WaitForQuiescence(std::chrono::milliseconds(0)));
  gate.Close();
  EXPECT_FALSE(gate.TryAcquire());
}

TEST(IoxOperationGate, PostCloseAdmissionIsRejectedWithoutSleep) {
  IoxOperationGate gate;
  std::promise<void> closed_promise;
  auto closed = closed_promise.get_future().share();
  auto admission = std::async(std::launch::async, [&] {
    closed.wait();
    return gate.TryAcquire();
  });

  gate.Close();
  closed_promise.set_value();
  EXPECT_FALSE(admission.get());
  EXPECT_TRUE(gate.WaitForQuiescence(std::chrono::milliseconds(0)));
}

TEST(IoxOperationGate, EstablishedLoanCompletesAfterAdmissionCloses) {
  IoxOperationGate gate;
  IoxLoanTracker tracker;
  {
    auto operation = gate.TryAcquire();
    ASSERT_TRUE(operation);
    ASSERT_TRUE(tracker.TryTrackLoan([] { return true; }));
  }

  gate.Close();
  EXPECT_TRUE(gate.WaitForQuiescence(std::chrono::milliseconds(0)));
  EXPECT_FALSE(gate.TryAcquire());
  EXPECT_EQ(tracker.Outstanding(), 1U);

  int completions = 0;
  tracker.CompleteTrackedLoan([&] { ++completions; });
  EXPECT_EQ(completions, 1);
  EXPECT_TRUE(tracker.WaitForLoans(std::chrono::milliseconds(0)));
}

TEST(IoxOperationGate, LookupBeforeCloseCannotLoanAfterPublisherShutdownBegins) {
  IoxOperationGate gate;
  IoxLoanTracker tracker;
  std::promise<void> lookup_complete_promise;
  auto lookup_complete = lookup_complete_promise.get_future();
  std::promise<void> publisher_closing_promise;
  auto publisher_closing = publisher_closing_promise.get_future().share();

  auto operation = std::async(std::launch::async, [&] {
    auto lease = gate.TryAcquire();
    if (!lease) return true;
    lookup_complete_promise.set_value();
    publisher_closing.wait();
    return !tracker.TryTrackLoan([] { return true; });
  });

  ASSERT_EQ(lookup_complete.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  gate.Close();
  tracker.BeginShutdown();
  publisher_closing_promise.set_value();

  EXPECT_TRUE(operation.get());
  EXPECT_TRUE(gate.WaitForQuiescence(std::chrono::milliseconds(0)));
  EXPECT_EQ(tracker.Outstanding(), 0U);
}

TEST(IoxCallbackContextRegistry, FailedRetriesDoNotAccumulateContexts) {
  IoxBusControl control;
  constexpr size_t kRetries = 8;

  for (size_t i = 0; i < kRetries; ++i) {
    auto context = std::make_shared<IoxSubscriptionCallbackContext>(
        std::weak_ptr<IoxSubscriptionState>{});
    std::lock_guard<std::mutex> lock(control.mutex);
    control.callback_contexts.Add(context);
    ASSERT_EQ(control.callback_contexts.Size(), 1U);
    EXPECT_TRUE(control.callback_contexts.Remove(context.get()));
    EXPECT_EQ(control.callback_contexts.Size(), 0U);
  }
}

TEST(IoxRegistrationGuard, SharedControlOutlivesOwnerAndNotifiesCompletion) {
  auto owner = std::make_shared<IoxBusControl>();
  std::weak_ptr<IoxBusControl> weak_control = owner;
  auto guard = std::make_unique<IoxRegistrationGuard>(owner);
  {
    std::lock_guard<std::mutex> lock(owner->mutex);
    guard->StartLocked();
    ASSERT_EQ(owner->registrations_in_flight, 1U);
  }

  auto waiter_control = owner;
  auto completion = std::async(std::launch::async, [waiter_control] {
    std::unique_lock<std::mutex> lock(waiter_control->mutex);
    waiter_control->registration_cv.wait(lock, [&waiter_control] {
      return waiter_control->registrations_in_flight == 0;
    });
    return true;
  });
  owner.reset();
  ASSERT_FALSE(weak_control.expired());

  guard.reset();
  EXPECT_EQ(completion.wait_for(std::chrono::milliseconds(100)),
            std::future_status::ready);
  EXPECT_TRUE(completion.get());

  waiter_control.reset();
  EXPECT_TRUE(weak_control.expired());
}

}  // namespace
}  // namespace ibmw::extensions::iceoryx_extension
