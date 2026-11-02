// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Tests of the shm transport extension (ShmBus + ShmTransportDriver), the
// replacement of iceoryx_ext. Numbered EXT-xx in the test document. No daemon
// is needed; every run uses a private discovery root.

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "base/buffer_util.h"
#include "base/url_encode.h"
#include "cpp/module_base.h"
#include "cpp/transport/transport_handle.h"
#include "cpp/util/buffer_array_allocator.h"
#include "engine/transport/transport_driver_dispatcher.h"
#include "shm_ext/global.h"
#include "shm_ext/iox_style_publisher.h"
#include "shm_ext/shm_bus.h"
#include "shm_ext/shm_ctx_publisher.h"
#include "shm_ext/shm_extension.h"
#include "shm_ext/shm_pod_publisher.h"
#include "shm_ext/shm_transport_driver.h"
#include "test_util.h"
#include "yaml-cpp/yaml.h"

#ifndef IBMW_SHM_EXT_PEER_PATH
#error "IBMW_SHM_EXT_PEER_PATH must point at the ibmw_shm_ext_peer binary"
#endif

extern char** environ;

namespace ibmw::extensions::shm_extension {
namespace {

using namespace std::chrono_literals;
namespace transport = runtime::core::transport;

// std::jthread stand-in: libc++ before 18 (the OHOS SDK ships 15) has no
// jthread. Joins on destruction and before move-assignment.
class JoiningThread {
 public:
  JoiningThread() noexcept = default;
  template <class F>
  explicit JoiningThread(F&& f) : t_(std::forward<F>(f)) {}
  JoiningThread(JoiningThread&&) noexcept = default;
  JoiningThread& operator=(JoiningThread&& o) noexcept {
    if (t_.joinable()) t_.join();
    t_ = std::move(o.t_);
    return *this;
  }
  ~JoiningThread() {
    if (t_.joinable()) t_.join();
  }
  void join() { t_.join(); }

 private:
  std::thread t_;
};

// ------------------------------------------------------------------ helpers

std::string MakeRoot() {
  const char* base = std::getenv("TMPDIR");
  std::string tmpl = std::string(base != nullptr && *base != '\0' ? base : "/tmp") + "/ibmwx.XXXXXX";
  std::vector<char> buf(tmpl.begin(), tmpl.end());
  buf.push_back('\0');
  if (::mkdtemp(buf.data()) == nullptr) std::abort();
  return buf.data();
}

void RemoveTree(const std::string& path) {
  if (DIR* d = ::opendir(path.c_str())) {
    while (struct dirent* de = ::readdir(d)) {
      std::string n = de->d_name;
      if (n == "." || n == "..") continue;
      std::string p = path + "/" + n;
      if (de->d_type == DT_DIR) {
        RemoveTree(p);
      } else {
        ::unlink(p.c_str());
      }
    }
    ::closedir(d);
  }
  ::rmdir(path.c_str());
}

class ShmExtEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    root_ = MakeRoot();
    ::setenv("IBMW_SHM_ROOT", root_.c_str(), 1);
    ::unsetenv("IBMW_SHM_DOMAIN");
    ::signal(SIGPIPE, SIG_IGN);
  }
  void TearDown() override { RemoveTree(root_); }

 private:
  std::string root_;
};

const auto* const g_env = ::testing::AddGlobalTestEnvironment(new ShmExtEnvironment());

bool WaitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 5000ms) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(1ms);
  }
  return pred();
}

// Unique per test so tests never see each other's endpoints.
std::string UniqueTopic(const std::string& suffix = "") {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  return std::string("shm_ext/") + info->name() + "/" + std::to_string(::getpid()) + suffix;
}

std::string StdUrl(const std::string& topic, const std::string& type) {
  return "/transport/" + common::util::UrlEncode(topic) + "/" + common::util::UrlEncode(type);
}

std::string PodUrl(const std::string& topic, const std::string& type) {
  return "/transport_zc_pod/" + common::util::UrlEncode(topic) + "/" + common::util::UrlEncode(type);
}

// ------------------------------------------------------- test message types

constexpr char kSerType[] = "test";
constexpr char kIntType[] = "shm_ext_test/Int";
constexpr char kBlobType[] = "shm_ext_test/Blob";

struct IntMsg {
  int32_t value = 0;
};

struct BlobMsg {
  std::string data;
};

const ibmw_string_view_t* SerTypes(void*) {
  static const ibmw_string_view_t kTypes[] = {{kSerType, sizeof(kSerType) - 1}};
  return kTypes;
}
size_t SerTypesNum(void*) { return 1; }
const void* NoCustom(void*) { return nullptr; }

ibmw_string_view_t IntName(void*) { return {kIntType, sizeof(kIntType) - 1}; }
void* IntCreate(void*) { return new IntMsg(); }
void IntDestroy(void*, void* m) { delete static_cast<IntMsg*>(m); }
void IntCopy(void*, const void* from, void* to) { *static_cast<IntMsg*>(to) = *static_cast<const IntMsg*>(from); }
void IntMove(void*, void* from, void* to) { IntCopy(nullptr, from, to); }
bool IntSerialize(void*, ibmw_string_view_t, const void* msg, const ibmw_buffer_array_allocator_t* allocator,
                  ibmw_buffer_array_t* buffer_array) {
  auto b = allocator->allocate(allocator->ctx, buffer_array, sizeof(IntMsg));
  if (b.data == nullptr || b.len != sizeof(IntMsg)) return false;
  std::memcpy(b.data, msg, sizeof(IntMsg));
  return true;
}
bool IntDeserialize(void*, ibmw_string_view_t, ibmw_buffer_array_view_t view, void* msg) {
  if (view.len != 1 || view.data[0].len != sizeof(IntMsg)) return false;
  std::memcpy(msg, view.data[0].data, sizeof(IntMsg));
  return true;
}

const ibmw_type_support_base_t* IntTypeSupport() {
  static const ibmw_type_support_base_t kSupport{
      .type_name = IntName,
      .create = IntCreate,
      .destroy = IntDestroy,
      .copy = IntCopy,
      .move = IntMove,
      .serialize = IntSerialize,
      .deserialize = IntDeserialize,
      .serialization_types_supported_num = SerTypesNum,
      .serialization_types_supported_list = SerTypes,
      .custom_type_support_ptr = NoCustom,
      .ctx = nullptr,
  };
  return &kSupport;
}

ibmw_string_view_t BlobName(void*) { return {kBlobType, sizeof(kBlobType) - 1}; }
void* BlobCreate(void*) { return new BlobMsg(); }
void BlobDestroy(void*, void* m) { delete static_cast<BlobMsg*>(m); }
void BlobCopy(void*, const void* from, void* to) {
  *static_cast<BlobMsg*>(to) = *static_cast<const BlobMsg*>(from);
}
void BlobMove(void*, void* from, void* to) {
  *static_cast<BlobMsg*>(to) = std::move(*static_cast<BlobMsg*>(from));
}
bool BlobSerialize(void*, ibmw_string_view_t, const void* msg, const ibmw_buffer_array_allocator_t* allocator,
                   ibmw_buffer_array_t* buffer_array) {
  const auto& data = static_cast<const BlobMsg*>(msg)->data;
  auto b = allocator->allocate(allocator->ctx, buffer_array, data.size());
  if (b.data == nullptr || b.len != data.size()) return false;
  std::memcpy(b.data, data.data(), data.size());
  return true;
}
bool BlobDeserialize(void*, ibmw_string_view_t, ibmw_buffer_array_view_t view, void* msg) {
  auto& data = static_cast<BlobMsg*>(msg)->data;
  data.clear();
  for (size_t i = 0; i < view.len; ++i) data.append(static_cast<const char*>(view.data[i].data), view.data[i].len);
  return true;
}

const ibmw_type_support_base_t* BlobTypeSupport() {
  static const ibmw_type_support_base_t kSupport{
      .type_name = BlobName,
      .create = BlobCreate,
      .destroy = BlobDestroy,
      .copy = BlobCopy,
      .move = BlobMove,
      .serialize = BlobSerialize,
      .deserialize = BlobDeserialize,
      .serialization_types_supported_num = SerTypesNum,
      .serialization_types_supported_list = SerTypes,
      .custom_type_support_ptr = NoCustom,
      .ctx = nullptr,
  };
  return &kSupport;
}

// Int type whose serializer throws, like an allocator running out of memory.
bool ThrowingSerialize(void*, ibmw_string_view_t, const void*, const ibmw_buffer_array_allocator_t*,
                       ibmw_buffer_array_t*) {
  throw std::runtime_error("serializer failure");
}

const ibmw_type_support_base_t* ThrowingIntTypeSupport() {
  static const ibmw_type_support_base_t kSupport{
      .type_name = IntName,
      .create = IntCreate,
      .destroy = IntDestroy,
      .copy = IntCopy,
      .move = IntMove,
      .serialize = ThrowingSerialize,
      .deserialize = IntDeserialize,
      .serialization_types_supported_num = SerTypesNum,
      .serialization_types_supported_list = SerTypes,
      .custom_type_support_ptr = NoCustom,
      .ctx = nullptr,
  };
  return &kSupport;
}

transport::TransportInfo IntInfo(const std::string& topic) {
  transport::TransportInfo info;
  info.msg_type = kIntType;
  info.topic_name = topic;
  info.msg_type_support_ref = ibmw::util::TypeSupportRef(IntTypeSupport());
  return info;
}

transport::TransportInfo BlobInfo(const std::string& topic) {
  transport::TransportInfo info;
  info.msg_type = kBlobType;
  info.topic_name = topic;
  info.msg_type_support_ref = ibmw::util::TypeSupportRef(BlobTypeSupport());
  return info;
}

std::shared_ptr<ibmw::transport::Context> PubContext() {
  auto ctx = std::make_shared<ibmw::transport::Context>(
      ibmw_transport_context_type_t::IBMW_TRANSPORT_PUBLISHER_CONTEXT);
  ctx->SetSerializationType(kSerType);
  ctx->SetMetaValue("origin", "test");
  return ctx;
}

std::string MakeBlob(size_t size, char seed) {
  std::string s(size, '\0');
  for (size_t i = 0; i < size; ++i) s[i] = static_cast<char>(seed + i * 7);
  return s;
}

// What a subscription handler observed.
struct Received {
  std::mutex mu;
  std::vector<int32_t> ints;
  std::vector<std::string> blobs;
  std::string driver;
  std::string origin;
  std::atomic<int> calls{0};

  size_t Ints() {
    std::lock_guard<std::mutex> lock(mu);
    return ints.size();
  }
  size_t Blobs() {
    std::lock_guard<std::mutex> lock(mu);
    return blobs.size();
  }
  // Snapshots: callbacks may still append concurrently.
  int32_t Int(size_t i) {
    std::lock_guard<std::mutex> lock(mu);
    return ints.at(i);
  }
  std::string Blob(size_t i) {
    std::lock_guard<std::mutex> lock(mu);
    return blobs.at(i);
  }
  std::string Origin() {
    std::lock_guard<std::mutex> lock(mu);
    return origin;
  }
};

// Spawned ibmw_shm_ext_peer with a pipe on its stdout.
struct Peer {
  pid_t pid = -1;
  int out = -1;

  ~Peer() {
    if (pid > 0) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, nullptr, 0);
    }
    if (out >= 0) ::close(out);
  }

  bool Spawn(const std::vector<std::string>& args) {
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) return false;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    // Cross-testing: the build-time path does not exist on the target device.
    const char* override_path = std::getenv("IBMW_SHM_EXT_PEER_PATH");
    const std::string path = override_path != nullptr ? override_path : IBMW_SHM_EXT_PEER_PATH;
    std::vector<std::string> all{path};
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : all) argv.push_back(a.data());
    argv.push_back(nullptr);
    const int rc = ::posix_spawn(&pid, path.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(fds[1]);
    out = fds[0];
    if (rc != 0) pid = -1;
    return rc == 0;
  }

  std::string ReadLine(std::chrono::milliseconds timeout = 10000ms) {
    std::string line;
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
      struct pollfd pfd {out, POLLIN, 0};
      if (::poll(&pfd, 1, 50) <= 0) continue;
      char c;
      if (::read(out, &c, 1) != 1) return line;
      if (c == '\n') return line;
      line.push_back(c);
    }
    return line;
  }

  int Wait(std::chrono::milliseconds timeout = 10000ms) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < end) {
      int status = 0;
      if (::waitpid(pid, &status, WNOHANG) == pid) {
        pid = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      }
      std::this_thread::sleep_for(5ms);
    }
    return -1;
  }
};

// ------------------------------------------------------------------ fixture

class ShmDriverTest : public ::testing::Test {
 protected:
  // Handlers capture locals of the test body, which die before TearDown().
  // Declared after those locals, this stops every callback first (after
  // running `before`, e.g. to open a gate a stalled handler waits on).
  struct StopGuard {
    ShmDriverTest* test;
    std::function<void()> before;
    ~StopGuard() {
      if (before) before();
      if (test->driver_) test->driver_->Shutdown();
    }
  };

  void TearDown() override {
    if (driver_) driver_->Shutdown();
    driver_.reset();
    bus_.reset();
  }

  // Creates the bus and an initialized driver (state Init).
  ShmTransportDriver& Init(YAML::Node options = YAML::Node(), ShmBusOptions bus_options = {},
                           std::string name = "shm") {
    bus_ = std::make_unique<ShmBus>();
    bus_->Initialize(bus_options);
    driver_ = std::make_unique<ShmTransportDriver>(*bus_, std::move(name));
    driver_->Initialize(options);
    return *driver_;
  }

  // Handlers must outlive the driver (it keeps pointers to them).
  const transport::MessageHandlerWrapper& Handler(const transport::TransportInfo& info, Received* rx,
                                                  std::function<void()> extra = {}) {
    transport::MessageHandlerWrapper h;
    h.info = info;
    h.callback = [rx, extra](transport::MessageFrame& frame, std::function<void()>&& release, const auto*) {
      transport::CheckMsg(frame);
      {
        std::lock_guard<std::mutex> lock(rx->mu);
        if (frame.info.msg_type == kIntType) {
          rx->ints.push_back(static_cast<const IntMsg*>(frame.msg_ptr)->value);
        } else {
          rx->blobs.push_back(static_cast<const BlobMsg*>(frame.msg_ptr)->data);
        }
        rx->driver = std::string(frame.ctx_ref.GetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_DRIVER));
        rx->origin = std::string(frame.ctx_ref.GetMetaValue("origin"));
      }
      if (extra) extra();
      rx->calls.fetch_add(1);
      release();
    };
    handlers_.push_back(std::move(h));
    return handlers_.back();
  }

  bool HasSubscribers(const std::string& url) {
    auto lease = bus_->TryAcquireOperation();
    auto* pub = bus_->GetPublisher(lease, url);
    return pub != nullptr && pub->HasSubscribers();
  }

  ShmPublisher* Publisher(const std::string& url) {
    auto lease = bus_->TryAcquireOperation();
    return bus_->GetPublisher(lease, url);
  }

  void SendInt(const transport::TransportInfo& info, int32_t value) {
    IntMsg msg{value};
    auto ctx = PubContext();  // ContextRef does not own the context
    transport::MessageFrame frame{.info = info, .msg_ptr = &msg, .ctx_ref = ctx};
    driver_->Send(frame);
  }

  void SendBlob(const transport::TransportInfo& info, const std::string& data) {
    BlobMsg msg{data};
    auto ctx = PubContext();
    transport::MessageFrame frame{.info = info, .msg_ptr = &msg, .ctx_ref = ctx};
    driver_->Send(frame);
  }

  std::deque<transport::MessageHandlerWrapper> handlers_;
  std::unique_ptr<ShmBus> bus_;
  std::unique_ptr<ShmTransportDriver> driver_;
};

transport::SendTypeWrapper SendType(const transport::TransportInfo& info) {
  transport::SendTypeWrapper w;
  w.info = info;
  return w;
}

// ============================================================ pure functions

