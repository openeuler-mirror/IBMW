// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Reporter V1 host boundary of the shm transport, the counterpart of
// iceoryx_ext/reporter_host_boundary_test.cc. This executable is the host: it
// exports the reporter query and has the plugin sources compiled in, so a
// ShmTransportDriver reports into the real core registry. Every run uses a
// private shm discovery root.

#include <dirent.h>
#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "yaml-cpp/yaml.h"

#include <gtest/gtest.h>

#include "base/url_encode.h"
#include "extension_api/ibmw_reporter_v1.h"
#include "ibmw_selfdev/shm/runtime.h"
#include "shm_ext/shm_bus.h"
#include "shm_ext/shm_transport_driver.h"
#include "test_util.h"

#ifdef IBMW_ENABLE_CORE_METRICS
#include "observability/core_metrics.h"
#endif

namespace ibmw::extensions::shm_extension {
namespace {

using namespace std::chrono_literals;
namespace transport = runtime::core::transport;
using Query = const ibmw_reporter_v1* (*)(void);

// The engine loads plugins with RTLD_DEEPBIND. ASan/TSan abort on it
// (google/sanitizers#611), so sanitizer builds load the plugin without it.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || !defined(RTLD_DEEPBIND)
constexpr int kDeepbind = 0;
#else
constexpr int kDeepbind = RTLD_DEEPBIND;
#endif

std::string MakeRoot() {
  std::string buf = (std::filesystem::temp_directory_path() / "ibmwr.XXXXXX").string();
  if (::mkdtemp(buf.data()) == nullptr) std::abort();
  return buf;
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

class ShmRootEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    root_ = MakeRoot();
    ::setenv("IBMW_SHM_ROOT", root_.c_str(), 1);
    ::unsetenv("IBMW_SHM_DOMAIN");
  }
  // Every fixture has destroyed its shm objects by now: stop the control
  // thread first, otherwise it re-creates the removed service directory.
  void TearDown() override {
    ibmw::selfdev::shm::Runtime::Shutdown();
    for (int i = 0; i < 100 && ::access(root_.c_str(), F_OK) == 0; ++i) {
      RemoveTree(root_);
      if (::access(root_.c_str(), F_OK) == 0) std::this_thread::sleep_for(10ms);
    }
  }

 private:
  std::string root_;
};

const auto* const g_env = ::testing::AddGlobalTestEnvironment(new ShmRootEnvironment());

bool WaitFor(const std::function<bool()>& pred, std::chrono::milliseconds timeout = 5000ms) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < end) {
    if (pred()) return true;
    std::this_thread::sleep_for(1ms);
  }
  return pred();
}

// ------------------------------------------------------- test message type

constexpr char kSerType[] = "test";
constexpr char kBlobType[] = "shm_reporter_test/Blob";

struct BlobMsg {
  std::string data;
};

const ibmw_string_view_t* SerTypes(void*) {
  static const ibmw_string_view_t kTypes[] = {{kSerType, sizeof(kSerType) - 1}};
  return kTypes;
}
size_t SerTypesNum(void*) { return 1; }
const void* NoCustom(void*) { return nullptr; }
ibmw_string_view_t BlobName(void*) { return {kBlobType, sizeof(kBlobType) - 1}; }
void* BlobCreate(void*) { return new BlobMsg(); }
void BlobDestroy(void*, void* m) { delete static_cast<BlobMsg*>(m); }
void BlobCopy(void*, const void* from, void* to) { *static_cast<BlobMsg*>(to) = *static_cast<const BlobMsg*>(from); }
void BlobMove(void*, void* from, void* to) { *static_cast<BlobMsg*>(to) = std::move(*static_cast<BlobMsg*>(from)); }
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

std::string UniqueTopic(const std::string& suffix = "") {
  const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string name = info->name();
  for (auto& c : name) {
    if (c == '/') c = '_';
  }
  return "shm_reporter_" + name + "_" + std::to_string(::getpid()) + suffix;
}

transport::TransportInfo BlobInfo(const std::string& topic) {
  transport::TransportInfo info;
  info.msg_type = kBlobType;
  info.topic_name = topic;
  info.msg_type_support_ref = ibmw::util::TypeSupportRef(BlobTypeSupport());
  return info;
}

transport::SendTypeWrapper SendType(const transport::TransportInfo& info) {
  transport::SendTypeWrapper w;
  w.info = info;
  return w;
}

