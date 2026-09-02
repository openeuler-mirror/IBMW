// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>

#include "dds_ext/dds_loan_bus.h"
#include "engine/transport/transport_driver_tools.h"

namespace ibmw::extensions::dds_extension {
namespace {

using ibmw::runtime::core::transport::MessageHandlerTool;
using ibmw::runtime::core::transport::MessageHandlerWrapper;

struct TestPayload {
  int value = 0;
};

ibmw_string_view_t TestTypeName(void*) {
  static constexpr char kTypeName[] = "dds_loan_mode_test/TestPayload";
  return {kTypeName, sizeof(kTypeName) - 1};
}

void* TestCreate(void*) { return new TestPayload(); }
void TestDestroy(void*, void* msg) { delete static_cast<TestPayload*>(msg); }
void TestCopy(void*, const void* from, void* to) {
  *static_cast<TestPayload*>(to) = *static_cast<const TestPayload*>(from);
}
void TestMove(void*, void* from, void* to) { TestCopy(nullptr, from, to); }

bool TestSerialize(
    void*,
    ibmw_string_view_t,
    const void* msg,
    const ibmw_buffer_array_allocator_t* allocator,
    ibmw_buffer_array_t* buffer_array) {
  auto buffer = allocator->allocate(allocator->ctx, buffer_array, sizeof(TestPayload));
  if (buffer.data == nullptr || buffer.len != sizeof(TestPayload)) return false;
  std::memcpy(buffer.data, msg, sizeof(TestPayload));
  return true;
}

bool TestDeserialize(
    void*,
    ibmw_string_view_t,
    ibmw_buffer_array_view_t buffer_array_view,
    void* msg) {
  if (buffer_array_view.len == 0 || buffer_array_view.data[0].data == nullptr ||
      buffer_array_view.data[0].len != sizeof(TestPayload)) {
    return false;
  }
  std::memcpy(msg, buffer_array_view.data[0].data, sizeof(TestPayload));
  return true;
}

size_t TestSerializationTypesSupportedNum(void*) { return 1; }
const ibmw_string_view_t* TestSerializationTypesSupportedList(void*) {
  static constexpr char kSerializationType[] = "test";
  static const ibmw_string_view_t kTypes[] = {{kSerializationType, sizeof(kSerializationType) - 1}};
  return kTypes;
}
const void* TestCustomTypeSupportPtr(void*) { return nullptr; }

const ibmw_type_support_base_t* TestTypeSupport() {
  static const ibmw_type_support_base_t kSupport{
      .type_name = TestTypeName,
      .create = TestCreate,
      .destroy = TestDestroy,
      .copy = TestCopy,
      .move = TestMove,
      .serialize = TestSerialize,
      .deserialize = TestDeserialize,
      .serialization_types_supported_num = TestSerializationTypesSupportedNum,
      .serialization_types_supported_list = TestSerializationTypesSupportedList,
      .custom_type_support_ptr = TestCustomTypeSupportPtr,
      .ctx = nullptr,
  };
  return &kSupport;
}

MessageHandlerWrapper MakeHandler(
    std::string module_name,
    int* observed_value,
    bool* saw_direct_pointer,
    int* calls) {
  MessageHandlerWrapper handler;
  handler.info.msg_type = "dds_loan_mode_test/TestPayload";
  handler.info.topic_name = "fanout";
  handler.info.pkg_path = "pkg";
  handler.info.module_name = std::move(module_name);
  handler.info.msg_type_support_ref = ibmw::util::TypeSupportRef(TestTypeSupport());
  handler.callback = [observed_value, saw_direct_pointer, calls](
                         auto& frame, auto&& release,
                         const auto*) {
    ++(*calls);
    *saw_direct_pointer = (frame.msg_ptr != nullptr);
    ibmw::runtime::core::transport::CheckMsg(frame);
    *observed_value = static_cast<const TestPayload*>(frame.msg_ptr)->value;
    release();
  };
  return handler;
}

TEST(DdsLoanModeTest, ParseLoanModeRecognizesExplicitModes) {
  EXPECT_EQ(ParseLoanMode("sync"), LoanMode::SYNC);
  EXPECT_EQ(ParseLoanMode("async"), LoanMode::ASYNC);
}

TEST(DdsLoanModeTest, ParseLoanModeDefaultsToAuto) {
  EXPECT_EQ(ParseLoanMode(""), LoanMode::AUTO);
  EXPECT_EQ(ParseLoanMode("unknown"), LoanMode::AUTO);
}

TEST(DdsLoanModeTest, ResolveEffectiveLoanModeChoosesByExecutorForAuto) {
  EXPECT_EQ(ResolveEffectiveLoanMode(LoanMode::AUTO, false), LoanMode::SYNC);
  EXPECT_EQ(ResolveEffectiveLoanMode(LoanMode::AUTO, true), LoanMode::ASYNC);
}

TEST(DdsLoanModeTest, ResolveEffectiveLoanModeKeepsExplicitModes) {
  EXPECT_EQ(ResolveEffectiveLoanMode(LoanMode::SYNC, false), LoanMode::SYNC);
  EXPECT_EQ(ResolveEffectiveLoanMode(LoanMode::SYNC, true), LoanMode::SYNC);
  EXPECT_EQ(ResolveEffectiveLoanMode(LoanMode::ASYNC, false), LoanMode::ASYNC);
  EXPECT_EQ(ResolveEffectiveLoanMode(LoanMode::ASYNC, true), LoanMode::ASYNC);
}

TEST(DdsLoanModeTest, LoanModeToStringReturnsUppercaseNames) {
  EXPECT_STREQ(LoanModeToString(LoanMode::SYNC), "SYNC");
  EXPECT_STREQ(LoanModeToString(LoanMode::ASYNC), "ASYNC");
  EXPECT_STREQ(LoanModeToString(LoanMode::AUTO), "AUTO");
}

TEST(DdsMessageHandlerToolTest, SharedPtrFanoutDeliversFirstHandlerDirectlyAndSerializesFollowers) {
  int first_value = 0;
  int second_value = 0;
  bool first_direct = false;
  bool second_direct = true;
  int first_calls = 0;
  int second_calls = 0;

  auto first = MakeHandler("first", &first_value, &first_direct, &first_calls);
  auto second = MakeHandler("second", &second_value, &second_direct, &second_calls);

  MessageHandlerTool tool;
  tool.AddMessageHandlerWrapper(&first);
  tool.AddMessageHandlerWrapper(&second);

  auto payload = std::make_shared<TestPayload>();
  payload->value = 42;
  auto ctx = std::make_shared<ibmw::transport::Context>(
      ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT);

  tool.DoMessageCallback(ctx, first, payload);

  EXPECT_EQ(first_calls, 1);
  EXPECT_EQ(second_calls, 1);
  EXPECT_EQ(first_value, 42);
  EXPECT_EQ(second_value, 42);
  EXPECT_TRUE(first_direct);
  EXPECT_FALSE(second_direct);
}

}  // namespace
}  // namespace ibmw::extensions::dds_extension
