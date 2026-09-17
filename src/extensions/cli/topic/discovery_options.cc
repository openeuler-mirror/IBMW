// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "discovery_options.h"

#include <charconv>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace ibmw::cli::topic {
namespace {

std::optional<std::string> ReadOption(
    const ibmw::cli::Invocation& invocation, std::string_view name) {
  const auto found = invocation.options.find(std::string(name));
  if (found == invocation.options.end()) return std::nullopt;
  if (found->second.size() != 1) {
    throw std::invalid_argument(std::string(name) + " must be specified exactly once.");
  }
  return found->second.front();
}

uint32_t ParseDomain(std::string_view value, std::string_view name) {
  uint32_t domain = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), domain);
  if (value.empty() || error != std::errc{} || end != value.data() + value.size()) {
    throw std::invalid_argument(std::string(name) +
        " must be a decimal integer in [0, 4294967295], got '" +
        std::string(value) + "'.");
  }
  return domain;
}

void RequireSelected(bool selected, std::string_view option, std::string_view backend) {
  if (!selected) {
    throw std::invalid_argument(std::string(option) + " requires " +
                                std::string(backend) + " discovery to be selected.");
  }
}

}  // namespace

void AddDiscoveryOptions(ibmw::cli::Command& command) {
  using ibmw::cli::OptionValueMode;
  command.AddOption("--registry", {}, {},
                    "Query local runtime registrations across all drivers (default source)");
  command.AddOption("--dds", "-d", {}, "Query DDS endpoints in the selected domain");
  command.AddOption("--ros2", "-r", {}, "Query the ROS 2 graph in the selected domain");
  command.AddOption("--zenoh", "-z", {}, "Query Zenoh (discovery adapter not yet available)");
  command.AddOption("--all", "-a", {}, "Query all four sources, not all domains");
  command.AddOption("--domain", {}, "id", "Domain for selected DDS and ROS 2 sources",
                    OptionValueMode::kRequired);
  command.AddOption("--dds-domain", {}, "id", "DDS domain (overrides --domain; default: 0)",
                    OptionValueMode::kRequired);
  command.AddOption("--ros-domain", {}, "id",
                    "ROS 2 domain (overrides --domain; otherwise ROS_DOMAIN_ID or 0)",
                    OptionValueMode::kRequired);
  command.AddOption("--zenoh-domain", {}, "name", "Zenoh limit_domain (default: empty)",
                    OptionValueMode::kRequired);
  command.AddOption("--zenoh-config", {}, "path", "Zenoh native connection configuration",
                    OptionValueMode::kRequired);
  command.AddOption("--registry-path", {}, "path",
                    "Local registration directory (default: " +
                        ibmw::discovery::DiscoveryOptions{}.registry_path.string() + ")",
                    OptionValueMode::kRequired);
}

std::vector<ibmw::discovery::DiscoveryOptions> ParseDiscoveryOptions(
    const ibmw::cli::Invocation& invocation) {
  using ibmw::discovery::DiscoveryBackend;
  using ibmw::discovery::DiscoveryOptions;
  const bool all = invocation.HasOption("--all");
  const bool dds = all || invocation.HasOption("--dds");
  const bool ros2 = all || invocation.HasOption("--ros2");
  const bool zenoh = all || invocation.HasOption("--zenoh");
  const bool registry_selected = all || invocation.HasOption("--registry") || !(dds || ros2 || zenoh);

  const auto domain = ReadOption(invocation, "--domain");
  const auto dds_domain = ReadOption(invocation, "--dds-domain");
  const auto ros_domain = ReadOption(invocation, "--ros-domain");
  const auto zenoh_domain = ReadOption(invocation, "--zenoh-domain");
  const auto zenoh_config = ReadOption(invocation, "--zenoh-config");
  const auto registry = ReadOption(invocation, "--registry-path");
  if (domain) RequireSelected(dds || ros2, "--domain", "DDS or ROS 2");
  if (dds_domain) RequireSelected(dds, "--dds-domain", "DDS");
  if (ros_domain) RequireSelected(ros2, "--ros-domain", "ROS 2");
  if (zenoh_domain) RequireSelected(zenoh, "--zenoh-domain", "Zenoh");
  if (zenoh_config) RequireSelected(zenoh, "--zenoh-config", "Zenoh");
  if (registry) RequireSelected(registry_selected, "--registry-path", "registry");
  if ((registry && registry->empty()) || (zenoh_config && zenoh_config->empty())) {
    throw std::invalid_argument("Discovery configuration paths must not be empty.");
  }

  const uint32_t common_domain = domain ? ParseDomain(*domain, "--domain") : 0;
  std::vector<DiscoveryOptions> result;
  if (registry_selected) {
    DiscoveryOptions options;
    if (registry) options.registry_path = *registry;
    result.push_back(std::move(options));
  }
  if (dds) {
    result.push_back(DiscoveryOptions{
        .backend = DiscoveryBackend::kDds,
        .domain_id = dds_domain ? ParseDomain(*dds_domain, "--dds-domain") : common_domain});
  }
  if (ros2) {
    uint32_t selected_domain = common_domain;
    if (ros_domain) {
      selected_domain = ParseDomain(*ros_domain, "--ros-domain");
    } else if (!domain) {
      if (const char* environment = std::getenv("ROS_DOMAIN_ID");
          environment != nullptr && *environment != '\0') {
        selected_domain = ParseDomain(environment, "ROS_DOMAIN_ID");
      }
    }
    result.push_back(DiscoveryOptions{
        .backend = DiscoveryBackend::kRos2, .domain_id = selected_domain});
  }
  if (zenoh) {
    result.push_back(DiscoveryOptions{
        .backend = DiscoveryBackend::kZenoh,
        .zenoh_domain = zenoh_domain.value_or(""),
        .zenoh_config = zenoh_config.value_or("")});
  }
  return result;
}

}  // namespace ibmw::cli::topic
