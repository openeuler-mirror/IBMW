// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for the gRPC service path (EXT-004): an answer given
// inline or from another thread is never lost, a missing answer ends the
// request by timeout, and a late answer is harmless.
// EXT-002: an oversized request body is refused before it reaches the service.
// Review S2/S3/N3: a failed call ends at once with an error status (not by
// the client timeout), and grpc-timeout values follow the 8-digit format.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <stdexcept>

#include <dirent.h>
#include <sys/resource.h>
#include <unistd.h>
#include <sys/socket.h>

#include "cpp/util/string.h"
#include "grpc_ext/grpc/status.h"
#include "grpc_ext/grpc/timeout.h"
#include "grpc_ext/grpc_extension.h"
#include "grpc_ext/grpc_rpc_driver.h"
#include "grpc_ext/http2/header.h"

namespace ibmw::extensions::grpc_extension {
namespace {

using namespace std::chrono_literals;
namespace asio = boost::asio;
using Tcp = asio::ip::tcp;
using runtime::core::service::ServiceMessageFrame;

uint16_t FreeTcpPort() {
  asio::io_context io;
  Tcp::acceptor acceptor(io, Tcp::endpoint(asio::ip::address_v4::loopback(), 0));
  return acceptor.local_endpoint().port();
}

class IoThreads {
 public:
  explicit IoThreads(int n = 2) : io_(std::make_shared<asio::io_context>()), guard_(io_->get_executor()) {
    for (int i = 0; i < n; ++i) threads_.emplace_back([this] { io_->run(); });
  }
  ~IoThreads() {
    guard_.reset();
    // Lets the pending handlers finish: stopping with work queued keeps their
    // frames, which hold sessions and through them the io_context, alive.
    for (const auto deadline = std::chrono::steady_clock::now() + 10s;
         !io_->stopped() && std::chrono::steady_clock::now() < deadline;) {
      std::this_thread::sleep_for(10ms);
    }
    io_->stop();
    for (auto& t : threads_) t.join();
  }
  const std::shared_ptr<asio::io_context>& Io() { return io_; }

