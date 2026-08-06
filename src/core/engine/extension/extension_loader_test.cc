// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <dlfcn.h>

#include <cstdint>
#include <memory>

#include <gtest/gtest.h>

#include "base/exception.h"
#include "engine/extension/extension_loader.h"

namespace ibmw::runtime::core::extension {
namespace {

struct LibraryCloser {
  void operator()(void* library) const noexcept { dlclose(library); }
};

using Library = std::unique_ptr<void, LibraryCloser>;

Library OpenFixture(const char* path) {
  dlerror();
  return Library(dlopen(path, RTLD_NOW | RTLD_LOCAL));
}

uint32_t CreateCount(void* library) {
  dlerror();
  auto* symbol = dlsym(library, "IbmwExtFixtureCreateCount");
  EXPECT_NE(symbol, nullptr) << dlerror();
  return symbol == nullptr ? UINT32_MAX
                           : reinterpret_cast<uint32_t (*)()>(symbol)();
}

TEST(ExtensionLoaderAbi, V2OnlyFixtureFailsClosedForLegacyLoader) {
  auto library = OpenFixture(EXTENSION_V2_FIXTURE_PATH);
  ASSERT_NE(library, nullptr) << dlerror();

  dlerror();
  EXPECT_EQ(dlsym(library.get(), "IbmwExtEntry"), nullptr);
  EXPECT_EQ(CreateCount(library.get()), 0U);
}

TEST(ExtensionLoaderAbi, LoadsValidatedV2OnlyFixture) {
  auto library = OpenFixture(EXTENSION_V2_FIXTURE_PATH);
  ASSERT_NE(library, nullptr) << dlerror();

  ExtensionLoader loader;
  loader.LoadExtension(EXTENSION_V2_FIXTURE_PATH);
  ASSERT_NE(loader.GetExtension(), nullptr);
  EXPECT_EQ(loader.GetExtension()->Name(), "extension-v2-fixture");
  EXPECT_EQ(CreateCount(library.get()), 1U);
  loader.UnLoadExtension();
}

TEST(ExtensionLoaderAbi, FallsBackToLegacyEntry) {
  auto library = OpenFixture(EXTENSION_LEGACY_FIXTURE_PATH);
  ASSERT_NE(library, nullptr) << dlerror();

  ExtensionLoader loader;
  loader.LoadExtension(EXTENSION_LEGACY_FIXTURE_PATH);
  ASSERT_NE(loader.GetExtension(), nullptr);
  EXPECT_EQ(loader.GetExtension()->Name(), "extension-v2-fixture");
  EXPECT_EQ(CreateCount(library.get()), 1U);
  loader.UnLoadExtension();
}

TEST(ExtensionLoaderAbi, RejectsWrongV2VersionBeforeCreate) {
  auto library = OpenFixture(EXTENSION_V2_BAD_VERSION_FIXTURE_PATH);
  ASSERT_NE(library, nullptr) << dlerror();

  ExtensionLoader loader;
  EXPECT_THROW(loader.LoadExtension(EXTENSION_V2_BAD_VERSION_FIXTURE_PATH),
               ibmw::common::util::IbmwException);
  EXPECT_EQ(CreateCount(library.get()), 0U);
  EXPECT_FALSE(loader.GetDynamicLib().IsLoaded());

  EXPECT_NO_THROW(loader.LoadExtension(EXTENSION_V2_FIXTURE_PATH));
  EXPECT_NE(loader.GetExtension(), nullptr);
  loader.UnLoadExtension();
}

TEST(ExtensionLoaderAbi, RejectsWrongV2SizeBeforeCreate) {
  auto library = OpenFixture(EXTENSION_V2_BAD_SIZE_FIXTURE_PATH);
  ASSERT_NE(library, nullptr) << dlerror();

  ExtensionLoader loader;
  EXPECT_THROW(loader.LoadExtension(EXTENSION_V2_BAD_SIZE_FIXTURE_PATH),
               ibmw::common::util::IbmwException);
  EXPECT_EQ(CreateCount(library.get()), 0U);
  EXPECT_FALSE(loader.GetDynamicLib().IsLoaded());

  EXPECT_NO_THROW(loader.LoadExtension(EXTENSION_V2_FIXTURE_PATH));
  EXPECT_NE(loader.GetExtension(), nullptr);
  loader.UnLoadExtension();
}

TEST(ExtensionLoaderAbi, RejectsSecondLoadAndKeepsFirstExtension) {
  auto second_library = OpenFixture(EXTENSION_LEGACY_FIXTURE_PATH);
  ASSERT_NE(second_library, nullptr) << dlerror();

  ExtensionLoader loader;
  loader.LoadExtension(EXTENSION_V2_FIXTURE_PATH);
  auto* first_extension = loader.GetExtension();
  ASSERT_NE(first_extension, nullptr);

  EXPECT_THROW(loader.LoadExtension(EXTENSION_LEGACY_FIXTURE_PATH),
               ibmw::common::util::IbmwException);
  EXPECT_EQ(loader.GetExtension(), first_extension);
  EXPECT_EQ(loader.GetExtension()->Name(), "extension-v2-fixture");
  EXPECT_EQ(CreateCount(second_library.get()), 0U);
  EXPECT_TRUE(loader.GetDynamicLib().IsLoaded());
  EXPECT_NO_THROW(loader.UnLoadExtension());
  EXPECT_EQ(loader.GetExtension(), nullptr);
}

TEST(ExtensionLoaderAbi, DestroyThrowLeaksDsoAndClearsLoaderState) {
  ExtensionLoader loader;
  loader.LoadExtension(EXTENSION_V2_DESTROY_THROWS_FIXTURE_PATH);
  ASSERT_NE(loader.GetExtension(), nullptr);

  EXPECT_NO_THROW(loader.UnLoadExtension());
  EXPECT_EQ(loader.GetExtension(), nullptr);
  EXPECT_FALSE(loader.GetDynamicLib().IsLoaded());
  EXPECT_NO_THROW(loader.UnLoadExtension());

#if defined(RTLD_NOLOAD)
  dlerror();
  Library still_mapped(
      dlopen(EXTENSION_V2_DESTROY_THROWS_FIXTURE_PATH,
             RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD));
  ASSERT_NE(still_mapped, nullptr) << dlerror();
  EXPECT_EQ(CreateCount(still_mapped.get()), 1U);
#endif
}

}  // namespace
}  // namespace ibmw::runtime::core::extension
