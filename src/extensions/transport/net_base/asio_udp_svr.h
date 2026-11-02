// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>

#include <boost/asio.hpp>

#include "net_base/asio_endpoint_hash.h"
#include "base/log_util.h"
#include "base/string_util.h"

namespace ibmw::common::net {

class AsioUdpServer : public std::enable_shared_from_this<AsioUdpServer> {
 public:
  using IOCtx = boost::asio::io_context;
  using Udp = boost::asio::ip::udp;
  using Strand = boost::asio::strand<IOCtx::executor_type>;
  using Timer = boost::asio::steady_timer;
  using Streambuf = boost::asio::streambuf;

  template <class T>
  using Awaitable = boost::asio::awaitable<T>;

  using MsgHandle =
      std::function<void(const Udp::endpoint&, const std::shared_ptr<Streambuf>&)>;

  struct Options {
    /// Listening address
    Udp::endpoint ep = Udp::endpoint{boost::asio::ip::address_v4::loopback(), 53927};

    /// Maximum number of sessions (remote endpoints). When reached, datagrams
    /// from new endpoints are dropped; known endpoints are still served.
    size_t max_session_num = 1000000;

    /// Managing coroutine timer intervals
    std::chrono::nanoseconds mgr_timer_dt = std::chrono::seconds(10);

    /// Maximum time without data
    std::chrono::nanoseconds max_no_data_duration = std::chrono::seconds(300);

    /// The maximum length of each packet. Cannot be greater than 65507
    size_t max_package_size = 1024;

    static Options Verify(const Options& verify_options) {
      Options options(verify_options);

      if (options.max_session_num < 1) options.max_session_num = 1;

      if (options.mgr_timer_dt < std::chrono::milliseconds(100))
        options.mgr_timer_dt = std::chrono::milliseconds(100);

      if (options.max_no_data_duration < std::chrono::seconds(10))
        options.max_no_data_duration = std::chrono::seconds(10);

      if (options.max_package_size > 65507) options.max_package_size = 65507;

      return options;
    }
  };

  explicit AsioUdpServer(const std::shared_ptr<IOCtx>& io_ptr)
      : io_ptr_(io_ptr),
        mgr_strand_(boost::asio::make_strand(*io_ptr_)),
        mgr_timer_(mgr_strand_),
        sock_(mgr_strand_),  // used only on the mgr strand
        logger_ptr_(std::make_shared<ibmw::common::util::LoggerWrapper>()) {}

  ~AsioUdpServer() = default;

  AsioUdpServer(const AsioUdpServer&) = delete;
  AsioUdpServer& operator=(const AsioUdpServer&) = delete;

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
  }

