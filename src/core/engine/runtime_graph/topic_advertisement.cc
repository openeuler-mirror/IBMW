// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/runtime_graph/topic_advertisement.h"

#include "base/exception.h"

namespace ibmw::runtime::core::runtime_graph {

nlohmann::json TopicAdvertisementToJson(
    const TopicAdvertisement& advertisement) {
  const auto& topic = advertisement.topic;
  nlohmann::json capture_capabilities = nlohmann::json::object();
  for (const auto& [driver, capability] :
       advertisement.capture_capabilities) {
    capture_capabilities[driver] = {
        {"abi_version", capability.abi_version},
        {"mode", capability.mode},
        {"priority", capability.priority},
        {"extension_name", capability.extension_name},
        {"extension_path", capability.extension_path},
        {"extension_options_yaml", capability.extension_options_yaml},
        {"driver_options_yaml", capability.driver_options_yaml},
        {"provider_instance_option", capability.provider_instance_option},
        {"provider_clear_option", capability.provider_clear_option},
        {"provider_strip_leading_slash_option",
         capability.provider_strip_leading_slash_option}};
  }
  nlohmann::json driver_qos = nlohmann::json::object();
  for (const auto& [driver, qos] : advertisement.driver_qos) {
    driver_qos[driver] = qos.policies;
  }
  return nlohmann::json{
      {"runtime_id", advertisement.runtime_id},
      {"control_socket", advertisement.control_socket},
      {"publisher_id", advertisement.publisher_id},
      {"topic", topic.topic},
      {"msg_type", topic.msg_type},
      {"serialization_type", topic.serialization_type},
      {"schema",
       {{"type_name", topic.schema.type_name},
        {"serialization_type", topic.schema.serialization_type},
        {"name", topic.schema.schema_name},
        {"encoding", topic.schema.schema_encoding},
        {"data", topic.schema.schema_data},
        {"hash", topic.schema.schema_hash}}},
      {"qos", topic.qos.policies},
      {"module_name", advertisement.module_name},
      {"driver_qos", std::move(driver_qos)},
      {"available_drivers", topic.available_drivers},
      {"type_support_package", advertisement.type_support_package},
      {"capture_capabilities", std::move(capture_capabilities)},
      {"published", advertisement.published},
      {"hidden", advertisement.hidden}};
}

TopicAdvertisement TopicAdvertisementFromJson(const nlohmann::json& value) {
  MW_ASSERT(value.is_object(), "Topic advertisement must be a JSON object.");
  TopicAdvertisement advertisement;
  advertisement.runtime_id = value.at("runtime_id").get<std::string>();
  advertisement.control_socket = value.at("control_socket").get<std::string>();
  advertisement.publisher_id = value.at("publisher_id").get<std::string>();
  advertisement.topic.topic = value.at("topic").get<std::string>();
  advertisement.topic.msg_type = value.at("msg_type").get<std::string>();
  advertisement.topic.serialization_type =
      value.at("serialization_type").get<std::string>();
  const auto& schema = value.at("schema");
  advertisement.topic.schema = {
      .type_name = schema.at("type_name").get<std::string>(),
      .serialization_type =
          schema.at("serialization_type").get<std::string>(),
      .schema_name = schema.at("name").get<std::string>(),
      .schema_encoding = schema.at("encoding").get<std::string>(),
      .schema_data = schema.at("data").get<std::string>(),
      .schema_hash = schema.at("hash").get<std::string>()};
  const bool has_schema = !advertisement.topic.schema.schema_data.empty();
  if (has_schema) {
    MW_ASSERT(
        advertisement.topic.schema.type_name == advertisement.topic.msg_type &&
            advertisement.topic.schema.serialization_type ==
                advertisement.topic.serialization_type &&
            !advertisement.topic.schema.schema_name.empty() &&
            !advertisement.topic.schema.schema_encoding.empty() &&
            advertisement.topic.schema.schema_hash ==
                ibmw::util::ComputeSchemaHash(
                    advertisement.topic.schema.schema_data),
        "Topic advertisement for '{}' contains an invalid Schema.",
        advertisement.topic.topic);
  }
  if (value.contains("qos")) {
    advertisement.topic.qos.policies =
        value.at("qos").get<std::map<std::string, std::string>>();
  }
  if (value.contains("available_drivers")) {
    advertisement.topic.available_drivers =
        value.at("available_drivers").get<std::vector<std::string>>();
  }
  advertisement.type_support_package =
      value.value("type_support_package", "");
  if (value.contains("capture_capabilities")) {
    for (const auto& [driver, item] :
         value.at("capture_capabilities").items()) {
      advertisement.capture_capabilities.emplace(
          driver,
          TransportCaptureCapability{
              .abi_version = item.value("abi_version", 0u),
              .mode = item.value("mode", "runtime_tap"),
              .priority = item.value("priority", 100u),
              .extension_name = item.value("extension_name", ""),
              .extension_path = item.value("extension_path", ""),
              .extension_options_yaml =
                  item.value("extension_options_yaml", ""),
              .driver_options_yaml = item.value("driver_options_yaml", ""),
              .provider_instance_option =
                  item.value("provider_instance_option", ""),
              .provider_clear_option =
                  item.value("provider_clear_option", ""),
              .provider_strip_leading_slash_option = item.value(
                  "provider_strip_leading_slash_option", "")});
    }
  }
  for (const auto& [driver, capability] :
       advertisement.capture_capabilities) {
    MW_ASSERT(
        capability.mode == "native_serialized" ||
            capability.mode == "cached_serialized" ||
            capability.mode == "runtime_tap",
        "Topic advertisement contains invalid capture mode '{}' for '{}'.",
        capability.mode, driver);
    if (capability.mode != "runtime_tap") {
      MW_ASSERT(
          !capability.extension_name.empty() &&
              !capability.extension_path.empty(),
          "Direct capture capability '{}' has no extension configuration.",
          driver);
    }
    if (capability.mode == "cached_serialized") {
      MW_ASSERT(
          !advertisement.type_support_package.empty(),
          "Cached capture capability '{}' has no TypeSupport package.",
          driver);
    }
  }
  advertisement.published = value.value("published", true);
  advertisement.hidden = value.value("hidden", false);
  advertisement.module_name = value.at("module_name").get<std::string>();
  const auto& driver_qos = value.at("driver_qos");
  MW_ASSERT(driver_qos.is_object(), "driver_qos must be an object.");
  for (const auto& [driver, policies] : driver_qos.items()) {
    advertisement.driver_qos[driver].policies =
        policies.get<std::map<std::string, std::string>>();
  }

  MW_ASSERT(
      !advertisement.runtime_id.empty() &&
          !advertisement.publisher_id.empty() &&
          !advertisement.topic.topic.empty() &&
          !advertisement.topic.msg_type.empty(),
      "Topic advertisement contains an empty identity field.");
  return advertisement;
}

}  // namespace ibmw::runtime::core::runtime_graph
