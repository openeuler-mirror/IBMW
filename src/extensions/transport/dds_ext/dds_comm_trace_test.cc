// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "dds_ext/internal/dds_comm_trace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include <gtest/gtest.h>
#include <openssl/evp.h>

namespace ibmw::extensions::dds_extension::comm_trace {
void SetProducerAdmittedHookForTest(ProducerGateHookForTest hook) noexcept;

// Exact callback admission must keep allocation failure on its noexcept,
// fail-closed path.
static_assert(noexcept(ExactCallbackBegin()));

namespace {

std::atomic<uint64_t> clock_calls{0};
std::atomic<bool> gate_hook_entered{false};
std::atomic<bool> gate_hook_release{false};
std::atomic<bool> admitted_gate_hook_entered{false};
std::atomic<bool> admitted_gate_hook_release{false};

uint64_t CountingClock() noexcept {
  return clock_calls.fetch_add(1, std::memory_order_relaxed) + 10;
}

void BlockingGateHook() noexcept {
  gate_hook_entered.store(true, std::memory_order_release);
  while (!gate_hook_release.load(std::memory_order_acquire)) std::this_thread::yield();
}

void BlockingAdmittedGateHook() noexcept {
  admitted_gate_hook_entered.store(true, std::memory_order_release);
  while (!admitted_gate_hook_release.load(std::memory_order_acquire)) std::this_thread::yield();
}

Guid KnownGuid(uint8_t seed) {
  Guid guid = Guid::unknown();
  for (size_t i = 0; i < 12; ++i) guid.guidPrefix.value[i] = seed + i;
  for (size_t i = 0; i < 4; ++i) guid.entityId.value[i] = seed + 12 + i;
  return guid;
}

SampleIdentity Identity(uint8_t seed, int32_t high, uint32_t low) {
  SampleIdentity identity;
  identity.writer_guid() = KnownGuid(seed);
  identity.sequence_number(fastdds_compat::rtps::SequenceNumber_t(high, low));
  return identity;
}

SampleKey Key(uint32_t sequence) {
  SampleKey key{};
  CopyGuid(KnownGuid(1), key.writer_guid);
  key.sequence_high = 0;
  key.sequence_low = sequence;
  return key;
}

DeliveryKey Delivery(uint32_t sequence) {
  DeliveryKey key{};
  key.sample = Key(sequence);
  CopyGuid(KnownGuid(40), key.reader_guid);
  return key;
}

class TempDirectory final {
 public:
  TempDirectory() {
    std::array<char, 64> pattern{};
    std::strcpy(pattern.data(), "/tmp/ibmw-dds-trace-XXXXXX");
    path_ = mkdtemp(pattern.data());
    fd_ = open(path_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  }
  ~TempDirectory() {
    if (fd_ >= 0) close(fd_);
    std::filesystem::remove_all(path_);
  }
  int fd() const { return fd_; }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  int fd_ = -1;
};

void SetManagedEnvironment(const TempDirectory& directory,
                           const char* path_id = "dds-path") {
  setenv("IBMW_COMM_TRACE", "full", 1);
  setenv("IBMW_COMM_TRACE_SINK", "managed", 1);
  const std::string fd = std::to_string(directory.fd());
  setenv("MW_PERF_COMM_TRACE_DIR_FD", fd.c_str(), 1);
  setenv("MW_PERF_RUN_ID", "run-1", 1);
  setenv("MW_PERF_CAPABILITY_ID", "dds-stage-only", 1);
  setenv("MW_PERF_PATH_ID", path_id, 1);
  setenv("MW_PERF_DEPLOYMENT_MODE", "managed", 1);
  setenv("MW_PERF_TOPOLOGY_GENERATION", "generation-7", 1);
  setenv("MW_PERF_TOPOLOGY_SHA256",
         "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", 1);
}

void ClearTraceEnvironment() {
  unsetenv("IBMW_COMM_TRACE");
  unsetenv("IBMW_COMM_TRACE_SINK");
  unsetenv("MW_PERF_COMM_TRACE_DIR_FD");
  unsetenv("MW_PERF_RUN_ID");
  unsetenv("MW_PERF_CAPABILITY_ID");
  unsetenv("MW_PERF_PATH_ID");
  unsetenv("MW_PERF_DEPLOYMENT_MODE");
  unsetenv("MW_PERF_TOPOLOGY_GENERATION");
  unsetenv("MW_PERF_TOPOLOGY_SHA256");
  unsetenv("MW_PERF_EXACT_REQUIRED");
}

std::vector<std::string> Files(const std::string& path) {
  std::vector<std::string> result;
  for (const auto& entry : std::filesystem::directory_iterator(path)) {
    result.push_back(entry.path().filename().string());
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::vector<uint8_t> ReadFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

bool EndsWith(const std::string& value, const char* suffix) {
  const size_t suffix_size = std::strlen(suffix);
  return value.size() >= suffix_size &&
         value.compare(value.size() - suffix_size, suffix_size, suffix) == 0;
}

uint64_t GetLe64(const uint8_t* data) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data[i]) << (i * 8);
  return value;
}

uint16_t GetLe16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0]) |
         static_cast<uint16_t>(data[1]) << 8;
}

size_t JsonKeyCount(const std::string& json) {
  size_t count = 0;
  for (size_t position = 0; (position = json.find("\":", position)) != std::string::npos;
       position += 2) {
    ++count;
  }
  return count;
}

std::array<uint8_t, 32> Digest(const uint8_t* data, size_t size) {
  std::array<uint8_t, 32> result{};
  unsigned int output_size = 0;
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  EXPECT_NE(context, nullptr);
  EXPECT_EQ(EVP_DigestInit_ex(context, EVP_sha256(), nullptr), 1);
  EXPECT_EQ(EVP_DigestUpdate(context, data, size), 1);
  EXPECT_EQ(EVP_DigestFinal_ex(context, result.data(), &output_size), 1);
  EXPECT_EQ(output_size, result.size());
  EVP_MD_CTX_free(context);
  return result;
}

class DdsCommTraceTest : public testing::Test {
 protected:
  void SetUp() override {
    ClearTraceEnvironment();
    ResetForTest();
    SetModeForTest(RuntimeMode::full);
  }
  void TearDown() override {
    ResetForTest();
    ClearTraceEnvironment();
  }
};

