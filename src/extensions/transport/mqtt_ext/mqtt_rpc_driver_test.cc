// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for the MQTT RPC client path: every failure reaches the
// client callback exactly once and releases the call record, also after
// Shutdown (the client tool lives until the driver is destroyed).
// TRANS-004: answer topics of incoming requests are validated.

#include <gtest/gtest.h>

#include <cstdlib>
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
      .deserialize = [](void*, ibmw_string_view_t, ibmw_buffer_array_view_t buffers, void* msg) {
        std::string text;
        for (size_t i = 0; i < buffers.len; ++i)
          text.append(static_cast<const char*>(buffers.data[i].data), buffers.data[i].len);
        if (text.empty()) return false;
        *static_cast<int*>(msg) = std::stoi(text);
        return true;
      },
      .serialization_types_supported_num = [](void*) -> size_t { return 1; },
      .serialization_types_supported_list = [](void*) -> const ibmw_string_view_t* { return kTypes; },
      .custom_type_support_ptr = [](void*) -> const void* { return nullptr; },
      .ctx = nullptr};
  return &ts;
}

// A driver on a Paho client that never connects: every publish fails.
class MqttRpcDriverTest : public ::testing::Test {
 protected:
  void SetUp() override { Init(false); }

  // buffer_offline: publishes are queued by Paho, so calls stay pending.
  void Init(bool buffer_offline) {
    MQTTAsync_createOptions create_opts = MQTTAsync_createOptions_initializer;
    create_opts.sendWhileDisconnected = buffer_offline ? 1 : 0;
    ASSERT_EQ(MQTTAsync_createWithOptions(&client_, "tcp://127.0.0.1:1", "ibmw_rpc_test", MQTTCLIENT_PERSISTENCE_NONE,
                                          nullptr, &create_opts),
              MQTTASYNC_SUCCESS);
    if (buffer_offline) {
      // Paho queues offline publishes only once a connection is wanted.
      MQTTAsync_connectOptions conn_opts = MQTTAsync_connectOptions_initializer;
      conn_opts.automaticReconnect = 1;
      conn_opts.minRetryInterval = 60;
      conn_opts.maxRetryInterval = 60;
      ASSERT_EQ(MQTTAsync_connect(client_, &conn_opts), MQTTASYNC_SUCCESS);
    }
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
    if (driver_) driver_->Shutdown();
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

class MqttRpcDriverPendingTest : public MqttRpcDriverTest {
 protected:
  void SetUp() override { Init(true); }
};

// A call still waiting when the driver goes away gets exactly one result.
TEST_F(MqttRpcDriverPendingTest, DestructorCompletesPendingCallOnce) {
  std::weak_ptr<runtime::core::service::ServiceMessageFrame> weak;
  auto codes = CallOnce(&weak);
  ASSERT_TRUE(codes->empty()) << "publish was expected to be queued";
  driver_->Shutdown();
  EXPECT_TRUE(codes->empty());
  driver_.reset();
  ASSERT_EQ(codes->size(), 1u);
  EXPECT_EQ((*codes)[0], static_cast<uint32_t>(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
  EXPECT_TRUE(weak.expired());
}

// Registrations after the catalog is sealed are rejected.
TEST_F(MqttRpcDriverTest, RegistrationAfterSealIsRejected) {
  auto catalog = std::make_shared<MsgHandleCatalog>();
  MqttRpcDriver driver("ibmw_rpc_test", client_, 1024 * 1024, catalog);
  driver.Initialize(YAML::Node());
  catalog->Seal();
  runtime::core::service::ClientFuncWrapper client;
  client.info = info_;
  EXPECT_FALSE(driver.RegisterClientFunc(client));
  runtime::core::service::ServiceFuncWrapper service;
  service.info = info_;
  service.service_func = [](const std::shared_ptr<runtime::core::service::ServiceMessageFrame>&) {};
  EXPECT_FALSE(driver.RegisterServiceFunc(service));
}

// A request as a client sends it: [serialization][answer topic][req id][no meta][body].
MQTTAsync_message* NewRequest(const std::string& answer_topic, const std::string& body) {
  std::string payload;
  payload += static_cast<char>(4);
  payload += "json";
  payload += static_cast<char>(answer_topic.size());
  payload += answer_topic;
  payload.append(4, '\0');  // req id
  payload += '\0';          // meta count
  payload += body;
  auto* msg = static_cast<MQTTAsync_message*>(MQTTAsync_malloc(sizeof(MQTTAsync_message)));
  const MQTTAsync_message init = MQTTAsync_message_initializer;
  std::memcpy(msg, &init, sizeof(init));
  msg->payload = MQTTAsync_malloc(payload.size());
  std::memcpy(msg->payload, payload.data(), payload.size());
  msg->payloadlen = static_cast<int>(payload.size());
  return msg;
}

// TRANS-004: the server answers only on client answer topics, never on a
// topic chosen by the peer (a data or request topic of someone else).
TEST_F(MqttRpcDriverTest, RequestWithForeignAnswerTopicIsDropped) {
  auto catalog = std::make_shared<MsgHandleCatalog>();
  MqttRpcDriver driver("ibmw_rpc_test", client_, 1024 * 1024, catalog);
  driver.Initialize(YAML::Node());
  std::vector<int> served;
  runtime::core::service::ServiceFuncWrapper service;
  service.info = info_;
  service.service_func = [&served](const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& frame) {
    served.push_back(*static_cast<const int*>(frame->req_ptr));
    // Takes the answer callback like the service dispatcher does; it holds
    // the frame, so leaving it in the frame would keep both alive.
    auto callback = std::move(frame->callback);
  };
  ASSERT_TRUE(driver.RegisterServiceFunc(service));
  driver.Start();

  auto deliver = [&](const std::string& answer_topic, const std::string& body) {
    const std::string topic = "ibmw_rpc_req/%2Ftest%2Fsvc";
    char* raw = static_cast<char*>(MQTTAsync_malloc(topic.size() + 1));
    std::memcpy(raw, topic.c_str(), topic.size() + 1);
    catalog->HandleServerMsg(raw, raw, NewRequest(answer_topic, body));
  };
  deliver("sensors/temperature", "1");
  deliver("ibmw_rpc_req/%2Ftest%2Fsvc", "2");
  deliver("ibmw_rpc_rsp/cli/%2Ftest%2Fsvc", "3");
  EXPECT_EQ(served, std::vector<int>{3});
  driver.Shutdown();
}

}  // namespace
}  // namespace ibmw::extensions::mqtt_extension
