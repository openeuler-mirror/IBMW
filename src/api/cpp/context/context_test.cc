// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <memory>

#include "cpp/context/context.h"

namespace ibmw::context {
namespace {

TEST(ContextTest, DefaultConstructionKeepsCurrentThreadContext) {
  details::ScopedThreadContextRestore restore;
  auto current = std::make_shared<Context>();
  current->LetMe();
  ASSERT_EQ(details::g_thread_ctx.ctx_ptr.lock(), current);

  auto other = std::make_shared<Context>();
  EXPECT_EQ(details::g_thread_ctx.ctx_ptr.lock(), current);

  other->LetMe();
  EXPECT_EQ(details::g_thread_ctx.ctx_ptr.lock(), other);
}

}  // namespace
}  // namespace ibmw::context
