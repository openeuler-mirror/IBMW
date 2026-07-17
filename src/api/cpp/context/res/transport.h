// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/source_location.h"
#include "cpp/transport/transport_context.h"
#include "cpp/context/details/concepts.h"
#include "cpp/context/res/base.h"
#include "cpp/executor/executor.h"

namespace ibmw::context::res {

template <class T>
class Transport : public details::Base {
 public:
  using MessageType = T;
  using details::Base::Base;
};

template <class T>
class Publisher : public Transport<T> {
 public:
  void Send(const T& msg, ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current()) const;
  void Send(ibmw::transport::ContextRef ch_ctx, const T& msg, ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current()) const;
};

template <class T>
class Subscriber : public Transport<T> {
 public:
  template <concepts::SupportedSubscriber<T> TCallback>
  Subscriber<T> SubscribeOn(
      const ibmw::executor::ExecutorHandle& exe, TCallback callback, ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current()) const;

  template <concepts::SupportedSubscriber<T> TCallback>
  Subscriber<T> SubscribeInline(TCallback callback, ibmw::common::util::SourceLocation loc = ibmw::common::util::SourceLocation::current()) const;
};

}  // namespace ibmw::context::res
