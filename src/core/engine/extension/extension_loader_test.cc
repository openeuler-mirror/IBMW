// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <dlfcn.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "base/exception.h"
#include "engine/engine.h"
#include "engine/extension/extension_bus.h"
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

uint32_t InitializationCount(void* library, const char* name) {
  auto* symbol = dlsym(library, name);
  EXPECT_NE(symbol, nullptr) << dlerror();
  return symbol ? reinterpret_cast<uint32_t (*)()>(symbol)() : UINT32_MAX;
}

TEST(ExtensionLoaderAbi, RejectsRuntimeV1WithoutCallingEitherInitializer) {
  auto library = OpenFixture(EXTENSION_RUNTIME_V1_FIXTURE_PATH);
  ASSERT_NE(library, nullptr) << dlerror();
  Engine engine;
  ExtensionBus bus;
  bus.RegisterCorePtr(&engine);
  YAML::Node options;
  options["items"][0]["name"] = "extension-v2-fixture";
  options["items"][0]["path"] = EXTENSION_RUNTIME_V1_FIXTURE_PATH;
  try {
    bus.Initialize(options);
    FAIL() << "An incompatible Runtime ABI was accepted";
  } catch (const ibmw::common::util::IbmwException& error) {
    EXPECT_NE(std::string(error.what()).find("requires Runtime ABI 1"),
              std::string::npos);
  }
  EXPECT_EQ(InitializationCount(library.get(), "IbmwExtFixtureEngineInitCount"), 0u);
  EXPECT_EQ(InitializationCount(library.get(), "IbmwExtFixtureRuntimeInitCount"), 0u);
}

TEST(ExtensionLoaderAbi, InitializesMatchingRuntimeV2AcrossDso) {
  auto library = OpenFixture(EXTENSION_RUNTIME_V2_FIXTURE_PATH);
  ASSERT_NE(library, nullptr) << dlerror();
  Engine engine;
  ExtensionBus bus;
  bus.RegisterCorePtr(&engine);
  YAML::Node options;
  options["items"][0]["name"] = "extension-v2-fixture";
  options["items"][0]["path"] = EXTENSION_RUNTIME_V2_FIXTURE_PATH;
  EXPECT_NO_THROW(bus.Initialize(options));
  EXPECT_EQ(InitializationCount(library.get(), "IbmwExtFixtureEngineInitCount"), 0u);
  EXPECT_EQ(InitializationCount(library.get(), "IbmwExtFixtureRuntimeInitCount"), 1u);
  bus.Shutdown();
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

// CORE-016: the resolved library path never throws and is absolute, for both
// explicit paths and bare names resolved through the loader search path.
TEST(DynamicLibPath, ResolvesFullPathWithoutThrowing) {
  ibmw::common::util::DynamicLib by_path;
  ASSERT_TRUE(by_path.Load(EXTENSION_V2_FIXTURE_PATH));
  EXPECT_EQ(by_path.GetLibFullPath(),
            std::filesystem::canonical(EXTENSION_V2_FIXTURE_PATH).string());

#if defined(__GLIBC__)  // soname of a library resolved through the search path
  ibmw::common::util::DynamicLib by_name;
  ASSERT_TRUE(by_name.Load("libm.so.6"));
  EXPECT_TRUE(std::filesystem::path(by_name.GetLibFullPath()).is_absolute()) << by_name.GetLibFullPath();
  EXPECT_TRUE(std::filesystem::exists(by_name.GetLibFullPath())) << by_name.GetLibFullPath();
#endif
}

// CORE-026: a lib whose extension name does not match the config must have its
// extension destroyed instead of leaked together with the DSO.
TEST(ExtensionBusLifecycle, NameMismatchDestroysLoadedExtension) {
  auto library = OpenFixture(EXTENSION_V2_FIXTURE_PATH);
  ASSERT_NE(library, nullptr) << dlerror();
  const uint32_t created_before = CreateCount(library.get());
  const uint32_t destroyed_before = InitializationCount(library.get(), "IbmwExtFixtureDestroyCount");

  Engine engine;
  ExtensionBus bus;
  bus.RegisterCorePtr(&engine);
  YAML::Node options;
  options["items"][0]["name"] = "some-other-name";
  options["items"][0]["path"] = EXTENSION_V2_FIXTURE_PATH;
  EXPECT_ANY_THROW(bus.Initialize(options));

  EXPECT_EQ(CreateCount(library.get()), created_before + 1U);
  EXPECT_EQ(InitializationCount(library.get(), "IbmwExtFixtureDestroyCount"), destroyed_before + 1U);
  bus.Shutdown();
}

class CountingExtension final : public ibmw::EngineExtensionBase {
 public:
  CountingExtension(std::string name, bool init_ok) : name_(std::move(name)), init_ok_(init_ok) {}
  std::string_view Name() const noexcept override { return name_; }
  bool Initialize(Engine*) noexcept override { return init_ok_; }
  void Shutdown() noexcept override { ++shutdown_count; }
  int shutdown_count = 0;

 private:
  std::string name_;
  bool init_ok_;
};

YAML::Node DirectExtensionOptions(std::initializer_list<const char*> names) {
  YAML::Node options;
  size_t i = 0;
  for (const char* name : names) {
    options["items"][i]["name"] = name;
    options["items"][i]["path"] = "";
    ++i;
  }
  return options;
}

// CORE-026: directly registered extensions are shut down too, exactly once,
// in reverse initialization order.
TEST(ExtensionBusLifecycle, ShutsDownDirectlyRegisteredExtensions) {
  Engine engine;
  ExtensionBus bus;
  CountingExtension first("first", true);
  CountingExtension second("second", true);
  bus.RegisterCorePtr(&engine);
  bus.RegisterExtension(&first);
  bus.RegisterExtension(&second);
  ASSERT_NO_THROW(bus.Initialize(DirectExtensionOptions({"first", "second"})));
  bus.Shutdown();
  bus.Shutdown();
  EXPECT_EQ(first.shutdown_count, 1);
  EXPECT_EQ(second.shutdown_count, 1);
}

// An extension whose Initialize() fails gets one Shutdown() to release what it
// acquired, and is not visited again by the bus Shutdown().
TEST(ExtensionBusLifecycle, FailedInitializeIsShutDownOnce) {
  Engine engine;
  ExtensionBus bus;
  CountingExtension good("good", true);
  CountingExtension bad("bad", false);
  bus.RegisterCorePtr(&engine);
  bus.RegisterExtension(&good);
  bus.RegisterExtension(&bad);
  EXPECT_ANY_THROW(bus.Initialize(DirectExtensionOptions({"good", "bad"})));
  bus.Shutdown();
  EXPECT_EQ(good.shutdown_count, 1);
  EXPECT_EQ(bad.shutdown_count, 1);
}

}  // namespace
}  // namespace ibmw::runtime::core::extension
