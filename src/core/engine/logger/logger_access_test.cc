// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// CORE-038: a level from the C ABI that is not a message level never reaches
// the drivers, which index per-level tables with it.

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "engine/logger/logger_access.h"

namespace ibmw::runtime::core::logger {
namespace {

class RecordingDriver final : public LoggerDriverBase {
 public:
  explicit RecordingDriver(std::vector<ibmw_log_level_t>* levels) : levels_(levels) {}
  std::string_view Type() const noexcept override { return "recording"; }
  void Initialize(YAML::Node) override {}
  void Start() override {}
  void Shutdown() override {}
  bool AllowDuplicates() const noexcept override { return true; }
  void Log(const LogDataWrapper& data) noexcept override { levels_->push_back(data.lvl); }

 private:
  std::vector<ibmw_log_level_t>* levels_;
};

TEST(LoggerAccessTest, LevelsFromOffUpAreDropped) {
  std::vector<ibmw_log_level_t> levels;
  std::vector<std::unique_ptr<LoggerDriverBase>> drivers;
  drivers.push_back(std::make_unique<RecordingDriver>(&levels));
  LoggerAccess access("module", IBMW_LOG_LEVEL_TRACE, drivers);
  const auto* base = access.NativeHandle();
  for (int lvl : {0, 5, 6, 7, 100, -1}) {
    base->log(base->ctx, static_cast<ibmw_log_level_t>(lvl), 1, "f.cc", "F", "x", 1);
  }
  EXPECT_EQ(levels, (std::vector<ibmw_log_level_t>{IBMW_LOG_LEVEL_TRACE, IBMW_LOG_LEVEL_FATAL}));
}

}  // namespace
}  // namespace ibmw::runtime::core::logger
