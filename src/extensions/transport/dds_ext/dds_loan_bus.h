// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <fastdds/dds/core/LoanableSequence.hpp>
#include <fastdds/dds/subscriber/DataReader.hpp>
#include <fastdds/dds/subscriber/SampleInfo.hpp>

// ReturnCode_t compatibility
#if __has_include(<fastdds/dds/core/detail/DDSReturnCode.hpp>)
#include <fastdds/dds/core/detail/DDSReturnCode.hpp>
#else
#include <fastrtps/types/TypesBase.h>
#endif

#include "cpp/executor/executor.h"

namespace ibmw::extensions::dds_extension {

// ReturnCode_t compatibility aliases
#if __has_include(<fastdds/dds/core/detail/DDSReturnCode.hpp>)
using ReturnCode_t = eprosima::fastdds::dds::ReturnCode_t;
constexpr auto kRetcodeOk = eprosima::fastdds::dds::RETCODE_OK;
constexpr auto kRetcodeNoData = eprosima::fastdds::dds::RETCODE_NO_DATA;
constexpr auto kRetcodePreconditionNotMet = eprosima::fastdds::dds::RETCODE_PRECONDITION_NOT_MET;
#else
using ReturnCode_t = eprosima::fastrtps::types::ReturnCode_t;
constexpr auto kRetcodeOk = eprosima::fastrtps::types::ReturnCode_t::RETCODE_OK;
constexpr auto kRetcodeNoData = eprosima::fastrtps::types::ReturnCode_t::RETCODE_NO_DATA;
constexpr auto kRetcodePreconditionNotMet = eprosima::fastrtps::types::ReturnCode_t::RETCODE_PRECONDITION_NOT_MET;
#endif

/**
 * @brief Loan mode for subscriber data reception
 */
enum class LoanMode {
  // Synchronous: callback on the DDS listener thread. This is the performance
  // baseline for FastDDS DataSharing/loaned zero-copy because loaned samples are
  // returned promptly and the DataSharing pool can keep progressing.
  SYNC,
  // Asynchronous: callback on an IBMW executor. This provides executor
  // isolation, but user callbacks hold FastDDS loaned sample references until
  // completion; at high rates that can pin DataSharing pool/history slots. Treat
  // this as a functional isolation mode, not the 1000 Hz loaned performance path.
  ASYNC,
  AUTO    // Automatic: choose based on executor config
};

/**
 * @brief Batch loan bus for FastDDS loaned samples
 *
 * Manages FastDDS loaned sample lifetimes using shared_ptr reference counting.
 * When all references are released, return_loan is called automatically.
 */
class DdsLoanBatch;

struct DdsLoanedAsyncProfileStats {
  std::atomic<uint64_t> batches{0};
  std::atomic<uint64_t> valid_samples{0};
  std::atomic<uint64_t> sample_refs{0};
  std::atomic<uint64_t> batch_holds{0};
  std::atomic<uint64_t> reclaimer_enqueues{0};
  std::atomic<uint64_t> return_count{0};
  std::atomic<uint64_t> return_total_ns{0};
  std::atomic<uint64_t> return_max_ns{0};
  std::atomic<uint64_t> batch_lifetime_total_ns{0};
  std::atomic<uint64_t> batch_lifetime_max_ns{0};
  std::atomic<uint64_t> dispatch_count{0};
  std::atomic<uint64_t> dispatch_queue_total_ns{0};
  std::atomic<uint64_t> dispatch_queue_max_ns{0};
  std::atomic<uint64_t> dispatch_enqueue_count{0};
  std::atomic<uint64_t> dispatch_enqueue_total_ns{0};
  std::atomic<uint64_t> dispatch_enqueue_max_ns{0};
  std::atomic<uint64_t> dispatch_wakeup_count{0};
  std::atomic<uint64_t> dispatch_wakeup_total_ns{0};
  std::atomic<uint64_t> dispatch_wakeup_max_ns{0};
  std::atomic<uint64_t> callback_before_dispatch_return{0};
  std::atomic<uint64_t> callback_total_ns{0};
  std::atomic<uint64_t> callback_max_ns{0};
  std::atomic<uint64_t> dispatch_failures{0};
  // Counts outstanding async callback/loan holds, not unique messages.
  std::atomic<uint64_t> pending_high_water{0};
  static constexpr size_t kLatencyBucketCount = 16;
  std::array<std::atomic<uint64_t>, kLatencyBucketCount> dispatch_enqueue_buckets{};
  std::array<std::atomic<uint64_t>, kLatencyBucketCount> dispatch_wakeup_buckets{};