// EXT-01 Delivery options: defaults when omitted, lossless only for the exact
// core metrics profile (same rule as the iceoryx driver).
TEST(ShmDriverOptionsTest, ResolveDeliveryOptions) {
  ShmTransportDriver::Options o;
  auto d = ShmTransportDriver::ResolveDeliveryOptions(o);
  ASSERT_TRUE(d.has_value());
  EXPECT_FALSE(d->lossless);
  EXPECT_EQ(d->queue_capacity, 1u);

  o.benchmark_profile = "core_metrics_e2e_lossless_v1";
  o.publisher_too_slow_policy = "WAIT_FOR_CONSUMER";
  o.subscriber_queue_full_policy = "BLOCK_PRODUCER";
  o.subscriber_queue_capacity = 256;
  d = ShmTransportDriver::ResolveDeliveryOptions(o);
  ASSERT_TRUE(d.has_value());
  EXPECT_TRUE(d->lossless);
  EXPECT_EQ(d->queue_capacity, 256u);

  auto bad = o;
  bad.subscriber_queue_capacity = 128;
  EXPECT_FALSE(ShmTransportDriver::ResolveDeliveryOptions(bad).has_value());
  bad = o;
  bad.publisher_too_slow_policy = "DISCARD_OLDEST_DATA";
  EXPECT_FALSE(ShmTransportDriver::ResolveDeliveryOptions(bad).has_value());
  ShmTransportDriver::Options only_capacity;
  only_capacity.subscriber_queue_capacity = 4;
  EXPECT_FALSE(ShmTransportDriver::ResolveDeliveryOptions(only_capacity).has_value());
}

// EXT-02 Loan size growth doubles from the hint and stops at the largest class.
TEST(ShmDriverOptionsTest, NextShmLoanSize) {
  EXPECT_EQ(NextShmLoanSize(1024, 10), 1024u);
  EXPECT_EQ(NextShmLoanSize(1024, 1025), 2048u);
  EXPECT_EQ(NextShmLoanSize(64, 100000), 131072u);
  EXPECT_EQ(NextShmLoanSize(0, 1), 1u);
  EXPECT_EQ(NextShmLoanSize(1u << 27, (1u << 28)), size_t{1} << 28);
  EXPECT_FALSE(NextShmLoanSize(1024, (size_t{1} << 28) + 1).has_value());
}

// EXT-03 ctx-meta header: size and encoding match the wire format; oversize
// fields are rejected.
TEST(ShmDriverOptionsTest, CtxMetaHeaderEncoding) {
  std::unordered_map<std::string_view, std::string_view> kv{{"k", "vv"}};
  const size_t size = ShmTransportDriver::ComputeCtxMetaHeaderSize("ser", kv);
  EXPECT_EQ(size, 4u + 1 + 3 + 1 + 2 + 1 + 2 + 2);
  std::vector<char> buf(size + 8);
  EXPECT_EQ(ShmTransportDriver::EncodeCtxMetaHeader(buf.data(), buf.size(), 1234, "ser", kv), size);
  EXPECT_EQ(common::util::GetUint32FromBuf(buf.data()), 1234u);
  EXPECT_EQ(buf[4], 3);
  EXPECT_EQ(std::string(buf.data() + 5, 3), "ser");
  EXPECT_EQ(buf[8], 1);
  EXPECT_EQ(ShmTransportDriver::EncodeCtxMetaHeader(buf.data(), size - 1, 1234, "ser", kv), 0u);
  EXPECT_EQ(ShmTransportDriver::ComputeCtxMetaHeaderSize(std::string(256, 's'), {}), 0u);
  const std::string long_value(70000, 'v');
  EXPECT_EQ(ShmTransportDriver::ComputeCtxMetaHeaderSize("ser", {{"k", long_value}}), 0u);
}

// ============================================================ lifecycle

// EXT-04 Initialize validates options: executor keys and partial delivery
// controls are rejected; a driver initializes only once.
TEST_F(ShmDriverTest, InitializeValidatesOptions) {
  bus_ = std::make_unique<ShmBus>();
  bus_->Initialize({});
  {
    ShmTransportDriver d(*bus_);
    EXPECT_ANY_THROW(d.Initialize(YAML::Load("{executor: x}")));
  }
  {
    ShmTransportDriver d(*bus_);
    EXPECT_ANY_THROW(d.Initialize(YAML::Load("{subscriber_queue_capacity: 8}")));
  }
  {
    ShmTransportDriver d(*bus_);
    EXPECT_NO_THROW(d.Initialize(YAML::Load("{listener_thread_name: shm_t}")));
    EXPECT_ANY_THROW(d.Initialize(YAML::Node()));
    EXPECT_ANY_THROW(d.Initialize(YAML::Node()));
  }
}

// EXT-05 The driver name is configurable so "iceoryx" is a drop-in
// replacement for routing, and received contexts carry the name; the slot
// stays "shm" (H2).
TEST_F(ShmDriverTest, DriverNameAliasIsDropIn) {
  auto& d = Init(YAML::Node(), {}, "iceoryx");
  EXPECT_EQ(d.Name(), "iceoryx");
  // Routing follows the name; the slot is the C++ type and never follows it.
  EXPECT_EQ(d.Slot(), ::ibmw::DriverSlotFromString("shm"));
  EXPECT_EQ(d.Slot(), IBMW_DRIVER_ID(ShmTransportDriver));
  EXPECT_NE(d.Slot(), ::ibmw::DriverSlotFromString("iceoryx"));

  const auto info = IntInfo(UniqueTopic());
  Received rx;
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.OnMessage(Handler(info, &rx)));
  StopGuard stop{this};
  d.Start();
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(StdUrl(info.topic_name, kIntType)); }));
  SendInt(info, 5);
  ASSERT_TRUE(WaitFor([&] { return rx.Ints() == 1; }));
  std::lock_guard<std::mutex> lock(rx.mu);
  EXPECT_EQ(rx.driver, "iceoryx");

  ShmBus default_bus;
  ShmTransportDriver plain(default_bus);
  EXPECT_EQ(plain.Name(), "shm");
  EXPECT_EQ(plain.Slot(), IBMW_DRIVER_ID(ShmTransportDriver));
}

// EXT-06 Initialization report names the driver and delivery mode.
TEST_F(ShmDriverTest, InitializationReport) {
  auto& d = Init(YAML::Load(
      "{benchmark_profile: core_metrics_e2e_lossless_v1, publisher_too_slow_policy: WAIT_FOR_CONSUMER, "
      "subscriber_queue_full_policy: BLOCK_PRODUCER, subscriber_queue_capacity: 256}"));
  const auto report = d.GenInitializationReport();
  bool has_driver = false, lossless = false;
  for (const auto& [k, v] : report) {
    has_driver |= (k == "driver" && v == "shm");
    lossless |= (k == "delivery" && v == "lossless");
  }
  EXPECT_TRUE(has_driver);
  EXPECT_TRUE(lossless);
}

// ============================================================ framed Send

// EXT-07 Send -> OnMessage round trip with context meta and driver tag.
TEST_F(ShmDriverTest, SendRoundTrip) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  Received rx;
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.OnMessage(Handler(info, &rx)));
  StopGuard stop{this};
  d.Start();
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(StdUrl(info.topic_name, kIntType)); }));
  for (int32_t v = 1; v <= 5; ++v) {
    SendInt(info, v);
    ASSERT_TRUE(WaitFor([&] { return rx.Ints() == static_cast<size_t>(v); }));
  }
  std::lock_guard<std::mutex> lock(rx.mu);
  EXPECT_EQ(rx.ints, (std::vector<int32_t>{1, 2, 3, 4, 5}));
  EXPECT_EQ(rx.driver, "shm");
  EXPECT_EQ(rx.origin, "test");
}

// EXT-08 Two handlers on one topic share one subscription and both run.
TEST_F(ShmDriverTest, HandlersOnSameTopicShareSubscription) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  Received a, b;
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.OnMessage(Handler(info, &a)));
  ASSERT_TRUE(d.OnMessage(Handler(info, &b)));
  EXPECT_EQ(bus_->SubscriptionCount(), 1u);
  StopGuard stop{this};
  d.Start();
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(StdUrl(info.topic_name, kIntType)); }));
  SendInt(info, 42);
  ASSERT_TRUE(WaitFor([&] { return a.Ints() == 1 && b.Ints() == 1; }));
  EXPECT_EQ(a.Int(0), 42);
  EXPECT_EQ(b.Int(0), 42);
}

// EXT-09 Messages larger than the loan hint grow the loan (doubling) and
// raise the hint, so the next message needs one loan.
TEST_F(ShmDriverTest, SendGrowsLoanForLargeMessages) {
  ShmBusOptions bo;
  bo.shm_init_size = 64;
  auto& d = Init(YAML::Node(), bo);
  const auto info = BlobInfo(UniqueTopic());
  Received rx;
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.OnMessage(Handler(info, &rx)));
  StopGuard stop{this};
  d.Start();
  const auto url = StdUrl(info.topic_name, kBlobType);
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(url); }));
  EXPECT_EQ(Publisher(url)->SizeHint(), 64u);
  const auto big = MakeBlob(100000, 'a');
  SendBlob(info, big);
  ASSERT_TRUE(WaitFor([&] { return rx.Blobs() == 1; }));
  EXPECT_EQ(rx.Blob(0), big);
  EXPECT_EQ(Publisher(url)->SizeHint(), 131072u);
  const auto small = MakeBlob(10, 'z');
  SendBlob(info, small);
  ASSERT_TRUE(WaitFor([&] { return rx.Blobs() == 2; }));
  EXPECT_EQ(rx.Blob(1), small);
  EXPECT_EQ(Publisher(url)->Outstanding(), 0u);
}

// EXT-10 Send with a serialization cache writes the cached buffers verbatim.
TEST_F(ShmDriverTest, SendUsesSerializationCache) {
  auto& d = Init();
  const auto info = BlobInfo(UniqueTopic());
  Received rx;
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.OnMessage(Handler(info, &rx)));
  StopGuard stop{this};
  d.Start();
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(StdUrl(info.topic_name, kBlobType)); }));

  BlobMsg msg{MakeBlob(5000, 'c')};
  auto array = std::make_shared<ibmw::util::BufferArray>();
  ASSERT_TRUE(info.msg_type_support_ref.Serialize(kSerType, &msg, array->AllocatorNativeHandle(),
                                                  array->BufferArrayNativeHandle()));
  auto ctx = PubContext();
  transport::MessageFrame frame{.info = info, .msg_ptr = nullptr, .ctx_ref = ctx};
  frame.serialization_cache.emplace(kSerType, std::shared_ptr<ibmw::util::BufferArrayView>(
                                                  new ibmw::util::BufferArrayView(*array),
                                                  [array](const ibmw::util::BufferArrayView* p) { delete p; }));
  d.Send(frame);
  ASSERT_TRUE(WaitFor([&] { return rx.Blobs() == 1; }));
  EXPECT_EQ(rx.Blob(0), msg.data);
}

// EXT-11 A chunk whose pkg_size exceeds the chunk is discarded and released;
// delivery continues with the next valid message.
TEST_F(ShmDriverTest, MalformedChunkIsDiscarded) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  Received rx;
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.OnMessage(Handler(info, &rx)));
  StopGuard stop{this};
  d.Start();
  const auto url = StdUrl(info.topic_name, kIntType);
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(url); }));
  auto* pub = Publisher(url);
  void* chunk = pub->Loan(16);
  ASSERT_NE(chunk, nullptr);
  const uint32_t lie = 4096;
  std::memcpy(chunk, &lie, sizeof(lie));
  pub->Publish(chunk);
  EXPECT_EQ(pub->Stats().publishes, 1u);
  std::this_thread::sleep_for(50ms);
  SendInt(info, 7);
  ASSERT_TRUE(WaitFor([&] { return rx.Ints() == 1; }));
  EXPECT_EQ(rx.Int(0), 7);
  EXPECT_EQ(rx.calls.load(), 1);
  // Both chunks were taken (none evicted): the malformed one really reached
  // the handler path and was discarded there.
  EXPECT_EQ(pub->Stats().deliveries, 2u);
  EXPECT_EQ(pub->Stats().evictions, 0u);
}

// ============================================================ delivery modes

// EXT-12 Default delivery is latest-value: a stalled subscriber never blocks
// the publisher, and the newest sample arrives once it resumes.
TEST_F(ShmDriverTest, DefaultDeliveryNeverBlocksPublisher) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  Received rx;
  std::atomic<bool> gate{false};
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.OnMessage(Handler(info, &rx, [&] {
    while (!gate.load()) std::this_thread::sleep_for(1ms);
  })));
  StopGuard stop{this, [&] { gate = true; }};
  d.Start();
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(StdUrl(info.topic_name, kIntType)); }));
  SendInt(info, 0);
  ASSERT_TRUE(WaitFor([&] { return rx.Ints() == 1; }));  // handler now stalls
  const auto t0 = std::chrono::steady_clock::now();
  for (int32_t v = 1; v <= 200; ++v) SendInt(info, v);
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s);
  gate = true;
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(rx.mu);
    return !rx.ints.empty() && rx.ints.back() == 200;
  }));
  EXPECT_LT(rx.Ints(), 201u);
}

// EXT-13 The lossless profile delivers every message in order even when the
// subscriber is slower than the publisher.
TEST_F(ShmDriverTest, LosslessProfileDeliversEverything) {
  auto& d = Init(YAML::Load(
      "{benchmark_profile: core_metrics_e2e_lossless_v1, publisher_too_slow_policy: WAIT_FOR_CONSUMER, "
      "subscriber_queue_full_policy: BLOCK_PRODUCER, subscriber_queue_capacity: 256}"));
  const auto info = IntInfo(UniqueTopic());
  Received rx;
  std::atomic<int> n{0};
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.OnMessage(Handler(info, &rx, [&] {
    if (++n % 100 == 0) std::this_thread::sleep_for(5ms);
  })));
  StopGuard stop{this};
  d.Start();
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(StdUrl(info.topic_name, kIntType)); }));
  constexpr int32_t kCount = 3000;
  for (int32_t v = 0; v < kCount; ++v) SendInt(info, v);
  ASSERT_TRUE(WaitFor([&] { return rx.Ints() == kCount; }, 20000ms));
  std::lock_guard<std::mutex> lock(rx.mu);
  for (int32_t v = 0; v < kCount; ++v) ASSERT_EQ(rx.ints[v], v);
}

// ============================================================ loans

// EXT-14 POD loans: registration rules, alignment, publish to AttachLoaned,
// release of an unpublished loan.
TEST_F(ShmDriverTest, PodLoanRoundTrip) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  EXPECT_FALSE(d.EnsurePodTopic(info.topic_name, kIntType, 0, 8));
  EXPECT_FALSE(d.EnsurePodTopic(info.topic_name, kIntType, 64, 3));
  EXPECT_FALSE(d.EnsurePodTopic(info.topic_name, kIntType, 64, 8192));
  ASSERT_TRUE(d.RegisterLoanedPodTopic(info, 64, 64));
  EXPECT_TRUE(d.EnsurePodTopic(info.topic_name, kIntType, 64, 64));    // idempotent
  EXPECT_FALSE(d.EnsurePodTopic(info.topic_name, kIntType, 128, 64));  // conflicting hint

  std::mutex mu;
  std::vector<uint64_t> got;
  transport::LoanedMessageHandlerWrapper w;
  w.info = info;
  w.callback = [&](const ibmw_transport_context_base_t* ctx, const void* p, size_t size,
                   std::function<void()>&& release) {
    EXPECT_EQ(ctx, nullptr);
    EXPECT_EQ(size, 64u);
    uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    {
      std::lock_guard<std::mutex> lock(mu);
      got.push_back(v);
    }
    release();
  };
  ASSERT_TRUE(d.AttachLoaned(w));
  StopGuard stop{this};
  d.Start();
  EXPECT_FALSE(d.EnsurePodTopic(info.topic_name + "x", kIntType, 64, 8));  // not in Init any more
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(PodUrl(info.topic_name, kIntType)); }));

  transport::LoanedSampleHandle h;
  ASSERT_TRUE(d.LoanSample(info, 64, transport::LoanMode::kPodOnly, nullptr, h));
  EXPECT_EQ(h.mode, transport::LoanMode::kPodOnly);
  EXPECT_EQ(h.source_driver, &d);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(h.user_payload_ptr) % 64, 0u);
  const uint64_t value = 0xC0FFEE;
  std::memcpy(h.user_payload_ptr, &value, sizeof(value));
  d.PublishLoaned(h);
  EXPECT_EQ(h.user_payload_ptr, nullptr);
  EXPECT_EQ(h.driver_token, nullptr);
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return got.size() == 1;
  }));
  {
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_EQ(got[0], value);
  }

  transport::LoanedSampleHandle dropped;
  ASSERT_TRUE(d.LoanSample(info, 64, transport::LoanMode::kPodOnly, nullptr, dropped));
  d.ReleaseLoaned(dropped);
  EXPECT_EQ(dropped.user_payload_ptr, nullptr);
  auto* pub = Publisher(PodUrl(info.topic_name, kIntType));
  EXPECT_EQ(pub->Outstanding(), 0u);

  transport::LoanedSampleHandle unknown;
  EXPECT_FALSE(d.LoanSample(IntInfo(info.topic_name + "/none"), 64, transport::LoanMode::kPodOnly, nullptr,
                            unknown));
}