TEST_F(DdsCommTraceTest, EventLayoutAndKindsAreStable) {
  EXPECT_EQ(sizeof(Event), 64U);
  EXPECT_EQ(alignof(Event), 8U);
  EXPECT_EQ(static_cast<uint8_t>(EventKind::publish_write), 1U);
  EXPECT_EQ(static_cast<uint8_t>(EventKind::take_receive), 2U);
  EXPECT_EQ(static_cast<uint8_t>(EventKind::dispatch), 3U);
}

TEST_F(DdsCommTraceTest, PublishWriteAdmissionRequiresEnvelopeInExactMode) {
  SetExactEnabledForTest(true);
  EXPECT_FALSE(ShouldEmitPublishWrite(false));
  EXPECT_TRUE(ShouldEmitPublishWrite(true));

  SetExactEnabledForTest(false);
  EXPECT_TRUE(ShouldEmitPublishWrite(false));
}

TEST_F(DdsCommTraceTest, MakeEventDefaultsToSuccessAndPreservesFailureStatus) {
  const Event success = MakeEvent(1, 2, Key(1), 3, EventKind::dispatch);
  const Event failure = MakeEvent(1, 2, Key(2), 3, EventKind::dispatch, 1);
  EXPECT_EQ(success.status, 0U);
  EXPECT_EQ(failure.status, 1U);
  EXPECT_EQ(failure.kind, static_cast<uint8_t>(EventKind::dispatch));
}

TEST_F(DdsCommTraceTest, ExactQueryRoundTripsCallbackIdentity) {
  SetExactEnabledForTest(true);
  const DeliveryKey delivery = Delivery(9);
  ExactSubmitCallback(10, 20, delivery, 73);
  ExactEvent event{};
  const ExactQuery* query = ExactQueryInternal();
  ASSERT_EQ(query->drain(&event, 1), 1U);
  EXPECT_EQ(event.start_timestamp_ns, 10U);
  EXPECT_EQ(event.end_timestamp_ns, 20U);
  EXPECT_EQ(event.callback_registration_id, 73U);
  EXPECT_EQ(event.kind, static_cast<uint8_t>(ExactEventKind::callback_span));
  EXPECT_EQ(query->snapshot().pending, 0U);
}

TEST_F(DdsCommTraceTest, ExactValidatorAccountingFailsClosed) {
  SetExactEnabledForTest(true);
  ExactSubmitCallback(1, 2, Delivery(1), 0);
  ExactUnsupportedAsync();
  ExactUnsupportedMultiHandler();
  const ExactSnapshot snapshot = ExactQueryInternal()->snapshot();
  EXPECT_EQ(snapshot.pending, 0U);
  EXPECT_EQ(snapshot.missing_registration, 1U);
  EXPECT_EQ(snapshot.unsupported_async, 1U);
  EXPECT_EQ(snapshot.unsupported_multi_handler, 1U);
}

TEST_F(DdsCommTraceTest, GuidCopyAndIdentityValidationAreExplicit) {
  const Guid writer = KnownGuid(7);
  SampleKey key{};
  ASSERT_TRUE(MakeSampleKey(Identity(7, 2, 9), &key));
  for (size_t i = 0; i < 16; ++i) EXPECT_EQ(key.writer_guid[i], 7 + i);
  EXPECT_EQ(key.sequence_high, 2);
  EXPECT_EQ(key.sequence_low, 9U);
  EXPECT_TRUE(WriterMatches(key, writer));
  EXPECT_FALSE(WriterMatches(key, KnownGuid(8)));

  SampleKey unknown_guid{};
  unknown_guid.sequence_high = 0;
  unknown_guid.sequence_low = 1;
  EXPECT_FALSE(SampleKeyValid(unknown_guid));
  EXPECT_FALSE(MakeSampleKey(Identity(7, -1, 0), &key));

  DeliveryKey delivery{};
  EXPECT_TRUE(MakeDeliveryKey(Identity(7, 0, 1), KnownGuid(40), &delivery));
  EXPECT_FALSE(MakeDeliveryKey(Identity(7, 0, 1), Guid::unknown(), &delivery));
}

TEST_F(DdsCommTraceTest, TopicHashIsFnv1a32) {
  constexpr char value[] = "rt/chatter";
  EXPECT_EQ(TopicHash(value, sizeof(value) - 1), 0x76f1d565U);
  EXPECT_EQ(TopicHash("", 0), 2166136261U);
}

TEST_F(DdsCommTraceTest, SamplingUsesTheCompleteSampleKeyDeterministically) {
  const SampleKey key = Key(123);
  EXPECT_EQ(ShouldSample(key), ShouldSample(key));
  bool guid_changes_decision = false;
  bool high_changes_decision = false;
  bool low_changes_decision = false;
  for (uint32_t value = 1; value < 256; ++value) {
    SampleKey changed_guid = key;
    changed_guid.writer_guid[15] = static_cast<uint8_t>(value);
    guid_changes_decision |= ShouldSample(key) != ShouldSample(changed_guid);
    SampleKey changed_high = key;
    changed_high.sequence_high = static_cast<int32_t>(value);
    high_changes_decision |= ShouldSample(key) != ShouldSample(changed_high);
    SampleKey changed_low = key;
    changed_low.sequence_low = value;
    low_changes_decision |= ShouldSample(key) != ShouldSample(changed_low);
  }
  EXPECT_TRUE(guid_changes_decision);
  EXPECT_TRUE(high_changes_decision);
  EXPECT_TRUE(low_changes_decision);
}

TEST_F(DdsCommTraceTest, RuntimeOffGuardDoesNotReadClockOrTouchRing) {
  clock_calls.store(0, std::memory_order_relaxed);
  SetClockForTest(&CountingClock);
  SetModeForTest(RuntimeMode::off);
  if (active()) {
    Submit(MakeEvent(NowNs(), NowNs(), Key(1), 0, EventKind::publish_write));
  }
  InvalidIdentity();
  UnsupportedAsync();
  EXPECT_EQ(clock_calls.load(std::memory_order_relaxed), 0U);
  const SnapshotV1 snapshot = ibmw_dds_comm_trace_query_v1()->snapshot();
  EXPECT_EQ(snapshot.pending, 0U);
  EXPECT_EQ(snapshot.invalid_identity, 0U);
  EXPECT_EQ(snapshot.unsupported_async, 0U);
}

