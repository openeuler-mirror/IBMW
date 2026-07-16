// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <any>
#include <string>
#include <string_view>
#include <utility>

#include "cpp/transport/transport_context.h"
#include "cpp/transport/transport_handle.h"
#include "cpp/context/transport_context.h"
#include "cpp/context/details/concepts.h"
#include "cpp/context/details/type_support.h"
#include "cpp/context/op_base.h"
#include "cpp/context/res/transport.h"

namespace ibmw::context {

class OpPub : public OpBase {
 public:
  using OpBase::OpBase;

  OpPub(Context& ctx, ibmw::common::util::SourceLocation loc) noexcept : OpBase(ctx, loc) {}

  template <class T>
  [[nodiscard]] res::Publisher<T> Init(std::string_view topic_name);

  template <class T>
  void Send(const res::Transport<T>& ch, const T& msg);

  template <class T>
  void Send(const res::Transport<T>& ch, ibmw::transport::ContextRef ch_ctx, const T& msg);

 private:
  template <class T>
  std::pair<res::Publisher<T>, TransportResource&> DoInit(std::string_view topic_name);

  template <class T>
  static typename Context::SendFunction<T> CreateSendFunction();
};

template <class T>
res::Publisher<T> OpPub::Init(std::string_view topic_name) {
  auto [transport, transport_ctx] = DoInit<T>(topic_name);
  transport_ctx.pub_f = CreateSendFunction<T>();
  return transport;
}

}  // namespace ibmw::context
