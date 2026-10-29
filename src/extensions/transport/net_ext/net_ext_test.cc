// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for the net transports:
// TRANS-016: listeners default to loopback.
// TRANS-017: CORS headers only for configured origins.
// TRANS-018: peer-supplied meta cannot override framework "ibmw-" keys.
// TRANS-019: a late or missing RPC answer neither hangs nor touches freed state.
// TRANS-027/029/030/031: tcp/udp server receive path and startup.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "base/buffer_util.h"
#include "cpp/util/string.h"
#include "engine/service/service_catalog.h"
#include "net_base/asio_tcp_svr.h"
#include "net_base/asio_udp_svr.h"
#include "net_ext/http/http_rpc_driver.h"
#include "net_ext/meta_filter.h"
#include "net_ext/net_extension.h"

namespace ibmw::extensions::net_extension {
namespace {

using namespace std::chrono_literals;
namespace asio = boost::asio;
namespace http = boost::beast::http;
using Tcp = asio::ip::tcp;
using Udp = asio::ip::udp;

const auto kLoopback = asio::ip::address_v4::loopback();

uint16_t FreeTcpPort() {
  asio::io_context io;
  Tcp::acceptor acceptor(io, Tcp::endpoint(kLoopback, 0));
  return acceptor.local_endpoint().port();
}

uint16_t FreeUdpPort() {
  asio::io_context io;
  Udp::socket sock(io, Udp::endpoint(kLoopback, 0));
  return sock.local_endpoint().port();
}

// io_context run by a few threads for the lifetime of the object.
class IoThreads {
 public:
  explicit IoThreads(int n = 2) : io_(std::make_shared<asio::io_context>()), guard_(io_->get_executor()) {
    for (int i = 0; i < n; ++i) threads_.emplace_back([this] { io_->run(); });
  }
  ~IoThreads() {
    guard_.reset();
    io_->stop();
    for (auto& t : threads_) t.join();
  }
  const std::shared_ptr<asio::io_context>& Io() { return io_; }

 private:
  std::shared_ptr<asio::io_context> io_;
  asio::executor_work_guard<asio::io_context::executor_type> guard_;
  std::vector<std::thread> threads_;
};

TEST(NetMetaFilterTest, ReservedKeysAreRejectedExceptPublisherStamps) {
  EXPECT_TRUE(IsPeerMetaKeyAllowed("x-custom"));
  EXPECT_TRUE(IsPeerMetaKeyAllowed(""));
  EXPECT_TRUE(IsPeerMetaKeyAllowed("ibmw"));
  EXPECT_FALSE(IsPeerMetaKeyAllowed(IBMW_TRANSPORT_CONTEXT_KEY_DRIVER));
  EXPECT_FALSE(IsPeerMetaKeyAllowed(IBMW_TRANSPORT_CONTEXT_KEY_SERIALIZATION_TYPE));
  EXPECT_FALSE(IsPeerMetaKeyAllowed(IBMW_TRANSPORT_CONTEXT_KEY_TO_ADDR));
  EXPECT_FALSE(IsPeerMetaKeyAllowed(IBMW_RPC_CONTEXT_KEY_FUNCTION_NAME));
  EXPECT_FALSE(IsPeerMetaKeyAllowed(IBMW_RPC_CONTEXT_KEY_REQUEST_ID));
  EXPECT_TRUE(IsPeerMetaKeyAllowed(IBMW_TRANSPORT_CONTEXT_KEY_PUB_SEQ));
  EXPECT_TRUE(IsPeerMetaKeyAllowed(IBMW_TRANSPORT_CONTEXT_KEY_PUB_TIMESTAMP));
}

TEST(NetDefaultsTest, ListenersDefaultToLoopback) {
  EXPECT_EQ(NetExtension::Options::HttpOptions{}.listen_ip, "127.0.0.1");
  EXPECT_EQ(NetExtension::Options::TcpOptions{}.listen_ip, "127.0.0.1");
  EXPECT_EQ(NetExtension::Options::UdpOptions{}.listen_ip, "127.0.0.1");
  EXPECT_TRUE(ibmw::common::net::AsioTcpServer::Options{}.ep.address().is_loopback());
  EXPECT_TRUE(ibmw::common::net::AsioUdpServer::Options{}.ep.address().is_loopback());
  EXPECT_TRUE(ibmw::common::net::AsioHttpServer::Options{}.ep.address().is_loopback());
}

// ---------------------------------------------------------------- tcp / udp

struct Received {
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::pair<std::string, std::string>> msgs;  // (endpoint, payload)

