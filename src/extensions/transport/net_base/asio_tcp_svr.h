// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <chrono>
#include <list>
#include <memory>
#include <unordered_map>
#include <vector>

#include <boost/asio.hpp>

#include "net_base/asio_endpoint_hash.h"
#include "base/buffer_util.h"
#include "base/log_util.h"
#include "base/string_util.h"

namespace ibmw::common::net {

class AsioTcpServer : public std::enable_shared_from_this<AsioTcpServer> {
 public:
  using IOCtx = boost::asio::io_context;
  using Tcp = boost::asio::ip::tcp;
  using Strand = boost::asio::strand<IOCtx::executor_type>;
  using Timer = boost::asio::steady_timer;
  using Streambuf = boost::asio::streambuf;

  template <class T>
  using Awaitable = boost::asio::awaitable<T>;

  using MsgHandle =
      std::function<void(const Tcp::endpoint&, const std::shared_ptr<Streambuf>&)>;

  struct Options {
    /// Listening address
    Tcp::endpoint ep = Tcp::endpoint{boost::asio::ip::address_v4::loopback(), 57634};

    /// Maximum number of connections
    size_t max_session_num = 1000000;

    /// Managing coroutine timer intervals
    std::chrono::nanoseconds mgr_timer_dt = std::chrono::seconds(10);

    /// Maximum time without data
    std::chrono::nanoseconds max_no_data_duration = std::chrono::seconds(300);

    /// Maximum package size
    uint32_t max_recv_size = 1024 * 1024 * 16;

    static Options Verify(const Options& verify_options) {
      Options options(verify_options);

      if (options.max_session_num < 1) options.max_session_num = 1;

      if (options.max_session_num > Tcp::acceptor::max_listen_connections)
        options.max_session_num = Tcp::acceptor::max_listen_connections;

      if (options.mgr_timer_dt < std::chrono::milliseconds(100))
        options.mgr_timer_dt = std::chrono::milliseconds(100);

      if (options.max_no_data_duration < std::chrono::seconds(10))
        options.max_no_data_duration = std::chrono::seconds(10);

      return options;
    }
  };

  explicit AsioTcpServer(const std::shared_ptr<IOCtx>& io_ptr)
      : io_ptr_(io_ptr),
        mgr_strand_(boost::asio::make_strand(*io_ptr_)),
        acceptor_(mgr_strand_),
        acceptor_timer_(mgr_strand_),
        mgr_timer_(mgr_strand_),
        logger_ptr_(std::make_shared<ibmw::common::util::LoggerWrapper>()) {}

  ~AsioTcpServer() = default;

  AsioTcpServer(const AsioTcpServer&) = delete;
  AsioTcpServer& operator=(const AsioTcpServer&) = delete;

  void SetLogger(const std::shared_ptr<ibmw::common::util::LoggerWrapper>& logger_ptr) {
    MW_REQUIRE(
        state_.load() == State::kPreInit,
        "Method can only be called when state is 'PreInit'.");

    logger_ptr_ = logger_ptr;
  }

  template <typename... Args>
    requires std::constructible_from<MsgHandle, Args...>
  void RegisterMsgHandle(Args&&... args) {
    MW_REQUIRE(
        state_.load() == State::kPreInit,
        "Method can only be called when state is 'PreInit'.");

    msg_handle_ptr_ = std::make_shared<MsgHandle>(std::forward<Args>(args)...);
  }

  void Initialize(const Options& options) {
    MW_REQUIRE(
        msg_handle_ptr_,
        "Msg handle is not set before initialize.");

    MW_REQUIRE(
        std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
        "Method can only be called when state is 'PreInit'.");

    options_ = Options::Verify(options);
    session_options_ptr_ = std::make_shared<SessionOptions>(options_);

    MW_REQUIRE(CheckListenAddr(options_.ep),
                            "{} is already in use.", ibmw::common::util::SSToString(options_.ep));
  }

