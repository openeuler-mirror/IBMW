// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

#include <sys/mman.h>

#include "cpp/core.h"
#include "cpp/module_base.h"
#include "cpp/transport/loaned_subscriber.h"

namespace ibmw::transport {
namespace {

constexpr size_t kRuntimeAccessorCount = 7;

const void* RecordRuntimeAccessor(void* ctx, size_t index) {
  auto* values = static_cast<int*>(ctx);
  values[index] = ++values[kRuntimeAccessorCount];
  return index == 3 ? nullptr : reinterpret_cast<const void*>(0x1);
}

static_assert(!std::is_copy_constructible_v<ReceivedLoan>);
static_assert(!std::is_copy_assignable_v<ReceivedLoan>);
static_assert(std::is_move_constructible_v<ReceivedLoan>);
static_assert(!std::is_copy_constructible_v<LoanedSubscription>);
static_assert(std::is_move_constructible_v<LoanedSubscription>);

void ExpectRuntimeRejected(const ibmw_core_base_t* base, std::string_view message) {
  try {
    Runtime runtime(base);
    FAIL() << "Runtime accepted the core";
  } catch (const std::invalid_argument& e) {
    EXPECT_NE(std::string_view(e.what()).find(message), std::string_view::npos) << e.what();
  }
}

class SetupCountingModule : public Module {
 public:
  ModuleDescriptor Info() const override { return {}; }
  bool Setup(Runtime) override {
    ++setup_calls;
    return true;
  }
  bool Run() override { return true; }
  void Stop() override {}

