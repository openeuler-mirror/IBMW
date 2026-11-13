// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "log_control_ext/service.h"

#include "cpp/logger/logger.h"
#include "cpp/util/type_support.h"
#include "gtest/gtest.h"

namespace ibmw::extensions::log_control_extension {
namespace {
namespace protocol = ibmw::protocols::log_control_extension;
namespace logger = ibmw::runtime::core::logger;

class TestService : public LogControlServiceImpl {
 public:
  template <typename Rsp, typename Req>
  Rsp Call(std::string_view name, const Req& request, uint32_t expected_status = 0) {
    Rsp response;
    unsigned completed = 0;
    ibmw::rpc::ServiceCallback callback([&](uint32_t status) {
      EXPECT_EQ(status, expected_status);
      ++completed;
    });
    for (auto& method : service_func_wrapper_vec_) {
      if (method.func_name != name) continue;
      method.service_func(nullptr, &request, &response, callback.NativeHandle());
      EXPECT_EQ(completed, 1u);
      return response;
    }
    ADD_FAILURE() << "Missing method " << name;
    return response;
  }
};

class CaptureDriver : public logger::LoggerDriverBase {
 public:
  std::string_view Type() const noexcept override { return "capture"; }
  bool AllowDuplicates() const noexcept override { return false; }
  void Initialize(YAML::Node) override {}
  void Start() override {}
  void Shutdown() override {}
  void Log(const logger::LogDataWrapper& data) noexcept override { levels.push_back(data.lvl); }
  std::vector<ibmw_log_level_t> levels;
};

class LogControlServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    logging.RegisterLoggerDriverGenFunc("capture", [&] {
      auto driver = std::make_unique<CaptureDriver>();
      capture = driver.get();
      return driver;
    });
    logging.Initialize(YAML::Load("core_level: INFO\nbackends: [{type: capture}]"));
    car = ibmw::logger::LoggerRef(logging.GetLoggerAccess("CarModule").NativeHandle());
    other = ibmw::logger::LoggerRef(logging.GetLoggerAccess("OtherModule").NativeHandle());
    logging.GetLoggerAccess("core");
    logging.Start();
    service.SetLoggingSubsystem(&logging);
  }
  void TearDown() override { logging.Shutdown(); }
  protocol::SetModuleLogLevelRsp Set(std::map<std::string, std::string> values) {
    return service.Call<protocol::SetModuleLogLevelRsp>("SetModuleLogLevel", protocol::SetModuleLogLevelReq{values});
  }
  protocol::GetModuleLogLevelRsp Get(std::vector<std::string> names = {}) {
    return service.Call<protocol::GetModuleLogLevelRsp>("GetModuleLogLevel", protocol::GetModuleLogLevelReq{names});
  }
  logger::LoggingSubsystem logging;
  ibmw::logger::LoggerRef car, other;
  CaptureDriver* capture = nullptr;
  TestService service;
};

TEST_F(LogControlServiceTest, ListsAllOrSelectedLoggersIncludingCore) {
  const std::map<std::string, std::string> expected{{"CarModule", "Info"}, {"OtherModule", "Info"}, {"core", "Info"}};
  EXPECT_EQ(Get().module_log_level_map, expected);
  EXPECT_EQ(Get({"CarModule"}).module_log_level_map,
            (std::map<std::string, std::string>{{"CarModule", "Info"}}));
  for (const auto* name : {"", "MissingModule"}) {
    const auto response = Get({"CarModule", name});
    EXPECT_EQ(response.code, 1u);
    EXPECT_TRUE(response.module_log_level_map.empty());
  }
}