  static uint64_t NowNs() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
  }

  static void UpdateMax(std::atomic<uint64_t>& max_value, uint64_t value) noexcept {
    uint64_t current = max_value.load(std::memory_order_relaxed);
    while (current < value &&
           !max_value.compare_exchange_weak(
               current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
  }

  static size_t LatencyBucketIndex(uint64_t value_ns) noexcept {
    uint64_t upper_ns = 1000;
    for (size_t i = 0; i + 1 < kLatencyBucketCount; ++i) {
      if (value_ns <= upper_ns) return i;
      upper_ns *= 2;
    }
    return kLatencyBucketCount - 1;
  }

  static uint64_t LatencyBucketUpperNs(size_t index) noexcept {
    uint64_t upper_ns = 1000;
    for (size_t i = 0; i < index && i + 1 < kLatencyBucketCount; ++i) {
      upper_ns *= 2;
    }
    return upper_ns;
  }

  static void ObserveLatencyBucket(
      std::array<std::atomic<uint64_t>, kLatencyBucketCount>& buckets,
      uint64_t value_ns) noexcept {
    buckets[LatencyBucketIndex(value_ns)].fetch_add(1, std::memory_order_relaxed);
  }

  static uint64_t EstimateP99BucketNs(
      const std::array<std::atomic<uint64_t>, kLatencyBucketCount>& buckets,
      uint64_t count) noexcept {
    // Return the upper bound of the bucket containing p99, not an interpolated percentile.
    if (count == 0) return 0;
    const uint64_t target = (count * 99 + 99) / 100;
    uint64_t cumulative = 0;
    for (size_t i = 0; i < kLatencyBucketCount; ++i) {
      cumulative += buckets[i].load(std::memory_order_relaxed);
      if (cumulative >= target) return LatencyBucketUpperNs(i);
    }
    return LatencyBucketUpperNs(kLatencyBucketCount - 1);
  }
};

/**
 * @brief Per-listener reclaim path for FastDDS loan returns.
 *
 * FastDDS requires take()/return_loan() to be serialized for a reader, but
 * returning the loan on executor worker threads regresses callback p99. This
 * small worker owns final return_loan calls and shares the reader mutex used by
 * take(), keeping executor callbacks to shared_ptr release only.
 */
class DdsLoanReclaimer : public std::enable_shared_from_this<DdsLoanReclaimer> {
 public:
  using DataReader = eprosima::fastdds::dds::DataReader;

  DdsLoanReclaimer(DataReader* reader, std::shared_ptr<std::mutex> reader_mutex)
      : reader_(reader), reader_mutex_(std::move(reader_mutex)) {
    worker_ = std::thread([this]() { Run(); });
  }

  ~DdsLoanReclaimer() { StopAndDrain(); }

  DdsLoanReclaimer(const DdsLoanReclaimer&) = delete;
  DdsLoanReclaimer& operator=(const DdsLoanReclaimer&) = delete;

  void Enqueue(std::shared_ptr<DdsLoanBatch> batch) {
    if (!batch) return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      queue_.emplace_back(std::move(batch));
    }
    cv_.notify_one();
  }

  void StopAndDrain() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_one();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  DataReader* reader() const noexcept { return reader_; }
  const std::shared_ptr<std::mutex>& reader_mutex() const noexcept { return reader_mutex_; }

 private:
  void Run();

  DataReader* reader_;
  std::shared_ptr<std::mutex> reader_mutex_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::shared_ptr<DdsLoanBatch>> queue_;
  bool stopping_{false};
  std::thread worker_;
};

class DdsLoanBatch : public std::enable_shared_from_this<DdsLoanBatch> {
 public:
  using DataReader = eprosima::fastdds::dds::DataReader;
  using LoanableSequence = eprosima::fastdds::dds::LoanableSequence<void*>;
  using SampleInfoSeq = eprosima::fastdds::dds::SampleInfoSeq;