  void Start() {
    MW_REQUIRE(
        state_.load() == State::kInit,
        "Method can only be called when state is 'Init'.");

    // Bind synchronously (no async operation exists yet) so a failure is
    // reported to the caller instead of being lost in a detached coroutine.
    boost::system::error_code ec;
    acceptor_.open(options_.ep.protocol(), ec);
    if (!ec) acceptor_.set_option(Tcp::acceptor::reuse_address(true), ec);
    if (!ec) acceptor_.bind(options_.ep, ec);
    if (!ec) acceptor_.listen(Tcp::acceptor::max_listen_connections, ec);
    if (ec) {
      boost::system::error_code ignored;
      acceptor_.close(ignored);
      MW_THROW("Tcp svr failed to listen on {}: {}.",
               ibmw::common::util::SSToString(options_.ep), ec.message());
    }

    MW_REQUIRE(
        std::atomic_exchange(&state_, State::kStart) == State::kInit,
        "Method can only be called when state is 'Init'.");

    auto self = shared_from_this();
    boost::asio::co_spawn(
        mgr_strand_,
        [this, self]() -> Awaitable<void> {
          while (state_.load() == State::kStart) {
            try {
              // If the number of connections reaches the upper limit, wait for a while before trying
              if (session_ptr_map_.size() >= options_.max_session_num) {
                acceptor_timer_.expires_after(options_.mgr_timer_dt);
                co_await acceptor_timer_.async_wait(boost::asio::use_awaitable);
                continue;
              }

              auto session_ptr = std::make_shared<Session>(
                  io_ptr_, logger_ptr_, msg_handle_ptr_);
              session_ptr->Initialize(session_options_ptr_);

              co_await acceptor_.async_accept(session_ptr->Socket(),
                                              boost::asio::use_awaitable);

              // The peer may already be gone: skip it instead of throwing.
              boost::system::error_code ec;
              auto remote_ep = session_ptr->Socket().remote_endpoint(ec);
              if (ec) {
                MW_TRACE("Tcp svr accepted connection is already closed: {}", ec.message());
                session_ptr->Socket().close(ec);
                continue;
              }
              MW_TRACE(
                  "Tcp svr accept a new connection, remote addr is {}",
                  ibmw::common::util::SSToString(remote_ep));

              // Registered before Start (same strand as SendMsg); a stale
              // session with a reused endpoint is shut down and replaced.
              auto [itr, inserted] = session_ptr_map_.try_emplace(remote_ep, session_ptr);
              if (!inserted) {
                itr->second->Shutdown();
                itr->second = session_ptr;
              }
              session_ptr->Start(remote_ep);

            } catch (const std::exception& e) {
              MW_TRACE(
                  "Tcp svr accept connection get exception and exit, exception info: {}",
                  e.what());
            }
          }

          Shutdown();

          co_return;
        },
        boost::asio::detached);

    boost::asio::co_spawn(
        mgr_strand_,
        [this, self]() -> Awaitable<void> {
          while (state_.load() == State::kStart) {
            try {
              mgr_timer_.expires_after(options_.mgr_timer_dt);
              co_await mgr_timer_.async_wait(boost::asio::use_awaitable);

              for (auto itr = session_ptr_map_.begin(); itr != session_ptr_map_.end();) {
                if (itr->second->IsRunning())
                  ++itr;
                else
                  session_ptr_map_.erase(itr++);
              }
            } catch (const std::exception& e) {
              MW_TRACE(
                  "Tcp svr timer get exception and exit, exception info: {}",
                  e.what());
            }
          }

          Shutdown();

          co_return;
        },
        boost::asio::detached);
  }

  void Shutdown() {
    if (std::atomic_exchange(&state_, State::kShutdown) == State::kShutdown)
      return;

    auto self = shared_from_this();
    boost::asio::dispatch(mgr_strand_, [this, self]() {
      uint32_t stop_step = 1;
      while (stop_step) {
        try {
          switch (stop_step) {
            case 1:
              acceptor_timer_.cancel();
              ++stop_step;
            case 2:
              mgr_timer_.cancel();
              ++stop_step;
            case 3:
              acceptor_.cancel();
              ++stop_step;
            case 4:
              acceptor_.close();
              ++stop_step;
            case 5:
              acceptor_.release();
              ++stop_step;
            default:
              stop_step = 0;
              break;
          }
        } catch (const std::exception& e) {
          MW_WARN(
              "Tcp svr stop get exception at step {}, exception info: {}",
              stop_step, e.what());
          ++stop_step;
        }
      }

      for (auto& session_ptr : session_ptr_map_) session_ptr.second->Shutdown();

      session_ptr_map_.clear();
    });
  }

