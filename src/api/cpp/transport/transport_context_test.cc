// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression test: transport::Context copies/moves must not keep using the
// source object's memory pool after the source is destroyed.

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

// API-026: get_meta_keys/get_meta_key_vals hand out arrays owned by the
// context; reading them never overwrites another reader's result.
TEST(TransportContext, MetaArraysDoNotOverwriteEachOther) {
  Context ctx;
  ctx.SetMetaValue("k0", "v0");
  ctx.SetMetaValue("k1", "v1");
  ContextRef ref(ctx);
  ref.GetMetaKeyValsArray();
  const auto keys = ref.GetMetaKeysArray();
  ref.GetMetaKeyValsArray();
  ASSERT_EQ(keys.len, 2u);
  for (size_t i = 0; i < keys.len; ++i) {
    EXPECT_EQ(ibmw::util::ToStdStringView(keys.str_array[i]).front(), 'k');
  }

  ctx.SetMetaValue("k2", "v2");
  EXPECT_EQ(ref.GetMetaKeysArray().len, 3u);
  EXPECT_EQ(ref.GetMetaKeyValsArray().len, 6u);
  ctx.Reset();
  EXPECT_EQ(ref.GetMetaKeysArray().len, 0u);
}

TEST(TransportContext, ConcurrentMetaArrayReadsAreConsistent) {
  Context ctx;
  for (int i = 0; i < 8; ++i) ctx.SetMetaValue("k" + std::to_string(i), "v" + std::to_string(i));
  ContextRef ref(ctx);
  std::atomic<int> bad{0};
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      for (int i = 0; i < 20000; ++i) {
        const auto arr = ref.GetMetaKeyValsArray();
        if (arr.len != 16) {
          ++bad;
          continue;
        }
        for (size_t j = 0; j < arr.len; ++j) {
          if (ibmw::util::ToStdStringView(arr.str_array[j]).front() != (j % 2 ? 'v' : 'k')) ++bad;
        }
      }
    });
  }
  for (auto& reader : readers) reader.join();
  EXPECT_EQ(bad.load(), 0);
}

// The arrays follow every change, including past the inline capacity.
TEST(TransportContext, MetaArraysTrackChanges) {
  const auto check = [](const Context& ctx, size_t expected) {
    const auto kv = ContextRef(ctx).GetMetaKeyValsArray();
    const auto keys = ContextRef(ctx).GetMetaKeysArray();
    ASSERT_EQ(keys.len, expected);
    ASSERT_EQ(kv.len, 2 * expected);
    for (size_t i = 0; i < expected; ++i) {
      const auto key = ibmw::util::ToStdStringView(kv.str_array[2 * i]);
      EXPECT_EQ(ibmw::util::ToStdStringView(keys.str_array[i]), key);
      EXPECT_EQ(ibmw::util::ToStdStringView(kv.str_array[2 * i + 1]), ctx.GetMetaValue(key)) << key;
    }
  };
  Context ctx;
  for (int i = 0; i < 10; ++i) ctx.SetMetaValue("k" + std::to_string(i), "v" + std::to_string(i));
  check(ctx, 10);
  ctx.SetMetaValue("k1", std::string(100, 'x'));
  ctx.SetMetaValue("k7", std::string(100, 'y'));
  check(ctx, 10);

  const Context copy(ctx);
  check(copy, 10);
  EXPECT_NE(ContextRef(copy).GetMetaKeysArray().str_array[0].str,
            ContextRef(ctx).GetMetaKeysArray().str_array[0].str);

  ctx.Reset();
  check(ctx, 0);
  ctx.SetMetaValue("a", "b");
  check(ctx, 1);
}


// COMM-020: of several concurrent claims on one context exactly one wins.
TEST(TransportContextUsed, TrySetUsedHasExactlyOneWinner) {
  constexpr int kThreads = 4;
  for (int round = 0; round < 500; ++round) {
    Context ctx;
    ContextRef ref(ctx);
    std::atomic<int> start{0};
    std::atomic<int> winners{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&] {
        ++start;
        while (start.load() < kThreads) {
        }
        if (ref.TrySetUsed()) ++winners;
      });
    }
    for (auto& th : threads) th.join();
    ASSERT_EQ(winners.load(), 1) << "round " << round;
    EXPECT_TRUE(ctx.CheckUsed());
  }
}

TEST(TransportContextUsed, TrySetUsedAfterResetAndWithoutOp) {
  Context ctx;
  ContextRef ref(ctx);
  EXPECT_TRUE(ref.TrySetUsed());
  EXPECT_FALSE(ref.TrySetUsed());
  ctx.Reset();
  EXPECT_TRUE(ref.TrySetUsed());

  // A table without try_set_used cannot claim the context.
  ibmw_transport_context_base_ops_t ops = *ctx.NativeHandle()->ops;
  ops.try_set_used = nullptr;
  ctx.Reset();
  const ibmw_transport_context_base_t base{.ops = &ops, .ctx = &ctx};
  EXPECT_FALSE(ContextRef(&base).TrySetUsed());
  EXPECT_FALSE(ctx.CheckUsed());
}

}  // namespace
}  // namespace ibmw::transport
