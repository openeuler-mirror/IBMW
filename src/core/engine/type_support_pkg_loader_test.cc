// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "base/exception.h"
#include "engine/type_support_pkg_loader.h"

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

}  // namespace
}  // namespace ibmw::runtime::core::util
