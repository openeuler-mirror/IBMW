// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "engine/runtime_graph/runtime_directory.h"
#include "engine/runtime_graph/runtime_graph_agent.h"

namespace ibmw::runtime::core::runtime_graph {
namespace {

class RuntimeDirectoryTest : public testing::Test {
 protected:
  void SetUp() override {
    std::string path =
        (std::filesystem::temp_directory_path() / "ibmw-registry-security-XXXXXX").string();
    ASSERT_NE(::mkdtemp(path.data()), nullptr);
    root_ = path;
  }
  void TearDown() override { std::filesystem::remove_all(root_); }
  std::filesystem::path root_;
};

TEST_F(RuntimeDirectoryTest, RejectsSymlinkComponentsWithoutCreatingTheirChildren) {
  std::filesystem::create_directory(root_ / "target");
  std::filesystem::create_directory_symlink(root_ / "target", root_ / "link");
  EXPECT_THROW(OpenRuntimeDirectory(root_ / "link" / "child", true),
               ibmw::common::util::IbmwException);
  EXPECT_FALSE(std::filesystem::exists(root_ / "target" / "child"));
}

TEST_F(RuntimeDirectoryTest, RejectsSharedDirectoriesWithoutChangingPermissions) {
  ASSERT_EQ(::chmod(root_.c_str(), 0755), 0);
  EXPECT_THROW(OpenRuntimeDirectory(root_, true), ibmw::common::util::IbmwException);
  struct stat status {};
  ASSERT_EQ(::stat(root_.c_str(), &status), 0);
  EXPECT_EQ(status.st_mode & 0777, 0755);
  ASSERT_EQ(::chmod(root_.c_str(), 0770), 0);
  EXPECT_THROW(OpenRuntimeDirectory(root_ / "private", true),
               ibmw::common::util::IbmwException);
  EXPECT_FALSE(std::filesystem::exists(root_ / "private"));
}

TEST_F(RuntimeDirectoryTest, RegistrationNeverTruncatesSymlinkTargets) {
  const auto victim = root_ / "victim";
  std::ofstream(victim) << "keep";
  const auto directory = root_ / "v2";
  const auto fd = OpenRuntimeDirectory(directory, true);
  const auto registration_path = directory / "runtime.json";
  std::filesystem::create_symlink(victim, registration_path);
  std::filesystem::create_symlink(victim, directory / "runtime.json.tmp");
  for (unsigned i = 0; i < 64; ++i) {
    std::filesystem::create_symlink(victim, directory /
        ("runtime.json." + std::to_string(::getpid()) + "." + std::to_string(i) + ".tmp"));
  }
  const RuntimeRegistration registration{
      .runtime_id = "runtime", .pid = ::getpid(),
      .process_start_time_ticks = *ReadProcessStartTimeTicks(::getpid()),
      .control_socket = (directory / "runtime.sock").string()};
  try {
    WriteRuntimeRegistration(registration, registration_path);
    EXPECT_FALSE(std::filesystem::is_symlink(registration_path));
  } catch (const ibmw::common::util::IbmwException&) {
    // Exhausting the bounded set of temporary names must fail closed.
    EXPECT_TRUE(std::filesystem::is_symlink(registration_path));
  }
  std::string content;
  std::ifstream(victim) >> content;
  EXPECT_EQ(content, "keep");
}

TEST_F(RuntimeDirectoryTest, AgentRejectsSymlinkLock) {
  const auto directory = root_ / "v2";
  const auto fd = OpenRuntimeDirectory(directory, true);
  const auto victim = root_ / "victim";
  std::ofstream(victim) << "keep";
  std::filesystem::create_symlink(victim, directory / "runtime.lock");
  RuntimeGraphAgent agent({.registration = {.runtime_id = "runtime"}, .registry_path = root_});
  EXPECT_THROW(agent.Initialize(), ibmw::common::util::IbmwException);
  EXPECT_FALSE(std::filesystem::exists(directory / "runtime.json"));
  std::string content;
  std::ifstream(victim) >> content;
  EXPECT_EQ(content, "keep");
}

// COMM-031: the size limit counts the bytes read from the open file, so a
// file that grows after a size check, or reports st_size 0, can not exceed it.
TEST_F(RuntimeDirectoryTest, ReadRuntimeFileBelowBoundsTheBytesRead) {
  const auto path = root_ / "data.json";
  std::ofstream(path) << std::string(15, 'x');
  {
    const RuntimeFileDescriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    EXPECT_EQ(ReadRuntimeFileBelow(file.Get(), 16), std::string(15, 'x'));
  }
  {
    const RuntimeFileDescriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    EXPECT_EQ(ReadRuntimeFileBelow(file.Get(), 15), std::nullopt);
  }
  const RuntimeFileDescriptor proc(::open("/proc/self/status", O_RDONLY | O_CLOEXEC));
  struct stat status {};
  ASSERT_EQ(::fstat(proc.Get(), &status), 0);
  ASSERT_EQ(status.st_size, 0);
  EXPECT_EQ(ReadRuntimeFileBelow(proc.Get(), 16), std::nullopt);
}

}  // namespace
}  // namespace ibmw::runtime::core::runtime_graph