std::shared_ptr<ibmw::transport::Context> PubContext() {
  auto ctx = std::make_shared<ibmw::transport::Context>(
      ibmw_transport_context_type_t::IBMW_TRANSPORT_PUBLISHER_CONTEXT);
  ctx->SetSerializationType(kSerType);
  return ctx;
}

// ----------------------------------------------------------------- fixture

class ShmReporterHostBoundary : public ::testing::Test {
 protected:
  void TearDown() override {
    retained_.reset();
    if (driver_) driver_->Shutdown();
    driver_.reset();
    bus_.reset();
    unsetenv("IBMW_CORE_METRICS");
    unsetenv("IBMW_OBSERVABILITY_OUTPUT_DIR");
#ifdef IBMW_ENABLE_CORE_METRICS
    ibmw::runtime::core::observability::Controller::Instance().ResetForTesting();
#endif
  }

  ShmTransportDriver& Init(const std::string& name) {
    bus_ = std::make_unique<ShmBus>();
    bus_->Initialize({});
    driver_ = std::make_unique<ShmTransportDriver>(*bus_, name);
    driver_->Initialize(YAML::Node());
    return *driver_;
  }

  ShmPublisher* Publisher(const std::string& url) {
    auto lease = bus_->TryAcquireOperation();
    return bus_->GetPublisher(lease, url);
  }

  std::unique_ptr<ShmBus> bus_;
  std::unique_ptr<ShmTransportDriver> driver_;
  std::unique_ptr<transport::LoanedSubscriptionHandle> retained_;
  std::vector<transport::MessageHandlerWrapper> handlers_;
};

TEST_F(ShmReporterHostBoundary, DeepbindShmResolvesTheHostOwner) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
#ifdef IBMW_ENABLE_CORE_METRICS
  ibmw::runtime::core::observability::Controller::Instance().Initialize();
#endif
  void* plugin = dlopen(SHM_EXTENSION_PATH, RTLD_NOW | RTLD_LOCAL | kDeepbind);
  ASSERT_NE(plugin, nullptr) << dlerror();
  auto plugin_query = reinterpret_cast<Query>(dlsym(plugin, "ibmw_shm_query_host_reporter_v1"));
  ASSERT_NE(plugin_query, nullptr) << dlerror();
  Dl_info plugin_owner{};
  ASSERT_NE(dladdr(reinterpret_cast<void*>(plugin_query), &plugin_owner), 0);
  ASSERT_NE(plugin_owner.dli_fname, nullptr);
  EXPECT_NE(std::strstr(plugin_owner.dli_fname, "ibmw_shm_transport"), nullptr);
  void* host = dlopen(nullptr, RTLD_NOW | RTLD_LOCAL);
  ASSERT_NE(host, nullptr);
  auto host_query = reinterpret_cast<Query>(dlsym(host, "ibmw_reporter_query_v1"));
  ASSERT_NE(host_query, nullptr);

  Dl_info owner{};
  ASSERT_NE(dladdr(reinterpret_cast<void*>(host_query), &owner), 0);
  ASSERT_NE(owner.dli_fname, nullptr);
  EXPECT_EQ(std::strstr(owner.dli_fname, "ibmw_shm_transport"), nullptr);
#ifdef IBMW_ENABLE_REPORTER_V1
  const auto* api = plugin_query();
  ASSERT_EQ(api, host_query());
  ASSERT_NE(api, nullptr);
  ibmw_reporter_slot_descriptor_v1 descriptor{};
  descriptor.abi_version = IBMW_REPORTER_ABI_VERSION_V1;
  descriptor.struct_size = sizeof(descriptor);
  descriptor.kind = {"fixture", 7};
  descriptor.metric = {"count", 5};
  auto* slot = api->register_slot(api->owner, &descriptor);
  ASSERT_NE(slot, nullptr);
  api->add(api->owner, slot, 1);
#else
  EXPECT_EQ(plugin_query(), nullptr);
  EXPECT_EQ(host_query(), nullptr);
#endif
  dlclose(host);
  dlclose(plugin);
}

