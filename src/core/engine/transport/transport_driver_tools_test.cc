// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// COMM-038: payloads that fail to (de)serialize are dropped with a counter
// instead of throwing out of the transport receive thread.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>

#include "engine/transport/transport_driver_tools.h"

namespace ibmw::runtime::core::transport {
namespace {

struct TestPayload {
  int value = 0;
};

ibmw_string_view_t TestTypeName(void*) {
  static constexpr char kTypeName[] = "transport_driver_tools_test/TestPayload";
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

bool FailingSerialize(
    void*, ibmw_string_view_t, const void*, const ibmw_buffer_array_allocator_t*, ibmw_buffer_array_t*) {
  return false;
}

bool FailingDeserialize(void*, ibmw_string_view_t, ibmw_buffer_array_view_t, void*) { return false; }

// Same type as TestTypeSupport, but every (de)serialization fails.
const ibmw_type_support_base_t* FailingTypeSupport() {
  static const ibmw_type_support_base_t kSupport = [] {
    ibmw_type_support_base_t support = *TestTypeSupport();
    support.serialize = FailingSerialize;
    support.deserialize = FailingDeserialize;
    return support;
  }();
  return &kSupport;
}

struct Observed {
  int calls = 0;
  int value = 0;
};

MessageHandlerWrapper MakeHandler(Observed* observed, const ibmw_type_support_base_t* type_support) {
  MessageHandlerWrapper handler;
  handler.info.msg_type = "transport_driver_tools_test/TestPayload";
  handler.info.topic_name = "topic";
  handler.info.pkg_path = "pkg";
  handler.info.module_name = "module";
  handler.info.msg_type_support_ref = ibmw::util::TypeSupportRef(type_support);
  handler.callback = [observed](auto& frame, auto&& release, const auto*) {
    ++observed->calls;
    CheckMsg(frame);
    observed->value = static_cast<const TestPayload*>(frame.msg_ptr)->value;
    release();
  };
  return handler;
}

std::shared_ptr<ibmw::transport::Context> MakeContext() {
  return std::make_shared<ibmw::transport::Context>(
      ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT);
}

TEST(MessageHandlerToolTest, DeserializeFailureSkipsOnlyThatHandler) {
  Observed bad_seen, good_seen;
  auto bad = MakeHandler(&bad_seen, FailingTypeSupport());
  auto good = MakeHandler(&good_seen, TestTypeSupport());
  MessageHandlerTool tool;
  tool.AddMessageHandlerWrapper(&bad);
  tool.AddMessageHandlerWrapper(&good);

  TestPayload payload{.value = 7};
  EXPECT_NO_THROW(tool.DoMessageCallback(MakeContext(), "test", &payload, sizeof(payload)));
  EXPECT_EQ(bad_seen.calls, 0);
  EXPECT_EQ(good_seen.calls, 1);
  EXPECT_EQ(good_seen.value, 7);
  EXPECT_EQ(tool.DeserializeFailureCount(), 1u);

  // A truncated payload from the peer fails for every handler.
  EXPECT_NO_THROW(tool.DoMessageCallback(MakeContext(), "test", &payload, sizeof(payload) - 1));
  EXPECT_EQ(good_seen.calls, 1);
  EXPECT_EQ(tool.DeserializeFailureCount(), 3u);

  ibmw::util::BufferArrayView view(&payload, sizeof(payload));
  EXPECT_NO_THROW(tool.DoMessageCallback(MakeContext(), "test", view));
  EXPECT_EQ(good_seen.calls, 2);
  EXPECT_EQ(tool.DeserializeFailureCount(), 4u);
}

TEST(MessageHandlerToolTest, FanoutSerializeFailureSkipsFollowers) {
  Observed first_seen, second_seen;
  auto first = MakeHandler(&first_seen, FailingTypeSupport());
  auto second = MakeHandler(&second_seen, TestTypeSupport());
  MessageHandlerTool tool;
  tool.AddMessageHandlerWrapper(&first);
  tool.AddMessageHandlerWrapper(&second);

  auto payload = std::make_shared<TestPayload>();
  payload->value = 9;
  EXPECT_NO_THROW(tool.DoMessageCallback(MakeContext(), first, payload));
  EXPECT_EQ(first_seen.calls, 1);
  EXPECT_EQ(first_seen.value, 9);
  EXPECT_EQ(second_seen.calls, 0);
  EXPECT_EQ(tool.SerializeFailureCount(), 1u);
}

TEST(MessageHandlerToolTest, FanoutDeliversToAllHandlers) {
  Observed first_seen, second_seen;
  auto first = MakeHandler(&first_seen, TestTypeSupport());
  auto second = MakeHandler(&second_seen, TestTypeSupport());
  MessageHandlerTool tool;
  tool.AddMessageHandlerWrapper(&first);
  tool.AddMessageHandlerWrapper(&second);

  auto payload = std::make_shared<TestPayload>();
  payload->value = 11;
  tool.DoMessageCallback(MakeContext(), first, payload);
  EXPECT_EQ(first_seen.value, 11);
  EXPECT_EQ(second_seen.value, 11);
  EXPECT_EQ(tool.SerializeFailureCount(), 0u);
}

TEST(MessageHandlerToolTest, FanoutWithoutHandlersThrows) {
  Observed seen;
  auto handler = MakeHandler(&seen, TestTypeSupport());
  MessageHandlerTool tool;
  EXPECT_ANY_THROW(tool.DoMessageCallback(MakeContext(), handler, std::make_shared<TestPayload>()));
  EXPECT_EQ(seen.calls, 0);
}

}  // namespace
}  // namespace ibmw::runtime::core::transport