  DdsLoanBatch(
      std::shared_ptr<DdsLoanReclaimer> reclaimer,
      const std::string& topic_name = "",
      std::shared_ptr<DdsLoanedAsyncProfileStats> profile_stats = nullptr)
      : reclaimer_(std::move(reclaimer)),
        topic_name_(topic_name),
        profile_stats_(std::move(profile_stats)),
        create_ns_(profile_stats_ ? DdsLoanedAsyncProfileStats::NowNs() : 0) {}

  ~DdsLoanBatch() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!returned_ && data_seq_.length() > 0) {
      // Destructors must not call FastDDS return_loan inline on executor
      // threads. A non-returned batch at destruction means shutdown raced with
      // retained samples; mark it returned to avoid a UAF against a deleted
      // DataReader rather than invoking FastDDS from the wrong thread.
      returned_ = true;
    }
  }

  DdsLoanBatch(const DdsLoanBatch&) = delete;
  DdsLoanBatch& operator=(const DdsLoanBatch&) = delete;

  ReturnCode_t TakeLoan() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (returned_) return kRetcodePreconditionNotMet;

    auto ret = TakeLocked();
    if (ret == kRetcodeOk) {
      for (LoanableSequence::size_type i = 0; i < info_seq_.length(); ++i) {
        if (info_seq_[i].valid_data) {
          valid_sample_count_++;
        }
      }
      if (profile_stats_) {
        profile_stats_->batches.fetch_add(1, std::memory_order_relaxed);
        profile_stats_->valid_samples.fetch_add(valid_sample_count_, std::memory_order_relaxed);
      }
    }
    return ret;
  }

  std::shared_ptr<void> CreateSharedPtr(
      size_t index,
      std::shared_ptr<DdsLoanBatch> batch_holder) {

    std::lock_guard<std::mutex> lock(mutex_);

    if (returned_ || index >= data_seq_.length() || !info_seq_[index].valid_data) {
      return nullptr;
    }

    const void* const* buffer = data_seq_.buffer();
    if (!buffer) return nullptr;

    void* data = const_cast<void*>(buffer[index]);

    active_ref_count_.fetch_add(1, std::memory_order_acq_rel);
    if (profile_stats_) {
      profile_stats_->sample_refs.fetch_add(1, std::memory_order_relaxed);
    }

    return std::shared_ptr<void>(
        data,
        [batch_holder](void*) mutable {
          batch_holder->DecrementRefCount(batch_holder);
        });
  }

  std::shared_ptr<void> CreateBatchHold(std::shared_ptr<DdsLoanBatch> batch_holder) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (returned_) return nullptr;

    active_ref_count_.fetch_add(1, std::memory_order_acq_rel);
    if (profile_stats_) {
      profile_stats_->batch_holds.fetch_add(1, std::memory_order_relaxed);
    }

    return std::shared_ptr<void>(
        this,
        [batch_holder](void*) mutable {
          batch_holder->DecrementRefCount(batch_holder);
        });
  }

  size_t GetSampleCount() const { return data_seq_.length(); }
  size_t GetValidSampleCount() const { return valid_sample_count_; }

  bool IsValidSample(size_t index) const {
    if (index >= info_seq_.length()) return false;
    return info_seq_[index].valid_data;
  }

  const eprosima::fastdds::dds::SampleInfo* GetSampleInfo(size_t index) const {
    if (index >= info_seq_.length()) return nullptr;
    return &info_seq_[index];
  }

  bool IsReturned() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return returned_;
  }

  int GetActiveRefCount() const { return active_ref_count_.load(); }

 private:
  void DecrementRefCount(const std::shared_ptr<DdsLoanBatch>& batch_holder) {
    std::shared_ptr<DdsLoanReclaimer> reclaimer;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      int prev_count = active_ref_count_.fetch_sub(1);
      if (prev_count == 1 && !returned_ && data_seq_.length() > 0) {
        returned_ = true;
        reclaimer = reclaimer_;
      }
    }
    if (reclaimer) {
      if (profile_stats_) {
        const uint64_t lifetime_ns =
            DdsLoanedAsyncProfileStats::NowNs() - create_ns_;
        profile_stats_->reclaimer_enqueues.fetch_add(1, std::memory_order_relaxed);
        profile_stats_->batch_lifetime_total_ns.fetch_add(lifetime_ns, std::memory_order_relaxed);
        DdsLoanedAsyncProfileStats::UpdateMax(
            profile_stats_->batch_lifetime_max_ns, lifetime_ns);
      }
      reclaimer->Enqueue(batch_holder);
    }
  }

  ReturnCode_t TakeLocked() {
    if (!reclaimer_) return kRetcodePreconditionNotMet;
    std::lock_guard<std::mutex> reader_lock(*reclaimer_->reader_mutex());
    return reclaimer_->reader()->take(data_seq_, info_seq_);
  }

  void ReturnLoanLocked() {
    if (!reclaimer_) return;
    const uint64_t start_ns = profile_stats_ ? DdsLoanedAsyncProfileStats::NowNs() : 0;
    std::lock_guard<std::mutex> reader_lock(*reclaimer_->reader_mutex());
    reclaimer_->reader()->return_loan(data_seq_, info_seq_);
    if (profile_stats_) {
      const uint64_t elapsed_ns = DdsLoanedAsyncProfileStats::NowNs() - start_ns;
      profile_stats_->return_count.fetch_add(1, std::memory_order_relaxed);
      profile_stats_->return_total_ns.fetch_add(elapsed_ns, std::memory_order_relaxed);
      DdsLoanedAsyncProfileStats::UpdateMax(profile_stats_->return_max_ns, elapsed_ns);
    }
  }

  friend class DdsLoanReclaimer;

 private:
  std::shared_ptr<DdsLoanReclaimer> reclaimer_;
  std::string topic_name_;
  std::shared_ptr<DdsLoanedAsyncProfileStats> profile_stats_;
  uint64_t create_ns_{0};
  LoanableSequence data_seq_;
  SampleInfoSeq info_seq_;
  std::atomic<int> active_ref_count_{0};
  size_t valid_sample_count_{0};
  bool returned_{false};
  mutable std::mutex mutex_;
};

