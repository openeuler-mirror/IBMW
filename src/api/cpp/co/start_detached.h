// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#ifdef IBMW_EXECUTOR_USE_STDEXEC

  #include <stdexec/execution.hpp>

namespace ibmw::co {

inline constexpr auto& StartDetached = stdexec::start_detached;

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
 */
template <typename Sender>
  requires unifex::sender<Sender>
inline void StartDetached(Sender&& sender) {
  struct AsyncScopeDeleter {
    void operator()(unifex::async_scope* p) {
      unifex::sync_wait(p->cleanup());
      delete p;
    }
  };
  static std::unique_ptr<unifex::async_scope, AsyncScopeDeleter> scope_ptr{
      new unifex::async_scope()};
  scope_ptr->spawn((Sender &&) sender);
}

}  // namespace ibmw::co

#endif
