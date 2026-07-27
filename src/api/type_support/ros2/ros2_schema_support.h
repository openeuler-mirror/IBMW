// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>

#include <rosidl_runtime_cpp/traits.hpp>

#include "cpp/util/schema_support.h"

namespace ibmw::ros2 {

using Ros2DefinitionLoader = std::function<std::string(std::string_view)>;

namespace schema_detail {

inline std::string Trim(std::string_view value) {
  const auto begin = value.find_first_not_of(" \t\r");
  if (begin == std::string_view::npos) {
    return {};
  }
  const auto end = value.find_last_not_of(" \t\r");
  return std::string(value.substr(begin, end - begin + 1));
}

inline bool IsPrimitiveTypeName(std::string_view type) {
  static constexpr std::string_view kPrimitive[] = {
      "bool", "byte", "char", "float32", "float64", "int8", "uint8",
      "int16", "uint16", "int32", "uint32", "int64", "uint64", "string",
      "wstring"};
  for (const auto primitive : kPrimitive) {
    if (type == primitive ||
        (type.starts_with(primitive) && type.size() > primitive.size() &&
         type[primitive.size()] == '<')) {
      return true;
    }
  }
  return false;
}

inline std::string CanonicalRos2TypeName(
    std::string_view package,
    std::string_view field_type) {
  const auto slash = field_type.find('/');
  if (slash == std::string_view::npos) {
    return std::string(package) + "/msg/" + std::string(field_type);
  }
  const auto second = field_type.find('/', slash + 1);
  if (second == std::string_view::npos) {
    return std::string(field_type.substr(0, slash)) + "/msg/" +
           std::string(field_type.substr(slash + 1));
  }
  return std::string(field_type);
}

inline std::string CanonicalRos2SchemaFromDefinitions(
    std::string_view root_type,
    const Ros2DefinitionLoader& load_definition) {
  const std::string root = CanonicalRos2TypeName({}, root_type);
  const auto root_slash = root.find('/');
  if (root_slash == std::string::npos || root.front() == '/') {
    throw std::invalid_argument("ROS2 type must include a package");
  }

  std::stringstream schema;
  std::queue<std::string> pending;
  std::unordered_set<std::string> visited;
  pending.push(root);
  visited.insert(root);
  bool first = true;

  while (!pending.empty()) {
    const std::string current = std::move(pending.front());
    pending.pop();
    const std::string definition = load_definition(current);
    if (definition.empty()) {
      throw std::runtime_error(
          "ROS2 message definition is empty for " + current);
    }
    if (!first) {
      schema << "================================================================================\n"
             << "MSG: " << current << '\n';
    }
    first = false;

    const auto package_end = current.find('/');
    const std::string package = current.substr(0, package_end);
    std::istringstream lines(definition);
    std::string line;
    while (std::getline(lines, line)) {
      const auto comment = line.find('#');
      if (comment != std::string::npos) {
        line.erase(comment);
      }
      line = Trim(line);
      if (line.empty()) {
        continue;
      }

      std::istringstream tokens(line);
      std::string field_type;
      tokens >> field_type;
      std::string declaration;
      std::getline(tokens, declaration);
      declaration = Trim(declaration);
      if (field_type.empty() || declaration.empty()) {
        throw std::runtime_error(
            "Invalid ROS2 message declaration in " + current + ": " + line);
      }

      std::string array_suffix;
      const auto array = field_type.find('[');
      if (array != std::string::npos) {
        array_suffix = field_type.substr(array);
        field_type.erase(array);
      }
      if (IsPrimitiveTypeName(field_type)) {
        schema << field_type << array_suffix << ' ' << declaration << '\n';
        continue;
      }

      const std::string nested = CanonicalRos2TypeName(package, field_type);
      schema << nested << array_suffix << ' ' << declaration << '\n';
      if (visited.insert(nested).second) {
        pending.push(nested);
      }
    }
  }
  return schema.str();
}

inline std::string ServiceMessageDefinition(
    const std::filesystem::path& path,
    bool response) {
  std::ifstream input(path);
  if (!input) {
    return {};
  }
  std::stringstream selected;
  std::string line;
  bool in_response = false;
  while (std::getline(input, line)) {
    if (Trim(line) == "---") {
      in_response = true;
      continue;
    }
    if (in_response == response) {
      selected << line << '\n';
    }
  }
  return selected.str();
}

inline std::string LoadDefinitionFromPrefix(
    const std::filesystem::path& prefix,
    std::string_view type) {
  const auto first = type.find('/');
  const auto second = type.find('/', first + 1);
  if (first == std::string_view::npos || second == std::string_view::npos) {
    return {};
  }
  const std::string package(type.substr(0, first));
  const std::string kind(type.substr(first + 1, second - first - 1));
  const std::string name(type.substr(second + 1));
  const auto package_share = prefix / "share" / package;
  if (kind == "msg") {
    std::ifstream input(package_share / "msg" / (name + ".msg"));
    if (!input) {
      return {};
    }
    std::stringstream contents;
    contents << input.rdbuf();
    return contents.str();
  }
  if (kind == "srv") {
    static constexpr std::string_view kRequest = "_Request";
    static constexpr std::string_view kResponse = "_Response";
    const bool request = name.ends_with(kRequest);
    const bool response = name.ends_with(kResponse);
    if (!request && !response) {
      return {};
    }
    const auto suffix_size = request ? kRequest.size() : kResponse.size();
    return ServiceMessageDefinition(
        package_share / "srv" /
            (name.substr(0, name.size() - suffix_size) + ".srv"),
        response);
  }
  return {};
}

}  // namespace schema_detail

inline std::string LoadRos2MessageDefinitionFromEnvironment(
    std::string_view type) {
  static constexpr const char* kPrefixVariables[] = {
      "AMENT_PREFIX_PATH", "COLCON_PREFIX_PATH", "CMAKE_PREFIX_PATH"};
  std::unordered_set<std::string> visited;
  for (const char* variable : kPrefixVariables) {
    const char* value = std::getenv(variable);
    if (value == nullptr) {
      continue;
    }
    std::istringstream prefixes(value);
    std::string prefix;
    while (std::getline(prefixes, prefix, ':')) {
      if (prefix.empty() || !visited.insert(prefix).second) {
        continue;
      }
      auto definition = schema_detail::LoadDefinitionFromPrefix(prefix, type);
      if (!definition.empty()) {
        return definition;
      }
    }
  }
  throw std::runtime_error(
      "Cannot find ROS2 definition for " + std::string(type) +
      " in AMENT_PREFIX_PATH, COLCON_PREFIX_PATH, or CMAKE_PREFIX_PATH");
}

inline ibmw::util::SchemaDescriptor BuildRos2SchemaDescriptor(
    std::string_view root_type,
    const Ros2DefinitionLoader& load_definition) {
  const std::string canonical =
      schema_detail::CanonicalRos2TypeName({}, root_type);
  const std::string data =
      schema_detail::CanonicalRos2SchemaFromDefinitions(
          canonical, load_definition);
  return ibmw::util::SchemaDescriptor{
      .type_name = "ros2:" + canonical,
      .serialization_type = "ros2",
      .schema_name = canonical,
      .schema_encoding = "ros2msg",
      .schema_data = data,
      .schema_hash = ibmw::util::ComputeSchemaHash(data)};
}

template <typename Ros2MessageType>
const ibmw_schema_support_base_t* GetRos2MessageSchemaSupport(
    Ros2DefinitionLoader load_definition =
        LoadRos2MessageDefinitionFromEnvironment) {
  static const std::string kRos2TypeName =
      rosidl_generator_traits::name<Ros2MessageType>();
  static const ibmw::util::SchemaSupport kSchema(
      BuildRos2SchemaDescriptor(kRos2TypeName, load_definition));
  return kSchema.NativeHandle();
}

}  // namespace ibmw::ros2
