// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// TRANS-045: shm loans that turn out too small go back to the pool, loan sizes
// are bounded by the pool, and a message the pool cannot hold is sent over the
// network. One isolated session (no scouting, loopback only) carries both sides.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cpp/util/string.h"
#include "zenoh_ext/zenoh_transport_driver.h"

namespace ibmw::extensions::zenoh_extension {
namespace {

using namespace std::chrono_literals;

// "raw" std::string: its bytes.
const ibmw_type_support_base_t* BlobTypeSupport() {
  static const ibmw_string_view_t kTypes[] = {{.str = "raw", .len = 3}};
  static const ibmw_type_support_base_t ts{
      .type_name = [](void*) { return ibmw::util::ToIbmwStringView("test:Blob"); },
      .create = [](void*) -> void* { return new std::string(); },
      .destroy = [](void*, void* msg) { delete static_cast<std::string*>(msg); },
      .copy = [](void*, const void* from, void* to) {
        *static_cast<std::string*>(to) = *static_cast<const std::string*>(from);
      },
      .move = [](void*, void* from, void* to) {
        *static_cast<std::string*>(to) = std::move(*static_cast<std::string*>(from));
      },
      .serialize = [](void*, ibmw_string_view_t, const void* msg, const ibmw_buffer_array_allocator_t* allocator,
                      ibmw_buffer_array_t* buffer_array) {
        const auto& text = *static_cast<const std::string*>(msg);
        auto buf = allocator->allocate(allocator->ctx, buffer_array, text.size());
        if (buf.data == nullptr) return false;
        memcpy(buf.data, text.data(), text.size());
        return true;
      },
      .deserialize = [](void*, ibmw_string_view_t, ibmw_buffer_array_view_t, void*) { return false; },
      .serialization_types_supported_num = [](void*) -> size_t { return 1; },
      .serialization_types_supported_list = [](void*) -> const ibmw_string_view_t* { return kTypes; },
      .custom_type_support_ptr = [](void*) -> const void* { return nullptr; },
      .ctx = nullptr};
  return &ts;
}

constexpr size_t kPool = 1024 * 1024;

class ZenohTransportDriverTest : public ::testing::Test {
 protected:
  struct Sample {
    size_t size;
    bool shm;
    uint32_t frame_len;  // the length prefix
    char last;           // last byte of the frame
    std::string tail;    // last bytes of the frame, up to 4 KiB
  };

  void SetUp() override {
    char dir[] = "/tmp/ibmw_zenoh_transport_test_XXXXXX";
    ASSERT_NE(::mkdtemp(dir), nullptr);
    directory_ = dir;
    std::ofstream(directory_ / "peer.json5")
        << R"({"mode":"peer","scouting":{"multicast":{"enabled":false},"gossip":{"enabled":false}},)"
        << R"("listen":{"endpoints":["tcp/127.0.0.1:0"]}})";
    bus_ = std::make_shared<ZenohBus>();
    bus_->Initialize((directory_ / "peer.json5").string(), kPool);

    driver_ = std::make_unique<ZenohTransportDriver>(bus_, "", 1024);
    driver_->Initialize(YAML::Load("{publishers: [{topic: /blob, shm_enabled: true}]}"));
    wrapper_.info.topic_name = "/blob";
    wrapper_.info.msg_type = "test:Blob";
    wrapper_.info.msg_type_support_ref = ibmw::util::TypeSupportRef(BlobTypeSupport());
    ASSERT_TRUE(driver_->RegisterSendType(wrapper_));
    driver_->Start();

    z_owned_subscriber_t sub;
    ASSERT_TRUE(bus_->CreateSubscriber("transport/%2Fblob/test%3ABlob", [this](const z_loaned_sample_t* sample) {
      const auto* payload = z_sample_payload(sample);
      const z_loaned_shm_t* shm = nullptr;
      z_owned_slice_t slice;
      z_bytes_to_slice(payload, &slice);
      const auto* data = reinterpret_cast<const char*>(z_slice_data(z_loan(slice)));
      const size_t len = z_slice_len(z_loan(slice));
      const uint32_t frame_len = len < 4 ? 0 : common::util::GetUint32FromBuf(data);
      const bool whole = frame_len != 0 && 4 + size_t{frame_len} <= len;
      const char last = whole ? data[4 + frame_len - 1] : '\0';
      const size_t tail = whole ? std::min<size_t>(frame_len, 4096) : 0;
      std::lock_guard lock(mutex_);
      seen_.push_back({len, z_bytes_to_loaned_shm(payload, &shm) == Z_OK, frame_len, last,
                       whole ? std::string(data + 4 + frame_len - tail, tail) : std::string()});
      z_drop(z_move(slice));
    }, sub));
    subs_.push_back(sub);
  }