TEST_F(DdsCommTraceTest, MpscProducersDrainEveryCommittedRecord) {
  constexpr size_t kProducers = 8;
  constexpr size_t kPerProducer = 300;
  std::vector<std::thread> producers;
  for (size_t producer = 0; producer < kProducers; ++producer) {
    producers.emplace_back([producer] {
      for (size_t i = 0; i < kPerProducer; ++i) {
        const uint32_t sequence = static_cast<uint32_t>(producer * kPerProducer + i + 1);
        Submit(MakeEvent(sequence, sequence + 1, Key(sequence), 7,
                         EventKind::publish_write));
      }
    });
  }
  for (auto& producer : producers) producer.join();

  std::vector<Event> events(kProducers * kPerProducer);
  const auto* api = ibmw_dds_comm_trace_query_v1();
  ASSERT_EQ(api->drain(events.data(), events.size()), events.size());
  EXPECT_EQ(api->snapshot().pending, 0U);
  std::sort(events.begin(), events.end(), [](const Event& lhs, const Event& rhs) {
    return lhs.sequence_low < rhs.sequence_low;
  });
  for (size_t i = 0; i < events.size(); ++i) EXPECT_EQ(events[i].sequence_low, i + 1);
}

TEST_F(DdsCommTraceTest, FullRingDropsWholeRecordAndSealRejectsProducer) {
  for (size_t i = 0; i < kRingCapacity; ++i) {
    Submit(MakeEvent(i, i + 1, Key(i + 1), 0, EventKind::take_receive));
  }
  Submit(MakeEvent(0, 1, Key(9999), 0, EventKind::take_receive));
  auto snapshot = ibmw_dds_comm_trace_query_v1()->snapshot();
  EXPECT_EQ(snapshot.pending, kRingCapacity);
  EXPECT_EQ(snapshot.dropped, 1U);

  ibmw_dds_comm_trace_query_v1()->seal();
  Submit(MakeEvent(0, 1, Key(10000), 0, EventKind::take_receive));
  snapshot = ibmw_dds_comm_trace_query_v1()->snapshot();
  EXPECT_TRUE(snapshot.sealed);
  EXPECT_EQ(snapshot.dropped, 1U);
  EXPECT_EQ(snapshot.active_producers, 0U);

  std::vector<Event> events(kRingCapacity);
  EXPECT_EQ(ibmw_dds_comm_trace_query_v1()->drain(events.data(), events.size()),
            kRingCapacity);
  const SnapshotV1 finalized = ibmw_dds_comm_trace_query_v1()->snapshot();
  EXPECT_TRUE(finalized.sealed);
  EXPECT_EQ(finalized.active_producers, 0U);
  EXPECT_EQ(finalized.pending, 0U);
  EXPECT_EQ(finalized.dropped, 1U);
}

TEST_F(DdsCommTraceTest, InitializeAndShutdownAreIdempotent) {
  setenv("IBMW_COMM_TRACE", "full", 1);
  setenv("IBMW_COMM_TRACE_SINK", "none", 1);
  const QueryV1* api = ibmw_dds_comm_trace_query_v1();
  api->initialize();
  api->initialize();
  EXPECT_FALSE(api->snapshot().sealed);

  Shutdown();
  EXPECT_FALSE(api->snapshot().sealed);
  Shutdown();
  EXPECT_TRUE(api->snapshot().sealed);
}

TEST_F(DdsCommTraceTest, ShutdownClosesGateBeforePreEntrySubmitCanPersistAccounting) {
  setenv("IBMW_COMM_TRACE", "full", 1);
  setenv("IBMW_COMM_TRACE_SINK", "none", 1);
  ASSERT_TRUE(Initialize());
  const SnapshotV2 before = ibmw_dds_comm_trace_query_v2()->snapshot();
  gate_hook_entered.store(false);
  gate_hook_release.store(false);
  SetProducerGateHookForTest(&BlockingGateHook);
  std::thread producer([] {
    Submit(MakeEvent(1, 2, Key(1), 0, EventKind::publish_write));
  });
  while (!gate_hook_entered.load(std::memory_order_acquire)) std::this_thread::yield();
  Shutdown();
  gate_hook_release.store(true, std::memory_order_release);
  producer.join();
  SetProducerGateHookForTest(nullptr);
  const SnapshotV2 after = ibmw_dds_comm_trace_query_v2()->snapshot();
  EXPECT_EQ(after.eligible, before.eligible);
  EXPECT_EQ(after.sampled_out, before.sampled_out);
  EXPECT_EQ(after.accepted, before.accepted);
  EXPECT_EQ(after.dropped, before.dropped);
  EXPECT_EQ(after.pending, before.pending);
  EXPECT_EQ(after.active_producers, 0U);
}

TEST_F(DdsCommTraceTest, SnapshotSealOrderingIncludesPausedAdmittedProducer) {
  setenv("IBMW_COMM_TRACE", "full", 1);
  setenv("IBMW_COMM_TRACE_SINK", "none", 1);
  ASSERT_TRUE(Initialize());
  admitted_gate_hook_entered.store(false);
  admitted_gate_hook_release.store(false);
  SetProducerAdmittedHookForTest(&BlockingAdmittedGateHook);
  std::thread producer([] {
    Submit(MakeEvent(1, 2, Key(1), 0, EventKind::publish_write));
  });
  while (!admitted_gate_hook_entered.load(std::memory_order_acquire)) std::this_thread::yield();

  const QueryV1* api1 = ibmw_dds_comm_trace_query_v1();
  const QueryV2* api2 = ibmw_dds_comm_trace_query_v2();
  api1->seal();
  const SnapshotV1 snapshot1 = api1->snapshot();
  const SnapshotV2 snapshot2 = api2->snapshot();
  EXPECT_TRUE(snapshot1.sealed);
  EXPECT_EQ(snapshot1.active_producers, 1U);
  EXPECT_TRUE(snapshot2.sealed);
  EXPECT_EQ(snapshot2.active_producers, 1U);

  admitted_gate_hook_release.store(true, std::memory_order_release);
  producer.join();
  SetProducerAdmittedHookForTest(nullptr);
  EXPECT_EQ(api1->snapshot().active_producers, 0U);
}

