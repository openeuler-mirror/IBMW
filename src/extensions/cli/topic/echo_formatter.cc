// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "echo_formatter.h"
#include "message_text.h"

#include <charconv>
#include <cmath>
#include <iomanip>
#include <regex>
#include <sstream>
#include <stdexcept>

#include "nlohmann/json.hpp"
#include "yaml-cpp/yaml.h"

namespace ibmw::cli::topic {
namespace {
using Json = nlohmann::json;

// DDS discovery exposes the generated wire name, whereas a ROS TypeSupport
// package registers its canonical ROS name. Only recognize the ROS convention;
// the caller must still find an explicitly loaded decoder for that exact type.
std::string RosTypeForDdsName(const std::string& type) {
  static const std::regex pattern(
      R"(^dds:([A-Za-z_][A-Za-z0-9_]*)::(msg|srv|action)::dds_::([A-Za-z_][A-Za-z0-9_]*)_$)");
  std::smatch match;
  if (!std::regex_match(type, match, pattern)) return {};
  return "ros2:" + match[1].str() + "/" + match[2].str() + "/" + match[3].str();
}

void EmitYaml(YAML::Emitter& output, const Json& value) {
  if (value.is_object()) {
    output << YAML::BeginMap;
    for (auto it = value.begin(); it != value.end(); ++it) {
      output << YAML::Key << it.key() << YAML::Value;
      EmitYaml(output, it.value());
    }
    output << YAML::EndMap;
  } else if (value.is_array()) {
    output << YAML::BeginSeq;
    for (const auto& item : value) EmitYaml(output, item);
    output << YAML::EndSeq;
  } else if (value.is_string()) {
    output << YAML::DoubleQuoted << value.get<std::string>();
  } else if (value.is_boolean()) {
    output << value.get<bool>();
  } else if (value.is_number_unsigned()) {
    output << value.get<uint64_t>();
  } else if (value.is_number_integer()) {
    output << value.get<int64_t>();
  } else if (value.is_number_float()) {
    output << YAML::DoublePrecision(17) << value.get<double>();
  } else {
    output << YAML::Null;
  }
}
}  // namespace

EchoFormatter::EchoFormatter(std::string format, std::string field, bool raw)
    : format_(std::move(format)), raw_(raw) {
  if (format_ != "yaml" && format_ != "json") {
    throw std::invalid_argument("--format must be yaml or json.");
  }
  if (!field.empty()) {
    size_t start = 0;
    do {
      const auto end = field.find('.', start);
      const auto part = field.substr(start, end == std::string::npos ? end : end - start);
      if (part.empty()) throw std::invalid_argument("--field requires a nonempty dot-separated path.");
      field_.push_back(part);
      if (end == std::string::npos) break;
      start = end + 1;
    } while (true);
  }
}

void EchoFormatter::LoadPackage(const std::string& path) {
  if (path.empty()) throw std::invalid_argument("--type-support requires a nonempty path.");
  types_.Load(path);
}

void EchoFormatter::LoadConfiguration(const std::filesystem::path& path) {
  if (path.empty()) return;
  const auto root = YAML::LoadFile(path.string());
  const auto config = root["ibmw"];
  if (!config.IsMap()) throw std::invalid_argument("--config-file must contain an ibmw map.");
  const auto runtime = config["runtime_graph"];
  if (!runtime) return;
  const auto packages = runtime["type_support_pkgs"];
  if (!packages) return;
  if (!packages.IsSequence()) throw std::invalid_argument("ibmw.runtime_graph.type_support_pkgs must be a sequence.");
  for (const auto& package : packages) {
    std::filesystem::path library = package.IsScalar() ? package.as<std::string>() : package["path"].as<std::string>();
    if (library.empty()) throw std::invalid_argument("TypeSupport package path must not be empty.");
    if (library.is_relative()) library = std::filesystem::absolute(path).parent_path() / library;
    LoadPackage(library.lexically_normal().string());
  }
}

void EchoFormatter::Remember(const ibmw::runtime::core::runtime_graph::TopicAdvertisement& publisher) {
  if (!publisher.type_support_package.empty()) {
    packages_.try_emplace(publisher.topic.msg_type, publisher.type_support_package);
  }
}

std::string EchoFormatter::Format(const ibmw::transport::SerializedMessage& message) {
  if (!message.payload) throw std::runtime_error("Echo received a null payload.");
  if (raw_) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    bool first = true;
    for (size_t index = 0; index < message.payload->Size(); ++index) {
      const auto& buffer = message.payload->Data()[index];
      const auto* data = static_cast<const unsigned char*>(buffer.data);
      for (size_t byte = 0; byte < buffer.len; ++byte) {
        if (!first) output << ' ';
        output << std::setw(2) << static_cast<unsigned>(data[byte]);
        first = false;
      }
    }
    return output.str();
  }