  // Subscriptions go first: no callback runs into a destroyed driver.
  void TearDown() override {
    for (auto& sub : subs_) z_drop(z_move(sub));
    if (bus_) bus_->Shutdown();
    if (driver_) driver_->Shutdown();
    driver_.reset();
    std::filesystem::remove_all(directory_);
  }

  // Sends a message of `size` bytes (already serialized in `cached` buffers
  // if not 0, buffer i filled with 'a' + i) and returns how it arrived. The
  // message ends with 'z'.
  Sample SendBlob(size_t size, size_t cached = 0) {
    std::string blob(size, 'x');
    ibmw::transport::Context ctx;
    runtime::core::transport::MessageFrame frame{.info = wrapper_.info, .msg_ptr = &blob, .ctx_ref = ctx};
    if (cached) {
      std::vector<ibmw_buffer_view_t> views;
      for (size_t i = 0; i < cached; ++i) {
        const size_t begin = size * i / cached, end = size * (i + 1) / cached;
        if (cached > 1) std::fill(blob.begin() + begin, blob.begin() + end, static_cast<char>('a' + i));
        views.push_back({.data = blob.data() + begin, .len = end - begin});
      }
      frame.serialization_cache["raw"] = std::make_shared<ibmw::util::BufferArrayView>(views);
    }
    blob.back() = 'z';
    size_t before;
    {
      std::lock_guard lock(mutex_);
      before = seen_.size();
    }
    driver_->Send(frame);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard lock(mutex_);
        if (seen_.size() > before) return seen_[before];
      }
      std::this_thread::sleep_for(1ms);
    }
    return {0, false, 0, '\0', {}};
  }

  // The frame carries a message of `size` bytes (SendBlob's) after a short header.
  static void ExpectFrame(const Sample& sample, size_t size) {
    EXPECT_GT(sample.frame_len, size);
    EXPECT_LT(sample.frame_len, size + 256);
    EXPECT_EQ(sample.last, 'z');
  }

  std::filesystem::path directory_;
  std::shared_ptr<ZenohBus> bus_;
  std::unique_ptr<ZenohTransportDriver> driver_;
  runtime::core::transport::SendTypeWrapper wrapper_;
  std::vector<z_owned_subscriber_t> subs_;
  std::mutex mutex_;
  std::vector<Sample> seen_;
};

// Growing the 1 KiB loan to fit 600 KiB returns each smaller loan to the
// pool, so the message (and the next ones) still go through shm.
TEST_F(ZenohTransportDriverTest, LoansThatWereTooSmallGoBackToThePool) {
  for (int round = 0; round < 3; ++round) {
    const auto sample = SendBlob(600 * 1024);
    ExpectFrame(sample, 600 * 1024);
    EXPECT_TRUE(sample.shm) << round;
  }
}

// An already serialized message gets a loan of its size once, and the send
// completes (a resized loan used to be retried without ever fitting it).
TEST_F(ZenohTransportDriverTest, CachedMessageGetsALoanOfItsSize) {
  for (int round = 0; round < 3; ++round) {
    const auto sample = SendBlob(600 * 1024, 1);
    ExpectFrame(sample, 600 * 1024);
    EXPECT_TRUE(sample.shm) << round;
  }
}

