// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "publisher_module/publisher_module.h"

#include <algorithm>
#include <chrono>

// IBMW_R1_EXEMPT_BEGIN
#include "dds_types/util/dds_type_support.h"
// IBMW_R1_EXEMPT_END

namespace ibmw::samples::transport_benchmark::dds_loaned {

namespace {
// Single shared IBMW topic across all 5 typed publishers. Each Plain
// bench type still gets its OWN DDS topic at the FastDDS layer because
// DdsTransportDriver::GetDdsTopicName() encodes the type into the DDS
// topic suffix when ros2_compatible=false:
//   dds_topic = ibmw_topic + "_native_" + sanitize(msg_type)
// So our 5 typed slots map to 5 distinct FastDDS DataWriters under the
// same IBMW topic name -- exactly what we want.
constexpr std::string_view kTopic = "dds_bench_loaned_topic";

// Resolve the DDS topic name the dds driver uses internally when it
// registered our type via RegisterSendType<PlainT>. The driver keys
// every PublisherInfo on this exact string (see dds_transport_driver.cc
// GetDdsTopicName / publisher_map_), so create_publisher() must look up
// the DataWriter by the same key. We can't go through
// create_publisher_with_driver_config() because that helper only
// rewrites the IBMW topic in ros2_compatible mode; in native mode it
// just passes ibmw_topic straight through, which would miss our
// "_native_<type>" registration.
//
// Mirrors the encoding rule in
//   DdsTransportDriver::GetDdsTopicName():
//     std::replace(name.begin(), name.end(), ':', '_');
//     std::replace(name.begin(), name.end(), '.', '_');
//     return ibmw_topic + "_native_" + name;
// where name is the full msg_type (i.e. "dds:" + DdsTypeName<T>::get()).
inline std::string ResolveDdsTopicName(const std::string& msg_type) {
  std::string encoded = msg_type;
  std::replace(encoded.begin(), encoded.end(), ':', '_');
  std::replace(encoded.begin(), encoded.end(), '.', '_');
  return std::string(kTopic) + "_native_" + encoded;
}

// The msg_type strings MUST match what GetDdsMessageTypeSupport<T>()
// stamps onto the type-support record (i.e. "dds:" + DdsTypeName<T>).
template <typename PlainT>
inline std::string MsgTypeOf();

#define IBMW_PUB_MSG_TYPE_FN(LBL, N, ALIAS)                              \
  template <>                                                            \
  inline std::string MsgTypeOf<ALIAS>() {                                \
    return std::string("dds:") +                                         \
           "ibmw::samples::transport_benchmark::BenchPlainPayload" LBL;    \
  }
IBMW_BENCH_PLAIN_FOR_EACH(IBMW_PUB_MSG_TYPE_FN)
#undef IBMW_PUB_MSG_TYPE_FN

}  // namespace

bool PublisherModule::Setup(ibmw::Runtime core) {
  core_ = core;

  auto cfg_path = core_.GetConfigurator().GetConfigFilePath();
  opts_ = LoadBenchOptions(cfg_path);
  if (PayloadSizeLabel(opts_.payload_bytes) == nullptr) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "PublisherModule: payload_bytes={} not in supported bench "
           "matrix (1024/4096/16384/65536/262144/1048576/4194304)",
           opts_.payload_bytes);
    return false;
  }

  // Engine-side wiring MUST happen in Setup() -- the dds driver is in
  // State::kInit only between Initialize() and Start(); RegisterSendType
  // refuses to add new (topic, type) entries once the engine has
  // started. Each RegisterSendType<PlainT> below also triggers
  // type.register_type() against the FastDDS Participant and creates
  // the underlying DataWriter, so by Run() time GetDataWriter() is
  // guaranteed to return non-null for all 5 types.
  //
  // The shared GetPublisher(kTopic) call returns the same SenderRef
  // each time (transport bus de-dups on topic name); each typed
  // RegisterSendType adds its own DDS topic keyed on
  // ResolveDdsTopicName(msg_type).
