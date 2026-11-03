// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests: the in-process RPC path must report every failure through
// the client callback and must survive servers that answer late.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

#include "cpp/service/rpc_handle.h"
#include "cpp/util/string.h"
#include "engine/service/inprocess_service.h"
#include "engine/service/service_access.h"
#include "engine/service/service_dispatcher.h"
#include "engine/service/service_driver_dispatcher.h"
#include "engine/service/service_driver_tools.h"

namespace ibmw::runtime::core::service {
namespace {

ibmw_type_support_base_t MakeCopyOnlyTypeSupport() {
  ibmw_type_support_base_t ts{};
  ts.serialization_types_supported_num = [](void*) -> size_t { return 0; };
  return ts;
}

const ibmw_type_support_base_t kTs = MakeCopyOnlyTypeSupport();

// One in-process driver with a single same-package service "svc".
class InProcessServiceTest : public ::testing::Test {
 protected:
  void SetUpService(ServiceFunc func, ServiceInfo info = MakeInfo()) {
    auto wrapper = std::make_unique<ServiceFuncWrapper>();
    wrapper->info = std::move(info);
    wrapper->service_func = std::move(func);
    const ServiceFuncWrapper& ref = *wrapper;
    ASSERT_TRUE(catalog_.RegisterServiceFunc(std::move(wrapper)));

    driver_ = std::make_unique<InProcessServiceDriver>();
    driver_->SetServiceCatalog(&catalog_);
    driver_->Initialize(YAML::Node());
    ASSERT_TRUE(driver_->RegisterServiceFunc(ref));
    driver_->Start();
  }

  static ServiceInfo MakeInfo() {
    ServiceInfo info;
    info.func_name = "svc";
    info.pkg_path = "pkg";
    info.module_name = "mod";
    info.req_type_support_ref = ibmw::util::TypeSupportRef(&kTs);
    info.rsp_type_support_ref = ibmw::util::TypeSupportRef(&kTs);
    return info;
  }

  // Issues one call; the returned optional receives the status code.
  std::shared_ptr<std::optional<uint32_t>> CallOnce() {
    auto result = std::make_shared<std::optional<uint32_t>>();
    auto frame = std::make_shared<ServiceMessageFrame>(ServiceMessageFrame{
        .info = client_info_, .req_ptr = &req_, .rsp_ptr = &rsp_, .ctx_ref = client_ctx_});
    frame->callback = [result](ibmw::rpc::Status status) { *result = status.Code(); };
    driver_->Call(frame);
    return result;
  }