// EXT-15 ctx-meta loans are framed in place and arrive through OnMessage.
TEST_F(ShmDriverTest, CtxMetaLoanRoundTrip) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  Received rx;
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.OnMessage(Handler(info, &rx)));
  StopGuard stop{this};
  d.Start();
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(StdUrl(info.topic_name, kIntType)); }));

  auto ctx = PubContext();
  transport::LoanedSampleHandle h;
  EXPECT_FALSE(d.LoanSample(info, sizeof(IntMsg), transport::LoanMode::kCtxMetaThenPayload, nullptr, h));
  ASSERT_TRUE(d.LoanSample(info, sizeof(IntMsg), transport::LoanMode::kCtxMetaThenPayload, ctx.get(), h));
  EXPECT_EQ(h.mode, transport::LoanMode::kCtxMetaThenPayloadUnreported);
  EXPECT_EQ(h.user_payload_size, sizeof(IntMsg));
  IntMsg msg{99};
  std::memcpy(h.user_payload_ptr, &msg, sizeof(msg));
  d.PublishLoaned(h);
  EXPECT_EQ(h.driver_token, nullptr);
  ASSERT_TRUE(WaitFor([&] { return rx.Ints() == 1; }));
  EXPECT_EQ(rx.Int(0), 99);
  EXPECT_EQ(rx.Origin(), "test");

  // Every loan slot can be used and released; the table never runs out.
  std::vector<transport::LoanedSampleHandle> held(shm::kMaxChunksAllocatedPerPublisher);
  for (auto& x : held) {
    ASSERT_TRUE(d.LoanSample(info, sizeof(IntMsg), transport::LoanMode::kCtxMetaThenPayload, ctx.get(), x));
  }
  transport::LoanedSampleHandle extra;
  EXPECT_FALSE(d.LoanSample(info, sizeof(IntMsg), transport::LoanMode::kCtxMetaThenPayload, ctx.get(), extra));
  for (auto& x : held) d.ReleaseLoaned(x);
  EXPECT_EQ(Publisher(StdUrl(info.topic_name, kIntType))->Outstanding(), 0u);
}

// EXT-16 LoanSerializedSample grows past the 4 KB first guess and reports
// the serialized size.
TEST_F(ShmDriverTest, SerializedLoanGrows) {
  auto& d = Init();
  const auto info = BlobInfo(UniqueTopic());
  Received rx;
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.OnMessage(Handler(info, &rx)));
  StopGuard stop{this};
  d.Start();
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(StdUrl(info.topic_name, kBlobType)); }));

  BlobMsg msg{MakeBlob(20000, 's')};
  auto ctx = PubContext();
  transport::LoanedSampleHandle h;
  EXPECT_FALSE(d.LoanSerializedSample(info, *ctx, nullptr, h));
  auto no_ts = info;
  no_ts.msg_type_support_ref = ibmw::util::TypeSupportRef();
  EXPECT_FALSE(d.LoanSerializedSample(no_ts, *ctx, &msg, h));
  ASSERT_TRUE(d.LoanSerializedSample(info, *ctx, &msg, h));
  EXPECT_EQ(h.user_payload_size, msg.data.size());
  EXPECT_EQ(h.mode, transport::LoanMode::kCtxMetaThenPayloadUnreported);
  d.PublishLoaned(h);
  ASSERT_TRUE(WaitFor([&] { return rx.Blobs() == 1; }));
  EXPECT_EQ(rx.Blob(0), msg.data);
}

// EXT-29 ShmCtxMetaPublisher (shm_ctx_publisher.h) drives the ctx-meta loan
// through the TransportDriver base pointer: serialize-into-chunk + publish,
// delivered to a standard handler; null driver/message and an unregistered
// topic fail without holding a loan.
TEST_F(ShmDriverTest, CtxMetaPublisherHelper) {
  auto& d = Init();
  const auto info = BlobInfo(UniqueTopic());
  Received rx;
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.OnMessage(Handler(info, &rx)));
  StopGuard stop{this};
  d.Start();
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(StdUrl(info.topic_name, kBlobType)); }));

  transport::TransportDriver* base = &d;
  ShmCtxMetaPublisher pub(base, std::string(info.topic_name), kBlobType, info.msg_type_support_ref);
  EXPECT_EQ(pub.info().topic_name, info.topic_name);
  auto ctx = PubContext();
  BlobMsg msg{MakeBlob(10000, 'c')};
  EXPECT_FALSE(pub.Publish(*ctx, nullptr));
  // Wait per sample: the default delivery is latest-only (queue depth 1).
  for (size_t i = 1; i <= 3; ++i) {
    ASSERT_TRUE(pub.Publish(*ctx, &msg));
    ASSERT_TRUE(WaitFor([&] { return rx.Blobs() == i; }));
  }
  EXPECT_EQ(rx.Blob(2), msg.data);
  EXPECT_EQ(rx.Origin(), "test");
  EXPECT_EQ(Publisher(StdUrl(info.topic_name, kBlobType))->Outstanding(), 0u);

  ShmCtxMetaPublisher no_driver(nullptr, std::string(info.topic_name), kBlobType, info.msg_type_support_ref);
  EXPECT_FALSE(no_driver.Publish(*ctx, &msg));
  ShmCtxMetaPublisher unregistered(base, UniqueTopic("/unregistered"), kBlobType, info.msg_type_support_ref);
  EXPECT_FALSE(unregistered.Publish(*ctx, &msg));
}

// EXT-30 ShmRawPodPublisher (shm_pod_publisher.h, counterpart of iceoryx_ext's
// CreateRawPodPublisher): Init-only registration, borrow/write/publish to
// RegisterPodSubscriber, unpublished and move-overwritten loans go back to
// the publisher.
TEST_F(ShmDriverTest, RawPodPublisherHelper) {
  auto& d = Init();
  const std::string topic = UniqueTopic();
  constexpr std::string_view kRawType = "shm_ext_test/RawPod";
  EXPECT_EQ(CreateShmRawPodPublisher(nullptr, topic, kRawType, 256), nullptr);
  auto pub = CreateShmRawPodPublisher(&d, topic, kRawType, 256);
  ASSERT_NE(pub, nullptr);

  std::mutex mu;
  std::vector<std::string> got;
  ASSERT_TRUE(d.RegisterPodSubscriber(topic, kRawType, [&](const void* p, size_t size) {
    std::lock_guard<std::mutex> lock(mu);
    got.emplace_back(static_cast<const char*>(p), size);
  }));
  StopGuard stop{this};
  d.Start();
  EXPECT_EQ(CreateShmRawPodPublisher(&d, topic + "/late", kRawType, 256), nullptr);  // not in Init any more
  const auto url = PodUrl(topic, std::string(kRawType));
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(url); }));

  EXPECT_FALSE(pub->Borrow(0).valid());
  EXPECT_FALSE(pub->Publish(ShmLoanedBytes()));
  for (size_t i = 1; i <= 2; ++i) {
    auto sample = pub->Borrow(256);
    ASSERT_TRUE(sample.valid());
    ASSERT_GE(sample.size(), 256u);
    std::memset(sample.data(), static_cast<int>('a' + i), 256);
    ASSERT_TRUE(pub->Publish(std::move(sample)));
    EXPECT_FALSE(sample.valid());  // NOLINT(bugprone-use-after-move): moved-from state is specified
    ASSERT_TRUE(WaitFor([&] {
      std::lock_guard<std::mutex> lock(mu);
      return got.size() == i;
    }));
  }
  {
    std::lock_guard<std::mutex> lock(mu);
    for (size_t i = 0; i < 2; ++i) {
      ASSERT_GE(got[i].size(), 256u);
      EXPECT_EQ(got[i].substr(0, 256), std::string(256, static_cast<char>('b' + i)));
    }
  }

  auto* raw = Publisher(url);
  {
    auto a = pub->Borrow(256);
    auto b = pub->Borrow(256);
    ASSERT_TRUE(a.valid() && b.valid());
    EXPECT_EQ(raw->Outstanding(), 2u);
    a = std::move(b);  // releases a's loan
    EXPECT_EQ(raw->Outstanding(), 1u);
  }
  EXPECT_EQ(raw->Outstanding(), 0u);
}

// ============================================================ retained loans

struct RetainedProbe {
  std::mutex mu;
  std::vector<uint64_t> values;
  std::vector<std::function<void()>> releases;

  size_t Count() {
    std::lock_guard<std::mutex> lock(mu);
    return values.size();
  }
  void ReleaseOne() {
    std::function<void()> r;
    {
      std::lock_guard<std::mutex> lock(mu);
      r = std::move(releases.front());
      releases.erase(releases.begin());
    }
    r();
  }
  void ReleaseAll() {
    while (true) {
      {
        std::lock_guard<std::mutex> lock(mu);
        if (releases.empty()) return;
      }
      ReleaseOne();
    }
  }
};

transport::LoanedMessageHandlerWrapper RetainedWrapper(const transport::TransportInfo& info, RetainedProbe* probe) {
  transport::LoanedMessageHandlerWrapper w;
  w.info = info;
  w.callback = [probe](const ibmw_transport_context_base_t*, const void* p, size_t, std::function<void()>&& release) {
    uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    std::lock_guard<std::mutex> lock(probe->mu);
    probe->values.push_back(v);
    probe->releases.push_back(std::move(release));
  };
  return w;
}

void PublishPod(ShmTransportDriver& d, const transport::TransportInfo& info, uint64_t value) {
  transport::LoanedSampleHandle h;
  ASSERT_TRUE(d.LoanSample(info, sizeof(value), transport::LoanMode::kPodOnly, nullptr, h));
  std::memcpy(h.user_payload_ptr, &value, sizeof(value));
  d.PublishLoaned(h);
}

// EXT-17 Retained loans: samples outlive the callback, at most max_outstanding
// are held, extra samples are dropped (not replayed), Close waits for every
// release and frees the URL for reuse.
TEST_F(ShmDriverTest, RetainedLoanLimitCloseAndReuse) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  EXPECT_EQ(d.AttachLoanedRetained(RetainedWrapper(info, nullptr), {.max_outstanding = 0}), nullptr);
  EXPECT_EQ(d.AttachLoanedRetained(RetainedWrapper(info, nullptr), {.max_outstanding = 257}), nullptr);
  ASSERT_TRUE(d.EnsurePodTopic(info.topic_name, kIntType, 8, 8));
  RetainedProbe probe;
  auto handle = d.AttachLoanedRetained(RetainedWrapper(info, &probe), {.max_outstanding = 2});
  ASSERT_NE(handle, nullptr);
  // The URL is taken while the subscription lives.
  EXPECT_EQ(d.AttachLoanedRetained(RetainedWrapper(info, &probe), {.max_outstanding = 1}), nullptr);
  StopGuard stop{this};
  d.Start();
  const auto url = PodUrl(info.topic_name, kIntType);
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(url); }));

  PublishPod(d, info, 1);
  ASSERT_TRUE(WaitFor([&] { return probe.Count() == 1; }));
  PublishPod(d, info, 2);
  ASSERT_TRUE(WaitFor([&] { return probe.Count() == 2; }));
  PublishPod(d, info, 3);  // over the limit: dropped
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(probe.Count(), 2u);

  probe.ReleaseOne();
  PublishPod(d, info, 4);
  ASSERT_TRUE(WaitFor([&] { return probe.Count() == 3; }));
  {
    std::lock_guard<std::mutex> lock(probe.mu);
    EXPECT_EQ(probe.values, (std::vector<uint64_t>{1, 2, 4}));
  }

  handle->Stop();
  EXPECT_FALSE(handle->Close(50ms));  // two samples still held
  // A stopped but unreleased subscription still owns the URL.
  EXPECT_FALSE(bus_->ReserveSubscriberUrl(url));
  probe.ReleaseAll();
  EXPECT_TRUE(handle->Close(1s));
  handle.reset();

  // Closed: the URL can be subscribed again and receives new samples.
  std::atomic<int> again{0};
  auto sub = bus_->RegisterSubscriber(url, [&](const void*, size_t, std::function<void()>&& release) {
    again.fetch_add(1);
    release();
  });
  ASSERT_NE(sub, nullptr);
  StopGuard stop_again{this};
  ASSERT_TRUE(WaitFor([&] { return sub->HasPublisher(); }));
  PublishPod(d, info, 5);
  ASSERT_TRUE(WaitFor([&] { return again.load() == 1; }));
}

// EXT-18 A retained release functor may run on any thread, more than once,
// or never (dropping the last copy releases the sample).
TEST_F(ShmDriverTest, RetainedReleaseFromOtherThreadAndByDestruction) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  ASSERT_TRUE(d.EnsurePodTopic(info.topic_name, kIntType, 8, 8));
  RetainedProbe probe;
  auto handle = d.AttachLoanedRetained(RetainedWrapper(info, &probe), {.max_outstanding = 4});
  ASSERT_NE(handle, nullptr);
  StopGuard stop{this};
  d.Start();
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(PodUrl(info.topic_name, kIntType)); }));
  for (uint64_t v = 1; v <= 3; ++v) {
    PublishPod(d, info, v);
    ASSERT_TRUE(WaitFor([&] { return probe.Count() == v; }));
  }
  std::function<void()> r0, r1, r2;
  {
    std::lock_guard<std::mutex> lock(probe.mu);
    r0 = std::move(probe.releases[0]);
    r1 = std::move(probe.releases[1]);
    r2 = std::move(probe.releases[2]);
    probe.releases.clear();
  }
  std::thread([&] {
    r0();
    r0();  // idempotent
  }).join();
  r1 = nullptr;  // dropping the functor releases
  auto copy = r2;
  copy();
  r2();
  EXPECT_TRUE(handle->Close(1s));
}

// ============================================================ shutdown

// EXT-19 Shutdown is refused on a listener callback thread and succeeds on
// the owner thread afterwards.
TEST_F(ShmDriverTest, ShutdownRefusedOnCallbackThread) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  Received rx;
  std::atomic<int> can_shutdown{-1}, prepared{-1};
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.OnMessage(Handler(info, &rx, [&] {
    can_shutdown = d.CanShutdownFromCurrentThread() ? 1 : 0;
    prepared = d.TryPrepareShutdown(10ms) ? 1 : 0;
    d.Shutdown();  // logged and ignored
  })));
  StopGuard stop{this};
  d.Start();
  EXPECT_TRUE(d.CanShutdownFromCurrentThread());
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(StdUrl(info.topic_name, kIntType)); }));
  SendInt(info, 1);
  ASSERT_TRUE(WaitFor([&] { return rx.calls.load() == 1; }));
  EXPECT_EQ(can_shutdown.load(), 0);
  EXPECT_EQ(prepared.load(), 0);
  EXPECT_FALSE(bus_->Prepared());
  SendInt(info, 2);  // still running
  ASSERT_TRUE(WaitFor([&] { return rx.calls.load() == 2; }));
  EXPECT_TRUE(d.TryPrepareShutdown(1s));
  d.Shutdown();
  EXPECT_EQ(bus_->PublisherCount(), 0u);
  EXPECT_EQ(bus_->SubscriptionCount(), 0u);
}

