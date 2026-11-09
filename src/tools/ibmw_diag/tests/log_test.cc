// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "cli/log/log_client.h"
#include "cli/log/log_command.h"
#include "support/command_capture.h"
#include "support/rpc_fake.h"

namespace ibmw::diag {
namespace {
class DiagLog : public RpcTest {
 protected:
  cli::Invocation Connection() {
    cli::Invocation in;
    in.options = {{"--http", {"localhost:8090"}}, {"--service", {"robot/logs"}}};
    in.positional_args = {"ignored", "arguments"};
    return in;
  }
  CommandResult Run(std::vector<std::string> args) {
    args.insert(args.begin(), "log");
    args.insert(args.begin() + 2, {"--http", "localhost:8090"});
    return Capture(RunLogCommand, args);
  }
};

TEST_F(DiagLog, QueryNormalizesLevelsAndUsesExplicitModuleScope) {
  Reply({{"code", 0}, {"msg", ""}, {"module_log_level_map", {{"camera", "DeBuG"}, {"motor", "OFF"}}}});
  const auto levels = cli::log::QueryLogLevels(Connection(), {"camera", "motor"}, "diag probe");
  EXPECT_EQ(levels, (std::map<std::string, std::string>{{"camera", "DEBUG"}, {"motor", "OFF"}}));
  ASSERT_EQ(Rpc().requests.size(), 1u);
  EXPECT_EQ(Rpc().requests[0].function, "json:/robot/logs/GetModuleLogLevel");
  EXPECT_EQ(Rpc().requests[0].payload["module_names"], (nlohmann::json::array({"camera", "motor"})));
  EXPECT_EQ(Rpc().requests[0].diagnostic, "diag probe");
}

TEST_F(DiagLog, EmptySuccessfulMapIsDifferentFromMissingData) {
  Reply({{"code", 0}, {"msg", ""}, {"module_log_level_map", nlohmann::json::object()}});
  EXPECT_TRUE(cli::log::QueryLogLevels(Connection(), {}).empty());
  EXPECT_TRUE(Rpc().requests.front().payload["module_names"].empty());
  Reply({{"code", 0}, {"msg", ""}});
  EXPECT_THROW(cli::log::QueryLogLevels(Connection(), {}), std::exception);
}

TEST_F(DiagLog, AllLevelsAreNormalizedBeforeSendingExactlyOneSetterRequest) {
  for (const auto* level : {"trace", "debug", "info", "warn", "error", "fatal", "off"}) {
    Rpc().requests.clear();
    Reply({{"code", 0}, {"msg", ""}});
    EXPECT_NO_THROW(cli::log::SetLogLevel(Connection(), "camera", level));
    ASSERT_EQ(Rpc().requests.size(), 1u);
    std::string upper(level);
    for (auto& character : upper) character -= 'a' - 'A';
    EXPECT_EQ(Rpc().requests[0].payload["module_log_level_map"]["camera"], upper);
    EXPECT_EQ(Rpc().requests[0].function, "json:/robot/logs/SetModuleLogLevel");
  }
}

TEST_F(DiagLog, InvalidLevelNamesAndEmptyModulesNeverIssueRpc) {
  for (const auto* level : {"", "WARNING", " INFO", "INFO ", "7"}) {
    EXPECT_THROW(cli::log::SetLogLevel(Connection(), "camera", level), std::invalid_argument);
  }
  EXPECT_THROW(cli::log::SetLogLevel(Connection(), "", "INFO"), std::invalid_argument);
  EXPECT_THROW(cli::log::QueryLogLevels(Connection(), {""}), std::invalid_argument);
  EXPECT_TRUE(Rpc().requests.empty());
}

TEST_F(DiagLog, BusinessFailureDoesNotConsumeSuccessLookingData) {
  Reply({{"code", 1}, {"msg", "INVALID_MODULE_NAME"}, {"module_log_level_map", {{"camera", "INFO"}}}});
  const auto result = Run({"list", "camera"});
  EXPECT_NE(result.code, 0);
  EXPECT_TRUE(result.output.empty());
  EXPECT_NE(result.error.find("INVALID_MODULE_NAME"), std::string::npos);
}

TEST_F(DiagLog, MalformedOrInvalidResponseDoesNotPrintPartialResults) {
  for (const auto& response : std::vector<nlohmann::json>{
           {{"code", 0}, {"msg", ""}, {"module_log_level_map", {{"a", "INFO"}, {"z", "UNKNOWN"}}}},
           {{"msg", ""}, {"module_log_level_map", nlohmann::json::object()}},
           {{"code", 0}, {"module_log_level_map", nlohmann::json::object()}}}) {
    Reply(response);
    const auto result = Run({"list"});
    EXPECT_NE(result.code, 0);
    EXPECT_TRUE(result.output.empty());
  }
}

TEST_F(DiagLog, ListSortsOutputAndSetUsesTheSameClient) {
  Reply({{"code", 0}, {"msg", ""}, {"module_log_level_map", {{"z", "Warn"}, {"a", "info"}}}});
  const auto list = Run({"list"});
  EXPECT_EQ(list.code, 0);
  EXPECT_EQ(list.output, "a INFO\nz WARN\n");
  Reply({{"code", 0}, {"msg", ""}});
  const auto set = Run({"set", "a", "error"});
  EXPECT_EQ(set.code, 0);
  EXPECT_EQ(set.output, "Set log level successful\n");
  ASSERT_EQ(Rpc().requests.size(), 2u);
  EXPECT_EQ(Rpc().requests[1].payload["module_log_level_map"]["a"], "ERROR");
}

TEST_F(DiagLog, FailedTransportRetainsExitCodeAndNeverRetries) {
  Reply(nullptr, 143);
  const auto result = Run({"list"});
  EXPECT_EQ(result.code, 143);
  EXPECT_TRUE(result.output.empty());
  EXPECT_EQ(Rpc().requests.size(), 1u);
}
}  // namespace
}  // namespace ibmw::diag
