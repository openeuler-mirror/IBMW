// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Listener: futex-based wake-up, edge-triggered callbacks, synchronous
// detach, capacity limits, no lost wake-ups, zero idle wake-ups.

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "ibmw_selfdev/shm/shm.h"
#include "test_util.h"

namespace ibmw::selfdev::shm {
namespace {

using namespace std::chrono_literals;
using test::CheckPattern;
using test::FillPattern;
using test::UniqueUrl;
using test::WaitFor;

void PublishSeq(Publisher& pub, uint64_t seq) {
  auto l = pub.Loan(64);
  ASSERT_TRUE(l.ok()) << ErrorName(l.error());
  FillPattern(*l, 64, seq);
  pub.Publish(*l);
}

struct Counter {
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> taken{0};
  bool drain = true;
};

void CountAndDrain(Subscriber* s, void* ctx) {
  auto* c = static_cast<Counter*>(ctx);
  c->calls.fetch_add(1);
  if (!c->drain) return;
  while (auto t = s->Take()) {
    c->taken.fetch_add(1);
    s->Release(*t);
  }
}

PublisherOptions EagerPool() {
  PublisherOptions po;
  po.pools = {{64, 32}};
  return po;
}

// LS-01 A publish wakes the listener and runs the callback.
TEST(ShmListenerTest, CallbackOnData) {
  auto url = UniqueUrl("ls01");
  Publisher pub(url, EagerPool());
  Subscriber sub(url, SubscriberOptions{16, QueueFullPolicy::kDiscardOldest, 16});
  Listener listener;
  ASSERT_TRUE(listener.Valid());
  Counter c;
  ASSERT_TRUE(listener.Attach(sub, CountAndDrain, &c).ok());
  EXPECT_EQ(listener.Size(), 1u);
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }));
  for (uint64_t i = 1; i <= 5; ++i) {
    PublishSeq(pub, i);
    ASSERT_TRUE(WaitFor([&] { return c.taken.load() == i; })) << i;
  }
  EXPECT_GE(c.calls.load(), 5u);
}

// LS-02 Data already queued at attach time fires the callback once.
TEST(ShmListenerTest, FiresForDataWaitingAtAttach) {
  auto url = UniqueUrl("ls02");
  Publisher pub(url, EagerPool());
  Subscriber sub(url, SubscriberOptions{16, QueueFullPolicy::kDiscardOldest, 16});
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }));
  PublishSeq(pub, 1);
  ASSERT_TRUE(WaitFor([&] { return sub.HasData(); }));
  Listener listener;
  Counter c;
  ASSERT_TRUE(listener.Attach(sub, CountAndDrain, &c).ok());
  ASSERT_TRUE(WaitFor([&] { return c.taken.load() == 1; }));
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(c.calls.load(), 1u);
}

// LS-03 Edge-triggered: an undrained queue does not re-fire the callback
// until new data arrives (iceoryx DATA_RECEIVED semantics, no busy loop).
TEST(ShmListenerTest, EdgeTriggered) {
  auto url = UniqueUrl("ls03");
  Publisher pub(url, EagerPool());
  Subscriber sub(url, SubscriberOptions{16, QueueFullPolicy::kDiscardOldest, 16});
  Listener listener;
  Counter c;
  c.drain = false;
  ASSERT_TRUE(listener.Attach(sub, CountAndDrain, &c).ok());
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }));
  PublishSeq(pub, 1);
  ASSERT_TRUE(WaitFor([&] { return c.calls.load() == 1; }));
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(c.calls.load(), 1u);
  EXPECT_TRUE(sub.HasData());
  PublishSeq(pub, 2);
  ASSERT_TRUE(WaitFor([&] { return c.calls.load() == 2; }));
}

