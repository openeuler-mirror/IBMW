#include "dds_ext/internal/dds_comm_trace.h"

#include <openssl/evp.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <mutex>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace ibmw::extensions::dds_extension::comm_trace {
namespace {

static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);

struct Slot final {
  std::atomic<uint64_t> sequence{0};
  Event event{};
};

struct Binding final {
  std::string run_id;
  std::string capability_id;
  std::string path_id;
  std::string deployment_mode;
  std::string topology_sha256;
  std::string topology_generation;
  RuntimeMode mode = RuntimeMode::off;

  bool operator==(const Binding& rhs) const {
    return run_id == rhs.run_id && capability_id == rhs.capability_id &&
           path_id == rhs.path_id && deployment_mode == rhs.deployment_mode &&
           topology_sha256 == rhs.topology_sha256 &&
           topology_generation == rhs.topology_generation && mode == rhs.mode;
  }
};

std::array<Slot, kRingCapacity> slots;
std::atomic<uint64_t> enqueue_position{0};
std::atomic<uint64_t> dequeue_position{0};
std::atomic<uint64_t> dropped_count{0};
std::atomic<uint64_t> invalid_identity_count{0};
std::atomic<uint64_t> unsupported_async_count{0};
std::atomic<uint64_t> eligible_count{0};
std::atomic<uint64_t> sampled_out_count{0};
std::atomic<uint64_t> accepted_count{0};
std::atomic<uint64_t> exported_count{0};
std::atomic<uint64_t> export_discarded_count{0};
constexpr uint64_t kProducerGateClosed = uint64_t{1} << 63;
constexpr uint64_t kProducerGateCountMask = ~kProducerGateClosed;
std::atomic<uint64_t> producer_gate{0};
std::atomic<uint32_t> owner_count{0};
std::atomic<RuntimeMode> runtime_mode{RuntimeMode::off};
std::atomic<bool> sealed{true};
std::atomic<bool> managed{false};
std::atomic<bool> io_error{false};
std::array<ExactEvent, kRingCapacity> exact_events{};
std::atomic<uint64_t> exact_read{0};
std::atomic<uint64_t> exact_write{0};
std::atomic<uint64_t> exact_dropped{0};
std::atomic<uint64_t> exact_invalid_identity{0};
std::atomic<uint64_t> exact_missing_registration{0};
std::atomic<uint64_t> exact_unsupported_async{0};
std::atomic<uint64_t> exact_unsupported_multi_handler{0};
std::atomic<uint64_t> exact_exported{0};
std::atomic<bool> exact_enabled{false};
std::mutex exact_mutex;
std::atomic<bool> stop_worker{false};
std::atomic<bool> writer_failure_for_test{false};
std::atomic<bool> directory_sync_failure_for_test{false};
std::atomic_flag drain_guard = ATOMIC_FLAG_INIT;

std::mutex lifecycle_mutex;
std::mutex worker_mutex;
std::condition_variable worker_wakeup;
std::thread worker;
Binding binding;
int directory_fd = -1;
pid_t process_id = 0;
uint64_t process_start_ticks = 0;
std::array<uint8_t, 16> stream_id{};
std::array<uint8_t, 32> meta_sha256{};
std::array<uint8_t, 32> exact_meta_sha256{};
std::string stream_hex;
std::string prefix;
std::string exact_prefix;
uint32_t next_segment = 0;
uint64_t next_ordinal = 0;
uint64_t last_segment_dropped = 0;

