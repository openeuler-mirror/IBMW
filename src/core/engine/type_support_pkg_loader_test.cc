// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "base/exception.h"
#include "engine/type_support_pkg_loader.h"
#include "extension_api/type_support_package_loader.h"

namespace ibmw::runtime::core::util {
namespace {

TEST(TypeSupportPkgLoader, LoadsSchemaSupportV1) {
  TypeSupportPkgLoader loader;
  ASSERT_TRUE(loader.TryLoadTypeSupportPkg(TYPE_SUPPORT_V1_FIXTURE_PATH));
  ASSERT_EQ(loader.GetTypeSupportArray().size(), 1U);
  ASSERT_EQ(loader.GetSchemaSupportArray().size(), 1U);
  const auto* schema = loader.GetSchemaSupportArray().front();
  ASSERT_NE(schema, nullptr);
  EXPECT_EQ(schema->abi_version, IBMW_SCHEMA_SUPPORT_ABI_VERSION_V1);
  EXPECT_EQ(std::string_view(schema->type_name.str, schema->type_name.len),
            "json:fixture.Message");
  loader.UnLoadTypeSupportPkg();
}

TEST(TypeSupportPkgLoader, FailedReloadKeepsPreviouslyValidatedPackage) {
  TypeSupportPkgLoader loader;
  ASSERT_TRUE(loader.TryLoadTypeSupportPkg(TYPE_SUPPORT_V1_FIXTURE_PATH));
  const auto* original_schema = loader.GetSchemaSupportArray().front();

  EXPECT_FALSE(loader.TryLoadTypeSupportPkg(
      "/path/that/does/not/exist/libibmw_type_support.so"));
  ASSERT_TRUE(loader.GetDynamicLib().IsLoaded());
  ASSERT_EQ(loader.GetSchemaSupportArray().size(), 1U);
  EXPECT_EQ(loader.GetSchemaSupportArray().front(), original_schema);
  EXPECT_EQ(std::string_view(original_schema->schema_encoding.str,
                             original_schema->schema_encoding.len),
            "jsonschema");
  loader.UnLoadTypeSupportPkg();
}

TEST(TypeSupportPkgLoader, InvalidV1ReloadKeepsPreviouslyValidatedPackage) {
  TypeSupportPkgLoader loader;
  ASSERT_TRUE(loader.TryLoadTypeSupportPkg(TYPE_SUPPORT_V1_FIXTURE_PATH));
  const auto* original_schema = loader.GetSchemaSupportArray().front();

  EXPECT_THROW(
      loader.TryLoadTypeSupportPkg(TYPE_SUPPORT_BAD_V1_FIXTURE_PATH),
      ibmw::common::util::IbmwException);
  ASSERT_TRUE(loader.GetDynamicLib().IsLoaded());
  ASSERT_EQ(loader.GetSchemaSupportArray().size(), 1U);
  EXPECT_EQ(loader.GetSchemaSupportArray().front(), original_schema);
  loader.UnLoadTypeSupportPkg();
}

TEST(TypeSupportPkgLoader, RejectsTruncatedSchemaDescriptor) {
  TypeSupportPkgLoader loader;
  EXPECT_THROW(
      loader.TryLoadTypeSupportPkg(TYPE_SUPPORT_BAD_SIZE_V1_FIXTURE_PATH),
      ibmw::common::util::IbmwException);
  EXPECT_FALSE(loader.GetDynamicLib().IsLoaded());
  EXPECT_TRUE(loader.GetSchemaSupportArray().empty());
}

TEST(TypeSupportPkgLoader, RejectsTruncatedSchemaPackage) {
  TypeSupportPkgLoader loader;
  EXPECT_THROW(
      loader.TryLoadTypeSupportPkg(
          TYPE_SUPPORT_BAD_PACKAGE_SIZE_V1_FIXTURE_PATH),
      ibmw::common::util::IbmwException);
  EXPECT_FALSE(loader.GetDynamicLib().IsLoaded());
}

// COMM-024: packages built before msg_min_size existed must be rebuilt.
TEST(TypeSupportPkgLoader, RejectsOlderPackageAbi) {
  TypeSupportPkgLoader loader;
  try {
    loader.TryLoadTypeSupportPkg(TYPE_SUPPORT_OLD_PACKAGE_FIXTURE_PATH);
    FAIL() << "older package ABI was accepted";
  } catch (const ibmw::common::util::IbmwException& e) {
    EXPECT_NE(std::string(e.what()).find("older ABI"), std::string::npos) << e.what();
  }
  EXPECT_FALSE(loader.GetDynamicLib().IsLoaded());
}

TEST(TypeSupportPkgLoader, RejectsNullTypeSupportEntry) {
  TypeSupportPkgLoader loader;
  EXPECT_THROW(
      loader.TryLoadTypeSupportPkg(TYPE_SUPPORT_NULL_TYPE_V1_FIXTURE_PATH),
      ibmw::common::util::IbmwException);
  EXPECT_FALSE(loader.GetDynamicLib().IsLoaded());
  EXPECT_TRUE(loader.GetTypeSupportArray().empty());
}

// CORE-042: the runtime loader and the extension loader share one set of
// checks, so they accept and reject the same packages with the same message.
TEST(TypeSupportPkgLoader, ExtensionLoaderMatchesRuntimeLoader) {
  {
    TypeSupportPkgLoader runtime_loader;
    ibmw::extension::TypeSupportPackageLoader extension_loader;
    ASSERT_TRUE(runtime_loader.TryLoadTypeSupportPkg(TYPE_SUPPORT_V1_FIXTURE_PATH));
    ASSERT_TRUE(extension_loader.TryLoad(TYPE_SUPPORT_V1_FIXTURE_PATH));
    EXPECT_EQ(extension_loader.TypeSupports().size(), runtime_loader.GetTypeSupportArray().size());
    EXPECT_EQ(extension_loader.SchemaSupports().size(), runtime_loader.GetSchemaSupportArray().size());
    extension_loader.Unload();
  }
  for (const char* path : {TYPE_SUPPORT_BAD_V1_FIXTURE_PATH, TYPE_SUPPORT_BAD_SIZE_V1_FIXTURE_PATH,
                           TYPE_SUPPORT_BAD_PACKAGE_SIZE_V1_FIXTURE_PATH, TYPE_SUPPORT_OLD_PACKAGE_FIXTURE_PATH,
                           TYPE_SUPPORT_NULL_TYPE_V1_FIXTURE_PATH}) {
    std::string runtime_error;
    std::string extension_error;
    try {
      TypeSupportPkgLoader loader;
      loader.TryLoadTypeSupportPkg(path);
    } catch (const ibmw::common::util::IbmwException& e) {
      runtime_error = e.what();
    }
    try {
      ibmw::extension::TypeSupportPackageLoader loader;
      loader.TryLoad(path);
    } catch (const ibmw::common::util::IbmwException& e) {
      extension_error = e.what();
    }
    ASSERT_FALSE(runtime_error.empty()) << path;
    // what() carries the source location, which is the shared header for both.
    EXPECT_EQ(extension_error, runtime_error) << path;
  }
}

}  // namespace
}  // namespace ibmw::runtime::core::util