  void Start() {
    MW_REQUIRE(
        state_.load() == State::kInit,
        "Method can only be called when state is 'Init'.");

    // Bind synchronously (no async operation exists yet) so a failure is
    // reported to the caller instead of being lost in a detached coroutine.
    boost::system::error_code ec;
    sock_.open(options_.ep.protocol(), ec);
    if (!ec) sock_.bind(options_.ep, ec);
    if (ec) {
      boost::system::error_code ignored;
      sock_.close(ignored);
      MW_THROW("udp svr failed to bind {}: {}.",
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
              auto msg_buf = std::make_shared<boost::asio::streambuf>();
              boost::asio::ip::udp::endpoint remote_ep;
              // One spare byte: a datagram that fills it was longer than the
              // limit and has been truncated by the kernel, so drop it.
              size_t read_data_size = co_await sock_.async_receive_from(
                  msg_buf->prepare(options_.max_package_size + 1), remote_ep,
                  boost::asio::use_awaitable);
              MW_TRACE("udp svr session async read {} bytes from {}",
                          read_data_size, ibmw::common::util::SSToString(remote_ep));
              if (read_data_size > options_.max_package_size) [[unlikely]] {
                OnTruncatedDatagram(remote_ep);
                continue;
              }
              msg_buf->commit(read_data_size);

              std::shared_ptr<Session> session_ptr;

              auto finditr = session_ptr_map_.find(remote_ep);
              if (finditr != session_ptr_map_.end()) {
                session_ptr = finditr->second;
              } else if (session_ptr_map_.size() >= options_.max_session_num) [[unlikely]] {
                // Not stopping the receive loop: that would also block the
                // known endpoints (TRANS-032).
                OnSessionTableFull(remote_ep);
                continue;
              } else {
                session_ptr = std::make_shared<Session>(
                    io_ptr_, logger_ptr_, msg_handle_ptr_, remote_ep);
                session_ptr->Initialize(session_options_ptr_);
                session_ptr->Start();
                session_ptr_map_.emplace(remote_ep, session_ptr);
              }

              session_ptr->HandleMsg(msg_buf);

            } catch (const std::exception& e) {
              MW_TRACE(
                  "udp svr accept connection get exception and exit, exception info: {}",
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
              if (session_ptr_map_.size() < options_.max_session_num) table_full_ = false;
            } catch (const std::exception& e) {
              MW_TRACE(
                  "udp svr timer get exception and exit, exception info: {}",
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
              sock_.shutdown(boost::asio::ip::udp::socket::shutdown_both);
              ++stop_step;
            case 2:
              sock_.cancel();
              ++stop_step;
            case 3:
              sock_.close();
              ++stop_step;
            case 4:
              mgr_timer_.cancel();
              ++stop_step;
            default:
              stop_step = 0;
              break;
          }
        } catch (const std::exception& e) {
          MW_WARN(
              "udp svr stop get exception at step {}, exception info: {}",
              stop_step, e.what());
          ++stop_step;
        }
      }

      for (auto& session_ptr : session_ptr_map_)
        session_ptr.second->Shutdown();

      session_ptr_map_.clear();
    });
  }

  const ibmw::common::util::LoggerWrapper& GetLogger() const { return *logger_ptr_; }

 private:
  struct SessionOptions {
    explicit SessionOptions(const Options& options)
        : max_no_data_duration(options.max_no_data_duration) {}

    std::chrono::nanoseconds max_no_data_duration;
  };

  class Session : public std::enable_shared_from_this<Session> {
   public:
    Session(const std::shared_ptr<IOCtx>& io_ptr,
            const std::shared_ptr<ibmw::common::util::LoggerWrapper>& logger_ptr,
            const std::shared_ptr<MsgHandle>& msg_handle_ptr,
            const Udp::endpoint& remote_ep)
        : io_ptr_(io_ptr),
          session_mgr_strand_(boost::asio::make_strand(*io_ptr_)),
          timer_(session_mgr_strand_),
          logger_ptr_(logger_ptr),
          msg_handle_ptr_(msg_handle_ptr),
          remote_ep_(remote_ep) {}

    ~Session() = default;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void Initialize(const std::shared_ptr<const SessionOptions>& session_options_ptr) {
      MW_REQUIRE(
          std::atomic_exchange(&state_, SessionState::kInit) == SessionState::kPreInit,
          "Method can only be called when state is 'PreInit'.");

      session_options_ptr_ = session_options_ptr;
    }

    void Start() {
      MW_REQUIRE(
          std::atomic_exchange(&state_, SessionState::kStart) == SessionState::kInit,
          "Method can only be called when state is 'Init'.");

      auto self = shared_from_this();

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
                      "udp svr session exit due to timeout({}ms), addr {}.",
                      std::chrono::duration_cast<std::chrono::milliseconds>(session_options_ptr_->max_no_data_duration).count(),
                      ibmw::common::util::SSToString(remote_ep_));
                  break;
                }
              }
            } catch (const std::exception& e) {
              MW_TRACE(
                  "udp svr session timer get exception and exit, addr {}, exception info: {}",
                  ibmw::common::util::SSToString(remote_ep_), e.what());
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
            MW_TRACE(
                "udp svr session mgr stop get exception at step {}, exception info: {}",
                stop_step, e.what());
            ++stop_step;
          }
        }
      });
    }

    void HandleMsg(const std::shared_ptr<boost::asio::streambuf>& msg_buf_ptr) {
      auto self = shared_from_this();
      boost::asio::dispatch(
          *io_ptr_,
          [this, self, msg_buf_ptr]() {
            tick_has_data_ = true;

            (*msg_handle_ptr_)(remote_ep_, msg_buf_ptr);
          });
    }

    const ibmw::common::util::LoggerWrapper& GetLogger() const { return *logger_ptr_; }

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
    Udp::endpoint remote_ep_;
    std::atomic_bool tick_has_data_ = false;
  };

 private:
  enum class State : uint32_t {
    kPreInit,
    kInit,
    kStart,
    kShutdown,
  };

  [[gnu::cold, gnu::noinline]] void OnTruncatedDatagram(const Udp::endpoint& remote_ep) {
    // Log the 1st, 2nd, 4th, 8th, ... drop so a flood cannot flood the log.
    const uint64_t count = ++truncated_count_;
    if ((count & (count - 1)) != 0) return;
    MW_WARN("udp svr dropped a datagram longer than max_package_size {} from {} ({} dropped so far).",
            options_.max_package_size, ibmw::common::util::SSToString(remote_ep), count);
  }

  [[gnu::cold, gnu::noinline]] void OnSessionTableFull(const Udp::endpoint& remote_ep) {
    if (table_full_) return;
    table_full_ = true;
    MW_WARN("udp svr session table is full ({} sessions), dropping datagrams from new endpoints such as {}.",
            session_ptr_map_.size(), ibmw::common::util::SSToString(remote_ep));
  }

  // IO CTX
  std::shared_ptr<IOCtx> io_ptr_;
  Strand mgr_strand_;
  Timer mgr_timer_;
  Udp::socket sock_;

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
  std::unordered_map<Udp::endpoint, std::shared_ptr<Session>, AsioEndpointHash>
      session_ptr_map_;
  bool table_full_ = false;  // warned for the current full episode (mgr strand)
  uint64_t truncated_count_ = 0;  // datagrams dropped as truncated (receive loop on mgr strand)
};

}  // namespace ibmw::common::net
