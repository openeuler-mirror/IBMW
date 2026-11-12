// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "cli/param/param_command.h"
#include "support/command_capture.h"
#include "support/rpc_fake.h"

namespace ibmw::diag {
namespace {
class DiagParam : public RpcTest {
 protected:
  CommandResult Run(std::vector<std::string> args) {
    args.insert(args.begin(), "param");
    // All transport interaction terminates at the fake ExecuteCall boundary.
    args.insert(args.begin() + 2, {"--http", "localhost:8090"});
    return Capture(RunParamCommand, args);
  }
};

TEST_F(DiagParam, GetPreservesLiteralValueAndConstructsTheSelectedServiceRequest) {
  Reply({{"code", 0}, {"msg", ""}, {"parameter_value", "001\nfalse"}});
  const auto result = Run({"get", "camera", "label", "--hide-type", "--service", "robot/parameters"});
  EXPECT_EQ(result.code, 0) << result.error;
  EXPECT_EQ(result.output, "001\nfalse\n");
  ASSERT_EQ(Rpc().requests.size(), 1u);
  EXPECT_EQ(Rpc().requests[0].function, "json:/robot/parameters/Get");
  EXPECT_EQ(Rpc().requests[0].payload, (nlohmann::json{{"module_name", "camera"}, {"parameter_key", "label"}}));
  EXPECT_EQ(Rpc().requests[0].serialization, "json");
}

TEST_F(DiagParam, SetSendsStringsIncludingEmptyAndOptionLikeValuesOnce) {
  for (const auto* value : {"", "false", "[1, 2]", "中文", "--help", "-2.5"}) {
    SCOPED_TRACE(value);
    Rpc().requests.clear();
    Reply({{"code", 0}, {"msg", ""}});
    const auto result = Run({"set", "camera", "label", "--", value});
    EXPECT_EQ(result.code, 0) << result.error;
    EXPECT_EQ(result.output, "Set parameter successful\n");
    ASSERT_EQ(Rpc().requests.size(), 1u);
    EXPECT_TRUE(Rpc().requests[0].payload["parameter_value"].is_string());
    EXPECT_EQ(Rpc().requests[0].payload["parameter_value"], value);
  }
}

TEST_F(DiagParam, PrefixFilterHonorsDottedNamespaceRatherThanStringPrefix) {
  Reply({{"code", 0}, {"msg", ""}, {"parameter_keys", {"controller2.gain", "controller.gain", "controller", "speed"}}});
  const auto result = Run({"list", "camera", "--param-prefixes", "controller", "--param-type"});
  EXPECT_EQ(result.code, 0) << result.error;
  EXPECT_EQ(result.output, "controller (string)\ncontroller.gain (string)\n");
  EXPECT_EQ(Rpc().requests.size(), 1u);
}

TEST_F(DiagParam, RepeatedPrefixesCombineWithRegexAndSortResults) {
  Reply({{"code", 0}, {"msg", ""}, {"parameter_keys", {"motor.z", "camera.b", "motor.a", "other.a"}}});
  const auto result = Run({"list", "device", "--param-prefixes", "motor", "--param-prefixes", "camera", "--filter", ".*\\.a$"});
  EXPECT_EQ(result.code, 0) << result.error;
  EXPECT_EQ(result.output, "motor.a\n");
}

TEST_F(DiagParam, AllModulesAreSortedAndEmptyGroupsAreOmitted) {
  Reply({{"code", 0}, {"msg", ""}, {"module_names", {"z", "empty", "a"}}});
  Reply({{"code", 0}, {"msg", ""}, {"parameter_keys", {"b", "a"}}});
  Reply({{"code", 0}, {"msg", ""}, {"parameter_keys", nlohmann::json::array()}});
  Reply({{"code", 0}, {"msg", ""}, {"parameter_keys", {"last"}}});
  const auto result = Run({"list"});
  EXPECT_EQ(result.code, 0) << result.error;
  EXPECT_EQ(result.output, "a:\n  a\n  b\nz:\n  last\n");
  ASSERT_EQ(Rpc().requests.size(), 4u);
  EXPECT_EQ(Rpc().requests[1].payload["module_name"], "a");
  EXPECT_EQ(Rpc().requests[2].payload["module_name"], "empty");
  EXPECT_EQ(Rpc().requests[3].payload["module_name"], "z");
}

TEST_F(DiagParam, LaterModuleFailureDoesNotPrintAPartialList) {
  Reply({{"code", 0}, {"msg", ""}, {"module_names", {"a", "b"}}});
  Reply({{"code", 0}, {"msg", ""}, {"parameter_keys", {"first"}}});
  Reply({{"code", 1}, {"msg", "INVALID_MODULE_NAME"}});
  const auto result = Run({"list"});
  EXPECT_NE(result.code, 0);
  EXPECT_TRUE(result.output.empty());
  EXPECT_NE(result.error.find("INVALID_MODULE_NAME"), std::string::npos);
  EXPECT_EQ(Rpc().requests.size(), 3u);
}

TEST_F(DiagParam, MalformedStatusAndMissingParametersCannotLookSuccessful) {
  for (const auto& response : std::vector<nlohmann::json>{
           {{"msg", ""}, {"parameter_value", "wrong"}},
           {{"code", 0}, {"parameter_value", "wrong"}},
           {{"code", 2}, {"msg", "PARAMETER_NOT_FOUND"}}}) {
    Reply(response);
    const auto result = Run({"get", "camera", "missing"});
    EXPECT_NE(result.code, 0);
    EXPECT_TRUE(result.output.empty());
  }
}

TEST_F(DiagParam, TransportFailureAndIndependentDeadlinesArePreservedWithoutRetry) {
  Reply(nullptr, 130);
  const auto result = Run({"get", "camera", "speed", "--timeout", "0.2", "--discovery-timeout", "0.7"});
  EXPECT_EQ(result.code, 130);
  EXPECT_TRUE(result.output.empty());
  ASSERT_EQ(Rpc().requests.size(), 1u);
  EXPECT_EQ(Rpc().requests[0].timeout.count(), 200);
  EXPECT_EQ(Rpc().requests[0].discovery_timeout.count(), 700);
}

TEST_F(DiagParam, InvalidRegexAndMissingArgumentsNeverIssueRpc) {
  for (const auto& args : std::vector<std::vector<std::string>>{
           {"list", "camera", "--filter", "["}, {"get", "camera"},
           {"set", "", "key", "value"}, {"get", "camera", "key", "--timeout", "nan"}}) {
    EXPECT_NE(Run(args).code, 0);
  }
  EXPECT_TRUE(Rpc().requests.empty());
}
}  // namespace
}  // namespace ibmw::diag
