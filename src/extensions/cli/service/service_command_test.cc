// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "cpp/module_base.h"
#include "cpp/util/json_rpc_helpers.h"
#include "engine/discovery/service_discovery.h"
#include "engine/engine.h"
#include "engine/runtime_graph/runtime_graph_agent.h"
#include "extension_api/ibmw_cli_provider.h"
#include "gtest/gtest.h"

namespace {

using ibmw::extension::RuntimeRegistration;
using ibmw::extension::ServiceAdvertisement;
using ibmw::runtime::core::runtime_graph::RuntimeGraphAgent;
using ibmw::runtime::core::runtime_graph::RuntimeRegistrationFromJson;
using ibmw::runtime::core::runtime_graph::RuntimeRegistrationToJson;

constexpr char kName[] = "json:/demo.Calculator/Compute";
constexpr char kType[] = "json:demo.Request -> json:demo.Response";
constexpr char kHidden[] = "json:/demo/_hidden/Compute";

struct CommandResult {
  int code;
  std::string output;
  std::string error;
};

CommandResult InvokeService(std::vector<std::string> arguments) {
  arguments.insert(arguments.begin(), "service");
  std::vector<const char*> argv;
  for (const auto& argument : arguments) argv.push_back(argument.c_str());
  testing::internal::CaptureStdout();
  testing::internal::CaptureStderr();
  const int code = IbmwCliEntryV1().commands[0].invoke(static_cast<int>(argv.size()), argv.data());
  auto output = testing::internal::GetCapturedStdout();
  auto error = testing::internal::GetCapturedStderr();
  return {code, std::move(output), std::move(error)};
}

ServiceAdvertisement MakeService(std::string id, std::string name = kName, bool server = true) {
  return {.endpoint_id = std::move(id), .service_name = std::move(name),
          .request_type = "json:demo.Request", .response_type = "json:demo.Response",
          .module_name = "calculator", .package_path = "core",
          .available_drivers = {"local", "dds"}, .server = server};
}

class RegistryServiceCommandTest : public testing::Test {
 protected:
  void SetUp() override {
    std::string pattern = "/tmp/ibmw-service-test-XXXXXX";
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    directory_ = pattern;
  }
  void TearDown() override {
    agents_.clear();
    std::filesystem::remove_all(directory_);
  }
  void Register(std::string id, std::optional<std::vector<ServiceAdvertisement>> services) {
    auto agent = std::make_unique<RuntimeGraphAgent>(RuntimeGraphAgent::Options{
        .registration = {.runtime_id = std::move(id), .services = std::move(services)},
        .registry_path = directory_});
    agent->Initialize();
    agents_.push_back(std::move(agent));
  }
  void RegisterExamples() {
    auto hidden = MakeService("hidden", kHidden);
    hidden.hidden = true;
    Register("runtime-a", std::vector<ServiceAdvertisement>{
        MakeService("server"), MakeService("client", kName, false),
        MakeService("client-only", "json:/demo/Absent", false), hidden});
    Register("runtime-b", std::vector<ServiceAdvertisement>{MakeService("another-server")});
  }
  CommandResult Query(std::vector<std::string> arguments) {
    arguments.insert(arguments.end(), {"--registry-path", directory_.string()});
    return InvokeService(std::move(arguments));
  }