  ServiceCatalog catalog_;
  std::unique_ptr<InProcessServiceDriver> driver_;
  ServiceInfo client_info_ = MakeInfo();
  ibmw::rpc::Context client_ctx_;
  int req_ = 0;
  int rsp_ = 0;
};

// Serializes to an empty buffer; deserializing fails when the template
// argument says so.
template <bool kDeserializeOk>
ibmw_type_support_base_t MakeSerializingTypeSupport() {
  static const ibmw_string_view_t kTypes[] = {{"json", 4}};
  ibmw_type_support_base_t ts{};
  ts.create = [](void*) -> void* { return new int(0); };
  ts.destroy = [](void*, void* msg) { delete static_cast<int*>(msg); };
  ts.serialize = [](void*, ibmw_string_view_t, const void*, const ibmw_buffer_array_allocator_t*,
                    ibmw_buffer_array_t*) { return true; };
  ts.deserialize = [](void*, ibmw_string_view_t, ibmw_buffer_array_view_t, void*) { return kDeserializeOk; };
  ts.serialization_types_supported_num = [](void*) -> size_t { return 1; };
  ts.serialization_types_supported_list = [](void*) -> const ibmw_string_view_t* { return kTypes; };
  return ts;
}

const ibmw_type_support_base_t kSerializingTs = MakeSerializingTypeSupport<true>();
const ibmw_type_support_base_t kUndeserializableTs = MakeSerializingTypeSupport<false>();

// COMM-015: a throwing service handler must complete the call with an error.
TEST_F(InProcessServiceTest, ThrowingServiceFuncReportsInternalError) {
  SetUpService([](const std::shared_ptr<ServiceMessageFrame>&) { throw std::runtime_error("boom"); });
  auto result = CallOnce();
  ASSERT_TRUE(result->has_value());
  EXPECT_EQ(**result, static_cast<uint32_t>(IBMW_RPC_STATUS_SVR_DRIVER_INTERNAL_ERROR));
}

TEST_F(InProcessServiceTest, NonStdExceptionFromServiceFuncIsContained) {
  SetUpService([](const std::shared_ptr<ServiceMessageFrame>&) { throw 42; });
  auto result = CallOnce();
  ASSERT_TRUE(result->has_value());
  EXPECT_EQ(**result, static_cast<uint32_t>(IBMW_RPC_STATUS_SVR_DRIVER_INTERNAL_ERROR));
}

// A client callback that throws while a failure is reported (from the catch
// of the noexcept Call) must not terminate the process.
TEST_F(InProcessServiceTest, ThrowingClientCallbackOnFailureIsContained) {
  SetUpService([](const std::shared_ptr<ServiceMessageFrame>&) { throw std::runtime_error("boom"); });
  int completions = 0;
  auto frame = std::make_shared<ServiceMessageFrame>(ServiceMessageFrame{
      .info = client_info_, .req_ptr = &req_, .rsp_ptr = &rsp_, .ctx_ref = client_ctx_});
  frame->callback = [&completions](ibmw::rpc::Status) {
    ++completions;
    throw std::runtime_error("client boom");
  };
  driver_->Call(frame);
  EXPECT_EQ(completions, 1);
}

// COMM-015: a server that keeps the answer callback and then throws gets
// exactly one completion (the error) and its record must not linger; a late
// answer is ignored.
TEST_F(InProcessServiceTest, ThrowAfterKeepingCallbackCompletesOnceAndReleasesRecord) {
  std::shared_ptr<ServiceMessageFrame> pending;
  SetUpService([&pending](const std::shared_ptr<ServiceMessageFrame>& frame) {
    pending = frame;
    throw std::runtime_error("boom");
  });

  int completions = 0;
  uint32_t last_code = 0;
  std::weak_ptr<ServiceMessageFrame> weak_frame;
  {
    auto frame = std::make_shared<ServiceMessageFrame>(ServiceMessageFrame{
        .info = client_info_, .req_ptr = &req_, .rsp_ptr = &rsp_, .ctx_ref = client_ctx_});
    frame->callback = [&](ibmw::rpc::Status status) {
      ++completions;
      last_code = status.Code();
    };
    weak_frame = frame;
    driver_->Call(frame);
  }
  EXPECT_EQ(completions, 1);
  EXPECT_EQ(last_code, static_cast<uint32_t>(IBMW_RPC_STATUS_SVR_DRIVER_INTERNAL_ERROR));
  // Only the driver's record could still keep the client frame alive.
  EXPECT_TRUE(weak_frame.expired());

  ASSERT_TRUE(pending);
  pending->callback(ibmw::rpc::Status(IBMW_RPC_STATUS_OK));
  EXPECT_EQ(completions, 1);
}

// COMM-040: across packages the request is deserialized by the server and
// the response by the client; each failure is reported for its own side.
TEST_F(InProcessServiceTest, RequestDeserializationFailureIsServerSide) {
  bool served = false;
  ServiceInfo service_info = MakeInfo();
  service_info.req_type_support_ref = ibmw::util::TypeSupportRef(&kUndeserializableTs);
  service_info.rsp_type_support_ref = ibmw::util::TypeSupportRef(&kSerializingTs);
  SetUpService([&served](const std::shared_ptr<ServiceMessageFrame>&) { served = true; }, service_info);
  client_info_.pkg_path = "client_pkg";
  client_info_.req_type_support_ref = ibmw::util::TypeSupportRef(&kSerializingTs);
  client_info_.rsp_type_support_ref = ibmw::util::TypeSupportRef(&kSerializingTs);
  client_ctx_.SetSerializationType("json");
  auto result = CallOnce();
  ASSERT_TRUE(result->has_value());
  EXPECT_EQ(**result, static_cast<uint32_t>(IBMW_RPC_STATUS_SVR_DESERIALIZATION_FAILED));
  EXPECT_FALSE(served);
}

TEST_F(InProcessServiceTest, ResponseDeserializationFailureIsClientSide) {
  ServiceInfo service_info = MakeInfo();
  service_info.req_type_support_ref = ibmw::util::TypeSupportRef(&kSerializingTs);
  service_info.rsp_type_support_ref = ibmw::util::TypeSupportRef(&kSerializingTs);
  SetUpService([](const std::shared_ptr<ServiceMessageFrame>& frame) {
    frame->callback(ibmw::rpc::Status(IBMW_RPC_STATUS_OK));
  }, service_info);
  client_info_.pkg_path = "client_pkg";
  client_info_.req_type_support_ref = ibmw::util::TypeSupportRef(&kSerializingTs);
  client_info_.rsp_type_support_ref = ibmw::util::TypeSupportRef(&kUndeserializableTs);
  client_ctx_.SetSerializationType("json");
  auto result = CallOnce();
  ASSERT_TRUE(result->has_value());
  EXPECT_EQ(**result, static_cast<uint32_t>(IBMW_RPC_STATUS_CLI_DESERIALIZATION_FAILED));
}

// COMM-010: a server answering after the driver was shut down and destroyed
// must not touch the driver; the client still gets the answer.
TEST_F(InProcessServiceTest, LateAnswerAfterDriverDestroyedIsSafe) {
  std::shared_ptr<ServiceMessageFrame> pending;
  SetUpService([&pending](const std::shared_ptr<ServiceMessageFrame>& frame) { pending = frame; });
  auto result = CallOnce();
  ASSERT_TRUE(pending);
  EXPECT_FALSE(result->has_value());

  driver_->Shutdown();
  driver_.reset();

  pending->callback(ibmw::rpc::Status(IBMW_RPC_STATUS_OK));
  ASSERT_TRUE(result->has_value());
  EXPECT_EQ(**result, static_cast<uint32_t>(IBMW_RPC_STATUS_OK));
}

// COMM-012: Call() is reached from noexcept C ABI entry points; when the
// dispatcher is not started it must answer through the callback, not throw.
TEST(ServiceDriverDispatcherCall, NotStartedReportsNotReady) {
  ServiceDriverDispatcher dispatcher;
  std::optional<uint32_t> code;
  ibmw::rpc::ClientCallback callback([&code](uint32_t c) { code = c; });
  ibmw::rpc::Context ctx;
  int req = 0, rsp = 0;
  EXPECT_NO_THROW(dispatcher.Call(CallAccessInfoWrapper{
      .pkg_path = "pkg",
      .module_name = "mod",
      .func_name = ibmw::util::ToIbmwStringView("svc"),
      .ctx_ptr = ctx.NativeHandle(),
      .req_ptr = &req,
      .rsp_ptr = &rsp,
      .callback = callback.NativeHandle()}));
  ASSERT_TRUE(code.has_value());
  EXPECT_EQ(*code, static_cast<uint32_t>(IBMW_RPC_STATUS_CLI_NOT_READY));
}

// Keeps every call it gets and never answers on its own.
class HoldingServiceDriver : public ServiceDriver {
 public:
  std::string_view Name() const noexcept override { return "holding"; }
  void Initialize(YAML::Node) override {}
  void Start() override {}
  void Shutdown() override {}
  bool RegisterServiceFunc(const ServiceFuncWrapper&) noexcept override { return true; }
  bool RegisterClientFunc(const ClientFuncWrapper&) noexcept override { return true; }
  void Call(const std::shared_ptr<ServiceMessageFrame>& frame) noexcept override { held = frame; }

