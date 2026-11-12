// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include <stdexcept>
#include <string>
#include <vector>

#include "cli/module/module_command.h"
#include "engine/runtime_graph/runtime_registry.h"
#include "support/command_capture.h"

namespace ibmw::diag {
struct RegistryFake {
  std::vector<extension::RuntimeRegistration> runtimes;
  unsigned queries = 0;
  bool fail = false;
  std::filesystem::path path;
};
RegistryFake registry;
}  // namespace ibmw::diag

// This executable does not link the real registry implementation. Only input
// collection is replaced; command parsing, selection and rendering are real.
namespace ibmw::runtime::core::runtime_graph {
std::vector<RuntimeRegistration> RuntimeRegistry::DiscoverLocalRuntimes() const {
  auto& fake = diag::registry;
  ++fake.queries;
  fake.path = RegistryPath();
  if (fake.fail) throw std::runtime_error("registry query failed");
  return fake.runtimes;
}
}  // namespace ibmw::runtime::core::runtime_graph

namespace ibmw::diag {
namespace {
class DiagModule : public testing::Test {
 protected:
  void SetUp() override { registry = {}; }
  void TearDown() override { registry = {}; }
  CommandResult Run(std::vector<std::string> args) {
    args.insert(args.begin(), "module");
    args.insert(args.end(), {"--registry-path", "unit-registry"});
    return Capture(RunModuleCommand, args);
  }
  void AddRuntime(std::string id, std::vector<std::string> modules) {
    extension::RuntimeRegistration item;
    item.runtime_id = std::move(id);
    item.pid = 42;
    item.modules = std::move(modules);
    registry.runtimes.push_back(std::move(item));
  }
};

TEST_F(DiagModule, ListKeepsRuntimeIdentityAndCountsModuleInstances) {
  AddRuntime("first", {"camera", "motor"});
  AddRuntime("second", {"camera"});
  auto result = Run({"list"});
  EXPECT_EQ(result.code, 0);
  EXPECT_EQ(result.output, "first/camera [pid: 42]\nfirst/motor [pid: 42]\nsecond/camera [pid: 42]\n");
  EXPECT_EQ(registry.path, extension::RuntimeRegistryDirectory("unit-registry"));
  EXPECT_EQ(Run({"list", "-c"}).output, "3\n");
  EXPECT_EQ(Run({"list", "--runtime", "second", "-c"}).output, "1\n");
}

TEST_F(DiagModule, MissingRuntimeDiffersFromAnEmptyRegistry) {
  EXPECT_EQ(Run({"list", "-c"}).output, "0\n");
  const auto result = Run({"list", "--runtime", "missing"});
  EXPECT_NE(result.code, 0);
  EXPECT_TRUE(result.output.empty());
  EXPECT_NE(result.error.find("was not found"), std::string::npos);
}

TEST_F(DiagModule, OlderRuntimeCannotBeReportedAsCompleteCoverage) {
  AddRuntime("known", {"camera"});
  extension::RuntimeRegistration old;
  old.runtime_id = "old";  // No modules field: an older protocol, not zero modules.
  registry.runtimes.push_back(old);
  const auto result = Run({"list", "-c"});
  EXPECT_EQ(result.code, 1);
  EXPECT_EQ(result.output, "1\n");
  EXPECT_NE(result.error.find("does not advertise"), std::string::npos);
  EXPECT_EQ(Run({"list", "--runtime", "known"}).code, 0);
}

TEST_F(DiagModule, InfoRequiresDisambiguationBeforePrintingAnyDetails) {
  AddRuntime("first", {"camera"});
  AddRuntime("second", {"camera"});
  const auto ambiguous = Run({"info", "camera"});
  EXPECT_NE(ambiguous.code, 0);
  EXPECT_TRUE(ambiguous.output.empty());
  EXPECT_NE(ambiguous.error.find("--runtime"), std::string::npos);
  const auto selected = Run({"info", "camera", "--runtime", "second"});
  EXPECT_EQ(selected.code, 0);
  EXPECT_NE(selected.output.find("Runtime: second\n"), std::string::npos);
}

TEST_F(DiagModule, InfoDeduplicatesInterfacesAndSeparatesTypesAndDirections) {
  AddRuntime("runtime", {"camera", "other"});
  extension::TopicAdvertisement endpoint;
  endpoint.module_name = "camera";
  endpoint.published = true;
  endpoint.topic.topic = "/frames";
  endpoint.topic.msg_type = "Image";
  auto& endpoints = registry.runtimes.front().topics;
  endpoints = {endpoint, endpoint};
  endpoint.topic.msg_type = "Compressed";
  endpoints.push_back(endpoint);
  endpoint.published = false;
  endpoint.topic.topic = "/control";
  endpoint.topic.msg_type = "Control";
  endpoints.push_back(endpoint);
  endpoint.module_name = "other";
  endpoint.topic.topic = "/unrelated";
  endpoints.push_back(endpoint);
  const auto result = Run({"info", "camera"});
  EXPECT_EQ(result.code, 0);
  EXPECT_EQ(result.output,
            "Module: camera\nRuntime: runtime\nPID: 42\n\n"
            "Publishers:\n  /frames [Compressed]\n  /frames [Image]\n\n"
            "Subscribers:\n  /control [Control]\n");
}

TEST_F(DiagModule, RegistryFailureIsAnErrorAndValidationDoesNotQuery) {
  registry.fail = true;
  const auto failure = Run({"list"});
  EXPECT_NE(failure.code, 0);
  EXPECT_TRUE(failure.output.empty());
  EXPECT_NE(failure.error.find("registry query failed"), std::string::npos);
  registry.queries = 0;
  for (const auto& args : std::vector<std::vector<std::string>>{
           {"info"}, {"list", "extra"}, {"list", "--runtime", ""}, {"list", "--unknown"}}) {
    EXPECT_NE(Run(args).code, 0);
  }
  EXPECT_EQ(registry.queries, 0u);
}
}  // namespace
}  // namespace ibmw::diag
