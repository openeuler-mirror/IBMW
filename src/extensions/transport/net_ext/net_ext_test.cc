// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for the net transports:
// TRANS-016: listeners default to loopback.
// TRANS-017: CORS headers only for configured origins (also on errors, Vary).
// TRANS-018: peer-supplied meta cannot override framework "ibmw-" keys.
// TRANS-019: a late or missing RPC answer neither hangs nor touches freed state.
// TRANS-027/029/030/031: tcp/udp server receive path and startup.
// TRANS-028: bounded tcp send queue.
// TRANS-032: a full udp session table does not stop the receive loop.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#include "base/buffer_util.h"
#include "base/url_encode.h"
#include "cpp/util/string.h"
#include "engine/service/service_catalog.h"
#include "net_base/asio_endpoint_parse.h"
#include "net_base/asio_tcp_cli.h"
#include "net_base/asio_tcp_svr.h"
#include "net_base/asio_tools.h"
#include "net_base/asio_udp_cli.h"
#include "net_base/asio_udp_svr.h"
#include "net_base/http_media_type.h"
#include "net_ext/global.h"
#include "net_ext/http/http_rpc_driver.h"
#include "net_ext/http/http_transport_driver.h"
#include "net_ext/meta_filter.h"
#include "net_ext/net_extension.h"
#include "net_ext/tcp/tcp_transport_driver.h"
#include "net_ext/udp/udp_transport_driver.h"

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

