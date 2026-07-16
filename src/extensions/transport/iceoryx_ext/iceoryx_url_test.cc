// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Unit tests for the iceoryx URL pipeline:
//   1. ibmw::common::util::UrlEncode  (driver-level percent encoding)
//   2. TruncateString                 (per-segment 100-char policy)
//   3. Url2ServiceDescription         (3-segment "/transport/<topic>/<msg>"
//                                      -> iox::capro::ServiceDescription)
//
// These are pure functions and do NOT require a running RouDi daemon,
// so the test is safe to run in CI.

#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "base/url_encode.h"
#include "iceoryx_ext/iceoryx_transport_driver.h"
#include "iceoryx_posh/capro/service_description.hpp"
#include "iceoryx_posh/iceoryx_posh_types.hpp"
#include "yaml-cpp/yaml.h"

namespace ibmw::extensions::iceoryx_extension {

// Forward declarations of the two helpers defined (without header
// declaration) inside iceoryx_bus.cc.  They live in this same namespace
// with external linkage, so the linker resolves them when the gtest
// executable links against the iceoryx_ext sources.
std::string TruncateString(std::string_view input);
iox::capro::ServiceDescription Url2ServiceDescription(std::string_view url);

namespace {

// ---------------------------------------------------------------------------
// UrlEncode  --  driver-level encoding used to build /transport/<topic>/<msg>
// ---------------------------------------------------------------------------

TEST(IceoryxUrlEncode, AlphaNumericIsUntouched) {
  EXPECT_EQ(common::util::UrlEncode("HelloWorld123"), "HelloWorld123");
}

TEST(IceoryxUrlEncode, UnreservedCharactersAreUntouched) {
  // RFC 3986 unreserved set used by the encoder.
  EXPECT_EQ(common::util::UrlEncode("a-b_c.d~e"), "a-b_c.d~e");
}

TEST(IceoryxUrlEncode, SpaceBecomesPlus) {
  EXPECT_EQ(common::util::UrlEncode("hello world"), "hello+world");
}

TEST(IceoryxUrlEncode, SlashIsPercentEncoded) {
  // The encoder is invoked exactly to keep the topic / msg_type from
  // injecting extra '/' separators into the iceoryx URL.
  EXPECT_EQ(common::util::UrlEncode("a/b"), "a%2Fb");
}

TEST(IceoryxUrlEncode, NamespacedRosTypeStaysCollapsible) {
  // Real-world IBMW msg_type strings carry "::" which must be encoded so
  // that Url2ServiceDescription only sees three '/' separators.
  const auto encoded = common::util::UrlEncode("iceoryx_demo::CompressedImage");
  EXPECT_EQ(encoded.find('/'), std::string::npos);
  // ':' is not in the unreserved set -> percent encoded.
  EXPECT_NE(encoded.find('%'), std::string::npos);
}

// ---------------------------------------------------------------------------
// TruncateString  --  enforce iceoryx 100-char per-segment limit
// ---------------------------------------------------------------------------

TEST(IceoryxTruncateString, ShortStringIsKeptVerbatim) {
  const std::string s = "short";
  EXPECT_EQ(TruncateString(s), s);
}

TEST(IceoryxTruncateString, BoundaryLengthIsKeptVerbatim) {
  const std::string s(iox::MAX_RUNTIME_NAME_LENGTH, 'x');
  EXPECT_EQ(TruncateString(s).size(), iox::MAX_RUNTIME_NAME_LENGTH);
  EXPECT_EQ(TruncateString(s), s);
}

TEST(IceoryxTruncateString, OversizedStringIsTruncatedWithEllipsis) {
  const std::string s(iox::MAX_RUNTIME_NAME_LENGTH * 2, 'a');
  const std::string out = TruncateString(s);

  EXPECT_EQ(out.size(), iox::MAX_RUNTIME_NAME_LENGTH);
  EXPECT_NE(out.find("..."), std::string::npos);
}

TEST(IceoryxTruncateString, TruncationPreservesPrefixAndSuffix) {
  // Build a string whose head and tail are distinguishable.
  const std::string head(iox::MAX_RUNTIME_NAME_LENGTH, 'H');
  const std::string tail(iox::MAX_RUNTIME_NAME_LENGTH, 'T');
  const std::string s = head + tail;
  const std::string out = TruncateString(s);

  EXPECT_EQ(out.size(), iox::MAX_RUNTIME_NAME_LENGTH);
  EXPECT_EQ(out.front(), 'H');
  EXPECT_EQ(out.back(), 'T');
  EXPECT_NE(out.find("..."), std::string::npos);
}

// ---------------------------------------------------------------------------
// Url2ServiceDescription  --  /transport/<topic>/<msg> -> 3-tuple
// ---------------------------------------------------------------------------

TEST(IceoryxUrl2ServiceDescription, SplitsThreeSegments) {
  const auto desc =
      Url2ServiceDescription("/transport/test_topic/test_msg_type");

  // Each segment is an iox::capro::IdString_t (cxx::string<100>).
  // The first segment is everything up to (and excluding) the second '/',
  // i.e. "/transport".
  EXPECT_STREQ(desc.getServiceIDString().c_str(), "/transport");
  EXPECT_STREQ(desc.getInstanceIDString().c_str(), "/test_topic");
  EXPECT_STREQ(desc.getEventIDString().c_str(), "/test_msg_type");
}

TEST(IceoryxUrl2ServiceDescription, HandlesPercentEncodedSegments) {
  // What the driver actually produces for a namespaced ROS-style msg_type.
  const std::string topic = ibmw::common::util::UrlEncode("iceoryx_demo/raw");
  const std::string msg =
      ibmw::common::util::UrlEncode("iceoryx_demo::CompressedImage");
  const std::string url = "/transport/" + topic + "/" + msg;

  const auto desc = Url2ServiceDescription(url);
  EXPECT_STREQ(desc.getServiceIDString().c_str(), "/transport");
  // Instance and event must contain *no* raw '/' or ':' since the encoder
  // already percent-escaped them.
  const std::string instance = desc.getInstanceIDString().c_str();
  const std::string event = desc.getEventIDString().c_str();
  EXPECT_EQ(std::count(instance.begin() + 1, instance.end(), '/'), 0);
  EXPECT_EQ(event.find(':'), std::string::npos);
}

TEST(IceoryxUrl2ServiceDescription, OversizedSegmentsAreTruncatedNotRejected) {
  // Build a topic whose encoded form blows past the 100-char per-segment
  // limit and make sure Url2ServiceDescription still returns a usable
  // ServiceDescription rather than throwing.
  const std::string huge_topic(iox::MAX_RUNTIME_NAME_LENGTH * 2, 'T');
  const std::string url = "/transport/" + huge_topic + "/MsgType";

  const auto desc = Url2ServiceDescription(url);
  EXPECT_STREQ(desc.getServiceIDString().c_str(), "/transport");
  EXPECT_LE(std::string(desc.getInstanceIDString().c_str()).size(),
            iox::MAX_RUNTIME_NAME_LENGTH);
  EXPECT_STREQ(desc.getEventIDString().c_str(), "/MsgType");
}

// ---------------------------------------------------------------------------
// IceoryxTransportDriver options  --  unsupported callback-executor boundary
// ---------------------------------------------------------------------------

TEST(IceoryxTransportDriverOptions, AllowsListenerThreadOptionsAndLegacyIgnoredFields) {
  IceoryxBus bus;
  IceoryxTransportDriver driver(bus);

  YAML::Node options = YAML::Load(R"yaml(
listener_thread_name: iox_listener
listener_thread_sched_policy: SCHED_OTHER
listener_thread_bind_cpu: [0]
shm_init_size: 4096
runtime_id: iox_legacy_runtime
)yaml");

  EXPECT_NO_THROW(driver.Initialize(options));
}

TEST(IceoryxTransportDriverOptions, RejectsExecutorOption) {
  IceoryxBus bus;
  IceoryxTransportDriver driver(bus);

  YAML::Node options = YAML::Load(R"yaml(
listener_thread_name: iox_listener
executor: work_thread_pool
)yaml");

  EXPECT_THROW(driver.Initialize(options), std::exception);
}

TEST(IceoryxTransportDriverOptions, RejectsCallbackExecutorOption) {
  IceoryxBus bus;
  IceoryxTransportDriver driver(bus);

  YAML::Node options = YAML::Load(R"yaml(
callback_executor: work_thread_pool
)yaml");

  EXPECT_THROW(driver.Initialize(options), std::exception);
}

TEST(IceoryxTransportDriverOptions, RejectsDdsStyleSubscribersOption) {
  IceoryxBus bus;
  IceoryxTransportDriver driver(bus);

  YAML::Node options = YAML::Load(R"yaml(
subscribers:
  - topic: iox_topic
    executor: work_thread_pool
)yaml");

  EXPECT_THROW(driver.Initialize(options), std::exception);
}

}  // namespace
}  // namespace ibmw::extensions::iceoryx_extension