  // Exercise the deployed provider manifest and a separate CLI process.
  CommandResult ExternalQuery(std::vector<std::string> arguments) {
    arguments.insert(arguments.begin(), {IBMW_SERVICE_TEST_CLI, "service"});
    arguments.insert(arguments.end(), {"--registry-path", directory_.string()});
    std::vector<char*> argv;
    for (auto& arg : arguments) argv.push_back(arg.data());
    argv.push_back(nullptr);
    int output[2];
    if (::pipe(output) != 0) throw std::runtime_error("pipe failed");
    const auto pid = ::fork();
    if (pid == 0) {
      ::close(output[0]);
      ::dup2(output[1], STDOUT_FILENO);
      ::dup2(output[1], STDERR_FILENO);
      ::close(output[1]);
      ::execv(argv[0], argv.data());
      ::_exit(127);
    }
    ::close(output[1]);
    if (pid < 0) {
      ::close(output[0]);
      throw std::runtime_error("fork failed");
    }
    std::string text;
    char buffer[1024];
    while (true) {
      const auto size = ::read(output[0], buffer, sizeof(buffer));
      if (size < 0 && errno == EINTR) continue;
      if (size <= 0) break;
      text.append(buffer, static_cast<size_t>(size));
    }
    ::close(output[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
      if (errno != EINTR) throw std::runtime_error("waitpid failed");
    }
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1, std::move(text), {}};
  }