TEST(NetMetaFilterTest, ReservedKeysAreRejectedExceptSenderStamps) {
  EXPECT_TRUE(IsPeerMetaKeyAllowed("x-custom"));
  EXPECT_TRUE(IsPeerMetaKeyAllowed(""));
  EXPECT_TRUE(IsPeerMetaKeyAllowed("ibmw"));
  EXPECT_FALSE(IsPeerMetaKeyAllowed(IBMW_TRANSPORT_CONTEXT_KEY_DRIVER));
  EXPECT_FALSE(IsPeerMetaKeyAllowed(IBMW_TRANSPORT_CONTEXT_KEY_SERIALIZATION_TYPE));
  EXPECT_FALSE(IsPeerMetaKeyAllowed(IBMW_TRANSPORT_CONTEXT_KEY_TO_ADDR));
  EXPECT_FALSE(IsPeerMetaKeyAllowed(IBMW_RPC_CONTEXT_KEY_FUNCTION_NAME));
  EXPECT_TRUE(IsPeerMetaKeyAllowed(IBMW_RPC_CONTEXT_KEY_REQUEST_ID));
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

// TRANS-044: a missing required option is named in the error.
TEST(NetOptionsTest, MissingRequiredOptionIsNamed) {
  const auto expect_error = [](const char* yaml, const char* option) {
    try {
      NetExtension::ParseOptions(YAML::Load(yaml));
      ADD_FAILURE() << yaml;
    } catch (const std::exception& e) {
      EXPECT_NE(std::string_view(e.what()).find(option), std::string_view::npos) << e.what();
    }
  };
  expect_error("{http_options: {listen_port: 1}}", "thread_num");
  expect_error("{thread_num: 1, http_options: {}}", "http_options.listen_port");
  expect_error("{thread_num: 1, tcp_options: {listen_ip: 127.0.0.1}}", "tcp_options.listen_port");
  expect_error("{thread_num: 1, udp_options: {}}", "udp_options.listen_port");

  const auto options = NetExtension::ParseOptions(YAML::Load("{thread_num: 3, tcp_options: {listen_port: 7}}"));
  EXPECT_EQ(options.thread_num, 3u);
  ASSERT_TRUE(options.tcp_options);
  EXPECT_EQ(options.tcp_options->listen_port, 7);
  EXPECT_FALSE(options.http_options);
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

// TRANS-056: the address is checked by the real bind in Start, not by a trial
// bind in Initialize, so a failed Start can be retried once the port is free.
TEST(NetTcpServerTest, InitializeDoesNotProbeAndStartCanRetry) {
  IoThreads io;
  Received received;
  const uint16_t port = FreeTcpPort();
  asio::io_context other;
  auto squatter = std::make_unique<Tcp::acceptor>(other, Tcp::endpoint(kLoopback, port));
  std::shared_ptr<ibmw::common::net::AsioTcpServer> svr;
  ASSERT_NO_THROW(svr = MakeTcpServer(io, received, port));
  EXPECT_ANY_THROW(svr->Start());
  squatter.reset();
  EXPECT_NO_THROW(svr->Start());
  svr->Shutdown();
}

TEST(NetHttpServerTest, InitializeDoesNotProbeAndStartCanRetry) {
  IoThreads io;
  const uint16_t port = FreeTcpPort();
  asio::io_context other;
  auto squatter = std::make_unique<Tcp::acceptor>(other, Tcp::endpoint(kLoopback, port));
  auto svr = std::make_shared<ibmw::common::net::AsioHttpServer>(io.Io());
  ASSERT_NO_THROW(svr->Initialize({.ep = Tcp::endpoint(kLoopback, port)}));
  EXPECT_ANY_THROW(svr->Start());
  squatter.reset();
  EXPECT_NO_THROW(svr->Start());
  svr->Shutdown();
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
  bool closed = false;
  sock.async_read_some(asio::buffer(&c, 1), [&](const boost::system::error_code& e, size_t) {
    ec = e;
    closed = true;
  });
  cli_io.run_for(10s);  // the server closes the session
  ASSERT_TRUE(closed) << "server kept the session open";
  EXPECT_TRUE(ec);
  EXPECT_FALSE(received.WaitFor(1, 100ms));
  svr->Shutdown();
}

// Reads from `sock` until nothing arrives for a while; returns the bytes read.
size_t DrainSocket(Tcp::socket& sock) {
  sock.non_blocking(true);
  std::vector<char> chunk(1024 * 1024);
  size_t bytes = 0;
  for (auto idle_since = std::chrono::steady_clock::now(); std::chrono::steady_clock::now() - idle_since < 1s;) {
    boost::system::error_code ec;
    const size_t n = sock.read_some(asio::buffer(chunk), ec);
    if (n > 0) {
      bytes += n;
      idle_since = std::chrono::steady_clock::now();
    } else if (ec == asio::error::would_block) {
      std::this_thread::sleep_for(5ms);
    } else {
      break;
    }
  }
  return bytes;
}

// Bytes the loopback socket buffers absorb towards a peer that does not read
// (receive buffer 4096 as in the tests below), measured like the sessions
// write: until the kernel stops taking data.
size_t KernelBufferedBytes() {
  asio::io_context io;
  Tcp::acceptor acceptor(io, Tcp::endpoint(kLoopback, 0));
  acceptor.set_option(asio::socket_base::receive_buffer_size(4096));
  Tcp::socket sender(io);
  sender.connect(acceptor.local_endpoint());
  Tcp::socket receiver = acceptor.accept();
  sender.non_blocking(true);
  const std::string chunk(64 * 1024, 'k');
  size_t bytes = 0;
  for (auto idle_since = std::chrono::steady_clock::now(); std::chrono::steady_clock::now() - idle_since < 300ms;) {
    boost::system::error_code ec;
    const size_t n = sender.write_some(asio::buffer(chunk), ec);
    if (n > 0) {
      bytes += n;
      idle_since = std::chrono::steady_clock::now();
    } else {
      std::this_thread::sleep_for(5ms);
    }
  }
  return bytes;
}

constexpr size_t kSendQueueLimit = 4 * 1024 * 1024;

// TRANS-028: sending to a peer that does not read is bounded: messages
// beyond max_send_queue_size are dropped instead of queued without limit.
// The limit covers the batch being written as well: at most the limit (and
// one message accepted into an empty queue) is held besides the kernel buffers.
TEST(NetTcpServerTest, SendQueueToSlowPeerIsBounded) {
  IoThreads io;
  Received received;
  const uint16_t port = FreeTcpPort();
  auto svr = std::make_shared<ibmw::common::net::AsioTcpServer>(io.Io());
  svr->RegisterMsgHandle([&received](const Tcp::endpoint& ep, const std::shared_ptr<asio::streambuf>& buf) {
    received.Add(ep, buf);
  });
  svr->Initialize({.ep = Tcp::endpoint(kLoopback, port), .max_send_queue_size = kSendQueueLimit});
  svr->Start();

  asio::io_context cli_io;
  Tcp::socket sock(cli_io);
  sock.open(Tcp::v4());
  sock.set_option(asio::socket_base::receive_buffer_size(4096));
  sock.connect(Tcp::endpoint(kLoopback, port));
  asio::write(sock, asio::buffer(TcpFrame("hi")));  // makes the session known
  ASSERT_TRUE(received.WaitFor(1));

  // 64 MiB to a peer not reading: far more than the socket buffers hold.
  constexpr size_t kMsgSize = 64 * 1024;
  constexpr size_t kMsgNum = 1000;
  const Tcp::endpoint peer = sock.local_endpoint();
  for (size_t i = 0; i < kMsgNum; ++i) {
    auto buf = std::make_shared<asio::streambuf>();
    buf->commit(asio::buffer_copy(buf->prepare(kMsgSize), asio::buffer(std::string(kMsgSize, 'd'))));
    svr->SendMsg(peer, buf);
  }
  std::this_thread::sleep_for(300ms);

  const size_t delivered = DrainSocket(sock) / (6 + kMsgSize);
  EXPECT_GE(delivered, kSendQueueLimit / kMsgSize);  // at least the queue limit got through
  EXPECT_LE(delivered, KernelBufferedBytes() / kMsgSize + kSendQueueLimit / kMsgSize + 8)
      << "more than the send queue limit was held";
  svr->Shutdown();
}

// TRANS-028: same bound on the client side.
TEST(NetTcpClientTest, SendQueueToSlowPeerIsBounded) {
  IoThreads io;
  const uint16_t port = FreeTcpPort();
  asio::io_context svr_io;
  Tcp::acceptor acceptor(svr_io);
  acceptor.open(Tcp::v4());
  acceptor.set_option(asio::socket_base::receive_buffer_size(4096));  // inherited by accepted sockets
  acceptor.bind(Tcp::endpoint(kLoopback, port));
  acceptor.listen();

  auto cli = std::make_shared<ibmw::common::net::AsioTcpClient>(io.Io());
  cli->Initialize({.svr_ep = Tcp::endpoint(kLoopback, port), .max_send_queue_size = kSendQueueLimit});
  cli->Start();

  constexpr size_t kMsgSize = 64 * 1024;
  constexpr size_t kMsgNum = 1000;
  for (size_t i = 0; i < kMsgNum; ++i) {
    auto buf = std::make_shared<asio::streambuf>();
    buf->commit(asio::buffer_copy(buf->prepare(kMsgSize), asio::buffer(std::string(kMsgSize, 'd'))));
    cli->SendMsg(buf);
  }
  Tcp::socket sock = acceptor.accept();
  std::this_thread::sleep_for(300ms);

  const size_t delivered = DrainSocket(sock) / (6 + kMsgSize);
  EXPECT_GE(delivered, kSendQueueLimit / kMsgSize);
  EXPECT_LE(delivered, KernelBufferedBytes() / kMsgSize + kSendQueueLimit / kMsgSize + 8)
      << "more than the send queue limit was held";
  cli->Shutdown();
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

// TRANS-032: a full session table drops new endpoints but keeps serving
// the known ones (it used to stop receiving altogether).
TEST(NetUdpServerTest, FullSessionTableDropsOnlyNewEndpoints) {
  IoThreads io;
  Received received;
  const uint16_t port = FreeUdpPort();
  auto svr = std::make_shared<ibmw::common::net::AsioUdpServer>(io.Io());
  svr->RegisterMsgHandle([&received](const Udp::endpoint& ep, const std::shared_ptr<asio::streambuf>& buf) {
    received.Add(ep, buf);
  });
  svr->Initialize({.ep = Udp::endpoint(kLoopback, port), .max_session_num = 1});
  svr->Start();

  asio::io_context cli_io;
  Udp::socket known(cli_io, Udp::endpoint(kLoopback, 0));
  Udp::socket other(cli_io, Udp::endpoint(kLoopback, 0));
  known.send_to(asio::buffer(std::string("a1")), Udp::endpoint(kLoopback, port));
  ASSERT_TRUE(received.WaitFor(1));
  other.send_to(asio::buffer(std::string("b1")), Udp::endpoint(kLoopback, port));
  known.send_to(asio::buffer(std::string("a2")), Udp::endpoint(kLoopback, port));
  ASSERT_TRUE(received.WaitFor(2, 2s)) << "known endpoint not served while the table is full";
  EXPECT_FALSE(received.WaitFor(3, 200ms));
  std::lock_guard lock(received.mutex);
  EXPECT_EQ(received.msgs[1].second, "a2");
  svr->Shutdown();
}

// TRANS-033: a datagram longer than max_package_size used to be delivered
// silently truncated; it is now dropped. The sender drops oversized messages.
TEST(NetUdpServerTest, OversizedDatagramIsDroppedNotTruncated) {
  IoThreads io;
  Received received;
  const uint16_t port = FreeUdpPort();
  auto svr = std::make_shared<ibmw::common::net::AsioUdpServer>(io.Io());
  svr->RegisterMsgHandle([&received](const Udp::endpoint& ep, const std::shared_ptr<asio::streambuf>& buf) {
    received.Add(ep, buf);
  });
  svr->Initialize({.ep = Udp::endpoint(kLoopback, port), .max_package_size = 8});
  svr->Start();

  asio::io_context cli_io;
  Udp::socket sock(cli_io, Udp::endpoint(kLoopback, 0));
  sock.send_to(asio::buffer(std::string("123456789")), Udp::endpoint(kLoopback, port));
  sock.send_to(asio::buffer(std::string("12345678")), Udp::endpoint(kLoopback, port));
  ASSERT_TRUE(received.WaitFor(1));
  EXPECT_FALSE(received.WaitFor(2, 200ms));
  std::lock_guard lock(received.mutex);
  EXPECT_EQ(received.msgs[0].second, "12345678");
  svr->Shutdown();
}

// TRANS-035: the client socket follows the address family of the server.
TEST(NetUdpClientTest, SendsToIpv6Server) {
  IoThreads io;
  Received received;
  const auto v6_loopback = asio::ip::address_v6::loopback();
  uint16_t port = 0;
  {
    asio::io_context probe;
    port = Udp::socket(probe, Udp::endpoint(v6_loopback, 0)).local_endpoint().port();
  }
  auto svr = std::make_shared<ibmw::common::net::AsioUdpServer>(io.Io());
  svr->RegisterMsgHandle([&received](const Udp::endpoint& ep, const std::shared_ptr<asio::streambuf>& buf) {
    received.Add(ep, buf);
  });
  svr->Initialize({.ep = Udp::endpoint(v6_loopback, port)});
  svr->Start();

  auto cli = std::make_shared<ibmw::common::net::AsioUdpClient>(io.Io());
  cli->Initialize({.svr_ep = Udp::endpoint(v6_loopback, port)});
  cli->Start();
  auto buf = std::make_shared<asio::streambuf>();
  std::ostream(buf.get()) << "v6";
  cli->SendMsg(buf);
  ASSERT_TRUE(received.WaitFor(1));
  std::lock_guard lock(received.mutex);
  EXPECT_EQ(received.msgs[0].second, "v6");
  cli->Shutdown();
  svr->Shutdown();
}

TEST(NetUdpClientTest, OversizedMessageIsNotSent) {
  IoThreads io;
  Received received;
  const uint16_t port = FreeUdpPort();
  auto svr = std::make_shared<ibmw::common::net::AsioUdpServer>(io.Io());
  svr->RegisterMsgHandle([&received](const Udp::endpoint& ep, const std::shared_ptr<asio::streambuf>& buf) {
    received.Add(ep, buf);
  });
  svr->Initialize({.ep = Udp::endpoint(kLoopback, port)});
  svr->Start();

  auto cli = std::make_shared<ibmw::common::net::AsioUdpClient>(io.Io());
  cli->Initialize({.svr_ep = Udp::endpoint(kLoopback, port), .max_package_size = 4});
  cli->Start();
  auto make = [](const std::string& text) {
    auto buf = std::make_shared<asio::streambuf>();
    std::ostream(buf.get()) << text;
    return buf;
  };
  cli->SendMsg(make("too-long"));
  cli->SendMsg(make("ok"));
  ASSERT_TRUE(received.WaitFor(1));
  EXPECT_FALSE(received.WaitFor(2, 200ms));
  std::lock_guard lock(received.mutex);
  EXPECT_EQ(received.msgs[0].second, "ok");
  cli->Shutdown();
  svr->Shutdown();
}

TEST(NetEndpointParseTest, AcceptsIpv4AndBracketedIpv6) {
  using ibmw::common::net::ParseIpEndpoint;
  auto v4 = ParseIpEndpoint<Tcp>("127.0.0.1:9000");
  ASSERT_TRUE(v4);
  EXPECT_EQ(v4->address().to_string(), "127.0.0.1");
  EXPECT_EQ(v4->port(), 9000);

  auto v6 = ParseIpEndpoint<Udp>("[::1]:65535");
  ASSERT_TRUE(v6);
  EXPECT_TRUE(v6->address().is_v6());
  EXPECT_EQ(v6->port(), 65535);
}

TEST(NetEndpointParseTest, RejectsMalformedInputWithoutThrowing) {
  using ibmw::common::net::ParseIpEndpoint;
  for (std::string_view bad : {"", "127.0.0.1", "127.0.0.1:", ":80", "127.0.0.1:abc", "127.0.0.1:80x",
                               "127.0.0.1:-1", "127.0.0.1:0", "127.0.0.1:65536", "127.0.0.1:99999999999",
                               "localhost:80", "::1:80", "[::1]", "[::1]80", "[::1:80", "[]:80",
                               "300.0.0.1:80"}) {
    EXPECT_FALSE(ParseIpEndpoint<Tcp>(bad)) << bad;
  }
}

TEST(NetEndpointParseTest, ExtractsAuthorityFromUrl) {
  using ibmw::common::net::ExtractUrlAuthority;
  EXPECT_EQ(ExtractUrlAuthority("127.0.0.1:80"), "127.0.0.1:80");
  EXPECT_EQ(ExtractUrlAuthority("tcp://127.0.0.1:80/p?q#f"), "127.0.0.1:80");
  EXPECT_EQ(ExtractUrlAuthority("[::1]:80?x"), "[::1]:80");
}

// Registers one publisher whose server_url_list holds `url`; returns the result.
template <typename Driver>
bool RegisterWithServerUrl(const std::string& url) {
  Driver driver(nullptr, nullptr, nullptr, nullptr);
  driver.Initialize(YAML::Load("publishers: [{topic: /t, server_url_list: ['" + url + "']}]"));
  runtime::core::transport::SendTypeWrapper wrapper;
  wrapper.info.topic_name = "/t";
  wrapper.info.msg_type = "pb:test.Msg";
  return driver.RegisterSendType(wrapper);
}

TEST(NetDriverServerUrlTest, MalformedServerUrlFailsRegistration) {
  for (const char* bad : {"127.0.0.1", "127.0.0.1:abc", "127.0.0.1:70000", "not-an-ip:80"}) {
    EXPECT_FALSE(RegisterWithServerUrl<TcpTransportDriver>(bad)) << bad;
    EXPECT_FALSE(RegisterWithServerUrl<UdpTransportDriver>(bad)) << bad;
  }
}

TEST(NetDriverServerUrlTest, ValidServerUrlRegisters) {
  for (const char* good : {"127.0.0.1:9000", "[::1]:9000"}) {
    EXPECT_TRUE(RegisterWithServerUrl<TcpTransportDriver>(good)) << good;
    EXPECT_TRUE(RegisterWithServerUrl<UdpTransportDriver>(good)) << good;
  }
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

  // One keep-alive connection; every exchange has a deadline, so a hung
  // server fails the test instead of blocking it.
  class Client {
   public:
    explicit Client(uint16_t port) : sock_(io_) { sock_.connect(Tcp::endpoint(kLoopback, port)); }

    std::optional<http::response<http::string_body>> Send(
        const std::string& body,
        const std::vector<std::pair<std::string, std::string>>& headers = {},
        http::verb verb = http::verb::post) {
      http::request<http::string_body> req{verb, "/rpc/test/svc", 11};
      req.set(http::field::host, "127.0.0.1");
      req.set(http::field::content_type, "application/json");
      req.keep_alive(true);
      for (const auto& [k, v] : headers) req.set(k, v);
      req.body() = body;
      req.prepare_payload();

      http::response<http::string_body> rsp;
      boost::system::error_code ec;
      bool done = false;
      http::async_write(sock_, req, [&](const boost::system::error_code& e, size_t) {
        if (e) {
          ec = e;
          done = true;
          return;
        }
        http::async_read(sock_, buffer_, rsp, [&](const boost::system::error_code& e2, size_t) {
          ec = e2;
          done = true;
        });
      });
      io_.restart();
      io_.run_for(10s);
      if (!done || ec) {
        ADD_FAILURE() << (done ? ec.message() : "no answer within 10s");
        return std::nullopt;
      }
      return rsp;
    }

   private:
    asio::io_context io_;
    Tcp::socket sock_;
    boost::beast::flat_buffer buffer_;
  };

  http::response<http::string_body> Post(
      const std::string& body,
      std::vector<std::pair<std::string, std::string>> headers = {},
      http::verb verb = http::verb::post) {
    Client client(port_);
    auto rsp = client.Send(body, headers, verb);
    return rsp ? std::move(*rsp) : http::response<http::string_body>{};
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
  std::string driver_meta, custom_meta, function_meta, serialization_meta, request_id_meta;
  SetUpDriver([&](const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& frame) {
    driver_meta = frame->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_DRIVER);
    function_meta = frame->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_FUNCTION_NAME);
    custom_meta = frame->ctx_ref.GetMetaValue("x-custom");
    serialization_meta = frame->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE);
    request_id_meta = frame->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_REQUEST_ID);
    *static_cast<int*>(frame->rsp_ptr) = *static_cast<const int*>(frame->req_ptr) * 2;
    Answer(frame, IBMW_RPC_STATUS_OK);
  });
  auto rsp = Post("21", {{"ibmw-driver", "evil"}, {"ibmw-function_name", "evil"}, {"ibmw-serialization_type", "evil"}, {"x-custom", "v"},
                   {"ibmw-request_id", "123"}});
  EXPECT_EQ(rsp.result(), http::status::ok);
  EXPECT_EQ(rsp.body(), "42");
  EXPECT_EQ(driver_meta, std::string(driver_->Name()));
  EXPECT_EQ(function_meta, "pb:/test/svc");
  EXPECT_EQ(custom_meta, "v");
  EXPECT_EQ(serialization_meta, "json");
  EXPECT_EQ(request_id_meta, "123");  // correlation stamp passes through
  // TRANS-017: no CORS unless configured.
  EXPECT_EQ(rsp.find(http::field::access_control_allow_origin), rsp.end());
}

