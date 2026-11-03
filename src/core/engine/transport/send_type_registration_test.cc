// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// COMM-022: a publisher that one driver refuses must not stay half registered.

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "cpp/transport/transport_handle.h"
#include "cpp/util/string.h"
#include "engine/transport/transport_access.h"

namespace ibmw::runtime::core::transport {
namespace {

ibmw_type_support_base_t MakeTypeSupport() {
  ibmw_type_support_base_t ts{};
  ts.type_name = [](void*) { return ibmw::util::ToIbmwStringView("T"); };
  ts.serialization_types_supported_num = [](void*) -> size_t { return 0; };
  return ts;
}

const ibmw_type_support_base_t kTs = MakeTypeSupport();

// Refuses publishers of module "bad"; counts sends.
class RecordingDriver : public TransportDriver {
 public:
  explicit RecordingDriver(std::string name, bool refuse_bad) : name_(std::move(name)), refuse_bad_(refuse_bad) {}
  std::string_view Name() const noexcept override { return name_; }
  IBMW_DECLARE_DRIVER_SLOT("recording")

  void Initialize(YAML::Node) override {}
  void Start() override {}
  void Shutdown() override {}
  bool RegisterSendType(const SendTypeWrapper& wrapper) noexcept override {
    if (refuse_bad_ && wrapper.info.module_name == "bad") return false;
    accepted.push_back(&wrapper);
    return true;
  }
  bool OnMessage(const MessageHandlerWrapper&) noexcept override { return true; }
  void Send(MessageFrame&) noexcept override { ++sends; }

  std::vector<const SendTypeWrapper*> accepted;
  int sends = 0;

 private:
  std::string name_;
  bool refuse_bad_;
};

class SendTypeRegistrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dispatcher_.SetTransportCatalog(&catalog_);
    dispatcher_.SetPublishFrameworkAsyncTransportFilterBus(&publish_filters_);
    dispatcher_.SetSubscribeFrameworkAsyncTransportFilterBus(&subscribe_filters_);
    dispatcher_.SetPubTopicsDriversRules({{".*", {"a", "b"}}});
    dispatcher_.RegisterTransportDriver(&a_);
    dispatcher_.RegisterTransportDriver(&b_);
    dispatcher_.Initialize();
  }

  ibmw::transport::SenderRef Sender(std::string_view module) {
    senders_.push_back(std::make_unique<SenderAccess>("pkg", module, "topic", logger_, dispatcher_, passed_keys_));
    return ibmw::transport::SenderRef(senders_.back()->NativeHandle());
  }

  void SendOnce(ibmw::transport::SenderRef sender) {
    ibmw::transport::Context ctx;
    int msg = 0;
    sender.Send("T", ibmw::transport::ContextRef(ctx.NativeHandle()), &msg);
  }

  ibmw::common::util::LoggerWrapper logger_;
  std::unordered_set<std::string> passed_keys_;
  TransportCatalog catalog_;
  FrameworkAsyncTransportFilterBus publish_filters_;
  FrameworkAsyncTransportFilterBus subscribe_filters_;
  TransportDriverDispatcher dispatcher_;
  RecordingDriver a_{"a", false};
  RecordingDriver b_{"b", true};
  std::vector<std::unique_ptr<SenderAccess>> senders_;
};

TEST_F(SendTypeRegistrationTest, RefusedByOneDriverIsDisabledOnAll) {
  auto good = Sender("good");
  auto bad = Sender("bad");
  ASSERT_TRUE(good.RegisterSendType(&kTs));
  EXPECT_FALSE(bad.RegisterSendType(&kTs));

  // The driver that accepted still holds a valid wrapper.
  ASSERT_EQ(a_.accepted.size(), 2u);
  EXPECT_EQ(a_.accepted[1]->info.module_name, "bad");

  EXPECT_EQ(catalog_.GetSendTypeWrapperPtr("T", "topic", "pkg", "bad"), nullptr);
  ASSERT_NE(catalog_.GetSendTypeWrapperPtr("T", "topic", "pkg", "good"), nullptr);
  const auto& by_topic = catalog_.GetPubTopicIndexMap();
  ASSERT_EQ(by_topic.count("topic"), 1u);
  ASSERT_EQ(by_topic.at("topic").size(), 1u);
  EXPECT_EQ(by_topic.at("topic")[0]->info.module_name, "good");

  dispatcher_.Start();
  SendOnce(bad);
  EXPECT_EQ(a_.sends, 0);
  EXPECT_EQ(b_.sends, 0);
  SendOnce(good);
  EXPECT_EQ(a_.sends, 1);
  EXPECT_EQ(b_.sends, 1);
  dispatcher_.Shutdown();
}

TEST_F(SendTypeRegistrationTest, LastPublisherOfTopicRefusedLeavesNoTopicEntry) {
  EXPECT_FALSE(Sender("bad").RegisterSendType(&kTs));
  EXPECT_EQ(catalog_.GetPubTopicIndexMap().count("topic"), 0u);
  EXPECT_TRUE(catalog_.GetSendTypeWrapperMap().empty());
}

// COMM-020: a caller's context is claimed once; a table that cannot claim
// it atomically is rejected; calls without a context always send.
TEST_F(SendTypeRegistrationTest, CallerContextIsClaimedOnce) {
  auto good = Sender("good");
  ASSERT_TRUE(good.RegisterSendType(&kTs));
  dispatcher_.Start();
  int msg = 0;

  ibmw::transport::Context ctx;
  good.Send("T", ibmw::transport::ContextRef(ctx.NativeHandle()), &msg);
  good.Send("T", ibmw::transport::ContextRef(ctx.NativeHandle()), &msg);
  EXPECT_EQ(a_.sends, 1);

  ibmw_transport_context_base_ops_t ops = *ctx.NativeHandle()->ops;
  ops.try_set_used = nullptr;
  ibmw::transport::Context fresh;
  const ibmw_transport_context_base_t no_claim{.ops = &ops, .ctx = &fresh};
  good.Send("T", ibmw::transport::ContextRef(&no_claim), &msg);
  EXPECT_EQ(a_.sends, 1);

  good.Send("T", ibmw::transport::ContextRef(), &msg);
  good.Send("T", ibmw::transport::ContextRef(), &msg);
  EXPECT_EQ(a_.sends, 3);
  dispatcher_.Shutdown();
}

}  // namespace
}  // namespace ibmw::runtime::core::transport