  std::filesystem::path directory_;
  std::vector<std::unique_ptr<RuntimeGraphAgent>> agents_;
};

TEST_F(RegistryServiceCommandTest, ListsServersDeduplicatesAndFiltersHiddenServices) {
  RegisterExamples();
  const auto list = Query({"list"});
  EXPECT_EQ(list.code, 0);
  EXPECT_TRUE(list.error.empty());
  EXPECT_EQ(list.output, std::string(kName) + '\n');
  EXPECT_EQ(Query({"list", "--registry"}).output, list.output);
  EXPECT_EQ(Query({"list", "-t"}).output, std::string(kName) + " [" + kType + "]\n");
  EXPECT_EQ(Query({"list", "-c"}).output, "1\n");
  EXPECT_EQ(Query({"list", "-c", "--include-hidden-services"}).output, "2\n");
  EXPECT_EQ(Query({"--include-hidden-services", "list"}).output,
            std::string(kName) + '\n' + kHidden + '\n');
}

TEST_F(RegistryServiceCommandTest, TypeAndFindUseTheSameExactTypePair) {
  RegisterExamples();
  const auto type = Query({"type", kName});
  EXPECT_EQ(type.code, 0);
  EXPECT_EQ(type.output, std::string(kType) + '\n');
  const auto found = Query({"find", kType});
  EXPECT_EQ(found.code, 0);
  EXPECT_EQ(found.output, std::string(kName) + '\n');
  EXPECT_EQ(Query({"find", kType, "-c"}).output, "1\n");
  EXPECT_EQ(Query({"find", kType, "--include-hidden-services", "-c"}).output, "2\n");
  EXPECT_EQ(Query({"type", kHidden}).code, 1);
  EXPECT_EQ(Query({"--include-hidden-services", "type", kHidden}).output, type.output);
  EXPECT_EQ(Query({"type", "json:/demo/Absent"}).code, 1);
  EXPECT_EQ(Query({"find", "json:demo.Request"}).output, "");
}

TEST_F(RegistryServiceCommandTest, PreservesMultipleTypesForTheSameName) {
  auto other = MakeService("server-b");
  other.response_type = "json:demo.OtherResponse";
  Register("runtime", std::vector<ServiceAdvertisement>{MakeService("server-a"), other});
  EXPECT_EQ(Query({"list", "-c"}).output, "1\n");
  EXPECT_EQ(Query({"type", kName}).output,
            "json:demo.Request -> json:demo.OtherResponse\n" + std::string(kType) + '\n');
  EXPECT_EQ(Query({"find", "json:demo.Request -> json:demo.OtherResponse"}).output,
            std::string(kName) + '\n');
}

TEST_F(RegistryServiceCommandTest, EmptyMissingAndExitedServicesHaveDistinctResults) {
  EXPECT_EQ(Query({"list"}).code, 0);
  EXPECT_EQ(Query({"list", "-c"}).output, "0\n");
  EXPECT_EQ(Query({"find", kType}).code, 0);
  const auto missing = Query({"type", kName});
  EXPECT_EQ(missing.code, 1);
  EXPECT_NE(missing.error.find("Unknown service"), std::string::npos);
  RegisterExamples();
  agents_.clear();
  EXPECT_EQ(Query({"list"}).output, "");
  EXPECT_EQ(Query({"list"}).code, 0);
}

TEST_F(RegistryServiceCommandTest, OldRuntimesReportIncompleteDiscoveryAndKeepKnownResults) {
  RegisterExamples();
  Register("old-runtime", std::nullopt);
  for (const auto& arguments : std::vector<std::vector<std::string>>{
           {"list"}, {"type", kName}, {"find", kType}}) {
    const auto result = Query(arguments);
    EXPECT_EQ(result.code, 1);
    EXPECT_FALSE(result.output.empty());
    EXPECT_NE(result.error.find("old-runtime"), std::string::npos);
    EXPECT_NE(result.error.find("upgrade and restart"), std::string::npos);
  }
  EXPECT_EQ(Query({"list", "-c"}).output, "1\n");
  EXPECT_EQ(Query({"type", "/missing"}).error.find("Unknown service"), std::string::npos);
}

TEST_F(RegistryServiceCommandTest, RegistryFailureIsNotReportedAsAnEmptySuccessfulQuery) {
  std::ofstream(directory_ / "file") << "not a directory";
  const auto result = InvokeService({"type", kName, "--registry-path", (directory_ / "file").string()});
  EXPECT_EQ(result.code, 1);
  EXPECT_TRUE(result.output.empty());
  EXPECT_FALSE(result.error.empty());
  EXPECT_EQ(result.error.find("Unknown service"), std::string::npos);
}

TEST(ServiceCommandTest, HelpAndArgumentValidation) {
  for (const std::string verb : {"list", "type", "find"}) {
    const auto help = InvokeService({verb, "--help"});
    EXPECT_EQ(help.code, 0);
    EXPECT_NE(help.output.find("--registry"), std::string::npos);
    EXPECT_NE(help.output.find("--include-hidden-services"), std::string::npos);
    for (const std::string source : {"--dds", "--ros2", "--zenoh", "--http", "--grpc", "--mqtt"}) {
      EXPECT_NE(help.output.find(source), std::string::npos);
    }
    EXPECT_NE(InvokeService({verb, "--unknown"}).code, 0);
  }
  EXPECT_NE(InvokeService({"type"}).code, 0);
  EXPECT_NE(InvokeService({"find"}).code, 0);
  EXPECT_NE(InvokeService({"list", "extra"}).code, 0);
  EXPECT_NE(InvokeService({"list", "--registry-path", ""}).code, 0);
  EXPECT_NE(InvokeService({"list", "--registry-path", "/tmp/a", "--registry-path", "/tmp/b"}).code, 0);
  EXPECT_NE(InvokeService({"list", "--dds", "--dds-domain", "invalid"}).code, 0);
}

TEST_F(RegistryServiceCommandTest, RegistrationRoundTripsAndOldRecordsRemainReadable) {
  RegisterExamples();
  const auto& registration = agents_.front()->Registration();
  auto json = RuntimeRegistrationToJson(registration);
  EXPECT_EQ(RuntimeRegistrationFromJson(json).services, registration.services);
  json.erase("services");
  EXPECT_FALSE(RuntimeRegistrationFromJson(json).services.has_value());
  json["services"] = nlohmann::json::array();
  ASSERT_TRUE(RuntimeRegistrationFromJson(json).services.has_value());
  EXPECT_TRUE(RuntimeRegistrationFromJson(json).services->empty());
  json["services"] = RuntimeRegistrationToJson(registration).at("services");
  json["services"].push_back(json["services"][0]);
  EXPECT_THROW(RuntimeRegistrationFromJson(json), std::exception);
  json["services"] = nlohmann::json::object();
  EXPECT_THROW(RuntimeRegistrationFromJson(json), std::exception);
}

struct Request {
  static constexpr const char* kTypeName = "demo.Request";
  int value = 0;
  NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Request, value)
};