  std::shared_ptr<ServiceMessageFrame> held;
};

// One client "svc" routed to a HoldingServiceDriver through the given filters.
// debug_log serializes the request, so the type support must serialize.
class HoldingDispatcherTest : public ::testing::TestWithParam<const char*> {
 protected:
  void SetUp() override {
    auto driver = std::make_unique<HoldingServiceDriver>();
    driver_ = driver.get();
    dispatcher_.RegisterRpcDriver(std::move(driver));
    std::string filters = GetParam();
    dispatcher_.Initialize(YAML::Load(
        "drivers: [{type: holding}]\n"
        "clients: [{func_name: svc, enable_drivers: [holding], enable_filters: [" + filters + "]}]"));
    handle_ = dispatcher_.GetRpcAccess("mod").NativeHandle();
    ASSERT_TRUE(handle_->register_client_func(
        handle_->ctx, ibmw::util::ToIbmwStringView("svc"), nullptr, &kSerializingTs, &kSerializingTs));
    dispatcher_.Start();
  }

  void TearDown() override { dispatcher_.Shutdown(); }

  void Call() {
    ibmw::rpc::ClientCallback callback([this](uint32_t c) { codes_.push_back(c); });
    handle_->call(handle_->ctx, ibmw::util::ToIbmwStringView("svc"), ctx_.NativeHandle(),
                  &req_, &rsp_, callback.NativeHandle());
  }

