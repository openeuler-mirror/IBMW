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

// A provider returning an odd key/value count must not make readers touch
// the element past the end ([3] is a sentinel that must never be read).
constexpr ibmw_string_view_t kOddKeyVals[] = {{"k0", 2}, {"v0", 2}, {"k1", 2}, {"OOB", 3}};

TEST(TransportContext, OddKeyValCountIgnoresTrailingKey) {
  Context real;
  ibmw_transport_context_base_ops_t ops = *real.NativeHandle()->ops;
  ops.get_meta_key_vals = [](void*) { return ibmw_string_view_array_t{kOddKeyVals, 3}; };
  const ibmw_transport_context_base_t base{.ops = &ops, .ctx = real.NativeHandle()->ctx};
  ContextRef ref(&base);

  const auto kv = ref.GetMetaKeyVals();
  EXPECT_EQ(kv.size(), 1u);
  EXPECT_EQ(kv.at("k0"), "v0");
  EXPECT_EQ(kv.count("k1"), 0u);
  EXPECT_EQ(ref.ToString().find("OOB"), std::string::npos);
}

}  // namespace
}  // namespace ibmw::transport