// LS-04 After Detach returns the callback is not running and never runs again.
TEST(ShmListenerTest, DetachIsSynchronous) {
  auto url = UniqueUrl("ls04");
  Publisher pub(url, EagerPool());
  Subscriber sub(url, SubscriberOptions{16, QueueFullPolicy::kDiscardOldest, 16});
  Listener listener;
  struct Ctx {
    std::atomic<bool> in_callback{false};
    std::atomic<bool> detached{false};
    std::atomic<int> violations{0};
    std::atomic<int> calls{0};
  } ctx;
  auto cb = [](Subscriber* s, void* p) {
    auto* x = static_cast<Ctx*>(p);
    if (x->detached.load()) x->violations.fetch_add(1);
    x->in_callback = true;
    x->calls.fetch_add(1);
    std::this_thread::sleep_for(20ms);
    while (auto t = s->Take()) s->Release(*t);
    x->in_callback = false;
  };
  ASSERT_TRUE(listener.Attach(sub, cb, &ctx).ok());
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }));
  PublishSeq(pub, 1);
  ASSERT_TRUE(WaitFor([&] { return ctx.in_callback.load(); }));
  listener.Detach(sub);
  EXPECT_FALSE(ctx.in_callback.load());
  ctx.detached = true;
  EXPECT_EQ(listener.Size(), 0u);
  for (uint64_t i = 2; i < 10; ++i) PublishSeq(pub, i);
  std::this_thread::sleep_for(50ms);
  EXPECT_EQ(ctx.violations.load(), 0);
  EXPECT_EQ(ctx.calls.load(), 1);
  // Re-attach works after detach.
  Counter c;
  ASSERT_TRUE(listener.Attach(sub, CountAndDrain, &c).ok());
  ASSERT_TRUE(WaitFor([&] { return c.calls.load() >= 1; }));
}

// LS-05 Detach and subscriber destruction from inside the callback.
TEST(ShmListenerTest, DetachFromCallback) {
  auto url = UniqueUrl("ls05");
  Publisher pub(url, EagerPool());
  Listener listener;
  struct Ctx {
    Listener* l;
    std::atomic<int> calls{0};
  } ctx{&listener};
  auto sub = std::make_unique<Subscriber>(url);
  auto cb = [](Subscriber* s, void* p) {
    auto* x = static_cast<Ctx*>(p);
    x->calls.fetch_add(1);
    x->l->Detach(*s);
  };
  ASSERT_TRUE(listener.Attach(*sub, cb, &ctx).ok());
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }));
  PublishSeq(pub, 1);
  ASSERT_TRUE(WaitFor([&] { return listener.Size() == 0; }));
  PublishSeq(pub, 2);
  std::this_thread::sleep_for(30ms);
  EXPECT_EQ(ctx.calls.load(), 1);
  // Destroying an attached subscriber detaches it.
  Counter c;
  ASSERT_TRUE(listener.Attach(*sub, CountAndDrain, &c).ok());
  sub.reset();
  EXPECT_EQ(listener.Size(), 0u);
}

// LS-06 Capacity and double-attach checks.
TEST(ShmListenerTest, AttachLimits) {
  auto url = UniqueUrl("ls06");
  ListenerOptions lo;
  lo.capacity = 2;
  Listener listener(lo);
  Listener other;
  Subscriber a(url), b(url), c(url), invalid("");
  Counter cnt;
  ASSERT_TRUE(listener.Attach(a, CountAndDrain, &cnt).ok());
  EXPECT_EQ(listener.Attach(a, CountAndDrain, &cnt).error(), Error::kInvalidArg);
  EXPECT_EQ(other.Attach(a, CountAndDrain, &cnt).error(), Error::kInvalidArg);  // owned by `listener`
  ASSERT_TRUE(listener.Attach(b, CountAndDrain, &cnt).ok());
  EXPECT_EQ(listener.Attach(c, CountAndDrain, &cnt).error(), Error::kNoMemory);
  EXPECT_EQ(listener.Attach(c, nullptr, &cnt).error(), Error::kInvalidArg);
  EXPECT_EQ(listener.Attach(invalid, CountAndDrain, &cnt).error(), Error::kInvalidArg);
  listener.Detach(c);  // not attached: no-op
  EXPECT_EQ(listener.Size(), 2u);
}

// LS-07 At most kMaxListenersPerProcess listeners per process.
TEST(ShmListenerTest, ProcessListenerLimit) {
  std::vector<std::unique_ptr<Listener>> ls;
  for (;;) {
    auto l = std::make_unique<Listener>();
    if (!l->Valid()) {
      EXPECT_EQ(l->InitError(), Error::kNoMemory);
      break;
    }
    ls.push_back(std::move(l));
    ASSERT_LE(ls.size(), kMaxListenersPerProcess);
  }
  EXPECT_EQ(ls.size(), kMaxListenersPerProcess);
  ls.pop_back();
  EXPECT_TRUE(Listener().Valid());  // slot reusable
}