 private:
  std::shared_ptr<asio::io_context> io_;
  asio::executor_work_guard<asio::io_context::executor_type> guard_;
  std::vector<std::thread> threads_;
};

// "cdr" int: the decimal text of the value.
const ibmw_type_support_base_t* IntTypeSupport() {
  static const ibmw_string_view_t kTypes[] = {{.str = "cdr", .len = 3}};
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
      .deserialize = [](void*, ibmw_string_view_t, ibmw_buffer_array_view_t view, void* msg) {
        std::string text;
        for (size_t i = 0; i < view.len; ++i)
          text.append(static_cast<const char*>(view.data[i].data), view.data[i].len);
        try {
          *static_cast<int*>(msg) = std::stoi(text);
          return true;
        } catch (...) {
          return false;
        }
      },
      .serialization_types_supported_num = [](void*) -> size_t { return 1; },
      .serialization_types_supported_list = [](void*) -> const ibmw_string_view_t* { return kTypes; },
      .custom_type_support_ptr = [](void*) -> const void* { return nullptr; },
      .ctx = nullptr};
  return &ts;
}

TEST(GrpcTimeoutTest, ParsesUpToEightDigits) {
  EXPECT_EQ(grpc::ParseTimeout("1H"), std::chrono::hours(1));
  EXPECT_EQ(grpc::ParseTimeout("2M"), std::chrono::minutes(2));
  EXPECT_EQ(grpc::ParseTimeout("3S"), std::chrono::seconds(3));
  EXPECT_EQ(grpc::ParseTimeout("4m"), std::chrono::milliseconds(4));
  EXPECT_EQ(grpc::ParseTimeout("5u"), std::chrono::microseconds(5));
  EXPECT_EQ(grpc::ParseTimeout("0n"), std::chrono::nanoseconds(0));
  EXPECT_EQ(grpc::ParseTimeout("2562047H"), std::chrono::hours(2562047));
  // Beyond the nanosecond range: saturates instead of wrapping negative.
  EXPECT_EQ(grpc::ParseTimeout("2562048H"), std::chrono::nanoseconds::max());
  EXPECT_EQ(grpc::ParseTimeout("99999999H"), std::chrono::nanoseconds::max());
  EXPECT_EQ(grpc::ParseTimeout("99999999M"), std::chrono::minutes(99999999));  // 6e18 ns: still fits
  for (const char* bad : {"", "S", "1", "123456789S", "99999999999999999999H", "-1S", "+1S", "1s", "1 S", "1SS",
                          "a1S"}) {
    EXPECT_FALSE(grpc::ParseTimeout(bad).has_value()) << bad;
  }
}

TEST(GrpcTimeoutTest, FormatsWithinEightDigitsAndNeverShortens) {
  EXPECT_EQ(grpc::FormatTimeout(std::chrono::nanoseconds(0)), "0n");
  EXPECT_EQ(grpc::FormatTimeout(std::chrono::nanoseconds(99999999)), "99999999n");
  EXPECT_EQ(grpc::FormatTimeout(std::chrono::nanoseconds(100000001)), "100001u");
  EXPECT_EQ(grpc::FormatTimeout(5s), "5000000u");
  EXPECT_EQ(grpc::FormatTimeout(std::chrono::hours(48)), "172800S");
  EXPECT_EQ(grpc::FormatTimeout(std::chrono::nanoseconds::max()), "2562048H");
  for (auto t : {std::chrono::nanoseconds(1), std::chrono::nanoseconds(123456789), std::chrono::nanoseconds(5s),
                 std::chrono::nanoseconds(std::chrono::hours(48)), std::chrono::nanoseconds::max()}) {
    const std::string text = grpc::FormatTimeout(t);
    auto parsed = grpc::ParseTimeout(text);
    ASSERT_TRUE(parsed.has_value()) << text;
    EXPECT_GE(*parsed, t) << text;
  }
}

TEST(GrpcStatusTest, MapsBothWays) {
  for (uint32_t code : {IBMW_RPC_STATUS_TIMEOUT, IBMW_RPC_STATUS_SVR_NOT_IMPLEMENTED,
                        IBMW_RPC_STATUS_SVR_DRIVER_INTERNAL_ERROR, IBMW_RPC_STATUS_SVR_NOT_READY}) {
    EXPECT_EQ(grpc::FromGrpcStatus(grpc::ToGrpcStatus(code)), code);
  }
  EXPECT_STREQ(grpc::ToGrpcStatus(IBMW_RPC_STATUS_SVR_NOT_FOUND), "12");
  EXPECT_STREQ(grpc::ToGrpcStatus(IBMW_RPC_STATUS_SVR_SERIALIZATION_FAILED), "13");
  EXPECT_STREQ(grpc::ToGrpcStatus(IBMW_RPC_STATUS_SVR_HANDLE_FAILED), "2");
  EXPECT_EQ(grpc::FromGrpcStatus("2"), static_cast<uint32_t>(IBMW_RPC_STATUS_CLI_UNKNOWN));
  EXPECT_EQ(grpc::FromGrpcStatus("x"), static_cast<uint32_t>(IBMW_RPC_STATUS_CLI_UNKNOWN));
}

void Answer(const std::shared_ptr<ServiceMessageFrame>& frame, uint32_t code) {
  auto callback = std::move(frame->callback);
  frame->callback = nullptr;
  callback(ibmw::rpc::Status(code));
}

// One driver serving "cdr:/test/svc" and calling itself over loopback.
class GrpcRpcDriverTest : public ::testing::Test {
 protected:
  void SetUpDriver(runtime::core::service::ServiceFunc func, size_t max_request_body_size = 16U << 20) {
    const uint16_t port = FreeTcpPort();
    svr_addr_ = "127.0.0.1:" + std::to_string(port);
    svr_ = std::make_shared<server::AsioHttp2Server>(io_.Io());
    pool_ = std::make_shared<client::AsioHttp2ClientPool>(io_.Io());
    driver_ = std::make_unique<GrpcRpcDriver>(io_.Io(), svr_, pool_);
    driver_->Initialize(YAML::Load("{clients: [{func_name: '.*', server_url: 'http://127.0.0.1:" +
                                   std::to_string(port) + "'}]}"));

    info_.func_name = "cdr:/test/svc";
    info_.pkg_path = "pkg";
    info_.module_name = "mod";
    info_.req_type_support_ref = ibmw::util::TypeSupportRef(IntTypeSupport());
    info_.rsp_type_support_ref = ibmw::util::TypeSupportRef(IntTypeSupport());
    service_.info = info_;
    service_.service_func = std::move(func);
    client_.info = info_;
    ASSERT_TRUE(driver_->RegisterServiceFunc(service_));
    ASSERT_TRUE(driver_->RegisterClientFunc(client_));

    svr_->Initialize(server::ServerOptions{.ep = {asio::ip::address_v4::loopback(), port},
                                           .http2_settings = {.max_request_body_size = max_request_body_size}});
    svr_->Start();
    pool_->Initialize(client::ClientPoolOptions{});
    pool_->Start();
    driver_->Start();
  }