  int setup_calls = 0;
};

// API-048: a module rejecting the core fails its setup; the host process lives.
TEST(RuntimeV2Abi, ModuleSetupFailsOnIncompatibleCoreWithoutAborting) {
  ibmw_core_base_t base{};
  base.abi_version = 1;

  SetupCountingModule module;
  const auto* handle = module.NativeHandle();
  EXPECT_FALSE(handle->setup(handle->ctx, &base));
  EXPECT_EQ(module.setup_calls, 0);
}

TEST(RuntimeV2Abi, RejectsV1BeforeCallingTransportAccessor) {
  auto* transport_access_calls = static_cast<int*>(mmap(
      nullptr, sizeof(int), PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  ASSERT_NE(transport_access_calls, MAP_FAILED);
  *transport_access_calls = 0;

  ibmw_core_base_t base{};
  base.abi_version = 1;
  base.ctx = transport_access_calls;
  base.transport_handle = [](void* ctx) -> const ibmw_transport_handle_base_t* {
    ++*static_cast<int*>(ctx);
    return nullptr;
  };

  ExpectRuntimeRejected(&base, "ABI version mismatch");
  EXPECT_EQ(*transport_access_calls, 0);
  EXPECT_EQ(munmap(transport_access_calls, sizeof(int)), 0);
}

TEST(RuntimeV2Abi, RejectsMalformedCurrentVersionBeforeAnyAccessorCall) {
  constexpr size_t kAccessorCount = 7;
  auto* accessor_calls = static_cast<int*>(mmap(
      nullptr, sizeof(int) * kAccessorCount, PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  ASSERT_NE(accessor_calls, MAP_FAILED);
  for (size_t i = 0; i < kAccessorCount; ++i) accessor_calls[i] = 0;

  ibmw_core_base_t base{};
  base.abi_version = IBMW_CORE_ABI_VERSION;
  base.struct_size = sizeof(ibmw_core_base_t);
  base.info = [](void*) { return ibmw_module_info_t{}; };
  base.configurator = [](void* ctx) -> const ibmw_configurator_base_t* {
    ++static_cast<int*>(ctx)[0];
    return nullptr;
  };
  base.executor_bus = [](void* ctx) -> const ibmw_executor_bus_base_t* {
    ++static_cast<int*>(ctx)[1];
    return nullptr;
  };
  base.logger = [](void* ctx) -> const ibmw_logger_base_t* {
    ++static_cast<int*>(ctx)[2];
    return nullptr;
  };
  base.memory_handle = [](void* ctx) -> const ibmw_allocator_base_t* {
    ++static_cast<int*>(ctx)[3];
    return nullptr;
  };
  base.service_handle = [](void* ctx) -> const ibmw_service_handle_base_t* {
    ++static_cast<int*>(ctx)[4];
    return nullptr;
  };
  base.transport_handle = [](void* ctx) -> const ibmw_transport_handle_base_t* {
    ++static_cast<int*>(ctx)[5];
    return nullptr;
  };
  base.parameter_handle = nullptr;
  base.ctx = accessor_calls;

  ExpectRuntimeRejected(&base, "Malformed current-version core ABI");
  for (size_t i = 0; i < kAccessorCount; ++i) EXPECT_EQ(accessor_calls[i], 0);
  EXPECT_EQ(munmap(accessor_calls, sizeof(int) * kAccessorCount), 0);
}

TEST(RuntimeV2Abi, RejectsNullReturnedSubHandleAfterValidFunctionTable) {
  auto* calls = static_cast<int*>(mmap(
      nullptr, sizeof(int) * (kRuntimeAccessorCount + 1), PROT_READ | PROT_WRITE,
      MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  ASSERT_NE(calls, MAP_FAILED);
  for (size_t i = 0; i <= kRuntimeAccessorCount; ++i) calls[i] = 0;
  ibmw_core_base_t base{};
  base.abi_version = IBMW_CORE_ABI_VERSION;
  base.struct_size = sizeof(ibmw_core_base_t);
  base.info = [](void*) { return ibmw_module_info_t{}; };
  base.configurator = [](void* ctx) -> const ibmw_configurator_base_t* {
    return static_cast<const ibmw_configurator_base_t*>(RecordRuntimeAccessor(ctx, 0));
  };
  base.executor_bus = [](void* ctx) -> const ibmw_executor_bus_base_t* {
    return static_cast<const ibmw_executor_bus_base_t*>(RecordRuntimeAccessor(ctx, 1));
  };
  base.logger = [](void* ctx) -> const ibmw_logger_base_t* {
    return static_cast<const ibmw_logger_base_t*>(RecordRuntimeAccessor(ctx, 2));
  };
  base.memory_handle = [](void* ctx) -> const ibmw_allocator_base_t* {
    return static_cast<const ibmw_allocator_base_t*>(RecordRuntimeAccessor(ctx, 3));
  };
  base.service_handle = [](void* ctx) -> const ibmw_service_handle_base_t* {
    return static_cast<const ibmw_service_handle_base_t*>(RecordRuntimeAccessor(ctx, 4));
  };
  base.transport_handle = [](void* ctx) -> const ibmw_transport_handle_base_t* {
    return static_cast<const ibmw_transport_handle_base_t*>(RecordRuntimeAccessor(ctx, 5));
  };
  base.parameter_handle = [](void* ctx) -> const ibmw_parameter_handle_base_t* {
    return static_cast<const ibmw_parameter_handle_base_t*>(RecordRuntimeAccessor(ctx, 6));
  };
  base.ctx = calls;

  ExpectRuntimeRejected(&base, "required accessor returned null");
  EXPECT_EQ(calls[kRuntimeAccessorCount], static_cast<int>(kRuntimeAccessorCount));
  for (size_t i = 0; i < kRuntimeAccessorCount; ++i) EXPECT_EQ(calls[i], i + 1);
  EXPECT_EQ(munmap(calls, sizeof(int) * (kRuntimeAccessorCount + 1)), 0);
}

// API-024: a table smaller than this SDK's layout is rejected, not read past its end.
TEST(RuntimeV3Abi, RejectsUndersizedCoreStructSize) {
  ibmw_core_base_t base{};
  base.abi_version = IBMW_CORE_ABI_VERSION;
  base.struct_size = sizeof(ibmw_core_base_t) - 1;
  ExpectRuntimeRejected(&base, "Malformed core ABI: struct_size=");
}

template <class Table>
const Table* FullTable() {
  static const Table table = [] {
    Table t{};
    t.struct_size = sizeof(Table);
    return t;
  }();
  return &table;
}

TEST(RuntimeV3Abi, RejectsUndersizedSubHandleTable) {
  static const ibmw_logger_base_t short_logger = [] {
    ibmw_logger_base_t t{};
    t.struct_size = sizeof(ibmw_logger_base_t) - sizeof(void*);
    return t;
  }();
  ibmw_core_base_t base{};
  base.abi_version = IBMW_CORE_ABI_VERSION;
  base.struct_size = sizeof(ibmw_core_base_t);
  base.info = [](void*) { return ibmw_module_info_t{}; };
  base.configurator = [](void*) { return FullTable<ibmw_configurator_base_t>(); };
  base.executor_bus = [](void*) { return FullTable<ibmw_executor_bus_base_t>(); };
  base.logger = [](void*) { return &short_logger; };
  base.memory_handle = [](void*) { return FullTable<ibmw_allocator_base_t>(); };
  base.service_handle = [](void*) { return FullTable<ibmw_service_handle_base_t>(); };
  base.transport_handle = [](void*) { return FullTable<ibmw_transport_handle_base_t>(); };
  base.parameter_handle = [](void*) { return FullTable<ibmw_parameter_handle_base_t>(); };
  ExpectRuntimeRejected(&base, "a handle table is smaller than expected");

  base.logger = [](void*) { return FullTable<ibmw_logger_base_t>(); };
  EXPECT_NO_THROW(Runtime runtime(&base));
}

ibmw_transport_sender_base_t UsableSender() {
  ibmw_transport_sender_base_t t{};
  t.struct_size = sizeof(t);
  t.register_send_type = [](void*, const ibmw_type_support_base_t*) { return true; };
  t.send = [](void*, ibmw_string_view_t, const ibmw_transport_context_base_t*, const void*) {};
  t.get_topic = [](void*) { return ibmw_string_view_t{"topic", 5}; };
  t.merge_subscribe_context_to_publish_context = [](void*, const ibmw_transport_context_base_t*,
                                                    const ibmw_transport_context_base_t*) {};
  t.loan = [](void*, ibmw_string_view_t, size_t, LoanMode, ibmw_loaned_sample_handle_t*) {};
  t.publish_loaned = [](void*, ibmw_loaned_sample_handle_t*) {};
  t.release_loaned = [](void*, ibmw_loaned_sample_handle_t*) {};
  return t;
}

ibmw_transport_receiver_base_t UsableReceiver() {
  ibmw_transport_receiver_base_t t{};
  t.struct_size = sizeof(t);
  t.on_message = [](void*, const ibmw_type_support_base_t*, ibmw_function_base_t*) { return true; };
  t.get_topic = [](void*) { return ibmw_string_view_t{"topic", 5}; };
  return t;
}

TEST(TransportV3Abi, UndersizedSenderOrReceiverIsNotReturned) {
  static ibmw_transport_sender_base_t sender = UsableSender();
  static ibmw_transport_receiver_base_t receiver = UsableReceiver();
  ibmw_transport_handle_base_t transport_base{};
  transport_base.struct_size = sizeof(transport_base);
  transport_base.get_sender = [](void*, ibmw_string_view_t) -> const ibmw_transport_sender_base_t* { return &sender; };
  transport_base.get_receiver = [](void*, ibmw_string_view_t) -> const ibmw_transport_receiver_base_t* { return &receiver; };
  TransportRef transport(&transport_base);

  sender.struct_size = sizeof(sender) - 1;
  receiver.struct_size = 0;
  EXPECT_FALSE(transport.GetPublisher("topic"));
  EXPECT_FALSE(transport.GetSubscriber("topic"));

  sender.struct_size = sizeof(sender);
  receiver.struct_size = sizeof(receiver);
  EXPECT_TRUE(transport.GetPublisher("topic"));
  EXPECT_TRUE(transport.GetSubscriber("topic"));
}

// API-049: a table missing an entry the wrappers call unconditionally is
// refused when obtained, so no call can jump through a null pointer.
TEST(TransportV3Abi, SenderOrReceiverMissingRequiredEntryIsNotReturned) {
  static ibmw_transport_sender_base_t sender{};
  static ibmw_transport_receiver_base_t receiver{};
  ibmw_transport_handle_base_t transport_base{};
  transport_base.struct_size = sizeof(transport_base);
  transport_base.get_sender = [](void*, ibmw_string_view_t) -> const ibmw_transport_sender_base_t* { return &sender; };
  transport_base.get_receiver = [](void*, ibmw_string_view_t) -> const ibmw_transport_receiver_base_t* { return &receiver; };
  TransportRef transport(&transport_base);

  const auto sender_without = [&](auto clear) {
    sender = UsableSender();
    clear(sender);
    return static_cast<bool>(transport.GetPublisher("topic"));
  };
  EXPECT_TRUE(sender_without([](auto&) {}));
  EXPECT_FALSE(sender_without([](auto& t) { t.register_send_type = nullptr; }));
  EXPECT_FALSE(sender_without([](auto& t) { t.send = nullptr; }));
  EXPECT_FALSE(sender_without([](auto& t) { t.get_topic = nullptr; }));
  EXPECT_FALSE(sender_without([](auto& t) { t.merge_subscribe_context_to_publish_context = nullptr; }));
  EXPECT_FALSE(sender_without([](auto& t) { t.loan = nullptr; }));
  EXPECT_FALSE(sender_without([](auto& t) { t.publish_loaned = nullptr; }));
  EXPECT_FALSE(sender_without([](auto& t) { t.release_loaned = nullptr; }));

  const auto receiver_without = [&](auto clear) {
    receiver = UsableReceiver();
    clear(receiver);
    return static_cast<bool>(transport.GetSubscriber("topic"));
  };
  EXPECT_TRUE(receiver_without([](auto&) {}));
  EXPECT_FALSE(receiver_without([](auto& t) { t.on_message = nullptr; }));
  EXPECT_FALSE(receiver_without([](auto& t) { t.get_topic = nullptr; }));
  // Optional entries stay optional.
  EXPECT_TRUE(receiver_without([](auto& t) { t.attach_loaned = nullptr; }));
}

ReceivedLoan MakeLoan(int& releases) {
  SubscriberReleaseCallback callback([&releases] { ++releases; });
  return ReceivedLoan(reinterpret_cast<const void*>(0x1234), 8,
                      callback.NativeHandle());
}

TEST(ReceivedLoan, ExplicitDestructorMoveAndDoubleReleaseAreExactlyOnce) {
  int releases = 0;
  {
    auto loan = MakeLoan(releases);
    EXPECT_EQ(loan.Data(), reinterpret_cast<const void*>(0x1234));
    loan.Release();
    loan.Release();
  }
  EXPECT_EQ(releases, 1);

  {
    auto source = MakeLoan(releases);
    auto destination = std::move(source);
    EXPECT_FALSE(source);
    EXPECT_TRUE(destination);
  }
  EXPECT_EQ(releases, 2);
}

struct FakeSubscription {
  int stops = 0;
  int closes = 0;
  int destroys = 0;
  uint64_t timeout_ms = 0;
  bool close_result = true;
  ibmw_loaned_subscription_base_t base{};

  FakeSubscription() {
    base.stop = [](void* ctx) { ++static_cast<FakeSubscription*>(ctx)->stops; };
    base.close = [](void* ctx, uint64_t timeout_ms) {
        auto* self = static_cast<FakeSubscription*>(ctx);
        ++self->closes;
        self->timeout_ms = timeout_ms;
        return self->close_result;
      };
    base.destroy = [](void* ctx) {
      ++static_cast<FakeSubscription*>(ctx)->destroys;
    };
    base.ctx = this;
  }
};

struct FakeReceiver {
  FakeSubscription subscription;
  LoanedMessageCallback attached_storage;
  ibmw_function_base_t* attached_callback = nullptr;
  LoanedMessageCallback retained_storage;
  ibmw_function_base_t* retained_callback = nullptr;
  ibmw_transport_receiver_base_t base{};

  FakeReceiver() {
    base.ctx = this;
    base.attach_loaned = [](void* ctx, const ibmw_type_support_base_t*,
                            ibmw_function_base_t* callback) {
      auto* self = static_cast<FakeReceiver*>(ctx);
      self->attached_storage = LoanedMessageCallback(callback);
      self->attached_callback = self->attached_storage.NativeHandle();
      return true;
    };
    base.attach_loaned_retained = [](void* ctx,
                                     const ibmw_type_support_base_t*,
                                     ibmw_function_base_t* callback,
                                     ibmw_retained_loan_options_t,
                                     ibmw_loaned_subscription_base_t** out) {
      auto* self = static_cast<FakeReceiver*>(ctx);
      self->retained_storage = LoanedMessageCallback(callback);
      self->retained_callback = self->retained_storage.NativeHandle();
      *out = &self->subscription.base;
      return true;
    };
  }
};

TEST(LoanedSubscription, MoveCloseStopAndResetForwardToCHandle) {
  FakeReceiver fake;
  ReceiverRef receiver(&fake.base);
  LoanedMessageCallback callback(
      [](const ibmw_transport_context_base_t*, const void*, size_t,
         ibmw_function_base_t*) {});
  LoanedSubscription first;
  ASSERT_TRUE(receiver.AttachLoanedRetained(
      nullptr, std::move(callback), &first, RetainedLoanOptions{2}));

  LoanedSubscription second(std::move(first));
  EXPECT_FALSE(first);
  second.Stop();
  EXPECT_TRUE(second.Close(std::chrono::milliseconds(17)));
  EXPECT_EQ(fake.subscription.stops, 2);
  EXPECT_EQ(fake.subscription.closes, 1);
  EXPECT_EQ(fake.subscription.timeout_ms, 17u);
  second.Reset();
  second.Reset();
  EXPECT_EQ(fake.subscription.destroys, 1);
}

TEST(TransportV2MalformedProvider, MissingNewFunctionPointersFailClosed) {
  ibmw_transport_sender_base_t sender_base{};
  SenderRef sender(&sender_base);
  EXPECT_FALSE(sender.RegisterLoanedPodTopic("Pod", sizeof(uint32_t),
                                             alignof(uint32_t)));

  ibmw_transport_receiver_base_t receiver_base{};
  ReceiverRef receiver(&receiver_base);
  LoanedSubscription subscription;
  LoanedMessageCallback callback(
      [](const ibmw_transport_context_base_t*, const void*, size_t,
         ibmw_function_base_t*) {});
  EXPECT_FALSE(receiver.AttachLoanedRetained(
      nullptr, std::move(callback), &subscription, RetainedLoanOptions{1}));
}

TEST(TransportV2MalformedProvider, MissingSenderAndReceiverAccessorsFailClosed) {
  ibmw_transport_handle_base_t transport_base{};
  TransportRef transport(&transport_base);
  EXPECT_FALSE(transport.GetPublisher("topic"));
  EXPECT_FALSE(transport.GetSubscriber("topic"));
}

TEST(TransportV2MalformedProvider, MalformedSubscriptionVtableFailsClosed) {
  int destroys = 0;
  ibmw_loaned_subscription_base_t subscription_base{};
  subscription_base.ctx = &destroys;
  subscription_base.stop = [](void*) {};
  subscription_base.close = [](void*, uint64_t) { return true; };
  subscription_base.destroy = [](void* ctx) { ++*static_cast<int*>(ctx); };

  ibmw_transport_receiver_base_t receiver_base{};
  receiver_base.ctx = &subscription_base;
  receiver_base.attach_loaned_retained =
      [](void* ctx, const ibmw_type_support_base_t*, ibmw_function_base_t*,
         ibmw_retained_loan_options_t,
         ibmw_loaned_subscription_base_t** out) {
        *out = static_cast<ibmw_loaned_subscription_base_t*>(ctx);
        return true;
      };
  ReceiverRef receiver(&receiver_base);

  const auto attach = [&] {
    LoanedSubscription subscription;
    LoanedMessageCallback callback(
        [](const ibmw_transport_context_base_t*, const void*, size_t,
           ibmw_function_base_t*) {});
    EXPECT_FALSE(receiver.AttachLoanedRetained(
        nullptr, std::move(callback), &subscription, RetainedLoanOptions{1}));
    EXPECT_FALSE(subscription);
  };

  subscription_base.stop = nullptr;
  attach();
  EXPECT_EQ(destroys, 1);
  subscription_base.stop = [](void*) {};
  subscription_base.close = nullptr;
  attach();
  EXPECT_EQ(destroys, 2);
  subscription_base.close = [](void*, uint64_t) { return true; };
  subscription_base.destroy = nullptr;
  attach();
  EXPECT_EQ(destroys, 2);
}

TEST(TransportV2MalformedProvider,
     FalseAttachWithNativeSubscriptionDestroysExactlyOnce) {
  int destroys = 0;
  ibmw_loaned_subscription_base_t subscription_base{};
  subscription_base.ctx = &destroys;
  subscription_base.destroy = [](void* ctx) { ++*static_cast<int*>(ctx); };

  ibmw_transport_receiver_base_t receiver_base{};
  receiver_base.ctx = &subscription_base;
  receiver_base.attach_loaned_retained =
      [](void* ctx, const ibmw_type_support_base_t*, ibmw_function_base_t*,
         ibmw_retained_loan_options_t,
         ibmw_loaned_subscription_base_t** out) {
        *out = static_cast<ibmw_loaned_subscription_base_t*>(ctx);
        return false;
      };

  ReceiverRef receiver(&receiver_base);
  LoanedSubscription subscription;
  LoanedMessageCallback callback(
      [](const ibmw_transport_context_base_t*, const void*, size_t,
         ibmw_function_base_t*) {});
  EXPECT_FALSE(receiver.AttachLoanedRetained(
      nullptr, std::move(callback), &subscription, RetainedLoanOptions{1}));
  EXPECT_FALSE(subscription);
  EXPECT_EQ(destroys, 1);
}

struct Pod {
  uint32_t value;
};

TEST(LoanedSubscriber, AttachLeaseBytesTransfersReleaseOwnership) {
  FakeReceiver fake;
  ReceiverRef receiver(&fake.base);
  LoanedSubscription subscription;
  ReceivedLoan retained;
  size_t seen_size = 0;

  ASSERT_TRUE(LoanedSubscriber<Pod>::AttachLeaseBytes(
      receiver, "Pod",
      [&retained, &seen_size](const Pod&, size_t size, ReceivedLoan loan) {
        seen_size = size;
        retained = std::move(loan);
      },
      &subscription, RetainedLoanOptions{1}));

  int releases = 0;
  SubscriberReleaseCallback release([&releases] { ++releases; });
  Pod pod{42};
  LoanedMessageCallback callback(fake.retained_callback);
  callback(nullptr, &pod, sizeof(pod), release.NativeHandle());

  EXPECT_EQ(seen_size, sizeof(pod));
  EXPECT_EQ(retained.Data(), &pod);
  EXPECT_EQ(releases, 0);
  retained.Release();
  EXPECT_EQ(releases, 1);
}

TEST(LoanedSubscriber, ThrowingAttachLeaseBytesReleasesExactlyOnce) {
  FakeReceiver fake;
  LoanedSubscription subscription;
  ASSERT_TRUE(LoanedSubscriber<Pod>::AttachLeaseBytes(
      ReceiverRef(&fake.base), "Pod",
      [](const Pod&, size_t, ReceivedLoan) {
        throw std::runtime_error("expected");
      },
      &subscription, RetainedLoanOptions{1}));

  int releases = 0;
  SubscriberReleaseCallback release([&releases] { ++releases; });
  Pod pod{42};
  LoanedMessageCallback callback(fake.retained_callback);
  EXPECT_NO_THROW(
      callback(nullptr, &pod, sizeof(pod), release.NativeHandle()));
  EXPECT_EQ(releases, 1);
}

TEST(LoanedSubscriber, ThrowingAttachCallbacksDoNotEscapeNoexceptBridge) {
  FakeReceiver fake;
  ASSERT_TRUE(LoanedSubscriber<Pod>::Attach(
      ReceiverRef(&fake.base), "Pod",
      [](const Pod&) { throw std::runtime_error("expected"); }));
  Pod pod{42};
  LoanedMessageCallback callback(fake.attached_callback);
  EXPECT_NO_THROW(callback(nullptr, &pod, sizeof(pod), nullptr));

  ASSERT_TRUE(LoanedSubscriber<Pod>::AttachBytes(
      ReceiverRef(&fake.base), "Pod",
      [](const Pod&, size_t) { throw std::runtime_error("expected"); }));
  callback = LoanedMessageCallback(fake.attached_callback);
  EXPECT_NO_THROW(callback(nullptr, &pod, sizeof(pod), nullptr));
}

TEST(LoanedSubscriber, ThrowingAttachDispatcherReturnsFalseFromNoexceptApi) {
  ibmw_transport_receiver_base_t receiver{};
  receiver.attach_loaned = [](void*, const ibmw_type_support_base_t*,
                              ibmw_function_base_t*) -> bool {
    throw std::runtime_error("dispatcher failure");
  };
  receiver.attach_loaned_retained = [](
      void*, const ibmw_type_support_base_t*, ibmw_function_base_t*,
      ibmw_retained_loan_options_t,
      ibmw_loaned_subscription_base_t**) -> bool {
    throw std::runtime_error("dispatcher failure");
  };

  EXPECT_FALSE(LoanedSubscriber<Pod>::Attach(
      ReceiverRef(&receiver), "Pod", [](const Pod&) {}));
  LoanedSubscription subscription;
  EXPECT_FALSE(LoanedSubscriber<Pod>::AttachLeaseBytes(
      ReceiverRef(&receiver), "Pod", [](const Pod&, size_t, ReceivedLoan) {},
      &subscription, RetainedLoanOptions{1}));
}

TEST(LoanedSubscriber, TypeNameAllocationFailureReturnsFalseFromNoexceptApi) {
  bool dispatcher_called = false;
  ibmw_transport_receiver_base_t receiver{};
  receiver.ctx = &dispatcher_called;
  receiver.attach_loaned = [](void* ctx, const ibmw_type_support_base_t*,
                              ibmw_function_base_t*) -> bool {
    *static_cast<bool*>(ctx) = true;
    return true;
  };

  const std::string_view oversized_type("x",
                                        std::numeric_limits<size_t>::max());
  EXPECT_FALSE(LoanedSubscriber<Pod>::Attach(
      ReceiverRef(&receiver), oversized_type, [](const Pod&) {}));
  EXPECT_FALSE(dispatcher_called);
}

// API-030: the dispatcher keeps the type support pointer in every driver's
// handler info, so it must outlive the call and stay complete.
TEST(LoanedSubscriber, PodTypeSupportIsStableAndComplete) {
  struct Captured {
    const ibmw_type_support_base_t* type_support = nullptr;
  };
  auto capture = [](void* ctx, const ibmw_type_support_base_t* type_support,
                    ibmw_function_base_t*) -> bool {
    static_cast<Captured*>(ctx)->type_support = type_support;
    return true;
  };
  auto attach = [&](std::string_view msg_type) {
    Captured captured;
    ibmw_transport_receiver_base_t receiver{};
    receiver.ctx = &captured;
    receiver.attach_loaned = capture;
    EXPECT_TRUE(LoanedSubscriber<Pod>::Attach(ReceiverRef(&receiver), msg_type,
                                              [](const Pod&) {}));
    return captured.type_support;
  };
  auto name_of = [](const ibmw_type_support_base_t* ts) {
    const auto view = ts->type_name(ts->ctx);
    return std::string(view.str, view.len);
  };

  const auto* first = attach("pod/A");
  ASSERT_NE(first, nullptr);
  const auto* second = attach("pod/B");
  const ibmw_type_support_base_t* from_thread = nullptr;
  std::thread([&] { from_thread = attach("pod/C"); }).join();

  EXPECT_EQ(name_of(first), "pod/A");
  EXPECT_EQ(name_of(second), "pod/B");
  EXPECT_EQ(name_of(from_thread), "pod/C");
  EXPECT_EQ(attach("pod/A"), first);

  for (const auto* ts : {first, second, from_thread}) {
    ASSERT_NE(ts->create, nullptr);
    ASSERT_NE(ts->destroy, nullptr);
    ASSERT_NE(ts->copy, nullptr);
    ASSERT_NE(ts->move, nullptr);
    ASSERT_NE(ts->serialize, nullptr);
    ASSERT_NE(ts->deserialize, nullptr);
    ASSERT_NE(ts->serialization_types_supported_num, nullptr);
    ASSERT_NE(ts->serialization_types_supported_list, nullptr);
    ASSERT_NE(ts->custom_type_support_ptr, nullptr);
  }
  void* msg = first->create(first->ctx);
  ASSERT_NE(msg, nullptr);
  void* other = first->create(first->ctx);
  first->copy(first->ctx, msg, other);
  first->destroy(first->ctx, other);
  first->destroy(first->ctx, msg);
  EXPECT_EQ(first->serialization_types_supported_num(first->ctx), 0u);
  EXPECT_FALSE(first->serialize(first->ctx, {}, nullptr, nullptr, nullptr));
}

}  // namespace
}  // namespace ibmw::transport
