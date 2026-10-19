// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "dds_ext/detail/dds_qos_descriptor.h"

#include <fastdds/dds/publisher/qos/DataWriterQos.hpp>

#include "gtest/gtest.h"

namespace ibmw::extensions::dds_extension {
namespace {

using ibmw::runtime::core::transport::QosDescriptor;

TEST(DdsQosDescriptorTest, UnknownPoliciesAndZeroDepthPreserveDefaults) {
  eprosima::fastdds::dds::DataWriterQos qos;
  qos.history().kind = eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS;
  qos.history().depth = 10;
  qos.reliability().kind =
      eprosima::fastdds::dds::BEST_EFFORT_RELIABILITY_QOS;
  qos.durability().kind =
      eprosima::fastdds::dds::TRANSIENT_LOCAL_DURABILITY_QOS;

  detail::ApplyQosDescriptor(
      QosDescriptor{.policies =
                        {{"history", "unknown"},
                         {"depth", "0"},
                         {"reliability", "unknown"},
                         {"durability", "system_default"}}},
      qos);

  EXPECT_EQ(qos.history().kind,
            eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS);
  EXPECT_EQ(qos.history().depth, 10);
  EXPECT_EQ(qos.reliability().kind,
            eprosima::fastdds::dds::BEST_EFFORT_RELIABILITY_QOS);
  EXPECT_EQ(qos.durability().kind,
            eprosima::fastdds::dds::TRANSIENT_LOCAL_DURABILITY_QOS);
}

TEST(DdsQosDescriptorTest, RecognizedPoliciesOverrideDefaults) {
  eprosima::fastdds::dds::DataWriterQos qos;
  qos.reliability().kind =
      eprosima::fastdds::dds::BEST_EFFORT_RELIABILITY_QOS;
  qos.durability().kind =
      eprosima::fastdds::dds::TRANSIENT_LOCAL_DURABILITY_QOS;

  detail::ApplyQosDescriptor(
      QosDescriptor{.policies =
                        {{"history", "keep_last"},
                         {"depth", "17"},
                         {"reliability", "reliable"},
                         {"durability", "volatile"}}},
      qos);

  EXPECT_EQ(qos.history().kind,
            eprosima::fastdds::dds::KEEP_LAST_HISTORY_QOS);
  EXPECT_EQ(qos.history().depth, 17);
  EXPECT_EQ(qos.reliability().kind,
            eprosima::fastdds::dds::RELIABLE_RELIABILITY_QOS);
  EXPECT_EQ(qos.durability().kind,
            eprosima::fastdds::dds::VOLATILE_DURABILITY_QOS);
}

TEST(DdsQosDescriptorTest, MalformedDepthDoesNotThrowOrChangeQos) {
  eprosima::fastdds::dds::DataWriterQos qos;
  qos.history().depth = 7;

  EXPECT_NO_THROW(detail::ApplyQosDescriptor(
      QosDescriptor{.policies = {{"depth", "not-a-number"}}}, qos));
  EXPECT_EQ(qos.history().depth, 7);
}

TEST(DdsQosDescriptorTest, DiscoveryOwnershipAndLivelinessOverrideReaderDefaults) {
  eprosima::fastdds::dds::DataWriterQos qos;
  qos.liveliness().kind = eprosima::fastdds::dds::MANUAL_BY_TOPIC_LIVELINESS_QOS;
  detail::ApplyQosDescriptor(
      QosDescriptor{.policies = {{"ownership", "exclusive"}, {"liveliness", "automatic"}}}, qos);
  EXPECT_EQ(qos.ownership().kind, eprosima::fastdds::dds::EXCLUSIVE_OWNERSHIP_QOS);
  EXPECT_EQ(qos.liveliness().kind, eprosima::fastdds::dds::AUTOMATIC_LIVELINESS_QOS);
}

}  // namespace
}  // namespace ibmw::extensions::dds_extension
