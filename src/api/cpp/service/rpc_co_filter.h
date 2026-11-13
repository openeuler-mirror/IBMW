// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "base/inline_function.h"
#include "cpp/co/task.h"
#include "cpp/service/rpc_context.h"
#include "cpp/service/rpc_status.h"

namespace ibmw::rpc {

using CoRpcHandle = ibmw::util::InlineFunction<co::Task<Status>(ContextRef, const void*, void*)>;
using CoRpcFilter = ibmw::util::InlineFunction<co::Task<Status>(ContextRef, const void*, void*, const CoRpcHandle&)>;
using CoRpcHandlePtr = std::shared_ptr<CoRpcHandle>;
using CoRpcInternalFilter = ibmw::util::InlineFunction<co::Task<Status>(ContextRef, const void*, void*, CoRpcHandlePtr)>;

// Every filter chain ever built stays alive at a stable address until the bus
// is destroyed, so a call suspended inside an older chain keeps running its
// filters after RegisterFilter() or Clear() (API-045). A call only loads the
// current head node.
class CoFilterBus {
 public:
  CoFilterBus() { head_.store(base_, std::memory_order_relaxed); }
  ~CoFilterBus() = default;

  CoFilterBus(const CoFilterBus&) = delete;
  CoFilterBus& operator=(const CoFilterBus&) = delete;

  void RegisterFilter(CoRpcFilter&& filter) {
    std::lock_guard lock(mutex_);
    const Node* next = head_.load(std::memory_order_relaxed);
    nodes_.push_back(std::make_unique<const Node>(Node{.filter = std::move(filter), .next = next}));
    head_.store(nodes_.back().get(), std::memory_order_release);
  }

  ibmw::co::Task<Status> InvokeRpc(CoRpcHandlePtr h, ContextRef ctx_ref, const void* req, void* rsp) const {
    return Run(head_.load(std::memory_order_acquire), ctx_ref, req, rsp, std::move(h));
  }

  // Removes all filters; calls still inside them finish normally.
  void Clear() {
    std::lock_guard lock(mutex_);
    head_.store(base_, std::memory_order_release);
  }

 private:
  // The base node has no filter and no next node.
  struct Node {
    CoRpcFilter filter;
    const Node* next = nullptr;
  };

  static ibmw::co::Task<Status> Run(const Node* node, ContextRef ctx_ref, const void* req, void* rsp,
                                    CoRpcHandlePtr h) {
    if (node->next == nullptr) {
      auto keep_alive = std::move(h);
      co_return co_await (*keep_alive)(ctx_ref, req, rsp);
    }
    auto next_handle = std::make_shared<CoRpcHandle>(
        [next = node->next, h{std::move(h)}](ContextRef ctx_ref, const void* req, void* rsp) -> ibmw::co::Task<Status> {
          return Run(next, ctx_ref, req, rsp, h);
        });
    co_return co_await node->filter(ctx_ref, req, rsp, *next_handle);
  }

  std::mutex mutex_;
  std::vector<std::unique_ptr<const Node>> nodes_;
  const Node base_node_{};
  const Node* const base_ = &base_node_;
  std::atomic<const Node*> head_{nullptr};
};

}  // namespace ibmw::rpc
