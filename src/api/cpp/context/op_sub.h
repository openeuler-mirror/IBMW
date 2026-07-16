// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <string_view>

#include "cpp/transport/transport_handle.h"
#include "cpp/context/details/concepts.h"
#include "cpp/context/op_base.h"
#include "cpp/context/res/transport.h"
#include "cpp/executor/executor.h"

namespace ibmw::context {

struct TransportResource;

class OpSub : public OpBase {
 public:
  using OpBase::OpBase;

  OpSub(Context& ctx, ibmw::common::util::SourceLocation loc) noexcept : OpBase(ctx, loc) {}

  template <class T>
  [[nodiscard]] res::Subscriber<T> Init(std::string_view topic_name);

  template <class T, concepts::SupportedSubscriber<T> TCallback>
  void SubscribeInline(const res::Subscriber<T>& ch, TCallback callback);

  template <class T, concepts::SupportedSubscriber<T> TCallback>
  void SubscribeOn(const res::Subscriber<T>& ch, ibmw::executor::ExecutorHandle exe, TCallback callback);

 private:
  template <class T>
  std::pair<res::Subscriber<T>, TransportResource&> DoInit(std::string_view topic_name);

  template <class T>
  static typename Context::SubscribeFunction<T> CreateSubscribeFunction();

  template <class T, concepts::SupportedSubscriber<T> TCallback>
  void DoOnMessage(const res::Subscriber<T>& ch, TCallback callback, ibmw::executor::ExecutorHandle exe);

  template <class T, class F>
  static bool RawOnMessage(
      ibmw::transport::ReceiverRef subscriber,
      ibmw::executor::ExecutorHandle exe,
      F&& callback);

  template <class T, concepts::SupportedSubscriber<T> F>
  static auto StandardizeSubscriber(F&& cb);
};

template <class T>
res::Subscriber<T> OpSub::Init(std::string_view topic_name) {
  auto [subscriber, transport_ctx] = DoInit<T>(topic_name);
  transport_ctx.sub_f = CreateSubscribeFunction<T>();
  return subscriber;
}

}  // namespace ibmw::context
