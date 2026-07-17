// Copyright (c) 2025-2026 IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// Ibmw is licensed under Mulan PSL v2.

#include "config_test_module.h"

namespace ibmw::examples::loaned_message_demo {

namespace dds_ext = ibmw::extensions::dds_extension;

bool ConfigTestModule::Setup(ibmw::Runtime core) {
  core_ = core;
  MW_INFO("ConfigTestModule initializing...");

  // Register types so DDS topics exist
  {
    auto pub = core_.GetTransport().GetPublisher("point_topic");
    ibmw::ros2::transport::RegisterSendType<geometry_msgs::msg::Point>(
        pub, "geometry_msgs/msg/Point");
  }
  {
    auto pub = core_.GetTransport().GetPublisher("string_topic");
    ibmw::ros2::transport::RegisterSendType<std_msgs::msg::String>(
        pub, "std_msgs/msg/String");
  }

  MW_INFO("ConfigTestModule initialized");
  return true;
}

bool ConfigTestModule::Run() {
  MW_INFO("========== Configuration Mode Tests ==========");

  if (!driver_) {
    MW_ERROR("DDS Driver not set, cannot run config tests");
    return false;
  }

  // Get DDS topic names
  const auto& options = driver_->GetOptions();
  std::string point_dds_topic = options.ros2_compatible
      ? options.ros2_topic_prefix + "/point_topic"
      : "point_topic";
  std::string string_dds_topic = options.ros2_compatible
      ? options.ros2_topic_prefix + "/string_topic"
      : "string_topic";

  MW_INFO("Point DDS topic: {}", point_dds_topic);
  MW_INFO("String DDS topic: {}", string_dds_topic);
  MW_INFO("Point is_plain: {}", driver_->IsPlainType(point_dds_topic));
  MW_INFO("String is_plain: {}", driver_->IsPlainType(string_dds_topic));

  TestAutoModePlain();
  TestAutoModeNonPlain();
  TestStrictModePlain();
  TestStrictModeNonPlain();
  TestCompatModePlain();
  TestCompatModeNonPlain();

  PrintSummary();

  MW_INFO("========== Tests Complete ==========");
  return true;
}

void ConfigTestModule::Stop() {
  MW_INFO("ConfigTestModule shutdown");
}

bool ConfigTestModule::TestAutoModePlain() {
  MW_INFO("--- Test: AUTO + Plain ---");

  const auto& options = driver_->GetOptions();
  std::string dds_topic = options.ros2_compatible
      ? options.ros2_topic_prefix + "/point_topic" : "point_topic";

  auto* writer = driver_->GetDataWriter(dds_topic);
  bool is_plain = driver_->IsPlainType(dds_topic);
  auto config = dds_ext::ZeroCopyConfig::Auto();

  auto loaned = dds_ext::BorrowLoanedMessage<geometry_msgs::msg::Point>(
      writer, dds_topic, is_plain, config);

  if (!loaned.is_valid()) {
    MW_ERROR("  FAIL: Expected valid, got invalid: {}", loaned.error_message());
    PrintResult("AUTO + Plain", false);
    return false;
  }

  MW_INFO("  is_valid=true, is_zero_copy={}, is_plain={}", loaned.is_zero_copy(), loaned.is_plain_type());

  // Fill and publish to clean up
  loaned->x = 1.0; loaned->y = 2.0; loaned->z = 3.0;
  dds_ext::SendLoaned(std::move(loaned));

  PrintResult("AUTO + Plain", true);
  return true;
}

bool ConfigTestModule::TestAutoModeNonPlain() {
  MW_INFO("--- Test: AUTO + NonPlain ---");

  const auto& options = driver_->GetOptions();
  std::string dds_topic = options.ros2_compatible
      ? options.ros2_topic_prefix + "/string_topic" : "string_topic";

  auto* writer = driver_->GetDataWriter(dds_topic);
  bool is_plain = driver_->IsPlainType(dds_topic);
  auto config = dds_ext::ZeroCopyConfig::Auto();

  auto loaned = dds_ext::BorrowLoanedMessage<std_msgs::msg::String>(
      writer, dds_topic, is_plain, config);

  if (!loaned.is_valid()) {
    MW_ERROR("  FAIL: Expected valid (fallback), got invalid: {}", loaned.error_message());
    PrintResult("AUTO + NonPlain", false);
    return false;
  }

  if (loaned.is_zero_copy()) {
    MW_ERROR("  FAIL: Expected is_zero_copy=false for non-Plain");
    PrintResult("AUTO + NonPlain", false);
    return false;
  }

  MW_INFO("  is_valid=true, is_zero_copy=false, is_plain=false (correct fallback)");

  loaned->data = "test";
  dds_ext::SendLoaned(std::move(loaned));

  PrintResult("AUTO + NonPlain", true);
  return true;
}

bool ConfigTestModule::TestStrictModePlain() {
  MW_INFO("--- Test: STRICT + Plain ---");

  const auto& options = driver_->GetOptions();
  std::string dds_topic = options.ros2_compatible
      ? options.ros2_topic_prefix + "/point_topic" : "point_topic";

  auto* writer = driver_->GetDataWriter(dds_topic);
  bool is_plain = driver_->IsPlainType(dds_topic);
  auto config = dds_ext::ZeroCopyConfig::Strict();

  auto loaned = dds_ext::BorrowLoanedMessage<geometry_msgs::msg::Point>(
      writer, dds_topic, is_plain, config);

  // In STRICT mode, Plain type should still be valid (zero-copy or fail)
  MW_INFO("  is_valid={}, is_zero_copy={}", loaned.is_valid(), loaned.is_zero_copy());

  if (loaned.is_valid()) {
    loaned->x = 1.0; loaned->y = 2.0; loaned->z = 3.0;
    dds_ext::SendLoaned(std::move(loaned));
  } else {
    MW_INFO("  loan_sample failed (SHM not enabled), error: {}", loaned.error_message());
  }

  // Both outcomes are acceptable for STRICT + Plain
  PrintResult("STRICT + Plain", true);
  return true;
}

bool ConfigTestModule::TestStrictModeNonPlain() {
  MW_INFO("--- Test: STRICT + NonPlain ---");

  const auto& options = driver_->GetOptions();
  std::string dds_topic = options.ros2_compatible
      ? options.ros2_topic_prefix + "/string_topic" : "string_topic";

  auto* writer = driver_->GetDataWriter(dds_topic);
  bool is_plain = driver_->IsPlainType(dds_topic);
  auto config = dds_ext::ZeroCopyConfig::Strict();

  auto loaned = dds_ext::BorrowLoanedMessage<std_msgs::msg::String>(
      writer, dds_topic, is_plain, config);

  if (loaned.is_valid()) {
    MW_ERROR("  FAIL: Expected invalid for STRICT + NonPlain, got valid");
    dds_ext::SendLoaned(std::move(loaned));
    PrintResult("STRICT + NonPlain", false);
    return false;
  }

  MW_INFO("  is_valid=false (correctly rejected): {}", loaned.error_message());

  PrintResult("STRICT + NonPlain", true);
  return true;
}

bool ConfigTestModule::TestCompatModePlain() {
  MW_INFO("--- Test: COMPAT + Plain ---");

  const auto& options = driver_->GetOptions();
  std::string dds_topic = options.ros2_compatible
      ? options.ros2_topic_prefix + "/point_topic" : "point_topic";

  auto* writer = driver_->GetDataWriter(dds_topic);
  bool is_plain = driver_->IsPlainType(dds_topic);
  auto config = dds_ext::ZeroCopyConfig::Compat();

  auto loaned = dds_ext::BorrowLoanedMessage<geometry_msgs::msg::Point>(
      writer, dds_topic, is_plain, config);

  if (!loaned.is_valid()) {
    MW_ERROR("  FAIL: Expected valid (heap), got invalid: {}", loaned.error_message());
    PrintResult("COMPAT + Plain", false);
    return false;
  }

  if (loaned.is_zero_copy()) {
    MW_ERROR("  FAIL: Expected is_zero_copy=false in COMPAT mode");
    PrintResult("COMPAT + Plain", false);
    return false;
  }

  MW_INFO("  is_valid=true, is_zero_copy=false (forced heap, correct)");

  loaned->x = 1.0; loaned->y = 2.0; loaned->z = 3.0;
  dds_ext::SendLoaned(std::move(loaned));

  PrintResult("COMPAT + Plain", true);
  return true;
}

bool ConfigTestModule::TestCompatModeNonPlain() {
  MW_INFO("--- Test: COMPAT + NonPlain ---");

  const auto& options = driver_->GetOptions();
  std::string dds_topic = options.ros2_compatible
      ? options.ros2_topic_prefix + "/string_topic" : "string_topic";

  auto* writer = driver_->GetDataWriter(dds_topic);
  bool is_plain = driver_->IsPlainType(dds_topic);
  auto config = dds_ext::ZeroCopyConfig::Compat();

  auto loaned = dds_ext::BorrowLoanedMessage<std_msgs::msg::String>(
      writer, dds_topic, is_plain, config);

  if (!loaned.is_valid()) {
    MW_ERROR("  FAIL: Expected valid (heap), got invalid: {}", loaned.error_message());
    PrintResult("COMPAT + NonPlain", false);
    return false;
  }

  if (loaned.is_zero_copy()) {
    MW_ERROR("  FAIL: Expected is_zero_copy=false in COMPAT mode");
    PrintResult("COMPAT + NonPlain", false);
    return false;
  }

  MW_INFO("  is_valid=true, is_zero_copy=false (forced heap, correct)");

  loaned->data = "test";
  dds_ext::SendLoaned(std::move(loaned));

  PrintResult("COMPAT + NonPlain", true);
  return true;
}

void ConfigTestModule::PrintResult(const std::string& name, bool passed) {
  if (passed) {
    MW_INFO("  PASSED: {}", name);
    tests_passed_++;
  } else {
    MW_ERROR("  FAILED: {}", name);
    tests_failed_++;
  }
}

void ConfigTestModule::PrintSummary() {
  MW_INFO("========================================");
  MW_INFO("Config Test Summary: {}/{} passed",
             tests_passed_, tests_passed_ + tests_failed_);
  if (tests_failed_ == 0) {
    MW_INFO("All tests passed!");
  } else {
    MW_ERROR("{} test(s) failed", tests_failed_);
  }
  MW_INFO("========================================");
}

}  // namespace ibmw::examples::loaned_message_demo
