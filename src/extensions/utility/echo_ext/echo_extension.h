// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "engine/extension/extension_base.h"
#include "cpp/util/type_support.h"
#include "engine/type_support_pkg_loader.h"

#include "engine/core_types.h"

namespace ibmw::extensions::echo_extension {

class EchoExtension : public ExtensionBase {
 public:
  struct Options {
    struct TopicMeta {
      std::string topic_name;
      std::string msg_type;
      std::string echo_type;
    };
    std::vector<TopicMeta> topic_meta_list;

    struct TypeSupportPkg {
      std::string path;
    };
    std::vector<TypeSupportPkg> type_support_pkgs;
  };

 public:
  EchoExtension();
  ~EchoExtension() override = default;

  std::string_view Name() const noexcept override { return "echo_extension"; }

  bool OnInit(runtime::core::Engine* core_ptr) override;
  void OnShutdown() override;

 private:
  void InitTypeSupport(Options::TypeSupportPkg& options);

  void RegisterEchoTransport();

  void FormatJson(std::string& json_str);

 private:
  Options options_;

  std::vector<std::unique_ptr<runtime::core::util::TypeSupportPkgLoader>>
      type_support_pkg_loader_vec_;

  struct TypeSupportWrapper {
    const Options::TypeSupportPkg& options;
    ibmw::util::TypeSupportRef type_support_ref;
    runtime::core::util::TypeSupportPkgLoader* loader_ptr;
  };

  std::unordered_map<std::string_view, TypeSupportWrapper> type_support_map_;

  struct TopicMeta {
    std::string topic_name;
    std::string msg_type;
    std::string echo_type;
  };
  std::unordered_map<runtime::core::util::TopicMetaKey, TopicMeta,
                     runtime::core::util::TopicMetaKey::Hash>
      topic_meta_map_;
};

}  // namespace ibmw::extensions::echo_extension
