// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Tests of the shm transport extension (ShmBus + ShmTransportDriver), the
// replacement of iceoryx_ext. Numbered EXT-xx in the test document. No daemon
// is needed; every run uses a private discovery root.

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "base/buffer_util.h"
#include "base/url_encode.h"
#include "cpp/util/buffer_array_allocator.h"
#include "shm_ext/shm_bus.h"
#include "shm_ext/shm_transport_driver.h"
#include "yaml-cpp/yaml.h"

#ifndef IBMW_SHM_EXT_PEER_PATH
#error "IBMW_SHM_EXT_PEER_PATH must point at the ibmw_shm_ext_peer binary"
#endif

extern char** environ;

namespace ibmw::extensions::shm_extension {
namespace {

using namespace std::chrono_literals;
namespace transport = runtime::core::transport;

// ------------------------------------------------------------------ helpers

std::string MakeRoot() {
  const char* base = std::getenv("TMPDIR");
  std::string tmpl = std::string(base != nullptr && *base != '\0' ? base : "/tmp") + "/ibmw_shm_ext_test.XXXXXX";
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
    std::vector<std::string> all{IBMW_SHM_EXT_PEER_PATH};
    all.insert(all.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : all) argv.push_back(a.data());
    argv.push_back(nullptr);
    const int rc = ::posix_spawn(&pid, IBMW_SHM_EXT_PEER_PATH, &actions, nullptr, argv.data(), environ);
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

// EXT-05 The driver name is configurable; the slot follows it so "iceoryx"
// is a drop-in replacement, and received contexts carry the name.
TEST_F(ShmDriverTest, DriverNameAliasIsDropIn) {
  auto& d = Init(YAML::Node(), {}, "iceoryx");
  EXPECT_EQ(d.Name(), "iceoryx");
  EXPECT_EQ(d.Slot(), ::ibmw::DriverSlotFromString("iceoryx"));
  EXPECT_NE(d.Slot(), ::ibmw::DriverSlotFromString(ShmTransportDriver::kStableDriverId));

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
  std::jthread producer, closer;
  StopGuard stop{this, [&] { release_handler = true; }};
  d.Start();
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(StdUrl(info.topic_name, kIntType)); }));
  producer = std::jthread([&] {
    for (int32_t v = 0; v < 400; ++v) {
      SendInt(info, v);
      sent.fetch_add(1);
    }
  });
  // 1 in the handler + 256 queued; the producer then blocks.
  ASSERT_TRUE(WaitFor([&] { return sent.load() >= 257; }));
  std::this_thread::sleep_for(50ms);
  EXPECT_LT(sent.load(), 400);
  closer = std::jthread([&] {
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
// another driver, cannot drive the loan count below the live loans.
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

  transport::LoanedSampleHandle p;
  ASSERT_TRUE(d.LoanSample(info, sizeof(IntMsg), transport::LoanMode::kPodOnly, nullptr, p));
  auto p_copy = p;
  d.ReleaseLoaned(p);
  d.ReleaseLoaned(p_copy);
  d.PublishLoaned(p_copy);
  EXPECT_EQ(pod->Outstanding(), 0u);
  EXPECT_EQ(pod->Stats().publishes, 0u);
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
  std::jthread producer;
  StopGuard stop{this};
  d.Start();
  Peer peer;
  ASSERT_TRUE(peer.Spawn({"stall", url}));
  ASSERT_EQ(peer.ReadLine(), "ready");
  ASSERT_TRUE(WaitFor([&] { return HasSubscribers(url); }));
  producer = std::jthread([&] {
    for (int32_t v = 0; v < 64; ++v) {
      SendInt(info, v);
      sent.fetch_add(1);
    }
  });
  // The peer's queue holds 4: the producer is now blocked for good.
  ASSERT_TRUE(WaitFor([&] { return Publisher(url)->Stats().blocked >= 1; }));
  EXPECT_LT(sent.load(), 64);
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_TRUE(d.TryPrepareShutdown(2000ms));
  EXPECT_LT(std::chrono::steady_clock::now() - t0, 2000ms);
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