TEST(HttpMediaTypeTest, IgnoresParametersAndCase) {
  using ibmw::common::net::HttpMediaTypeIs;
  EXPECT_TRUE(HttpMediaTypeIs("application/json", "application/json"));
  EXPECT_TRUE(HttpMediaTypeIs("application/json; charset=utf-8", "application/json"));
  EXPECT_TRUE(HttpMediaTypeIs("application/json;charset=UTF-8", "application/json"));
  EXPECT_TRUE(HttpMediaTypeIs("Application/JSON", "application/json"));
  EXPECT_TRUE(HttpMediaTypeIs(" application/cdr ", "application/cdr"));
  EXPECT_TRUE(HttpMediaTypeIs("application/json charset=utf-8", "application/json"));  // legacy form
  EXPECT_FALSE(HttpMediaTypeIs("application/jsonx", "application/json"));
  EXPECT_FALSE(HttpMediaTypeIs("text/json", "application/json"));
  EXPECT_FALSE(HttpMediaTypeIs("", "application/json"));
}

TEST_F(HttpRpcDriverTest, AcceptsStandardJsonContentTypeWithCharset) {
  SetUpDriver([](const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& frame) {
    *static_cast<int*>(frame->rsp_ptr) = *static_cast<const int*>(frame->req_ptr) + 1;
    Answer(frame, IBMW_RPC_STATUS_OK);
  });
  for (const char* ct : {"application/json; charset=utf-8", "Application/JSON"}) {
    auto rsp = Post("1", {{"Content-Type", ct}});
    EXPECT_EQ(rsp.result(), http::status::ok) << ct;
    EXPECT_EQ(rsp.body(), "2") << ct;
  }
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

// The answer depends on the origin also when it is rejected (Vary for
// caches), and error responses keep the CORS headers so that the browser
// can read them.
TEST_F(HttpRpcDriverTest, CorsVaryOnRejectAndHeadersOnErrors) {
  std::shared_ptr<runtime::core::service::ServiceMessageFrame> pending;
  std::mutex mutex;
  SetUpDriver(
      [&](const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& frame) {
        std::lock_guard lock(mutex);
        if (*static_cast<const int*>(frame->req_ptr) == 0) {
          pending = frame;  // never answered in time
        } else {
          Answer(frame, IBMW_RPC_STATUS_OK);
        }
      },
      "{cors_allow_origins: [\"https://ok.example\"]}");
  auto bad = Post("1", {{"Origin", "https://evil.example"}});
  EXPECT_EQ(bad.find(http::field::access_control_allow_origin), bad.end());
  EXPECT_EQ(bad[http::field::vary], "Origin");

  auto no_origin = Post("1", {});
  EXPECT_EQ(no_origin.find(http::field::access_control_allow_origin), no_origin.end());
  EXPECT_EQ(no_origin[http::field::vary], "Origin");

  auto error = Post("0", {{"Origin", "https://ok.example"}, {"Timeout", "1"}});
  EXPECT_EQ(error.result(), http::status::internal_server_error);
  EXPECT_EQ(error[http::field::access_control_allow_origin], "https://ok.example");
  EXPECT_EQ(error[http::field::vary], "Origin");

  std::lock_guard lock(mutex);
  if (pending) Answer(pending, IBMW_RPC_STATUS_OK);
}

TEST_F(HttpRpcDriverTest, CorsAcceptsScalarWildcard) {
  SetUpDriver(
      [](const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& frame) {
        Answer(frame, IBMW_RPC_STATUS_OK);
      },
      "{cors_allow_origins: \"*\"}");
  auto rsp = Post("1", {{"Origin", "https://any.example"}});
  EXPECT_EQ(rsp[http::field::access_control_allow_origin], "*");
  EXPECT_EQ(rsp.find(http::field::vary), rsp.end());
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

  // The server is still healthy: the next request is answered (here by
  // timeout again, since the service never answers in time).
  auto next = Post("1", {{"Timeout", "1"}});
  EXPECT_EQ(next.result(), http::status::internal_server_error);

  // An unanswered frame keeps itself alive through its callback.
  std::lock_guard lock(mutex);
  if (pending) Answer(pending, IBMW_RPC_STATUS_OK);
}

// Answers given inline, before the handler waits, must never be lost.
TEST_F(HttpRpcDriverTest, InlineAnswersAreNeverLost) {
  SetUpDriver([](const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& frame) {
    *static_cast<int*>(frame->rsp_ptr) = *static_cast<const int*>(frame->req_ptr) + 1;
    Answer(frame, IBMW_RPC_STATUS_OK);
  });
  Client client(port_);
  for (int i = 0; i < 2000; ++i) {
    auto rsp = client.Send(std::to_string(i));
    ASSERT_TRUE(rsp) << "request " << i;
    ASSERT_EQ(rsp->body(), std::to_string(i + 1)) << "request " << i;
  }
}

// Answers from another thread race the handler's suspension.
TEST_F(HttpRpcDriverTest, CrossThreadAnswersAreNeverLost) {
  IoThreads answerer(2);
  SetUpDriver([&answerer](const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& frame) {
    asio::post(*answerer.Io(), [frame] {
      *static_cast<int*>(frame->rsp_ptr) = *static_cast<const int*>(frame->req_ptr) + 1;
      Answer(frame, IBMW_RPC_STATUS_OK);
    });
  });
  Client client(port_);
  for (int i = 0; i < 2000; ++i) {
    auto rsp = client.Send(std::to_string(i));
    ASSERT_TRUE(rsp) << "request " << i;
    ASSERT_EQ(rsp->body(), std::to_string(i + 1)) << "request " << i;
  }
}

// "Timeout: 0" (a sub-second client timeout rounded down by older clients)
// means "not set": an asynchronous answer still arrives.
TEST_F(HttpRpcDriverTest, ZeroTimeoutHeaderDoesNotFailAsyncAnswers) {
  IoThreads answerer(1);
  SetUpDriver([&answerer](const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& frame) {
    auto timer = std::make_shared<asio::steady_timer>(*answerer.Io(), 50ms);
    timer->async_wait([frame, timer](const boost::system::error_code&) {
      *static_cast<int*>(frame->rsp_ptr) = 7;
      Answer(frame, IBMW_RPC_STATUS_OK);
    });
  });
  auto rsp = Post("1", {{"Timeout", "0"}});
  EXPECT_EQ(rsp.result(), http::status::ok);
  EXPECT_EQ(rsp.body(), "7");
}

// TRANS-039: a failed call tells the client why it failed.
TEST(HttpRpcStatusTest, HeaderWinsThenHttpStatusIsMapped) {
  EXPECT_EQ(HttpRpcDriver::RpcStatusFromHttp(500, "1008"), IBMW_RPC_STATUS_SVR_HANDLE_FAILED);
  EXPECT_EQ(HttpRpcDriver::RpcStatusFromHttp(500, "2"), IBMW_RPC_STATUS_TIMEOUT);
  EXPECT_EQ(HttpRpcDriver::RpcStatusFromHttp(500, "77"), 77u);  // user-defined code
  EXPECT_EQ(HttpRpcDriver::RpcStatusFromHttp(404, ""), IBMW_RPC_STATUS_SVR_NOT_FOUND);
  EXPECT_EQ(HttpRpcDriver::RpcStatusFromHttp(504, ""), IBMW_RPC_STATUS_TIMEOUT);
  EXPECT_EQ(HttpRpcDriver::RpcStatusFromHttp(408, ""), IBMW_RPC_STATUS_TIMEOUT);
  EXPECT_EQ(HttpRpcDriver::RpcStatusFromHttp(500, ""), IBMW_RPC_STATUS_SVR_HANDLE_FAILED);
  EXPECT_EQ(HttpRpcDriver::RpcStatusFromHttp(501, ""), IBMW_RPC_STATUS_SVR_NOT_IMPLEMENTED);
  EXPECT_EQ(HttpRpcDriver::RpcStatusFromHttp(503, ""), IBMW_RPC_STATUS_SVR_NOT_READY);
  EXPECT_EQ(HttpRpcDriver::RpcStatusFromHttp(400, ""), IBMW_RPC_STATUS_SVR_UNKNOWN);
  // A malformed or zero header falls back to the HTTP status.
  for (std::string_view bad : {"0", "x", "12x", "-1", "99999999999"}) {
    EXPECT_EQ(HttpRpcDriver::RpcStatusFromHttp(500, bad), IBMW_RPC_STATUS_SVR_HANDLE_FAILED) << bad;
  }
}

TEST_F(HttpRpcDriverTest, FailedCallsCarryRpcStatusHeader) {
  SetUpDriver([](const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& frame) {
    Answer(frame, *static_cast<const int*>(frame->req_ptr) == 1 ? 77 : IBMW_RPC_STATUS_SVR_HANDLE_FAILED);
  });
  auto status_of = [](const http::response<http::string_body>& rsp) {
    auto itr = rsp.find(HttpRpcDriver::kRpcStatusHeader);
    return itr == rsp.end() ? std::string() : std::string(itr->value());
  };

  auto user_code = Post("1");
  EXPECT_EQ(user_code.result(), http::status::internal_server_error);
  EXPECT_EQ(status_of(user_code), "77");

  auto handle_failed = Post("2");
  EXPECT_EQ(handle_failed.result(), http::status::internal_server_error);
  EXPECT_EQ(status_of(handle_failed), std::to_string(IBMW_RPC_STATUS_SVR_HANDLE_FAILED));

  auto bad_body = Post("not a number");
  EXPECT_EQ(bad_body.result(), http::status::internal_server_error);
  EXPECT_EQ(status_of(bad_body), std::to_string(IBMW_RPC_STATUS_SVR_DESERIALIZATION_FAILED));

  auto bad_type = Post("1", {{"Content-Type", "text/plain"}});
  EXPECT_EQ(bad_type.result(), http::status::internal_server_error);
  EXPECT_EQ(status_of(bad_type), std::to_string(IBMW_RPC_STATUS_SVR_INVALID_DESERIALIZATION_TYPE));
}

// The client reports the server's status instead of SVR_NOT_FOUND.
TEST_F(HttpRpcDriverTest, ClientCallReportsServerStatus) {
  SetUpDriver([](const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& frame) {
    Answer(frame, 77);
  });
  cli_->Initialize({});
  cli_->Start();

  auto ctx = std::make_shared<ibmw::rpc::Context>(ibmw_rpc_context_type_t::IBMW_RPC_CLIENT_CONTEXT);
  ctx->SetMetaValue(IBMW_RPC_CONTEXT_KEY_TO_ADDR, "http://127.0.0.1:" + std::to_string(port_));
  ctx->SetMetaValue(IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE, "json");
  ctx->SetTimeout(std::chrono::seconds(5));
  int req = 1;
  int rsp = 0;
  std::promise<uint32_t> done;
  auto frame = std::make_shared<runtime::core::service::ServiceMessageFrame>(
      runtime::core::service::ServiceMessageFrame{
          .info = wrapper_.info,
          .req_ptr = &req,
          .rsp_ptr = &rsp,
          .ctx_ref = ctx,
          .callback = [&done](ibmw::rpc::Status status) { done.set_value(status.Code()); }});
  driver_->Call(frame);
  auto result = done.get_future();
  ASSERT_EQ(result.wait_for(10s), std::future_status::ready);
  EXPECT_EQ(result.get(), 77u);
}

TEST_F(HttpRpcDriverTest, ServerTimeoutCarriesTimeoutStatus) {
  std::shared_ptr<runtime::core::service::ServiceMessageFrame> pending;
  std::mutex mutex;
  SetUpDriver([&](const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& frame) {
    std::lock_guard lock(mutex);
    pending = frame;
  });
  auto rsp = Post("1", {{"Timeout", "1"}});
  EXPECT_EQ(rsp.result(), http::status::internal_server_error);
  auto itr = rsp.find(HttpRpcDriver::kRpcStatusHeader);
  ASSERT_NE(itr, rsp.end());
  EXPECT_EQ(std::string(itr->value()), std::to_string(IBMW_RPC_STATUS_TIMEOUT));
  std::lock_guard lock(mutex);
  if (pending) Answer(pending, IBMW_RPC_STATUS_OK);
}

// One publish fans out to several servers: each server must see its own Host
// header and the full body, and an unreachable target must not affect others.
// TRANS-024: an exception thrown by a handler is only logged; the client gets
// a generic error without its text.
TEST_F(HttpRpcDriverTest, HandlerExceptionTextIsNotSentToClient) {
  port_ = FreeTcpPort();
  svr_ = std::make_shared<ibmw::common::net::AsioHttpServer>(io_.Io());
  svr_->RegisterHttpHandleFunc<http::string_body>(
      "/rpc/test/svc",
      [](const http::request<http::dynamic_body>&, http::response<http::string_body>&,
         std::chrono::nanoseconds) -> asio::awaitable<ibmw::common::net::AsioHttpServer::HttpHandleStatus> {
        throw std::runtime_error("secret /internal/path");
        co_return ibmw::common::net::AsioHttpServer::HttpHandleStatus::kOk;
      });
  svr_->Initialize({.ep = Tcp::endpoint(kLoopback, port_)});
  svr_->Start();

  const auto rsp = Post("1");
  EXPECT_EQ(rsp.result(), http::status::internal_server_error);
  EXPECT_EQ(rsp.body().find("secret"), std::string::npos) << rsp.body();
  EXPECT_FALSE(rsp.body().empty());
  svr_->Shutdown();
}

TEST(HttpTransportDriverTest, FanOutGivesEachTargetItsOwnHostAndBody) {
  IoThreads io(4);
  const std::string pattern = "/transport/" + ibmw::common::util::UrlEncode("/t") + "/" +
                              ibmw::common::util::UrlEncode("test:Int");

  struct Received {
    std::mutex mtx;
    std::vector<std::string> hosts;
    std::vector<std::string> bodies;
  };
  Received rcv[2];
  std::shared_ptr<ibmw::common::net::AsioHttpServer> svrs[2];
  uint16_t ports[2];
  for (int i = 0; i < 2; ++i) {
    ports[i] = FreeTcpPort();
    svrs[i] = std::make_shared<ibmw::common::net::AsioHttpServer>(io.Io());
    svrs[i]->RegisterHttpHandleFunc<http::dynamic_body>(
        pattern,
        [r = &rcv[i]](const http::request<http::dynamic_body>& req, http::response<http::dynamic_body>& rsp,
                      std::chrono::nanoseconds) -> asio::awaitable<ibmw::common::net::AsioHttpServer::HttpHandleStatus> {
          std::lock_guard<std::mutex> lock(r->mtx);
          r->hosts.emplace_back(req[http::field::host]);
          r->bodies.emplace_back(boost::beast::buffers_to_string(req.body().data()));
          rsp.result(http::status::ok);
          co_return ibmw::common::net::AsioHttpServer::HttpHandleStatus::kOk;
        });
    svrs[i]->Initialize({.ep = Tcp::endpoint(kLoopback, ports[i])});
    svrs[i]->Start();
  }
  const uint16_t dead_port = FreeTcpPort();

  auto cli = std::make_shared<ibmw::common::net::AsioHttpClientPool>(io.Io());
  cli->Initialize({});
  cli->Start();
  HttpTransportDriver driver(io.Io(), cli, nullptr);
  driver.Initialize(YAML::Load(
      "publishers: [{topic: /t, server_url_list: ['http://127.0.0.1:" + std::to_string(ports[0]) +
      "', 'http://127.0.0.1:" + std::to_string(dead_port) + "', 'http://localhost:" + std::to_string(ports[1]) +
      "']}]"));
  runtime::core::transport::SendTypeWrapper wrapper;
  wrapper.info.topic_name = "/t";
  wrapper.info.msg_type = "test:Int";
  wrapper.info.msg_type_support_ref = ibmw::util::TypeSupportRef(JsonIntTypeSupport());
  ASSERT_TRUE(driver.RegisterSendType(wrapper));
  driver.Start();

  constexpr int kMsgs = 200;
  for (int v = 0; v < kMsgs; ++v) {
    runtime::core::transport::MessageFrame frame{.info = wrapper.info, .msg_ptr = &v};
    driver.Send(frame);
  }

  auto deadline = std::chrono::steady_clock::now() + 20s;
  auto count = [&](int i) {
    std::lock_guard<std::mutex> lock(rcv[i].mtx);
    return rcv[i].bodies.size();
  };
  while ((count(0) < kMsgs || count(1) < kMsgs) && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);

  const char* expect_host[2] = {"127.0.0.1", "localhost"};
  for (int i = 0; i < 2; ++i) {
    std::lock_guard<std::mutex> lock(rcv[i].mtx);
    ASSERT_EQ(rcv[i].bodies.size(), static_cast<size_t>(kMsgs)) << "server " << i;
    std::vector<bool> seen(kMsgs, false);
    for (size_t k = 0; k < rcv[i].bodies.size(); ++k) {
      EXPECT_EQ(rcv[i].hosts[k], expect_host[i]) << "server " << i;
      int v = std::stoi(rcv[i].bodies[k]);
      ASSERT_TRUE(v >= 0 && v < kMsgs);
      seen[v] = true;
    }
    EXPECT_EQ(std::count(seen.begin(), seen.end(), true), kMsgs) << "server " << i;
  }

  driver.Shutdown();
  for (auto& svr : svrs) svr->Shutdown();
}

// Keeps what the extension logs while it is alive.
class LogCapture {
 public:
  LogCapture() {
    base_.get_log_level = [](void*) { return static_cast<ibmw_log_level_t>(ibmw::common::util::kLogLevelWarn); };
    base_.log = [](void* ctx, ibmw_log_level_t level, uint32_t, const char*, const char*, const char* data, size_t size) {
      auto* self = static_cast<LogCapture*>(ctx);
      std::lock_guard lock(self->mutex_);
      self->text_.append(data, size).push_back('\n');
      if (static_cast<uint32_t>(level) >= ibmw::common::util::kLogLevelError) ++self->errors_;
    };
    base_.ctx = this;
    g_logger.Set(ibmw::logger::LoggerRef(&base_));
  }
  ~LogCapture() { g_logger.Set(ibmw::logger::LoggerRef()); }
  std::string Text() {
    std::lock_guard lock(mutex_);
    return text_;
  }
  size_t Errors() {
    std::lock_guard lock(mutex_);
    return errors_;
  }
  size_t Lines() {
    const auto text = Text();
    return std::count(text.begin(), text.end(), '\n');
  }

 private:
  ibmw_logger_base_t base_{};
  std::mutex mutex_;
  std::string text_;
  size_t errors_ = 0;
};

// TRANS-053: exceptions on the executor threads reach the configured logger.
TEST(NetAsioExecutorTest, ThreadExceptionGoesToConfiguredLogger) {
  std::mutex mutex;
  std::string text;
  auto logger = std::make_shared<ibmw::common::util::LoggerWrapper>(ibmw::common::util::LoggerWrapper{
      .get_log_level_func = []() -> uint32_t { return 0; },
      .log_func = [&](uint32_t, uint32_t, const char*, const char*, const char* data, size_t size) {
        std::lock_guard lock(mutex);
        text.append(data, size);
      }});
  ibmw::common::net::AsioExecutor executor(1);
  EXPECT_THROW(executor.SetLogger(nullptr), std::invalid_argument);
  executor.SetLogger(logger);
  asio::post(*executor.IO(), [] { throw std::runtime_error("boom"); });
  executor.Start();
  EXPECT_THROW(executor.SetLogger(logger), std::runtime_error);
  executor.Shutdown();
  executor.Join();
  std::lock_guard lock(mutex);
  EXPECT_NE(text.find("AsioExecutor thread get exception boom."), std::string::npos) << text;
}

// Sends one message whose serialized form is `view` through a started driver,
// which stays up until `received` (when given) got it: sending is asynchronous.
template <typename Driver, typename Pool>
void SendCached(IoThreads& io, uint16_t port, std::shared_ptr<ibmw::util::BufferArrayView> view,
                Received* received = nullptr) {
  auto pool = std::make_shared<Pool>(io.Io());
  pool->Initialize({});
  pool->Start();
  Driver driver(io.Io(), pool, nullptr, nullptr);
  driver.Initialize(YAML::Load("publishers: [{topic: /t, server_url_list: ['127.0.0.1:" + std::to_string(port) + "']}]"));
  runtime::core::transport::SendTypeWrapper wrapper;
  wrapper.info.topic_name = "/t";
  wrapper.info.msg_type = "test:Int";
  wrapper.info.msg_type_support_ref = ibmw::util::TypeSupportRef(JsonIntTypeSupport());
  ASSERT_TRUE(driver.RegisterSendType(wrapper));
  driver.Start();
  int value = 0;
  runtime::core::transport::MessageFrame frame{.info = wrapper.info, .msg_ptr = &value};
  frame.serialization_cache.emplace("json", std::move(view));
  driver.Send(frame);
  if (received) EXPECT_TRUE(received->WaitFor(1));
  driver.Shutdown();
}

// TRANS-036: the package length is checked before it is narrowed to the 32 bit
// frame header. Only the declared size is huge: the payload is never read.
template <typename Driver, typename Pool>
void ExpectOversizedPackageIsRefused() {
  IoThreads io;
  static const char kPayload[] = "x";
  const size_t declared = (size_t{1} << 32) + 16;
  LogCapture capture;
  SendCached<Driver, Pool>(io, FreeTcpPort(),
                           std::make_shared<ibmw::util::BufferArrayView>(kPayload, declared));
  EXPECT_NE(capture.Text().find("exceeds the 4 GiB - 1 limit"), std::string::npos) << capture.Text();
}

TEST(NetDriverPackageSizeTest, OversizedPackageIsRefusedBeforeNarrowing) {
  ExpectOversizedPackageIsRefused<TcpTransportDriver, ibmw::common::net::AsioTcpClientPool>();
  ExpectOversizedPackageIsRefused<UdpTransportDriver, ibmw::common::net::AsioUdpClientPool>();
}

// TRANS-055: like the TCP driver, the UDP driver only takes send types before
// Start; Send reads the publisher map without a lock.
template <typename Driver, typename Pool>
void ExpectSendTypeOnlyBeforeStart() {
  IoThreads io;
  auto pool = std::make_shared<Pool>(io.Io());
  pool->Initialize({});
  pool->Start();
  Driver driver(io.Io(), pool, nullptr, nullptr);
  driver.Initialize(YAML::Load("publishers: [{topic: /t, server_url_list: ['127.0.0.1:1']}]"));
  runtime::core::transport::SendTypeWrapper wrapper;
  wrapper.info.topic_name = "/t";
  wrapper.info.msg_type = "test:Int";
  wrapper.info.msg_type_support_ref = ibmw::util::TypeSupportRef(JsonIntTypeSupport());
  driver.Start();
  LogCapture capture;
  EXPECT_FALSE(driver.RegisterSendType(wrapper));
  EXPECT_NE(capture.Text().find("state is 'Init'"), std::string::npos) << capture.Text();
  driver.Shutdown();
}

TEST(NetDriverStateTest, SendTypeOnlyBeforeStart) {
  ExpectSendTypeOnlyBeforeStart<TcpTransportDriver, ibmw::common::net::AsioTcpClientPool>();
  ExpectSendTypeOnlyBeforeStart<UdpTransportDriver, ibmw::common::net::AsioUdpClientPool>();
}

TEST(NetDriverPackageSizeTest, NormalPackageStillArrives) {
  IoThreads io;
  static const char kPayload[] = "payload";
  const auto view = [] { return std::make_shared<ibmw::util::BufferArrayView>(kPayload, sizeof(kPayload) - 1); };
  const auto ends_with_payload = [](const std::string& msg) { return msg.ends_with("payload"); };

  Received tcp_received;
  const uint16_t tcp_port = FreeTcpPort();
  auto tcp_svr = MakeTcpServer(io, tcp_received, tcp_port);
  tcp_svr->Start();
  SendCached<TcpTransportDriver, ibmw::common::net::AsioTcpClientPool>(io, tcp_port, view(), &tcp_received);
  ASSERT_EQ(tcp_received.msgs.size(), 1u);
  EXPECT_TRUE(ends_with_payload(tcp_received.msgs[0].second));
  tcp_svr->Shutdown();

  Received udp_received;
  const uint16_t udp_port = FreeUdpPort();
  auto udp_svr = std::make_shared<ibmw::common::net::AsioUdpServer>(io.Io());
  udp_svr->RegisterMsgHandle([&udp_received](const Udp::endpoint& ep, const std::shared_ptr<asio::streambuf>& buf) {
    udp_received.Add(ep, buf);
  });
  udp_svr->Initialize({.ep = Udp::endpoint(kLoopback, udp_port)});
  udp_svr->Start();
  SendCached<UdpTransportDriver, ibmw::common::net::AsioUdpClientPool>(io, udp_port, view(), &udp_received);
  ASSERT_EQ(udp_received.msgs.size(), 1u);
  EXPECT_TRUE(ends_with_payload(udp_received.msgs[0].second));
  udp_svr->Shutdown();
}

std::shared_ptr<asio::streambuf> MakeBuf(const std::string& data) {
  auto buf = std::make_shared<asio::streambuf>();
  asio::buffer_copy(buf->prepare(data.size()), asio::buffer(data));
  buf->commit(data.size());
  return buf;
}

// TRANS-040: these addresses have the same AsioEndpointHash on every port.
const auto kCollidingA = asio::ip::make_address("127.214.200.135");
const auto kCollidingB = asio::ip::make_address("127.214.201.198");

TEST(NetClientPoolTest, EndpointsWithEqualHashReachTheirOwnServer) {
  IoThreads io;
  const uint16_t tcp_port = FreeTcpPort();
  const Tcp::endpoint tcp_ep[2] = {{kCollidingA, tcp_port}, {kCollidingB, tcp_port}};
  ASSERT_EQ(ibmw::common::net::AsioEndpointHash{}(tcp_ep[0]), ibmw::common::net::AsioEndpointHash{}(tcp_ep[1]));
  Received tcp_received[2];
  std::shared_ptr<ibmw::common::net::AsioTcpServer> tcp_svr[2];
  auto tcp_pool = std::make_shared<ibmw::common::net::AsioTcpClientPool>(io.Io());
  tcp_pool->Initialize({});
  tcp_pool->Start();
  for (int i = 0; i < 2; ++i) {
    tcp_svr[i] = std::make_shared<ibmw::common::net::AsioTcpServer>(io.Io());
    tcp_svr[i]->RegisterMsgHandle([r = &tcp_received[i]](const Tcp::endpoint& ep, const std::shared_ptr<asio::streambuf>& buf) {
      r->Add(ep, buf);
    });
    tcp_svr[i]->Initialize({.ep = tcp_ep[i]});
    tcp_svr[i]->Start();
    auto cli = asio::co_spawn(*io.Io(), tcp_pool->GetClient({.svr_ep = tcp_ep[i]}), asio::use_future).get();
    ASSERT_TRUE(cli);
    cli->SendMsg(MakeBuf(std::to_string(i)));
    ASSERT_TRUE(tcp_received[i].WaitFor(1)) << "tcp server " << i;
    EXPECT_EQ(tcp_received[i].msgs[0].second, std::to_string(i));
  }

  const uint16_t udp_port = FreeUdpPort();
  const Udp::endpoint udp_ep[2] = {{kCollidingA, udp_port}, {kCollidingB, udp_port}};
  Received udp_received[2];
  std::shared_ptr<ibmw::common::net::AsioUdpServer> udp_svr[2];
  auto udp_pool = std::make_shared<ibmw::common::net::AsioUdpClientPool>(io.Io());
  udp_pool->Initialize({});
  udp_pool->Start();
  for (int i = 0; i < 2; ++i) {
    udp_svr[i] = std::make_shared<ibmw::common::net::AsioUdpServer>(io.Io());
    udp_svr[i]->RegisterMsgHandle([r = &udp_received[i]](const Udp::endpoint& ep, const std::shared_ptr<asio::streambuf>& buf) {
      r->Add(ep, buf);
    });
    udp_svr[i]->Initialize({.ep = udp_ep[i]});
    udp_svr[i]->Start();
    auto cli = asio::co_spawn(*io.Io(), udp_pool->GetClient({.svr_ep = udp_ep[i]}), asio::use_future).get();
    ASSERT_TRUE(cli);
    cli->SendMsg(MakeBuf(std::to_string(i)));
    ASSERT_TRUE(udp_received[i].WaitFor(1)) << "udp server " << i;
    EXPECT_EQ(udp_received[i].msgs[0].second, std::to_string(i));
  }

  tcp_pool->Shutdown();
  udp_pool->Shutdown();
  for (auto& svr : tcp_svr) svr->Shutdown();
  for (auto& svr : udp_svr) svr->Shutdown();
}

// TRANS-040: GetClient starts when it is awaited; the options it uses are the
// ones of the call, whatever happens to the caller's object until then.
template <typename Pool, typename Options>
void ExpectGetClientKeepsOptions(IoThreads& io, Options options, const Options& changed) {
  auto pool = std::make_shared<Pool>(io.Io());
  pool->Initialize({});
  pool->Start();
  auto first = asio::co_spawn(*io.Io(), pool->GetClient(options), asio::use_future).get();
  auto pending = pool->GetClient(options);
  options = changed;
  auto second = asio::co_spawn(*io.Io(), std::move(pending), asio::use_future).get();
  EXPECT_TRUE(first);
  EXPECT_EQ(first, second);
  pool->Shutdown();
}

TEST(NetClientPoolTest, GetClientKeepsOptionsUntilAwaited) {
  IoThreads io;
  Received received;
  const uint16_t tcp_port = FreeTcpPort();
  auto svr = MakeTcpServer(io, received, tcp_port);
  svr->Start();
  ExpectGetClientKeepsOptions<ibmw::common::net::AsioTcpClientPool>(
      io, ibmw::common::net::AsioTcpClient::Options{.svr_ep = Tcp::endpoint(kLoopback, tcp_port)},
      ibmw::common::net::AsioTcpClient::Options{.svr_ep = Tcp::endpoint(kCollidingA, tcp_port)});
  ExpectGetClientKeepsOptions<ibmw::common::net::AsioUdpClientPool>(
      io, ibmw::common::net::AsioUdpClient::Options{.svr_ep = Udp::endpoint(kLoopback, 9)},
      ibmw::common::net::AsioUdpClient::Options{.svr_ep = Udp::endpoint(kCollidingA, 9)});
  ExpectGetClientKeepsOptions<ibmw::common::net::AsioHttpClientPool>(
      io, ibmw::common::net::AsioHttpClient::Options{.host = "127.0.0.1", .service = "80"},
      ibmw::common::net::AsioHttpClient::Options{.host = "127.0.0.2", .service = "80"});
  svr->Shutdown();
}

// TRANS-041: a client's heartbeat keeps the session alive and is no message.
TEST(NetTcpServerTest, HeartbeatIsNotDispatched) {
  IoThreads io;
  Received received;
  const uint16_t port = FreeTcpPort();
  auto svr = MakeTcpServer(io, received, port);
  svr->Start();
  asio::io_context cli_io;
  Tcp::socket sock(cli_io);
  sock.connect(Tcp::endpoint(kLoopback, port));
  asio::write(sock, asio::buffer(TcpFrame("") + TcpFrame("") + TcpFrame("x")));
  ASSERT_TRUE(received.WaitFor(1));
  std::lock_guard lock(received.mutex);
  ASSERT_EQ(received.msgs.size(), 1u);
  EXPECT_EQ(received.msgs[0].second, "x");
  svr->Shutdown();
}

// TRANS-041: reconnecting with garbage does not flood the log; the session
// still ends.
TEST(NetTcpServerTest, InvalidHeadLogIsLimited) {
  IoThreads io;
  Received received;
  const uint16_t port = FreeTcpPort();
  LogCapture capture;
  auto svr = std::make_shared<ibmw::common::net::AsioTcpServer>(io.Io());
  svr->SetLogger(std::make_shared<ibmw::common::util::LoggerWrapper>(ibmw::common::util::LoggerWrapper{
      .get_log_level_func = [logger = g_logger.Get()] { return logger.GetLogLevel(); },
      .log_func = [logger = g_logger.Get()](uint32_t lvl, uint32_t line, const char* file, const char* func,
                                            const char* data, size_t size) { logger.Log(lvl, line, file, func, data, size); }}));
  svr->RegisterMsgHandle([&received](const Tcp::endpoint& ep, const std::shared_ptr<asio::streambuf>& buf) {
    received.Add(ep, buf);
  });
  svr->Initialize({.ep = Tcp::endpoint(kLoopback, port)});
  svr->Start();

  constexpr size_t kConnections = 30;
  for (size_t i = 0; i < kConnections; ++i) {
    asio::io_context cli_io;
    Tcp::socket sock(cli_io);
    sock.connect(Tcp::endpoint(kLoopback, port));
    asio::write(sock, asio::buffer("garbage!", 8));
    char byte = 0;
    boost::system::error_code ec;
    asio::read(sock, asio::buffer(&byte, 1), ec);
    EXPECT_TRUE(ec) << "the server keeps a session with an invalid head";
  }
  svr->Shutdown();
  EXPECT_LT(capture.Lines(), kConnections / 2) << capture.Text();
  EXPECT_EQ(capture.Errors(), 0u) << capture.Text();
  EXPECT_EQ(received.msgs.size(), 0u);
}

// TRANS-041: undecodable messages are dropped; each kind logs at most once per
// interval, and good messages keep being handled.
TEST(NetMsgCatalogTest, PeerGarbageLogIsLimited) {
  LogCapture capture;
  auto catalog = std::make_shared<MsgHandleCatalog<Tcp::endpoint>>();
  size_t handled = 0;
  catalog->RegisterMsgHandle("/a", [&handled](const std::shared_ptr<asio::streambuf>&) { ++handled; });
  catalog->RegisterMsgHandle("/throws", [](const std::shared_ptr<asio::streambuf>&) { throw std::runtime_error("bad"); });
  const auto handle = catalog->GetMsgHandleFunc();
  const auto msg = [](std::string_view uri) { return std::string(1, static_cast<char>(uri.size())) + std::string(uri) + "d"; };

  constexpr size_t kRounds = 100;
  for (size_t i = 0; i < kRounds; ++i) {
    handle({}, MakeBuf(""));                  // shorter than a header
    handle({}, MakeBuf(std::string("\xff", 1) + "short"));  // uri longer than the message
    handle({}, MakeBuf(msg("/unknown")));
    handle({}, MakeBuf(msg("/throws")));
    handle({}, MakeBuf(msg("/a")));
  }
  EXPECT_EQ(handled, kRounds);
  EXPECT_LT(capture.Lines(), kRounds) << capture.Text();
}

// TRANS-043: the function given to a server keeps the catalog alive, whatever
// the destruction order of their owners.
TEST(NetMsgCatalogTest, HandleFuncKeepsCatalogAlive) {
  auto catalog = std::make_shared<MsgHandleCatalog<Tcp::endpoint>>();
  size_t handled = 0;
  catalog->RegisterMsgHandle("/a", [&handled](const std::shared_ptr<asio::streambuf>&) { ++handled; });
  const std::weak_ptr<MsgHandleCatalog<Tcp::endpoint>> observer = catalog;
  auto handle = catalog->GetMsgHandleFunc();
  catalog.reset();
  ASSERT_FALSE(observer.expired());
  handle({}, MakeBuf(std::string(1, '\x02') + "/ad"));
  EXPECT_EQ(handled, 1u);
  handle = nullptr;
  EXPECT_TRUE(observer.expired());
}

}  // namespace
}  // namespace ibmw::extensions::net_extension