// EXT-20 Prepare waits for outstanding publisher loans and retained samples;
// it is retryable and nothing is freed until it succeeds.
TEST_F(ShmDriverTest, PrepareWaitsForLoansAndRetainedSamples) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  ASSERT_TRUE(d.EnsurePodTopic(info.topic_name, kIntType, 8, 8));
  RetainedProbe probe;
  auto handle = d.AttachLoanedRetained(RetainedWrapper(info, &probe), {.max_outstanding = 1});
  ASSERT_NE(handle, nullptr);
  StopGuard stop{this};
  d.Start();
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(PodUrl(info.topic_name, kIntType)); }));
  PublishPod(d, info, 1);
  ASSERT_TRUE(WaitFor([&] { return probe.Count() == 1; }));
  transport::LoanedSampleHandle loan;
  ASSERT_TRUE(d.LoanSample(info, 8, transport::LoanMode::kPodOnly, nullptr, loan));

  EXPECT_FALSE(d.TryPrepareShutdown(50ms));  // retained sample held
  probe.ReleaseAll();
  EXPECT_FALSE(d.TryPrepareShutdown(50ms));  // publisher loan outstanding
  // Intake is closed: no new loans or registrations while preparing.
  transport::LoanedSampleHandle refused;
  EXPECT_FALSE(d.LoanSample(info, 8, transport::LoanMode::kPodOnly, nullptr, refused));
  EXPECT_FALSE(d.RegisterSendType(SendType(IntInfo(info.topic_name + "/late"))));
  EXPECT_EQ(bus_->PublisherCount(), 1u);  // nothing freed yet
  d.ReleaseLoaned(loan);
  EXPECT_TRUE(d.TryPrepareShutdown(1s));
  d.Shutdown();
  d.Shutdown();  // idempotent
  EXPECT_EQ(bus_->PublisherCount(), 0u);
}

// EXT-21 A Send blocked in a lossless publish is released by shutdown of the
// consumer side (subscription stop discards its queue).
TEST_F(ShmDriverTest, ShutdownUnblocksLosslessPublisher) {
  auto& d = Init(YAML::Load(
      "{benchmark_profile: core_metrics_e2e_lossless_v1, publisher_too_slow_policy: WAIT_FOR_CONSUMER, "
      "subscriber_queue_full_policy: BLOCK_PRODUCER, subscriber_queue_capacity: 256}"));
  const auto info = IntInfo(UniqueTopic());
  Received rx;
  std::atomic<bool> release_handler{false};
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.OnMessage(Handler(info, &rx, [&] {
    while (!release_handler.load()) std::this_thread::sleep_for(1ms);
  })));
  // Declared before the guard: joined only after shutdown unblocked them.
  std::atomic<int> sent{0};
  JoiningThread producer, closer;
  StopGuard stop{this, [&] { release_handler = true; }};
  d.Start();
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(StdUrl(info.topic_name, kIntType)); }));
  producer = JoiningThread([&] {
    for (int32_t v = 0; v < 400; ++v) {
      SendInt(info, v);
      sent.fetch_add(1);
    }
  });
  // 1 in the handler + 256 queued; the producer then blocks.
  ASSERT_TRUE(WaitFor([&] { return sent.load() >= 257; }));
  std::this_thread::sleep_for(50ms);
  EXPECT_LT(sent.load(), 400);
  closer = JoiningThread([&] {
    std::this_thread::sleep_for(50ms);
    release_handler = true;  // lets the in-flight callback finish
  });
  EXPECT_TRUE(d.TryPrepareShutdown(5s));
  producer.join();
  closer.join();
  EXPECT_EQ(sent.load(), 400);  // remaining sends returned (refused or discarded)
  d.Shutdown();
}

// ============================================================ cross-process

// EXT-22 A separate process encoding frames on raw selfdev shm is received
// by the driver (wire format compatibility, publisher in another process).
TEST_F(ShmDriverTest, CrossProcessPeerPublishesToDriver) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  Received rx;
  ASSERT_TRUE(d.OnMessage(Handler(info, &rx)));
  StopGuard stop{this};
  d.Start();
  Peer peer;
  ASSERT_TRUE(peer.Spawn({"pub", StdUrl(info.topic_name, kIntType), "3", "100"}));
  ASSERT_TRUE(WaitFor([&] { return rx.Ints() == 3; }, 10000ms));
  EXPECT_EQ(peer.ReadLine(), "done");
  std::lock_guard<std::mutex> lock(rx.mu);
  EXPECT_EQ(rx.ints, (std::vector<int32_t>{100, 101, 102}));
  EXPECT_EQ(rx.origin, "peer");
  EXPECT_EQ(rx.driver, "shm");
}

// EXT-23 The driver's frames are decoded by an independent process.
TEST_F(ShmDriverTest, CrossProcessDriverPublishesToPeer) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  StopGuard stop{this};
  d.Start();
  const auto url = StdUrl(info.topic_name, kIntType);
  Peer peer;
  ASSERT_TRUE(peer.Spawn({"sub", url, "3"}));
  ASSERT_EQ(peer.ReadLine(), "ready");
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(url); }, 10000ms));
  for (int32_t v = 7; v <= 9; ++v) SendInt(info, v);
  EXPECT_EQ(peer.ReadLine(), "got 7 8 9");
  EXPECT_EQ(peer.Wait(), 0);
}

}  // namespace
}  // namespace ibmw::extensions::shm_extension

namespace ibmw::extensions::shm_extension {
namespace {

// ============================================================ review fixes

// EXT-24 A throwing serializer gives the loan back (Send and
// LoanSerializedSample), so the loan limit is never used up and shutdown can
// still prepare.
TEST_F(ShmDriverTest, ThrowingSerializerDoesNotLeakLoans) {
  auto& d = Init();
  auto info = IntInfo(UniqueTopic());
  info.msg_type_support_ref = ibmw::util::TypeSupportRef(ThrowingIntTypeSupport());
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  StopGuard stop{this};
  d.Start();
  auto* pub = Publisher(StdUrl(info.topic_name, kIntType));
  ASSERT_NE(pub, nullptr);
  auto ctx = PubContext();
  IntMsg msg{1};
  for (int i = 0; i < 3 * static_cast<int>(shm::kMaxChunksAllocatedPerPublisher); ++i) {
    transport::MessageFrame frame{.info = info, .msg_ptr = &msg, .ctx_ref = ctx};
    d.Send(frame);
    transport::LoanedSampleHandle h;
    EXPECT_FALSE(d.LoanSerializedSample(info, *ctx, &msg, h));
    EXPECT_EQ(h.driver_token, nullptr);
  }
  EXPECT_EQ(pub->Outstanding(), 0u);
  EXPECT_EQ(pub->Stats().publishes, 0u);
  EXPECT_TRUE(d.TryPrepareShutdown(1000ms));
}

// EXT-25 Publishing or releasing a handle twice, through a copy or from
// another driver, cannot drive the loan count below the live loans, and a
// stale copy cannot act on a later loan of the same slot and chunk.
TEST_F(ShmDriverTest, DuplicateAndForeignLoanHandlesAreIgnored) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.EnsurePodTopic(info.topic_name, kIntType, 64, 8));
  StopGuard stop{this};
  d.Start();
  auto* pub = Publisher(StdUrl(info.topic_name, kIntType));
  auto* pod = Publisher(PodUrl(info.topic_name, kIntType));
  ASSERT_NE(pub, nullptr);
  ASSERT_NE(pod, nullptr);
  auto ctx = PubContext();

  transport::LoanedSampleHandle a, b;
  ASSERT_TRUE(d.LoanSample(info, sizeof(IntMsg), transport::LoanMode::kCtxMetaThenPayload, ctx.get(), a));
  ASSERT_TRUE(d.LoanSample(info, sizeof(IntMsg), transport::LoanMode::kCtxMetaThenPayload, ctx.get(), b));
  EXPECT_EQ(pub->Outstanding(), 2u);
  auto a_copy = a;
  const auto a_stale = a;  // calls clear the handle passed in; keep a pristine copy
  d.PublishLoaned(a);
  d.PublishLoaned(a_copy);  // already given back
  d.ReleaseLoaned(a_copy);
  EXPECT_EQ(pub->Outstanding(), 1u);
  EXPECT_EQ(pub->Stats().publishes, 1u);
  auto foreign = b;
  foreign.source_driver = nullptr;
  d.ReleaseLoaned(foreign);
  EXPECT_EQ(pub->Outstanding(), 1u);
  d.ReleaseLoaned(b);
  EXPECT_EQ(pub->Outstanding(), 0u);

  // The freed slot (and recycled chunk) is loaned again; the stale copy of
  // `a` must not publish or release the new loan.
  transport::LoanedSampleHandle c;
  ASSERT_TRUE(d.LoanSample(info, sizeof(IntMsg), transport::LoanMode::kCtxMetaThenPayload, ctx.get(), c));
  EXPECT_EQ(ShmLoanSlot::FromToken(c.driver_token), ShmLoanSlot::FromToken(a_stale.driver_token));
  EXPECT_NE(c.driver_token, a_stale.driver_token);
  auto s1 = a_stale, s2 = a_stale;
  d.PublishLoaned(s1);
  d.ReleaseLoaned(s2);
  EXPECT_EQ(pub->Outstanding(), 1u);
  EXPECT_EQ(pub->Stats().publishes, 1u);
  d.PublishLoaned(c);
  EXPECT_EQ(pub->Outstanding(), 0u);
  EXPECT_EQ(pub->Stats().publishes, 2u);

  transport::LoanedSampleHandle p;
  ASSERT_TRUE(d.LoanSample(info, sizeof(IntMsg), transport::LoanMode::kPodOnly, nullptr, p));
  auto p_copy = p;
  const auto p_stale = p;
  d.ReleaseLoaned(p);
  d.ReleaseLoaned(p_copy);
  d.PublishLoaned(p_copy);
  EXPECT_EQ(pod->Outstanding(), 0u);
  EXPECT_EQ(pod->Stats().publishes, 0u);
  transport::LoanedSampleHandle q;
  ASSERT_TRUE(d.LoanSample(info, sizeof(IntMsg), transport::LoanMode::kPodOnly, nullptr, q));
  EXPECT_EQ(q.user_payload_ptr, p_stale.user_payload_ptr);  // same chunk recycled
  auto s3 = p_stale, s4 = p_stale;
  d.PublishLoaned(s3);
  d.ReleaseLoaned(s4);
  EXPECT_EQ(pod->Outstanding(), 1u);
  EXPECT_EQ(pod->Stats().publishes, 0u);
  d.PublishLoaned(q);
  EXPECT_EQ(pod->Outstanding(), 0u);
  EXPECT_EQ(pod->Stats().publishes, 1u);
  EXPECT_TRUE(d.TryPrepareShutdown(1000ms));
}

// EXT-26 An encoded URL over the selfdev limit is refused at registration
// (iceoryx truncated it, which could merge distinct topics).
TEST_F(ShmDriverTest, OverlongUrlIsRejected) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic("/" + std::string(1100, 'x')));
  Received rx;
  EXPECT_FALSE(d.RegisterSendType(SendType(info)));
  EXPECT_FALSE(d.OnMessage(Handler(info, &rx)));
  EXPECT_EQ(bus_->PublisherCount(), 0u);
  EXPECT_EQ(bus_->SubscriptionCount(), 0u);
}

// EXT-27 A Send blocked on a lossless consumer in another process that never
// drains is released by shutdown, which then prepares within its timeout.
TEST_F(ShmDriverTest, ShutdownUnblocksSendToStalledRemoteConsumer) {
  auto& d = Init(YAML::Load(
      "{benchmark_profile: core_metrics_e2e_lossless_v1, publisher_too_slow_policy: WAIT_FOR_CONSUMER, "
      "subscriber_queue_full_policy: BLOCK_PRODUCER, subscriber_queue_capacity: 256}"));
  const auto info = IntInfo(UniqueTopic());
  const auto url = StdUrl(info.topic_name, kIntType);
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  std::atomic<int> sent{0};
  JoiningThread producer;
  StopGuard stop{this};
  d.Start();
  Peer peer;
  ASSERT_TRUE(peer.Spawn({"stall", url}));
  ASSERT_EQ(peer.ReadLine(), "ready");
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(url); }));
  producer = JoiningThread([&] {
    for (int32_t v = 0; v < 64; ++v) {
      SendInt(info, v);
      sent.fetch_add(1);
    }
  });
  // The peer's queue holds 4: the producer is now blocked for good.
  ASSERT_TRUE(WaitFor([&] { return Publisher(url)->Stats().blocked >= 1; }));
  EXPECT_LT(sent.load(), 64);
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_TRUE(d.TryPrepareShutdown(5000ms));
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 5000ms);
  producer.join();
  EXPECT_EQ(sent.load(), 64);
}

// EXT-28 A process runs one shm domain: a bus asking for another fails
// loudly instead of silently joining the running one.
TEST_F(ShmDriverTest, ConflictingDomainIsRejected) {
  Init();
  ShmBus same;
  EXPECT_NO_THROW(same.Initialize(ShmBusOptions{}));
  ShmBus other;
  ShmBusOptions o;
  o.domain = "ext28_other";
  EXPECT_THROW(other.Initialize(o), std::runtime_error);
}

}  // namespace
}  // namespace ibmw::extensions::shm_extension

namespace ibmw::extensions::shm_extension {
namespace {

// ============================================================ driver slot

// Type-level stand-in for iceoryx_ext's driver (not built here): its slot is
// what a module typed for that driver compares against.
struct IceoryxDriverStandIn {
  static constexpr const char* kStableDriverId = "iceoryx";
};

template <class DriverT>
class SlotModule : public ibmw::Module {
  IBMW_USE_DRIVER(DriverT, driver_)

 public:
  ibmw::ModuleDescriptor Info() const override { return {}; }
  bool Setup(ibmw::Runtime) override { return true; }
  bool Run() override { return true; }
  void Stop() override {}
  DriverT* driver_ = nullptr;
};

// What Engine does for each driver: BroadcastDriverSlot(td->Slot(), td).
template <class ModuleT>
void Broadcast(ModuleT& module, runtime::core::transport::TransportDriver& td) {
  module.SetDriverSlot(td.Slot(), &td);
}

// EXT-31 Under the alias "iceoryx" IBMW_USE_DRIVER(ShmTransportDriver) binds,
// and a module typed for the iceoryx driver is not handed this object.
TEST_F(ShmDriverTest, AliasKeepsTypedDriverSlot) {
  auto& d = Init(YAML::Node(), {}, "iceoryx");
  SlotModule<ShmTransportDriver> typed;
  Broadcast(typed, d);
  EXPECT_EQ(typed.driver_, &d);

  SlotModule<IceoryxDriverStandIn> iceoryx_typed;
  Broadcast(iceoryx_typed, d);
  EXPECT_EQ(iceoryx_typed.driver_, nullptr);
  EXPECT_NE(d.Slot(), IBMW_DRIVER_ID(IceoryxDriverStandIn));
}

// EXT-32 The public loan API routes by name: under the alias a dispatcher
// configured for "iceoryx" loans, publishes and delivers through this driver.
TEST_F(ShmDriverTest, AliasServesDispatcherLoanApi) {
  auto& d = Init(YAML::Node(), {}, "iceoryx");
  const std::string topic = UniqueTopic();
  const ibmw_string_view_t msg_type{kIntType, sizeof(kIntType) - 1};

  transport::TransportCatalog catalog;
  transport::TransportDriverDispatcher dispatcher;
  dispatcher.SetTransportCatalog(&catalog);
  dispatcher.SetPubTopicsDriversRules({{".*", {"iceoryx"}}});
  dispatcher.SetSubTopicsDriversRules({{".*", {"iceoryx"}}});
  dispatcher.RegisterTransportDriver(
      &d, transport::TransportDriverCapabilitiesV1{
              .shutdown_v1 = static_cast<transport::TransportShutdownCapabilityV1*>(&d),
              .loaned_pod_registration_v1 = static_cast<transport::LoanedPodRegistrationCapabilityV1*>(&d),
              .retained_loan_v1 = static_cast<transport::RetainedLoanCapabilityV1*>(&d)});
  dispatcher.Initialize();

  ASSERT_TRUE(dispatcher.RegisterLoanedPodTopic(transport::RegisterLoanedPodAccessInfoWrapper{
      .pkg_path = "pkg", .module_name = "module", .topic_name = topic, .msg_type = msg_type,
      .initial_size_hint = 8, .alignment = 8}));
  std::atomic<uint64_t> received{0};
  ibmw::transport::LoanedMessageCallback callback(
      [&](const ibmw_transport_context_base_t*, const void* payload, size_t size, ibmw_function_base_t*) {
        uint64_t v = 0;
        if (size >= sizeof(v)) std::memcpy(&v, payload, sizeof(v));
        received.store(v);
      });
  ASSERT_TRUE(dispatcher.AttachLoaned(transport::MessageHandlerAccessInfoWrapper{
      .pkg_path = "pkg", .module_name = "module", .topic_name = topic, .msg_type_support = IntTypeSupport(),
      .callback = callback.NativeHandle()}));
  StopGuard stop{this};
  dispatcher.Start();
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(PodUrl(topic, kIntType)); }));

  auto handle = dispatcher.Loan(transport::LoanAccessInfoWrapper{
      .pkg_path = "pkg", .module_name = "module", .topic_name = topic, .msg_type = msg_type,
      .requested_size = sizeof(uint64_t), .mode = transport::LoanMode::kPodOnly});
  ASSERT_EQ(handle.source_driver, &d);
  ASSERT_NE(handle.user_payload_ptr, nullptr);
  const uint64_t value = 0x51;
  std::memcpy(handle.user_payload_ptr, &value, sizeof(value));
  dispatcher.PublishLoaned(handle);
  EXPECT_TRUE(WaitFor([&] { return received.load() == value; }));
}

