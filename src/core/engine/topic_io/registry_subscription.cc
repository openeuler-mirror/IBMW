// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/topic_io/registry_subscription.h"

#include <algorithm>
#include <filesystem>
#include <tuple>
#include <unistd.h>

#include "base/exception.h"
#include "base/url_encode.h"
#include "engine/transport/serialized_transport_driver.h"

namespace ibmw::runtime::core::topic_io {
namespace {
using runtime_graph::TransportCaptureCapability;

YAML::Node Options(std::string_view yaml) {
  auto node = yaml.empty() ? YAML::Node(YAML::NodeType::Map) : YAML::Load(std::string(yaml));
  if (!node || node.IsNull()) node = YAML::Node(YAML::NodeType::Map);
  MW_ASSERT(node.IsMap(), "Registry subscription connection options must be a map.");
  return node;
}

nlohmann::json Canonical(const YAML::Node& node) {
  if (!node || node.IsNull()) return nullptr;
  if (node.IsScalar()) return node.as<std::string>();
  if (node.IsSequence()) {
    auto result = nlohmann::json::array();
    for (const auto& value : node) result.push_back(Canonical(value));
    return result;
  }
  auto result = nlohmann::json::object();
  for (const auto& value : node) result[value.first.as<std::string>()] = Canonical(value.second);
  return result;
}

YAML::Node ObserverOptions(const TransportCaptureCapability& capability) {
  auto options = Options(capability.extension_options_yaml);
  if (!capability.provider_instance_option.empty())
    options[capability.provider_instance_option] = "ibmw_observer_" + std::to_string(::getpid());
  if (!capability.provider_clear_option.empty()) options[capability.provider_clear_option] = "";
  if (!capability.provider_strip_leading_slash_option.empty() &&
      options[capability.provider_strip_leading_slash_option]) {
    auto value = options[capability.provider_strip_leading_slash_option].as<std::string>();
    value.erase(0, value.find_first_not_of('/'));
    if (value.empty()) options.remove(capability.provider_strip_leading_slash_option);
    else options[capability.provider_strip_leading_slash_option] = value;
  }
  return options;
}

std::string ConnectionKey(const TransportCaptureCapability& capability) {
  return nlohmann::json::array({capability.abi_version, capability.mode,
      capability.extension_name, capability.extension_path, Canonical(ObserverOptions(capability)),
      Canonical(Options(capability.driver_options_yaml))}).dump();
}

bool Direct(const runtime_graph::TopicAdvertisement& publisher, std::string_view driver) {
  const auto found = publisher.capture_capabilities.find(std::string(driver));
  if (found == publisher.capture_capabilities.end()) return false;
  const auto& capability = found->second;
  return capability.abi_version == transport::SerializedTransportCapabilityV2::kAbiVersion &&
      (capability.mode == "native_serialized" ||
       (capability.mode == "cached_serialized" && !publisher.type_support_package.empty())) &&
      !capability.extension_name.empty() && !capability.extension_path.empty();
}
}  // namespace

std::vector<RegistrySubscription> SelectRegistrySubscriptions(
    const std::vector<runtime_graph::TopicAdvertisement>& advertisements,
    const RegistrySubscriptionFilter& available) {
  std::vector<runtime_graph::TopicAdvertisement> publishers;
  for (const auto& advertisement : advertisements)
    if (advertisement.published) publishers.push_back(advertisement);
  if (publishers.empty()) return {};
  std::sort(publishers.begin(), publishers.end(), [](const auto& lhs, const auto& rhs) {
    return std::tie(lhs.runtime_id, lhs.publisher_id) < std::tie(rhs.runtime_id, rhs.publisher_id);
  });
  const auto& first = publishers.front();
  for (const auto& publisher : publishers) {
    MW_ASSERT(publisher.topic.topic == first.topic.topic &&
                  publisher.topic.msg_type == first.topic.msg_type &&
                  publisher.topic.serialization_type == first.topic.serialization_type &&
                  publisher.topic.schema.schema_hash == first.topic.schema.schema_hash,
              "Topic '{}' has conflicting publisher types or schemas.", first.topic.topic);
  }
  std::vector<std::pair<uint32_t, std::string>> drivers;
  for (const auto& driver : first.topic.available_drivers) {
    if (Direct(first, driver)) drivers.emplace_back(first.capture_capabilities.at(driver).priority, driver);
  }
  std::sort(drivers.begin(), drivers.end());
  for (const auto& [priority, driver] : drivers) {
    // Preserve the original planner's priority: Runtime Tap is the last
    // eligible route after direct drivers with priorities 0 through 4.
    if (priority >= 5) continue;
    const auto& capability = first.capture_capabilities.at(driver);
    if (capability.mode == "cached_serialized" && publishers.size() != 1) continue;
    const auto connection = ConnectionKey(capability);
    if (!std::all_of(publishers.begin(), publishers.end(), [&](const auto& publisher) {
          return std::find(publisher.topic.available_drivers.begin(), publisher.topic.available_drivers.end(), driver) !=
                     publisher.topic.available_drivers.end() && Direct(publisher, driver) &&
                 ConnectionKey(publisher.capture_capabilities.at(driver)) == connection;
        })) continue;
    RegistrySubscription selected{first, driver};
    if (driver == "dds" || driver == "ros2") {
      auto& qos = selected.publisher.topic.qos.policies;
      qos = {{"reliability", "reliable"}, {"durability", "transient_local"}, {"liveliness", "automatic"}};
      std::string ownership;
      bool compatible = true;
      for (const auto& publisher : publishers) {
        const auto per_driver = publisher.driver_qos.find(driver);
        const auto& offered = per_driver == publisher.driver_qos.end() ? publisher.topic.qos : per_driver->second;
        const auto value = [&](const char* name) {
          const auto found = offered.policies.find(name);
          return found == offered.policies.end() ? std::string{} : found->second;
        };
        if (value("reliability") != "reliable") qos["reliability"] = "best_effort";
        if (value("durability") != "transient_local" && value("durability") != "transient" &&
            value("durability") != "persistent") qos["durability"] = "volatile";
        if (driver == "dds") {
          const auto current = value("ownership").empty() ? "shared" : value("ownership");
          if (!ownership.empty() && ownership != current) compatible = false;
          ownership = current;
          qos["ownership"] = current;
        }
      }
      if (!compatible) continue;
    }
    if (!available || available(selected)) return {std::move(selected)};
  }
  std::vector<RegistrySubscription> taps;
  for (const auto& publisher : publishers) {
    const auto capability = publisher.capture_capabilities.find("runtime-tap");
    MW_ASSERT(capability != publisher.capture_capabilities.end() && capability->second.mode == "runtime_tap" &&
                  !publisher.control_socket.empty(),
              "No capture route covers every publisher for topic '{}'.", first.topic.topic);
    taps.push_back({publisher, "runtime-tap"});
  }
  return taps;
}

std::string RegistrySubscriptionKey(const RegistrySubscription& subscription) {
  const auto& publisher = subscription.publisher;
  const auto& topic = publisher.topic;
  auto key = nlohmann::json::array({subscription.driver, topic.topic, topic.msg_type,
      topic.serialization_type, topic.schema.schema_hash, topic.qos.policies});
  if (subscription.driver == "runtime-tap") {
    key.push_back({publisher.runtime_id, publisher.publisher_id, publisher.control_socket});
  } else {
    const auto& capability = publisher.capture_capabilities.at(subscription.driver);
    key.push_back(ConnectionKey(capability));
    if (capability.mode == "cached_serialized") {
      key.push_back({publisher.runtime_id, publisher.publisher_id, publisher.type_support_package});
    }
  }
  return key.dump();
}

std::string RegistrySubscriptionConnectionKey(const RegistrySubscription& subscription) {
  return ConnectionKey(subscription.publisher.capture_capabilities.at(subscription.driver));
}

YAML::Node RegistrySubscriptionConfig(const RegistrySubscription& subscription) {
  const auto& capability = subscription.publisher.capture_capabilities.at(subscription.driver);
  YAML::Node root, extension, driver, console;
  extension["name"] = capability.extension_name;
  extension["path"] = capability.extension_path;
  extension["options"] = ObserverOptions(capability);
  driver["type"] = subscription.driver;
  driver["options"] = Options(capability.driver_options_yaml);
  auto runtime = root["ibmw"];
  runtime["extensions"]["items"].push_back(extension);
  runtime["messaging"]["drivers"].push_back(driver);
  runtime["runtime_graph"]["enabled"] = false;
  // No module configs are written here; keep ConfigSubsystem from creating ./cfg/tmp in the user's cwd.
  runtime["config"]["temp_cfg_path"] = std::filesystem::temp_directory_path().string();
  runtime["logging"]["core_level"] = "WARN";
  console["type"] = "console";
  runtime["logging"]["backends"].push_back(console);
  return root;
}

transport::SerializedSubscriberOptionsV2 RegistrySubscriberOptions(const RegistrySubscription& subscription) {
  transport::SerializedSubscriberOptionsV2 options{
      .topic = subscription.publisher.topic, .requested_qos = subscription.publisher.topic.qos};
  if (subscription.driver == "ros2") {
    MW_ASSERT(options.topic.msg_type.starts_with("ros2:"), "Invalid ROS2 topic type.");
    options.wire_topic_name = options.topic.topic;
    options.wire_type_name = options.topic.msg_type.substr(5);
  } else if (subscription.driver == "zenoh") {
    const auto extension = ObserverOptions(subscription.publisher.capture_capabilities.at("zenoh"));
    const auto domain = extension["limit_domain"] ? extension["limit_domain"].as<std::string>() : std::string{};
    options.wire_topic_name = "transport/" + ibmw::common::util::UrlEncode(options.topic.topic) + "/" +
        ibmw::common::util::UrlEncode(options.topic.msg_type) + (domain.empty() ? "" : "/" + domain);
    options.wire_type_name = options.topic.msg_type;
  }
  // DDS uses its driver's logical-name mapping when wire addresses are empty.
  return options;
}

}  // namespace ibmw::runtime::core::topic_io
