// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// API-046: AsyncScope::complete() works on every executor backend without a
// macro that renames other `complete` members.

#include <gtest/gtest.h>

namespace {
// Declared before the scope header, like a type from another library.
struct Job {
  int done = 0;
  void complete() { done = 1; }
};
}  // namespace

#include "cpp/co/async_scope.h"
#include "cpp/co/sync_wait.h"

namespace ibmw::co {
namespace {

TEST(AsyncScope, CompleteKeepsOtherCompleteMembers) {
  AsyncScope scope;
  SyncWait(scope.complete());

  Job job;
  job.complete();
  EXPECT_EQ(job.done, 1);
}

}  // namespace
}  // namespace ibmw::co