#define IBMW_PUB_SETUP_REGISTER(LBL, N, ALIAS)                                \
  {                                                                           \
    auto& slot = slot_##N##_;                                                 \
    slot.sender = core_.GetTransport().GetPublisher(std::string(kTopic));     \
    if (!ibmw::transport::RegisterSendType<ALIAS>(slot.sender)) {             \
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,           \
             "PublisherModule: RegisterSendType<" LBL "> failed");            \
      return false;                                                           \
    }                                                                         \
  }
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_PUB_SETUP_REGISTER)
#undef IBMW_PUB_SETUP_REGISTER

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PublisherModule Setup OK: label={} payload_bytes={} ({}) hz={} "
         "duration_sec={} warmup={}",
         opts_.label, opts_.payload_bytes,
         PayloadSizeLabel(opts_.payload_bytes), opts_.hz, opts_.duration_sec,
         opts_.warmup_messages);
  return true;
}

bool PublisherModule::Run() {
  if (driver_ == nullptr) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "PublisherModule: dds driver not injected before Run()");
    return false;
  }

  // Build all 5 typed Ros2StylePublishers around the FastDDS
  // DataWriters that the driver created in Setup(). We bypass
  // create_publisher_with_driver_config<T>() and call the lower-level
  // create_publisher<T>() directly because the driver-config helper
  // only rewrites IBMW topic -> DDS topic in ros2_compatible mode; in
  // our native mode it passes ibmw_topic straight through, which would
  // miss the "_native_<type>" suffix the driver actually registered.
  //
  // From this point on, the chosen slot's publish path is:
  //   borrow_loaned_message() -> write Plain fields directly into the
  //   loaned SHM chunk -> publish(std::move(loaned)). No engine CDR,
  //   no heap copy.
#define IBMW_PUB_RUN_CREATE(LBL, N, ALIAS)                                     \
  {                                                                            \
    auto& slot = slot_##N##_;                                                  \
    const std::string dds_topic = ResolveDdsTopicName(MsgTypeOf<ALIAS>());     \
    slot.typed_pub = ibmw::extensions::dds_extension::                         \
        create_publisher<ALIAS>(driver_, dds_topic);                           \
    if (slot.typed_pub == nullptr) {                                           \
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,            \
             "PublisherModule: create_publisher<" LBL "> failed for "          \
             "dds_topic={}", dds_topic);                                       \
      return false;                                                            \
    }                                                                          \
    /* Surface plain-ness + loan capability up-front so a missing      */      \
    /* DataSharing wiring (yaml typo, no compatible reader, etc.) is   */      \
    /* visible in the log instead of silently degrading to fallback.   */      \
    bool can_loan = slot.typed_pub->can_loan_messages();                       \
    bool is_plain = driver_->IsPlainType(dds_topic);                           \
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,               \
           "PublisherModule: slot " LBL " ready dds_topic={} is_plain={} "     \
           "can_loan={}", dds_topic, is_plain, can_loan);                      \
    if (!is_plain || !can_loan) {                                              \
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,            \
             "PublisherModule: slot " LBL " not zero-copy capable -- "         \
             "publish would silently fall back to copy path");                 \
      return false;                                                            \
    }                                                                          \
  }
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_PUB_RUN_CREATE)
#undef IBMW_PUB_RUN_CREATE

  running_ = true;
  thread_ = std::thread([this]() { PublishLoop(); });
  return true;
}

void PublisherModule::Stop() {
  running_ = false;
  if (thread_.joinable()) thread_.join();
#define IBMW_PUB_STOP_RESET(LBL, N, ALIAS) slot_##N##_.typed_pub.reset();
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_PUB_STOP_RESET)
#undef IBMW_PUB_STOP_RESET
}

bool PublisherModule::WaitForSelectedPublisherMatch() {
  using clock = std::chrono::steady_clock;
  constexpr auto kWriterMatchTimeout = std::chrono::seconds(10);
  constexpr auto kWriterMatchPoll = std::chrono::milliseconds(50);

  if (driver_ == nullptr) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "PublisherModule: DDS driver is null before match wait");
    return false;
  }

  auto wait_for_slot = [&](auto& slot, const char* label) -> bool {
    if (!slot.typed_pub) {
      MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
             "PublisherModule: slot {} has no typed publisher before match wait", label);
      return false;
    }

    const auto deadline = clock::now() + kWriterMatchTimeout;
    int matched_count = 0;
    while (running_ && clock::now() <= deadline) {
      matched_count = driver_->GetPublicationMatchedCount(slot.typed_pub->get_topic_name());
      if (matched_count > 0) {
        MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
               "PublisherModule: slot {} matched subscriber current_count={}",
               label, matched_count);
        return true;
      }
      std::this_thread::sleep_for(kWriterMatchPoll);
    }

    matched_count = driver_->GetPublicationMatchedCount(slot.typed_pub->get_topic_name());
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "PublisherModule: slot {} failed to match subscriber before publish "
           "timeout_ms={} current_count={}",
           label,
           std::chrono::duration_cast<std::chrono::milliseconds>(
               kWriterMatchTimeout).count(),
           matched_count);
    return false;
  };

