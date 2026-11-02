// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for ibmw::util::Function storage/assignment safety.

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "cpp/executor/executor.h"

namespace ibmw::util {
namespace {

using Task = ibmw::executor::Task;

struct Counters {
  int alive = 0;
  int calls = 0;
};

struct Tracked {
  explicit Tracked(Counters* c) : c(c) { ++c->alive; }
  Tracked(const Tracked& o) : c(o.c) { ++c->alive; }
  Tracked(Tracked&& o) noexcept : c(o.c) { ++c->alive; }
  ~Tracked() { --c->alive; }
  void operator()() const { ++c->calls; }
  Counters* c;
};

struct ThrowOnCopy {
  ThrowOnCopy() = default;
  ThrowOnCopy(const ThrowOnCopy&) { throw std::runtime_error("copy"); }
  ThrowOnCopy(ThrowOnCopy&&) noexcept = default;
  void operator()() const {}
};

TEST(UtilFunction, AssignmentThatThrowsLeavesFunctionEmpty) {
  Counters c;
  {
    Task t(Tracked{&c});
    ASSERT_EQ(c.alive, 1);
    const ThrowOnCopy thrower;
    EXPECT_THROW(t = thrower, std::runtime_error);
    // The old callable is destroyed exactly once and nothing dangles.
    EXPECT_EQ(c.alive, 0);
    EXPECT_FALSE(static_cast<bool>(t));
    t = Tracked{&c};
    t();
    EXPECT_EQ(c.calls, 1);
  }
  EXPECT_EQ(c.alive, 0);
}

TEST(UtilFunction, EmptyStatesAreObservableBeforeCalling) {
  Task empty;
  EXPECT_FALSE(static_cast<bool>(empty));
  Task from_null(nullptr);
  EXPECT_FALSE(static_cast<bool>(from_null));
  Task from_null_base(static_cast<ibmw_function_base_t*>(nullptr));
  EXPECT_FALSE(static_cast<bool>(from_null_base));
}

#ifndef NDEBUG
TEST(UtilFunctionDeathTest, CallingEmptyFunctionAssertsInDebugBuilds) {
  Task empty;
  EXPECT_DEATH(empty(), "empty ibmw::util::Function");
}
#endif

// Small enough for the inline buffer, but more strictly aligned than it.
struct alignas(32) OverAligned {
  bool* ok;
  void operator()() const { *ok = (reinterpret_cast<std::uintptr_t>(this) % 32) == 0; }
};
static_assert(sizeof(OverAligned) <= IBMW_FUNCTION_LOCAL_BUF_SIZE);

TEST(UtilFunction, OverAlignedCallableIsProperlyAligned) {
  // Consecutive Tasks land on different 32-byte phases.
  std::vector<Task> tasks;
  bool ok[8] = {};
  for (auto& o : ok) tasks.emplace_back(OverAligned{&o});
  for (auto& t : tasks) t();
  for (bool o : ok) EXPECT_TRUE(o);
}

struct ThrowingMove {
  ThrowingMove(Counters* c) : c(c) {}
  ThrowingMove(const ThrowingMove&) = default;
  ThrowingMove(ThrowingMove&& o) noexcept(false) : c(o.c) {}
  void operator()() const { ++c->calls; }
  Counters* c;
};

TEST(UtilFunction, ThrowingMoveCallableSurvivesRelocation) {
  Counters c;
  Task t(ThrowingMove{&c});
  Task moved(std::move(t));
  moved();
  EXPECT_EQ(c.calls, 1);
}

}  // namespace
}  // namespace ibmw::util