// ============================================================ listener threads

struct CallbackThreadProbe {
  std::mutex mu;
  std::vector<std::thread::id> threads;
  bool all_bound = true;
  int expected_cpu = -1;

  void Record() {
    cpu_set_t set;
    CPU_ZERO(&set);
    const bool ok = ::sched_getaffinity(0, sizeof(set), &set) == 0 && CPU_COUNT(&set) == 1 &&
                    CPU_ISSET(expected_cpu, &set);
    std::lock_guard<std::mutex> lock(mu);
    all_bound = all_bound && ok;
    threads.push_back(std::this_thread::get_id());
  }
  size_t Count() {
    std::lock_guard<std::mutex> lock(mu);
    return threads.size();
  }
};

int LastAllowedCpu() {
  cpu_set_t set;
  CPU_ZERO(&set);
  if (::sched_getaffinity(0, sizeof(set), &set) != 0) return -1;
  for (int cpu = CPU_SETSIZE - 1; cpu >= 0; --cpu) {
    if (CPU_ISSET(cpu, &set) && static_cast<unsigned>(cpu) < std::thread::hardware_concurrency()) return cpu;
  }
  return -1;
}

// EXT-33 listener_thread_bind_cpu covers every subscriber kind and every pool
// thread, and is in effect before the first callback of each thread.
TEST_F(ShmDriverTest, ListenerThreadOptionsCoverEveryKindAndThread) {
  const int cpu = LastAllowedCpu();
  ASSERT_GE(cpu, 0);
  YAML::Node options;
  options["listener_thread_bind_cpu"].push_back(cpu);
  auto& d = Init(options);

  CallbackThreadProbe probe;
  probe.expected_cpu = cpu;
  // One listener thread serves 256 subscribers; the 257th needs a second one.
  std::vector<transport::TransportInfo> pods;
  for (int i = 0; i < 257; ++i) {
    pods.push_back(IntInfo(UniqueTopic("/" + std::to_string(i))));
    ASSERT_TRUE(d.RegisterPodSubscriber(pods.back().topic_name, kIntType,
                                        [&probe](const void*, size_t) { probe.Record(); }));
  }
  for (const auto& info : pods) ASSERT_TRUE(d.EnsurePodTopic(info.topic_name, kIntType, 8, 8));

  const auto std_info = IntInfo(UniqueTopic("/std"));
  Received rx;
  ASSERT_TRUE(d.RegisterSendType(SendType(std_info)));
  ASSERT_TRUE(d.OnMessage(Handler(std_info, &rx, [&probe] { probe.Record(); })));

  const auto loaned_info = IntInfo(UniqueTopic("/loaned"));
  ASSERT_TRUE(d.EnsurePodTopic(loaned_info.topic_name, kIntType, 8, 8));
  transport::LoanedMessageHandlerWrapper loaned;
  loaned.info = loaned_info;
  loaned.callback = [&probe](const ibmw_transport_context_base_t*, const void*, size_t, std::function<void()>&&) {
    probe.Record();
  };
  ASSERT_TRUE(d.AttachLoaned(loaned));

  const auto retained_info = IntInfo(UniqueTopic("/retained"));
  ASSERT_TRUE(d.EnsurePodTopic(retained_info.topic_name, kIntType, 8, 8));
  auto retained_wrapper = loaned;
  retained_wrapper.info = retained_info;
  auto retained = d.AttachLoanedRetained(retained_wrapper, {.max_outstanding = 1});
  ASSERT_NE(retained, nullptr);

  StopGuard stop{this};
  d.Start();
  EXPECT_GE(bus_->ListenerThreads(), 2u);
  // Which subscriptions share a thread is unspecified: publish to all of them.
  pods.push_back(loaned_info);
  pods.push_back(retained_info);
  for (const auto& info : pods) {
    ASSERT_TRUE(WaitFor([&] { return HasSubscribers(PodUrl(info.topic_name, kIntType)); }));
    PublishPod(d, info, 1);
  }
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(StdUrl(std_info.topic_name, kIntType)); }));
  SendInt(std_info, 1);
  ASSERT_TRUE(WaitFor([&] { return probe.Count() == pods.size() + 1; }));

  std::lock_guard<std::mutex> lock(probe.mu);
  EXPECT_TRUE(probe.all_bound);
  std::set<std::thread::id> distinct(probe.threads.begin(), probe.threads.end());
  EXPECT_GE(distinct.size(), 2u);
}

// ============================================================ delivery gate

struct PodValues {
  std::mutex mu;
  std::vector<uint64_t> values;
  std::atomic<size_t> count{0};
  std::vector<uint64_t> Snapshot() {
    std::lock_guard<std::mutex> lock(mu);
    return values;
  }
};

ShmTransportDriver::PodMessageCallback RecordPod(PodValues* out) {
  return [out](const void* payload, size_t) {
    uint64_t v;
    std::memcpy(&v, payload, sizeof(v));
    std::lock_guard<std::mutex> lock(out->mu);
    out->values.push_back(v);
    out->count.fetch_add(1);
  };
}

// EXT-34 Default delivery: no callback runs before Start(); of the samples sent
// meanwhile the newest one is delivered right after Start().
TEST_F(ShmDriverTest, NoDeliveryBeforeStartKeepsNewestSample) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  ASSERT_TRUE(d.EnsurePodTopic(info.topic_name, kIntType, 8, 8));
  PodValues rx;
  ASSERT_TRUE(d.RegisterPodSubscriber(info.topic_name, kIntType, RecordPod(&rx)));
  StopGuard stop{this};
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(PodUrl(info.topic_name, kIntType)); }));
  for (uint64_t v = 1; v <= 3; ++v) PublishPod(d, info, v);
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(rx.count.load(), 0u);
  EXPECT_EQ(bus_->ListenerThreads(), 0u);

  d.Start();
  ASSERT_TRUE(WaitFor([&] { return rx.count.load() == 1; }));
  EXPECT_EQ(rx.Snapshot(), std::vector<uint64_t>{3});
  PublishPod(d, info, 4);
  ASSERT_TRUE(WaitFor([&] { return rx.count.load() == 2; }));
}

constexpr char kLosslessOptions[] =
    "{benchmark_profile: core_metrics_e2e_lossless_v1, publisher_too_slow_policy: WAIT_FOR_CONSUMER, "
    "subscriber_queue_full_policy: BLOCK_PRODUCER, subscriber_queue_capacity: 256}";

// EXT-35 Lossless delivery: every sample sent before Start() arrives in order.
TEST_F(ShmDriverTest, LosslessSamplesBeforeStartAreDeliveredInOrder) {
  auto& d = Init(YAML::Load(kLosslessOptions));
  const auto info = IntInfo(UniqueTopic());
  ASSERT_TRUE(d.EnsurePodTopic(info.topic_name, kIntType, 8, 8));
  PodValues rx;
  ASSERT_TRUE(d.RegisterPodSubscriber(info.topic_name, kIntType, RecordPod(&rx)));
  StopGuard stop{this};
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(PodUrl(info.topic_name, kIntType)); }));
  std::vector<uint64_t> sent;
  for (uint64_t v = 1; v <= 20; ++v) {
    PublishPod(d, info, v);
    sent.push_back(v);
  }
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(rx.count.load(), 0u);
  d.Start();
  ASSERT_TRUE(WaitFor([&] { return rx.count.load() == sent.size(); }));
  EXPECT_EQ(rx.Snapshot(), sent);
}

// EXT-36 Handlers can be added to a URL while another driver already publishes
// to it: delivery starts only after all of them are registered.
TEST_F(ShmDriverTest, HandlersRegisterSafelyWhileSamplesArrive) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());

  ShmBus other_bus;
  other_bus.Initialize({});
  ShmTransportDriver other(other_bus);
  other.Initialize(YAML::Node());
  ASSERT_TRUE(other.RegisterSendType(SendType(info)));
  other.Start();

  std::atomic_bool halt{false};
  JoiningThread sender([&] {
    IntMsg msg{7};
    auto ctx = PubContext();
    while (!halt.load()) {
      transport::MessageFrame frame{.info = info, .msg_ptr = &msg, .ctx_ref = ctx};
      other.Send(frame);
      std::this_thread::sleep_for(100us);
    }
  });

  Received first, second;
  std::vector<std::unique_ptr<Received>> more;
  ASSERT_TRUE(d.OnMessage(Handler(info, &first)));
  std::this_thread::sleep_for(20ms);
  ASSERT_TRUE(d.OnMessage(Handler(info, &second)));
  for (int i = 0; i < 20; ++i) {
    more.push_back(std::make_unique<Received>());
    ASSERT_TRUE(d.OnMessage(Handler(info, more.back().get())));
  }
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(first.calls.load(), 0);
  EXPECT_EQ(second.calls.load(), 0);

  StopGuard stop{this, [&] { halt = true; }};
  d.Start();
  EXPECT_TRUE(WaitFor([&] { return first.calls.load() > 0 && second.calls.load() > 0; }));
  EXPECT_TRUE(WaitFor([&] { return more.back()->calls.load() > 0; }));
  halt = true;
}

// EXT-37 A lossless producer filling the queue of a subscriber that never
// started blocks like behind a slow consumer; shutdown without Start() frees it
// and no callback ever runs.
TEST_F(ShmDriverTest, ShutdownBeforeStartUnblocksProducer) {
  auto& d = Init(YAML::Load(kLosslessOptions));
  const auto info = IntInfo(UniqueTopic());
  ASSERT_TRUE(d.EnsurePodTopic(info.topic_name, kIntType, 8, 8));
  PodValues rx;
  ASSERT_TRUE(d.RegisterPodSubscriber(info.topic_name, kIntType, RecordPod(&rx)));
  const auto url = PodUrl(info.topic_name, kIntType);
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(url); }));
  auto* pub = Publisher(url);
  ASSERT_NE(pub, nullptr);

  std::atomic_uint64_t published{0};
  JoiningThread producer([&] {
    for (uint64_t v = 1; v <= 300; ++v) {
      transport::LoanedSampleHandle h;
      if (!d.LoanSample(info, sizeof(v), transport::LoanMode::kPodOnly, nullptr, h)) return;
      std::memcpy(h.user_payload_ptr, &v, sizeof(v));
      d.PublishLoaned(h);
      published.fetch_add(1);
    }
  });
  ASSERT_TRUE(WaitFor([&] { return pub->Stats().blocked > 0; }));
  EXPECT_EQ(published.load(), 256u);

  ASSERT_TRUE(d.TryPrepareShutdown(5s));
  d.Shutdown();
  producer = JoiningThread();
  EXPECT_EQ(rx.count.load(), 0u);
  EXPECT_EQ(bus_->PublisherCount(), 0u);
  EXPECT_EQ(bus_->SubscriptionCount(), 0u);
}

// ============================================================ retained limit

// EXT-38 max_outstanding 256 leaves no spare held slot: a sample over the limit
// must not stay queued forever. It is dropped on the next release, and the
// subscription keeps delivering.
TEST_F(ShmDriverTest, RetainedAtMaxLimitDoesNotStrandSamples) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  ASSERT_TRUE(d.EnsurePodTopic(info.topic_name, kIntType, 8, 8));
  RetainedProbe probe;
  ShmSubscriptionOptions options;
  options.retained = true;
  options.max_outstanding = shm::kMaxChunksHeldPerSubscriber;
  auto sub = bus_->RegisterSubscriber(
      PodUrl(info.topic_name, kIntType),
      [&probe](const void* p, size_t, std::function<void()>&& release) {
        uint64_t v;
        std::memcpy(&v, p, sizeof(v));
        std::lock_guard<std::mutex> lock(probe.mu);
        probe.values.push_back(v);
        probe.releases.push_back(std::move(release));
      },
      options);
  StopGuard stop{this, [&] { probe.ReleaseAll(); }};
  d.Start();
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(PodUrl(info.topic_name, kIntType)); }));

  for (uint64_t v = 1; v <= shm::kMaxChunksHeldPerSubscriber; ++v) {
    PublishPod(d, info, v);
    ASSERT_TRUE(WaitFor([&] { return probe.Count() == v; }));
  }
  EXPECT_EQ(sub->Outstanding(), size_t{shm::kMaxChunksHeldPerSubscriber});

  PublishPod(d, info, 1000);  // over the limit, finds every held slot taken
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(probe.Count(), size_t{shm::kMaxChunksHeldPerSubscriber});
  EXPECT_EQ(sub->Stats().takes, uint64_t{shm::kMaxChunksHeldPerSubscriber});

  probe.ReleaseOne();
  EXPECT_TRUE(WaitFor([&] { return sub->Dropped() == 1; }));
  EXPECT_EQ(sub->Stats().takes, uint64_t{shm::kMaxChunksHeldPerSubscriber} + 1);
  EXPECT_EQ(probe.Count(), size_t{shm::kMaxChunksHeldPerSubscriber});

  PublishPod(d, info, 2000);  // a released slot serves the next sample
  ASSERT_TRUE(WaitFor([&] { return probe.Count() == size_t{shm::kMaxChunksHeldPerSubscriber} + 1; }));
  std::lock_guard<std::mutex> lock(probe.mu);
  EXPECT_EQ(probe.values.back(), 2000u);
}

// ============================================================ hot path lifetime

// EXT-39 A releaser on the lock-free path never touches the gate after a waiter
// saw it idle: the gate is freed immediately (ASan/TSan judge this).
TEST(ShmGateTest, ReleaseNeverOvertakesWaiterThatFreesTheGate) {
  for (int i = 0; i < 2000; ++i) {
    auto gate = std::make_unique<ShmOperationGate>();
    auto lease = gate->TryAcquire();
    ASSERT_TRUE(lease);
    std::atomic_bool go{false};
    JoiningThread releaser([&go, l = std::move(lease)]() mutable {
      while (!go.load()) {
      }
      l = ShmOperationGate::Lease();
    });
    if (i % 2 == 0) go = true;  // both orders of release and Close
    gate->Close();
    go = true;
    ASSERT_TRUE(gate->WaitIdle(std::chrono::steady_clock::now() + 10s));
    gate.reset();
  }
}