  void SendMsg(const Tcp::endpoint& ep, const std::shared_ptr<Streambuf>& msg_buf_ptr) {
    auto self = shared_from_this();
    boost::asio::dispatch(mgr_strand_, [this, self, ep, msg_buf_ptr]() {
      if (state_.load() != State::kStart) [[unlikely]] {
        MW_WARN("Tcp svr is closed, will not send current msg.");
        return;
      }

      auto finditr = session_ptr_map_.find(ep);
      if (finditr == session_ptr_map_.end()) {
        MW_WARN("Tcp svr can not find endpoint {} in session map",
                   ibmw::common::util::SSToString(ep));
        return;
      }

      auto session_ptr = finditr->second;
      if (session_ptr && session_ptr->IsRunning()) {
        session_ptr->SendMsg(msg_buf_ptr);
      }
    });
  }

  const ibmw::common::util::LoggerWrapper& GetLogger() const { return *logger_ptr_; }

  bool IsRunning() const { return state_.load() == State::kStart; }

 private:
  // | 2byte magicnum | 4byte msglen |
  static constexpr size_t kHeadSize = 6;
  static constexpr char kHeadByte1 = 'Y';
  static constexpr char kHeadByte2 = 'T';
  static constexpr size_t kEagerRecvSize = 4 * 1024 * 1024;
  static constexpr size_t kRecvProbeSize = 256 * 1024;

  static bool CheckListenAddr(const Tcp::endpoint& ep) {
    try {
      IOCtx io;
      Tcp::acceptor acceptor(io, ep);
      return true;
    } catch (...) {
      return false;
    }
  }

  struct SessionOptions {
    explicit SessionOptions(const Options& options)
        : max_no_data_duration(options.max_no_data_duration),
          max_recv_size(options.max_recv_size) {}

    std::chrono::nanoseconds max_no_data_duration;
    uint32_t max_recv_size;
  };

  class Session : public std::enable_shared_from_this<Session> {
   public:
    Session(const std::shared_ptr<IOCtx>& io_ptr,
            const std::shared_ptr<ibmw::common::util::LoggerWrapper>& logger_ptr,
            const std::shared_ptr<MsgHandle>& msg_handle_ptr)
        : io_ptr_(io_ptr),
          session_socket_strand_(boost::asio::make_strand(*io_ptr)),
          sock_(session_socket_strand_),
          send_sig_timer_(session_socket_strand_),
          session_mgr_strand_(boost::asio::make_strand(*io_ptr)),
          timer_(session_mgr_strand_),
          logger_ptr_(logger_ptr),
          msg_handle_ptr_(msg_handle_ptr) {}

    ~Session() = default;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void Initialize(const std::shared_ptr<const SessionOptions>& session_options_ptr) {
      MW_REQUIRE(
          std::atomic_exchange(&state_, SessionState::kInit) == SessionState::kPreInit,
          "Method can only be called when state is 'PreInit'.");

      session_options_ptr_ = session_options_ptr;
    }

