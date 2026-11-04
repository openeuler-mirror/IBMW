// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "dds_ext/loaned_message.h"

// Compatibility for ReturnCode_t (fastdds 3.x vs fastrtps 2.x)
#if __has_include(<fastdds/dds/core/detail/DDSReturnCode.hpp>)
namespace loaned_msg_compat = eprosima::fastdds::dds;
#else
namespace loaned_msg_compat {
using ReturnCode_t = eprosima::fastrtps::types::ReturnCode_t;
constexpr auto RETCODE_OK = eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
}  // namespace loaned_msg_compat
#endif

namespace ibmw::extensions::dds_extension {

// ============================================================================
// BorrowLoanedMessage implementation
// ============================================================================

template <typename MsgType>
LoanedMessage<MsgType> BorrowLoanedMessage(
    eprosima::fastdds::dds::DataWriter* writer,
    const std::string& topic_name,
    bool is_plain,
    const ZeroCopyConfig& config) {

  // COMPAT mode: always use heap
  if (config.mode == ZeroCopyConfig::Mode::COMPAT) {
    MsgType* fallback = new (std::nothrow) MsgType();
    if (fallback == nullptr) {
      return LoanedMessage<MsgType>(nullptr, is_plain, "Failed to allocate fallback memory");
    }
    return LoanedMessage<MsgType>(fallback, is_plain, "", writer);
  }

  // Non-plain type cannot use loan
  if (!is_plain) {
    if (config.mode == ZeroCopyConfig::Mode::STRICT) {
      return LoanedMessage<MsgType>(nullptr, is_plain, "STRICT mode but type is not Plain");
    }
    MsgType* fallback = new (std::nothrow) MsgType();
    if (fallback == nullptr) {
      return LoanedMessage<MsgType>(nullptr, is_plain, "Failed to allocate fallback memory");
    }
    return LoanedMessage<MsgType>(fallback, is_plain, "Non-Plain type, using fallback", writer);
  }

  // Try loan from DDS
  if (writer == nullptr) {
    if (config.mode == ZeroCopyConfig::Mode::STRICT) {
      return LoanedMessage<MsgType>(nullptr, is_plain, "DataWriter is null");
    }
    MsgType* fallback = new (std::nothrow) MsgType();
    if (fallback == nullptr) {
      return LoanedMessage<MsgType>(nullptr, is_plain, "Failed to allocate fallback memory");
    }
    return LoanedMessage<MsgType>(fallback, is_plain, "DataWriter is null, using fallback");
  }

  void* loaned_ptr = nullptr;
  auto ret = writer->loan_sample(loaned_ptr);

  if (ret == loaned_msg_compat::RETCODE_OK && loaned_ptr != nullptr) {
    return LoanedMessage<MsgType>(loaned_ptr, writer, is_plain);
  }

  // Loan failed
  if (config.mode == ZeroCopyConfig::Mode::STRICT) {
    return LoanedMessage<MsgType>(nullptr, is_plain, "Failed to loan sample from DDS");
  }

  MsgType* fallback = new (std::nothrow) MsgType();
  if (fallback == nullptr) {
    return LoanedMessage<MsgType>(nullptr, is_plain, "Failed to allocate fallback memory");
  }
  return LoanedMessage<MsgType>(fallback, is_plain, "Loan failed, using fallback", writer);
}

// ============================================================================
// CanLoanMessages implementation
// ============================================================================

template <typename MsgType>
bool CanLoanMessages(
    eprosima::fastdds::dds::DataWriter* writer,
    bool is_plain) {
  if (writer == nullptr) return false;
  return is_plain;
}

// ============================================================================
// SendLoaned implementation
// ============================================================================

template <typename MsgType>
bool SendLoaned(LoanedMessage<MsgType>&& loaned) {
  if (!loaned.is_valid()) return false;

  void* msg_ptr = loaned.GetRawPointer();
  bool is_loaned = loaned.IsLoaned();
  auto* writer = loaned.GetWriter();

  loaned.MarkPublished();

  if (is_loaned && writer != nullptr) {
    bool success = writer->write(msg_ptr);
    if (success) return true;
    writer->discard_loan(msg_ptr);
    return false;
  } else {
    if (writer != nullptr) {
      bool success = writer->write(msg_ptr);
      delete static_cast<MsgType*>(msg_ptr);
      return success;
    } else {
      if (msg_ptr != nullptr) {
        delete static_cast<MsgType*>(msg_ptr);
      }
      return false;
    }
  }
}

}  // namespace ibmw::extensions::dds_extension
