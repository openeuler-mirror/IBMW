// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/dynamic_lib.h"

#include <gtest/gtest.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace ibmw::common::util {
namespace {

// CORE-018: reading the error twice (or with none pending) must not build a
// std::string from NULL.
TEST(DynamicLib, GetErrWithoutPendingError) {
  DynamicLib lib;
  EXPECT_FALSE(lib.Load("/path/that/does/not/exist/libibmw_missing.so"));
  EXPECT_NE(DynamicLib::GetErr().find("libibmw_missing.so"), std::string::npos);
  std::string second;
  EXPECT_NO_THROW(second = DynamicLib::GetErr());
  EXPECT_FALSE(second.empty());
}

// CORE-017: libraries given by path are refused when other users could have
// replaced them.
class DynamicLibTrustTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string pattern = (std::filesystem::temp_directory_path() / "ibmw_dynlib_XXXXXX").string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    dir_ = pattern;
    lib_ = dir_ / "libfixture.so";
    std::filesystem::copy_file(PKG_FIXTURE_PATH, lib_);
    std::filesystem::permissions(lib_, std::filesystem::perms(0644));
  }
  void TearDown() override {
    std::filesystem::permissions(dir_, std::filesystem::perms(0700));
    std::filesystem::remove_all(dir_);
  }
  static void Chmod(const std::filesystem::path& p, unsigned mode) { ASSERT_EQ(::chmod(p.c_str(), mode), 0); }

  std::filesystem::path dir_;
  std::filesystem::path lib_;
};

TEST_F(DynamicLibTrustTest, LoadsPrivateAndGroupWritableLibraries) {
  DynamicLib lib;
  EXPECT_TRUE(lib.Load(lib_.string())) << DynamicLib::GetErr();
  Chmod(lib_, 0664);
  Chmod(dir_, 0775);  // umask 002 layouts keep working
  EXPECT_TRUE(lib.Load(lib_.string())) << DynamicLib::GetErr();
  Chmod(dir_, 01777);  // sticky, like /tmp
  EXPECT_TRUE(lib.Load(lib_.string())) << DynamicLib::GetErr();
}

TEST_F(DynamicLibTrustTest, RefusesLibraryWritableByOthers) {
  Chmod(lib_, 0646);
  DynamicLib lib;
  EXPECT_FALSE(lib.Load(lib_.string()));
  EXPECT_FALSE(lib.IsLoaded());
  const auto error = DynamicLib::GetErr();
  EXPECT_NE(error.find("Refusing to load"), std::string::npos) << error;
  EXPECT_EQ(error.find("directory"), std::string::npos) << error;
}

TEST_F(DynamicLibTrustTest, RefusesDirectoryWritableByOthers) {
  Chmod(dir_, 0757);
  DynamicLib lib;
  EXPECT_FALSE(lib.Load(lib_.string()));
  const auto error = DynamicLib::GetErr();
  EXPECT_NE(error.find("directory '" + std::filesystem::canonical(dir_).string() + "'"), std::string::npos)
      << error;
}

TEST_F(DynamicLibTrustTest, ChecksTheSymlinkTarget) {
  Chmod(lib_, 0646);
  const auto link = std::filesystem::temp_directory_path() / (dir_.filename().string() + "_link.so");
  std::filesystem::create_symlink(lib_, link);
  DynamicLib lib;
  EXPECT_FALSE(lib.Load(link.string()));
  EXPECT_NE(DynamicLib::GetErr().find(std::filesystem::canonical(lib_).string()), std::string::npos);
  std::filesystem::remove(link);
}

// The checked path is the one dlopen gets, so re-pointing a symlink after the
// check does not change what is loaded.
TEST_F(DynamicLibTrustTest, LoadsTheCheckedPath) {
  const auto link = std::filesystem::temp_directory_path() / (dir_.filename().string() + "_link.so");
  std::filesystem::create_symlink(lib_, link);
  std::string dlopen_name;
  EXPECT_EQ(CheckLibraryTrust(link.string(), dlopen_name), "");
  EXPECT_EQ(dlopen_name, std::filesystem::canonical(lib_).string());
  std::filesystem::remove(link);
}

TEST_F(DynamicLibTrustTest, ChecksRelativePaths) {
  const auto cwd = std::filesystem::current_path();
  std::filesystem::current_path(dir_);
  DynamicLib lib;
  EXPECT_TRUE(lib.Load("./libfixture.so")) << DynamicLib::GetErr();
  Chmod(lib_, 0646);
  EXPECT_FALSE(lib.Load("./libfixture.so"));
  std::filesystem::current_path(cwd);
  EXPECT_NE(DynamicLib::GetErr().find("Refusing to load"), std::string::npos);
}

// A refusal neither leaks into a later GetErr() nor leaves an older dlerror()
// to be reported after the next successful load.
TEST_F(DynamicLibTrustTest, ErrorsDoNotOutliveTheirLoad) {
  DynamicLib lib;
  EXPECT_FALSE(lib.Load("/path/that/does/not/exist/libibmw_missing.so"));
  Chmod(lib_, 0646);
  EXPECT_FALSE(lib.Load(lib_.string()));
  EXPECT_NE(DynamicLib::GetErr().find("Refusing to load"), std::string::npos);
  EXPECT_EQ(DynamicLib::GetErr(), "no dynamic library error pending");
  Chmod(lib_, 0644);
  EXPECT_TRUE(lib.Load(lib_.string()));
  EXPECT_EQ(DynamicLib::GetErr(), "no dynamic library error pending");
}

TEST_F(DynamicLibTrustTest, RefusesLibraryOwnedByAnotherUser) {
  if (::geteuid() != 0) GTEST_SKIP() << "needs root to chown the fixture";
  ASSERT_EQ(::chown(lib_.c_str(), 65534, 65534), 0);
  DynamicLib lib;
  EXPECT_FALSE(lib.Load(lib_.string()));
  EXPECT_NE(DynamicLib::GetErr().find("Refusing to load"), std::string::npos);
}

TEST(DynamicLib, BareNamesUseTheLoaderSearchPath) {
  DynamicLib lib;
#if defined(__GLIBC__)
  EXPECT_TRUE(lib.Load("libc.so.6")) << DynamicLib::GetErr();
#else
  EXPECT_TRUE(lib.Load("libc.so")) << DynamicLib::GetErr();  // musl (OHOS)
#endif
}

}  // namespace
}  // namespace ibmw::common::util