// EXT-40 Shutdown under concurrent multi-topic Send and handle publishing: every
// counted operation finished before anything is freed, on the fast paths too.
TEST_F(ShmDriverTest, ShutdownRacesConcurrentSendAndLoans) {
  for (int round = 0; round < 10; ++round) {
    driver_.reset();
    auto& d = Init();
    constexpr int kTopics = 8;
    std::vector<transport::TransportInfo> infos;
    for (int i = 0; i < kTopics; ++i) {
      infos.push_back(IntInfo(UniqueTopic("/" + std::to_string(i))));
      ASSERT_TRUE(d.RegisterSendType(SendType(infos.back())));
      ASSERT_TRUE(d.EnsurePodTopic(infos.back().topic_name, kIntType, 64, 8));
    }
    d.Start();
    std::atomic_bool halt{false};
    std::atomic_uint64_t ops{0};
    std::vector<JoiningThread> threads;
    for (int t = 0; t < kTopics; ++t) {
      threads.emplace_back([&, t] {
        IntMsg msg{t};
        auto ctx = PubContext();
        while (!halt.load(std::memory_order_relaxed)) {
          transport::MessageFrame frame{.info = infos[t], .msg_ptr = &msg, .ctx_ref = ctx};
          d.Send(frame);
          transport::LoanedSampleHandle h;
          if (d.LoanSample(infos[t], 8, transport::LoanMode::kPodOnly, nullptr, h)) d.PublishLoaned(h);
          ops.fetch_add(1, std::memory_order_relaxed);
        }
      });
    }
    auto halt_threads = [&] {
      halt = true;
      threads.clear();
    };
    StopGuard stop{this, halt_threads};
    ASSERT_TRUE(WaitFor([&] { return ops.load() > 50; }));
    ASSERT_TRUE(d.TryPrepareShutdown(10s));
    d.Shutdown();
    EXPECT_EQ(bus_->PublisherCount(), 0u);
    const uint64_t at_shutdown = ops.load();
    ASSERT_TRUE(WaitFor([&] { return ops.load() > at_shutdown + 50; }));  // refused, not crashed
    halt_threads();
    EXPECT_EQ(d.SendFailures(), 0u);
  }
}

// EXT-41 Waits are bounded by their steady deadline, also when already past it.
TEST(ShmGateTest, WaitsRespectDeadline) {
  ShmOperationGate gate;
  auto lease = gate.TryAcquire();
  gate.Close();
  EXPECT_FALSE(gate.TryAcquire());
  auto begin = std::chrono::steady_clock::now();
  EXPECT_FALSE(gate.WaitIdle(begin - 1s));
  EXPECT_FALSE(gate.WaitIdle(std::chrono::steady_clock::now() + 30ms));
  const auto elapsed = std::chrono::steady_clock::now() - begin;
  EXPECT_GE(elapsed, 30ms);
  EXPECT_LT(elapsed, 5s);
  lease = ShmOperationGate::Lease();
  EXPECT_TRUE(gate.WaitIdle(std::chrono::steady_clock::now() + 5s));
}

// ============================================================ send failures

// Counts what reaches the extension's logger; undone on destruction.
class ScopedLogCounter {
 public:
  ScopedLogCounter() { g_logger.Set(ibmw::logger::LoggerRef(&base_)); }
  ~ScopedLogCounter() { g_logger.Set(ibmw::logger::LoggerRef()); }
  uint64_t Errors() const { return errors_.load(); }
  std::string Last() {
    std::lock_guard<std::mutex> lock(mu_);
    return last_;
  }

 private:
  ibmw_logger_base_t base_{
      .get_log_level = [](void*) -> ibmw_log_level_t { return IBMW_LOG_LEVEL_ERROR; },
      .log = [](void* ctx, ibmw_log_level_t, uint32_t, const char*, const char*, const char* data, size_t size) {
        auto* self = static_cast<ScopedLogCounter*>(ctx);
        self->errors_.fetch_add(1);
        std::lock_guard<std::mutex> lock(self->mu_);
        self->last_.assign(data, size);
      },
      .ctx = this};
  std::atomic_uint64_t errors_{0};
  std::mutex mu_;
  std::string last_;
};

// EXT-42 Failing sends are counted one by one and logged at most once a second.
TEST_F(ShmDriverTest, SendFailuresAreCountedAndRateLimited) {
  auto& d = Init();
  StopGuard stop{this};
  d.Start();
  const auto unregistered = IntInfo(UniqueTopic());
  ScopedLogCounter logs;
  const auto begin = std::chrono::steady_clock::now();
  for (int i = 0; i < 1000; ++i) SendInt(unregistered, i);
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - begin);
  EXPECT_EQ(d.SendFailures(), 1000u);
  EXPECT_GE(logs.Errors(), 1u);
  EXPECT_LE(logs.Errors(), static_cast<uint64_t>(seconds.count()) + 2);
  EXPECT_EQ(d.SendFailureLogsSuppressed(), 1000u - logs.Errors());
  EXPECT_NE(logs.Last().find("not registered"), std::string::npos) << logs.Last();

  // The next period logs again and says how much went unlogged.
  std::this_thread::sleep_for(1100ms);
  const auto before = logs.Errors();
  SendInt(unregistered, 0);
  EXPECT_EQ(logs.Errors(), before + 1);
  EXPECT_NE(logs.Last().find("1001 send failures"), std::string::npos) << logs.Last();

  const auto report = d.GenInitializationReport();
  std::map<std::string, std::string> rows(report.begin(), report.end());
  EXPECT_EQ(rows["send failures"], "1001");
  EXPECT_EQ(rows["send failure logs suppressed"], std::to_string(d.SendFailureLogsSuppressed()));
}

// ============================================================ configuration

// Sets an environment variable for one scope.
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    if (const char* v = std::getenv(name)) old_ = v;
    ::setenv(name, value, 1);
  }
  ~ScopedEnv() {
    if (old_) {
      ::setenv(name_, old_->c_str(), 1);
    } else {
      ::unsetenv(name_);
    }
  }

 private:
  const char* name_;
  std::optional<std::string> old_;
};

std::string InitializeError(ShmBus& bus, const ShmBusOptions& options) {
  try {
    bus.Initialize(options);
  } catch (const std::exception& e) {
    return e.what();
  }
  return {};
}

// EXT-43 Invalid location settings fail Initialize with an error that names
// them, and the report shows the location in effect.
TEST_F(ShmDriverTest, InvalidLocationIsReportedClearly) {
  {
    ScopedEnv env("IBMW_SHM_GROUP", "not-a-gid");
    ShmBus bus;
    const auto error = InitializeError(bus, {});
    EXPECT_NE(error.find("IBMW_SHM_GROUP"), std::string::npos) << error;
  }
  {
    ShmBus bus;
    ShmBusOptions options;
    options.domain = std::string(33, 'd');
    const auto error = InitializeError(bus, options);
    EXPECT_NE(error.find("domain"), std::string::npos) << error;
  }
  {
    // A numeric group selects another service directory than the running one.
    ShmBus bus;
    ShmBusOptions options;
    options.group = ::getegid();
    const auto error = InitializeError(bus, options);
    EXPECT_NE(error.find("group '" + std::to_string(::getegid()) + "'"), std::string::npos) << error;
  }
  auto& d = Init();
  const auto report = d.GenInitializationReport();
  std::map<std::string, std::string> rows(report.begin(), report.end());
  const std::string dir = shm::Runtime::ServiceDirectory();
  ASSERT_FALSE(rows["domain"].empty());
  EXPECT_EQ(dir.substr(dir.size() - rows["domain"].size() - 1), "." + rows["domain"]);
}

// EXT-44 YAML group and require_read_only reach the bus, the runtime and the
// publisher options; invalid values are refused with a clear error.
TEST(ShmExtensionOptionsTest, GroupAndRequireReadOnly) {
  auto parsed = ParseShmExtensionOptions(YAML::Load("{group: 1234, require_read_only: true, domain: d}"));
  ASSERT_TRUE(parsed.group.has_value());
  EXPECT_EQ(*parsed.group, 1234u);
  EXPECT_TRUE(parsed.require_read_only);
  const auto bus_options = MakeShmBusOptions(parsed);
  EXPECT_EQ(bus_options.group, std::optional<gid_t>(1234));
  EXPECT_TRUE(bus_options.require_read_only);
  EXPECT_TRUE(MakeShmPublisherOptions(bus_options, {}).require_read_only);

  auto defaults = ParseShmExtensionOptions(YAML::Load("{domain: d}"));
  EXPECT_FALSE(defaults.group.has_value());
  EXPECT_FALSE(defaults.require_read_only);
  EXPECT_FALSE(MakeShmPublisherOptions(MakeShmBusOptions(defaults), {}).require_read_only);

  for (const char* bad : {"{group: staff}", "{group: -1}", "{group: 4294967295}", "{group: 4294967296}",
                          "{group: 1.5}", "{group: \"\"}", "{group: [1]}", "{group: {a: 1}}"}) {
    try {
      (void)ParseShmExtensionOptions(YAML::Load(bad));
      ADD_FAILURE() << bad << " was accepted";
    } catch (const std::runtime_error& e) {
      EXPECT_NE(std::string(e.what()).find("numeric gid"), std::string::npos) << bad << ": " << e.what();
    }
  }
  EXPECT_FALSE(ParseShmGroupId("+1").has_value());
  EXPECT_FALSE(ParseShmGroupId(" 1").has_value());
  EXPECT_EQ(ParseShmGroupId("0"), std::optional<gid_t>(0));
  EXPECT_EQ(ParseShmGroupId("4294967294"), std::optional<gid_t>(4294967294u));
}

// EXT-45 A publisher works under require_read_only on this kernel, and the
// selfdev health counters reach the initialization report.
TEST_F(ShmDriverTest, ReportsSelfdevHealthCounters) {
  ShmBusOptions bus_options;
  bus_options.require_read_only = true;
  auto& d = Init(YAML::Node(), bus_options);
  const auto info = IntInfo(UniqueTopic());
  Received rx;
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.OnMessage(Handler(info, &rx)));
  StopGuard stop{this};
  d.Start();
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(StdUrl(info.topic_name, kIntType)); }));
  SendInt(info, 1);
  ASSERT_TRUE(WaitFor([&] { return rx.Ints() == 1; }));

  const auto stats = bus_->Stats();
  const auto report = d.GenInitializationReport();
  std::map<std::string, std::string> rows(report.begin(), report.end());
  EXPECT_EQ(rows["require read only"], "true");
  EXPECT_EQ(rows["quarantined"], std::to_string(stats.quarantined));
  EXPECT_EQ(rows["quarantined chunks"], std::to_string(stats.quarantined_chunks));
  EXPECT_EQ(rows["unverifiable peers"], std::to_string(stats.unverifiable_peers));
  EXPECT_EQ(rows["unsealed segments"], std::to_string(stats.unsealed_segments));
  EXPECT_EQ(rows["unknown messages"], std::to_string(stats.unknown_messages));
  EXPECT_EQ(rows["deferred sends"], std::to_string(stats.deferred_sends));
  EXPECT_EQ(rows["send queue overflows"], std::to_string(stats.send_queue_overflows));
  EXPECT_EQ(rows["connect retries"], std::to_string(stats.connect_retries));
  EXPECT_EQ(stats.unsealed_segments, 0u);
}

// EXT-46 Bus counters are the sum over the live publishers and subscriptions.
TEST_F(ShmDriverTest, BusStatsSumEveryEndpoint) {
  auto& d = Init();
  const auto a = IntInfo(UniqueTopic("/a"));
  const auto b = IntInfo(UniqueTopic("/b"));
  ASSERT_TRUE(d.EnsurePodTopic(a.topic_name, kIntType, 8, 8));
  ASSERT_TRUE(d.EnsurePodTopic(b.topic_name, kIntType, 8, 8));
  PodValues rx;
  ASSERT_TRUE(d.RegisterPodSubscriber(a.topic_name, kIntType, RecordPod(&rx)));
  StopGuard stop{this};
  d.Start();
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(PodUrl(a.topic_name, kIntType)); }));
  const auto stats = bus_->Stats();
  uint64_t expected_deferred = 0;
  for (const auto& url : {PodUrl(a.topic_name, kIntType), PodUrl(b.topic_name, kIntType)}) {
    auto* pub = Publisher(url);
    ASSERT_NE(pub, nullptr);
    expected_deferred += pub->Stats().deferred_sends;
  }
  EXPECT_GE(stats.deferred_sends, expected_deferred);
  EXPECT_EQ(stats.quarantined, 0u);
}

// ============================================================ re-Initialize

// EXT-47 A bus that was shut down starts again with the same location: all old
// state is gone, delivery is gated again, the old driver stays refused.
TEST_F(ShmDriverTest, BusReinitializesAfterShutdown) {
  auto& first = Init();
  EXPECT_THROW(bus_->Initialize({}), std::runtime_error);  // still live
  const auto info = IntInfo(UniqueTopic());
  Received rx1;
  ASSERT_TRUE(first.RegisterSendType(SendType(info)));
  ASSERT_TRUE(first.EnsurePodTopic(info.topic_name, kIntType, 8, 8));
  ASSERT_TRUE(first.OnMessage(Handler(info, &rx1)));
  first.Start();
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(StdUrl(info.topic_name, kIntType)); }));
  SendInt(info, 1);
  ASSERT_TRUE(WaitFor([&] { return rx1.Ints() == 1; }));

  // The engine path: the extension shuts the bus down, the driver never hears.
  ASSERT_TRUE(bus_->TryPrepareShutdown(5s));
  ASSERT_TRUE(bus_->Shutdown());
  auto stale = std::move(driver_);

  // A failed attempt changes nothing: the bus stays shut down and can retry.
  ShmBusOptions conflicting;
  conflicting.domain = "another_domain";
  const auto error = InitializeError(*bus_, conflicting);
  EXPECT_NE(error.find("another_domain"), std::string::npos) << error;
  EXPECT_FALSE(bus_->TryAcquireOperation());

  bus_->Initialize({});
  EXPECT_THROW(bus_->Initialize({}), std::runtime_error);
  EXPECT_EQ(bus_->PublisherCount(), 0u);
  EXPECT_EQ(bus_->SubscriptionCount(), 0u);
  EXPECT_EQ(bus_->ListenerThreads(), 0u);

  driver_ = std::make_unique<ShmTransportDriver>(*bus_);
  driver_->Initialize(YAML::Node());
  Received rx2;
  ASSERT_TRUE(driver_->RegisterSendType(SendType(info)));
  ASSERT_TRUE(driver_->EnsurePodTopic(info.topic_name, kIntType, 8, 8));
  ASSERT_TRUE(driver_->OnMessage(Handler(info, &rx2)));
  EXPECT_EQ(bus_->ListenerThreads(), 0u);
  StopGuard stop{this};
  driver_->Start();
  EXPECT_EQ(bus_->ListenerThreads(), 1u);
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(StdUrl(info.topic_name, kIntType)); }));
  SendInt(info, 2);
  ASSERT_TRUE(WaitFor([&] { return rx2.Ints() == 1; }));
  EXPECT_EQ(rx2.Int(0), 2);
  EXPECT_EQ(rx1.Ints(), 1u);
  auto* fresh = Publisher(StdUrl(info.topic_name, kIntType));
  ASSERT_NE(fresh, nullptr);
  EXPECT_EQ(fresh->Stats().publishes, 1u);

  // The old driver's cached publishers are gone: refused, not dereferenced.
  transport::LoanedSampleHandle h;
  EXPECT_FALSE(stale->LoanSample(info, 8, transport::LoanMode::kPodOnly, nullptr, h));
  IntMsg msg{3};
  auto ctx = PubContext();
  transport::MessageFrame frame{.info = info, .msg_ptr = &msg, .ctx_ref = ctx};
  stale->Send(frame);
  // Refused quietly like any send of a stopped driver: not a send failure,
  // and nothing reaches the new generation. Send() publishes synchronously,
  // so the new publisher's count is final when it returns.
  EXPECT_EQ(stale->SendFailures(), 0u);
  EXPECT_EQ(fresh->Stats().publishes, 1u);
  // Nor can it shut the new generation down.
  EXPECT_TRUE(stale->TryPrepareShutdown(1s));
  stale->Shutdown();
  EXPECT_TRUE(bus_->TryAcquireOperation());
  SendInt(info, 4);
  ASSERT_TRUE(WaitFor([&] { return rx2.Ints() == 2; }));
  EXPECT_EQ(rx2.Int(1), 4);
  EXPECT_EQ(fresh->Stats().publishes, 2u);
}