TEST_F(DdsCommTraceTest, ShutdownClosesGateBeforePreEntryInvalidIdentityCanIncrement) {
  setenv("IBMW_COMM_TRACE", "full", 1);
  setenv("IBMW_COMM_TRACE_SINK", "none", 1);
  ASSERT_TRUE(Initialize());
  const SnapshotV2 before = ibmw_dds_comm_trace_query_v2()->snapshot();
  gate_hook_entered.store(false);
  gate_hook_release.store(false);
  SetProducerGateHookForTest(&BlockingGateHook);
  std::thread producer([] { InvalidIdentity(); });
  while (!gate_hook_entered.load(std::memory_order_acquire)) std::this_thread::yield();
  Shutdown();
  gate_hook_release.store(true, std::memory_order_release);
  producer.join();
  SetProducerGateHookForTest(nullptr);
  const SnapshotV2 after = ibmw_dds_comm_trace_query_v2()->snapshot();
  EXPECT_EQ(after.invalid_identity, before.invalid_identity);
  EXPECT_EQ(after.active_producers, 0U);
}

TEST_F(DdsCommTraceTest, ShutdownClosesGateBeforePreEntryUnsupportedAsyncCanIncrement) {
  setenv("IBMW_COMM_TRACE", "full", 1);
  setenv("IBMW_COMM_TRACE_SINK", "none", 1);
  ASSERT_TRUE(Initialize());
  const SnapshotV2 before = ibmw_dds_comm_trace_query_v2()->snapshot();
  gate_hook_entered.store(false);
  gate_hook_release.store(false);
  SetProducerGateHookForTest(&BlockingGateHook);
  std::thread producer([] { UnsupportedAsync(); });
  while (!gate_hook_entered.load(std::memory_order_acquire)) std::this_thread::yield();
  Shutdown();
  gate_hook_release.store(true, std::memory_order_release);
  producer.join();
  SetProducerGateHookForTest(nullptr);
  const SnapshotV2 after = ibmw_dds_comm_trace_query_v2()->snapshot();
  EXPECT_EQ(after.unsupported_async, before.unsupported_async);
  EXPECT_EQ(after.active_producers, 0U);
}

TEST_F(DdsCommTraceTest, ShutdownClosesGateBeforePreEntryExactSubmitCanPersist) {
  TempDirectory directory;
  ASSERT_GE(directory.fd(), 0);
  SetManagedEnvironment(directory);
  setenv("MW_PERF_CAPABILITY_ID", kExactCapabilityId, 1);
  setenv("MW_PERF_EXACT_REQUIRED", "1", 1);
  ASSERT_TRUE(Initialize());
  const ExactSnapshot before = ExactQueryInternal()->snapshot();
  gate_hook_entered.store(false);
  gate_hook_release.store(false);
  SetProducerGateHookForTest(&BlockingGateHook);
  std::thread producer([] { ExactSubmitCallback(1, 2, Delivery(1), 73); });
  while (!gate_hook_entered.load(std::memory_order_acquire)) std::this_thread::yield();
  Shutdown();
  gate_hook_release.store(true, std::memory_order_release);
  producer.join();
  SetProducerGateHookForTest(nullptr);
  const ExactSnapshot after = ExactQueryInternal()->snapshot();
  EXPECT_EQ(after.pending, before.pending);
  EXPECT_EQ(after.dropped, before.dropped);
  EXPECT_EQ(after.invalid_identity, before.invalid_identity);
}

TEST_F(DdsCommTraceTest, ShutdownWaitsForAdmittedExactSubmitBeforeExport) {
  TempDirectory directory;
  ASSERT_GE(directory.fd(), 0);
  SetManagedEnvironment(directory);
  setenv("MW_PERF_CAPABILITY_ID", kExactCapabilityId, 1);
  setenv("MW_PERF_EXACT_REQUIRED", "1", 1);
  ASSERT_TRUE(Initialize());
  ExactSubmitCallback(10, 20, Delivery(77), 73);
  admitted_gate_hook_entered.store(false);
  admitted_gate_hook_release.store(false);
  SetProducerAdmittedHookForTest(&BlockingAdmittedGateHook);
  std::thread producer([] { ExactSubmitCallback(30, 40, Delivery(78), 74); });
  while (!admitted_gate_hook_entered.load(std::memory_order_acquire)) std::this_thread::yield();
  std::thread finalizer([] { Shutdown(); });
  while (!ibmw_dds_comm_trace_query_v1()->snapshot().sealed) std::this_thread::yield();
  EXPECT_EQ(ExactQueryInternal()->snapshot().pending, 1U);
  admitted_gate_hook_release.store(true, std::memory_order_release);
  producer.join();
  finalizer.join();
  SetProducerAdmittedHookForTest(nullptr);

  const auto files = Files(directory.path());
  const auto stream = std::find_if(files.begin(), files.end(),
      [](const std::string& name) { return EndsWith(name, ".ibce"); });
  ASSERT_NE(stream, files.end());
  const auto bytes = ReadFile(directory.path() + "/" + *stream);
  ASSERT_EQ(bytes.size(), 160U + sizeof(ExactEvent));
  EXPECT_EQ(GetLe64(bytes.data() + 80), 1U);
  EXPECT_EQ(GetLe64(bytes.data() + 88), 1U);
  const auto seal = std::find_if(files.begin(), files.end(),
      [](const std::string& name) { return EndsWith(name, ".exact.seal.json"); });
  ASSERT_NE(seal, files.end());
  const auto seal_bytes = ReadFile(directory.path() + "/" + *seal);
  EXPECT_NE(std::string(seal_bytes.begin(), seal_bytes.end()).find("\"finalized\":false"),
            std::string::npos);
}

