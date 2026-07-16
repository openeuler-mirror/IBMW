// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include "cpp/transport/transport_context.h"
#include "cpp/context/res/service.h"

namespace ibmw::context::concepts {

template <class R, class F, class... Args>
concept CallableR =
    std::invocable<F, Args...> &&
    (std::is_void_v<R> || std::convertible_to<std::invoke_result_t<F, Args...>, R>);

template <class F, class Signature>
struct FunctionMatcher;

template <class F, class R, class... Args>
struct FunctionMatcher<F, R(Args...)> {
  static constexpr bool value = CallableR<R, F, Args...>;
};

template <class F, class Signature>
concept Function = FunctionMatcher<F, Signature>::value;

template <class F, class T>
concept SubscriberFunction = Function<F, void(std::shared_ptr<const T>)>;

template <class F, class T>
concept SubscriberFunctionDeref = Function<F, void(const T &)>;

template <class F, class T>
concept SubscriberFunctionWithCtx = Function<F, void(ibmw::transport::ContextRef, std::shared_ptr<const T>)>;

template <class F, class T>
concept SubscriberFunctionDerefWithCtx = Function<F, void(ibmw::transport::ContextRef, const T &)>;

template <class F, class T>
concept SupportedSubscriber =
    SubscriberFunction<F, T> ||
    SubscriberFunctionDeref<F, T> ||
    SubscriberFunctionWithCtx<F, T> ||
    SubscriberFunctionDerefWithCtx<F, T>;

template <class F, class Q, class P>
concept ServerFunction =
    Function<F, ibmw::rpc::Status(const Q &, P &)>;
template <class F, class Q, class P>
concept ServerCoroutine =
    Function<F, ibmw::co::Task<ibmw::rpc::Status>(const Q &, P &)>;
template <class F, class Q, class P>
concept ServerRawCoroutine =
    Function<F, ibmw::co::Task<ibmw::rpc::Status>(const Q &, P &)>;
template <class F, class Q, class P>
concept ServerFunctionWithCtx =
    Function<F, ibmw::rpc::Status(ibmw::rpc::ContextRef, const Q &, P &)>;

template <class F, class Q, class P>
concept ServerCoroutineWithCtx =
    Function<F, ibmw::co::Task<ibmw::rpc::Status>(ibmw::rpc::ContextRef, const Q &, P &)>;

template <class F, class Q, class P>
concept ServerRawCoroutineWithCtx =
    Function<F, ibmw::co::Task<ibmw::rpc::Status>(ibmw::rpc::ContextRef, const Q &, P &)>;

template <class F, class Q, class P>
concept ServerFunctionReturnVoid =
    Function<F, void(const Q &, P &)>;

template <class F, class Q, class P>
concept ServerCoroutineReturnVoid =
    Function<F, ibmw::co::Task<void>(const Q &, P &)>;

template <class F, class Q, class P>
concept ServerFunctionReturnVoidWithCtx =
    Function<F, void(ibmw::rpc::ContextRef, const Q &, P &)>;

template <class F, class Q, class P>
concept ServerCoroutineReturnVoidWithCtx =
    Function<F, ibmw::co::Task<void>(ibmw::rpc::ContextRef, const Q &, P &)>;

template <class F, class Q, class P>
concept SupportedServer =
    ServerFunction<F, Q, P> or
    ServerCoroutine<F, Q, P> or
    ServerRawCoroutine<F, Q, P> or
    ServerFunctionWithCtx<F, Q, P> or
    ServerCoroutineWithCtx<F, Q, P> or
    ServerRawCoroutineWithCtx<F, Q, P> or
    ServerFunctionReturnVoid<F, Q, P> or
    ServerCoroutineReturnVoid<F, Q, P> or
    ServerFunctionReturnVoidWithCtx<F, Q, P> or
    ServerCoroutineReturnVoidWithCtx<F, Q, P>;

template <class F, class Q, class P>
concept RawClient =
    Function<F, ibmw::co::Task<ibmw::rpc::Status>(ibmw::rpc::ContextRef, const Q &, P &)>;

}  // namespace ibmw::context::concepts