  void TearDown() override {
    if (!driver_) return;
    driver_->Shutdown();
    pool_->Shutdown();
    svr_->Shutdown();
  }

  struct Result {
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<uint32_t> code;
    int rsp = 0;
  };

  // Calls the service; returns the status code, or nullopt without an answer within 10s.
  std::optional<uint32_t> Call(int req, int* rsp, std::chrono::nanoseconds timeout = 5s,
                               const std::vector<std::pair<std::string, std::string>>& meta = {}) {
    auto result = std::make_shared<Result>();
    auto ctx = std::make_shared<ibmw::rpc::Context>();
    ctx->SetSerializationType("cdr");
    ctx->SetTimeout(timeout);
    for (const auto& [key, value] : meta) ctx->SetMetaValue(key, value);
    auto req_ptr = std::make_shared<int>(req);
    auto frame = std::make_shared<ServiceMessageFrame>(ServiceMessageFrame{
        .info = info_, .req_ptr = req_ptr.get(), .rsp_ptr = &result->rsp, .ctx_ref = ibmw::rpc::ContextRef(*ctx)});
    frame->callback = [result, ctx, req_ptr](ibmw::rpc::Status status) {
      std::lock_guard lock(result->mutex);
      result->code = status.Code();
      result->cv.notify_all();
    };
    driver_->Call(frame);
    std::unique_lock lock(result->mutex);
    if (!result->cv.wait_for(lock, 10s, [&] { return result->code.has_value(); })) return std::nullopt;
    *rsp = result->rsp;
    return result->code;
  }

  IoThreads io_{2};
  std::string svr_addr_;
  std::shared_ptr<server::AsioHttp2Server> svr_;
  std::shared_ptr<client::AsioHttp2ClientPool> pool_;
  std::unique_ptr<GrpcRpcDriver> driver_;
  runtime::core::service::ServiceInfo info_;
  runtime::core::service::ServiceFuncWrapper service_;
  runtime::core::service::ClientFuncWrapper client_;
  // Requests a service kept unanswered; owned by the fixture so that the
  // service function (run on io threads) never touches a finished test body.
  std::mutex pending_mutex_;
  std::vector<std::shared_ptr<ServiceMessageFrame>> pending_;
};

TEST_F(GrpcRpcDriverTest, InlineAnswersAreNeverLost) {
  SetUpDriver([](const std::shared_ptr<ServiceMessageFrame>& frame) {
    *static_cast<int*>(frame->rsp_ptr) = *static_cast<const int*>(frame->req_ptr) + 1;
    Answer(frame, IBMW_RPC_STATUS_OK);
  });
  for (int i = 0; i < 500; ++i) {
    int rsp = 0;
    auto code = Call(i, &rsp);
    ASSERT_TRUE(code) << "request " << i << ": no answer";
    ASSERT_EQ(*code, static_cast<uint32_t>(IBMW_RPC_STATUS_OK)) << "request " << i;
    ASSERT_EQ(rsp, i + 1) << "request " << i;
  }
}

TEST_F(GrpcRpcDriverTest, CrossThreadAnswersAreNeverLost) {
  IoThreads answerer(2);
  SetUpDriver([&answerer](const std::shared_ptr<ServiceMessageFrame>& frame) {
    asio::post(*answerer.Io(), [frame] {
      *static_cast<int*>(frame->rsp_ptr) = *static_cast<const int*>(frame->req_ptr) + 1;
      Answer(frame, IBMW_RPC_STATUS_OK);
    });
  });
  for (int i = 0; i < 500; ++i) {
    int rsp = 0;
    auto code = Call(i, &rsp);
    ASSERT_TRUE(code) << "request " << i << ": no answer";
    ASSERT_EQ(*code, static_cast<uint32_t>(IBMW_RPC_STATUS_OK)) << "request " << i;
    ASSERT_EQ(rsp, i + 1) << "request " << i;
  }
}

// A service that never answers in time: the request ends by timeout, a late
// answer touches nothing that is gone, and the server keeps serving.
TEST_F(GrpcRpcDriverTest, MissingAnswerTimesOutAndLateAnswerIsHarmless) {
  SetUpDriver([this](const std::shared_ptr<ServiceMessageFrame>& frame) {
    std::lock_guard lock(pending_mutex_);
    pending_.push_back(frame);
  });
  int rsp = 0;
  const auto begin = std::chrono::steady_clock::now();
  auto code = Call(1, &rsp, 300ms);
  ASSERT_TRUE(code) << "no answer";
  EXPECT_NE(*code, static_cast<uint32_t>(IBMW_RPC_STATUS_OK));
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 3s);

