// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression: the heap fallback of BorrowLoanedMessage() (AUTO with a
// non-plain type, COMPAT) must keep the DataWriter so SendLoaned() can
// publish it by copy. Previously the fallback carried writer == nullptr and
// every fallback publish returned false (loaned_message_demo multi-process
// subscriber received nothing).

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include <fastdds/dds/domain/DomainParticipant.hpp>
#include <fastdds/dds/domain/DomainParticipantFactory.hpp>
#include <fastdds/dds/publisher/Publisher.hpp>
#include <fastdds/dds/topic/Topic.hpp>
#include <fastdds/dds/topic/TypeSupport.hpp>

#include "dds_ext/loaned_message_impl.h"
#include "dds_ext/ros2_style_publisher.h"
#include "dds/plain_dds_pubsub_type.h"

namespace ibmw::extensions::dds_extension {
namespace {

namespace fdds = eprosima::fastdds::dds;

struct LoanTestPayload {
  std::int32_t value = 0;
};

class LoanedMessageFallbackTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto* factory = fdds::DomainParticipantFactory::get_instance();
    participant_ = factory->create_participant(kDomainId, fdds::PARTICIPANT_QOS_DEFAULT);
    ASSERT_NE(participant_, nullptr);
    fdds::TypeSupport type(
        new ibmw::dds::PlainDdsPubSubType<LoanTestPayload>("ibmw_test::LoanTestPayload"));
    type.register_type(participant_);
    topic_ = participant_->create_topic(
        "ibmw_loaned_message_fallback_test", type.get_type_name(), fdds::TOPIC_QOS_DEFAULT);
    ASSERT_NE(topic_, nullptr);
    publisher_ = participant_->create_publisher(fdds::PUBLISHER_QOS_DEFAULT);
    ASSERT_NE(publisher_, nullptr);
    writer_ = publisher_->create_datawriter(topic_, fdds::DATAWRITER_QOS_DEFAULT);
    ASSERT_NE(writer_, nullptr);
  }

  void TearDown() override {
    if (participant_ != nullptr) {
      participant_->delete_contained_entities();
      fdds::DomainParticipantFactory::get_instance()->delete_participant(participant_);
    }
  }

  static constexpr fdds::DomainId_t kDomainId = 87;
  fdds::DomainParticipant* participant_ = nullptr;
  fdds::Topic* topic_ = nullptr;
  fdds::Publisher* publisher_ = nullptr;
  fdds::DataWriter* writer_ = nullptr;
};

TEST_F(LoanedMessageFallbackTest, AutoNonPlainFallbackPublishesThroughWriter) {
  auto msg = BorrowLoanedMessage<LoanTestPayload>(
      writer_, "ibmw_loaned_message_fallback_test", /*is_plain=*/false,
      ZeroCopyConfig::Auto());
  ASSERT_TRUE(msg.is_valid());
  EXPECT_FALSE(msg.is_zero_copy());
  EXPECT_EQ(msg.GetWriter(), writer_);
  msg->value = 42;
  EXPECT_TRUE(SendLoaned(std::move(msg)));
}

TEST_F(LoanedMessageFallbackTest, CompatFallbackPublishesThroughWriter) {
  auto msg = BorrowLoanedMessage<LoanTestPayload>(
      writer_, "ibmw_loaned_message_fallback_test", /*is_plain=*/true,
      ZeroCopyConfig::Compat());
  ASSERT_TRUE(msg.is_valid());
  EXPECT_FALSE(msg.is_zero_copy());
  EXPECT_EQ(msg.GetWriter(), writer_);
  msg->value = 7;
  EXPECT_TRUE(SendLoaned(std::move(msg)));
}

TEST_F(LoanedMessageFallbackTest, StrictNonPlainStillRefuses) {
  auto msg = BorrowLoanedMessage<LoanTestPayload>(
      writer_, "ibmw_loaned_message_fallback_test", /*is_plain=*/false,
      ZeroCopyConfig::Strict());
  EXPECT_FALSE(msg.is_valid());
  EXPECT_FALSE(SendLoaned(std::move(msg)));
}

// Fast DDS 3 write() returns RETCODE_OK (0) on success; reading it as a bool
// turned every successful publish into a failure.
TEST_F(LoanedMessageFallbackTest, PlainLoanPublishesThroughWriter) {
  auto msg = BorrowLoanedMessage<LoanTestPayload>(
      writer_, "ibmw_loaned_message_fallback_test", /*is_plain=*/true,
      ZeroCopyConfig::Auto());
  ASSERT_TRUE(msg.is_valid());
  msg->value = 9;
  EXPECT_TRUE(SendLoaned(std::move(msg)));
}

TEST_F(LoanedMessageFallbackTest, Ros2StylePublisherReportsSuccessfulWrites) {
  Ros2StylePublisher<LoanTestPayload> publisher(
      writer_, "ibmw_loaned_message_fallback_test", /*is_plain=*/false);
  const LoanTestPayload copy{3};
  EXPECT_TRUE(publisher.publish(copy));
  EXPECT_TRUE(publisher.publish(LoanTestPayload{5}));
  EXPECT_TRUE(publisher.publish(publisher.borrow_loaned_message()));
}

TEST(LoanedMessageFallbackNoWriterTest, NullWriterFallbackDoesNotPublish) {
  auto msg = BorrowLoanedMessage<LoanTestPayload>(
      nullptr, "ibmw_loaned_message_fallback_test", /*is_plain=*/true,
      ZeroCopyConfig::Auto());
  ASSERT_TRUE(msg.is_valid());
  EXPECT_EQ(msg.GetWriter(), nullptr);
  EXPECT_FALSE(SendLoaned(std::move(msg)));
}

}  // namespace
}  // namespace ibmw::extensions::dds_extension