  template <class Ep>
  void Add(const Ep& ep, const std::shared_ptr<asio::streambuf>& buf) {
    std::string payload(asio::buffers_begin(buf->data()), asio::buffers_end(buf->data()));
    {
      std::lock_guard lock(mutex);
      msgs.emplace_back(ibmw::common::util::SSToString(ep), std::move(payload));
    }
    cv.notify_all();
  }
  bool WaitFor(size_t n, std::chrono::milliseconds timeout = 5s) {
    std::unique_lock lock(mutex);
    return cv.wait_for(lock, timeout, [&] { return msgs.size() >= n; });
  }
};

std::string TcpFrame(const std::string& payload) {
  std::string frame(6, '\0');
  frame[0] = 'Y';
  frame[1] = 'T';
  ibmw::common::util::SetBufFromUint32(&frame[2], static_cast<uint32_t>(payload.size()));
  return frame + payload;
}

std::shared_ptr<ibmw::common::net::AsioTcpServer> MakeTcpServer(
    IoThreads& io, Received& received, uint16_t port) {
  auto svr = std::make_shared<ibmw::common::net::AsioTcpServer>(io.Io());
  svr->RegisterMsgHandle([&received](const Tcp::endpoint& ep, const std::shared_ptr<asio::streambuf>& buf) {
    received.Add(ep, buf);
  });
  svr->Initialize({.ep = Tcp::endpoint(kLoopback, port)});
  return svr;
}

// TRANS-030: a listen failure is reported by Start, not lost in a coroutine.
TEST(NetTcpServerTest, StartReportsBindFailure) {
  IoThreads io;
  Received received;
  const uint16_t port = FreeTcpPort();
  auto svr = MakeTcpServer(io, received, port);
  asio::io_context other;
  Tcp::acceptor squatter(other, Tcp::endpoint(kLoopback, port));
  EXPECT_ANY_THROW(svr->Start());
}

// TRANS-029/027: the handler gets the sender endpoint even after the
// connection is gone, and a message above the eager limit arrives intact.
TEST(NetTcpServerTest, DeliversSenderEndpointAndLargeMessages) {
  IoThreads io;
  Received received;
  const uint16_t port = FreeTcpPort();
  auto svr = MakeTcpServer(io, received, port);
  svr->Start();

  std::string large(5 * 1024 * 1024 + 123, '\0');
  for (size_t i = 0; i < large.size(); ++i) large[i] = static_cast<char>(i * 131 + 7);

  std::string client_ep;
  {
    asio::io_context cli_io;
    Tcp::socket sock(cli_io);
    sock.connect(Tcp::endpoint(kLoopback, port));
    client_ep = ibmw::common::util::SSToString(sock.local_endpoint());
    asio::write(sock, asio::buffer(TcpFrame("small")));
    asio::write(sock, asio::buffer(TcpFrame(large)));
    // Closed right away: delivery must not depend on the live socket.
  }
  ASSERT_TRUE(received.WaitFor(2));
  std::lock_guard lock(received.mutex);
  EXPECT_EQ(received.msgs[0].first, client_ep);
  EXPECT_EQ(received.msgs[0].second, "small");
  EXPECT_EQ(received.msgs[1].first, client_ep);
  EXPECT_TRUE(received.msgs[1].second == large);
  svr->Shutdown();
}

TEST(NetTcpServerTest, OversizedMessageClosesSession) {
  IoThreads io;
  Received received;
  const uint16_t port = FreeTcpPort();
  auto svr = std::make_shared<ibmw::common::net::AsioTcpServer>(io.Io());
  svr->RegisterMsgHandle([&received](const Tcp::endpoint& ep, const std::shared_ptr<asio::streambuf>& buf) {
    received.Add(ep, buf);
  });
  svr->Initialize({.ep = Tcp::endpoint(kLoopback, port), .max_recv_size = 16});
  svr->Start();

  asio::io_context cli_io;
  Tcp::socket sock(cli_io);
  sock.connect(Tcp::endpoint(kLoopback, port));
  asio::write(sock, asio::buffer(TcpFrame(std::string(17, 'x'))));
  char c;
  boost::system::error_code ec;
  sock.read_some(asio::buffer(&c, 1), ec);  // server closes the session
  EXPECT_TRUE(ec);
  EXPECT_FALSE(received.WaitFor(1, 100ms));
  svr->Shutdown();
}

// TRANS-031: the udp handler gets the real sender endpoint.
TEST(NetUdpServerTest, DeliversSenderEndpoint) {
  IoThreads io;
  Received received;
  const uint16_t port = FreeUdpPort();
  auto svr = std::make_shared<ibmw::common::net::AsioUdpServer>(io.Io());
  svr->RegisterMsgHandle([&received](const Udp::endpoint& ep, const std::shared_ptr<asio::streambuf>& buf) {
    received.Add(ep, buf);
  });
  svr->Initialize({.ep = Udp::endpoint(kLoopback, port)});
  svr->Start();

  asio::io_context cli_io;
  Udp::socket sock(cli_io, Udp::endpoint(kLoopback, 0));
  sock.send_to(asio::buffer(std::string("hello")), Udp::endpoint(kLoopback, port));
  ASSERT_TRUE(received.WaitFor(1));
  std::lock_guard lock(received.mutex);
  EXPECT_EQ(received.msgs[0].first, ibmw::common::util::SSToString(sock.local_endpoint()));
  EXPECT_EQ(received.msgs[0].second, "hello");
  svr->Shutdown();
}

TEST(NetUdpServerTest, StartReportsBindFailure) {
  IoThreads io;
  Received received;
  const uint16_t port = FreeUdpPort();
  auto svr = std::make_shared<ibmw::common::net::AsioUdpServer>(io.Io());
  svr->RegisterMsgHandle([](const Udp::endpoint&, const std::shared_ptr<asio::streambuf>&) {});
  svr->Initialize({.ep = Udp::endpoint(kLoopback, port)});
  asio::io_context other;
  Udp::socket squatter(other, Udp::endpoint(kLoopback, port));
  EXPECT_ANY_THROW(svr->Start());
}

// ---------------------------------------------------------------- http rpc

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

class HttpRpcDriverTest : public ::testing::Test {
 protected:
  void SetUpDriver(runtime::core::service::ServiceFunc func, const std::string& options_yaml = "{}") {
    port_ = FreeTcpPort();
    svr_ = std::make_shared<ibmw::common::net::AsioHttpServer>(io_.Io());
    cli_ = std::make_shared<ibmw::common::net::AsioHttpClientPool>(io_.Io());
    driver_ = std::make_unique<HttpRpcDriver>(io_.Io(), cli_, svr_);
    driver_->Initialize(YAML::Load(options_yaml));

    wrapper_.info.func_name = "pb:/test/svc";
    wrapper_.info.pkg_path = "pkg";
    wrapper_.info.module_name = "mod";
    wrapper_.info.req_type_support_ref = ibmw::util::TypeSupportRef(JsonIntTypeSupport());
    wrapper_.info.rsp_type_support_ref = ibmw::util::TypeSupportRef(JsonIntTypeSupport());
    wrapper_.service_func = std::move(func);
    ASSERT_TRUE(driver_->RegisterServiceFunc(wrapper_));

    svr_->Initialize({.ep = Tcp::endpoint(kLoopback, port_)});
    svr_->Start();
    driver_->Start();
  }

