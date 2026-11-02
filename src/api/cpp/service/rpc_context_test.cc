// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include "cpp/service/rpc_context.h"

namespace ibmw::rpc {
namespace {

// A provider returning an odd key/value count must not make readers touch
// the element past the end ([3] is a sentinel that must never be read).
constexpr ibmw_string_view_t kOddKeyVals[] = {{"k0", 2}, {"v0", 2}, {"k1", 2}, {"OOB", 3}};

TEST(RpcContext, OddKeyValCountIgnoresTrailingKey) {
  Context real;
  ibmw_rpc_context_base_ops_t ops = *real.NativeHandle()->ops;
  ops.get_meta_key_vals = [](void*) { return ibmw_string_view_array_t{kOddKeyVals, 3}; };
  const ibmw_service_context_base_t base{.ops = &ops, .ctx = real.NativeHandle()->ctx};
  ContextRef ref(&base);

  const auto kv = ref.GetMetaKeyVals();
  EXPECT_EQ(kv.size(), 1u);
  EXPECT_EQ(kv.at("k0"), "v0");
  EXPECT_EQ(ref.ToString().find("OOB"), std::string::npos);

  Context copied(ref);
  EXPECT_EQ(copied.GetMetaValue("k0"), "v0");
  EXPECT_EQ(copied.GetMetaValue("k1"), "");
}

}  // namespace
}  // namespace ibmw::rpc
