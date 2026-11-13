// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <vector>

namespace ibmw::samples::transport_benchmark::iox_send {

enum class SequenceResult { kWarmup, kAccepted, kDuplicate, kOutOfRange };

class SequenceAccounting {
 public:
  SequenceAccounting(uint32_t message_count, uint32_t warmup_messages)
      : message_count_(message_count),
        warmup_messages_(warmup_messages),
        seen_(message_count, false) {}

  bool IsFixedCount() const { return message_count_ != 0; }
  bool IsWarmup(uint32_t sequence) const {
    return sequence < warmup_messages_;
  }

  bool IsExpectedSequence(uint32_t sequence) const {
    return !IsFixedCount() || sequence < warmup_messages_ ||
           static_cast<uint64_t>(sequence) - warmup_messages_ < message_count_;
  }

  SequenceResult Observe(uint32_t sequence) {
    if (sequence < warmup_messages_) return SequenceResult::kWarmup;
    ++received_;
    if (have_last_sequence_ && sequence <= last_sequence_) ++out_of_order_;
    last_sequence_ = sequence;
    have_last_sequence_ = true;

    if (!IsFixedCount()) {
      ++unique_;
      return SequenceResult::kAccepted;
    }
    const uint64_t measured_sequence = sequence - warmup_messages_;
    if (measured_sequence >= message_count_) return SequenceResult::kOutOfRange;
    if (seen_[measured_sequence]) {
      ++duplicate_;
      return SequenceResult::kDuplicate;
    }
    seen_[measured_sequence] = true;
    ++unique_;
    return SequenceResult::kAccepted;
  }

  uint64_t Received() const { return received_; }
  uint64_t Unique() const { return unique_; }
  uint64_t Duplicate() const { return duplicate_; }
  uint64_t OutOfOrder() const { return out_of_order_; }
  uint64_t Missing() const {
    return IsFixedCount() && message_count_ > unique_
               ? message_count_ - unique_
               : 0;
  }
  bool StrictRunValid(uint64_t invalid_payload,
                      uint64_t invalid_timestamp) const {
    return IsFixedCount() && Missing() == 0 && received_ == message_count_ &&
           duplicate_ == 0 && out_of_order_ == 0 && invalid_payload == 0 &&
           invalid_timestamp == 0;
  }

 private:
  uint32_t message_count_;
  uint32_t warmup_messages_;
  std::vector<bool> seen_;
  uint64_t received_{0};
  uint64_t unique_{0};
  uint64_t duplicate_{0};
  uint64_t out_of_order_{0};
  uint32_t last_sequence_{0};
  bool have_last_sequence_{false};
};

// Counts only timestamp- and payload-valid, in-range measured arrivals as
// received. Invalid copies never consume sequence state, so a later valid copy
// of the same sequence can still be accepted.
class SubscriberAccounting {
 public:
  SubscriberAccounting(uint32_t message_count, uint32_t warmup_messages)
      : sequences_(message_count, warmup_messages) {}

  SequenceResult Observe(uint32_t sequence, bool timestamp_valid,
                         bool payload_valid) {
    if (!timestamp_valid) {
      ++invalid_timestamp_;
      return SequenceResult::kOutOfRange;
    }
    if (sequences_.IsWarmup(sequence)) {
      return sequences_.Observe(sequence);
    }
    if (!payload_valid || !sequences_.IsExpectedSequence(sequence)) {
      ++invalid_payload_;
      return SequenceResult::kOutOfRange;
    }
    return sequences_.Observe(sequence);
  }

  void RecordInvalidPayload() { ++invalid_payload_; }

  bool IsFixedCount() const { return sequences_.IsFixedCount(); }
  uint64_t Received() const { return sequences_.Received(); }
  uint64_t Unique() const { return sequences_.Unique(); }
  uint64_t Duplicate() const { return sequences_.Duplicate(); }
  uint64_t OutOfOrder() const { return sequences_.OutOfOrder(); }
  uint64_t Missing() const { return sequences_.Missing(); }
  uint64_t InvalidPayload() const { return invalid_payload_; }
  uint64_t InvalidTimestamp() const { return invalid_timestamp_; }
  bool StrictRunValid() const {
    return sequences_.StrictRunValid(invalid_payload_, invalid_timestamp_);
  }

 private:
  SequenceAccounting sequences_;
  uint64_t invalid_payload_{0};
  uint64_t invalid_timestamp_{0};
};

struct PublisherRunStatus {
  uint64_t attempted_total;
  uint64_t attempted_measured;
  bool completion;
  bool valid;
};

inline PublisherRunStatus MakePublisherRunStatus(
    uint32_t message_count, uint32_t warmup_messages,
    uint64_t attempted_total, bool duration_elapsed) {
  const uint64_t attempted_measured =
      attempted_total > warmup_messages
          ? attempted_total - warmup_messages
          : 0;
  if (message_count == 0) {
    return {attempted_total, attempted_measured, duration_elapsed,
            duration_elapsed};
  }
  const uint64_t expected_total =
      static_cast<uint64_t>(warmup_messages) + message_count;
  const bool complete = attempted_total == expected_total;
  return {attempted_total, attempted_measured, complete,
          complete && attempted_measured == message_count};
}

inline bool ShouldAppendCsv(uint32_t message_count, bool fixed_count_valid) {
  return message_count == 0 || fixed_count_valid;
}

}  // namespace ibmw::samples::transport_benchmark::iox_send