  // The server got the same deadline; give it ample time to give up too
  // (an answer before that is harmless as well).
  std::this_thread::sleep_for(500ms);
  std::vector<std::shared_ptr<ServiceMessageFrame>> late;
  {
    std::lock_guard lock(pending_mutex_);
    late.swap(pending_);
  }
  ASSERT_EQ(late.size(), 1u);
  *static_cast<int*>(late[0]->rsp_ptr) = 7;  // the response object is still alive
  Answer(late[0], IBMW_RPC_STATUS_OK);

  code = Call(2, &rsp, 300ms);
  ASSERT_TRUE(code) << "server no longer answers";
  EXPECT_NE(*code, static_cast<uint32_t>(IBMW_RPC_STATUS_OK));

  // An unanswered frame keeps itself alive through its callback.
  std::lock_guard lock(pending_mutex_);
  for (const auto& frame : pending_) Answer(frame, IBMW_RPC_STATUS_OK);
}

// A gRPC body is a 5 byte prefix plus the payload: "7" fits in 8 bytes,
// "1234567890" does not.
TEST_F(GrpcRpcDriverTest, OversizedRequestIsRefusedAndServerStaysHealthy) {
  std::atomic<int> served{0};
  SetUpDriver(
      [&served](const std::shared_ptr<ServiceMessageFrame>& frame) {
        ++served;
        *static_cast<int*>(frame->rsp_ptr) = *static_cast<const int*>(frame->req_ptr) + 1;
        Answer(frame, IBMW_RPC_STATUS_OK);
      },
      8);
  int rsp = 0;
  const auto begin = std::chrono::steady_clock::now();
  auto code = Call(1234567890, &rsp, 5s);
  ASSERT_TRUE(code) << "no answer";
  EXPECT_NE(*code, static_cast<uint32_t>(IBMW_RPC_STATUS_OK));
  // The reset stream fails the call at once, not by the client timeout.
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 2s);
  EXPECT_EQ(served.load(), 0);

  for (int i = 0; i < 5; ++i) {
    code = Call(7, &rsp);
    ASSERT_TRUE(code) << "server no longer answers";
    ASSERT_EQ(*code, static_cast<uint32_t>(IBMW_RPC_STATUS_OK));
    EXPECT_EQ(rsp, 8);
  }
  EXPECT_EQ(served.load(), 5);
}

// A service answering with an error status: the client gets an error at
// once (not its timeout) and the server keeps serving.
TEST_F(GrpcRpcDriverTest, ServiceErrorEndsTheCallAtOnce) {
  SetUpDriver([](const std::shared_ptr<ServiceMessageFrame>& frame) {
    const int req = *static_cast<const int*>(frame->req_ptr);
    if (req < 0) {
      Answer(frame, req == -1 ? IBMW_RPC_STATUS_TIMEOUT : IBMW_RPC_STATUS_SVR_HANDLE_FAILED);
      return;
    }
    *static_cast<int*>(frame->rsp_ptr) = req + 1;
    Answer(frame, IBMW_RPC_STATUS_OK);
  });
  for (int req : {-1, -2, 3}) {
    int rsp = 0;
    const auto begin = std::chrono::steady_clock::now();
    auto code = Call(req, &rsp, 5s);
    ASSERT_TRUE(code) << "request " << req << ": no answer";
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 2s) << "request " << req;
    if (req < 0) {
      // The status survives the trip: a server timeout is reported as one.
      EXPECT_EQ(*code, static_cast<uint32_t>(req == -1 ? IBMW_RPC_STATUS_TIMEOUT : IBMW_RPC_STATUS_CLI_UNKNOWN))
          << "request " << req;
    } else {
      EXPECT_EQ(*code, static_cast<uint32_t>(IBMW_RPC_STATUS_OK));
      EXPECT_EQ(rsp, 4);
    }
  }
}

