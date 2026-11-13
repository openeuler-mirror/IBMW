// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <utility>

namespace ibmw::selfdev::shm {

enum class Error : uint8_t {
  kOk = 0,
  kNoMemory,        // pool or byte budget exhausted
  kTooManyLoans,    // publisher already holds kMaxChunksAllocatedPerPublisher loans
  kTooManyHeld,     // subscriber already holds max_held chunks
  kNoData,          // Take() on an empty subscriber
  kInvalidArg,
  kNotConnected,
  kShuttingDown,
  kAbiMismatch,
  kPermission,
  kTimeout,
  kCorrupted,       // peer wrote an out-of-range value into shared memory
  kSystem,          // unexpected syscall failure; see errno in the log
  kAlreadyInitialized,  // Runtime::Init with options that differ from the running runtime
};

const char* ErrorName(Error e) noexcept;

// Minimal expected<T, Error>; T must be default constructible.
template <class T>
class Result {
 public:
  Result(T value) : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(Error e) : error_(e) {}                 // NOLINT(google-explicit-constructor)

  bool ok() const noexcept { return error_ == Error::kOk; }
  explicit operator bool() const noexcept { return ok(); }
  Error error() const noexcept { return error_; }
  T& value() & noexcept { return value_; }
  const T& value() const& noexcept { return value_; }
  T&& value() && noexcept { return std::move(value_); }
  T* operator->() noexcept { return &value_; }
  T& operator*() noexcept { return value_; }

 private:
  T value_{};
  Error error_ = Error::kOk;
};

template <>
class Result<void> {
 public:
  Result() = default;
  Result(Error e) : error_(e) {}  // NOLINT(google-explicit-constructor)
  bool ok() const noexcept { return error_ == Error::kOk; }
  explicit operator bool() const noexcept { return ok(); }
  Error error() const noexcept { return error_; }

 private:
  Error error_ = Error::kOk;
};

}  // namespace ibmw::selfdev::shm