TEST_F(DdsCommTraceTest, ShutdownWaitsForSynchronousExactCallbackBeforeCounterSeal) {
  TempDirectory directory;
  ASSERT_GE(directory.fd(), 0);
  SetManagedEnvironment(directory);
  setenv("MW_PERF_CAPABILITY_ID", kExactCapabilityId, 1);
  setenv("MW_PERF_EXACT_REQUIRED", "1", 1);
  ASSERT_TRUE(Initialize());

  std::atomic<bool> callback_updated{false};
  std::atomic<bool> callback_release{false};
  std::thread callback([&] {
    ASSERT_TRUE(ExactCallbackBegin());
    ExactSubmitCallback(10, 20, Delivery(77), 73);
    ExactInvalidIdentity();
    ExactMissingRegistration();
    ExactUnsupportedAsync();
    ExactUnsupportedMultiHandler();
    callback_updated.store(true, std::memory_order_release);
    while (!callback_release.load(std::memory_order_acquire)) std::this_thread::yield();
    ExactCallbackEnd();
  });
  while (!callback_updated.load(std::memory_order_acquire)) std::this_thread::yield();

  std::thread finalizer([] { Shutdown(); });
  while (!ibmw_dds_comm_trace_query_v1()->snapshot().sealed) std::this_thread::yield();
  EXPECT_EQ(ibmw_dds_comm_trace_query_v1()->snapshot().active_producers, 1U);
  callback_release.store(true, std::memory_order_release);
  callback.join();
  finalizer.join();

  const auto files = Files(directory.path());
  const auto stream = std::find_if(files.begin(), files.end(),
      [](const std::string& name) { return EndsWith(name, ".ibce"); });
  const auto seal = std::find_if(files.begin(), files.end(),
      [](const std::string& name) { return EndsWith(name, ".exact.seal.json"); });
  ASSERT_NE(stream, files.end());
  ASSERT_NE(seal, files.end());
  const auto bytes = ReadFile(directory.path() + "/" + *stream);
  ASSERT_EQ(bytes.size(), 160U + sizeof(ExactEvent));
  EXPECT_EQ(GetLe64(bytes.data() + 80), 1U);
  EXPECT_EQ(GetLe64(bytes.data() + 88), 0U);
  EXPECT_EQ(GetLe64(bytes.data() + 96), 1U);
  EXPECT_EQ(GetLe64(bytes.data() + 104), 1U);
  EXPECT_EQ(GetLe64(bytes.data() + 112), 1U);
  EXPECT_EQ(GetLe64(bytes.data() + 120), 1U);
  const auto seal_bytes = ReadFile(directory.path() + "/" + *seal);
  const std::string seal_json(seal_bytes.begin(), seal_bytes.end());
  EXPECT_NE(seal_json.find("\"record_count\":1"), std::string::npos);
  EXPECT_NE(seal_json.find("\"invalid_identity\":1"), std::string::npos);
  EXPECT_NE(seal_json.find("\"missing_registration\":1"), std::string::npos);
  EXPECT_NE(seal_json.find("\"unsupported_async\":1"), std::string::npos);
  EXPECT_NE(seal_json.find("\"unsupported_multi_handler\":1"), std::string::npos);
  EXPECT_NE(seal_json.find("\"pending\":0"), std::string::npos);
}

TEST_F(DdsCommTraceTest, QuerySealClosesGateAndStabilizesAllProducerAccounting) {
  Submit(MakeEvent(1, 2, Key(1), 0, EventKind::publish_write));
  InvalidIdentity();
  UnsupportedAsync();
  const QueryV1* api = ibmw_dds_comm_trace_query_v1();
  api->seal();

  SnapshotV2 sealed_snapshot{};
  do {
    sealed_snapshot = ibmw_dds_comm_trace_query_v2()->snapshot();
  } while (sealed_snapshot.sealed == 0 || sealed_snapshot.active_producers != 0);

  Submit(MakeEvent(2, 3, Key(2), 0, EventKind::publish_write));
  InvalidIdentity();
  UnsupportedAsync();
  const SnapshotV2 after = ibmw_dds_comm_trace_query_v2()->snapshot();
  EXPECT_EQ(after.dropped, sealed_snapshot.dropped);
  EXPECT_EQ(after.invalid_identity, sealed_snapshot.invalid_identity);
  EXPECT_EQ(after.unsupported_async, sealed_snapshot.unsupported_async);
  EXPECT_EQ(after.pending, sealed_snapshot.pending);
  EXPECT_EQ(after.eligible, sealed_snapshot.eligible);
  EXPECT_EQ(after.sampled_out, sealed_snapshot.sampled_out);
  EXPECT_EQ(after.accepted, sealed_snapshot.accepted);
  EXPECT_EQ(after.active_producers, 0U);
}

TEST_F(DdsCommTraceTest, ManagedRequestSealClosesGateAndLeavesShutdownToOwner) {
  TempDirectory directory;
  ASSERT_GE(directory.fd(), 0);
  SetManagedEnvironment(directory);
  ASSERT_TRUE(Initialize());
  const QueryV2* api = ibmw_dds_comm_trace_query_v2();
  api->request_seal();
  const SnapshotV2 sealed_snapshot = api->snapshot();
  ASSERT_TRUE(sealed_snapshot.sealed);
  EXPECT_TRUE(sealed_snapshot.managed);
  EXPECT_EQ(sealed_snapshot.owners, 1U);
  const auto files_before_shutdown = Files(directory.path());
  EXPECT_TRUE(std::none_of(files_before_shutdown.begin(), files_before_shutdown.end(),
      [](const std::string& name) { return EndsWith(name, ".seal.json"); }));

  InvalidIdentity();
  UnsupportedAsync();
  const SnapshotV2 after = api->snapshot();
  EXPECT_EQ(after.invalid_identity, sealed_snapshot.invalid_identity);
  EXPECT_EQ(after.unsupported_async, sealed_snapshot.unsupported_async);

  Shutdown();
  EXPECT_EQ(api->snapshot().owners, 0U);
  const auto files = Files(directory.path());
  EXPECT_TRUE(std::any_of(files.begin(), files.end(),
      [](const std::string& name) { return EndsWith(name, ".seal.json"); }));
}