// A message larger than the pool goes over the network; the loan size stays
// within the pool, so later messages still use shm.
TEST_F(ZenohTransportDriverTest, MessageLargerThanThePoolIsSentOverTheNetwork) {
  const auto big = SendBlob(2 * kPool);
  ExpectFrame(big, 2 * kPool);
  EXPECT_FALSE(big.shm);
  EXPECT_EQ(z_shm_provider_available(z_loan(bus_->shm_provider_)), kPool);  // the loan was collected
  const auto next = SendBlob(100);
  ExpectFrame(next, 100);
  EXPECT_TRUE(next.shm);
}

// The same for an already serialized message.
TEST_F(ZenohTransportDriverTest, CachedMessageLargerThanThePoolIsSentOverTheNetwork) {
  const auto big = SendBlob(2 * kPool, 1);
  ExpectFrame(big, 2 * kPool);
  EXPECT_FALSE(big.shm);
  const auto next = SendBlob(100);
  ExpectFrame(next, 100);
  EXPECT_TRUE(next.shm);
}

// A message serialized into several buffers is copied buffer by buffer.
TEST_F(ZenohTransportDriverTest, CachedMessageInSeveralBuffersIsCopiedWhole) {
  const auto sample = SendBlob(600, 3);
  ExpectFrame(sample, 600);
  EXPECT_TRUE(sample.shm);
  ASSERT_GE(sample.tail.size(), 600u);
  EXPECT_EQ(sample.tail.substr(sample.tail.size() - 600),
            std::string(200, 'a') + std::string(200, 'b') + std::string(199, 'c') + 'z');
}

// A loan that has to grow after an earlier shm send finds the pool split by
// that send and by the dropped smaller loan: it is collected and compacted,
// so the message still goes through shm.
TEST_F(ZenohTransportDriverTest, LoanGrowsAfterEarlierSends) {
  for (size_t size : {200 * 1024, 300 * 1024, 300 * 1024, 100}) {
    const auto sample = SendBlob(size);
    ExpectFrame(sample, size);
    EXPECT_TRUE(sample.shm) << size;
  }
}

// Grow doubles a loan up to the pool, Need takes what fits the pool, and
// neither lowers a size.
TEST(ShmLoanSizesTest, GrowAndNeedStayWithinThePool) {
  ShmLoanSizes sizes;
  EXPECT_EQ(sizes.Get("k", 7), 7u);
  sizes.Set("k", 1000);
  EXPECT_TRUE(sizes.Grow("k", 1000, 4001));
  EXPECT_EQ(sizes.Get("k", 0), 2000u);
  EXPECT_TRUE(sizes.Grow("h", 2000, 4001));  // half: doubled
  EXPECT_EQ(sizes.Get("h", 0), 4000u);
  EXPECT_TRUE(sizes.Grow("k", 2001, 4001));  // over half: the whole pool
  EXPECT_EQ(sizes.Get("k", 0), 4001u);
  EXPECT_FALSE(sizes.Grow("k", 4001, 4001));
  EXPECT_EQ(sizes.Get("k", 0), 4001u);
  EXPECT_TRUE(sizes.Grow("k", 1000, 4001));  // a stale smaller loan
  EXPECT_EQ(sizes.Get("k", 0), 4001u);

  EXPECT_FALSE(sizes.Need("n", 4002, 4001));
  EXPECT_EQ(sizes.Get("n", 0), 0u);
  EXPECT_TRUE(sizes.Need("n", 4001, 4001));
  EXPECT_EQ(sizes.Get("n", 0), 4001u);
  EXPECT_TRUE(sizes.Grow("z", 0, 4001));
  EXPECT_EQ(sizes.Get("z", 0), 1u);
}

}  // namespace
}  // namespace ibmw::extensions::zenoh_extension