// EXT-53 A publish the library refuses (out of memory while a subscriber is
// connected) gives the loan back: for a POD and a ctx-meta handle, and in
// Send(), which also counts the failure. Nothing is published, every slot
// stays usable and shutdown prepares at once.
TEST_F(ShmDriverTest, RefusedPublishReleasesLoan) {
  auto& d = Init();
  const auto info = BlobInfo(UniqueTopic());
  Received rx;
  PodValues pods;
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.EnsurePodTopic(info.topic_name, kBlobType, 8, 8));
  ASSERT_TRUE(d.OnMessage(Handler(info, &rx)));
  ASSERT_TRUE(d.RegisterPodSubscriber(info.topic_name, kBlobType, RecordPod(&pods)));
  StopGuard stop{this};
  d.Start();
  const auto std_url = StdUrl(info.topic_name, kBlobType);
  const auto pod_url = PodUrl(info.topic_name, kBlobType);
  // The library only allocates in a publish when it has a subscriber.
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(std_url) && HasSubscribers(pod_url); }));
  auto* std_pub = Publisher(std_url);
  auto* pod_pub = Publisher(pod_url);
  ASSERT_NE(std_pub, nullptr);
  ASSERT_NE(pod_pub, nullptr);

  transport::LoanedSampleHandle pod;
  ASSERT_TRUE(d.LoanSample(info, 8, transport::LoanMode::kPodOnly, nullptr, pod));
  shm::test::FailAllocations(1);
  EXPECT_FALSE(d.TryPublishLoaned(pod));
  shm::test::FailAllocations(0);
  EXPECT_EQ(pod.driver_token, nullptr);
  EXPECT_EQ(pod_pub->Outstanding(), 0u);
  EXPECT_EQ(pod_pub->Stats().publishes, 0u);

  auto ctx = PubContext();
  transport::LoanedSampleHandle meta;
  ASSERT_TRUE(d.LoanSample(info, 4, transport::LoanMode::kCtxMetaThenPayload, ctx.get(), meta));
  std::memcpy(meta.user_payload_ptr, "lost", 4);
  shm::test::FailAllocations(1);
  EXPECT_FALSE(d.TryPublishLoaned(meta));
  shm::test::FailAllocations(0);
  EXPECT_EQ(std_pub->Outstanding(), 0u);
  EXPECT_EQ(std_pub->Stats().publishes, 0u);

  // Send() from a serialization cache allocates nothing of its own once warm,
  // so the injected failure hits the library's publish.
  BlobMsg msg{MakeBlob(100, 'r')};
  auto array = std::make_shared<ibmw::util::BufferArray>();
  ASSERT_TRUE(info.msg_type_support_ref.Serialize(kSerType, &msg, array->AllocatorNativeHandle(),
                                                  array->BufferArrayNativeHandle()));
  transport::MessageFrame frame{.info = info, .msg_ptr = nullptr, .ctx_ref = ctx};
  frame.serialization_cache.emplace(kSerType, std::shared_ptr<ibmw::util::BufferArrayView>(
                                                  new ibmw::util::BufferArrayView(*array),
                                                  [array](const ibmw::util::BufferArrayView* p) { delete p; }));
  d.Send(frame);
  ASSERT_TRUE(WaitFor([&] { return rx.Blobs() == 1; }));
  {
    ScopedLogCounter log;
    shm::test::FailAllocations(1);
    d.Send(frame);
    shm::test::FailAllocations(0);
    EXPECT_EQ(log.Errors(), 1u);
    EXPECT_NE(log.Last().find("was refused"), std::string::npos) << log.Last();
  }
  EXPECT_EQ(d.SendFailures(), 1u);
  EXPECT_EQ(std_pub->Outstanding(), 0u);
  EXPECT_EQ(std_pub->Stats().publishes, 1u);

  d.Send(frame);
  ASSERT_TRUE(WaitFor([&] { return rx.Blobs() == 2; }));
  EXPECT_EQ(rx.Blob(1), msg.data);
  PublishPod(d, info, 7);
  ASSERT_TRUE(WaitFor([&] { return pods.count.load() == 1; }));
  EXPECT_EQ(pods.Snapshot().at(0), 7u);
  EXPECT_TRUE(d.TryPrepareShutdown(1s));
}

// EXT-54 A Start() that throws while attaching (out of memory) can be
// retried and the retry delivers; once a Start() succeeded, another throws.
TEST_F(ShmDriverTest, StartCanBeRetriedAfterFailure) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  Received rx;
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.OnMessage(Handler(info, &rx)));
  StopGuard stop{this};
  shm::test::FailAllocations(1);
  EXPECT_THROW(d.Start(), std::bad_alloc);
  shm::test::FailAllocations(0);
  EXPECT_EQ(bus_->ListenerThreads(), 0u);
  d.Start();
  EXPECT_EQ(bus_->ListenerThreads(), 1u);
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(StdUrl(info.topic_name, kIntType)); }));
  SendInt(info, 7);
  ASSERT_TRUE(WaitFor([&] { return rx.Ints() == 1; }));
  EXPECT_EQ(rx.Int(0), 7);
  try {
    d.Start();
    ADD_FAILURE() << "a second Start() after a successful one must throw";
  } catch (const std::exception& e) {
    EXPECT_NE(std::string(e.what()).find("again after a Start() that threw"), std::string::npos) << e.what();
  }
  EXPECT_EQ(bus_->ListenerThreads(), 1u);
}

// EXT-55 Before Start() a POD loan looks its topic up while another thread
// registers POD topics: no data race (TSAN build) and every loan succeeds.
TEST_F(ShmDriverTest, PreStartPodLoanRacesPodTopicRegistration) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  ASSERT_TRUE(d.EnsurePodTopic(info.topic_name, kIntType, 8, 8));
  std::atomic_bool done{false};
  size_t loans = 0;
  {
    JoiningThread registrar([&] {
      for (int i = 0; i < 64; ++i) {
        EXPECT_TRUE(d.EnsurePodTopic(info.topic_name + "/" + std::to_string(i), kIntType, 8, 8));
      }
      done.store(true);
    });
    do {
      transport::LoanedSampleHandle h;
      if (!d.LoanSample(info, 8, transport::LoanMode::kPodOnly, nullptr, h)) break;
      d.ReleaseLoaned(h);
      ++loans;
    } while (!done.load());
  }
  EXPECT_TRUE(done.load());
  EXPECT_GT(loans, 0u);
  transport::LoanedSampleHandle last;
  EXPECT_TRUE(d.LoanSample(info, 8, transport::LoanMode::kPodOnly, nullptr, last));
  d.ReleaseLoaned(last);
  EXPECT_EQ(Publisher(PodUrl(info.topic_name, kIntType))->Outstanding(), 0u);
  EXPECT_EQ(bus_->PublisherCount(), 65u);
}

// EXT-56 A loan held across TryPrepareShutdown() makes it fail, and the
// holder can still publish or give back its handles while shutdown prepares;
// then preparation and Shutdown() succeed.
TEST_F(ShmDriverTest, LoansHeldAcrossPrepareCanStillBeReturned) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.EnsurePodTopic(info.topic_name, kIntType, 8, 8));
  StopGuard stop{this};
  d.Start();
  auto* std_pub = Publisher(StdUrl(info.topic_name, kIntType));
  auto* pod_pub = Publisher(PodUrl(info.topic_name, kIntType));
  ASSERT_NE(std_pub, nullptr);
  ASSERT_NE(pod_pub, nullptr);

  auto ctx = PubContext();
  transport::LoanedSampleHandle meta, pod;
  ASSERT_TRUE(d.LoanSample(info, sizeof(IntMsg), transport::LoanMode::kCtxMetaThenPayload, ctx.get(), meta));
  ASSERT_TRUE(d.LoanSample(info, 8, transport::LoanMode::kPodOnly, nullptr, pod));
  const IntMsg msg{5};
  std::memcpy(meta.user_payload_ptr, &msg, sizeof(msg));
  EXPECT_FALSE(d.TryPrepareShutdown(50ms));
  EXPECT_FALSE(d.TryPrepareShutdown(10ms));  // retryable
  transport::LoanedSampleHandle refused;
  EXPECT_FALSE(d.LoanSample(info, 8, transport::LoanMode::kPodOnly, nullptr, refused));

  EXPECT_TRUE(d.TryPublishLoaned(meta));
  EXPECT_EQ(std_pub->Outstanding(), 0u);
  EXPECT_EQ(std_pub->Stats().publishes, 1u);
  EXPECT_FALSE(d.TryPrepareShutdown(10ms));  // the POD loan is still out
  d.ReleaseLoaned(pod);
  EXPECT_EQ(pod.driver_token, nullptr);
  EXPECT_EQ(pod_pub->Outstanding(), 0u);
  EXPECT_TRUE(d.TryPrepareShutdown(1s));
  d.Shutdown();
  EXPECT_EQ(bus_->PublisherCount(), 0u);
}

// EXT-57 After Shutdown() every loan fails cleanly, and copies of handles
// from before are refused without touching the freed publishers (ASAN build).
TEST_F(ShmDriverTest, LoansAfterShutdownFailCleanly) {
  auto& d = Init();
  const auto info = IntInfo(UniqueTopic());
  constexpr std::string_view kRawType = "shm_ext_test/AfterShutdownRaw";
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.EnsurePodTopic(info.topic_name, kIntType, 8, 8));
  auto raw = CreateRawPodPublisher(&d, info.topic_name, kRawType, 64);
  auto ctx_pub = CreateCtxMetaPublisher(&d, info.topic_name, kIntType);
  ASSERT_NE(raw, nullptr);
  ASSERT_NE(ctx_pub, nullptr);
  d.Start();

  auto ctx = PubContext();
  transport::LoanedSampleHandle pod, meta;
  ASSERT_TRUE(d.LoanSample(info, 8, transport::LoanMode::kPodOnly, nullptr, pod));
  ASSERT_TRUE(d.LoanSample(info, sizeof(IntMsg), transport::LoanMode::kCtxMetaThenPayload, ctx.get(), meta));
  const auto pod_copy = pod;
  const auto meta_copy = meta;
  d.ReleaseLoaned(pod);
  d.ReleaseLoaned(meta);
  d.Shutdown();
  ASSERT_EQ(bus_->PublisherCount(), 0u);

  transport::LoanedSampleHandle h;
  EXPECT_FALSE(d.LoanSample(info, 8, transport::LoanMode::kPodOnly, nullptr, h));
  EXPECT_FALSE(d.LoanSample(info, sizeof(IntMsg), transport::LoanMode::kCtxMetaThenPayload, ctx.get(), h));
  const IntMsg msg{1};
  EXPECT_FALSE(d.LoanSerializedSample(info, *ctx, &msg, h));
  EXPECT_EQ(h.driver_token, nullptr);
  EXPECT_FALSE(raw->borrow_loaned_sample(8).is_valid());
  EXPECT_FALSE(ctx_pub->borrow_loaned_buffer(*ctx, 8).is_valid());

  auto s1 = pod_copy, s2 = pod_copy, s3 = meta_copy, s4 = meta_copy;
  EXPECT_FALSE(d.TryPublishLoaned(s1));
  d.ReleaseLoaned(s2);
  EXPECT_FALSE(d.TryPublishLoaned(s3));
  d.ReleaseLoaned(s4);
  EXPECT_EQ(s2.driver_token, nullptr);
  EXPECT_EQ(s4.driver_token, nullptr);
  SendInt(info, 2);
  EXPECT_EQ(d.SendFailures(), 0u);  // refused quietly, like any send after shutdown
}

}  // namespace

// ============================================ iceoryx_ext-compatible helpers

namespace {

struct CompatPod {
  int32_t seq;
  int32_t value;
};

}  // namespace

// Every member of the class templates compiles.
template class IoxLoanedSample<CompatPod>;
template class IoxStylePublisher<CompatPod>;
template class IoxLoanedSample<std::byte>;

}  // namespace ibmw::extensions::shm_extension

// The typed ctx-meta publisher finds its type support through this trait,
// which generated typesupport headers specialize.
template <>
struct ibmw::MessageTypeSupportTraits<ibmw::extensions::shm_extension::BlobMsg> {
  static const ibmw_type_support_base_t* Get() { return ibmw::extensions::shm_extension::BlobTypeSupport(); }
};