TEST_F(DdsCommTraceTest, SampledAccountingUsesOneDecisionForEveryStage) {
  SetModeForTest(RuntimeMode::sampled);
  const SampleKey key = Key(1234);
  Submit(MakeEvent(1, 2, key, 1, EventKind::publish_write));
  Submit(MakeEvent(2, 3, key, 1, EventKind::take_receive));
  Submit(MakeEvent(3, 4, key, 1, EventKind::dispatch));
  const SnapshotV2 snapshot = ibmw_dds_comm_trace_query_v2()->snapshot();
  EXPECT_EQ(snapshot.eligible, 3U);
  if (ShouldSample(key)) {
    EXPECT_EQ(snapshot.accepted, 3U);
    EXPECT_EQ(snapshot.sampled_out, 0U);
  } else {
    EXPECT_EQ(snapshot.accepted, 0U);
    EXPECT_EQ(snapshot.sampled_out, 3U);
  }
}

TEST_F(DdsCommTraceTest, ManagedExporterBindsMetaHeaderPayloadAndSeal) {
  TempDirectory directory;
  ASSERT_GE(directory.fd(), 0);
  SetManagedEnvironment(directory);
  ASSERT_TRUE(Initialize());
  for (uint32_t i = 1; i <= 1025; ++i) {
    Submit(MakeEvent(i, i + 1, Delivery(i), 0x11223344U, EventKind::dispatch));
  }
  Shutdown();

  const auto files = Files(directory.path());
  const auto meta_it = std::find_if(files.begin(), files.end(),
      [](const std::string& name) { return EndsWith(name, ".meta.json"); });
  const auto seal_it = std::find_if(files.begin(), files.end(),
      [](const std::string& name) { return EndsWith(name, ".seal.json"); });
  ASSERT_NE(meta_it, files.end());
  ASSERT_NE(seal_it, files.end());
  const auto meta = ReadFile(directory.path() + "/" + *meta_it);
  const std::string meta_json(meta.begin(), meta.end());
  EXPECT_EQ(JsonKeyCount(meta_json), 14U);
  EXPECT_NE(meta_json.find("\"schema\":\"mw_perf.dds_comm_trace.meta.v1\""), std::string::npos);
  EXPECT_NE(meta_json.find("\"run_id\":\"run-1\""), std::string::npos);
  EXPECT_NE(meta_json.find("\"deployment_mode\":\"managed\""), std::string::npos);
  EXPECT_NE(meta_json.find("\"topology_generation\":\"generation-7\""), std::string::npos);
  EXPECT_NE(meta_json.find("\"trace_mode\":\"full\""), std::string::npos);
  EXPECT_NE(meta_json.find("\"sampling_algorithm\":0"), std::string::npos);
  EXPECT_NE(meta_json.find("\"sampling_permyriad\":10000"), std::string::npos);
  EXPECT_EQ(meta_json.find("\"event_schema\""), std::string::npos);
  EXPECT_EQ(meta_json.find("\"runtime_changed\""), std::string::npos);

  uint64_t expected_ordinal = 0;
  size_t segment_count = 0;
  const auto expected_meta_sha = Digest(meta.data(), meta.size());
  for (const auto& name : files) {
    if (!EndsWith(name, ".ibct")) continue;
    const auto bytes = ReadFile(directory.path() + "/" + name);
    ASSERT_GE(bytes.size(), 256U);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(bytes.data()), 7), "IBMWDCT");
    EXPECT_EQ(bytes[8], 1U);
    EXPECT_EQ(bytes[10], 0U);
    EXPECT_EQ(bytes[11], 1U);
    EXPECT_EQ(bytes[14], 64U);
    EXPECT_EQ(bytes[16], 1U);
    EXPECT_TRUE(std::all_of(bytes.begin() + 17, bytes.begin() + 24,
                            [](uint8_t value) { return value == 0; }));
    EXPECT_TRUE(std::equal(expected_meta_sha.begin(), expected_meta_sha.end(), bytes.begin() + 40));
    EXPECT_EQ(GetLe64(bytes.data() + 88), expected_ordinal);
    const uint64_t records = GetLe64(bytes.data() + 96);
    EXPECT_LE(records, 1024U);
    EXPECT_EQ(bytes.size(), 256U + records * 64U);
    const auto expected_payload_sha = Digest(bytes.data() + 256, records * 64U);
    EXPECT_TRUE(std::equal(expected_payload_sha.begin(), expected_payload_sha.end(), bytes.begin() + 136));
    EXPECT_EQ(bytes[168], static_cast<uint8_t>(RuntimeMode::full));
    EXPECT_EQ(bytes[169], 0U);
    EXPECT_EQ(GetLe16(bytes.data() + 170), 10000U);
    EXPECT_TRUE(std::all_of(bytes.begin() + 172, bytes.begin() + 256,
                            [](uint8_t value) { return value == 0; }));
    for (uint64_t record = 0; record < records; ++record) {
      const uint8_t* event = bytes.data() + 256 + record * 64;
      EXPECT_TRUE(std::any_of(event + 40, event + 56,
                              [](uint8_t value) { return value != 0; }));
      EXPECT_EQ(event[60], static_cast<uint8_t>(EventKind::dispatch));
      EXPECT_EQ(event[61], 0U);
    }
    expected_ordinal += records;
    ++segment_count;
  }
  EXPECT_EQ(expected_ordinal, 1025U);
  EXPECT_GE(segment_count, 2U);
  const auto seal = ReadFile(directory.path() + "/" + *seal_it);
  const std::string seal_json(seal.begin(), seal.end());
  EXPECT_EQ(JsonKeyCount(seal_json), 23U);
  EXPECT_NE(seal_json.find("\"finalized\":true"), std::string::npos);
  EXPECT_NE(seal_json.find("\"segment_count\":" + std::to_string(segment_count)),
            std::string::npos);
  EXPECT_NE(seal_json.find("\"record_count\":1025"), std::string::npos);
  EXPECT_NE(seal_json.find("\"exported\":1025"), std::string::npos);
  EXPECT_NE(seal_json.find("\"producer_pid\":"), std::string::npos);
  EXPECT_NE(seal_json.find("\"producer_start_time_ticks\":"), std::string::npos);
  EXPECT_EQ(seal_json.find("\"segments\""), std::string::npos);
  EXPECT_EQ(seal_json.find("\"exported_records\""), std::string::npos);
  EXPECT_TRUE(std::none_of(files.begin(), files.end(),
      [](const std::string& name) { return EndsWith(name, ".part"); }));
}

