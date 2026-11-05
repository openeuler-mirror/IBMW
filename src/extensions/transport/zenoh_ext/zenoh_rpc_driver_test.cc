// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// TRANS-005: a zenoh RPC server answers only on the answer key of the called
// function, and a client completes a call only with an answer on that key.
// TRANS-045: shm loans that turn out too small go back to the pool.
// TRANS-046: a function has one server, clients share its answer key; servers
// answer only after Start, which freezes the bus. Shutdown waits for the
// handlers and removes their subscribers.
// One isolated session (no scouting, loopback only) carries both sides.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cpp/util/string.h"
#include "zenoh_ext/zenoh_rpc_driver.h"

namespace ibmw::extensions::zenoh_extension {
namespace {

using namespace std::chrono_literals;

// "json" int: the decimal text of the value.
const ibmw_type_support_base_t* JsonIntTypeSupport() {
  static const ibmw_string_view_t kTypes[] = {{.str = "json", .len = 4}};
  static const ibmw_type_support_base_t ts{
      .type_name = [](void*) { return ibmw::util::ToIbmwStringView("test:Int"); },
      .create = [](void*) -> void* { return new int(0); },
      .destroy = [](void*, void* msg) { delete static_cast<int*>(msg); },
      .copy = [](void*, const void* from, void* to) { *static_cast<int*>(to) = *static_cast<const int*>(from); },
      .move = [](void*, void* from, void* to) { *static_cast<int*>(to) = *static_cast<int*>(from); },
      .serialize = [](void*, ibmw_string_view_t, const void* msg, const ibmw_buffer_array_allocator_t* allocator,
                      ibmw_buffer_array_t* buffer_array) {
        const std::string text = std::to_string(*static_cast<const int*>(msg));
        auto buf = allocator->allocate(allocator->ctx, buffer_array, text.size());
        if (buf.data == nullptr) return false;
        memcpy(buf.data, text.data(), text.size());
        return true;
      },
      .deserialize = [](void*, ibmw_string_view_t, ibmw_buffer_array_view_t buffers, void* msg) {
        std::string text;
        for (size_t i = 0; i < buffers.len; ++i)
          text.append(static_cast<const char*>(buffers.data[i].data), buffers.data[i].len);
        if (text.empty()) return false;
        *static_cast<int*>(msg) = std::stoi(text);
        return true;
      },
      .serialization_types_supported_num = [](void*) -> size_t { return 1; },
      .serialization_types_supported_list = [](void*) -> const ibmw_string_view_t* { return kTypes; },
      .custom_type_support_ptr = [](void*) -> const void* { return nullptr; },
      .ctx = nullptr};
  return &ts;
}

// "json" std::string: its bytes.
const ibmw_type_support_base_t* JsonBlobTypeSupport() {
  static const ibmw_string_view_t kTypes[] = {{.str = "json", .len = 4}};
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
      .deserialize = [](void*, ibmw_string_view_t, ibmw_buffer_array_view_t buffers, void* msg) {
        auto& text = *static_cast<std::string*>(msg);
        text.clear();
        for (size_t i = 0; i < buffers.len; ++i)
          text.append(static_cast<const char*>(buffers.data[i].data), buffers.data[i].len);
        return true;
      },
      .serialization_types_supported_num = [](void*) -> size_t { return 1; },
      .serialization_types_supported_list = [](void*) -> const ibmw_string_view_t* { return kTypes; },
      .custom_type_support_ptr = [](void*) -> const void* { return nullptr; },
      .ctx = nullptr};
  return &ts;
}

// Key of a function as both drivers build it (empty limit domain).
std::string Pattern(const std::string& func) { return "ibmw_rpc/" + common::util::UrlEncode(func); }

// [u32 len][u8 "json"][...fields][body], the framing of both directions.
std::vector<char> Frame(const std::string& fields, const std::string& body) {
  const std::string data = std::string(1, '\4') + "json" + fields + body;
  std::vector<char> out(4 + data.size());
  common::util::BufferOperator buf_oper(out.data(), out.size());
  buf_oper.SetUint32(static_cast<uint32_t>(data.size()));
  buf_oper.SetBuffer(data.data(), data.size());
  return out;
}

class ZenohRpcDriverTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char dir[] = "/tmp/ibmw_zenoh_rpc_test_XXXXXX";
    ASSERT_NE(::mkdtemp(dir), nullptr);
    directory_ = dir;
    std::ofstream(directory_ / "peer.json5")
        << R"({"mode":"peer","scouting":{"multicast":{"enabled":false},"gossip":{"enabled":false}},)"
        << R"("listen":{"endpoints":["tcp/127.0.0.1:0"]}})";
    bus_ = std::make_shared<ZenohBus>();
    bus_->Initialize((directory_ / "peer.json5").string(), 1024 * 1024);
    for (const char* name : {"pb:/test/a", "pb:/test/b"}) {
      runtime::core::service::ServiceInfo info;
      info.func_name = name;
      info.pkg_path = "pkg";
      info.module_name = "mod";
      info.req_type_support_ref = ibmw::util::TypeSupportRef(JsonIntTypeSupport());
      info.rsp_type_support_ref = ibmw::util::TypeSupportRef(JsonIntTypeSupport());
      infos_.push_back(info);
    }
    ctx_.SetMetaValue(IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE, "json");
  }

  // Subscriptions go first: no callback runs into a destroyed driver.
  void TearDown() override {
    for (auto& sub : subs_) z_drop(z_move(sub));
    if (bus_) bus_->Shutdown();
    if (driver_) driver_->Shutdown();
    driver_.reset();
    std::filesystem::remove_all(directory_);
  }

  ZenohRpcDriver& NewDriver(const std::string& options = "") {
    driver_ = std::make_unique<ZenohRpcDriver>(bus_, "");
    driver_->Initialize(options.empty() ? YAML::Node() : YAML::Load(options));
    return *driver_;
  }

  // infos_[0] with std::string request and response.
  runtime::core::service::ServiceInfo BlobInfo() const {
    auto info = infos_[0];
    info.req_type_support_ref = ibmw::util::TypeSupportRef(JsonBlobTypeSupport());
    info.rsp_type_support_ref = ibmw::util::TypeSupportRef(JsonBlobTypeSupport());
    return info;
  }

  // Collects (key, payload) of every sample on `keyexpr`.
  void Watch(const std::string& keyexpr) {
    z_owned_subscriber_t sub;
    ASSERT_TRUE(bus_->CreateSubscriber(keyexpr, [this](const z_loaned_sample_t* sample) {
      z_view_string_t key;
      z_keyexpr_as_view_string(z_sample_keyexpr(sample), &key);
      z_owned_slice_t slice;
      z_bytes_to_slice(z_sample_payload(sample), &slice);
      const z_loaned_shm_t* shm = nullptr;
      const bool is_shm = z_bytes_to_loaned_shm(z_sample_payload(sample), &shm) == Z_OK;
      std::lock_guard lock(mutex_);
      shm_.push_back(is_shm);
      seen_.emplace_back(std::string(z_string_data(z_loan(key)), z_string_len(z_loan(key))),
                         std::string(reinterpret_cast<const char*>(z_slice_data(z_loan(slice))),
                                     z_slice_len(z_loan(slice))));
      z_drop(z_move(slice));
    }, sub));
    subs_.push_back(sub);
  }

  // The test's own publishers, registered like the driver's before it starts.
  void PeerPublishes(std::initializer_list<std::string> keyexprs) {
    for (const auto& keyexpr : keyexprs) ASSERT_TRUE(bus_->RegisterPublisher(keyexpr, false));
  }

  void Put(const std::string& keyexpr, std::vector<char> frame) { bus_->Send(keyexpr, frame.data(), frame.size()); }

  // `payload` is a frame whose body of `size` bytes ends with `last`.
  static void ExpectFrame(const std::string& payload, size_t size, char last) {
    ASSERT_GE(payload.size(), 4u);
    const uint32_t frame_len = common::util::GetUint32FromBuf(payload.data());
    EXPECT_GT(frame_len, size);
    EXPECT_LT(frame_len, size + 256);
    ASSERT_LE(4 + size_t{frame_len}, payload.size());
    EXPECT_EQ(payload[4 + frame_len - 1], last);
  }

  // Answers a call. The frame's callback holds the frame: it is released first.
  static void Answer(const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& frame) {
    auto callback = std::move(frame->callback);
    frame->callback = nullptr;
    callback(ibmw::rpc::Status(IBMW_RPC_STATUS_OK));
  }

  // Waits until `count` samples were seen; returns a copy of them.
  std::vector<std::pair<std::string, std::string>> WaitSeen(size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      {
        std::lock_guard lock(mutex_);
        if (seen_.size() >= count) return seen_;
      }
      std::this_thread::sleep_for(5ms);
    }
    std::lock_guard lock(mutex_);
    return seen_;
  }

  std::filesystem::path directory_;
  std::shared_ptr<ZenohBus> bus_;
  std::unique_ptr<ZenohRpcDriver> driver_;
  std::vector<runtime::core::service::ServiceInfo> infos_;
  std::vector<z_owned_subscriber_t> subs_;
  std::mutex mutex_;
  std::vector<std::pair<std::string, std::string>> seen_;
  std::vector<bool> shm_;  // per seen_ entry: delivered through shm
  ibmw::rpc::Context ctx_;
};