// Core metrics off at run time (or the build switches off): no host reporter,
// so the driver reports nothing and hands out its unreported ctx-meta mode.
TEST_F(ShmReporterHostBoundary, RuntimeOffIsSafeNullAndLoansStayUnreported) {
#ifdef IBMW_ENABLE_CORE_METRICS
  ibmw::runtime::core::observability::Controller::Instance().Initialize();
#endif
  EXPECT_EQ(ibmw_reporter_query_v1(), nullptr);
  EXPECT_EQ(ibmw_reporter_query_batch_v1(), nullptr);

  auto& d = Init("shm");
  const auto info = BlobInfo(UniqueTopic());
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  d.Start();
  auto ctx = PubContext();
  transport::LoanedSampleHandle h;
  ASSERT_TRUE(d.LoanSample(info, 16, transport::LoanMode::kCtxMetaThenPayload, ctx.get(), h));
  EXPECT_EQ(h.mode, transport::LoanMode::kCtxMetaThenPayloadUnreported);
  EXPECT_TRUE(d.TryPublishLoaned(h));
  BlobMsg msg{"x"};
  ASSERT_TRUE(d.LoanSerializedSample(info, *ctx, &msg, h));
  EXPECT_EQ(h.mode, transport::LoanMode::kCtxMetaThenPayloadUnreported);
  d.ReleaseLoaned(h);
  EXPECT_EQ(h.driver_token, nullptr);
}

#if defined(IBMW_ENABLE_CORE_METRICS) && defined(IBMW_ENABLE_REPORTER_V1)

// Value of one reporter record, nullopt when there is no such record.
std::optional<uint64_t> Value(const std::string& snapshot, std::string_view kind, std::string_view metric,
                              const std::string& topic, std::string_view type, std::string_view driver) {
  const std::string record = "\"kind\":\"" + std::string(kind) + "\",\"metric\":\"" + std::string(metric) +
                             "\",\"topic\":\"" + topic + "\",\"type\":\"" + std::string(type) +
                             "\",\"driver\":\"" + std::string(driver) + "\",\"value\":";
  const auto pos = snapshot.find(record);
  if (pos == std::string::npos) return std::nullopt;
  return std::stoull(snapshot.substr(pos + record.size()));
}

size_t Records(const std::string& snapshot, std::string_view kind, const std::string& topic) {
  const std::string needle = "\"kind\":\"" + std::string(kind) + "\",";
  const std::string topic_field = "\"topic\":\"" + topic + "\"";
  size_t count = 0;
  for (size_t pos = 0; (pos = snapshot.find(needle, pos)) != std::string::npos; ++pos) {
    const auto end = snapshot.find('\n', pos);
    if (snapshot.substr(pos, end - pos).find(topic_field) != std::string::npos) ++count;
  }
  return count;
}

std::string Snapshot() {
  return ibmw::runtime::core::observability::Controller::Instance().registry().ReporterSnapshotJsonl();
}

class ShmReporterDriverName : public ShmReporterHostBoundary, public ::testing::WithParamInterface<const char*> {
 protected:
  void SetUp() override {
    ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
    ibmw::runtime::core::observability::Controller::Instance().Initialize();
    ASSERT_NE(ibmw_reporter_query_v1(), nullptr);
  }
};