TEST_F(DdsCommTraceTest, ManagedExactExporterRoundTripsIndependentArtifact) {
  TempDirectory directory;
  ASSERT_GE(directory.fd(), 0);
  SetManagedEnvironment(directory);
  setenv("MW_PERF_CAPABILITY_ID", kExactCapabilityId, 1);
  setenv("MW_PERF_EXACT_REQUIRED", "1", 1);
  ASSERT_TRUE(Initialize());
  ExactSubmitCallback(10, 20, Delivery(77), 73);
  Submit(MakeEvent(10, 20, Delivery(77), 1, EventKind::dispatch));
  Shutdown();

  const auto files = Files(directory.path());
  const auto exact_meta = std::find_if(files.begin(), files.end(),
      [](const std::string& name) { return EndsWith(name, ".exact.meta.json"); });
  const auto exact_stream = std::find_if(files.begin(), files.end(),
      [](const std::string& name) { return EndsWith(name, ".ibce"); });
  const auto exact_seal = std::find_if(files.begin(), files.end(),
      [](const std::string& name) { return EndsWith(name, ".exact.seal.json"); });
  ASSERT_NE(exact_meta, files.end());
  ASSERT_NE(exact_stream, files.end());
  ASSERT_NE(exact_seal, files.end());
  EXPECT_TRUE(std::any_of(files.begin(), files.end(),
      [](const std::string& name) { return EndsWith(name, ".ibct"); }));
  const auto stream = ReadFile(directory.path() + "/" + *exact_stream);
  ASSERT_EQ(stream.size(), 160U + sizeof(ExactEvent));
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(stream.data()), 7), "IBMWDCE");
  EXPECT_EQ(GetLe16(stream.data() + 12), sizeof(ExactEvent));
  EXPECT_EQ(GetLe64(stream.data() + 80), 1U);
  const auto seal = ReadFile(directory.path() + "/" + *exact_seal);
  const std::string seal_json(seal.begin(), seal.end());
  EXPECT_NE(seal_json.find("\"finalized\":true"), std::string::npos);
  EXPECT_NE(seal_json.find("\"record_count\":1"), std::string::npos);
  EXPECT_NE(seal_json.find("\"dropped\":0"), std::string::npos);
}

TEST_F(DdsCommTraceTest, TwoOwnersAndBindingConflictDoNotSealEarly) {
  TempDirectory directory;
  SetManagedEnvironment(directory);
  ASSERT_TRUE(Initialize());
  ASSERT_TRUE(Initialize());
  EXPECT_EQ(ibmw_dds_comm_trace_query_v2()->snapshot().owners, 2U);
  setenv("MW_PERF_PATH_ID", "conflicting-path", 1);
  EXPECT_FALSE(Initialize());
  Shutdown();
  EXPECT_FALSE(ibmw_dds_comm_trace_query_v1()->snapshot().sealed);
  const auto before_final_release = Files(directory.path());
  EXPECT_TRUE(std::none_of(before_final_release.begin(), before_final_release.end(),
      [](const std::string& name) { return EndsWith(name, ".seal.json"); }));
  Shutdown();
  EXPECT_TRUE(ibmw_dds_comm_trace_query_v1()->snapshot().sealed);
}

TEST_F(DdsCommTraceTest, ManagedInitializeRestoresCloexecOnSourceDirectoryFd) {
  TempDirectory directory;
  SetManagedEnvironment(directory);
  const int flags = fcntl(directory.fd(), F_GETFD);
  ASSERT_GE(flags, 0);
  ASSERT_EQ(fcntl(directory.fd(), F_SETFD, flags & ~FD_CLOEXEC), 0);
  ASSERT_TRUE(Initialize());
  EXPECT_NE(fcntl(directory.fd(), F_GETFD) & FD_CLOEXEC, 0);
  Shutdown();
}

TEST_F(DdsCommTraceTest, WriterFailureLeavesPartAndProducesNoFalseFinalization) {
  TempDirectory directory;
  SetManagedEnvironment(directory);
  ASSERT_TRUE(Initialize());
  SetWriterFailureForTest(true);
  Submit(MakeEvent(1, 2, Delivery(1), 0, EventKind::dispatch));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  SetWriterFailureForTest(false);
  Shutdown();
  const SnapshotV2 snapshot = ibmw_dds_comm_trace_query_v2()->snapshot();
  EXPECT_TRUE(snapshot.io_error);
  EXPECT_EQ(snapshot.export_discarded, 1U);
  const auto files = Files(directory.path());
  EXPECT_TRUE(std::any_of(files.begin(), files.end(),
      [](const std::string& name) { return EndsWith(name, ".part"); }));
  const auto seal_it = std::find_if(files.begin(), files.end(),
      [](const std::string& name) { return EndsWith(name, ".seal.json"); });
  ASSERT_NE(seal_it, files.end());
  const auto seal = ReadFile(directory.path() + "/" + *seal_it);
  EXPECT_NE(std::string(seal.begin(), seal.end()).find("\"finalized\":false"), std::string::npos);
}

