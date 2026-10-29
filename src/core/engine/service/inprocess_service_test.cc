// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests: the in-process RPC path must report every failure through
// the client callback and must survive servers that answer late.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <stdexcept>

#include "cpp/service/rpc_handle.h"
#include "cpp/util/string.h"
#include "engine/service/inprocess_service.h"
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
  void SetUpService(ServiceFunc func) {
    auto wrapper = std::make_unique<ServiceFuncWrapper>();
    wrapper->info = MakeInfo();
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

}  // namespace
}  // namespace ibmw::runtime::core::service
