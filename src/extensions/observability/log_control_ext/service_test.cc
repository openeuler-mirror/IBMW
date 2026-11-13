// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// EXT-015: an unknown log level name is rejected instead of silently turning
// the module's logging off.

#include <gtest/gtest.h>

#include <stdexcept>

#include "engine/logger/log_level_tool.h"
#include "log_control_ext/service.h"

namespace ibmw::extensions::log_control_extension {
namespace {

using ibmw::runtime::core::logger::LoggingSubsystem;
using ibmw::runtime::core::logger::LogLevelTool;
using ibmw::protocols::log_control_extension::GetModuleLogLevelReq;
using ibmw::protocols::log_control_extension::GetModuleLogLevelRsp;
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

  GetModuleLogLevelRsp Get(const GetModuleLogLevelReq& req) {
    GetModuleLogLevelRsp rsp;
    for (auto& wrapper : service_func_wrapper_vec_) {
      if (wrapper.func_name != "GetModuleLogLevel") continue;
      ibmw::rpc::ServiceCallback callback([](uint32_t) {});
      wrapper.service_func(nullptr, &req, &rsp, callback.NativeHandle());
    }
    return rsp;
  }

  // Fills part of the answer, then fails; run through Invoke like a
  // registered handler.
  void FillThenThrow(const GetModuleLogLevelReq&, GetModuleLogLevelRsp& rsp) {
    rsp.module_log_level_map["core"] = "Info";
    throw std::runtime_error("failed part way");
  }
  uint32_t CallThrowing(GetModuleLogLevelRsp& rsp) {
    const GetModuleLogLevelReq req;
    return Invoke(static_cast<void (LogControlServiceImpl::*)(const GetModuleLogLevelReq&,
                                                              GetModuleLogLevelRsp&)>(
                      &TestService::FillThenThrow),
                  &req, &rsp);
  }
};

// EXT-014: a call that arrives without a logging subsystem gets an answer.
TEST(LogControlServiceRegressionTest, MissingLoggingSubsystemIsReported) {
  TestService service;
  auto set = service.Set(SetModuleLogLevelReq{.module_log_level_map = {{"core", "debug"}}});
  EXPECT_EQ(set.msg, "NOT_READY");
  EXPECT_NE(set.code, 0u);
  EXPECT_EQ(service.Get(GetModuleLogLevelReq{}).msg, "NOT_READY");
}

// A handler exception is answered with INTERNAL_ERROR and no partial result.
TEST(LogControlServiceRegressionTest, ExceptionDiscardsPartialAnswer) {
  TestService service;
  GetModuleLogLevelRsp rsp;
  EXPECT_EQ(service.CallThrowing(rsp), static_cast<uint32_t>(IBMW_RPC_STATUS_SVR_HANDLE_FAILED));
  EXPECT_EQ(rsp.msg, "INTERNAL_ERROR");
  EXPECT_TRUE(rsp.module_log_level_map.empty());
}

TEST(LogLevelToolTest, UnknownNameIsDetected) {
  EXPECT_EQ(LogLevelTool::TryGetLogLevelFromName("warn"), IBMW_LOG_LEVEL_WARN);
  EXPECT_EQ(LogLevelTool::TryGetLogLevelFromName("OFF"), IBMW_LOG_LEVEL_OFF);
  EXPECT_FALSE(LogLevelTool::TryGetLogLevelFromName("warnning"));
  EXPECT_FALSE(LogLevelTool::TryGetLogLevelFromName(""));
  // Unchanged for configuration parsing.
  EXPECT_EQ(LogLevelTool::GetLogLevelFromName("warnning"), IBMW_LOG_LEVEL_OFF);
}

TEST(LogControlServiceRegressionTest, UnknownLevelIsRejectedAndNothingChanges) {
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