  auto format = message.topic.serialization_type;
  std::string text;
  if (format == "json" || format == "yaml") {
    text = message.payload->JoinToString();
  } else {
    auto type = types_.Find(message.topic.msg_type);
    if (!type) {
      const auto package = packages_.find(message.topic.msg_type);
      if (package != packages_.end()) {
        types_.Load(package->second);
        type = types_.Find(message.topic.msg_type, package->second);
      }
    }
    if (!type && message.source_driver == "dds" && format == "cdr") {
      const auto ros_type = RosTypeForDdsName(message.topic.msg_type);
      if (!ros_type.empty()) {
        type = types_.Find(ros_type);
        // This adapts only the decoder invocation. Keep the received descriptor
        // and wire identity intact, and never replace an exact native decoder.
        if (type) format = "ros2";
      }
    }
    if (!type) {
      throw std::runtime_error("No TypeSupport for '" + message.topic.msg_type +
          "'; provide --type-support <path> to decode it, or use --raw.");
    }
    const auto target = type.CheckSerializationTypeSupported("json") ? "json" : "yaml";
    if (!type.CheckSerializationTypeSupported(format) || !type.CheckSerializationTypeSupported(target)) {
      throw std::runtime_error("TypeSupport for '" + message.topic.msg_type + "' cannot convert '" + format +
          "' to JSON/YAML; provide a package with text serialization, or use --raw.");
    }
    auto object = type.CreateSharedPtr();
    if (!object || !type.Deserialize(format, *message.payload->NativeHandle(), object.get())) {
      throw std::runtime_error("Echo deserialization failed for '" + message.topic.msg_type + "' format '" + format + "'.");
    }
    text = type.SimpleSerialize(target, object.get());
    if (text.empty()) throw std::runtime_error("Echo text serialization failed for '" + message.topic.msg_type + "'.");
    format = target;
  }

  Json value;
  if (format == "json") {
    value = Json::parse(text);
  } else {
    const auto documents = YAML::LoadAll(text);
    if (documents.size() != 1) throw std::runtime_error("Echo expects one YAML document per message.");
    value = FromYaml(documents.front());
  }
  const Json* selected = &value;
  for (const auto& part : field_) {
    if (selected->is_object() && selected->contains(part)) {
      selected = &selected->at(part);
    } else if (selected->is_array()) {
      size_t index = 0;
      const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), index);
      if (error != std::errc{} || end != part.data() + part.size() || index >= selected->size()) {
        throw std::runtime_error("Echo field array index is invalid: " + part);
      }
      selected = &selected->at(index);
    } else {
      throw std::runtime_error("Echo field does not exist: " + part);
    }
  }
  if (format_ == "json") return selected->dump();
  YAML::Emitter output;
  EmitYaml(output, *selected);
  if (!output.good()) throw std::runtime_error("Echo YAML output failed: " + output.GetLastError());
  return output.c_str();
}

}  // namespace ibmw::cli::topic