TEST_F(DdsCommTraceTest, DirectorySyncFailureRollsBackVisibleSeal) {
  TempDirectory directory;
  SetManagedEnvironment(directory);
  ASSERT_TRUE(Initialize());
  Submit(MakeEvent(1, 2, Delivery(1), 0, EventKind::dispatch));
  for (size_t attempt = 0;
       attempt < 100 && ibmw_dds_comm_trace_query_v2()->snapshot().exported == 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(ibmw_dds_comm_trace_query_v2()->snapshot().exported, 1U);
  SetDirectorySyncFailureForTest(true);
  Shutdown();
  EXPECT_TRUE(ibmw_dds_comm_trace_query_v2()->snapshot().io_error);
  const auto files = Files(directory.path());
  EXPECT_TRUE(std::none_of(files.begin(), files.end(),
      [](const std::string& name) { return EndsWith(name, ".seal.json"); }));
}

TEST_F(DdsCommTraceTest, ManagedWorkerContinuouslyDrainsWithoutRingOverflow) {
  TempDirectory directory;
  SetManagedEnvironment(directory);
  ASSERT_TRUE(Initialize());
  constexpr uint32_t kRecords = 5000;
  for (uint32_t i = 1; i <= kRecords; ++i) {
    Submit(MakeEvent(i, i + 1, Key(i), 0, EventKind::take_receive));
    if (i % 100 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Shutdown();
  const SnapshotV2 snapshot = ibmw_dds_comm_trace_query_v2()->snapshot();
  EXPECT_EQ(snapshot.dropped, 0U);
  EXPECT_EQ(snapshot.exported, kRecords);
  EXPECT_EQ(snapshot.pending, 0U);
}

TEST_F(DdsCommTraceTest, RuntimeOffIgnoresMalformedManagedBinding) {
  SetModeForTest(RuntimeMode::off);
  setenv("IBMW_COMM_TRACE", "off", 1);
  setenv("IBMW_COMM_TRACE_SINK", "managed", 1);
  setenv("MW_PERF_COMM_TRACE_DIR_FD", "not-a-fd", 1);
  EXPECT_TRUE(Initialize());
  const SnapshotV2 snapshot = ibmw_dds_comm_trace_query_v2()->snapshot();
  EXPECT_EQ(snapshot.owners, 0U);
  EXPECT_FALSE(snapshot.managed);
}

TEST_F(DdsCommTraceTest, ConcurrentSealFinalizationProducesStableEvidence) {
  constexpr size_t kRounds = 25;
  constexpr size_t kProducers = 8;
  constexpr size_t kPerProducer = 500;
  const QueryV1* api = ibmw_dds_comm_trace_query_v1();

  for (size_t round = 0; round < kRounds; ++round) {
    ResetForTest();
    SetModeForTest(RuntimeMode::full);
    std::atomic<bool> start{false};
    std::vector<std::thread> producers;
    for (size_t producer = 0; producer < kProducers; ++producer) {
      producers.emplace_back([&, producer] {
        while (!start.load(std::memory_order_acquire)) {}
        for (size_t i = 0; i < kPerProducer; ++i) {
          const uint32_t sequence = static_cast<uint32_t>(
              producer * kPerProducer + i + 1);
          Submit(MakeEvent(sequence, sequence + 1, Key(sequence), 0,
                           EventKind::take_receive));
        }
      });
    }
    std::thread sealer([&] {
      while (!start.load(std::memory_order_acquire)) {}
      api->seal();
    });
    start.store(true, std::memory_order_release);
    for (auto& producer : producers) producer.join();
    sealer.join();

    SnapshotV1 quiesced{};
    do {
      quiesced = api->snapshot();
    } while (quiesced.sealed == 0 || quiesced.active_producers != 0);

    std::array<Event, 256> events{};
    while (api->drain(events.data(), events.size()) != 0) {}
    const SnapshotV1 finalized = api->snapshot();
    const SnapshotV1 stable = api->snapshot();
    EXPECT_NE(finalized.sealed, 0U);
    EXPECT_EQ(finalized.pending, 0U);
    EXPECT_EQ(finalized.active_producers, 0U);
    EXPECT_EQ(stable.pending, finalized.pending);
    EXPECT_EQ(stable.dropped, finalized.dropped);
    EXPECT_EQ(stable.invalid_identity, finalized.invalid_identity);
    EXPECT_EQ(stable.unsupported_async, finalized.unsupported_async);
  }
}

TEST_F(DdsCommTraceTest, ConcurrentDrainHasSingleConsumerIntegrity) {
  constexpr size_t kCount = 1000;
  for (size_t i = 0; i < kCount; ++i) {
    Submit(MakeEvent(i, i + 1, Key(i + 1), 0, EventKind::dispatch));
  }
  std::vector<Event> left(kCount);
  std::vector<Event> right(kCount);
  std::atomic<bool> start{false};
  size_t left_count = 0;
  size_t right_count = 0;
  std::thread first([&] {
    while (!start.load(std::memory_order_acquire)) {}
    left_count = ibmw_dds_comm_trace_query_v1()->drain(left.data(), left.size());
  });
  std::thread second([&] {
    while (!start.load(std::memory_order_acquire)) {}
    right_count = ibmw_dds_comm_trace_query_v1()->drain(right.data(), right.size());
  });
  start.store(true, std::memory_order_release);
  first.join();
  second.join();
  EXPECT_EQ(left_count + right_count, kCount);
  EXPECT_EQ(ibmw_dds_comm_trace_query_v1()->snapshot().pending, 0U);
}

TEST_F(DdsCommTraceTest, QueryAbiAndCountersAreVersioned) {
  const QueryV1* api = ibmw_dds_comm_trace_query_v1();
  ASSERT_NE(api, nullptr);
  EXPECT_EQ(api->abi_version, kAbiVersion);
  EXPECT_EQ(sizeof(QueryV1), 48U);
  EXPECT_EQ(api->struct_size, sizeof(QueryV1));
  EXPECT_EQ(api->reserved, 0U);
  ASSERT_NE(api->initialize, nullptr);
  ASSERT_NE(api->drain, nullptr);
  ASSERT_NE(api->snapshot, nullptr);
  ASSERT_NE(api->seal, nullptr);

  InvalidIdentity();
  UnsupportedAsync();
  const SnapshotV1 snapshot = api->snapshot();
  EXPECT_EQ(snapshot.abi_version, kAbiVersion);
  EXPECT_EQ(snapshot.struct_size, sizeof(SnapshotV1));
  EXPECT_EQ(snapshot.invalid_identity, 1U);
  EXPECT_EQ(snapshot.unsupported_async, 1U);
  EXPECT_EQ(snapshot.active_producers, 0U);

  const QueryV2* api2 = ibmw_dds_comm_trace_query_v2();
  ASSERT_NE(api2, nullptr);
  EXPECT_EQ(api2->abi_version, kAbiVersionV2);
  EXPECT_EQ(api2->struct_size, sizeof(QueryV2));
  EXPECT_EQ(api2->snapshot().struct_size, sizeof(SnapshotV2));
}

}  // namespace
}  // namespace ibmw::extensions::dds_extension::comm_trace