TEST_F(LogControlServiceTest, SetChangesTheModuleLoggerAndActualLogFiltering) {
  unsigned threshold = 0;
  for (const auto* level : {"trace", "DeBuG", "INFO", "warn", "ERROR", "Fatal", "OFF"}) {
    ASSERT_EQ(Set({{"CarModule", level}}).code, 0u);
    EXPECT_EQ(car.GetLogLevel(), threshold);
    EXPECT_EQ(other.GetLogLevel(), static_cast<uint32_t>(IBMW_LOG_LEVEL_INFO));
    EXPECT_EQ(Get({"core"}).module_log_level_map.at("core"), "Info");
    capture->levels.clear();
    for (unsigned emitted = 0; emitted < IBMW_LOG_LEVEL_OFF; ++emitted)
      car.Log(emitted, 1, __FILE__, __func__, "probe", 5);
    std::vector<ibmw_log_level_t> expected;
    for (unsigned emitted = threshold; emitted < IBMW_LOG_LEVEL_OFF; ++emitted)
      expected.push_back(static_cast<ibmw_log_level_t>(emitted));
    EXPECT_EQ(capture->levels, expected);
    ++threshold;
  }
  EXPECT_EQ(Set({{"core", "WARN"}}).code, 0u);
  EXPECT_EQ(Get({"core"}).module_log_level_map.at("core"), "Warn");
}

TEST_F(LogControlServiceTest, InvalidBatchesDoNotMutateAnyLogger) {
  for (const auto* invalid : {"", "DEBIG", " WARNING", "INFO ", "WARNING", "2"}) {
    const auto response = Set({{"CarModule", "DEBUG"}, {"OtherModule", invalid}});
    EXPECT_EQ(response.code, 2u);
    EXPECT_EQ(response.msg, "INVALID_LOG_LEVEL");
    EXPECT_EQ(car.GetLogLevel(), static_cast<uint32_t>(IBMW_LOG_LEVEL_INFO));
    EXPECT_EQ(other.GetLogLevel(), static_cast<uint32_t>(IBMW_LOG_LEVEL_INFO));
  }
  for (const auto* name : {"", "MissingModule"}) {
    EXPECT_EQ(Set({{"CarModule", "DEBUG"}, {name, "INFO"}}).code, 1u);
    EXPECT_EQ(car.GetLogLevel(), static_cast<uint32_t>(IBMW_LOG_LEVEL_INFO));
  }
  EXPECT_EQ(Set({}).code, 3u);
  EXPECT_EQ(Get().module_log_level_map.size(), 3u);
}

TEST(LogControlFailureTest, HandlerExceptionsCompleteTheRpcWithFailure) {
  TestService service;
  // Without a logging subsystem the call is answered with NOT_READY (EXT-014).
  const auto not_ready = service.Call<protocol::GetModuleLogLevelRsp>(
      "GetModuleLogLevel", protocol::GetModuleLogLevelReq{});
  EXPECT_EQ(not_ready.msg, "NOT_READY");
  logger::LoggingSubsystem logging;
  service.SetLoggingSubsystem(&logging);
  // The subsystem rejects calls before Start; the RPC must still complete once.
  service.Call<protocol::GetModuleLogLevelRsp>("GetModuleLogLevel", protocol::GetModuleLogLevelReq{},
                                             IBMW_RPC_STATUS_SVR_HANDLE_FAILED);
  service.Call<protocol::SetModuleLogLevelRsp>("SetModuleLogLevel",
      protocol::SetModuleLogLevelReq{{{"core", "INFO"}}}, IBMW_RPC_STATUS_SVR_HANDLE_FAILED);
}

template <typename T>
void RoundTrip(const T& value) {
  const ibmw::util::TypeSupportRef type(GetLogControlTypeSupport<T>());
  EXPECT_EQ(type.TypeName(), "json:" + std::string(T::kTypeName));
  for (const auto* format : {"json", "cdr"}) {
    SCOPED_TRACE(std::string(T::kTypeName) + " / " + format);
    if (!type.CheckSerializationTypeSupported(format)) continue;
    const auto encoded = type.SimpleSerialize(format, &value);
    T decoded;
    const ibmw_buffer_view_t buffer{encoded.data(), encoded.size()};
    ASSERT_TRUE(type.Deserialize(format, {.data = &buffer, .len = 1}, &decoded));
    EXPECT_EQ(nlohmann::json(decoded), nlohmann::json(value));
  }
}

