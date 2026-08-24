// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <cstdlib>
#include <limits>
#include <string>

#include <yaml-cpp/yaml.h>

#include "dds_ext/dds_transport_driver.h"

namespace ibmw::extensions::dds_extension {

class DdsQosTest : public ::testing::Test {
 protected:
  DdsQosTest() : bus_(31, "ibmw_dds_qos_test"), driver_(bus_) {}
  void SetUp() override { bus_.Initialize(); }

  void Configure(const char* yaml) { driver_.Initialize(YAML::Load(yaml)); }
  auto WriterQos() const { return driver_.GetWriterQos("topic", "topic"); }
  auto ReaderQos() const { return driver_.GetReaderQos("topic", "topic"); }

  DdsBus bus_;
  DdsTransportDriver driver_;
};

class ScopedExtraSamplesEnv {
 public:
  explicit ScopedExtraSamplesEnv(const char* value) {
    const char* previous = std::getenv("IBMW_DDS_RELIABLE_EXTRA_SAMPLES");
    if (previous != nullptr) {
      previous_ = previous;
      had_previous_ = true;
    }
    if (value == nullptr) {
      unsetenv("IBMW_DDS_RELIABLE_EXTRA_SAMPLES");
    } else {
      setenv("IBMW_DDS_RELIABLE_EXTRA_SAMPLES", value, 1);
    }
  }

  ~ScopedExtraSamplesEnv() {
    if (had_previous_) {
      setenv("IBMW_DDS_RELIABLE_EXTRA_SAMPLES", previous_.c_str(), 1);
    } else {
      unsetenv("IBMW_DDS_RELIABLE_EXTRA_SAMPLES");
    }
  }

 private:
  bool had_previous_ = false;
  std::string previous_;
};

namespace {

TEST_F(DdsQosTest, ExplicitHeadroomAppliesToReliableKeepLastWriterOnly) {
  ScopedExtraSamplesEnv env("23");
  Configure(R"yaml(
publishers:
  - topic: topic
    qos:
      history: keep_last
      depth: 7
      reliability: reliable
 )yaml");

  ASSERT_EQ(driver_.GetOptions().pub_topics_options.size(), 1u);
  EXPECT_EQ(WriterQos().resource_limits().extra_samples, 23);
}

TEST_F(DdsQosTest, DefaultHeadroomUsesSafeWriterValueAndReaderIsUnchanged) {
  ScopedExtraSamplesEnv env(nullptr);
  Configure(R"yaml(
publishers:
  - topic: topic
    qos: {history: keep_last, depth: 4, reliability: reliable}
subscribers:
  - topic: topic
    qos: {history: keep_last, depth: 4, reliability: reliable}
)yaml");

  const auto writer = WriterQos();
  const auto reader = ReaderQos();
  const eprosima::fastdds::dds::DataReaderQos default_reader;
  EXPECT_EQ(writer.resource_limits().extra_samples, 100);
  EXPECT_EQ(reader.resource_limits().extra_samples,
            default_reader.resource_limits().extra_samples);
}

TEST_F(DdsQosTest, BestEffortWriterDoesNotForceHeadroom) {
  ScopedExtraSamplesEnv env("37");
  Configure(R"yaml(
publishers:
  - topic: topic
    qos: {history: keep_last, reliability: best_effort}
)yaml");

  const auto qos = WriterQos();
  const eprosima::fastdds::dds::DataWriterQos default_writer;
  EXPECT_EQ(qos.reliability().kind, eprosima::fastdds::dds::BEST_EFFORT_RELIABILITY_QOS);
  EXPECT_EQ(qos.resource_limits().extra_samples,
            default_writer.resource_limits().extra_samples);
}

TEST_F(DdsQosTest, KeepAllWriterDoesNotForceHeadroom) {
  ScopedExtraSamplesEnv env("41");
  Configure(R"yaml(
publishers:
  - topic: topic
    qos: {history: keep_all, reliability: reliable}
)yaml");

  const auto qos = WriterQos();
  const eprosima::fastdds::dds::DataWriterQos default_writer;
  EXPECT_EQ(qos.history().kind, eprosima::fastdds::dds::KEEP_ALL_HISTORY_QOS);
  EXPECT_EQ(qos.resource_limits().extra_samples,
            default_writer.resource_limits().extra_samples);
}

TEST_F(DdsQosTest, MinusOnePreservesFastDdsDefault) {
  ScopedExtraSamplesEnv env("-1");
  Configure(R"yaml(
publishers:
  - topic: topic
    qos: {history: keep_last, reliability: reliable}
)yaml");

  const eprosima::fastdds::dds::DataWriterQos default_writer;
  EXPECT_EQ(WriterQos().resource_limits().extra_samples,
            default_writer.resource_limits().extra_samples);
}

TEST_F(DdsQosTest, InvalidHeadroomUsesSafeDefault) {
  ScopedExtraSamplesEnv env("-2");
  Configure(R"yaml(
publishers:
  - topic: topic
    qos: {history: keep_last, reliability: reliable}
)yaml");

  EXPECT_EQ(WriterQos().resource_limits().extra_samples, 100);
}

TEST_F(DdsQosTest, ZeroHeadroomIsAccepted) {
  ScopedExtraSamplesEnv env("0");
  Configure(R"yaml(
publishers:
  - topic: topic
    qos: {history: keep_last, reliability: reliable}
)yaml");

  EXPECT_EQ(WriterQos().resource_limits().extra_samples, 0);
}

TEST_F(DdsQosTest, MaximumHeadroomIsAccepted) {
  ScopedExtraSamplesEnv env("2147483647");
  Configure(R"yaml(
publishers:
  - topic: topic
    qos: {history: keep_last, reliability: reliable}
)yaml");

  EXPECT_EQ(WriterQos().resource_limits().extra_samples,
            std::numeric_limits<int32_t>::max());
}

TEST_F(DdsQosTest, MalformedHeadroomUsesSafeDefault) {
  ScopedExtraSamplesEnv env("100samples");
  Configure(R"yaml(
publishers:
  - topic: topic
    qos: {history: keep_last, reliability: reliable}
)yaml");

  EXPECT_EQ(WriterQos().resource_limits().extra_samples, 100);
}

}  // namespace
}  // namespace ibmw::extensions::dds_extension
