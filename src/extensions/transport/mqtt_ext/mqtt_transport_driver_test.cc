// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// TRANS-054: publishing without a context, and the package size option.

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>

#include "cpp/util/string.h"
#include "mqtt_ext/global.h"
#include "mqtt_ext/mqtt_extension.h"
#include "mqtt_ext/mqtt_transport_driver.h"

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

// Keeps what the extension logs at Warn and above.
class LogCapture {
 public:
  LogCapture() {
    base_.get_log_level = [](void*) { return static_cast<ibmw_log_level_t>(ibmw::common::util::kLogLevelWarn); };
    base_.log = [](void* ctx, ibmw_log_level_t, uint32_t, const char*, const char*, const char* data, size_t size) {
      auto* self = static_cast<LogCapture*>(ctx);
      std::lock_guard lock(self->mutex_);
      self->text_.append(data, size).push_back('\n');
    };
    base_.ctx = this;
    g_logger.Set(ibmw::logger::LoggerRef(&base_));
  }
  ~LogCapture() { g_logger.Set(ibmw::logger::LoggerRef()); }
  std::string Text() {
    std::lock_guard lock(mutex_);
    return text_;
  }

 private:
  ibmw_logger_base_t base_{};
  std::mutex mutex_;
  std::string text_;
};

// The context of a message is optional: without one the message still reaches
// the publish call (which fails here, the client never connects).
TEST(MqttTransportDriverTest, MessageWithoutContextIsPublished) {
  MQTTAsync client = nullptr;
  ASSERT_EQ(MQTTAsync_create(&client, "tcp://127.0.0.1:1", "ibmw_transport_test", MQTTCLIENT_PERSISTENCE_NONE,
                             nullptr),
            MQTTASYNC_SUCCESS);
  {
    MqttTransportDriver driver(client, 1024 * 1024, std::make_shared<MsgHandleCatalog>());
    driver.Initialize(YAML::Node());
    runtime::core::transport::SendTypeWrapper wrapper;
    wrapper.info.topic_name = "/t";
    wrapper.info.msg_type = "test:Int";
    wrapper.info.msg_type_support_ref = ibmw::util::TypeSupportRef(JsonIntTypeSupport());
    ASSERT_TRUE(driver.RegisterSendType(wrapper));
    driver.Start();

    LogCapture capture;
    int value = 7;
    runtime::core::transport::MessageFrame frame{.info = wrapper.info, .msg_ptr = &value};
    driver.Send(frame);
    const auto text = capture.Text();
    EXPECT_NE(text.find("publish mqtt msg failed"), std::string::npos) << text;
    EXPECT_EQ(text.find("Reference is null"), std::string::npos) << text;
    driver.Shutdown();
  }
  MQTTAsync_destroy(&client);
}

// Paho takes the payload length as int, and the limit is computed in 32 bits:
// a larger max_pkg_size_k is refused instead of wrapping.
TEST(MqttExtensionOptionsTest, PackageSizeLimitIsBounded) {
  auto options = [](uint64_t size_k) {
    return YAML::Load("{broker_addr: 'tcp://127.0.0.1:1883', client_id: c, max_pkg_size_k: " +
                      std::to_string(size_k) + "}");
  };
  EXPECT_EQ(MqttExtension::ParseOptions(options(MqttExtension::kMaxPkgSizeK)).max_pkg_size_k,
            MqttExtension::kMaxPkgSizeK);
  for (uint64_t size_k : {uint64_t{MqttExtension::kMaxPkgSizeK} + 1, uint64_t{4194304}}) {
    try {
      MqttExtension::ParseOptions(options(size_k));
      ADD_FAILURE() << size_k << " was accepted";
    } catch (const std::exception& e) {
      EXPECT_NE(std::string(e.what()).find("max_pkg_size_k"), std::string::npos) << e.what();
    }
  }
}

}  // namespace
}  // namespace ibmw::extensions::mqtt_extension