struct Response {
  static constexpr const char* kTypeName = "demo.Response";
  int result = 0;
  NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Response, result)
};

class CalculatorModule final : public ibmw::Module {
 public:
  CalculatorModule() : service_("json", "demo.Calculator") {
    ibmw::json::rpc::RegisterServiceFunc<Request, Response>(service_, "Compute",
        [](const Request& req, Response& rsp) -> uint32_t { rsp.result = req.value * 2; return 0; });
  }
  ibmw::ModuleDescriptor Info() const override { return {.module_name = "calculator"}; }
  bool Setup(ibmw::Runtime runtime) override {
    return runtime.GetService().RegisterService(&service_);
  }
  bool Run() override { return true; }
  void Stop() override {}

 private:
  ibmw::rpc::ServiceBase service_;
};

TEST_F(RegistryServiceCommandTest, ActualRuntimePublishesServicesAndRefreshesAppModeRegistrations) {
  CalculatorModule module;
  ibmw::rpc::ServiceBase late_service("json", "demo.Late");
  ibmw::json::rpc::RegisterServiceFunc<Request, Response>(late_service, "Compute",
      [](const Request&, Response&) -> uint32_t { return 0; });
  ibmw::runtime::core::Engine engine;
  engine.GetModuleBus().RegisterModule(module.NativeHandle());
  YAML::Node config = YAML::Load(R"(
ibmw:
  logging: {core_level: OFF}
  service:
    drivers: [{type: local}]
    servers: [{func_name: '(.*)', enable_drivers: [local]}]
    clients: [{func_name: '(.*)', enable_drivers: [local]}]
)");
  config["ibmw"]["runtime_graph"]["registry_path"] = directory_.string();
  config["ibmw"]["config"]["temp_cfg_path"] = (directory_ / "resolved").string();
  const auto path = directory_ / "runtime.yaml";
  std::ofstream(path) << config;
  engine.Initialize({.cfg_file_path = path.string()});
  const auto& initial = engine.GetRuntimeGraphSubsystem().Registration();
  EXPECT_TRUE(initial.topics.empty());
  ASSERT_TRUE(initial.services.has_value());
  ASSERT_EQ(initial.services->size(), 1u);
  EXPECT_EQ(initial.services->front().module_name, "calculator");
  EXPECT_EQ(initial.services->front().available_drivers, std::vector<std::string>{"local"});
  EXPECT_EQ(Query({"list"}).output, std::string(kName) + '\n');

  ibmw::rpc::ServiceRef handle(engine.GetServiceDispatcher().GetRpcAccess("late-app").NativeHandle());
  ASSERT_TRUE(handle.RegisterService(&late_service));
  ASSERT_TRUE((ibmw::json::rpc::RegisterClientFunc<Request, Response>(handle, "demo.Absent", "Compute")));
  auto stopped = engine.AsyncStart();
  const auto snapshot = ibmw::discovery::DiscoverRegistryServices(directory_);
  EXPECT_TRUE(snapshot.Succeeded());
  EXPECT_EQ(snapshot.services.size(), 3u);
  const auto list = ExternalQuery({"list", "-t"});
  EXPECT_EQ(list.code, 0) << list.output;
  EXPECT_EQ(list.output, std::string(kName) + " [" + kType + "]\n" +
                        "json:/demo.Late/Compute [" + kType + "]\n");
  const auto type = ExternalQuery({"type", kName});
  EXPECT_EQ(type.code, 0) << type.output;
  EXPECT_EQ(type.output, std::string(kType) + '\n');
  const auto find = ExternalQuery({"find", kType});
  EXPECT_EQ(find.code, 0) << find.output;
  EXPECT_EQ(find.output, std::string(kName) + "\njson:/demo.Late/Compute\n");
  engine.Shutdown();
  ASSERT_EQ(stopped.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  stopped.get();
  EXPECT_TRUE(Query({"list"}).output.empty());
}

}  // namespace
