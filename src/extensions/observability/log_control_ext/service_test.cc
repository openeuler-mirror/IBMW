// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// EXT-015: an unknown log level name is rejected instead of silently turning
// the module's logging off.

#include <gtest/gtest.h>

#include "engine/logger/log_level_tool.h"
#include "log_control_ext/service.h"

namespace ibmw::extensions::log_control_extension {
namespace {

using ibmw::runtime::core::logger::LoggingSubsystem;
using ibmw::runtime::core::logger::LogLevelTool;
using ibmw::protocols::log_control_extension::SetModuleLogLevelReq;
using ibmw::protocols::log_control_extension::SetModuleLogLevelRsp;

class TestService : public LogControlServiceImpl {
 public:
  // Runs SetModuleLogLevel through its registered service function.
  SetModuleLogLevelRsp Set(const SetModuleLogLevelReq& req) {
    SetModuleLogLevelRsp rsp;
    for (auto& wrapper : service_func_wrapper_vec_) {
      if (wrapper.func_name != "SetModuleLogLevel") continue;
      ibmw::rpc::ServiceCallback callback([](uint32_t) {});
      wrapper.service_func(nullptr, &req, &rsp, callback.NativeHandle());
    }
    return rsp;
  }
};

TEST(LogLevelToolTest, UnknownNameIsDetected) {
  EXPECT_EQ(LogLevelTool::TryGetLogLevelFromName("warn"), IBMW_LOG_LEVEL_WARN);
  EXPECT_EQ(LogLevelTool::TryGetLogLevelFromName("OFF"), IBMW_LOG_LEVEL_OFF);
  EXPECT_FALSE(LogLevelTool::TryGetLogLevelFromName("warnning"));
  EXPECT_FALSE(LogLevelTool::TryGetLogLevelFromName(""));
  // Unchanged for configuration parsing.
  EXPECT_EQ(LogLevelTool::GetLogLevelFromName("warnning"), IBMW_LOG_LEVEL_OFF);
}

TEST(LogControlServiceTest, UnknownLevelIsRejectedAndNothingChanges) {
  LoggingSubsystem logging;
  logging.Initialize(YAML::Node());
  logging.GetLoggerAccess("core");
  logging.Start();
  ASSERT_EQ(logging.GetAllLoggerLevels().at("core"), IBMW_LOG_LEVEL_INFO);

  TestService service;
  service.SetLoggingSubsystem(&logging);

  auto rsp = service.Set(SetModuleLogLevelReq{.module_log_level_map = {{"core", "warnning"}}});
  EXPECT_NE(rsp.code, 0u);
  EXPECT_EQ(rsp.msg, "INVALID_LOG_LEVEL");
  EXPECT_EQ(logging.GetAllLoggerLevels().at("core"), IBMW_LOG_LEVEL_INFO);

  rsp = service.Set(SetModuleLogLevelReq{.module_log_level_map = {{"core", "debug"}}});
  EXPECT_EQ(rsp.code, 0u);
  EXPECT_EQ(logging.GetAllLoggerLevels().at("core"), IBMW_LOG_LEVEL_DEBUG);

  logging.Shutdown();
}

}  // namespace
}  // namespace ibmw::extensions::log_control_extension