    void Start(const Tcp::endpoint& remote_ep) {
      MW_REQUIRE(
          std::atomic_exchange(&state_, SessionState::kStart) == SessionState::kInit,
          "Method can only be called when state is 'Init'.");

      remote_ep_ = remote_ep;
      remote_addr_ = ibmw::common::util::SSToString(remote_ep_);

      auto self = shared_from_this();

      // Sender co
      boost::asio::co_spawn(
          session_socket_strand_,
          [this, self]() -> Awaitable<void> {
            try {
              while (state_.load() == SessionState::kStart) {
                while (!data_list_.empty()) {
                  std::list<std::shared_ptr<Streambuf>> tmp_data_list;
                  tmp_data_list.swap(data_list_);

                  std::vector<char> head_buf(tmp_data_list.size() * kHeadSize);

                  std::vector<boost::asio::const_buffer> data_buf_vec;
                  data_buf_vec.reserve(tmp_data_list.size() * 2);
                  size_t ct = 0;
                  for (auto& itr : tmp_data_list) {
                    head_buf[ct * kHeadSize] = kHeadByte1;
                    head_buf[ct * kHeadSize + 1] = kHeadByte2;
                    ibmw::common::util::SetBufFromUint32(&head_buf[ct * kHeadSize + 2],
                                                          static_cast<uint32_t>(itr->size()));
                    data_buf_vec.emplace_back(
                        boost::asio::const_buffer(&head_buf[ct * kHeadSize], kHeadSize));
                    ++ct;

                    data_buf_vec.emplace_back(itr->data());
                  }

                  tick_has_data_ = true;
                  size_t write_data_size = co_await boost::asio::async_write(
                      sock_, data_buf_vec, boost::asio::use_awaitable);
                  MW_TRACE("Tcp svr session async write {} bytes to {}.",
                              write_data_size, RemoteAddr());
                }

                try {
                  send_sig_timer_.expires_after(std::chrono::seconds(3600));
                  co_await send_sig_timer_.async_wait(boost::asio::use_awaitable);
                } catch (const std::exception& e) {
                  MW_TRACE(
                      "Tcp svr session timer canceled, remote addr {}, exception info: {}",
                      RemoteAddr(), e.what());
                }
              }
            } catch (const std::exception& e) {
              MW_WARN(
                  "Tcp svr session send co get exception and exit, remote addr {}, exception info: {}",
                  RemoteAddr(), e.what());
            }

            Shutdown();

            co_return;
          },
          boost::asio::detached);

      // Receiver co
      boost::asio::co_spawn(
          session_socket_strand_,
          [this, self]() -> Awaitable<void> {
            try {
              std::vector<char> head_buf(kHeadSize);
              boost::asio::mutable_buffer asio_head_buf(head_buf.data(), kHeadSize);

              while (state_.load() == SessionState::kStart) {
                size_t read_data_size = co_await boost::asio::async_read(
                    sock_, asio_head_buf,
                    boost::asio::transfer_exactly(kHeadSize),
                    boost::asio::use_awaitable);
                MW_TRACE(
                    "Tcp svr session async read {} bytes from {} for head.",
                    read_data_size, RemoteAddr());
                tick_has_data_ = true;

                MW_REQUIRE(
                    read_data_size == kHeadSize && head_buf[0] == kHeadByte1 && head_buf[1] == kHeadByte2,
                    "Get an invalid head, remote addr {}, read_data_size: {}, head_buf[0]: {}, head_buf[1]: {}.",
                    RemoteAddr(), read_data_size,
                    static_cast<uint8_t>(head_buf[0]),
                    static_cast<uint8_t>(head_buf[1]));

                uint32_t msg_len = ibmw::common::util::GetUint32FromBuf(&head_buf[2]);

                MW_REQUIRE(
                    msg_len <= session_options_ptr_->max_recv_size,
                    "Msg too large, remote addr {}, size: {}.", RemoteAddr(), msg_len);

                auto msg_buf = std::make_shared<Streambuf>();

                // Up to kEagerRecvSize is reserved at once. A larger message
                // first has to deliver kRecvProbeSize real bytes before the rest
                // is reserved, so a bare header cannot make the server reserve
                // max_recv_size; the cost is one copy of the probe.
                size_t remaining = msg_len;
                while (remaining > 0) {
                  const size_t chunk =
                      (msg_len <= kEagerRecvSize || remaining < msg_len) ? remaining : kRecvProbeSize;
                  read_data_size = co_await boost::asio::async_read(
                      sock_, msg_buf->prepare(chunk),
                      boost::asio::transfer_exactly(chunk),
                      boost::asio::use_awaitable);
                  MW_TRACE("Tcp svr session async read {} bytes from {}.",
                              read_data_size, RemoteAddr());
                  tick_has_data_ = true;
                  msg_buf->commit(chunk);
                  remaining -= chunk;
                }

                // The handler may run after this session is gone.
                boost::asio::post(
                    *io_ptr_,
                    [msg_handle_ptr = msg_handle_ptr_, remote_ep = remote_ep_, msg_buf]() {
                      (*msg_handle_ptr)(remote_ep, msg_buf);
                    });
              }
            } catch (const std::exception& e) {
              MW_WARN(
                  "Tcp svr session recv co get exception and exit, remote addr {}, exception info: {}",
                  RemoteAddr(), e.what());
            }

            Shutdown();

            co_return;
          },
          boost::asio::detached);

      // Timer co
      boost::asio::co_spawn(
          session_mgr_strand_,
          [this, self]() -> Awaitable<void> {
            try {
              while (state_.load() == SessionState::kStart) {
                timer_.expires_after(session_options_ptr_->max_no_data_duration);
                co_await timer_.async_wait(boost::asio::use_awaitable);

                if (tick_has_data_) {
                  tick_has_data_ = false;
                } else {
                  MW_TRACE(
                      "Tcp svr session exit due to timeout({}ms), remote addr {}.",
                      std::chrono::duration_cast<std::chrono::milliseconds>(session_options_ptr_->max_no_data_duration).count(),
                      RemoteAddr());
                  break;
                }
              }
            } catch (const std::exception& e) {
              MW_TRACE(
                  "Tcp svr session timer get exception and exit, remote addr {}, exception info: {}",
                  RemoteAddr(), e.what());
            }

            Shutdown();

            co_return;
          },
          boost::asio::detached);
    }