#define IBMW_PUB_WAIT_MATCH(LBL, N, ALIAS)                                     \
  if (opts_.payload_bytes == (N)) {                                            \
    return wait_for_slot(slot_##N##_, LBL);                                    \
  } else
  IBMW_BENCH_PLAIN_FOR_EACH(IBMW_PUB_WAIT_MATCH)
  {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "PublisherModule: unsupported payload_bytes={} during match wait",
           opts_.payload_bytes);
    return false;
  }
#undef IBMW_PUB_WAIT_MATCH
}

void PublisherModule::PublishLoop() {
  using clock = std::chrono::steady_clock;

  const auto period_ns = std::chrono::nanoseconds(
      static_cast<int64_t>(1'000'000'000ULL / std::max<uint32_t>(opts_.hz, 1)));

  uint32_t seq = 0;
  uint64_t loan_failed = 0;
  uint64_t publish_failed = 0;
  uint64_t fallback_count = 0;

  if (!WaitForSelectedPublisherMatch()) {
    MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelError,
           "PublisherModule: aborting publish loop because selected DDS writer "
           "did not match a subscriber");
    return;
  }

  const auto deadline = clock::now() + std::chrono::seconds(opts_.duration_sec);
  auto next_tick = clock::now();

  while (running_ && clock::now() < deadline) {
    // Static fan-out by payload_bytes. The if-chain is fully resolved
    // at compile time per branch -- the only runtime cost over the
    // single-type loop is one `==` per iteration per non-matching
    // branch (negligible: 5 cmps at most for the largest bank).
#define IBMW_PUB_LOOP_DISPATCH(LBL, N, ALIAS)                                  \
  if (opts_.payload_bytes == (N)) {                                            \
    auto loaned = slot_##N##_.typed_pub->borrow_loaned_message();              \
    if (!loaned.is_valid()) {                                                  \
      ++loan_failed;                                                           \
    } else {                                                                   \
      if (!loaned.is_zero_copy()) ++fallback_count;                            \
      /* Write the Plain bench fields DIRECTLY into the loaned SHM   */        \
      /* chunk -- no intermediate heap object, no memcpy. The        */        \
      /* generated FastCDR layout for our Plain types matches the    */        \
      /* in-memory PlainT layout 1:1 (trivially-copyable, standard   */        \
      /* layout), which is what makes this end-to-end zero-copy.     */        \
      FillBenchPlainPayload(*loaned, seq);                                     \
      if (!slot_##N##_.typed_pub->publish(std::move(loaned))) {                \
        ++publish_failed;                                                      \
      }                                                                        \
    }                                                                          \
  } else
    IBMW_BENCH_PLAIN_FOR_EACH(IBMW_PUB_LOOP_DISPATCH)
    {
      // Caught by the Setup() validation; defensive only.
      ++publish_failed;
    }
#undef IBMW_PUB_LOOP_DISPATCH
    ++seq;

    next_tick += period_ns;
    auto now = clock::now();
    if (next_tick > now) {
      std::this_thread::sleep_until(next_tick);
    } else {
      // Behind schedule -- drop the catch-up to avoid burst artifacts
      // skewing the subscriber's percentile tail.
      next_tick = now;
    }
  }

  MW_LOG(core_.GetLogger(), ibmw::common::util::kLogLevelInfo,
         "PublisherModule done: total_seq={} loan_failed={} publish_failed={} "
         "fallback={}",
         seq, loan_failed, publish_failed, fallback_count);
}

}  // namespace ibmw::samples::transport_benchmark::dds_loaned