// direct_shm_loan follows the ctx-meta loan path of a real driver: attempt /
// success / fail per loan call, growth retries of LoanSerializedSample, and
// publish / release per handle that owned a chunk (a refused publish is a
// release, a stale handle nothing). Framed Send() and POD loans add nothing.
TEST_P(ShmReporterDriverName, DirectLoanSlotsFollowTheCtxMetaLoanPath) {
  const std::string driver_name = GetParam();
  auto& d = Init(driver_name);
  const auto info = BlobInfo(UniqueTopic());
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));
  ASSERT_TRUE(d.RegisterSendType(SendType(info)));  // existing publisher: one family
  ASSERT_TRUE(d.EnsurePodTopic(info.topic_name, kBlobType, 8, 8));
  std::atomic<int> received{0};
  transport::MessageHandlerWrapper handler;
  handler.info = info;
  handler.callback = [&received](transport::MessageFrame&, std::function<void()>&& release, const auto*) {
    received.fetch_add(1);
    release();
  };
  handlers_.reserve(1);
  handlers_.push_back(std::move(handler));
  ASSERT_TRUE(d.OnMessage(handlers_.back()));
  d.Start();
  const std::string url = "/transport/" + common::util::UrlEncode(info.topic_name) + "/" + common::util::UrlEncode(kBlobType);
  ASSERT_TRUE(WaitFor([&] {
    auto* pub = Publisher(url);
    return pub != nullptr && pub->HasSubscribers();
  }));
  const auto value = [&](std::string_view metric) {
    return Value(Snapshot(), "direct_shm_loan", metric, info.topic_name, kBlobType, driver_name);
  };
  EXPECT_EQ(Records(Snapshot(), "direct_shm_loan", info.topic_name), 6u);
  EXPECT_EQ(value("logical_attempt"), 0u);

  auto ctx = PubContext();
  // Published.
  transport::LoanedSampleHandle h;
  ASSERT_TRUE(d.LoanSample(info, 16, transport::LoanMode::kCtxMetaThenPayload, ctx.get(), h));
  EXPECT_EQ(h.mode, transport::LoanMode::kCtxMetaThenPayload);
  const auto stale = h;
  EXPECT_TRUE(d.TryPublishLoaned(h));
  // A stale copy owns no chunk.
  auto stale_copy = stale;
  EXPECT_FALSE(d.TryPublishLoaned(stale_copy));
  stale_copy = stale;
  d.ReleaseLoaned(stale_copy);
  // Given back.
  ASSERT_TRUE(d.LoanSample(info, 16, transport::LoanMode::kCtxMetaThenPayload, ctx.get(), h));
  d.ReleaseLoaned(h);
  // Failed: larger than the largest shm payload.
  EXPECT_FALSE(d.LoanSample(info, size_t{1} << 30, transport::LoanMode::kCtxMetaThenPayload, ctx.get(), h));
  // Serialized with growth: 4 KB first guess, 20 KB message -> 8, 16, 32 KB.
  BlobMsg big{std::string(20 * 1024, 'b')};
  ASSERT_TRUE(d.LoanSerializedSample(info, *ctx, &big, h));
  EXPECT_EQ(h.mode, transport::LoanMode::kCtxMetaThenPayload);
  EXPECT_TRUE(d.TryPublishLoaned(h));
  // Refused by the library (out of memory with a subscriber connected).
  ASSERT_TRUE(d.LoanSample(info, 16, transport::LoanMode::kCtxMetaThenPayload, ctx.get(), h));
  ibmw::selfdev::shm::test::FailAllocations(1);
  const bool refused_published = d.TryPublishLoaned(h);
  ibmw::selfdev::shm::test::FailAllocations(0);
  EXPECT_FALSE(refused_published);
  // Not counted: framed Send() and POD loans.
  BlobMsg small{"s"};
  transport::MessageFrame frame{.info = info, .msg_ptr = &small, .ctx_ref = ctx};
  d.Send(frame);
  ASSERT_TRUE(d.LoanSample(info, 8, transport::LoanMode::kPodOnly, nullptr, h));
  EXPECT_TRUE(d.TryPublishLoaned(h));

  EXPECT_EQ(value("logical_attempt"), 5u);
  EXPECT_EQ(value("logical_success"), 4u);
  EXPECT_EQ(value("logical_fail"), 1u);
  EXPECT_EQ(value("internal_retry"), 3u);
  EXPECT_EQ(value("publish"), 2u);
  EXPECT_EQ(value("release"), 2u);
  EXPECT_EQ(*value("logical_attempt"), *value("logical_success") + *value("logical_fail"));
  EXPECT_EQ(*value("logical_success"), *value("publish") + *value("release"));
  EXPECT_EQ(Records(Snapshot(), "direct_shm_loan", info.topic_name), 6u);
  EXPECT_EQ(Records(Snapshot(), "direct_iceoryx_loan", info.topic_name), 0u);
}