// LS-08 No lost wake-ups: 4 producers x 25k lossless samples into 4
// subscribers on one listener; every sample is seen via callbacks only.
TEST(ShmListenerTest, NoLostWakeupsUnderStress) {
  constexpr int kPairs = 4;
  constexpr uint64_t kPer = 25000;
  std::vector<std::string> urls;
  std::vector<std::unique_ptr<Publisher>> pubs;
  std::vector<std::unique_ptr<Subscriber>> subs;
  PublisherOptions po = EagerPool();
  po.too_slow = TooSlowPolicy::kWaitForConsumer;
  for (int i = 0; i < kPairs; ++i) {
    urls.push_back(UniqueUrl("ls08"));
    pubs.push_back(std::make_unique<Publisher>(urls.back(), po));
    subs.push_back(std::make_unique<Subscriber>(urls.back(),
                                                SubscriberOptions{4, QueueFullPolicy::kBlockProducer, 4}));
  }
  Listener listener;
  std::vector<Counter> counters(kPairs);
  for (int i = 0; i < kPairs; ++i) ASSERT_TRUE(listener.Attach(*subs[i], CountAndDrain, &counters[i]).ok());
  for (auto& p : pubs) ASSERT_TRUE(WaitFor([&] { return p->HasSubscribers(); }));
  std::vector<std::thread> ts;
  for (int i = 0; i < kPairs; ++i) {
    ts.emplace_back([&, i] {
      for (uint64_t s = 1; s <= kPer; ++s) PublishSeq(*pubs[i], s);
    });
  }
  for (auto& t : ts) t.join();
  for (int i = 0; i < kPairs; ++i) {
    EXPECT_TRUE(WaitFor([&] { return counters[i].taken.load() == kPer; }, 20s))
        << i << ": " << counters[i].taken.load();
    EXPECT_EQ(pubs[i]->Stats().evictions, 0u);
  }
}

// LS-09 Idle listener and control thread cost zero wake-ups.
TEST(ShmListenerTest, IdleHasZeroWakeups) {
  auto url = UniqueUrl("ls09");
  Publisher pub(url, EagerPool());
  Subscriber sub(url);
  ListenerOptions lo;
  lo.thread_name = "shm_idle_ls09";
  Listener listener(lo);
  Counter c;
  ASSERT_TRUE(listener.Attach(sub, CountAndDrain, &c).ok());
  ASSERT_TRUE(WaitFor([&] { return pub.HasSubscribers(); }));
  PublishSeq(pub, 1);
  ASSERT_TRUE(WaitFor([&] { return c.taken.load() == 1; }));
  std::this_thread::sleep_for(100ms);  // let everything settle

  int64_t l0 = test::ThreadSwitches("shm_idle_ls09");
  int64_t c0 = test::ThreadSwitches("ibmw_shm_ctl");
  ASSERT_GE(l0, 0);
  ASSERT_GE(c0, 0);
  std::this_thread::sleep_for(1s);
  EXPECT_EQ(test::ThreadSwitches("shm_idle_ls09") - l0, 0);
  EXPECT_EQ(test::ThreadSwitches("ibmw_shm_ctl") - c0, 0);
}

// LS-10 Callbacks see the subscriber they were attached with and the context.
TEST(ShmListenerTest, CallbackArguments) {
  auto url = UniqueUrl("ls10");
  Publisher pub(url, EagerPool());
  Subscriber s1(url), s2(url);
  Listener listener;
  struct Ctx {
    Subscriber* expected;
    std::atomic<int> ok{0}, bad{0};
  } c1{&s1}, c2{&s2};
  auto cb = [](Subscriber* s, void* p) {
    auto* x = static_cast<Ctx*>(p);
    (s == x->expected ? x->ok : x->bad).fetch_add(1);
    while (auto t = s->Take()) s->Release(*t);
  };
  ASSERT_TRUE(listener.Attach(s1, cb, &c1).ok());
  ASSERT_TRUE(listener.Attach(s2, cb, &c2).ok());
  ASSERT_TRUE(WaitFor([&] { return pub.Stats().connections == 2; }));
  PublishSeq(pub, 1);
  ASSERT_TRUE(WaitFor([&] { return c1.ok.load() >= 1 && c2.ok.load() >= 1; }));
  EXPECT_EQ(c1.bad.load() + c2.bad.load(), 0);
}

}  // namespace
}  // namespace ibmw::selfdev::shm
