// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "engine/extension/extension_base.h"
#include "cpp/executor/executor.h"
#include "cpp/util/type_support.h"
#include "engine/engine.h"
#include "engine/core_types.h"
#include "engine/type_support_pkg_loader.h"

#include "proxy_ext/proxy_action.h"

namespace ibmw::extensions::proxy_extension {

class ProxyExtension : public ExtensionBase {
 public:
  struct Options {
    struct ProxyAction {
      std::string name;
      YAML::Node options;
    };
    std::vector<ProxyAction> proxy_actions;

    struct TypeSupportPkg {
      std::string path;
    };
    std::vector<TypeSupportPkg> type_support_pkgs;

    std::string executor;
  };

 public:
  ProxyExtension();
  ~ProxyExtension() override = default;

  std::string_view Name() const noexcept override { return "proxy_extension"; }

  bool OnInit(runtime::core::Engine* core_ptr) override;
  void OnShutdown() override;

 private:
  void InitTypeSupport(Options::TypeSupportPkg& options);

  void RegisterSubTransport();
  void RegisterPubTransport();

 private:
  ibmw::executor::ExecutorHandle executor_;

  Options options_;

  std::vector<std::unique_ptr<runtime::core::util::TypeSupportPkgLoader>>
      type_support_pkg_loader_vec_;

  struct TypeSupportWrapper {
    const Options::TypeSupportPkg& options;
    ibmw::util::TypeSupportRef type_support_ref;
    runtime::core::util::TypeSupportPkgLoader* loader_ptr;
  };

  std::unordered_map<std::string_view, TypeSupportWrapper> type_support_map_;

  struct TopicPubWrapper {
    const ibmw::runtime::core::transport::SendTypeWrapper* send_type_wrapper_ptr;
  };

  std::unordered_map<runtime::core::util::TopicMetaKey, TopicPubWrapper,
                     runtime::core::util::TopicMetaKey::Hash>
      topic_pub_wrapper_map_;

  std::unordered_map<std::string_view, std::unique_ptr<ProxyAction>> proxy_action_map_;
};

}  // namespace ibmw::extensions::proxy_extension
