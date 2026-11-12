// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "call_type_support.h"

#include <cmath>
#include <dlfcn.h>
#include <map>
#include <stdexcept>
#include "nlohmann/json.hpp"

namespace ibmw::cli::service {
namespace {
std::string Trim(std::string value) {
  const auto begin = value.find_first_not_of(" \t\r\n");
  return begin == std::string::npos ? "" : value.substr(begin, value.find_last_not_of(" \t\r\n") - begin + 1);
}
// Deliberately local to service: topic's parsing and public interfaces stay intact.
nlohmann::json RequestJson(const YAML::Node& node, size_t depth = 0) {
  if (depth > 128) throw std::invalid_argument("Invalid request: YAML is cyclic or nested too deeply.");
  if (node.IsNull()) return nullptr;
  if (node.IsSequence()) {
    auto result = nlohmann::json::array();
    for (const auto& item : node) result.push_back(RequestJson(item, depth + 1));
    return result;
  }
  if (node.IsMap()) {
    auto result = nlohmann::json::object();
    for (const auto& item : node) {
      if (!item.first.IsScalar()) throw std::invalid_argument("Invalid request: keys must be scalar strings.");
      const auto key = item.first.Scalar();
      if (result.contains(key)) throw std::invalid_argument("Duplicate request key: " + key);
      result[key] = RequestJson(item.second, depth + 1);
    }
    return result;
  }
  const auto text = node.Scalar();
  if (node.Tag() == "!" || node.Tag() == "tag:yaml.org,2002:str") return text;
  bool boolean; int64_t integer; uint64_t unsigned_integer; double number;
  if (YAML::convert<bool>::decode(node, boolean)) return boolean;
  if (YAML::convert<int64_t>::decode(node, integer)) return integer;
  if (YAML::convert<uint64_t>::decode(node, unsigned_integer)) return unsigned_integer;
  if (YAML::convert<double>::decode(node, number)) {
    if (!std::isfinite(number)) throw std::invalid_argument("Invalid request: numbers must be finite.");
    return number;
  }
  return text;
}
}  // namespace

std::string ParseCallRequest(std::string_view text) {
  const auto documents = YAML::LoadAll(std::string(text));
  if (documents.size() != 1 || !documents.front().IsMap()) throw std::invalid_argument("Invalid request: expected one JSON/YAML object.");
  return RequestJson(documents.front()).dump();
}

CallTypes LoadCallTypes(const CallOptions& options) {
  CallTypes out;
  out.function = options.service;
  if (options.backend == "ros2") {
    if (!options.packages.empty()) throw std::invalid_argument("--ros2 loads native service types; use pkg/srv/Type without --type-support.");
    const auto path = CallLibrary("libibmw_service_call_ros2.so");
    void* library = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) throw std::runtime_error(::dlerror());
    out.native_library = std::shared_ptr<void>(library, [](void* p) { ::dlclose(p); });
    using Create = NativeCallTypes* (*)(const char*);
    auto create = reinterpret_cast<Create>(::dlsym(library, "IbmwServiceCallRos2TypesV1"));
    if (!create) throw std::runtime_error("Missing ROS 2 service call type adapter entry.");
    // Catch while the adapter is loaded: never propagate a DSO-owned exception.
    std::string error;
    try { out.native_types.reset(create(options.type.c_str())); }
    catch (const std::exception& e) { error = e.what(); }
    catch (...) { error = "Unknown ROS 2 type adapter error."; }
    if (!error.empty() || !out.native_types) throw std::runtime_error("Cannot load ROS 2 service types: " + error);
    out.request = out.native_types->request; out.response = out.native_types->response;
    out.custom_service = out.native_types->service;
    if (!out.function.starts_with("ros2:")) out.function = "ros2:" + out.function;
    if (!out.function.starts_with("ros2:/")) throw std::invalid_argument("ROS 2 service must be an absolute name.");
    out.function = "ros2:/ibmw_cli_internal_call";
  } else {
    const auto arrow = options.type.find("->");
    if (arrow == std::string::npos || options.type.find("->", arrow + 2) != std::string::npos)
      throw std::invalid_argument("Type must be 'request_type -> response_type'; native pkg/srv/Type requires --ros2.");
    const auto request_name = Trim(options.type.substr(0, arrow));
    const auto response_name = Trim(options.type.substr(arrow + 2));
    if (request_name.empty() || response_name.empty()) throw std::invalid_argument("Both request and response types are required.");
    if (options.packages.empty()) throw std::invalid_argument("Provide --type-support <library> for request and response types.");
    std::map<std::string, ibmw::util::TypeSupportRef> types;
    for (const auto& path : options.packages) {
      auto loader = std::make_unique<ibmw::extension::TypeSupportPackageLoader>();
      loader->Load(path);
      for (const auto* entry : loader->TypeSupports()) {
        ibmw::util::TypeSupportRef type(entry);
        const std::string name(type.TypeName());
        // Only the requested pair can make this call ambiguous.
        if (name != request_name && name != response_name) continue;
        const auto [it, inserted] = types.emplace(name, type);
        if (!inserted && it->second.NativeHandle() != entry) throw std::invalid_argument("Ambiguous TypeSupport: " + name);
      }
      out.packages.push_back(std::move(loader));
    }
    const auto find = [&](const std::string& name) {
      auto it = types.find(name);
      if (it == types.end()) throw std::invalid_argument("No TypeSupport for '" + name + "'. Supply canonical IBMW type names, not unverified wire names.");
      return it->second;
    };
    out.request = find(request_name); out.response = find(response_name);
    if (options.backend == "dds") {
      for (const auto type : {out.request, out.response})
        if (!(type.TypeName().starts_with("dds:") || type.TypeName().starts_with("ros2:")) ||
            !(type.CheckSerializationTypeSupported("cdr") || type.CheckSerializationTypeSupported("ros2")) || !type.CustomTypeSupportPtr())
          throw std::invalid_argument("DDS call requires native DDS TypeSupport with CDR support for both types.");
    }
  }
  if (!out.request || !out.response || !out.request.CheckSerializationTypeSupported("json") || !out.response.CheckSerializationTypeSupported("json"))
    throw std::invalid_argument("Request and response TypeSupport must support JSON input/output.");
  out.serialization = options.serialization;
  if (out.serialization.empty()) {
    const auto preferred = out.request.DefaultSerializationType();
    if (out.response.CheckSerializationTypeSupported(preferred)) out.serialization = preferred;
    else out.serialization = "json";
  }
  if (!out.request.CheckSerializationTypeSupported(out.serialization) || !out.response.CheckSerializationTypeSupported(out.serialization))
    throw std::invalid_argument("Requested wire serialization is not supported by both types.");
  return out;
}
}  // namespace ibmw::cli::service
