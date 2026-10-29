// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression test: transport::Context copies/moves must not keep using the
// source object's memory pool after the source is destroyed.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "cpp/transport/transport_context.h"

namespace ibmw::transport {
namespace {

std::unique_ptr<Context> MakeFilled() {
  auto src = std::make_unique<Context>();
  for (int i = 0; i < 16; ++i) {
    src->SetMetaValue("key_" + std::to_string(i), std::string(40, static_cast<char>('a' + i)));
  }
  return src;
}

void ExpectFilled(const Context& c) {
  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(c.GetMetaValue("key_" + std::to_string(i)), std::string(40, static_cast<char>('a' + i)));
  }
}

TEST(TransportContext, MoveSurvivesSourceDestruction) {
  auto src = MakeFilled();
  Context moved(std::move(*src));
  src.reset();
  ExpectFilled(moved);
  moved.SetMetaValue("extra", std::string(100, 'z'));
  EXPECT_EQ(moved.GetMetaValue("extra"), std::string(100, 'z'));
}

TEST(TransportContext, CopySurvivesSourceDestruction) {
  auto src = MakeFilled();
  Context copied(*src);
  src.reset();
  ExpectFilled(copied);
}

}  // namespace
}  // namespace ibmw::transport
