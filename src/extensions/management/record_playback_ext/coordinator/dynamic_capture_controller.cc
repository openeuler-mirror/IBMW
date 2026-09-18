// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "record_playback_ext/coordinator/dynamic_capture_controller.h"

#include <algorithm>
#include <exception>

#include "base/exception.h"

namespace ibmw::extensions::record_playback_extension {

DynamicCaptureController::DynamicCaptureController(
    ibmw::transport::SerializedMessageCallback callback,
    EventCallback event_callback)
    : callback_(std::move(callback)),
      event_callback_(std::move(event_callback)) {
  MW_ASSERT(callback_, "Dynamic capture callback cannot be empty.");
}

DynamicCaptureController::~DynamicCaptureController() {
  try {
    StopAll();
  } catch (...) {
  }
}

void DynamicCaptureController::Reconcile(
    const std::set<std::string>& selected_topics,
    const CaptureGraph& graph,
    const PlanBuilder& build_plan,
    const RegistrySubscriptionFilter& available) {
  for (auto& [topic, entry] : entries_) {
    if (entry.active &&
        (!selected_topics.contains(topic) ||
         !SourceStillAvailable(*entry.plan, graph, available))) {
      Stop(topic, entry);
    }
  }

  for (const auto& topic : selected_topics) {
    if (graph.Find(topic).empty()) {
      continue;
    }
    auto existing = entries_.find(topic);
    if (existing != entries_.end() && existing->second.active) {
      continue;
    }
    if (existing != entries_.end() &&
        SourceStillAvailable(*existing->second.plan, graph, available)) {
      Start(topic, existing->second);
      continue;
    }

    auto plan = build_plan(topic);
    MW_ASSERT(
        !plan.Entries().empty(),
        "Capture planner returned an empty plan for topic '{}'.", topic);
    Entry replacement{.plan = std::make_unique<CapturePlan>(std::move(plan))};
    if (existing == entries_.end()) {
      existing = entries_.emplace(topic, std::move(replacement)).first;
    } else {
      existing->second = std::move(replacement);
    }
    Start(topic, existing->second);
  }
}

void DynamicCaptureController::StopAll() {
  std::exception_ptr first_error;
  for (auto& [topic, entry] : entries_) {
    if (!entry.active) {
      continue;
    }
    try {
      Stop(topic, entry);
    } catch (...) {
      if (!first_error) {
        first_error = std::current_exception();
      }
    }
  }
  if (first_error) {
    std::rethrow_exception(first_error);
  }
}

size_t DynamicCaptureController::ActiveCount() const noexcept {
  size_t count = 0;
  for (const auto& [topic, entry] : entries_) {
    static_cast<void>(topic);
    count += entry.active ? 1 : 0;
  }
  return count;
}

bool DynamicCaptureController::SourceStillAvailable(
    const CapturePlan& plan,
    const CaptureGraph& graph,
    const RegistrySubscriptionFilter& available) {
  if (plan.Entries().empty()) return false;
  const auto current = graph.Subscriptions(plan.Entries().front().endpoint.topic.topic, available);
  const auto& entries = plan.Entries();
  return entries.size() == current.size() &&
         std::all_of(entries.begin(), entries.end(), [&](const auto& entry) {
           return std::any_of(current.begin(), current.end(), [&](const auto& endpoint) {
             return entry.endpoint.SameSubscription(endpoint);
           });
         });
}

void DynamicCaptureController::Start(
    std::string_view topic,
    Entry& entry) {
  MW_ASSERT(entry.plan != nullptr && !entry.active, "Invalid capture state.");
  entry.plan->StartAll(callback_);
  entry.active = true;
  if (event_callback_) {
    for (const auto& source : entry.plan->Entries()) {
      event_callback_(topic, source.description, true);
    }
  }
}

void DynamicCaptureController::Stop(
    std::string_view topic,
  Entry& entry) {
  MW_ASSERT(entry.plan != nullptr && entry.active, "Invalid capture state.");
  entry.active = false;
  entry.plan->StopAll();
  if (event_callback_) {
    for (const auto& source : entry.plan->Entries()) {
      event_callback_(topic, source.description, false);
    }
  }
}

}  // namespace ibmw::extensions::record_playback_extension
