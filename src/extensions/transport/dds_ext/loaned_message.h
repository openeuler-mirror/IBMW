// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <type_traits>

#include <fastdds/dds/publisher/DataWriter.hpp>

namespace ibmw::extensions::dds_extension {

// ============================================================================
// Zero-copy configuration
// ============================================================================

struct ZeroCopyConfig {
  enum class Mode {
    AUTO,    // Automatic fallback (default)
    STRICT,  // Force zero-copy; fail if not possible
    COMPAT   // Force heap allocation; never use zero-copy
  };
  Mode mode = Mode::AUTO;

  enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR,
    OFF
  };
  LogLevel log_level = LogLevel::WARN;

  bool monitoring_enabled = true;
  uint32_t monitoring_interval_sec = 60;

  static ZeroCopyConfig Auto() {
    return ZeroCopyConfig{Mode::AUTO, LogLevel::WARN, true, 60};
  }
  static ZeroCopyConfig Strict() {
    return ZeroCopyConfig{Mode::STRICT, LogLevel::ERROR, true, 60};
  }
  static ZeroCopyConfig Compat() {
    return ZeroCopyConfig{Mode::COMPAT, LogLevel::OFF, false, 0};
  }
  static ZeroCopyConfig Development() {
    return ZeroCopyConfig{Mode::AUTO, LogLevel::DEBUG, true, 30};
  }
  static ZeroCopyConfig Production() {
    return ZeroCopyConfig{Mode::AUTO, LogLevel::ERROR, true, 300};
  }
};

// ============================================================================
// Compile-time type check
// ============================================================================

template <typename MsgType>
constexpr bool IsPossiblyPlain() {
  return std::is_standard_layout_v<MsgType> &&
         std::is_trivially_copyable_v<MsgType>;
}

// ============================================================================
// LoanedMessage class template
// ============================================================================

template <typename MsgType>
class LoanedMessage {
 public:
  // Zero-copy constructor
  LoanedMessage(void* loaned_ptr,
                eprosima::fastdds::dds::DataWriter* writer,
                bool is_plain)
      : loaned_ptr_(loaned_ptr),
        writer_(writer),
        is_loaned_(true),
        is_plain_(is_plain),
        fallback_msg_(nullptr),
        published_(false) {}

  // Fallback (heap) constructor. `writer` lets SendLoaned() publish the heap
  // sample by copy; without it the fallback can never be written.
  LoanedMessage(MsgType* fallback_msg,
                bool is_plain,
                const std::string& error_msg = "",
                eprosima::fastdds::dds::DataWriter* writer = nullptr)
      : loaned_ptr_(nullptr),
        writer_(writer),
        is_loaned_(false),
        is_plain_(is_plain),
        fallback_msg_(fallback_msg),
        published_(false),
        error_msg_(error_msg) {}

  // Move constructor
  LoanedMessage(LoanedMessage&& other) noexcept
      : loaned_ptr_(other.loaned_ptr_),
        writer_(other.writer_),
        is_loaned_(other.is_loaned_),
        is_plain_(other.is_plain_),
        fallback_msg_(other.fallback_msg_),
        published_(other.published_),
        error_msg_(std::move(other.error_msg_)) {
    other.loaned_ptr_ = nullptr;
    other.writer_ = nullptr;
    other.fallback_msg_ = nullptr;
    other.published_ = true;
  }

  // Move assignment
  LoanedMessage& operator=(LoanedMessage&& other) noexcept {
    if (this != &other) {
      Cleanup();
      loaned_ptr_ = other.loaned_ptr_;
      writer_ = other.writer_;
      is_loaned_ = other.is_loaned_;
      is_plain_ = other.is_plain_;
      fallback_msg_ = other.fallback_msg_;
      published_ = other.published_;
      error_msg_ = std::move(other.error_msg_);
      other.loaned_ptr_ = nullptr;
      other.writer_ = nullptr;
      other.fallback_msg_ = nullptr;
      other.published_ = true;
    }
    return *this;
  }

  // No copy
  LoanedMessage(const LoanedMessage&) = delete;
  LoanedMessage& operator=(const LoanedMessage&) = delete;

  ~LoanedMessage() { Cleanup(); }

  // State queries
  bool is_valid() const {
    return (loaned_ptr_ != nullptr || fallback_msg_ != nullptr) && !published_;
  }
  bool is_zero_copy() const { return is_loaned_; }
  bool is_plain_type() const { return is_plain_; }
  const std::string& error_message() const { return error_msg_; }

  // Data access (auto-routing)
  MsgType* operator->() {
    return is_loaned_ ? static_cast<MsgType*>(loaned_ptr_) : fallback_msg_;
  }
  const MsgType* operator->() const {
    return is_loaned_ ? static_cast<const MsgType*>(loaned_ptr_) : fallback_msg_;
  }
  MsgType& operator*() {
    return is_loaned_ ? *static_cast<MsgType*>(loaned_ptr_) : *fallback_msg_;
  }
  const MsgType& operator*() const {
    return is_loaned_ ? *static_cast<const MsgType*>(loaned_ptr_) : *fallback_msg_;
  }
  MsgType& get() { return **this; }
  const MsgType& get() const { return **this; }

  // Internal use
  bool IsLoaned() const { return is_loaned_; }
  void* GetRawPointer() {
    return is_loaned_ ? loaned_ptr_ : static_cast<void*>(fallback_msg_);
  }
  eprosima::fastdds::dds::DataWriter* GetWriter() { return writer_; }

  void MarkPublished() {
    published_ = true;
    loaned_ptr_ = nullptr;
    writer_ = nullptr;
    fallback_msg_ = nullptr;
  }

 private:
  void Cleanup() {
    if (published_) return;
    if (loaned_ptr_ != nullptr && is_loaned_ && writer_ != nullptr) {
      writer_->discard_loan(loaned_ptr_);
      loaned_ptr_ = nullptr;
      writer_ = nullptr;
    } else if (fallback_msg_ != nullptr) {
      delete fallback_msg_;
      fallback_msg_ = nullptr;
    }
  }

 private:
  void* loaned_ptr_;
  eprosima::fastdds::dds::DataWriter* writer_;
  bool is_loaned_;
  bool is_plain_;
  MsgType* fallback_msg_;
  bool published_;
  std::string error_msg_;
};

// ============================================================================
// Function declarations (implemented in loaned_message_impl.h)
// ============================================================================

template <typename MsgType>
LoanedMessage<MsgType> BorrowLoanedMessage(
    eprosima::fastdds::dds::DataWriter* writer,
    const std::string& topic_name,
    bool is_plain,
    const ZeroCopyConfig& config = ZeroCopyConfig::Auto());

template <typename MsgType>
bool SendLoaned(LoanedMessage<MsgType>&& loaned);

}  // namespace ibmw::extensions::dds_extension
