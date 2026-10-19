// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

#include "record_playback_ext/coordinator/bag_capture_planner.h"

namespace ibmw::extensions::record_playback_extension {

class DynamicCaptureController {
 public:
  using PlanBuilder =
      std::function<CapturePlan(std::string_view topic)>;
  using EventCallback = std::function<void(
      std::string_view topic,
      std::string_view description,
      bool started)>;

  DynamicCaptureController(
      ibmw::transport::SerializedMessageCallback callback,
      EventCallback event_callback = {});
  ~DynamicCaptureController();

  DynamicCaptureController(const DynamicCaptureController&) = delete;
  DynamicCaptureController& operator=(const DynamicCaptureController&) = delete;

  void Reconcile(
      const std::set<std::string>& selected_topics,
      const CaptureGraph& graph,
      const PlanBuilder& build_plan,
      const RegistrySubscriptionFilter& available = {});
  void StopAll();

  size_t ActiveCount() const noexcept;

 private:
  struct Entry {
    std::unique_ptr<CapturePlan> plan;
    bool active = false;
  };

  static bool SourceStillAvailable(
      const CapturePlan& plan,
      const CaptureGraph& graph,
      const RegistrySubscriptionFilter& available);
  void Start(std::string_view topic, Entry& entry);
  void Stop(std::string_view topic, Entry& entry);

  ibmw::transport::SerializedMessageCallback callback_;
  EventCallback event_callback_;
  std::unordered_map<std::string, Entry> entries_;
};

}  // namespace ibmw::extensions::record_playback_extension
