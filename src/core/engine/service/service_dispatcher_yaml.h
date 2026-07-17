// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.
//
// YAML convert<> specialization for ServiceDispatcher::Options.
// Shared between service_dispatcher.cc and
// service_dispatcher_yaml_and_filters.cc.

#pragma once

#include "engine/service/service_dispatcher.h"

namespace YAML {
template <>
struct convert<ibmw::runtime::core::service::ServiceDispatcher::Options> {
  using Options = ibmw::runtime::core::service::ServiceDispatcher::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["drivers"] = YAML::Node();
    for (const auto& driver : rhs.drivers_options) {
      Node driver_options_node;
      driver_options_node["type"] = driver.type;
      driver_options_node["options"] = driver.options;
      node["drivers"].push_back(driver_options_node);
    }

    node["clients"] = YAML::Node();
    for (const auto& client_options : rhs.clients_options) {
      Node client_options_node;
      client_options_node["func_name"] = client_options.func_name;
      client_options_node["enable_drivers"] = client_options.enable_drivers;
      client_options_node["enable_filters"] = client_options.enable_filters;
      node["clients"].push_back(client_options_node);
    }

    node["servers"] = YAML::Node();
    for (const auto& server_options : rhs.servers_options) {
      Node server_options_node;
      server_options_node["func_name"] = server_options.func_name;
      server_options_node["enable_drivers"] = server_options.enable_drivers;
      server_options_node["enable_filters"] = server_options.enable_filters;
      node["servers"].push_back(server_options_node);
    }

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (!node.IsMap()) return false;

    if (node["drivers"] && node["drivers"].IsSequence()) {
      for (const auto& driver_options_node : node["drivers"]) {
        auto driver_options = Options::DriverOptions{
            .type = driver_options_node["type"].as<std::string>()};

        if (driver_options_node["options"])
          driver_options.options = driver_options_node["options"];
        else
          driver_options.options = YAML::Node(YAML::NodeType::Null);

        rhs.drivers_options.emplace_back(std::move(driver_options));
      }
    }

    if (node["clients"] && node["clients"].IsSequence()) {
      for (const auto& client_options_node : node["clients"]) {
        auto client_options = Options::ClientOptions{
            .func_name = client_options_node["func_name"].as<std::string>(),
            .enable_drivers = client_options_node["enable_drivers"].as<std::vector<std::string>>()};

        if (client_options_node["enable_filters"])
          client_options.enable_filters = client_options_node["enable_filters"].as<std::vector<std::string>>();

        rhs.clients_options.emplace_back(std::move(client_options));
      }
    }

    if (node["servers"] && node["servers"].IsSequence()) {
      for (const auto& server_options_node : node["servers"]) {
        auto server_options = Options::ServerOptions{
            .func_name = server_options_node["func_name"].as<std::string>(),
            .enable_drivers = server_options_node["enable_drivers"].as<std::vector<std::string>>()};

        if (server_options_node["enable_filters"])
          server_options.enable_filters = server_options_node["enable_filters"].as<std::vector<std::string>>();

        rhs.servers_options.emplace_back(std::move(server_options));
      }
    }

    return true;
  }
};
}  // namespace YAML