TEST(LogControlProtocolTest, AllMessagesRoundTripThroughAvailableCodecs) {
  RoundTrip(protocol::GetModuleLogLevelReq{});
  RoundTrip(protocol::GetModuleLogLevelReq{{"CarModule", "模块", std::string(8192, 'x')}});
  const std::map<std::string, std::string> levels{{"CarModule", "DEBUG"}, {"模块", "Info"}, {std::string(8192, 'x'), "OFF"}};
  RoundTrip(protocol::GetModuleLogLevelRsp{0, "", levels});
  RoundTrip(protocol::SetModuleLogLevelReq{levels});
  RoundTrip(protocol::SetModuleLogLevelRsp{2, "INVALID_LOG_LEVEL"});
}

// Exercise the actual registered TypeSupport, including DDS builds' JSON route.
template <typename T>
void CheckResponseValidation(const char* payload) {
  const ibmw::util::TypeSupportRef type(GetLogControlTypeSupport<T>());
  const nlohmann::json valid = T{};
  const bool has_payload = *payload != '\0';
  auto decode = [&](const nlohmann::json& wire, bool expected) {
    SCOPED_TRACE(wire.dump());
    const auto encoded = wire.dump();
    const ibmw_buffer_view_t buffer{encoded.data(), encoded.size()};
    T response;
    response.code = 17;
    response.msg = "unchanged";
    const nlohmann::json original = response;
    EXPECT_EQ(type.Deserialize("json", {.data = &buffer, .len = 1}, &response), expected);
    if (!expected) { EXPECT_EQ(nlohmann::json(response), original); }
  };
  decode(valid, true);  // Empty success payloads are legal.
  for (const auto& invalid : {nlohmann::json(), nlohmann::json::array(),
                              nlohmann::json::object(), nlohmann::json("text"),
                              nlohmann::json(0), nlohmann::json(false)}) decode(invalid, false);
  for (const auto* field : {"code", "msg"}) {
    auto invalid = valid;
    invalid.erase(field);
    decode(invalid, false);
  }
  for (const nlohmann::json& code : {nlohmann::json(-1), nlohmann::json(0.0),
                                   nlohmann::json(0.5), nlohmann::json("0"),
                                   nlohmann::json(true), nlohmann::json()}) {
    auto invalid = valid;
    invalid["code"] = code;
    decode(invalid, false);
  }
  auto boundary = valid;
  boundary["code"] = std::numeric_limits<decltype(T{}.code)>::max();
  decode(boundary, true);
  if constexpr (sizeof(T{}.code) < sizeof(uint64_t)) {
    boundary["code"] = uint64_t{1} << 32;
    decode(boundary, false);
  }
  auto invalid = valid;
  invalid["msg"] = 0;
  decode(invalid, false);
  auto failure = nlohmann::json{{"code", 2}, {"msg", "REMOTE_ERROR"}};
  decode(failure, true);  // Error responses need not contain a success payload.
  if (has_payload) {
    invalid = valid;
    invalid.erase(payload);
    decode(invalid, false);
    for (const nlohmann::json& bad : {nlohmann::json(), nlohmann::json(42),
                                    nlohmann::json::array({42}),
                                    nlohmann::json::object({{"key", 42}})}) {
      invalid = valid;
      invalid[payload] = bad;
      decode(invalid, false);
      failure[payload] = bad;
      decode(failure, false);
    }
  }
}

TEST(LogControlProtocolTest, SetModuleLogLevelRspRejectsMalformedJson) {
  CheckResponseValidation<protocol::SetModuleLogLevelRsp>("");
}

TEST(LogControlProtocolTest, GetModuleLogLevelRspRejectsMalformedJson) {
  CheckResponseValidation<protocol::GetModuleLogLevelRsp>("module_log_level_map");
}
}  // namespace
}  // namespace ibmw::extensions::log_control_extension
