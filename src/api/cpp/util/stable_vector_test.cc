// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cpp/util/stable_vector.h"

namespace ibmw::util {
namespace {

TEST(StableVector, IndicesAndReferencesStayValidWhileGrowing) {
  StableVector<std::string> v;
  std::vector<std::string*> refs;
  for (size_t i = 0; i < 5000; ++i) {
    EXPECT_EQ(v.emplace_back(std::to_string(i)), i);
    refs.push_back(&v[i]);
  }
  ASSERT_EQ(v.size(), 5000u);
  for (size_t i = 0; i < 5000; ++i) {
    EXPECT_EQ(&v[i], refs[i]);
    EXPECT_EQ(v[i], std::to_string(i));
  }
}

TEST(StableVector, DestroysElements) {
  auto token = std::make_shared<int>(0);
  {
    StableVector<std::shared_ptr<int>> v;
    for (int i = 0; i < 100; ++i) v.emplace_back(token);
    EXPECT_EQ(token.use_count(), 101);
  }
  EXPECT_EQ(token.use_count(), 1);
}

TEST(StableVector, ConcurrentAppendAndRead) {
  StableVector<size_t> v;
  v.emplace_back(size_t{0});
  const size_t& first = v[0];
  std::vector<std::thread> writers;
  for (int t = 0; t < 4; ++t) {
    writers.emplace_back([&v] {
      for (size_t i = 0; i < 2000; ++i) {
        const size_t idx = v.emplace_back(i);
        EXPECT_EQ(v[idx], i);
      }
    });
  }
  for (int i = 0; i < 10000; ++i) EXPECT_EQ(first, 0u);
  for (auto& w : writers) w.join();
  EXPECT_EQ(v.size(), 8001u);
}

}  // namespace
}  // namespace ibmw::util