// A request on a's key that names b's answer key is dropped, so it cannot
// put an answer on b's key; a normal request is served and answered on a's.
TEST_F(ZenohRpcDriverTest, ServerAnswersOnlyOnTheKeyOfTheCalledFunction) {
  auto& driver = NewDriver();
  std::vector<int> served;
  std::vector<runtime::core::service::ServiceFuncWrapper> services;
  for (const auto& info : infos_) {
    services.push_back({.info = info, .service_func = [this, &served](const auto& frame) {
                          std::unique_lock lock(mutex_);
                          served.push_back(*static_cast<const int*>(frame->req_ptr));
                          *static_cast<int*>(frame->rsp_ptr) = 100;
                          lock.unlock();
                          Answer(frame);
                        }});
  }
  for (const auto& service : services) ASSERT_TRUE(driver.RegisterServiceFunc(service));
  ASSERT_NO_FATAL_FAILURE(PeerPublishes({"req/" + Pattern("/test/a")}));
  driver.Start();
  Watch("rsp/ibmw_rpc/**");

  const std::string a = Pattern("/test/a"), b = Pattern("/test/b");
  auto request = [](const std::string& pattern, char id, const std::string& body) {
    return Frame(std::string(1, static_cast<char>(pattern.size())) + pattern + std::string(3, '\0') + id +
                     std::string(1, '\0'),
                 body);
  };
  Put("req/" + a, request(b, 1, "1"));
  Put("req/" + a, request(a, 2, "2"));

  const auto seen = WaitSeen(1);
  ASSERT_EQ(seen.size(), 1u);
  EXPECT_EQ(seen[0].first, "rsp/" + a);
  std::lock_guard lock(mutex_);
  EXPECT_EQ(served, std::vector<int>{2});
}