// EXT-001: the plaintext server warns unless it is reachable from this
// host only, and a bad listen address fails initialization.
TEST(GrpcHeaderLogTest, SensitiveValuesAreRedacted) {
  EXPECT_EQ(http2::LoggableHeaderValue("authorization", "Bearer abc"), "<redacted>");
  EXPECT_EQ(http2::LoggableHeaderValue("cookie", "s=1"), "<redacted>");
  EXPECT_EQ(http2::LoggableHeaderValue("x-custom", "v"), "v");
}

// EXT-005: request headers become server ctx meta, but a peer cannot set
// framework "ibmw-" keys, and Remote-Endpoint is the real peer address.
TEST_F(GrpcRpcDriverTest, PeerHeadersCannotSetReservedMeta) {
  std::mutex mutex;
  std::map<std::string, std::string> seen;
  SetUpDriver([&](const std::shared_ptr<ServiceMessageFrame>& frame) {
    {
      std::lock_guard lock(mutex);
      for (const char* key : {IBMW_RPC_CONTEXT_KEY_TO_ADDR, IBMW_RPC_CONTEXT_KEY_REQUEST_ID,
                              IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE, IBMW_RPC_CONTEXT_KEY_DRIVER,
                              "x-custom", "Remote-Endpoint", "remote-endpoint"})
        seen[key] = std::string(frame->ctx_ref.GetMetaValue(key));
    }
    *static_cast<int*>(frame->rsp_ptr) = 0;
    Answer(frame, IBMW_RPC_STATUS_OK);
  });
  int rsp = -1;
  auto code = Call(1, &rsp, 5s,
                   {{IBMW_RPC_CONTEXT_KEY_TO_ADDR, "http://" + svr_addr_},
                    {IBMW_RPC_CONTEXT_KEY_REQUEST_ID, "42"},
                    {"x-custom", "v"},
                    {"remote-endpoint", "10.0.0.1:1"}});
  ASSERT_TRUE(code) << "no answer";
  ASSERT_EQ(*code, static_cast<uint32_t>(IBMW_RPC_STATUS_OK));
  std::lock_guard lock(mutex);
  EXPECT_EQ(seen[IBMW_RPC_CONTEXT_KEY_TO_ADDR], "");
  EXPECT_EQ(seen[IBMW_RPC_CONTEXT_KEY_REQUEST_ID], "42");
  EXPECT_EQ(seen[IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE], "cdr");
  EXPECT_EQ(seen[IBMW_RPC_CONTEXT_KEY_DRIVER], "grpc");
  EXPECT_EQ(seen["x-custom"], "v");
  EXPECT_EQ(seen["Remote-Endpoint"].rfind("127.0.0.1:", 0), 0u) << seen["Remote-Endpoint"];
  EXPECT_EQ(seen["remote-endpoint"], "");
}

TEST(GrpcListenAddressTest, LoopbackIsRecognizedAndBadAddressesThrow) {
  EXPECT_TRUE(GrpcExtension::IsLoopbackListenAddress("127.0.0.1"));
  EXPECT_TRUE(GrpcExtension::IsLoopbackListenAddress("127.10.0.1"));
  EXPECT_FALSE(GrpcExtension::IsLoopbackListenAddress("0.0.0.0"));
  EXPECT_FALSE(GrpcExtension::IsLoopbackListenAddress("192.168.1.10"));
  EXPECT_THROW(GrpcExtension::IsLoopbackListenAddress("localhost"), std::invalid_argument);
  EXPECT_THROW(GrpcExtension::IsLoopbackListenAddress("256.0.0.1"), std::invalid_argument);
}