namespace ibmw::extensions::shm_extension {
namespace {

// Ported iceoryx_ext code names the driver IceoryxTransportDriver and runs
// the R8.2 check unchanged.
static_assert(std::is_same_v<IceoryxTransportDriver, ShmTransportDriver>);
struct LegacyOnlyModule {
  using legacy_iox_marker = IoxStylePublisher<CompatPod>;
};
static_assert(IbmwUsesLegacyIceoryxApi<LegacyOnlyModule>::value);
static_assert(!IbmwHasV2DriverSlot<LegacyOnlyModule>::value);
static_assert(sizeof(IbmwR82SlotMixCheck<LegacyOnlyModule>) > 0);

// EXT-48 Loan wrappers of iox_style_publisher.h without a driver: empty is
// invalid and a no-op to destroy, a too-small loan is not a valid T,
// ExtractForPublish hands over the handle and leaves the wrapper empty.
TEST(ShmIoxCompatTest, LoanWrappersWithoutDriver) {
  IoxLoanedSample<CompatPod> empty;
  EXPECT_FALSE(empty.is_valid());
  IoxCtxMetaLoanedBuffer empty_buf;
  EXPECT_FALSE(empty_buf.is_valid());
  EXPECT_EQ(empty_buf.size(), 0u);

  CompatPod storage{};
  transport::LoanedSampleHandle h;
  h.user_payload_ptr = &storage;
  h.user_payload_size = sizeof(CompatPod) - 1;
  // Never released: a null driver marks the wrappers as not owning a loan.
  IoxLoanedSample<CompatPod> short_loan(nullptr, h);
  EXPECT_FALSE(short_loan.is_valid());
  h.user_payload_size = sizeof(CompatPod);
  IoxLoanedSample<CompatPod> no_driver(nullptr, h);
  EXPECT_FALSE(no_driver.is_valid());
  EXPECT_EQ(no_driver.get(), &storage);
  const auto out = no_driver.ExtractForPublish();
  EXPECT_EQ(out.user_payload_ptr, &storage);
  EXPECT_EQ(no_driver.get(), nullptr);

  IoxCtxMetaLoanedBuffer buf(nullptr, h);
  EXPECT_FALSE(buf.is_valid());
  EXPECT_EQ(buf.ExtractForPublish().user_payload_ptr, &storage);
  EXPECT_EQ(buf.data(), nullptr);

  EXPECT_EQ(CreatePodPublisher<CompatPod>(nullptr, "t", "CompatPod"), nullptr);
  EXPECT_FALSE(RegisterPodSubscriber<CompatPod>(nullptr, "t", "CompatPod", [](const CompatPod&) {}));
  EXPECT_EQ(CreateRawPodPublisher(nullptr, "t", "Raw", 64), nullptr);
  EXPECT_FALSE(RegisterRawPodSubscriber(nullptr, "t", "Raw", [](const void*, size_t) {}));
  EXPECT_EQ(CreateCtxMetaPublisher(nullptr, "t", kBlobType), nullptr);
  EXPECT_EQ(CreateCtxMetaTypedPublisher<BlobMsg>(nullptr, "t", kBlobType), nullptr);
  IoxCtxMetaTypedPublisher<BlobMsg> typed_no_driver(nullptr, "t", kBlobType);
  EXPECT_FALSE(typed_no_driver.publish(*PubContext(), BlobMsg{"x"}));
}

// EXT-49 Typed POD path (CreatePodPublisher / IoxStylePublisher /
// RegisterPodSubscriber): Init-only registration, write in place and publish,
// variable-size loans, smaller payloads skipped by the typed subscriber,
// unpublished loans given back, and publish() false for a duplicate or
// foreign handle instead of claiming success.
TEST_F(ShmDriverTest, IoxCompatPodPublisher) {
  auto& d = Init();
  const std::string topic = UniqueTopic();
  constexpr std::string_view kType = "shm_ext_test/CompatPod";
  auto pub = CreatePodPublisher<CompatPod>(&d, topic, kType);
  ASSERT_NE(pub, nullptr);
  EXPECT_EQ(pub->topic_name(), topic);
  EXPECT_EQ(pub->msg_type(), kType);

  std::mutex mu;
  std::vector<CompatPod> got;
  ASSERT_TRUE(RegisterPodSubscriber<CompatPod>(&d, topic, kType, [&](const CompatPod& msg) {
    std::lock_guard<std::mutex> lock(mu);
    got.push_back(msg);
  }));
  StopGuard stop{this};
  d.Start();
  EXPECT_EQ(CreatePodPublisher<CompatPod>(&d, topic + "/late", kType), nullptr);  // not in Init any more
  const auto url = PodUrl(topic, std::string(kType));
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(url); }));
  auto* raw = Publisher(url);
  ASSERT_NE(raw, nullptr);
  auto received = [&](size_t n) {
    return WaitFor([&] {
      std::lock_guard<std::mutex> lock(mu);
      return got.size() == n;
    });
  };

  EXPECT_FALSE(pub->publish(IoxLoanedSample<CompatPod>()));
  auto sample = pub->borrow_loaned_sample();
  ASSERT_TRUE(sample.is_valid());
  sample->seq = 1;
  (*sample).value = 10;
  ASSERT_TRUE(pub->publish(std::move(sample)));
  EXPECT_FALSE(sample.is_valid());  // NOLINT(bugprone-use-after-move): moved-from state is specified
  ASSERT_TRUE(received(1));

  EXPECT_FALSE(pub->borrow_loaned_sample(sizeof(CompatPod) - 1).is_valid());
  auto big = pub->borrow_loaned_sample(sizeof(CompatPod) + 100);
  ASSERT_TRUE(big.is_valid());
  big->seq = 2;
  std::memset(reinterpret_cast<char*>(big.get()) + sizeof(CompatPod), 'x', 100);
  ASSERT_TRUE(pub->publish(std::move(big)));
  ASSERT_TRUE(received(2));

  // A payload smaller than T on the same topic never reaches the typed callback.
  IoxRawPodPublisher tiny(&d, topic, std::string(kType));
  auto one = tiny.borrow_loaned_sample(1);
  ASSERT_TRUE(one.is_valid());
  ASSERT_TRUE(tiny.publish(std::move(one)));
  auto last = pub->borrow_loaned_sample();
  ASSERT_TRUE(last.is_valid());
  last->seq = 3;
  ASSERT_TRUE(pub->publish(std::move(last)));
  ASSERT_TRUE(received(3));
  {
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_EQ(got[0].seq, 1);
    EXPECT_EQ(got[0].value, 10);
    EXPECT_EQ(got[1].seq, 2);
    EXPECT_EQ(got[2].seq, 3);
  }

  // Unpublished loans go back: on destruction and when overwritten by a move.
  {
    auto a = pub->borrow_loaned_sample();
    auto b = pub->borrow_loaned_sample();
    ASSERT_TRUE(a.is_valid() && b.is_valid());
    EXPECT_EQ(raw->Outstanding(), 2u);
    a = std::move(b);
    EXPECT_EQ(raw->Outstanding(), 1u);
  }
  EXPECT_EQ(raw->Outstanding(), 0u);

  // The same loan wrapped twice: only the first publish happens.
  const uint64_t publishes = raw->Stats().publishes;
  auto h = pub->borrow_loaned_sample().ExtractForPublish();
  ASSERT_NE(h.user_payload_ptr, nullptr);
  IoxLoanedSample<CompatPod> first(&d, h), duplicate(&d, h);
  EXPECT_TRUE(pub->publish(std::move(first)));
  EXPECT_FALSE(pub->publish(std::move(duplicate)));
  EXPECT_EQ(raw->Stats().publishes, publishes + 1);
  EXPECT_EQ(raw->Outstanding(), 0u);

  // A handle of another driver is not this driver's to publish; the real
  // owner still gives the loan back.
  auto mine = pub->borrow_loaned_sample().ExtractForPublish();
  auto foreign = mine;
  foreign.source_driver = nullptr;
  EXPECT_FALSE(pub->publish(IoxLoanedSample<CompatPod>(&d, foreign)));
  EXPECT_EQ(raw->Outstanding(), 1u);
  { IoxLoanedSample<CompatPod> owner(&d, mine); }
  EXPECT_EQ(raw->Outstanding(), 0u);
  EXPECT_EQ(raw->Stats().publishes, publishes + 1);
}

// EXT-50 Raw POD path (CreateRawPodPublisher / IoxRawPodPublisher /
// RegisterRawPodSubscriber) and ShmRawPodPublisher::Publish both report a
// duplicate publish as false.
TEST_F(ShmDriverTest, IoxCompatRawPodPublisher) {
  auto& d = Init();
  const std::string topic = UniqueTopic();
  constexpr std::string_view kRawType = "shm_ext_test/CompatRaw";
  auto pub = CreateRawPodPublisher(&d, topic, kRawType, 512);
  ASSERT_NE(pub, nullptr);
  EXPECT_EQ(pub->topic_name(), topic);
  EXPECT_EQ(pub->msg_type(), kRawType);
  auto native = CreateShmRawPodPublisher(&d, topic, kRawType, 512);
  ASSERT_NE(native, nullptr);

  std::mutex mu;
  std::vector<std::string> got;
  ASSERT_TRUE(RegisterRawPodSubscriber(&d, topic, kRawType, [&](const void* p, size_t size) {
    std::lock_guard<std::mutex> lock(mu);
    got.emplace_back(static_cast<const char*>(p), size);
  }));
  StopGuard stop{this};
  d.Start();
  const auto url = PodUrl(topic, std::string(kRawType));
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(url); }));
  auto* raw = Publisher(url);
  ASSERT_NE(raw, nullptr);

  EXPECT_FALSE(pub->borrow_loaned_sample(0).is_valid());
  EXPECT_FALSE(pub->publish(IoxLoanedSample<std::byte>()));
  auto sample = pub->borrow_loaned_sample(512);
  ASSERT_TRUE(sample.is_valid());
  std::memset(sample.get(), 'r', 512);
  ASSERT_TRUE(pub->publish(std::move(sample)));
  ASSERT_TRUE(WaitFor([&] {
    std::lock_guard<std::mutex> lock(mu);
    return got.size() == 1;
  }));
  {
    std::lock_guard<std::mutex> lock(mu);
    ASSERT_GE(got[0].size(), 512u);
    EXPECT_EQ(got[0].substr(0, 512), std::string(512, 'r'));
  }

  const uint64_t publishes = raw->Stats().publishes;
  auto h = pub->borrow_loaned_sample(64).ExtractForPublish();
  IoxLoanedSample<std::byte> first(&d, h), duplicate(&d, h);
  EXPECT_TRUE(pub->publish(std::move(first)));
  EXPECT_FALSE(pub->publish(std::move(duplicate)));

  auto nh = native->Borrow(64).Extract();
  ShmLoanedBytes native_first(&d, nh), native_duplicate(&d, nh);
  EXPECT_TRUE(native->Publish(std::move(native_first)));
  EXPECT_FALSE(native->Publish(std::move(native_duplicate)));
  EXPECT_EQ(raw->Stats().publishes, publishes + 2);
  EXPECT_EQ(raw->Outstanding(), 0u);
}

// EXT-51 Untyped ctx-meta buffer (CreateCtxMetaPublisher /
// borrow_loaned_buffer, LoanMode::kCtxMetaThenPayload): the header comes from
// the ctx at borrow time, the caller writes the serialized payload, a
// standard handler receives it. No RegisterSendType means no buffer; an
// unpublished buffer goes back; a duplicate publish is false.
TEST_F(ShmDriverTest, IoxCompatCtxMetaBuffer) {
  auto& d = Init();
  const auto info = BlobInfo(UniqueTopic());
  Received rx;
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.OnMessage(Handler(info, &rx)));
  auto pub = CreateCtxMetaPublisher(&d, info.topic_name, kBlobType);
  ASSERT_NE(pub, nullptr);
  EXPECT_EQ(pub->topic_name(), info.topic_name);
  EXPECT_EQ(pub->msg_type(), kBlobType);
  StopGuard stop{this};
  d.Start();
  const auto url = StdUrl(info.topic_name, kBlobType);
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(url); }));
  auto* raw = Publisher(url);
  ASSERT_NE(raw, nullptr);

  auto ctx = PubContext();
  const std::string payload = MakeBlob(3000, 'm');
  auto buf = pub->borrow_loaned_buffer(*ctx, payload.size());
  ASSERT_TRUE(buf.is_valid());
  EXPECT_EQ(buf.size(), payload.size());
  ctx->SetMetaValue("origin", "changed after borrow");  // header already written
  std::memcpy(buf.data(), payload.data(), payload.size());
  EXPECT_FALSE(pub->publish(IoxCtxMetaLoanedBuffer()));
  ASSERT_TRUE(pub->publish(std::move(buf)));
  EXPECT_FALSE(buf.is_valid());  // NOLINT(bugprone-use-after-move): moved-from state is specified
  ASSERT_TRUE(WaitFor([&] { return rx.Blobs() == 1; }));
  EXPECT_EQ(rx.Blob(0), payload);
  EXPECT_EQ(rx.Origin(), "test");

  auto unregistered = CreateCtxMetaPublisher(&d, UniqueTopic("/unregistered"), kBlobType);
  EXPECT_FALSE(unregistered->borrow_loaned_buffer(*PubContext(), 16).is_valid());

  { auto dropped = pub->borrow_loaned_buffer(*ctx, 16); }
  EXPECT_EQ(raw->Outstanding(), 0u);

  const uint64_t publishes = raw->Stats().publishes;
  auto h = pub->borrow_loaned_buffer(*PubContext(), 4).ExtractForPublish();
  std::memcpy(h.user_payload_ptr, "dupl", 4);
  IoxCtxMetaLoanedBuffer first(&d, h), duplicate(&d, h);
  EXPECT_TRUE(pub->publish(std::move(first)));
  EXPECT_FALSE(pub->publish(std::move(duplicate)));
  EXPECT_EQ(raw->Stats().publishes, publishes + 1);
  EXPECT_EQ(raw->Outstanding(), 0u);
  ASSERT_TRUE(WaitFor([&] { return rx.Blobs() == 2; }));
  EXPECT_EQ(rx.Blob(1), "dupl");
}

// EXT-52 Typed ctx-meta publisher (CreateCtxMetaTypedPublisher): serialize
// into the chunk and publish to a standard handler; an unregistered topic
// fails without holding a loan.
TEST_F(ShmDriverTest, IoxCompatCtxMetaTypedPublisher) {
  auto& d = Init();
  const auto info = BlobInfo(UniqueTopic());
  Received rx;
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.OnMessage(Handler(info, &rx)));
  StopGuard stop{this};
  d.Start();
  const auto url = StdUrl(info.topic_name, kBlobType);
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(url); }));

  auto pub = CreateCtxMetaTypedPublisher<BlobMsg>(&d, info.topic_name, kBlobType);
  ASSERT_NE(pub, nullptr);
  EXPECT_EQ(pub->topic_name(), info.topic_name);
  EXPECT_EQ(pub->msg_type(), kBlobType);
  auto ctx = PubContext();
  // Wait per sample: the default delivery is latest-only (queue depth 1).
  for (size_t i = 1; i <= 2; ++i) {
    BlobMsg msg{MakeBlob(5000 * i, static_cast<char>('a' + i))};
    ASSERT_TRUE(pub->publish(*ctx, msg));
    ASSERT_TRUE(WaitFor([&] { return rx.Blobs() == i; }));
    EXPECT_EQ(rx.Blob(i - 1), msg.data);
  }
  EXPECT_EQ(rx.Origin(), "test");
  EXPECT_EQ(Publisher(url)->Outstanding(), 0u);

  auto unregistered = CreateCtxMetaTypedPublisher<BlobMsg>(&d, UniqueTopic("/unregistered"), kBlobType);
  EXPECT_FALSE(unregistered->publish(*ctx, BlobMsg{"x"}));
}

// EXT-58 A sample or buffer borrowed from one driver and handed to a
// publisher of another driver is not published there; its own driver takes
// the loan back (typed, raw and ctx-meta publish()).
TEST_F(ShmDriverTest, IoxCompatForeignSampleGoesBackToItsDriver) {
  auto& a = Init();
  ShmTransportDriver b(*bus_, "shm_b");
  b.Initialize(YAML::Node());
  const std::string topic = UniqueTopic();
  constexpr std::string_view kType = "shm_ext_test/CompatPod";
  constexpr std::string_view kRawType = "shm_ext_test/CompatRaw";
  const auto info_a = BlobInfo(topic + "/a");
  const auto info_b = BlobInfo(topic + "/b");
  auto typed_a = CreatePodPublisher<CompatPod>(&a, info_a.topic_name, kType);
  auto typed_b = CreatePodPublisher<CompatPod>(&b, info_b.topic_name, kType);
  auto raw_a = CreateRawPodPublisher(&a, info_a.topic_name, kRawType, 64);
  auto raw_b = CreateRawPodPublisher(&b, info_b.topic_name, kRawType, 64);
  ASSERT_TRUE(a.RegisterSendType(SendType(info_a)));
  ASSERT_TRUE(b.RegisterSendType(SendType(info_b)));
  auto meta_a = CreateCtxMetaPublisher(&a, info_a.topic_name, kBlobType);
  auto meta_b = CreateCtxMetaPublisher(&b, info_b.topic_name, kBlobType);
  ASSERT_TRUE(typed_a && typed_b && raw_a && raw_b && meta_a && meta_b);
  a.Start();
  b.Start();
  auto* typed_pub = Publisher(PodUrl(info_a.topic_name, std::string(kType)));
  auto* raw_pub = Publisher(PodUrl(info_a.topic_name, std::string(kRawType)));
  auto* meta_pub = Publisher(StdUrl(info_a.topic_name, kBlobType));
  ASSERT_TRUE(typed_pub && raw_pub && meta_pub);

  auto sample = typed_a->borrow_loaned_sample();
  ASSERT_TRUE(sample.is_valid());
  EXPECT_EQ(typed_pub->Outstanding(), 1u);
  EXPECT_FALSE(typed_b->publish(std::move(sample)));
  EXPECT_EQ(typed_pub->Outstanding(), 0u);

  auto bytes = raw_a->borrow_loaned_sample(16);
  ASSERT_TRUE(bytes.is_valid());
  EXPECT_EQ(raw_pub->Outstanding(), 1u);
  EXPECT_FALSE(raw_b->publish(std::move(bytes)));
  EXPECT_EQ(raw_pub->Outstanding(), 0u);

  auto buf = meta_a->borrow_loaned_buffer(*PubContext(), 16);
  ASSERT_TRUE(buf.is_valid());
  EXPECT_EQ(meta_pub->Outstanding(), 1u);
  EXPECT_FALSE(meta_b->publish(std::move(buf)));
  EXPECT_EQ(meta_pub->Outstanding(), 0u);

  EXPECT_EQ(typed_pub->Stats().publishes + raw_pub->Stats().publishes + meta_pub->Stats().publishes, 0u);
  for (const auto& url : {PodUrl(info_b.topic_name, std::string(kType)),
                          PodUrl(info_b.topic_name, std::string(kRawType)), StdUrl(info_b.topic_name, kBlobType)}) {
    auto* pub = Publisher(url);
    ASSERT_NE(pub, nullptr) << url;
    EXPECT_EQ(pub->Stats().publishes, 0u) << url;
    EXPECT_EQ(pub->Outstanding(), 0u) << url;
  }
  b.Shutdown();  // shuts the shared bus down; `a` finds it done
}

}  // namespace
}  // namespace ibmw::extensions::shm_extension