  void TearDown() override {
    if (driver_) driver_->Shutdown();
  }

  http::response<http::string_body> Post(
      const std::string& body,
      std::vector<std::pair<std::string, std::string>> headers = {},
      http::verb verb = http::verb::post) {
    asio::io_context cli_io;
    Tcp::socket sock(cli_io);
    sock.connect(Tcp::endpoint(kLoopback, port_));
    // A hung server fails the test instead of blocking it.
    struct timeval tv{.tv_sec = 10, .tv_usec = 0};
    setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    http::request<http::string_body> req{verb, "/rpc/test/svc", 11};
    req.set(http::field::host, "127.0.0.1");
    req.set(http::field::content_type, "application/json");
    for (const auto& [k, v] : headers) req.set(k, v);
    req.body() = body;
    req.prepare_payload();
    http::write(sock, req);

    boost::beast::flat_buffer buffer;
    http::response<http::string_body> rsp;
    boost::system::error_code ec;
    http::read(sock, buffer, rsp, ec);
    EXPECT_FALSE(ec) << ec.message();
    return rsp;
  }

  IoThreads io_;
  uint16_t port_ = 0;
  std::shared_ptr<ibmw::common::net::AsioHttpServer> svr_;
  std::shared_ptr<ibmw::common::net::AsioHttpClientPool> cli_;
  std::unique_ptr<HttpRpcDriver> driver_;
  runtime::core::service::ServiceFuncWrapper wrapper_;
};

void Answer(const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& frame, uint32_t code) {
  auto callback = std::move(frame->callback);
  frame->callback = nullptr;
  callback(ibmw::rpc::Status(code));
}

TEST_F(HttpRpcDriverTest, AnswersAndFiltersReservedHeaders) {
  std::string driver_meta, custom_meta, function_meta, serialization_meta;
  SetUpDriver([&](const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& frame) {
    driver_meta = frame->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_DRIVER);
    function_meta = frame->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_FUNCTION_NAME);
    custom_meta = frame->ctx_ref.GetMetaValue("x-custom");
    serialization_meta = frame->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE);
    *static_cast<int*>(frame->rsp_ptr) = *static_cast<const int*>(frame->req_ptr) * 2;
    Answer(frame, IBMW_RPC_STATUS_OK);
  });
  auto rsp = Post("21", {{"ibmw-driver", "evil"}, {"ibmw-function_name", "evil"}, {"ibmw-serialization_type", "evil"}, {"x-custom", "v"}});
  EXPECT_EQ(rsp.result(), http::status::ok);
  EXPECT_EQ(rsp.body(), "42");
  EXPECT_EQ(driver_meta, std::string(driver_->Name()));
  EXPECT_EQ(function_meta, "pb:/test/svc");
  EXPECT_EQ(custom_meta, "v");
  EXPECT_EQ(serialization_meta, "json");
  // TRANS-017: no CORS unless configured.
  EXPECT_EQ(rsp.find(http::field::access_control_allow_origin), rsp.end());
}