// retained_shm_receive follows the retained lease path: delivered / released
// per lease, callback outcomes, outstanding and its peak, and drops at the
// max_outstanding limit.
TEST_P(ShmReporterDriverName, RetainedReceiveSlotsFollowTheLeasePath) {
  const std::string driver_name = GetParam();
  auto& d = Init(driver_name);
  const auto info = BlobInfo(UniqueTopic());
  ASSERT_TRUE(d.EnsurePodTopic(info.topic_name, kBlobType, 8, 8));
  std::mutex mu;
  std::vector<std::function<void()>> kept;
  std::atomic<int> calls{0};
  transport::LoanedMessageHandlerWrapper wrapper;
  wrapper.info = info;
  wrapper.callback = [&](const ibmw_transport_context_base_t*, const void* p, size_t,
                         std::function<void()>&& release) {
    uint64_t v;
    std::memcpy(&v, p, sizeof(v));
    calls.fetch_add(1);
    if (v == 2) throw std::runtime_error("std failure");
    if (v == 3) throw 42;
    std::lock_guard<std::mutex> lock(mu);
    kept.push_back(std::move(release));
  };
  retained_ = d.AttachLoanedRetained(wrapper, {.max_outstanding = 2});
  ASSERT_NE(retained_, nullptr);
  d.Start();
  const std::string url = "/transport_zc_pod/" + common::util::UrlEncode(info.topic_name) + "/" + common::util::UrlEncode(kBlobType);
  ASSERT_TRUE(WaitFor([&] {
    auto* pub = Publisher(url);
    return pub != nullptr && pub->HasSubscribers();
  }));
  const auto value = [&](std::string_view metric) {
    return Value(Snapshot(), "retained_shm_receive", metric, info.topic_name, kBlobType, driver_name);
  };
  EXPECT_EQ(Records(Snapshot(), "retained_shm_receive", info.topic_name), 9u);
  const auto publish = [&](uint64_t v) {
    transport::LoanedSampleHandle h;
    ASSERT_TRUE(d.LoanSample(info, sizeof(v), transport::LoanMode::kPodOnly, nullptr, h));
    std::memcpy(h.user_payload_ptr, &v, sizeof(v));
    ASSERT_TRUE(d.TryPublishLoaned(h));
  };
  // 1 kept, 2 throws std, 3 throws non-std, 4 kept, 5 over the limit.
  for (uint64_t v = 1; v <= 4; ++v) {
    publish(v);
    ASSERT_TRUE(WaitFor([&] { return calls.load() == static_cast<int>(v); })) << v;
    // Released in Deliver after the callback threw, before the next sample.
    ASSERT_TRUE(WaitFor([&] { return value("receive_released") == (v >= 3 ? 2u : v == 2 ? 1u : 0u); })) << v;
  }
  EXPECT_EQ(value("outstanding_current"), 2u);
  publish(5);
  ASSERT_TRUE(WaitFor([&] { return value("limit_drop") == 1u; }));
  EXPECT_EQ(calls.load(), 4);
  {
    std::lock_guard<std::mutex> lock(mu);
    for (auto& r : kept) r();
    kept.clear();
  }

  EXPECT_EQ(value("receive_delivered"), 4u);
  EXPECT_EQ(value("callback_entered"), 4u);
  EXPECT_EQ(value("callback_completed"), 2u);
  EXPECT_EQ(value("callback_threw_std"), 1u);
  EXPECT_EQ(value("callback_threw_nonstd"), 1u);
  EXPECT_EQ(value("receive_released"), 4u);
  EXPECT_EQ(value("outstanding_current"), 0u);
  EXPECT_EQ(value("outstanding_peak"), 2u);
  EXPECT_EQ(value("limit_drop"), 1u);
  EXPECT_EQ(*value("callback_entered"),
            *value("callback_completed") + *value("callback_threw_std") + *value("callback_threw_nonstd"));
  EXPECT_EQ(Records(Snapshot(), "retained_iceoryx_receive", info.topic_name), 0u);
  EXPECT_TRUE(retained_->Close(1s));
}

INSTANTIATE_TEST_SUITE_P(DriverLabel, ShmReporterDriverName, ::testing::Values("shm", "iceoryx"));

// Slots that cannot be committed (registry frozen) fail the registration and
// leave nothing behind: no publisher, the subscriber URL free again.
TEST_F(ShmReporterHostBoundary, FrozenRegistryFailsRegistrationClosed) {
  ASSERT_EQ(setenv("IBMW_CORE_METRICS", "counters", 1), 0);
  auto& controller = ibmw::runtime::core::observability::Controller::Instance();
  controller.Initialize();
  auto& d = Init("shm");
  controller.registry().Freeze();
  const auto info = BlobInfo(UniqueTopic());
  EXPECT_FALSE(d.RegisterSendType(SendType(info)));
  EXPECT_EQ(bus_->PublisherCount(), 0u);

  ASSERT_TRUE(d.EnsurePodTopic(info.topic_name, kBlobType, 8, 8));
  transport::LoanedMessageHandlerWrapper wrapper;
  wrapper.info = info;
  wrapper.callback = [](const ibmw_transport_context_base_t*, const void*, size_t, std::function<void()>&& release) {
    release();
  };
  EXPECT_EQ(d.AttachLoanedRetained(wrapper, {.max_outstanding = 1}), nullptr);
  EXPECT_EQ(bus_->SubscriptionCount(), 0u);
  const std::string url = "/transport_zc_pod/" + common::util::UrlEncode(info.topic_name) + "/" + common::util::UrlEncode(kBlobType);
  EXPECT_TRUE(bus_->ReserveSubscriberUrl(url));
  bus_->RollbackSubscriberUrl(url);
  EXPECT_EQ(Records(Snapshot(), "direct_shm_loan", info.topic_name), 0u);
  EXPECT_EQ(Records(Snapshot(), "retained_shm_receive", info.topic_name), 0u);
}

#endif

}  // namespace
}  // namespace ibmw::extensions::shm_extension
