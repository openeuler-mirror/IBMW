// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <string>

#include "engine/module/package_loader.h"

namespace ibmw::runtime::core::module {
namespace {

int LiveModules(PackageLoader& loader) {
  auto* live = reinterpret_cast<int (*)()>(loader.GetDynamicLib().GetSymbol("IbmwPkgFixtureLiveModules"));
  return live == nullptr ? -1 : live();
}

std::string LoadError(PackageLoader& loader, const char* path,
                      const std::vector<std::string>& enable_modules = {}) {
  try {
    loader.LoadPkg(path, {}, enable_modules);
  } catch (const std::exception& e) {
    return e.what();
  }
  return {};
}

// CORE-023: a module failing the name check destroys the ones created before.
TEST(PackageLoaderTest, FailedLoadDestroysModulesCreatedBefore) {
  PackageLoader loader;
  EXPECT_NE(LoadError(loader, PKG_FIXTURE_PATH).find("renamed"), std::string::npos);
  EXPECT_EQ(LiveModules(loader), 0);
  EXPECT_TRUE(loader.GetLoadedModuleNameList().empty());
  EXPECT_EQ(loader.GetModule("m0"), nullptr);
}

TEST(PackageLoaderTest, ModulesOutsideTheFailureStayLoaded) {
  PackageLoader loader;
  EXPECT_EQ(LoadError(loader, PKG_FIXTURE_PATH, {"m0", "m1"}), "");
  EXPECT_EQ(LiveModules(loader), 2);
  EXPECT_NE(loader.GetModule("m1"), nullptr);
  loader.UnLoadPkg();  // the fixture's module count is process wide
}

// CORE-021: the module count of a package is bounded before its name list is read.
TEST(PackageLoaderTest, ModuleCountIsBounded) {
  PackageLoader loader;
  EXPECT_NE(LoadError(loader, PKG_TOO_MANY_FIXTURE_PATH).find("modules"), std::string::npos);
  EXPECT_EQ(LiveModules(loader), 0);
}

}  // namespace
}  // namespace ibmw::runtime::core::module