// An answer that carries the call's id but arrives on another function's
// key leaves the call pending; the answer on the called key completes it.
TEST_F(ZenohRpcDriverTest, ClientCompletesCallOnlyWithAnswerOnItsKey) {
  auto& driver = NewDriver();
  for (const auto& info : infos_) ASSERT_TRUE(driver.RegisterClientFunc({.info = info}));
  ASSERT_NO_FATAL_FAILURE(PeerPublishes({"rsp/" + Pattern("/test/a"), "rsp/" + Pattern("/test/b")}));
  driver.Start();
  const std::string a = Pattern("/test/a"), b = Pattern("/test/b");
  Watch("req/" + a);

  std::vector<uint32_t> codes;
  int req = 7, rsp = 0;
  auto frame = std::make_shared<runtime::core::service::ServiceMessageFrame>(
      runtime::core::service::ServiceMessageFrame{
          .info = infos_[0], .req_ptr = &req, .rsp_ptr = &rsp, .ctx_ref = ibmw::rpc::ContextRef(ctx_)});
  frame->callback = [this, &codes](ibmw::rpc::Status status) {
    std::lock_guard lock(mutex_);
    codes.push_back(status.Code());
  };
  auto completed = [&] {
    std::lock_guard lock(mutex_);
    return codes;
  };
  driver.Call(frame);

  // [len][u8 "json"][u8 pattern][req id]...: the id follows the pattern.
  const auto seen = WaitSeen(1);
  ASSERT_EQ(seen.size(), 1u);
  const std::string req_id = seen[0].second.substr(4 + 1 + 4 + 1 + a.size(), 4);
  const std::string ok(4, '\0');

  Put("rsp/" + b, Frame(req_id + ok, "99"));
  std::this_thread::sleep_for(100ms);
  EXPECT_TRUE(completed().empty());

  Put("rsp/" + a, Frame(req_id + ok, "5"));
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (completed().empty() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
  ASSERT_EQ(completed(), std::vector<uint32_t>{IBMW_RPC_STATUS_OK});
  EXPECT_EQ(rsp, 5);
}

// A request with id `id` for `pattern`.
std::vector<char> Request(const std::string& pattern, char id, const std::string& body) {
  return Frame(std::string(1, static_cast<char>(pattern.size())) + pattern + std::string(3, '\0') + id +
                   std::string(1, '\0'),
               body);
}

// A request that arrives before Start is dropped: answering it would look up
// the answer publisher while others are still being registered.
TEST_F(ZenohRpcDriverTest, ServerAnswersOnlyAfterStart) {
  auto& driver = NewDriver();
  std::atomic<int> served = 0;
  const runtime::core::service::ServiceFuncWrapper service{.info = infos_[0], .service_func = [&](const auto& frame) {
    ++served;
    *static_cast<int*>(frame->rsp_ptr) = 1;
    Answer(frame);
  }};
  ASSERT_TRUE(driver.RegisterServiceFunc(service));
  const std::string a = Pattern("/test/a");
  ASSERT_NO_FATAL_FAILURE(PeerPublishes({"req/" + a}));
  Watch("rsp/" + a);
  Put("req/" + a, Request(a, 1, "1"));  // the test's peer; nothing registers concurrently
  std::this_thread::sleep_for(300ms);
  EXPECT_EQ(served.load(), 0);
  driver.Start();
  Put("req/" + a, Request(a, 2, "2"));
  EXPECT_EQ(WaitSeen(1).size(), 1u);
  EXPECT_EQ(served.load(), 1);
}

// A function registered again by another module is accepted, but requests
// keep going to the first server.
TEST_F(ZenohRpcDriverTest, SecondServerOfAFunctionIsNotCalled) {
  auto& driver = NewDriver();
  auto other = infos_[0];
  other.module_name = "other";
  std::vector<std::string> served;
  auto serve = [this, &served](const std::string& module) {
    return [this, &served, module](const auto& frame) {
      {
        std::lock_guard lock(mutex_);
        served.push_back(module);
      }
      *static_cast<int*>(frame->rsp_ptr) = 1;
      Answer(frame);
    };
  };
  const runtime::core::service::ServiceFuncWrapper first{.info = infos_[0], .service_func = serve("mod")};
  const runtime::core::service::ServiceFuncWrapper second{.info = other, .service_func = serve("other")};
  ASSERT_TRUE(driver.RegisterServiceFunc(first));
  ASSERT_TRUE(driver.RegisterServiceFunc(second));
  const std::string a = Pattern("/test/a");
  ASSERT_NO_FATAL_FAILURE(PeerPublishes({"req/" + a}));
  driver.Start();
  Watch("rsp/" + a);
  Put("req/" + a, Request(a, 1, "1"));
  ASSERT_EQ(WaitSeen(1).size(), 1u);
  std::lock_guard lock(mutex_);
  EXPECT_EQ(served, std::vector<std::string>{"mod"});
}

// Clients of one function in two modules share its answer subscriber: calls
// of both complete.
TEST_F(ZenohRpcDriverTest, ClientsOfOneFunctionShareItsAnswers) {
  auto& driver = NewDriver();
  auto other = infos_[0];
  other.module_name = "other";
  ASSERT_TRUE(driver.RegisterClientFunc({.info = infos_[0]}));
  ASSERT_TRUE(driver.RegisterClientFunc({.info = other}));
  const std::string a = Pattern("/test/a");
  ASSERT_NO_FATAL_FAILURE(PeerPublishes({"rsp/" + a}));
  driver.Start();
  Watch("req/" + a);

  std::vector<uint32_t> codes;
  int req = 7, rsp[2] = {0, 0};
  for (int i = 0; i < 2; ++i) {
    auto frame = std::make_shared<runtime::core::service::ServiceMessageFrame>(
        runtime::core::service::ServiceMessageFrame{.info = i == 0 ? infos_[0] : other,
                                                    .req_ptr = &req,
                                                    .rsp_ptr = &rsp[i],
                                                    .ctx_ref = ibmw::rpc::ContextRef(ctx_)});
    frame->callback = [this, &codes](ibmw::rpc::Status status) {
      std::lock_guard lock(mutex_);
      codes.push_back(status.Code());
    };
    driver.Call(frame);
  }
  const auto seen = WaitSeen(2);
  ASSERT_EQ(seen.size(), 2u);
  for (int i = 0; i < 2; ++i) {
    // [len][u8 "json"][u8 pattern][req id]...: the id follows the pattern.
    const std::string req_id = seen[i].second.substr(4 + 1 + 4 + 1 + a.size(), 4);
    Put("rsp/" + a, Frame(req_id + std::string(4, '\0'), std::to_string(5 + i)));
  }
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  auto done = [&] {
    std::lock_guard lock(mutex_);
    return codes.size() == 2;
  };
  while (!done() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
  std::lock_guard lock(mutex_);
  EXPECT_EQ(codes, (std::vector<uint32_t>{IBMW_RPC_STATUS_OK, IBMW_RPC_STATUS_OK}));
  EXPECT_EQ(rsp[0] + rsp[1], 11);
  EXPECT_NE(rsp[0], 0);
}

// The driver reports what the bus refuses, here because it is frozen.
TEST_F(ZenohRpcDriverTest, RegistrationOnAFrozenBusFails) {
  auto& driver = NewDriver();
  bus_->Freeze();
  EXPECT_FALSE(driver.RegisterClientFunc({.info = infos_[0]}));
  const runtime::core::service::ServiceFuncWrapper service{.info = infos_[1], .service_func = [](const auto&) {}};
  EXPECT_FALSE(driver.RegisterServiceFunc(service));
}

TEST_F(ZenohRpcDriverTest, StartFreezesTheBus) {
  NewDriver().Start();
  EXPECT_FALSE(bus_->RegisterPublisher("req/late", false));
}

// A cached message too long for the uint32 length prefix (its views claim
// 4 GiB + 100 bytes; nothing reads them) is refused, not cut to 100 bytes and
// copied whole into a small loan.
std::shared_ptr<ibmw::util::BufferArrayView> TooLongCache(const std::string& data) {
  return std::make_shared<ibmw::util::BufferArrayView>(std::vector<ibmw_buffer_view_t>{
      {.data = data.data(), .len = size_t{1} << 32}, {.data = data.data(), .len = 100}});
}

TEST_F(ZenohRpcDriverTest, ClientRefusesARequestTooLongForItsFrame) {
  auto& driver = NewDriver("{clients: [{func_name: /test/a, shm_enabled: true}]}");
  const runtime::core::service::ClientFuncWrapper client{.info = BlobInfo()};
  ASSERT_TRUE(driver.RegisterClientFunc(client));
  driver.Start();
  Watch("req/" + Pattern("/test/a"));

  auto req = std::make_shared<std::string>(100, 'x');
  auto rsp = std::make_shared<std::string>();
  auto frame = std::make_shared<runtime::core::service::ServiceMessageFrame>(
      runtime::core::service::ServiceMessageFrame{
          .info = client.info, .req_ptr = req.get(), .rsp_ptr = rsp.get(), .ctx_ref = ibmw::rpc::ContextRef(ctx_)});
  frame->req_serialization_cache["json"] = TooLongCache(*req);
  std::promise<uint32_t> code;
  frame->callback = [req, rsp, &code](ibmw::rpc::Status status) { code.set_value(status.Code()); };
  driver.Call(frame);
  auto result = code.get_future();
  ASSERT_EQ(result.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(result.get(), IBMW_RPC_STATUS_CLI_SERIALIZATION_FAILED);
  std::this_thread::sleep_for(100ms);
  EXPECT_TRUE(WaitSeen(0).empty());
}

TEST_F(ZenohRpcDriverTest, ServerRefusesAnAnswerTooLongForItsFrame) {
  auto& driver = NewDriver("{servers: [{func_name: /test/a, shm_enabled: true}]}");
  auto answer = std::make_shared<std::string>(100, 'w');
  const runtime::core::service::ServiceFuncWrapper service{
      .info = BlobInfo(), .service_func = [answer](const auto& frame) {
        *static_cast<std::string*>(frame->rsp_ptr) = *answer;
        frame->rsp_serialization_cache["json"] = TooLongCache(*answer);
        Answer(frame);
      }};
  ASSERT_TRUE(driver.RegisterServiceFunc(service));
  ASSERT_NO_FATAL_FAILURE(PeerPublishes({"req/" + Pattern("/test/a")}));
  driver.Start();
  const std::string a = Pattern("/test/a");
  Watch("rsp/" + a);

  Put("req/" + a, Frame(std::string(1, static_cast<char>(a.size())) + a + std::string(3, '\0') + '\1' +
                            std::string(1, '\0'),
                        "q"));
  ASSERT_EQ(WaitSeen(1).size(), 1u);
  std::this_thread::sleep_for(100ms);
  const auto seen = WaitSeen(0);
  ASSERT_EQ(seen.size(), 1u);  // only the refusal
  // [u32 len = 13][u8 4]["json"][u32 req id][u32 code]
  const std::string& answer_frame = seen[0].second;
  ASSERT_EQ(answer_frame.size(), 4u + 1 + 4 + 4 + 4);
  EXPECT_EQ(common::util::GetUint32FromBuf(answer_frame.data()), 13u);
  EXPECT_EQ(answer_frame.substr(4 + 1 + 4, 4), std::string(3, '\0') + '\1');
  EXPECT_EQ(common::util::GetUint32FromBuf(answer_frame.data() + 4 + 1 + 4 + 4),
            uint32_t{IBMW_RPC_STATUS_SVR_SERIALIZATION_FAILED});
}

// Shutdown removes the driver's subscribers and waits for a request being
// served, so the driver may be destroyed right after it.
TEST_F(ZenohRpcDriverTest, ShutdownWaitsForTheHandlersAndRemovesThem) {
  auto& driver = NewDriver();
  std::promise<void> entered, release;
  auto released = release.get_future().share();
  const runtime::core::service::ServiceFuncWrapper service{.info = infos_[0], .service_func = [&](const auto&) {
    entered.set_value();
    released.wait();
  }};
  ASSERT_TRUE(driver.RegisterServiceFunc(service));
  ASSERT_TRUE(driver.RegisterClientFunc({.info = infos_[1]}));
  const std::string a = Pattern("/test/a");
  ASSERT_NO_FATAL_FAILURE(PeerPublishes({"req/" + a}));
  driver.Start();
  // The local server runs in the put: it blocks the putting thread.
  auto request = std::async(std::launch::async, [&] { Put("req/" + a, Request(a, 1, "1")); });
  ASSERT_EQ(entered.get_future().wait_for(5s), std::future_status::ready);
  auto shutdown = std::async(std::launch::async, [&] { driver.Shutdown(); });
  EXPECT_EQ(shutdown.wait_for(200ms), std::future_status::timeout);
  release.set_value();
  ASSERT_EQ(shutdown.wait_for(5s), std::future_status::ready);
  request.get();
  EXPECT_FALSE(bus_->UnregisterSubscriber("req/" + a));
  EXPECT_FALSE(bus_->UnregisterSubscriber("rsp/" + Pattern("/test/b")));
}

// Shutdown keeps the client tool; a call still waiting gets its result, once.
TEST_F(ZenohRpcDriverTest, CallStillWaitingGetsItsResultFromTheDestructor) {
  auto& driver = NewDriver();
  ASSERT_TRUE(driver.RegisterClientFunc({.info = infos_[0]}));
  driver.Start();
  std::vector<uint32_t> codes;
  int req = 7, rsp = 0;
  auto frame = std::make_shared<runtime::core::service::ServiceMessageFrame>(
      runtime::core::service::ServiceMessageFrame{
          .info = infos_[0], .req_ptr = &req, .rsp_ptr = &rsp, .ctx_ref = ibmw::rpc::ContextRef(ctx_)});
  frame->callback = [&codes](ibmw::rpc::Status status) { codes.push_back(status.Code()); };
  driver.Call(frame);
  const std::weak_ptr<runtime::core::service::ServiceMessageFrame> recorded = frame;
  frame.reset();
  driver.Shutdown();
  EXPECT_TRUE(codes.empty());
  EXPECT_FALSE(recorded.expired());
  driver_.reset();
  EXPECT_EQ(codes, std::vector<uint32_t>{IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR});
  EXPECT_TRUE(recorded.expired());
}

// Requests of 600 KiB outgrow the 1 KiB loan: each smaller loan goes back
// to the 1 MiB pool, so this and the next requests still go through shm.
TEST_F(ZenohRpcDriverTest, ClientLoansThatWereTooSmallGoBackToThePool) {
  auto& driver = NewDriver("{clients: [{func_name: /test/a, shm_enabled: true}]}");
  // Frames keep a reference to this info.
  const runtime::core::service::ClientFuncWrapper client{.info = BlobInfo()};
  ASSERT_TRUE(driver.RegisterClientFunc(client));
  driver.Start();
  Watch("req/" + Pattern("/test/a"));

  auto req = std::make_shared<std::string>(600 * 1024, 'x');
  req->back() = 'z';
  auto rsp = std::make_shared<std::string>();
  for (size_t round = 0; round < 3; ++round) {
    auto frame = std::make_shared<runtime::core::service::ServiceMessageFrame>(
        runtime::core::service::ServiceMessageFrame{
            .info = client.info, .req_ptr = req.get(), .rsp_ptr = rsp.get(), .ctx_ref = ibmw::rpc::ContextRef(ctx_)});
    frame->callback = [req, rsp](ibmw::rpc::Status) {};
    driver.Call(frame);
    const auto seen = WaitSeen(round + 1);
    ASSERT_EQ(seen.size(), round + 1);
    ExpectFrame(seen[round].second, 600 * 1024, 'z');
    std::lock_guard lock(mutex_);
    EXPECT_TRUE(shm_[round]) << round;
  }
}

// An already serialized request gets a loan of its size once, and the call
// is sent (a resized loan used to be retried without ever fitting it).
TEST_F(ZenohRpcDriverTest, ClientCachedRequestGetsALoanOfItsSize) {
  auto& driver = NewDriver("{clients: [{func_name: /test/a, shm_enabled: true}]}");
  // Frames keep a reference to this info.
  const runtime::core::service::ClientFuncWrapper client{.info = BlobInfo()};
  ASSERT_TRUE(driver.RegisterClientFunc(client));
  driver.Start();
  Watch("req/" + Pattern("/test/a"));

  auto req = std::make_shared<std::string>(600 * 1024, 'x');
  req->back() = 'z';
  auto rsp = std::make_shared<std::string>();
  for (size_t round = 0; round < 3; ++round) {
    auto frame = std::make_shared<runtime::core::service::ServiceMessageFrame>(
        runtime::core::service::ServiceMessageFrame{
            .info = client.info, .req_ptr = req.get(), .rsp_ptr = rsp.get(), .ctx_ref = ibmw::rpc::ContextRef(ctx_)});
    frame->req_serialization_cache["json"] = std::make_shared<ibmw::util::BufferArrayView>(req->data(), req->size());
    frame->callback = [req, rsp](ibmw::rpc::Status) {};
    driver.Call(frame);
    const auto seen = WaitSeen(round + 1);
    ASSERT_EQ(seen.size(), round + 1);
    ExpectFrame(seen[round].second, 600 * 1024, 'z');
    std::lock_guard lock(mutex_);
    EXPECT_TRUE(shm_[round]) << round;
  }
}

// A request serialized into several buffers is copied buffer by buffer.
TEST_F(ZenohRpcDriverTest, ClientCachedRequestInSeveralBuffersIsCopiedWhole) {
  auto& driver = NewDriver("{clients: [{func_name: /test/a, shm_enabled: true}]}");
  // Frames keep a reference to this info.
  const runtime::core::service::ClientFuncWrapper client{.info = BlobInfo()};
  ASSERT_TRUE(driver.RegisterClientFunc(client));
  driver.Start();
  Watch("req/" + Pattern("/test/a"));

  auto req =
      std::make_shared<std::string>(std::string(200, 'a') + std::string(200, 'b') + std::string(199, 'c') + 'z');
  auto rsp = std::make_shared<std::string>();
  auto frame = std::make_shared<runtime::core::service::ServiceMessageFrame>(
      runtime::core::service::ServiceMessageFrame{
          .info = client.info, .req_ptr = req.get(), .rsp_ptr = rsp.get(), .ctx_ref = ibmw::rpc::ContextRef(ctx_)});
  frame->req_serialization_cache["json"] = std::make_shared<ibmw::util::BufferArrayView>(
      std::vector<ibmw_buffer_view_t>{{.data = req->data(), .len = 200},
                                      {.data = req->data() + 200, .len = 200},
                                      {.data = req->data() + 400, .len = 200}});
  frame->callback = [req, rsp](ibmw::rpc::Status) {};
  driver.Call(frame);
  const auto seen = WaitSeen(1);
  ASSERT_EQ(seen.size(), 1u);
  ExpectFrame(seen[0].second, 600, 'z');
  const uint32_t frame_len = common::util::GetUint32FromBuf(seen[0].second.data());
  EXPECT_EQ(seen[0].second.substr(4 + frame_len - 600, 600), *req);
  std::lock_guard lock(mutex_);
  EXPECT_TRUE(shm_[0]);
}

// The same for answers of 600 KiB.
TEST_F(ZenohRpcDriverTest, ServerLoansThatWereTooSmallGoBackToThePool) {
  auto& driver = NewDriver("{servers: [{func_name: /test/a, shm_enabled: true}]}");
  // The driver keeps a reference to the wrapper.
  const runtime::core::service::ServiceFuncWrapper service{
      .info = BlobInfo(), .service_func = [](const auto& frame) {
        *static_cast<std::string*>(frame->rsp_ptr) = std::string(600 * 1024 - 1, 'y') + 'w';
        Answer(frame);
      }};
  ASSERT_TRUE(driver.RegisterServiceFunc(service));
  ASSERT_NO_FATAL_FAILURE(PeerPublishes({"req/" + Pattern("/test/a")}));
  driver.Start();
  const std::string a = Pattern("/test/a");
  Watch("rsp/" + a);

  for (size_t round = 0; round < 3; ++round) {
    Put("req/" + a, Frame(std::string(1, static_cast<char>(a.size())) + a + std::string(3, '\0') +
                              static_cast<char>(round + 1) + std::string(1, '\0'),
                          "q"));
    const auto seen = WaitSeen(round + 1);
    ASSERT_EQ(seen.size(), round + 1);
    ExpectFrame(seen[round].second, 600 * 1024, 'w');
    std::lock_guard lock(mutex_);
    EXPECT_TRUE(shm_[round]) << round;
  }
}

// The same for an already serialized answer.
TEST_F(ZenohRpcDriverTest, ServerCachedAnswerGetsALoanOfItsSize) {
  auto& driver = NewDriver("{servers: [{func_name: /test/a, shm_enabled: true}]}");
  auto answer = std::make_shared<std::string>(600 * 1024, 'y');
  answer->back() = 'w';
  const runtime::core::service::ServiceFuncWrapper service{
      .info = BlobInfo(), .service_func = [answer](const auto& frame) {
        *static_cast<std::string*>(frame->rsp_ptr) = *answer;
        frame->rsp_serialization_cache["json"] =
            std::make_shared<ibmw::util::BufferArrayView>(answer->data(), answer->size());
        Answer(frame);
      }};
  ASSERT_TRUE(driver.RegisterServiceFunc(service));
  ASSERT_NO_FATAL_FAILURE(PeerPublishes({"req/" + Pattern("/test/a")}));
  driver.Start();
  const std::string a = Pattern("/test/a");
  Watch("rsp/" + a);

  for (size_t round = 0; round < 3; ++round) {
    Put("req/" + a, Frame(std::string(1, static_cast<char>(a.size())) + a + std::string(3, '\0') +
                              static_cast<char>(round + 1) + std::string(1, '\0'),
                          "q"));
    const auto seen = WaitSeen(round + 1);
    ASSERT_EQ(seen.size(), round + 1);
    ExpectFrame(seen[round].second, 600 * 1024, 'w');
    std::lock_guard lock(mutex_);
    EXPECT_TRUE(shm_[round]) << round;
  }
}

// The same for an answer serialized into several buffers.
TEST_F(ZenohRpcDriverTest, ServerCachedAnswerInSeveralBuffersIsCopiedWhole) {
  auto& driver = NewDriver("{servers: [{func_name: /test/a, shm_enabled: true}]}");
  auto answer =
      std::make_shared<std::string>(std::string(200, 'd') + std::string(200, 'e') + std::string(199, 'f') + 'w');
  const runtime::core::service::ServiceFuncWrapper service{
      .info = BlobInfo(), .service_func = [answer](const auto& frame) {
        *static_cast<std::string*>(frame->rsp_ptr) = *answer;
        frame->rsp_serialization_cache["json"] = std::make_shared<ibmw::util::BufferArrayView>(
            std::vector<ibmw_buffer_view_t>{{.data = answer->data(), .len = 200},
                                            {.data = answer->data() + 200, .len = 200},
                                            {.data = answer->data() + 400, .len = 200}});
        Answer(frame);
      }};
  ASSERT_TRUE(driver.RegisterServiceFunc(service));
  ASSERT_NO_FATAL_FAILURE(PeerPublishes({"req/" + Pattern("/test/a")}));
  driver.Start();
  const std::string a = Pattern("/test/a");
  Watch("rsp/" + a);

  Put("req/" + a, Frame(std::string(1, static_cast<char>(a.size())) + a + std::string(3, '\0') + '\1' +
                            std::string(1, '\0'),
                        "q"));
  const auto seen = WaitSeen(1);
  ASSERT_EQ(seen.size(), 1u);
  ExpectFrame(seen[0].second, 600, 'w');
  const uint32_t frame_len = common::util::GetUint32FromBuf(seen[0].second.data());
  EXPECT_EQ(seen[0].second.substr(4 + frame_len - 600, 600), *answer);
  std::lock_guard lock(mutex_);
  EXPECT_TRUE(shm_[0]);
}

}  // namespace
}  // namespace ibmw::extensions::zenoh_extension
