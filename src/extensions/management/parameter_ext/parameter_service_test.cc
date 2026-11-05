// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "parameter_ext/service.h"

#include "gtest/gtest.h"
#include "cpp/util/type_support.h"

namespace ibmw::extensions::parameter_extension {
namespace {
namespace protocol = ibmw::protocols::parameter_extension;

class TestService : public ParameterServiceImpl {
 public:
  template <typename Rsp, typename Req>
  Rsp Call(std::string_view name, const Req& request) {
    Rsp response;
    bool completed = false;
    ibmw::rpc::ServiceCallback callback([&](uint32_t status) {
      EXPECT_EQ(status, 0u);
      completed = true;
    });
    for (auto& method : service_func_wrapper_vec_) {
      if (method.func_name != name) continue;
      method.service_func(nullptr, &request, &response, callback.NativeHandle());
      EXPECT_TRUE(completed);
      return response;
    }
    ADD_FAILURE() << "Missing method " << name;
    return response;
  }
};

class ParameterServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    store.Initialize(YAML::Node{});
    car = ibmw::parameter::ParamsRef(store.GetParamsAccess("CarModule").NativeHandle());
    other = ibmw::parameter::ParamsRef(store.GetParamsAccess("OtherModule").NativeHandle());
    car.SetParameter("speed", "0.2");
    other.SetParameter("speed", "7");
    store.Start();
    service.SetParameterStore(&store);
  }
  void TearDown() override { store.Shutdown(); }
  runtime::core::parameter::ParameterStore store;
  ibmw::parameter::ParamsRef car, other;
  TestService service;
};

TEST_F(ParameterServiceTest, RemoteSetChangesTheSameStoreReadByTheModule) {
  EXPECT_EQ(service.Call<protocol::SetParameterRsp>("Set",
      protocol::SetParameterReq{"CarModule", "speed", "0.5"}).code, 0u);
  EXPECT_EQ(car.GetParameter("speed"), "0.5");
  EXPECT_EQ(other.GetParameter("speed"), "7");
  EXPECT_EQ(service.Call<protocol::GetParameterRsp>("Get",
      protocol::GetParameterReq{"CarModule", "speed"}).parameter_value, "0.5");
}

TEST_F(ParameterServiceTest, MissingParametersDifferFromLiteralEmptyStrings) {
  EXPECT_EQ(service.Call<protocol::GetParameterRsp>("Get",
      protocol::GetParameterReq{"CarModule", "missing"}).code, 2u);
  for (const auto* key : {"speed", "new_key"}) {
    EXPECT_EQ(service.Call<protocol::SetParameterRsp>("Set",
        protocol::SetParameterReq{"CarModule", key, ""}).code, 0u);
    const auto value = service.Call<protocol::GetParameterRsp>("Get",
        protocol::GetParameterReq{"CarModule", key});
    EXPECT_EQ(value.code, 0u);
    EXPECT_TRUE(value.parameter_value.empty());
    EXPECT_EQ(car.GetParameter(key), "");
    EXPECT_NE(store.GetParams("CarModule")->GetParameter(key), nullptr);
  }
  // Existing module API deletion semantics remain unchanged.
  car.SetParameter("speed", "");
  EXPECT_EQ(store.GetParams("CarModule")->GetParameter("speed"), nullptr);
}

TEST_F(ParameterServiceTest, InvalidTargetsDoNotCreateModulesOrEmptyKeys) {
  EXPECT_EQ(service.Call<protocol::SetParameterRsp>("Set",
      protocol::SetParameterReq{"MissingModule", "speed", "2"}).code, 1u);
  EXPECT_EQ(service.Call<protocol::SetParameterRsp>("Set",
      protocol::SetParameterReq{"CarModule", "", "2"}).code, 3u);
  EXPECT_EQ(car.GetParameter("speed"), "0.2");
  EXPECT_EQ(store.GetParams("MissingModule"), nullptr);
  EXPECT_EQ(store.GetParams("CarModule")->GetParameter(""), nullptr);
}

TEST_F(ParameterServiceTest, EnumeratesModulesAndSortedParameterNames) {
  car.SetParameter("z", "last");
  car.SetParameter("a", "first");
  EXPECT_EQ(service.Call<protocol::ListModulesRsp>("ListModules", protocol::ListModulesReq{}).module_names,
            (std::vector<std::string>{"CarModule", "OtherModule"}));
  EXPECT_EQ(service.Call<protocol::ListParameterRsp>("List",
      protocol::ListParameterReq{"CarModule"}).parameter_keys,
      (std::vector<std::string>{"a", "speed", "z"}));
}

template <typename T>
void RoundTrip(const T& value) {
  const ibmw::util::TypeSupportRef type(GetParameterTypeSupport<T>());
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

TEST(ParameterProtocolTest, AllProtocolMessagesRoundTripThroughAvailableCodecs) {
  RoundTrip(protocol::SetParameterReq{"模块", "key", "\"文本\"\n"});
  RoundTrip(protocol::SetParameterRsp{42, "reason"});
  RoundTrip(protocol::GetParameterReq{"car", "speed"});
  RoundTrip(protocol::GetParameterRsp{0, "", ""});
  RoundTrip(protocol::ListParameterReq{"car"});
  RoundTrip(protocol::ListParameterRsp{0, "", {"a", "速度", "nested.key"}});
  RoundTrip(protocol::ListModulesReq{});
  RoundTrip(protocol::ListModulesRsp{0, "", {"car", "other"}});
  RoundTrip(protocol::DumpParameterReq{{"car", "other"}});
  const std::map<std::string, protocol::ParameterMap> values{
      {"car", {{{"speed", "0.5"}, {"empty", ""}}}}, {"other", {{{"text", "中文"}}}}};
  RoundTrip(protocol::DumpParameterRsp{0, "", values});
  RoundTrip(protocol::LoadParameterReq{values});
  RoundTrip(protocol::LoadParameterRsp{0, ""});
}

TEST(ParameterProtocolTest, EmptyValueReleasesItsPublicApiHolder) {
  bool released = false;
  ibmw::parameter::ParameterValReleaseCallback release([&] { released = true; });
  ibmw_parameter_handle_base_t base{};
  base.ctx = &release;
  base.get_parameter = [](void* ctx, ibmw_string_view_t) {
    return ibmw_parameter_val_view_holder_t{
        .parameter_val = {"", 0},
        .release_callback = static_cast<ibmw::parameter::ParameterValReleaseCallback*>(ctx)->NativeHandle()};
  };
  EXPECT_EQ(ibmw::parameter::ParamsRef(&base).GetParameter("empty"), "");
  EXPECT_TRUE(released);
}

// Exercise the actual registered TypeSupport, including DDS builds' JSON route.
template <typename T>
void CheckResponseValidation(const char* payload) {
  const ibmw::util::TypeSupportRef type(GetParameterTypeSupport<T>());
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

TEST(ParameterProtocolTest, SetParameterRspRejectsMalformedJson) {
  CheckResponseValidation<protocol::SetParameterRsp>("");
}

TEST(ParameterProtocolTest, GetParameterRspRejectsMalformedJson) {
  CheckResponseValidation<protocol::GetParameterRsp>("parameter_value");
}

TEST(ParameterProtocolTest, ListParameterRspRejectsMalformedJson) {
  CheckResponseValidation<protocol::ListParameterRsp>("parameter_keys");
}

TEST(ParameterProtocolTest, ListModulesRspRejectsMalformedJson) {
  CheckResponseValidation<protocol::ListModulesRsp>("module_names");
}
}  // namespace
}  // namespace ibmw::extensions::parameter_extension