uint64_t SteadyClockNs() noexcept {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::atomic<ClockForTest> clock_source{&SteadyClockNs};
std::atomic<ProducerGateHookForTest> producer_gate_hook_for_test{nullptr};
std::atomic<ProducerGateHookForTest> producer_admitted_hook_for_test{nullptr};
thread_local uint32_t exact_callback_depth = 0;

class ProducerAdmission final {
 public:
  ProducerAdmission() noexcept {
    if (runtime_mode.load(std::memory_order_relaxed) == RuntimeMode::off) return;
    if (const auto hook = producer_gate_hook_for_test.load(std::memory_order_relaxed)) hook();
    uint64_t gate = producer_gate.load(std::memory_order_acquire);
    for (;;) {
      if ((gate & kProducerGateClosed) != 0 ||
          (gate & kProducerGateCountMask) == kProducerGateCountMask) return;
      if (producer_gate.compare_exchange_weak(gate, gate + 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        admitted_ = true;
        if (const auto hook = producer_admitted_hook_for_test.load(std::memory_order_relaxed)) hook();
        return;
      }
    }
  }

  ~ProducerAdmission() {
    if (admitted_) producer_gate.fetch_sub(1, std::memory_order_release);
  }

  explicit operator bool() const noexcept { return admitted_; }

  ProducerAdmission(const ProducerAdmission&) = delete;
  ProducerAdmission& operator=(const ProducerAdmission&) = delete;

 private:
  bool admitted_ = false;
};

thread_local std::unique_ptr<ProducerAdmission> exact_callback_admission;

void CloseProducerGateAndSeal() noexcept {
  producer_gate.fetch_or(kProducerGateClosed, std::memory_order_acq_rel);
  sealed.store(true, std::memory_order_release);
}

void WaitForProducers() noexcept {
  while ((producer_gate.load(std::memory_order_acquire) & kProducerGateCountMask) != 0) {
    std::this_thread::yield();
  }
}

RuntimeMode ParseMode(const char* value) noexcept {
  if (value != nullptr && std::strcmp(value, "full") == 0) return RuntimeMode::full;
  if (value != nullptr && std::strcmp(value, "sampled") == 0) return RuntimeMode::sampled;
  return RuntimeMode::off;
}

uint64_t SampleHash(const SampleKey& key) noexcept {
  uint64_t hash = 14695981039346656037ULL;
  const auto add = [&hash](uint8_t byte) { hash = (hash ^ byte) * 1099511628211ULL; };
  for (uint8_t byte : key.writer_guid) add(byte);
  const uint32_t high = static_cast<uint32_t>(key.sequence_high);
  for (unsigned shift = 0; shift != 32; shift += 8) add(static_cast<uint8_t>(high >> shift));
  for (unsigned shift = 0; shift != 32; shift += 8) add(static_cast<uint8_t>(key.sequence_low >> shift));
  return hash;
}

void CopyBytes(const uint8_t source[16], uint8_t destination[16]) noexcept {
  std::memcpy(destination, source, 16);
}

template <typename T>
void PutLe(uint8_t* output, T value) noexcept {
  using U = std::make_unsigned_t<T>;
  U bits = static_cast<U>(value);
  for (size_t i = 0; i < sizeof(T); ++i) output[i] = static_cast<uint8_t>(bits >> (i * 8));
}

std::array<uint8_t, 32> Sha256(const uint8_t* data, size_t size) noexcept {
  std::array<uint8_t, 32> result{};
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  unsigned int result_size = 0;
  if (context == nullptr || EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context, data, size) != 1 ||
      EVP_DigestFinal_ex(context, result.data(), &result_size) != 1 || result_size != result.size()) {
    result.fill(0);
    io_error.store(true, std::memory_order_relaxed);
  }
  EVP_MD_CTX_free(context);
  return result;
}

bool SafeId(const char* value) noexcept {
  if (value == nullptr) return false;
  const size_t size = std::strlen(value);
  if (size == 0 || size > 128) return false;
  for (size_t i = 0; i < size; ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' || c == '-')) return false;
  }
  return true;
}

bool ParseU64(const char* value, uint64_t* output) noexcept {
  if (value == nullptr || *value == '\0' || *value == '-') return false;
  errno = 0;
  char* end = nullptr;
  const unsigned long long parsed = std::strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0') return false;
  *output = static_cast<uint64_t>(parsed);
  return true;
}

bool LowerHex64(const char* value) noexcept {
  if (value == nullptr || std::strlen(value) != 64) return false;
  for (size_t i = 0; i < 64; ++i) {
    if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return false;
  }
  return true;
}

bool ParseFd(const char* value, int* output) noexcept {
  uint64_t parsed = 0;
  if (!ParseU64(value, &parsed) || parsed > static_cast<uint64_t>(INT32_MAX)) return false;
  *output = static_cast<int>(parsed);
  return true;
}

bool ValidateAndDuplicateDirectory(const char* value, int* output) noexcept {
  int source = -1;
  if (!ParseFd(value, &source)) return false;
  const int status_flags = fcntl(source, F_GETFL);
  if (status_flags < 0 || (status_flags & O_DIRECTORY) == 0) return false;
  struct stat status {};
  if (fstat(source, &status) != 0 || !S_ISDIR(status.st_mode) || status.st_uid != geteuid() ||
      (status.st_mode & (S_IWGRP | S_IWOTH)) != 0) return false;
  const int copy = fcntl(source, F_DUPFD_CLOEXEC, 3);
  if (copy < 0) return false;
  const int source_flags = fcntl(source, F_GETFD);
  if (source_flags < 0 || fcntl(source, F_SETFD, source_flags | FD_CLOEXEC) != 0) {
    close(copy);
    return false;
  }
  *output = copy;
  return true;
}

bool ReadStartTicks(uint64_t* output) noexcept {
  const int fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  std::array<char, 4096> buffer{};
  ssize_t count;
  do { count = read(fd, buffer.data(), buffer.size() - 1); } while (count < 0 && errno == EINTR);
  close(fd);
  if (count <= 0) return false;
  buffer[static_cast<size_t>(count)] = '\0';
  const char* cursor = std::strrchr(buffer.data(), ')');
  if (cursor == nullptr || cursor[1] != ' ') return false;
  cursor += 2;
  // cursor starts at field 3; starttime is field 22.
  for (int field = 3; field < 22; ++field) {
    cursor = std::strchr(cursor, ' ');
    if (cursor == nullptr) return false;
    ++cursor;
  }
  errno = 0;
  char* end = nullptr;
  const unsigned long long value = std::strtoull(cursor, &end, 10);
  if (errno != 0 || end == cursor) return false;
  *output = static_cast<uint64_t>(value);
  return true;
}

bool FillRandom(uint8_t* output, size_t size) noexcept {
  while (size != 0) {
    const ssize_t count = getrandom(output, size, 0);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return false;
    output += count;
    size -= static_cast<size_t>(count);
  }
  return true;
}

std::string Hex(const uint8_t* data, size_t size) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string result(size * 2, '0');
  for (size_t i = 0; i < size; ++i) {
    result[i * 2] = digits[data[i] >> 4];
    result[i * 2 + 1] = digits[data[i] & 15];
  }
  return result;
}

const char* ModeName(RuntimeMode value) noexcept {
  return value == RuntimeMode::full ? "full" : "sampled";
}

uint8_t SamplingAlgorithm(RuntimeMode value) noexcept {
  return value == RuntimeMode::full ? 0 : 1;
}

uint16_t SamplingPermyriad(RuntimeMode value) noexcept {
  return value == RuntimeMode::full ? 10000 : 625;
}

std::string MetaJson() {
  std::ostringstream out;
  out << "{\"schema\":\"mw_perf.dds_comm_trace.meta.v1\","
      << "\"run_id\":\"" << binding.run_id << "\","
      << "\"capability_id\":\"" << binding.capability_id << "\","
      << "\"path_id\":\"" << binding.path_id << "\","
      << "\"deployment_mode\":\"" << binding.deployment_mode << "\","
      << "\"finalized\":true,"
      << "\"stream_id\":\"" << stream_hex << "\","
      << "\"producer_pid\":" << process_id << ','
      << "\"producer_start_time_ticks\":" << process_start_ticks << ','
      << "\"topology_generation\":\"" << binding.topology_generation << "\","
      << "\"topology_sha256\":\"" << binding.topology_sha256 << "\","
      << "\"trace_mode\":\"" << ModeName(binding.mode) << "\","
      << "\"sampling_algorithm\":" << static_cast<unsigned>(SamplingAlgorithm(binding.mode)) << ','
      << "\"sampling_permyriad\":" << SamplingPermyriad(binding.mode) << "}\n";
  return out.str();
}

