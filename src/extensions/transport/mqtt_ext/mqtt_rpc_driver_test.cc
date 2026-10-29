// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for the MQTT RPC client path: every failure reaches the
// client callback exactly once and releases the call record, also after
// Shutdown (the client tool lives until the driver is destroyed).

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cpp/util/string.h"
#include "mqtt_ext/mqtt_rpc_driver.h"
#include "mqtt_ext/msg_handle_catalog.h"

namespace ibmw::extensions::mqtt_extension {
namespace {

// "json" int: the decimal text of the value.
const ibmw_type_support_base_t* JsonIntTypeSupport() {
  static const ibmw_string_view_t kTypes[] = {{.str = "json", .len = 4}};
  static const ibmw_type_support_base_t ts{
      .type_name = [](void*) { return ibmw::util::ToIbmwStringView("test:Int"); },
      .create = [](void*) -> void* { return new int(0); },
      .destroy = [](void*, void* msg) { delete static_cast<int*>(msg); },
      .copy = [](void*, const void* from, void* to) { *static_cast<int*>(to) = *static_cast<const int*>(from); },
      .move = [](void*, void* from, void* to) { *static_cast<int*>(to) = *static_cast<int*>(from); },
      .serialize = [](void*, ibmw_string_view_t, const void* msg, const ibmw_buffer_array_allocator_t* allocator,
                      ibmw_buffer_array_t* buffer_array) {
        const std::string text = std::to_string(*static_cast<const int*>(msg));
        auto buf = allocator->allocate(allocator->ctx, buffer_array, text.size());
        if (buf.data == nullptr) return false;
        memcpy(buf.data, text.data(), text.size());
        return true;
      },
      .deserialize = [](void*, ibmw_string_view_t, ibmw_buffer_array_view_t, void*) { return false; },
      .serialization_types_supported_num = [](void*) -> size_t { return 1; },
      .serialization_types_supported_list = [](void*) -> const ibmw_string_view_t* { return kTypes; },
      .custom_type_support_ptr = [](void*) -> const void* { return nullptr; },
      .ctx = nullptr};
  return &ts;
}

// A driver on a Paho client that never connects: every publish fails.
class MqttRpcDriverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(MQTTAsync_create(&client_, "tcp://127.0.0.1:1", "ibmw_rpc_test", MQTTCLIENT_PERSISTENCE_NONE, nullptr),
              MQTTASYNC_SUCCESS);
    driver_ = std::make_unique<MqttRpcDriver>("ibmw_rpc_test", client_, 1024 * 1024,
                                              std::make_shared<MsgHandleCatalog>());
    driver_->Initialize(YAML::Node());
    driver_->Start();

    info_.func_name = "pb:/test/svc";
    info_.pkg_path = "pkg";
    info_.module_name = "mod";
    info_.req_type_support_ref = ibmw::util::TypeSupportRef(JsonIntTypeSupport());
    info_.rsp_type_support_ref = ibmw::util::TypeSupportRef(JsonIntTypeSupport());
    ctx_.SetMetaValue(IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE, "json");
  }

  void TearDown() override {
    driver_->Shutdown();
    driver_.reset();
    MQTTAsync_destroy(&client_);
  }

  // Issues one call; returns the received codes and a handle to watch the frame.
  std::shared_ptr<std::vector<uint32_t>> CallOnce(std::weak_ptr<runtime::core::service::ServiceMessageFrame>* weak) {
    auto codes = std::make_shared<std::vector<uint32_t>>();
    auto frame = std::make_shared<runtime::core::service::ServiceMessageFrame>(
        runtime::core::service::ServiceMessageFrame{
            .info = info_, .req_ptr = &req_, .rsp_ptr = &rsp_, .ctx_ref = ibmw::rpc::ContextRef(ctx_)});
    frame->callback = [codes](ibmw::rpc::Status status) { codes->push_back(status.Code()); };
    *weak = frame;
    driver_->Call(frame);
    return codes;
  }

  MQTTAsync client_ = nullptr;
  std::unique_ptr<MqttRpcDriver> driver_;
  runtime::core::service::ServiceInfo info_;
  ibmw::rpc::Context ctx_;
  int req_ = 21;
  int rsp_ = 0;
};

TEST_F(MqttRpcDriverTest, FailedPublishCompletesOnceAndReleasesRecord) {
  std::weak_ptr<runtime::core::service::ServiceMessageFrame> weak;
  auto codes = CallOnce(&weak);
  ASSERT_EQ(codes->size(), 1u);
  EXPECT_EQ((*codes)[0], static_cast<uint32_t>(IBMW_RPC_STATUS_CLI_SEND_REQ_FAILED));
  // Only a lingering record could still keep the frame alive.
  EXPECT_TRUE(weak.expired());
}

TEST_F(MqttRpcDriverTest, CallAfterShutdownFailsThroughCallback) {
  driver_->Shutdown();
  std::weak_ptr<runtime::core::service::ServiceMessageFrame> weak;
  auto codes = CallOnce(&weak);
  ASSERT_EQ(codes->size(), 1u);
  EXPECT_EQ((*codes)[0], static_cast<uint32_t>(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
  EXPECT_TRUE(weak.expired());
}

}  // namespace
}  // namespace ibmw::extensions::mqtt_extension