    void Shutdown() {
      if (std::atomic_exchange(&state_, SessionState::kShutdown) == SessionState::kShutdown)
        return;

      auto self = shared_from_this();
      boost::asio::dispatch(session_socket_strand_, [this, self]() {
        uint32_t stop_step = 1;
        while (stop_step) {
          try {
            switch (stop_step) {
              case 1:
                send_sig_timer_.cancel();
                ++stop_step;
              case 2:
                sock_.shutdown(Tcp::socket::shutdown_both);
                ++stop_step;
              case 3:
                sock_.cancel();
                ++stop_step;
              case 4:
                sock_.close();
                ++stop_step;
              case 5:
                sock_.release();
                ++stop_step;
              default:
                stop_step = 0;
                break;
            }
          } catch (const std::exception& e) {
            MW_TRACE(
                "Tcp svr session stop get exception at step {}, remote addr {}, exception info: {}",
                stop_step, RemoteAddr(), e.what());
            ++stop_step;
          }
        }
      });

      boost::asio::dispatch(session_mgr_strand_, [this, self]() {
        uint32_t stop_step = 1;
        while (stop_step) {
          try {
            switch (stop_step) {
              case 1:
                timer_.cancel();
                ++stop_step;
              default:
                stop_step = 0;
                break;
            }
          } catch (const std::exception& e) {
            MW_WARN(
                "Tcp svr session mgr stop get exception at step {}, remote addr {}, exception info: {}",
                stop_step, RemoteAddr(), e.what());
            ++stop_step;
          }
        }
      });
    }

    void SendMsg(const std::shared_ptr<Streambuf>& msg_buf_ptr) {
      auto self = shared_from_this();
      boost::asio::dispatch(
          session_socket_strand_,
          [this, self, msg_buf_ptr]() {
            if (state_.load() != SessionState::kStart) [[unlikely]] {
              MW_WARN("Tcp svr session is closed, will not send current msg.");
              return;
            }

            data_list_.emplace_back(msg_buf_ptr);
            send_sig_timer_.cancel();
          });
    }

    const ibmw::common::util::LoggerWrapper& GetLogger() const { return *logger_ptr_; }

    Tcp::socket& Socket() { return sock_; }

    std::string_view RemoteAddr() const { return remote_addr_; }

    bool IsRunning() const { return state_.load() == SessionState::kStart; }

   private:
    enum class SessionState : uint32_t {
      kPreInit,
      kInit,
      kStart,
      kShutdown,
    };

    // IO CTX
    std::shared_ptr<IOCtx> io_ptr_;
    Strand session_socket_strand_;
    Tcp::socket sock_;
    Timer send_sig_timer_;
    Strand session_mgr_strand_;
    Timer timer_;

    // Log handle
    std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_ptr_;

    // Msg processing handle
    std::shared_ptr<MsgHandle> msg_handle_ptr_;

    // Options
    std::shared_ptr<const SessionOptions> session_options_ptr_;

    // State
    std::atomic<SessionState> state_ = SessionState::kPreInit;

    // misc
    Tcp::endpoint remote_ep_;
    std::string remote_addr_;
    std::atomic_bool tick_has_data_ = false;
    std::list<std::shared_ptr<Streambuf>> data_list_;
  };

 private:
  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

  // IO CTX
  std::shared_ptr<IOCtx> io_ptr_;
  Strand mgr_strand_;       // Session pool operation strand
  Tcp::acceptor acceptor_;  // Listener
  Timer acceptor_timer_;    // The sleep timer when the connection is full
  Timer mgr_timer_;         // Timer to manage session pool

  // Log handle
  std::shared_ptr<ibmw::common::util::LoggerWrapper> logger_ptr_;

  // Msg processing handle
  std::shared_ptr<MsgHandle> msg_handle_ptr_;

  // Options
  Options options_;

  // State
  std::atomic<State> state_ = State::kPreInit;

  // Session management
  std::shared_ptr<const SessionOptions> session_options_ptr_;
  std::unordered_map<Tcp::endpoint, std::shared_ptr<Session>, AsioEndpointHash>
      session_ptr_map_;
};

}  // namespace ibmw::common::net
