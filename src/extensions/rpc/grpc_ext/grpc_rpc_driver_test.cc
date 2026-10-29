// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for the gRPC service path (EXT-004): an answer given
// inline or from another thread is never lost, a missing answer ends the
// request by timeout, and a late answer is harmless.
// EXT-002: an oversized request body is refused before it reaches the service.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio.hpp>

#include "cpp/util/string.h"
#include "grpc_ext/grpc_rpc_driver.h"

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
  std::optional<uint32_t> Call(int req, int* rsp, std::chrono::nanoseconds timeout = 5s) {
    auto result = std::make_shared<Result>();
    auto ctx = std::make_shared<ibmw::rpc::Context>();
    ctx->SetSerializationType("cdr");
    ctx->SetTimeout(timeout);
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
  std::shared_ptr<server::AsioHttp2Server> svr_;
  std::shared_ptr<client::AsioHttp2ClientPool> pool_;
  std::unique_ptr<GrpcRpcDriver> driver_;
  runtime::core::service::ServiceInfo info_;
  runtime::core::service::ServiceFuncWrapper service_;
  runtime::core::service::ClientFuncWrapper client_;
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
  std::mutex mutex;
  std::vector<std::shared_ptr<ServiceMessageFrame>> pending;
  SetUpDriver([&](const std::shared_ptr<ServiceMessageFrame>& frame) {
    std::lock_guard lock(mutex);
    pending.push_back(frame);
  });
  int rsp = 0;
  auto code = Call(1, &rsp, 300ms);
  ASSERT_TRUE(code) << "no answer";
  EXPECT_NE(*code, static_cast<uint32_t>(IBMW_RPC_STATUS_OK));

  std::this_thread::sleep_for(100ms);  // the server side gave up as well
  std::vector<std::shared_ptr<ServiceMessageFrame>> late;
  {
    std::lock_guard lock(mutex);
    late.swap(pending);
  }
  ASSERT_EQ(late.size(), 1u);
  *static_cast<int*>(late[0]->rsp_ptr) = 7;  // the response object is still alive
  Answer(late[0], IBMW_RPC_STATUS_OK);

  code = Call(2, &rsp, 300ms);
  ASSERT_TRUE(code) << "server no longer answers";
  EXPECT_NE(*code, static_cast<uint32_t>(IBMW_RPC_STATUS_OK));
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
  auto code = Call(1234567890, &rsp, 1s);
  ASSERT_TRUE(code) << "no answer";
  EXPECT_NE(*code, static_cast<uint32_t>(IBMW_RPC_STATUS_OK));
  EXPECT_EQ(served.load(), 0);

  for (int i = 0; i < 5; ++i) {
    code = Call(7, &rsp);
    ASSERT_TRUE(code) << "server no longer answers";
    ASSERT_EQ(*code, static_cast<uint32_t>(IBMW_RPC_STATUS_OK));
    EXPECT_EQ(rsp, 8);
  }
  EXPECT_EQ(served.load(), 5);
}

}  // namespace
}  // namespace ibmw::extensions::grpc_extension