bool WriteAll(int fd, const uint8_t* data, size_t size) noexcept {
  while (size != 0) {
    if (writer_failure_for_test.load(std::memory_order_relaxed)) { errno = EIO; return false; }
    const ssize_t written = write(fd, data, size);
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    data += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}

bool PublishFile(const std::string& final_name, const uint8_t* data, size_t size) noexcept {
  const std::string part_name = final_name + ".part";
  const int fd = openat(directory_fd, part_name.c_str(),
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  const bool written = WriteAll(fd, data, size);
  const bool synced = written && fdatasync(fd) == 0;
  const bool closed = close(fd) == 0;
  if (!synced || !closed) return false;
  if (renameat(directory_fd, part_name.c_str(), directory_fd, final_name.c_str()) != 0) return false;
  const bool inject_sync_failure = directory_sync_failure_for_test.exchange(false);
  if (!inject_sync_failure && fsync(directory_fd) == 0) return true;

  const bool unlinked = unlinkat(directory_fd, final_name.c_str(), 0) == 0;
  const bool rollback_synced = fsync(directory_fd) == 0;
  if (!unlinked || !rollback_synced) {
    const int poison = openat(directory_fd, part_name.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (poison >= 0) {
      static constexpr uint8_t marker[] = "publication rollback unconfirmed\n";
      (void)WriteAll(poison, marker, sizeof(marker) - 1);
      (void)fdatasync(poison);
      (void)close(poison);
      (void)fsync(directory_fd);
    }
  }
  return false;
}

void SerializeEvent(const Event& event, uint8_t* output) noexcept {
  PutLe(output + 0, event.start_timestamp_ns);
  PutLe(output + 8, event.end_timestamp_ns);
  std::memcpy(output + 16, event.writer_guid, 16);
  PutLe(output + 32, event.sequence_high);
  PutLe(output + 36, event.sequence_low);
  std::memcpy(output + 40, event.reader_guid, 16);
  PutLe(output + 56, event.topic_hash);
  output[60] = event.kind;
  output[61] = event.status;
  output[62] = event.schema_version;
  output[63] = event.reserved;
}

size_t DrainInternal(Event* output, size_t capacity) noexcept {
  if (output == nullptr || capacity == 0 || drain_guard.test_and_set(std::memory_order_acquire)) return 0;
  size_t count = 0;
  uint64_t position = dequeue_position.load(std::memory_order_relaxed);
  while (count < capacity) {
    Slot& slot = slots[position % kRingCapacity];
    if (slot.sequence.load(std::memory_order_acquire) != position + 1) break;
    output[count++] = slot.event;
    slot.sequence.store(position + kRingCapacity, std::memory_order_release);
    ++position;
  }
  dequeue_position.store(position, std::memory_order_release);
  drain_guard.clear(std::memory_order_release);
  return count;
}

void ExportSegment(const Event* events, size_t count) noexcept {
  if (count == 0) return;
  std::vector<uint8_t> file(256 + count * sizeof(Event), 0);
  for (size_t i = 0; i < count; ++i) SerializeEvent(events[i], file.data() + 256 + i * 64);
  const auto payload_sha = Sha256(file.data() + 256, count * 64);
  uint8_t* header = file.data();
  const uint8_t magic[8] = {'I', 'B', 'M', 'W', 'D', 'C', 'T', 0};
  std::memcpy(header, magic, 8);
  PutLe<uint16_t>(header + 8, 1);
  PutLe<uint16_t>(header + 10, 256);
  PutLe<uint16_t>(header + 12, 1);
  PutLe<uint16_t>(header + 14, 64);
  header[16] = 1;
  std::memcpy(header + 24, stream_id.data(), stream_id.size());
  std::memcpy(header + 40, meta_sha256.data(), meta_sha256.size());
  PutLe<uint32_t>(header + 72, static_cast<uint32_t>(process_id));
  PutLe<uint32_t>(header + 76, next_segment);
  PutLe<uint64_t>(header + 80, process_start_ticks);
  PutLe<uint64_t>(header + 88, next_ordinal);
  PutLe<uint64_t>(header + 96, count);
  const uint64_t dropped = dropped_count.load(std::memory_order_relaxed);
  PutLe<uint64_t>(header + 104, last_segment_dropped);
  PutLe<uint64_t>(header + 112, dropped);
  PutLe<uint64_t>(header + 120, invalid_identity_count.load(std::memory_order_relaxed));
  PutLe<uint64_t>(header + 128, unsupported_async_count.load(std::memory_order_relaxed));
  std::memcpy(header + 136, payload_sha.data(), payload_sha.size());
  header[168] = static_cast<uint8_t>(binding.mode);
  header[169] = SamplingAlgorithm(binding.mode);
  PutLe<uint16_t>(header + 170, SamplingPermyriad(binding.mode));

  std::ostringstream name;
  name << prefix << '.' << std::setw(8) << std::setfill('0') << next_segment << ".ibct";
  if (io_error.load(std::memory_order_relaxed) || !PublishFile(name.str(), file.data(), file.size())) {
    io_error.store(true, std::memory_order_relaxed);
    export_discarded_count.fetch_add(count, std::memory_order_relaxed);
  } else {
    exported_count.fetch_add(count, std::memory_order_relaxed);
    ++next_segment;
    next_ordinal += count;
    last_segment_dropped = dropped;
  }
}

void DrainAndExport() noexcept {
  std::array<Event, 1024> events{};
  for (;;) {
    const size_t count = DrainInternal(events.data(), events.size());
    if (count == 0) break;
    ExportSegment(events.data(), count);
  }
}

void WorkerMain() noexcept {
  std::unique_lock<std::mutex> lock(worker_mutex);
  while (!stop_worker.load(std::memory_order_acquire)) {
    worker_wakeup.wait_for(lock, std::chrono::milliseconds(10));
    lock.unlock();
    DrainAndExport();
    lock.lock();
  }
  lock.unlock();
  DrainAndExport();
}

uint64_t Pending() noexcept {
  const uint64_t write_position = enqueue_position.load(std::memory_order_acquire);
  const uint64_t read_position = dequeue_position.load(std::memory_order_acquire);
  return write_position >= read_position ? write_position - read_position : 0;
}

SnapshotV1 Snapshot() noexcept {
  const bool captured_sealed = sealed.load(std::memory_order_acquire);
  const uint32_t active = static_cast<uint32_t>(
      producer_gate.load(std::memory_order_acquire) & kProducerGateCountMask);
  return SnapshotV1{kAbiVersion, sizeof(SnapshotV1), dropped_count.load(),
                    invalid_identity_count.load(), unsupported_async_count.load(), Pending(),
                    active, static_cast<uint8_t>(captured_sealed), {}};
}

SnapshotV2 Snapshot2() noexcept {
  const bool captured_sealed = sealed.load(std::memory_order_acquire);
  const uint32_t active = static_cast<uint32_t>(
      producer_gate.load(std::memory_order_acquire) & kProducerGateCountMask);
  return SnapshotV2{kAbiVersionV2, sizeof(SnapshotV2), dropped_count.load(),
                    invalid_identity_count.load(), unsupported_async_count.load(), Pending(),
                    eligible_count.load(), sampled_out_count.load(), accepted_count.load(),
                    exported_count.load(), export_discarded_count.load(), active,
                    owner_count.load(), static_cast<uint8_t>(captured_sealed),
                    static_cast<uint8_t>(managed.load()), static_cast<uint8_t>(io_error.load()), 0};
}

ExactSnapshot ExactSnapshotNow() noexcept {
  std::lock_guard<std::mutex> lock(exact_mutex);
  const uint64_t write = exact_write.load(std::memory_order_acquire);
  const uint64_t read = exact_read.load(std::memory_order_acquire);
  return ExactSnapshot{kSchemaVersion, sizeof(ExactEvent),
                       write >= read ? write - read : 0,
                       exact_dropped.load(), exact_invalid_identity.load(),
                       exact_missing_registration.load(),
                       exact_unsupported_async.load(),
                       exact_unsupported_multi_handler.load()};
}

size_t ExactDrain(ExactEvent* output, size_t capacity) noexcept {
  if (output == nullptr || capacity == 0) return 0;
  std::lock_guard<std::mutex> lock(exact_mutex);
  size_t count = 0;
  uint64_t read = exact_read.load(std::memory_order_relaxed);
  const uint64_t write = exact_write.load(std::memory_order_acquire);
  while (count < capacity && read < write) {
    output[count++] = exact_events[read % kRingCapacity];
    ++read;
  }
  exact_read.store(read, std::memory_order_release);
  return count;
}

const ExactQuery exact_query{kSchemaVersion, sizeof(ExactQuery), &ExactDrain,
                             &ExactSnapshotNow};

void RequestSeal() noexcept { CloseProducerGateAndSeal(); }

void QueryInitialize() noexcept { (void)Initialize(); }

size_t QueryDrain(Event* output, size_t capacity) noexcept {
  if (managed.load(std::memory_order_acquire)) return 0;
  return DrainInternal(output, capacity);
}

void QuerySeal() noexcept {
  CloseProducerGateAndSeal();
}

const QueryV1 query{kAbiVersion, sizeof(QueryV1), 0, &QueryInitialize, &QueryDrain, &Snapshot, &QuerySeal};

void ResetCountersAndRing() noexcept {
  enqueue_position.store(0);
  dequeue_position.store(0);
  dropped_count.store(0);
  invalid_identity_count.store(0);
  unsupported_async_count.store(0);
  eligible_count.store(0);
  sampled_out_count.store(0);
  accepted_count.store(0);
  exported_count.store(0);
  export_discarded_count.store(0);
  exact_read.store(0);
  exact_write.store(0);
  exact_dropped.store(0);
  exact_invalid_identity.store(0);
  exact_missing_registration.store(0);
  exact_unsupported_async.store(0);
  exact_unsupported_multi_handler.store(0);
  exact_exported.store(0);
  producer_gate.store(0, std::memory_order_release);
  for (size_t i = 0; i < kRingCapacity; ++i) slots[i].sequence.store(i, std::memory_order_relaxed);
}

bool ParseManagedBinding(RuntimeMode requested_mode, Binding* parsed, int* duplicated_fd) noexcept {
  const char* run = std::getenv("MW_PERF_RUN_ID");
  const char* capability = std::getenv("MW_PERF_CAPABILITY_ID");
  const char* path = std::getenv("MW_PERF_PATH_ID");
  const char* deployment = std::getenv("MW_PERF_DEPLOYMENT_MODE");
  const char* topology_generation = std::getenv("MW_PERF_TOPOLOGY_GENERATION");
  const char* topology_sha = std::getenv("MW_PERF_TOPOLOGY_SHA256");
  if (!SafeId(run) || !SafeId(capability) || !SafeId(path) || !SafeId(deployment) ||
      std::strcmp(deployment, "managed") != 0 || !SafeId(topology_generation) ||
      !LowerHex64(topology_sha) ||
      !ValidateAndDuplicateDirectory(std::getenv("MW_PERF_COMM_TRACE_DIR_FD"), duplicated_fd)) return false;
  *parsed = Binding{run, capability, path, deployment, topology_sha,
                    topology_generation, requested_mode};
  return true;
}

std::string SealJson(bool finalized) {
  std::ostringstream out;
  out << "{\"schema\":\"mw_perf.dds_comm_trace.seal.v1\","
      << "\"run_id\":\"" << binding.run_id << "\","
      << "\"capability_id\":\"" << binding.capability_id << "\","
      << "\"path_id\":\"" << binding.path_id << "\","
      << "\"deployment_mode\":\"" << binding.deployment_mode << "\","
      << "\"finalized\":" << (finalized ? "true" : "false") << ','
      << "\"stream_id\":\"" << stream_hex << "\","
      << "\"producer_pid\":" << process_id << ','
      << "\"producer_start_time_ticks\":" << process_start_ticks << ','
      << "\"trace_mode\":\"" << ModeName(binding.mode) << "\","
      << "\"sampling_algorithm\":" << static_cast<unsigned>(SamplingAlgorithm(binding.mode)) << ','
      << "\"sampling_permyriad\":" << SamplingPermyriad(binding.mode) << ','
      << "\"segment_count\":" << next_segment << ','
      << "\"record_count\":" << exported_count.load() << ','
      << "\"exported\":" << exported_count.load() << ','
      << "\"dropped\":" << dropped_count.load() << ",\"invalid_identity\":" << invalid_identity_count.load() << ','
      << "\"unsupported_async\":" << unsupported_async_count.load() << ','
      << "\"export_discarded\":" << export_discarded_count.load() << ','
      << "\"io_error\":" << (io_error.load() ? "true" : "false") << ','
      << "\"sealed\":" << (sealed.load() ? "true" : "false") << ','
      << "\"pending\":" << Pending() << ",\"active_producers\":"
      << (producer_gate.load(std::memory_order_acquire) & kProducerGateCountMask) << "}\n";
  return out.str();
}

void SerializeExactEvent(const ExactEvent& event, uint8_t* output) noexcept {
  PutLe(output + 0, event.start_timestamp_ns);
  PutLe(output + 8, event.end_timestamp_ns);
  std::memcpy(output + 16, event.writer_guid, 16);
  PutLe(output + 32, event.sequence_high);
  PutLe(output + 36, event.sequence_low);
  std::memcpy(output + 40, event.reader_guid, 16);
  PutLe(output + 56, event.callback_registration_id);
  output[64] = event.kind;
  output[65] = event.status;
  output[66] = event.schema_version;
  std::memcpy(output + 67, event.reserved, 5);
}

void ExportExact() noexcept {
  if (!exact_enabled.load(std::memory_order_acquire)) return;
  std::array<ExactEvent, kRingCapacity> events{};
  const size_t count = ExactDrain(events.data(), events.size());
  if (count == 0) return;
  constexpr size_t kHeaderSize = 160;
  std::vector<uint8_t> file(kHeaderSize + count * sizeof(ExactEvent), 0);
  for (size_t i = 0; i < count; ++i)
    SerializeExactEvent(events[i], file.data() + kHeaderSize + i * sizeof(ExactEvent));
  const auto payload_sha = Sha256(file.data() + kHeaderSize, count * sizeof(ExactEvent));
  const uint8_t magic[8] = {'I', 'B', 'M', 'W', 'D', 'C', 'E', 0};
  std::memcpy(file.data(), magic, 8);
  PutLe<uint16_t>(file.data() + 8, 1);
  PutLe<uint16_t>(file.data() + 10, kHeaderSize);
  PutLe<uint16_t>(file.data() + 12, sizeof(ExactEvent));
  file[14] = 1;
  std::memcpy(file.data() + 16, stream_id.data(), stream_id.size());
  std::memcpy(file.data() + 32, exact_meta_sha256.data(), exact_meta_sha256.size());
  PutLe<uint32_t>(file.data() + 64, static_cast<uint32_t>(process_id));
  PutLe<uint64_t>(file.data() + 72, process_start_ticks);
  PutLe<uint64_t>(file.data() + 80, count);
  PutLe<uint64_t>(file.data() + 88, exact_dropped.load());
  PutLe<uint64_t>(file.data() + 96, exact_invalid_identity.load());
  PutLe<uint64_t>(file.data() + 104, exact_missing_registration.load());
  PutLe<uint64_t>(file.data() + 112, exact_unsupported_async.load());
  PutLe<uint64_t>(file.data() + 120, exact_unsupported_multi_handler.load());
  std::memcpy(file.data() + 128, payload_sha.data(), payload_sha.size());
  if (!PublishFile(exact_prefix + ".ibce", file.data(), file.size())) {
    io_error.store(true, std::memory_order_relaxed);
  } else {
    exact_exported.store(count, std::memory_order_release);
  }
}

std::string ExactMetaJson() {
  std::ostringstream out;
  out << "{\"schema\":\"mw_perf.dds_exact_callback.meta.v1\","
      << "\"run_id\":\"" << binding.run_id << "\",\"capability_id\":\""
      << binding.capability_id << "\",\"path_id\":\"" << binding.path_id
      << "\",\"deployment_mode\":\"managed\",\"finalized\":true,"
      << "\"stream_id\":\"" << stream_hex << "\",\"producer_pid\":" << process_id
      << ",\"producer_start_time_ticks\":" << process_start_ticks
      << ",\"topology_generation\":\"" << binding.topology_generation
      << "\",\"topology_sha256\":\"" << binding.topology_sha256
      << "\",\"trace_mode\":\"full\",\"record_size\":72}\n";
  return out.str();
}

std::string ExactSealJson(bool finalized) {
  const ExactSnapshot snapshot = ExactSnapshotNow();
  std::ostringstream out;
  out << "{\"schema\":\"mw_perf.dds_exact_callback.seal.v1\","
      << "\"run_id\":\"" << binding.run_id << "\",\"capability_id\":\""
      << binding.capability_id << "\",\"path_id\":\"" << binding.path_id
      << "\",\"deployment_mode\":\"managed\",\"finalized\":"
      << (finalized ? "true" : "false") << ",\"stream_id\":\"" << stream_hex
      << "\",\"producer_pid\":" << process_id << ",\"producer_start_time_ticks\":"
      << process_start_ticks << ",\"trace_mode\":\"full\",\"record_count\":"
      << exact_exported.load()
      << ",\"dropped\":" << snapshot.dropped << ",\"invalid_identity\":" << snapshot.invalid_identity
      << ",\"missing_registration\":" << snapshot.missing_registration
      << ",\"unsupported_async\":" << snapshot.unsupported_async
      << ",\"unsupported_multi_handler\":" << snapshot.unsupported_multi_handler
      << ",\"pending\":" << snapshot.pending
      << ",\"io_error\":" << (io_error.load() ? "true" : "false")
      << ",\"sealed\":" << (sealed.load() ? "true" : "false") << "}\n";
  return out.str();
}

}  // namespace

uint32_t TopicHash(const char* data, size_t size) noexcept {
  uint32_t hash = 2166136261U;
  for (size_t i = 0; i < size; ++i) hash = (hash ^ static_cast<uint8_t>(data[i])) * 16777619U;
  return hash;
}

void CopyGuid(const Guid& source, uint8_t destination[16]) noexcept {
  std::memcpy(destination, source.guidPrefix.value, 12);
  std::memcpy(destination + 12, source.entityId.value, 4);
}

bool GuidBytesKnown(const uint8_t guid[16]) noexcept {
  uint8_t combined = 0;
  for (size_t i = 0; i < 16; ++i) combined |= guid[i];
  return combined != 0;
}

bool SampleKeyValid(const SampleKey& key) noexcept {
  return GuidBytesKnown(key.writer_guid) && !(key.sequence_high == -1 && key.sequence_low == 0);
}

bool MakeSampleKey(const SampleIdentity& identity, SampleKey* output) noexcept {
  if (output == nullptr) return false;
  CopyGuid(identity.writer_guid(), output->writer_guid);
  output->sequence_high = identity.sequence_number().high;
  output->sequence_low = identity.sequence_number().low;
  return SampleKeyValid(*output);
}

bool MakeDeliveryKey(const SampleIdentity& identity, const Guid& reader_guid, DeliveryKey* output) noexcept {
  if (output == nullptr || !MakeSampleKey(identity, &output->sample)) return false;
  CopyGuid(reader_guid, output->reader_guid);
  return GuidBytesKnown(output->reader_guid);
}

bool WriterMatches(const SampleKey& key, const Guid& writer_guid) noexcept {
  uint8_t expected[16];
  CopyGuid(writer_guid, expected);
  return std::memcmp(key.writer_guid, expected, 16) == 0;
}

bool ShouldSample(const SampleKey& key) noexcept { return (SampleHash(key) & 0x0fU) == 0; }

bool Initialize() noexcept {
  // Runtime-off intentionally returns before consulting any other environment,
  // touching lifecycle state/ring slots, reading a clock, opening, or locking.
  const RuntimeMode requested_mode = ParseMode(std::getenv("IBMW_COMM_TRACE"));
  if (requested_mode == RuntimeMode::off) return true;

  const char* sink = std::getenv("IBMW_COMM_TRACE_SINK");
  const bool requested_managed = sink != nullptr && std::strcmp(sink, "managed") == 0;
  const bool ring_only = sink == nullptr || std::strcmp(sink, "none") == 0;
  if (!requested_managed && !ring_only) return false;

  Binding parsed;
  parsed.mode = requested_mode;
  int parsed_fd = -1;
  if (requested_managed && !ParseManagedBinding(requested_mode, &parsed, &parsed_fd)) return false;

  std::lock_guard<std::mutex> lock(lifecycle_mutex);
  if (owner_count.load(std::memory_order_relaxed) != 0) {
    if (managed.load() != requested_managed || (requested_managed && !(binding == parsed)) ||
        runtime_mode.load() != requested_mode) {
      if (parsed_fd >= 0) close(parsed_fd);
      return false;
    }
    if (parsed_fd >= 0) close(parsed_fd);
    owner_count.fetch_add(1);
    return true;
  }

  ResetCountersAndRing();
  runtime_mode.store(requested_mode, std::memory_order_release);
  managed.store(requested_managed, std::memory_order_release);
  const char* capability = std::getenv("MW_PERF_CAPABILITY_ID");
  const char* exact_required = std::getenv("MW_PERF_EXACT_REQUIRED");
  exact_enabled.store(requested_managed && requested_mode == RuntimeMode::full &&
                          exact_required != nullptr && std::strcmp(exact_required, "1") == 0 &&
                          capability != nullptr &&
                          std::strcmp(capability, kExactCapabilityId) == 0,
                      std::memory_order_release);
  sealed.store(false, std::memory_order_release);
  io_error.store(false);
  stop_worker.store(false);
  next_segment = 0;
  next_ordinal = 0;
  last_segment_dropped = 0;

  if (requested_managed) {
    binding = std::move(parsed);
    directory_fd = parsed_fd;
    process_id = getpid();
    if (!ReadStartTicks(&process_start_ticks) || !FillRandom(stream_id.data(), stream_id.size())) {
      close(directory_fd);
      directory_fd = -1;
      runtime_mode.store(RuntimeMode::off);
      managed.store(false);
      sealed.store(true);
      return false;
    }
    stream_hex = Hex(stream_id.data(), stream_id.size());
    prefix = "ibmw_dds_trace." + std::to_string(process_id) + "." + stream_hex;
    exact_prefix = "ibmw_dds_exact." + std::to_string(process_id) + "." + stream_hex;
    const std::string meta = MetaJson();
    meta_sha256 = Sha256(reinterpret_cast<const uint8_t*>(meta.data()), meta.size());
    if (io_error.load() || !PublishFile(prefix + ".meta.json",
                                        reinterpret_cast<const uint8_t*>(meta.data()), meta.size())) {
      close(directory_fd);
      directory_fd = -1;
      runtime_mode.store(RuntimeMode::off);
      managed.store(false);
      sealed.store(true);
      return false;
    }
    if (exact_enabled.load(std::memory_order_acquire)) {
      const std::string exact_meta = ExactMetaJson();
      exact_meta_sha256 = Sha256(reinterpret_cast<const uint8_t*>(exact_meta.data()), exact_meta.size());
      if (io_error.load() || !PublishFile(exact_prefix + ".exact.meta.json",
              reinterpret_cast<const uint8_t*>(exact_meta.data()), exact_meta.size())) {
        close(directory_fd);
        directory_fd = -1;
        runtime_mode.store(RuntimeMode::off);
        managed.store(false);
        exact_enabled.store(false);
        sealed.store(true);
        return false;
      }
    }
    try {
      worker = std::thread(&WorkerMain);
    } catch (...) {
      close(directory_fd);
      directory_fd = -1;
      runtime_mode.store(RuntimeMode::off);
      managed.store(false);
      sealed.store(true);
      return false;
    }
  }
  owner_count.store(1, std::memory_order_release);
  return true;
}

void Shutdown() noexcept {
  std::unique_lock<std::mutex> lock(lifecycle_mutex);
  const uint32_t owners = owner_count.load(std::memory_order_relaxed);
  if (owners == 0) return;
  if (owners > 1) {
    owner_count.store(owners - 1, std::memory_order_release);
    return;
  }
  owner_count.store(0, std::memory_order_release);
  CloseProducerGateAndSeal();
  WaitForProducers();
  const bool was_managed = managed.load(std::memory_order_acquire);
  if (was_managed) {
    stop_worker.store(true, std::memory_order_release);
    worker_wakeup.notify_all();
    lock.unlock();
    if (worker.joinable()) worker.join();
    DrainAndExport();
    ExportExact();
    lock.lock();
    const bool finalized = sealed.load() &&
        (producer_gate.load() & kProducerGateCountMask) == 0 && Pending() == 0 && !io_error.load();
    const std::string seal_json = SealJson(finalized);
    if (!PublishFile(prefix + ".seal.json", reinterpret_cast<const uint8_t*>(seal_json.data()), seal_json.size())) {
      io_error.store(true);
    }
    if (exact_enabled.load(std::memory_order_acquire)) {
      const ExactSnapshot exact = ExactSnapshotNow();
      const bool exact_finalized = finalized && exact.pending == 0 && exact.dropped == 0 &&
          exact.invalid_identity == 0 && exact.missing_registration == 0 &&
          exact.unsupported_async == 0 && exact.unsupported_multi_handler == 0;
      const std::string exact_seal = ExactSealJson(exact_finalized);
      if (!PublishFile(exact_prefix + ".exact.seal.json",
              reinterpret_cast<const uint8_t*>(exact_seal.data()), exact_seal.size())) io_error.store(true);
    }
    close(directory_fd);
    directory_fd = -1;
  }
  managed.store(false, std::memory_order_release);
  runtime_mode.store(RuntimeMode::off, std::memory_order_release);
}

RuntimeMode mode() noexcept { return runtime_mode.load(std::memory_order_relaxed); }
bool active() noexcept { return mode() != RuntimeMode::off; }
uint64_t NowNs() noexcept { return clock_source.load(std::memory_order_relaxed)(); }

void Submit(Event event) noexcept {
  const RuntimeMode current = mode();
  if (current == RuntimeMode::off) return;
  ProducerAdmission admission;
  if (!admission) return;
  eligible_count.fetch_add(1, std::memory_order_relaxed);
  SampleKey key{};
  CopyBytes(event.writer_guid, key.writer_guid);
  key.sequence_high = event.sequence_high;
  key.sequence_low = event.sequence_low;
  if (current == RuntimeMode::sampled && !ShouldSample(key)) {
    sampled_out_count.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  accepted_count.fetch_add(1, std::memory_order_relaxed);
  if (sealed.load(std::memory_order_acquire)) {
    dropped_count.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  uint64_t position = enqueue_position.load(std::memory_order_relaxed);
  for (;;) {
    Slot& slot = slots[position % kRingCapacity];
    const uint64_t sequence = slot.sequence.load(std::memory_order_acquire);
    const int64_t difference = static_cast<int64_t>(sequence - position);
    if (difference == 0) {
      if (enqueue_position.compare_exchange_weak(position, position + 1, std::memory_order_relaxed)) {
        slot.event = event;
        slot.sequence.store(position + 1, std::memory_order_release);
        worker_wakeup.notify_one();
        return;
      }
    } else if (difference < 0) {
      dropped_count.fetch_add(1, std::memory_order_relaxed);
      return;
    } else {
      position = enqueue_position.load(std::memory_order_relaxed);
    }
  }
}

void InvalidIdentity() noexcept {
  ProducerAdmission admission;
  if (!admission) return;
  invalid_identity_count.fetch_add(1, std::memory_order_relaxed);
}

void UnsupportedAsync() noexcept {
  ProducerAdmission admission;
  if (!admission) return;
  unsupported_async_count.fetch_add(1, std::memory_order_relaxed);
}

bool ExactActive() noexcept {
  return mode() == RuntimeMode::full && managed.load(std::memory_order_acquire) &&
         exact_enabled.load(std::memory_order_acquire);
}

bool ExactCallbackBegin() noexcept {
  if (!ExactActive()) return false;
  if (exact_callback_depth != 0) {
    ++exact_callback_depth;
    return true;
  }
  exact_callback_admission.reset(new (std::nothrow) ProducerAdmission());
  // Allocation failure must remain a fail-closed, noexcept path.
  if (!exact_callback_admission || !*exact_callback_admission) {
    exact_callback_admission.reset();
    return false;
  }
  exact_callback_depth = 1;
  // Keep this admission alive across the complete synchronous handler call.
  // The callback path uses the thread-local marker so its exact records and
  // diagnostics remain part of this same admission.
  return true;
}

void ExactCallbackEnd() noexcept {
  if (exact_callback_depth == 0 || --exact_callback_depth != 0) return;
  exact_callback_admission.reset();
}

void ExactSubmitCallback(uint64_t start_ns, uint64_t end_ns,
                         const DeliveryKey& key,
                         uint64_t callback_registration_id,
                         uint8_t status) noexcept {
  if (!ExactActive()) return;
  std::optional<ProducerAdmission> admission;
  if (!exact_callback_admission) {
    admission.emplace();
    if (!*admission) return;
  }
  if (!SampleKeyValid(key.sample) || !GuidBytesKnown(key.reader_guid) ||
       end_ns < start_ns) {
    exact_invalid_identity.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (callback_registration_id == 0) {
    exact_missing_registration.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (sealed.load(std::memory_order_acquire)) {
    exact_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  std::lock_guard<std::mutex> lock(exact_mutex);
  const uint64_t write = exact_write.load(std::memory_order_relaxed);
  const uint64_t read = exact_read.load(std::memory_order_acquire);
  if (write - read >= kRingCapacity) {
    exact_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ExactEvent event{};
  event.start_timestamp_ns = start_ns;
  event.end_timestamp_ns = end_ns;
  CopyBytes(key.sample.writer_guid, event.writer_guid);
  event.sequence_high = key.sample.sequence_high;
  event.sequence_low = key.sample.sequence_low;
  CopyBytes(key.reader_guid, event.reader_guid);
  event.callback_registration_id = callback_registration_id;
  event.kind = static_cast<uint8_t>(ExactEventKind::callback_span);
  event.status = status;
  event.schema_version = kSchemaVersion;
  exact_events[write % kRingCapacity] = event;
  exact_write.store(write + 1, std::memory_order_release);
}

void ExactInvalidIdentity() noexcept {
  if (!ExactActive()) return;
  if (exact_callback_admission) {
    exact_invalid_identity.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ProducerAdmission admission;
  if (admission) exact_invalid_identity.fetch_add(1, std::memory_order_relaxed);
}
void ExactMissingRegistration() noexcept {
  if (!ExactActive()) return;
  if (exact_callback_admission) {
    exact_missing_registration.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ProducerAdmission admission;
  if (admission) exact_missing_registration.fetch_add(1, std::memory_order_relaxed);
}
void ExactUnsupportedAsync() noexcept {
  if (!ExactActive()) return;
  if (exact_callback_admission) {
    exact_unsupported_async.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ProducerAdmission admission;
  if (admission) exact_unsupported_async.fetch_add(1, std::memory_order_relaxed);
}
void ExactUnsupportedMultiHandler() noexcept {
  if (!ExactActive()) return;
  if (exact_callback_admission) {
    exact_unsupported_multi_handler.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  ProducerAdmission admission;
  if (admission) exact_unsupported_multi_handler.fetch_add(1, std::memory_order_relaxed);
}
const ExactQuery* ExactQueryInternal() noexcept { return &exact_query; }

#ifdef IBMW_COMM_TRACE_TEST_HOOKS
void InjectStageDroppedForTest() noexcept {
  ProducerAdmission admission;
  if (admission) dropped_count.fetch_add(1, std::memory_order_relaxed);
}

void InjectExactDroppedForTest() noexcept {
  if (!ExactActive()) return;
  ProducerAdmission admission;
  if (admission) exact_dropped.fetch_add(1, std::memory_order_relaxed);
}

const RuntimeFaultHooksForTest runtime_fault_hooks_for_test{
    1, sizeof(RuntimeFaultHooksForTest), &InjectStageDroppedForTest,
    &InjectExactDroppedForTest, &NowNs, &TopicHash,
    static_cast<Event (*)(uint64_t, uint64_t, const SampleKey&, uint32_t,
                          EventKind, uint8_t) noexcept>(&MakeEvent),
    &Submit, &ExactSubmitCallback};
#endif

#ifdef IBMW_COMM_TRACE_TEST_HOOKS
const QueryV2 query2{kAbiVersionV2, sizeof(QueryV2),
                     reinterpret_cast<uint64_t>(&runtime_fault_hooks_for_test),
                     &Snapshot2, &RequestSeal};
#else
const QueryV2 query2{kAbiVersionV2, sizeof(QueryV2), 0, &Snapshot2, &RequestSeal};
#endif

Event MakeEvent(uint64_t start_ns, uint64_t end_ns, const SampleKey& key,
                uint32_t topic_hash, EventKind kind, uint8_t status) noexcept {
  Event event{};
  event.start_timestamp_ns = start_ns;
  event.end_timestamp_ns = end_ns;
  CopyBytes(key.writer_guid, event.writer_guid);
  event.sequence_high = key.sequence_high;
  event.sequence_low = key.sequence_low;
  event.topic_hash = topic_hash;
  event.kind = static_cast<uint8_t>(kind);
  event.status = status;
  event.schema_version = kSchemaVersion;
  return event;
}

Event MakeEvent(uint64_t start_ns, uint64_t end_ns, const DeliveryKey& key,
                uint32_t topic_hash, EventKind kind, uint8_t status) noexcept {
  Event event = MakeEvent(start_ns, end_ns, key.sample, topic_hash, kind, status);
  CopyBytes(key.reader_guid, event.reader_guid);
  return event;
}

void ResetForTest() noexcept {
  while (owner_count.load() != 0) Shutdown();
  std::lock_guard<std::mutex> lock(lifecycle_mutex);
  ResetCountersAndRing();
  runtime_mode.store(RuntimeMode::off);
  managed.store(false);
  exact_enabled.store(false);
  sealed.store(false);
  io_error.store(false);
  clock_source.store(&SteadyClockNs);
  writer_failure_for_test.store(false);
  directory_sync_failure_for_test.store(false);
  producer_gate_hook_for_test.store(nullptr);
  producer_admitted_hook_for_test.store(nullptr);
  drain_guard.clear();
}

void SetModeForTest(RuntimeMode value) noexcept { runtime_mode.store(value, std::memory_order_release); }
void SetClockForTest(ClockForTest clock) noexcept {
  clock_source.store(clock == nullptr ? &SteadyClockNs : clock, std::memory_order_release);
}
void SetWriterFailureForTest(bool value) noexcept { writer_failure_for_test.store(value); }
void SetDirectorySyncFailureForTest(bool value) noexcept {
  directory_sync_failure_for_test.store(value);
}
void SetProducerGateHookForTest(ProducerGateHookForTest hook) noexcept {
  producer_gate_hook_for_test.store(hook, std::memory_order_release);
}
void SetProducerAdmittedHookForTest(ProducerGateHookForTest hook) noexcept {
  producer_admitted_hook_for_test.store(hook, std::memory_order_release);
}

void SetExactEnabledForTest(bool value) noexcept {
  exact_enabled.store(value, std::memory_order_release);
  managed.store(value, std::memory_order_release);
}

}  // namespace ibmw::extensions::dds_extension::comm_trace

extern "C" const ibmw::extensions::dds_extension::comm_trace::QueryV1*
ibmw_dds_comm_trace_query_v1() noexcept {
  return &ibmw::extensions::dds_extension::comm_trace::query;
}

extern "C" const ibmw::extensions::dds_extension::comm_trace::QueryV2*
ibmw_dds_comm_trace_query_v2() noexcept {
  return &ibmw::extensions::dds_extension::comm_trace::query2;
}
