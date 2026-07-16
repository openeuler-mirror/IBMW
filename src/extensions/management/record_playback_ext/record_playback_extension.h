// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>

#include "engine/extension/extension_base.h"
#include "cpp/executor/executor.h"
#include "cpp/util/type_support.h"
#include "engine/type_support_pkg_loader.h"
#include "record_playback_ext/playback_action.h"
#include "record_playback_ext/record_action.h"
#include "record_playback_ext/service.h"

namespace ibmw::extensions::record_playback_extension {

class RecordPlaybackExtension : public ExtensionBase {
 public:
  struct Options {
    std::string service_name;

    struct TypeSupportPkg {
      std::string path;
    };

    std::vector<TypeSupportPkg> type_support_pkgs;
    std::string timer_executor;

    struct RecordActionOptions {
      std::string name;
      YAML::Node options;
    };
    std::vector<RecordActionOptions> record_actions;

    struct PlaybackActionOptions {
      std::string name;
      YAML::Node options;
    };
    std::vector<PlaybackActionOptions> playback_actions;
  };

 public:
  RecordPlaybackExtension();
  ~RecordPlaybackExtension() override = default;

  std::string_view Name() const noexcept override { return "record_playback_extension"; }

  bool OnInit(runtime::core::Engine* core_ptr) override;
  void OnShutdown() override;

 private:
  void InitTypeSupport(Options::TypeSupportPkg& options);

  void RegisterRpcService();

  void RegisterRecordTransport();
  void RegisterPlaybackTransport();

 private:
  Options options_;

  std::unique_ptr<RecordPlaybackServiceImpl> service_ptr_;

  std::vector<std::unique_ptr<runtime::core::util::TypeSupportPkgLoader>> type_support_pkg_loader_vec_;

  ibmw::executor::ExecutorHandle timer_executor_ref_;

  struct TypeSupportWrapper {
    const Options::TypeSupportPkg& options;
    ibmw::util::TypeSupportRef type_support_ref;
    runtime::core::util::TypeSupportPkgLoader* loader_ptr;
  };
  std::unordered_map<std::string_view, TypeSupportWrapper> type_support_map_;

  std::unordered_map<std::string_view, std::unique_ptr<RecordAction>> record_action_map_;
  std::unordered_map<std::string_view, std::unique_ptr<PlaybackAction>> playback_action_map_;
};

}  // namespace ibmw::extensions::record_playback_extension
