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
    const BagTopicGraph& graph,
    const PlanBuilder& build_plan) {
  for (auto& [topic, entry] : entries_) {
    if (entry.active &&
        (!selected_topics.contains(topic) ||
         !SourceStillAvailable(*entry.plan, graph))) {
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
        SourceStillAvailable(*existing->second.plan, graph)) {
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
    const BagTopicGraph& graph) {
  if (plan.Entries().empty()) {
    return false;
  }
  const auto& entries = plan.Entries();
  const auto advertisements =
      graph.Find(entries.front().advertisement.topic.topic);
  const auto endpoint_matches = [](const auto& selected, const auto& current) {
    return current.runtime_id == selected.runtime_id &&
           current.publisher_id == selected.publisher_id &&
           current.control_socket == selected.control_socket &&
           current.topic.msg_type == selected.topic.msg_type &&
           current.topic.serialization_type ==
               selected.topic.serialization_type &&
           current.topic.schema.schema_hash == selected.topic.schema.schema_hash;
  };
  if (!std::all_of(entries.begin(), entries.end(), [&](const auto& entry) {
        return std::any_of(
            advertisements.begin(), advertisements.end(),
            [&](const auto& current) {
              return endpoint_matches(entry.advertisement, current);
            });
      })) {
    return false;
  }

  const std::string& source_name = entries.front().source_name;
  if (source_name == "runtime-tap") {
    const size_t published_tap_count = static_cast<size_t>(std::count_if(
        advertisements.begin(), advertisements.end(), [](const auto& current) {
          return current.published && current.embedded_tap;
        }));
    return entries.size() == published_tap_count;
  }

  if (entries.front().capture_mode == "cached_serialized") {
    const size_t published_endpoint_count = static_cast<size_t>(std::count_if(
        advertisements.begin(), advertisements.end(), [](const auto& current) {
          return current.published;
        }));
    if (published_endpoint_count != 1) {
      return false;
    }
  }

  const bool advertises_capability = std::any_of(
      advertisements.begin(), advertisements.end(), [&](const auto& current) {
        return current.capture_capabilities.contains(source_name);
      });
  if (!advertises_capability) {
    return true;
  }

  return std::all_of(
      advertisements.begin(), advertisements.end(), [&](const auto& current) {
        if (!current.published) {
          return true;
        }
        const auto capability = current.capture_capabilities.find(source_name);
        return std::find(
                   current.topic.available_drivers.begin(),
                   current.topic.available_drivers.end(), source_name) !=
                   current.topic.available_drivers.end() &&
               capability != current.capture_capabilities.end() &&
               (capability->second.mode == "native_serialized" ||
                capability->second.mode == "cached_serialized");
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