  ServiceDispatcher dispatcher_;
  HoldingServiceDriver* driver_ = nullptr;
  const ibmw_service_handle_base_t* handle_ = nullptr;
  ibmw::rpc::Context ctx_;
  int req_ = 0;
  int rsp_ = 0;
  std::vector<uint32_t> codes_;
};

// COMM-014: a driver that drops a call without answering must free the frame
// and the caller must get an error, not wait forever.
TEST_P(HoldingDispatcherTest, DroppedCallFreesFrameAndReportsInternalError) {
  Call();
  ASSERT_TRUE(driver_->held);
  EXPECT_TRUE(codes_.empty());

  std::weak_ptr<ServiceMessageFrame> weak_frame = driver_->held;
  driver_->held.reset();
  EXPECT_TRUE(weak_frame.expired());
  ASSERT_EQ(codes_.size(), 1u);
  EXPECT_EQ(codes_[0], static_cast<uint32_t>(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
}

// The drop fallback must not fire again after a real answer.
TEST_P(HoldingDispatcherTest, AnsweredCallReportsOnlyTheAnswer) {
  Call();
  ASSERT_TRUE(driver_->held);
  CallResultCallback(*driver_->held, ibmw::rpc::Status(IBMW_RPC_STATUS_OK));

  std::weak_ptr<ServiceMessageFrame> weak_frame = driver_->held;
  driver_->held.reset();
  EXPECT_TRUE(weak_frame.expired());
  ASSERT_EQ(codes_.size(), 1u);
  EXPECT_EQ(codes_[0], static_cast<uint32_t>(IBMW_RPC_STATUS_OK));
}

INSTANTIATE_TEST_SUITE_P(Filters, HoldingDispatcherTest, ::testing::Values("", "debug_log"));

}  // namespace
// HYG-039: merging contexts of the wrong types is skipped with a warning
// (rate limited), not silently.
TEST(ServiceAccessMergeContext, WrongContextTypesAreLogged) {
  std::vector<std::string> logs;
  ServiceDriverDispatcher dispatcher;
  dispatcher.SetLogger(std::make_shared<ibmw::common::util::LoggerWrapper>(ibmw::common::util::LoggerWrapper{
      .get_log_level_func = [] { return ibmw::common::util::kLogLevelTrace; },
      .log_func = [&logs](uint32_t, uint32_t, const char*, const char*, const char* data, size_t size) {
        logs.emplace_back(data, size);
      }}));
  const std::unordered_set<std::string> passed_keys{"trace"};  // ServiceAccess keeps a reference
  ServiceAccess access("pkg", "mod", dispatcher, passed_keys);
  const auto* handle = access.NativeHandle();

  ibmw::rpc::Context server(ibmw_rpc_context_type_t::IBMW_RPC_SERVER_CONTEXT);
  ibmw::rpc::Context client;
  server.SetMetaValue("trace", "t1");
  handle->merge_server_context_to_client_context(handle->ctx, server.NativeHandle(), client.NativeHandle());
  EXPECT_EQ(client.GetMetaValue("trace"), "t1");
  EXPECT_TRUE(logs.empty());

  ibmw::rpc::Context other_client;
  for (int i = 0; i < 3; ++i) {
    handle->merge_server_context_to_client_context(handle->ctx, client.NativeHandle(), other_client.NativeHandle());
  }
  EXPECT_TRUE(other_client.GetMetaValue("trace").empty());
  ASSERT_EQ(logs.size(), 1u);
  EXPECT_NE(logs[0].find("Service context merge skipped"), std::string::npos) << logs[0];
}

}  // namespace ibmw::runtime::core::service