inline void DdsLoanReclaimer::Run() {
  while (true) {
    std::shared_ptr<DdsLoanBatch> batch;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (stopping_) return;
        continue;
      }
      batch = std::move(queue_.front());
      queue_.pop_front();
    }
    if (batch) {
      batch->ReturnLoanLocked();
    }
  }
}

/**
 * @brief Factory for loan batches
 */
class DdsLoanBus {
 public:
  static std::shared_ptr<DdsLoanBatch> CreateLoanBatch(
      std::shared_ptr<DdsLoanReclaimer> reclaimer,
      const std::string& topic_name,
      std::shared_ptr<DdsLoanedAsyncProfileStats> profile_stats = nullptr) {
    return std::make_shared<DdsLoanBatch>(
        std::move(reclaimer), topic_name, std::move(profile_stats));
  }
};

/**
 * @brief Parse loan mode string
 */
inline LoanMode ParseLoanMode(const std::string& mode_str) {
  if (mode_str == "sync") return LoanMode::SYNC;
  if (mode_str == "async") return LoanMode::ASYNC;
  return LoanMode::AUTO;
}

inline LoanMode ResolveEffectiveLoanMode(LoanMode configured_mode, bool has_executor) {
  if (configured_mode == LoanMode::AUTO) {
    return has_executor ? LoanMode::ASYNC : LoanMode::SYNC;
  }
  return configured_mode;
}

inline const char* LoanModeToString(LoanMode mode) noexcept {
  switch (mode) {
    case LoanMode::SYNC:
      return "SYNC";
    case LoanMode::ASYNC:
      return "ASYNC";
    case LoanMode::AUTO:
      return "AUTO";
  }
  return "UNKNOWN";
}

}  // namespace ibmw::extensions::dds_extension
