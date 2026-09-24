// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <map>
#include <string>
#include <vector>

#include "cpp/transport/serialized_message.h"
#include "engine/runtime_graph/topic_advertisement.h"
#include "engine/runtime_graph/type_support_registry.h"

namespace ibmw::cli::topic {

// Used only on the command's lifecycle thread, including TypeSupport loading
// and deserialization. Transport callbacks just enqueue owned payload copies.
class EchoFormatter {
 public:
  EchoFormatter(std::string format, std::string field, bool raw);
  void LoadPackage(const std::string& path);
  void LoadConfiguration(const std::filesystem::path& path);
  void Remember(const ibmw::runtime::core::runtime_graph::TopicAdvertisement& publisher);
  std::string Format(const ibmw::transport::SerializedMessage& message);

 private:
  std::string format_;
  bool raw_;
  std::vector<std::string> field_;
  std::map<std::string, std::string> packages_;
  ibmw::runtime::core::runtime_graph::TypeSupportRegistry types_;
};

}  // namespace ibmw::cli::topic
