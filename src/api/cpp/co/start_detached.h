// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#if defined(IBMW_EXECUTOR_USE_STDEXEC)

  #include <stdexec/execution.hpp>

namespace ibmw::co {

inline constexpr auto& StartDetached = stdexec::start_detached;

}  // namespace ibmw::co

#elif defined(IBMW_EXECUTOR_USE_SELFDEV_CORO)

  #include "ibmw_selfdev/coro/start_detached.h"

namespace ibmw::co {

/**
 * @brief Detach executes a coroutine. Use a global async scope that is
 * drained during static destruction at process exit (not at dlclose of a
 * package). Returns false when the scope is already closed (the work is
 * dropped). See ibmw_selfdev/coro/start_detached.h.
 */
inline constexpr auto& StartDetached = ibmw::selfdev::coro::StartDetached;

}  // namespace ibmw::co

#else

  #include <unifex/async_scope.hpp>
  #include <unifex/sync_wait.hpp>
  #include <unifex/then.hpp>

namespace ibmw::co {

/**
 * @brief Detach executes a coroutine. Use a global async_scope
 *
 * @tparam Sender
 * @param sender
 * @return true, for API parity with the selfdev branch (unifex's spawn does
 * not report a stopped scope; the work is then dropped silently).
 */
template <typename Sender>
  requires unifex::sender<Sender>
inline bool StartDetached(Sender&& sender) {
  struct AsyncScopeDeleter {
    void operator()(unifex::async_scope* p) {
      unifex::sync_wait(p->cleanup());
      delete p;
    }
  };
  static std::unique_ptr<unifex::async_scope, AsyncScopeDeleter> scope_ptr{
      new unifex::async_scope()};
  scope_ptr->spawn((Sender &&) sender);
  return true;
}

}  // namespace ibmw::co

#endif