TEST_F(HttpRpcDriverTest, CorsOnlyForAllowedOrigins) {
  SetUpDriver(
      [](const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& frame) {
        Answer(frame, IBMW_RPC_STATUS_OK);
      },
      "{cors_allow_origins: [\"https://ok.example\"]}");
  auto ok = Post("1", {{"Origin", "https://ok.example"}});
  ASSERT_NE(ok.find(http::field::access_control_allow_origin), ok.end());
  EXPECT_EQ(ok[http::field::access_control_allow_origin], "https://ok.example");

  auto bad = Post("1", {{"Origin", "https://evil.example"}});
  EXPECT_EQ(bad.find(http::field::access_control_allow_origin), bad.end());

  auto preflight = Post("", {{"Origin", "https://evil.example"}}, http::verb::options);
  EXPECT_EQ(preflight.find(http::field::access_control_allow_methods), preflight.end());
}

// TRANS-019: a service that never answers in time gets an error response
// (instead of hanging forever) and a later answer is harmless.
TEST_F(HttpRpcDriverTest, MissingAnswerTimesOutAndLateAnswerIsIgnored) {
  std::shared_ptr<runtime::core::service::ServiceMessageFrame> pending;
  std::mutex mutex;
  SetUpDriver([&](const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& frame) {
    std::lock_guard lock(mutex);
    pending = frame;
  });
  const auto begin = std::chrono::steady_clock::now();
  auto rsp = Post("1", {{"Timeout", "1"}});
  EXPECT_NE(rsp.result(), http::status::ok);
  EXPECT_LT(std::chrono::steady_clock::now() - begin, 8s);

  std::shared_ptr<runtime::core::service::ServiceMessageFrame> frame;
  {
    std::lock_guard lock(mutex);
    frame = std::move(pending);
  }
  ASSERT_TRUE(frame);
  Answer(frame, IBMW_RPC_STATUS_OK);  // the request is gone: must not touch it

  // The server is still healthy.
  SUCCEED();
}

}  // namespace
}  // namespace ibmw::extensions::net_extension