// EXT-008: when accept fails (here EMFILE) the server backs off instead of
// spinning on the error, and accepts again once descriptors are available.
TEST(GrpcServerAcceptTest, AcceptFailureBacksOffAndRecovers) {
  IoThreads io(2);
  const uint16_t port = FreeTcpPort();
  auto svr = std::make_shared<server::AsioHttp2Server>(io.Io());
  svr->Initialize(server::ServerOptions{.ep = {asio::ip::address_v4::loopback(), port}});
  svr->Start();
  std::this_thread::sleep_for(100ms);  // listening

  asio::io_context cio;
  asio::ip::tcp::socket sock(cio);
  sock.open(asio::ip::tcp::v4());

  // Lower the soft fd limit to the highest open descriptor + 1 so the
  // server's accept() fails with EMFILE.
  int max_fd = 0;
  if (DIR* dir = opendir("/proc/self/fd")) {
    while (auto* ent = readdir(dir)) max_fd = std::max(max_fd, atoi(ent->d_name));
    closedir(dir);
  }
  rlimit old_lim{};
  ASSERT_EQ(getrlimit(RLIMIT_NOFILE, &old_lim), 0);
  rlimit low_lim = old_lim;
  low_lim.rlim_cur = static_cast<rlim_t>(max_fd + 1);
  ASSERT_EQ(setrlimit(RLIMIT_NOFILE, &low_lim), 0);
  // Take the descriptors still free below the limit (e.g. the one opendir
  // used), so that accept() really has none left.
  std::vector<int> fillers;
  for (int fd; (fd = dup(STDERR_FILENO)) >= 0;) fillers.push_back(fd);
  EXPECT_EQ(errno, EMFILE);

  sock.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port));  // sits in the backlog
  auto cpu_seconds = [] {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e6;
  };
  const double cpu0 = cpu_seconds();
  std::this_thread::sleep_for(500ms);
  const double cpu_used = cpu_seconds() - cpu0;
  ASSERT_EQ(setrlimit(RLIMIT_NOFILE, &old_lim), 0);
  for (int fd : fillers) close(fd);

  // A spinning accept loop burns about the whole 500ms on one core.
  EXPECT_LT(cpu_used, 0.15) << "accept loop spins on EMFILE";

  // Descriptors are back: the pending connection gets accepted and served
  // (the server answers the HTTP/2 preface with its SETTINGS frame).
  static const char kPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
  asio::write(sock, asio::buffer(kPreface, sizeof(kPreface) - 1));
  std::array<char, 9> frame_header{};
  sock.non_blocking(false);
  timeval tv{.tv_sec = 5, .tv_usec = 0};
  setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  boost::system::error_code ec;
  asio::read(sock, asio::buffer(frame_header), ec);
  EXPECT_FALSE(ec) << ec.message();
  EXPECT_EQ(frame_header[3], 0x4) << "expected a SETTINGS frame";  // type octet

  svr->Shutdown();
}

// EXT-003: the server keeps at most max_connection_num connections (default
// 1024 instead of 1000000); a connection over the limit waits in the backlog
// until another one goes away.
TEST(GrpcServerAcceptTest, ConnectionLimitIsEnforced) {
  EXPECT_EQ(server::ServerOptions{}.max_connection_num, 1024u);
  EXPECT_EQ(GrpcExtension::Options{}.max_connection_num, 1024u);
  EXPECT_EQ(GrpcExtension::Options{}.max_concurrent_streams, 100u);

  IoThreads io(2);
  const uint16_t port = FreeTcpPort();
  auto svr = std::make_shared<server::AsioHttp2Server>(io.Io());
  svr->Initialize(server::ServerOptions{.ep = {asio::ip::address_v4::loopback(), port},
                                        .max_connection_num = 2,
                                        .mgr_timer_dt = 100ms});
  svr->Start();
  std::this_thread::sleep_for(100ms);  // listening

  static const char kPreface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
  asio::io_context cio;
  // True if the server answers the preface within the timeout. Plain recv():
  // asio's blocking read polls again after SO_RCVTIMEO expires.
  auto served = [](asio::ip::tcp::socket& sock, int timeout_ms) {
    timeval tv{.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    std::array<char, 9> frame_header{};
    return ::recv(sock.native_handle(), frame_header.data(), frame_header.size(), 0) > 0;
  };
  std::vector<std::unique_ptr<asio::ip::tcp::socket>> socks;
  for (int i = 0; i < 3; ++i) {
    socks.push_back(std::make_unique<asio::ip::tcp::socket>(cio));
    socks.back()->connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), port));
    asio::write(*socks.back(), asio::buffer(kPreface, sizeof(kPreface) - 1));
    std::this_thread::sleep_for(50ms);  // accept in order
  }
  EXPECT_TRUE(served(*socks[0], 2000));
  EXPECT_TRUE(served(*socks[1], 2000));
  EXPECT_FALSE(served(*socks[2], 500)) << "third connection served over the limit";

  socks[0]->close();
  EXPECT_TRUE(served(*socks[2], 5000)) << "waiting connection not served after one closed";

  svr->Shutdown();
}

}  // namespace
}  // namespace ibmw::extensions::grpc_extension
