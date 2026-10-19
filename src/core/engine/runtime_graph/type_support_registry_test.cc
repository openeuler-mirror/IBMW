// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/runtime_graph/type_support_registry.h"

#include <dlfcn.h>
#include <future>
#include <gtest/gtest.h>

namespace ibmw::runtime::core::runtime_graph {
namespace {
using namespace std::chrono_literals;

class TypeSupportRegistryTest : public testing::Test {
 protected:
  void SetUp() override {
    library_ = ::dlopen(TYPE_SUPPORT_V1_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
    ASSERT_NE(library_, nullptr);
    set_hook_ = reinterpret_cast<SetHook>(::dlsym(library_, "SetTypeSupportLoadHook"));
    count_ = reinterpret_cast<unsigned (*)()>(::dlsym(library_, "TypeSupportLoadCount"));
    ASSERT_NE(set_hook_, nullptr);
    ASSERT_NE(count_, nullptr);
    set_hook_(nullptr, nullptr);
    std::string path = "/tmp/ibmw-typesupport-registry-XXXXXX";
    ASSERT_NE(::mkdtemp(path.data()), nullptr);
    directory_ = path;
  }
  void TearDown() override {
    if (set_hook_) set_hook_(nullptr, nullptr);
    if (library_) ::dlclose(library_);
    if (!directory_.empty()) std::filesystem::remove_all(directory_);
  }
  using SetHook = void (*)(void (*)(void*), void*);
  void* library_ = nullptr;
  SetHook set_hook_ = nullptr;
  unsigned (*count_)() = nullptr;
  std::filesystem::path directory_;
};

TEST_F(TypeSupportRegistryTest, RepeatedDiscoveryLoadsCanonicalPackageOnlyOnce) {
  TypeSupportRegistry registry;
  const auto alias = directory_ / "alias.so";
  std::filesystem::create_symlink(TYPE_SUPPORT_V1_FIXTURE_PATH, alias);
  for (unsigned i = 0; i < 20; ++i) {
    registry.Load(TYPE_SUPPORT_V1_FIXTURE_PATH);
    EXPECT_TRUE(registry.TryLoad(alias.string()));
    registry.Load((directory_ / "." / "alias.so").string());
  }
  EXPECT_EQ(count_(), 1u);
  EXPECT_TRUE(registry.Find("json:fixture.Message"));
  EXPECT_TRUE(registry.FindSchema("json:fixture.Message", "json"));
  EXPECT_EQ(registry.PackagePath("json:fixture.Message"),
            std::filesystem::canonical(TYPE_SUPPORT_V1_FIXTURE_PATH).string());
  EXPECT_FALSE(registry.TryLoad((directory_ / "missing.so").string()));
}

TEST_F(TypeSupportRegistryTest, ExistingLookupsProceedWhileAnotherPackageLoads) {
  TypeSupportRegistry registry;
  registry.Load(TYPE_SUPPORT_V1_FIXTURE_PATH);
  // A separate DSO holds a second package load at a known point, without
  // requiring the build directory and /tmp to share a filesystem.
  const auto second = directory_ / "second.so";
  std::filesystem::copy_file(TYPE_SUPPORT_V1_FIXTURE_PATH, second);
  const auto close_library = [](void* handle) { ::dlclose(handle); };
  std::unique_ptr<void, decltype(close_library)> second_library(
      ::dlopen(second.c_str(), RTLD_NOW | RTLD_LOCAL), close_library);
  ASSERT_NE(second_library, nullptr);
  const auto set_second_hook = reinterpret_cast<SetHook>(
      ::dlsym(second_library.get(), "SetTypeSupportLoadHook"));
  const auto second_count = reinterpret_cast<unsigned (*)()>(
      ::dlsym(second_library.get(), "TypeSupportLoadCount"));
  ASSERT_NE(set_second_hook, nullptr);
  ASSERT_NE(second_count, nullptr);
  struct Gate {
    std::promise<void> entered, release;
    std::shared_future<void> released = release.get_future().share();
  } gate;
  auto entered = gate.entered.get_future();
  set_second_hook([](void* context) {
    auto& gate = *static_cast<Gate*>(context);
    gate.entered.set_value();
    gate.released.wait();
  }, &gate);
  auto loading = std::async(std::launch::async, [&] { registry.Load(second.string()); });
  const auto started = entered.wait_for(2s);
  auto lookup = std::async(std::launch::async, [&] {
    return static_cast<bool>(registry.Find("json:fixture.Message")) &&
           registry.FindSchema("json:fixture.Message", "json").has_value();
  });
  const auto ready = lookup.wait_for(500ms);
  gate.release.set_value();
  loading.get();
  EXPECT_EQ(second_count(), 1u);
  set_second_hook(nullptr, nullptr);
  EXPECT_EQ(started, std::future_status::ready);
  EXPECT_EQ(ready, std::future_status::ready);
  EXPECT_TRUE(lookup.get());
  EXPECT_EQ(count_(), 1u);
}
}  // namespace
}  // namespace ibmw::runtime::core::runtime_graph
