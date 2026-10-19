// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
#pragma once
#include <cmath>
#include <stdexcept>
#include "nlohmann/json.hpp"
#include "yaml-cpp/yaml.h"
namespace ibmw::cli::topic {
using Json = nlohmann::json;
inline Json FromYaml(const YAML::Node& node, size_t depth = 0) {
  if (depth > 128) throw std::runtime_error("Message YAML nesting is too deep or contains a cyclic alias.");
  if (node.IsNull()) return nullptr;
  if (node.IsSequence()) {
    auto value = Json::array();
    for (const auto& item : node) value.push_back(FromYaml(item, depth + 1));
    return value;
  }
  if (node.IsMap()) {
    auto value = Json::object();
    for (const auto& item : node) {
      if (!item.first.IsScalar()) throw std::runtime_error("Message requires scalar YAML map keys.");
      const auto key = item.first.Scalar();
      if (value.contains(key)) throw std::runtime_error("Duplicate message YAML key: " + key);
      value[key] = FromYaml(item.second, depth + 1);
    }
    return value;
  }
  const auto text = node.Scalar();
  // yaml-cpp marks quoted strings with '!'; preserve strings such as "true"
  // and "123" when converting YAML to JSON or emitting YAML again.
  if (node.Tag() == "!" || node.Tag() == "tag:yaml.org,2002:str") return text;
  bool boolean;
  int64_t integer;
  uint64_t unsigned_integer;
  double number;
  if (YAML::convert<bool>::decode(node, boolean)) return boolean;
  if (YAML::convert<int64_t>::decode(node, integer)) return integer;
  if (YAML::convert<uint64_t>::decode(node, unsigned_integer)) return unsigned_integer;
  if (YAML::convert<double>::decode(node, number)) {
    if (!std::isfinite(number)) throw std::runtime_error("Cannot represent non-finite YAML numbers as JSON.");
    return number;
  }
  return text;
}

}  // namespace ibmw::cli::topic
