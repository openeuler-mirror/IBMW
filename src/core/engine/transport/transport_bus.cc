// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/transport/transport_bus.h"
#include "engine/transport/transport_driver_tools.h"
#include "engine/transport/inprocess_transport.h"
#include "engine/transport/internal/debug_log_format.h"
#include "engine/core_types.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace YAML {
template <>
struct convert<ibmw::runtime::core::transport::TransportBus::Options> {
  using Options = ibmw::runtime::core::transport::TransportBus::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["drivers"] = YAML::Node();
    for (const auto& driver : rhs.drivers_options) {
      Node driver_options_node;
      driver_options_node["type"] = driver.type;
      driver_options_node["options"] = driver.options;
      node["drivers"].push_back(driver_options_node);
    }

    node["publishers"] = YAML::Node();
    for (const auto& pub_topic_options : rhs.pub_topics_options) {
      Node pub_topic_options_node;
      pub_topic_options_node["topic"] = pub_topic_options.topic_name;
      pub_topic_options_node["enable_drivers"] = pub_topic_options.enable_drivers;
      pub_topic_options_node["enable_filters"] = pub_topic_options.enable_filters;
      node["publishers"].push_back(pub_topic_options_node);
    }

    node["subscribers"] = YAML::Node();
    for (const auto& sub_topic_options : rhs.sub_topics_options) {
      Node sub_topic_options_node;
      sub_topic_options_node["topic"] = sub_topic_options.topic_name;
      sub_topic_options_node["enable_drivers"] = sub_topic_options.enable_drivers;
      sub_topic_options_node["enable_filters"] = sub_topic_options.enable_filters;
      node["subscribers"].push_back(sub_topic_options_node);
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

    if (node["publishers"] && node["publishers"].IsSequence()) {
      for (const auto& pub_topic_options_node : node["publishers"]) {
        auto pub_topic_options = Options::PubTopicOptions{
            .topic_name = pub_topic_options_node["topic"].as<std::string>(),
            .enable_drivers = pub_topic_options_node["enable_drivers"].as<std::vector<std::string>>()};

        if (pub_topic_options_node["enable_filters"])
          pub_topic_options.enable_filters = pub_topic_options_node["enable_filters"].as<std::vector<std::string>>();

        rhs.pub_topics_options.emplace_back(std::move(pub_topic_options));
      }
    }

    if (node["subscribers"] && node["subscribers"].IsSequence()) {
      for (const auto& sub_topic_options_node : node["subscribers"]) {
        auto sub_topic_options = Options::SubTopicOptions{
            .topic_name = sub_topic_options_node["topic"].as<std::string>(),
            .enable_drivers = sub_topic_options_node["enable_drivers"].as<std::vector<std::string>>()};

        if (sub_topic_options_node["enable_filters"])
          sub_topic_options.enable_filters = sub_topic_options_node["enable_filters"].as<std::vector<std::string>>();

        rhs.sub_topics_options.emplace_back(std::move(sub_topic_options));
      }
    }

    return true;
  }
};
}  // namespace YAML

namespace ibmw::runtime::core::transport {

namespace {

bool IsTransportShutdownProfileEnabled() {
  const char* value = std::getenv("IBMW_TRANSPORT_SHUTDOWN_PROFILE");
  return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

}  // namespace

void TransportBus::Initialize(YAML::Node options_node) {
  RegisterInProcessTransport();
  RegisterDebugLogFilter();

  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kInit) == State::kPreInit,
      "Transport bus can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  transport_catalog_ptr_ = std::make_unique<TransportCatalog>();
  transport_catalog_ptr_->SetLogger(logger_ptr_);

  transport_driver_dispatcher_.SetLogger(logger_ptr_);
  transport_driver_dispatcher_.SetTransportCatalog(transport_catalog_ptr_.get());
  transport_driver_dispatcher_.SetPublishFrameworkAsyncTransportFilterBus(&publish_filter_bus_);
  transport_driver_dispatcher_.SetSubscribeFrameworkAsyncTransportFilterBus(&subscribe_filter_bus_);

  std::vector<std::string> transport_driver_name_vec;

  // Initialize the specified driver according to the configuration
  for (auto& driver_options : options_.drivers_options) {
    auto finditr = std::find_if(
        transport_driver_vec_.begin(), transport_driver_vec_.end(),
        [&driver_options](const auto& ptr) {
          return ptr->Name() == driver_options.type;
        });

    MW_REQUIRE(finditr != transport_driver_vec_.end(),
                            "Invalid transport driver type '{}'",
                            driver_options.type);

    (*finditr)->SetTransportCatalog(transport_catalog_ptr_.get());
    (*finditr)->Initialize(driver_options.options);

    transport_driver_dispatcher_.RegisterTransportDriver(
        finditr->get(), transport_driver_capabilities_map_.at(finditr->get()));

    used_transport_driver_vec_.emplace_back(finditr->get());
    transport_driver_name_vec.emplace_back((*finditr)->Name());
  }

  // Set rules
  std::vector<std::pair<std::string, std::vector<std::string>>> pub_drivers_rules;
  std::vector<std::pair<std::string, std::vector<std::string>>> pub_filters_rules;
  for (const auto& item : options_.pub_topics_options) {
    for (const auto& driver_name : item.enable_drivers) {
      MW_REQUIRE(
          std::find(transport_driver_name_vec.begin(), transport_driver_name_vec.end(), driver_name) != transport_driver_name_vec.end(),
          "Invalid transport driver type '{}' for pub topic '{}'",
          driver_name, item.topic_name);
    }

    pub_drivers_rules.emplace_back(item.topic_name, item.enable_drivers);
    pub_filters_rules.emplace_back(item.topic_name, item.enable_filters);
  }
  transport_driver_dispatcher_.SetPubTopicsDriversRules(pub_drivers_rules);
  transport_driver_dispatcher_.SetPublishFiltersRules(pub_filters_rules);

  std::vector<std::pair<std::string, std::vector<std::string>>> sub_drivers_rules;
  std::vector<std::pair<std::string, std::vector<std::string>>> sub_filters_rules;
  for (const auto& item : options_.sub_topics_options) {
    for (const auto& driver_name : item.enable_drivers) {
      MW_REQUIRE(
          std::find(transport_driver_name_vec.begin(), transport_driver_name_vec.end(), driver_name) != transport_driver_name_vec.end(),
          "Invalid transport driver type '{}' for sub topic '{}'",
          driver_name, item.topic_name);
    }

    sub_drivers_rules.emplace_back(item.topic_name, item.enable_drivers);
    sub_filters_rules.emplace_back(item.topic_name, item.enable_filters);
  }
  transport_driver_dispatcher_.SetSubTopicsDriversRules(sub_drivers_rules);
  transport_driver_dispatcher_.SetSubscribeFiltersRules(sub_filters_rules);

  // Initialize driver bus
  transport_driver_dispatcher_.Initialize();

  options_node = options_;

  MW_INFO("[Transport] initialized");
}

void TransportBus::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::kStart) == State::kInit,
      "Method can only be called when state is 'Init'.");

  transport_driver_dispatcher_.Start();
  transport_access_start_flag_.store(true);

  MW_INFO("[Transport] started");
}

void TransportBus::Shutdown() {
  MW_REQUIRE(TryShutdown(),
             "Transport shutdown was vetoed or did not prepare within its "
             "timeout; resources remain intact for retry.");
}

bool TransportBus::CanShutdownFromCurrentThread() const noexcept {
  std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex_);
  return CanShutdownFromCurrentThreadUnlocked();
}

bool TransportBus::CanShutdownFromCurrentThreadUnlocked() const noexcept {
  if (state_.load() == State::kShutdown) return true;
  for (const auto* driver : used_transport_driver_vec_) {
    const auto capabilities = transport_driver_capabilities_map_.find(driver);
    if (capabilities != transport_driver_capabilities_map_.end() &&
        capabilities->second.shutdown_v1 != nullptr &&
        !capabilities->second.shutdown_v1->CanShutdownFromCurrentThread()) {
      return false;
    }
  }
  return true;
}

bool TransportBus::TryPrepareShutdownUnlocked() {
  if (state_.load() == State::kShutdown || shutdown_prepared_) return true;
  if (!CanShutdownFromCurrentThreadUnlocked()) {
    MW_ERROR("Transport shutdown rejected on the current thread; invoke "
             "shutdown from the runtime lifecycle owner.");
    return false;
  }

  const auto prepare_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  for (const auto* driver : used_transport_driver_vec_) {
    if (prepared_shutdown_drivers_.contains(driver)) continue;
    const auto capabilities = transport_driver_capabilities_map_.find(driver);
    if (capabilities == transport_driver_capabilities_map_.end() ||
        capabilities->second.shutdown_v1 == nullptr) {
      continue;
    }
    const auto remaining = std::max(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            prepare_deadline - std::chrono::steady_clock::now()),
        std::chrono::milliseconds(0));
    if (!capabilities->second.shutdown_v1->TryPrepareShutdown(remaining)) {
      MW_ERROR("Transport driver '{}' did not prepare for shutdown within "
               "the shared shutdown budget; shutdown remains retryable.",
               driver->Name());
      return false;
    }
    prepared_shutdown_drivers_.insert(driver);
  }
  shutdown_prepared_ = true;
  return true;
}

bool TransportBus::TryPrepareShutdown() {
  std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex_);
  return TryPrepareShutdownUnlocked();
}

bool TransportBus::TryShutdown() {
  std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex_);
  if (state_.load() == State::kShutdown) return true;
  if (!TryPrepareShutdownUnlocked()) return false;

  state_.store(State::kShutdown);

  MW_INFO("[Transport] stopped");

  const bool shutdown_profile = IsTransportShutdownProfileEnabled();
  if (shutdown_profile) {
    MW_INFO(
        "Transport shutdown profile [enter]: access_wraps={} drivers={}",
        transport_access_wrap_map_.size(),
        transport_driver_vec_.size());
  }

  transport_access_wrap_map_.clear();

  transport_driver_dispatcher_.Shutdown();

  if (shutdown_profile) {
    MW_INFO("Transport shutdown profile [dispatcher-done]");
  }

  transport_driver_vec_.clear();
  used_transport_driver_vec_.clear();
  transport_driver_capabilities_map_.clear();
  serialized_transport_capabilities_map_.clear();
  prepared_shutdown_drivers_.clear();

  transport_catalog_ptr_.reset();

  subscribe_filter_bus_.Clear();
  publish_filter_bus_.Clear();

  get_executor_func_ = nullptr;
  return true;
}

std::list<std::pair<std::string, std::string>> TransportBus::GenInitializationReport() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  std::vector<std::vector<std::string>> pub_topic_info_table =
      {{"topic", "msg type", "module", "drivers", "filters"}};

  const auto& pub_topic_driver_info = transport_driver_dispatcher_.GetPubTopicDriverInfo();
  const auto& pub_topic_index_map = transport_catalog_ptr_->GetPubTopicIndexMap();

  // Used to track entries with the same topic and msg_type
  std::unordered_map<util::TopicMetaKey, size_t, util::TopicMetaKey::Hash> pub_topic_msg_indices;

  for (const auto& pub_topic_index_itr : pub_topic_index_map) {
    auto topic_name = pub_topic_index_itr.first;
    auto pub_topic_driver_itr = pub_topic_driver_info.find(topic_name);
    MW_REQUIRE(pub_topic_driver_itr != pub_topic_driver_info.end(),
                            "Invalid transport catalog info.");

    auto filter_name_vec = publish_filter_bus_.GetFilterNameVec(topic_name);

    for (const auto& item : pub_topic_index_itr.second) {
      auto key = util::TopicMetaKey{
          .topic_name = std::string(topic_name),
          .msg_type = std::string(item->info.msg_type),
      };
      auto it = pub_topic_msg_indices.find(key);

      if (it != pub_topic_msg_indices.end()) {
        pub_topic_info_table[it->second][2] += "\n" + item->info.module_name;
      } else {
        std::vector<std::string> cur_topic_info(5);
        cur_topic_info[0] = topic_name;
        cur_topic_info[1] = item->info.msg_type;
        cur_topic_info[2] = item->info.module_name;
        cur_topic_info[3] = ibmw::common::util::JoinVec(pub_topic_driver_itr->second, ",");
        cur_topic_info[4] = ibmw::common::util::JoinVec(filter_name_vec, ",");
        pub_topic_info_table.emplace_back(std::move(cur_topic_info));
        pub_topic_msg_indices.emplace(key, pub_topic_info_table.size() - 1);
      }
    }
  }

  std::vector<std::vector<std::string>> sub_topic_info_table =
      {{"topic", "msg type", "module", "drivers", "filters"}};

  const auto& sub_topic_driver_info = transport_driver_dispatcher_.GetSubTopicDriverInfo();
  const auto& sub_topic_index_map = transport_catalog_ptr_->GetSubTopicIndexMap();

  std::unordered_map<util::TopicMetaKey, size_t, util::TopicMetaKey::Hash> sub_topic_msg_indices;

  for (const auto& sub_topic_index_itr : sub_topic_index_map) {
    auto topic_name = sub_topic_index_itr.first;
    auto sub_topic_driver_itr = sub_topic_driver_info.find(topic_name);
    MW_REQUIRE(sub_topic_driver_itr != sub_topic_driver_info.end(),
                            "Invalid transport catalog info.");

    auto filter_name_vec = subscribe_filter_bus_.GetFilterNameVec(topic_name);

    for (const auto& item : sub_topic_index_itr.second) {
      auto key = util::TopicMetaKey{
          .topic_name = std::string(topic_name),
          .msg_type = std::string(item->info.msg_type),
      };
      auto it = sub_topic_msg_indices.find(key);

      if (it != sub_topic_msg_indices.end()) {
        sub_topic_info_table[it->second][2] += "\n" + item->info.module_name;
      } else {
        std::vector<std::string> cur_topic_info(5);
        cur_topic_info[0] = topic_name;
        cur_topic_info[1] = item->info.msg_type;
        cur_topic_info[2] = item->info.module_name;
        cur_topic_info[3] = ibmw::common::util::JoinVec(sub_topic_driver_itr->second, ",");
        cur_topic_info[4] = ibmw::common::util::JoinVec(filter_name_vec, ",");
        sub_topic_info_table.emplace_back(std::move(cur_topic_info));
        sub_topic_msg_indices.emplace(key, sub_topic_info_table.size() - 1);
      }
    }
  }

  std::vector<std::string> transport_driver_name_vec;
  transport_driver_name_vec.reserve(transport_driver_vec_.size());
  for (const auto& item : transport_driver_vec_)
    transport_driver_name_vec.emplace_back(item->Name());

  auto transport_pub_filter_name_vec = publish_filter_bus_.GetAllFiltersName();
  auto transport_sub_filter_name_vec = subscribe_filter_bus_.GetAllFiltersName();

  std::string transport_summary;
  transport_summary += "  drivers: " +
      (transport_driver_name_vec.empty() ? "(none)" : ibmw::common::util::JoinVec(transport_driver_name_vec, ", ")) + "\n";
  transport_summary += "  pub filters: " +
      (transport_pub_filter_name_vec.empty() ? "(none)" : ibmw::common::util::JoinVec(transport_pub_filter_name_vec, ", ")) + "\n";
  transport_summary += "  sub filters: " +
      (transport_sub_filter_name_vec.empty() ? "(none)" : ibmw::common::util::JoinVec(transport_sub_filter_name_vec, ", ")) + "\n";

  // Append publisher table if non-empty
  if (pub_topic_info_table.size() > 1) {
    transport_summary += "  publishers:\n" + ibmw::common::util::FormatTree(pub_topic_info_table, true, "    ");
  }
  // Append subscriber table if non-empty
  if (sub_topic_info_table.size() > 1) {
    transport_summary += "  subscribers:\n" + ibmw::common::util::FormatTree(sub_topic_info_table, true, "    ");
  }

  std::list<std::pair<std::string, std::string>> report{
      {"Transport", transport_summary}};

  for (const auto& driver_ptr : used_transport_driver_vec_) {
    report.splice(report.end(), driver_ptr->GenInitializationReport());
  }

  return report;
}

void TransportBus::RegisterTransportDriver(
    std::unique_ptr<TransportDriver>&& transport_driver_ptr) {
  RegisterTransportDriver(std::move(transport_driver_ptr), {});
}

void TransportBus::RegisterTransportDriver(
    std::unique_ptr<TransportDriver>&& transport_driver_ptr,
    TransportDriverCapabilitiesV1 capabilities) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  transport_driver_capabilities_map_.emplace(transport_driver_ptr.get(),
                                              capabilities);
  transport_driver_vec_.emplace_back(std::move(transport_driver_ptr));
}

void TransportBus::RegisterTransportDriver(
    std::unique_ptr<TransportDriver>&& transport_driver_ptr,
    TransportDriverCapabilitiesV1 capabilities,
    const SerializedTransportCapabilityV2& serialized_capability) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  MW_REQUIRE(
      transport_driver_ptr != nullptr,
      "Cannot register a null transport driver.");
  MW_REQUIRE(
      serialized_capability.HasValidContract(),
      "Transport driver '{}' registered an invalid serialized capability "
      "contract (version {}, size {}).",
      transport_driver_ptr->Name(), serialized_capability.abi_version,
      serialized_capability.struct_size);

  auto* driver = transport_driver_ptr.get();
  const auto [capability, inserted] =
      serialized_transport_capabilities_map_.emplace(
          driver, serialized_capability);
  MW_REQUIRE(
      inserted,
      "Transport driver '{}' registered its serialized capability twice.",
      driver->Name());
  try {
    RegisterTransportDriver(std::move(transport_driver_ptr), capabilities);
  } catch (...) {
    serialized_transport_capabilities_map_.erase(capability);
    throw;
  }
}

void TransportBus::RegisterGetExecutorFunc(
    const std::function<executor::ExecutorHandle(std::string_view)>& get_executor_func) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");

  get_executor_func_ = get_executor_func;
}

const TransportAccess& TransportBus::GetBusAccess(
    const util::ModuleDetailInfo& module_info) {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");

  auto itr = transport_access_wrap_map_.find(module_info.module_name);
  if (itr != transport_access_wrap_map_.end())
    return itr->second->transport_handle_access;

  auto emplace_ret = transport_access_wrap_map_.emplace(
      module_info.module_name,
      std::make_unique<TransportAccessWrap>(
          module_info.pkg_path,
          module_info.module_name,
          *logger_ptr_,
          transport_driver_dispatcher_,
          passed_context_meta_keys_,
          transport_access_start_flag_));

  return emplace_ret.first->second->transport_handle_access;
}

const TransportDriverDispatcher& TransportBus::GetTransportDriverDispatcher() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");
  return transport_driver_dispatcher_;
}

const FrameworkAsyncTransportFilterBus& TransportBus::GetPublishFilterBus() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");
  return publish_filter_bus_;
}

const FrameworkAsyncTransportFilterBus& TransportBus::GetSubscribeFilterBus() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");
  return subscribe_filter_bus_;
}

void TransportBus::RegisterPublishFilter(std::string_view name, FrameworkAsyncTransportFilter&& filter) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  publish_filter_bus_.RegisterFilter(name, std::move(filter));
}

void TransportBus::RegisterSubscribeFilter(std::string_view name, FrameworkAsyncTransportFilter&& filter) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  subscribe_filter_bus_.RegisterFilter(name, std::move(filter));
}

void TransportBus::AddPassedContextMetaKeys(const std::unordered_set<std::string>& keys) {
  MW_REQUIRE(
      state_.load() == State::kPreInit,
      "Method can only be called when state is 'PreInit'.");
  passed_context_meta_keys_.insert(keys.begin(), keys.end());
}

const TransportCatalog* TransportBus::GetTransportCatalog() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");
  return transport_catalog_ptr_.get();
}

const std::vector<TransportDriver*>& TransportBus::GetUsedTransportDriver() const {
  MW_REQUIRE(
      state_.load() == State::kInit,
      "Method can only be called when state is 'Init'.");
  return used_transport_driver_vec_;
}

const SerializedTransportCapabilityV2*
TransportBus::GetSerializedTransportCapability(
    const TransportDriver& driver) const noexcept {
  const auto found = serialized_transport_capabilities_map_.find(&driver);
  return found == serialized_transport_capabilities_map_.end()
             ? nullptr
             : &found->second;
}

void TransportBus::RegisterInProcessTransport() {
  std::unique_ptr<TransportDriver> local_transport_driver_ptr =
      std::make_unique<InProcessTransport>();

  static_cast<InProcessTransport*>(local_transport_driver_ptr.get())
      ->RegisterGetExecutorFunc(get_executor_func_);

  static_cast<InProcessTransport*>(local_transport_driver_ptr.get())
      ->SetLogger(logger_ptr_);

  RegisterTransportDriver(std::move(local_transport_driver_ptr));
}

void TransportBus::RegisterDebugLogFilter() {
  RegisterPublishFilter(
      "debug_log",
      [this](MessageFrame& msg_wrapper, FrameworkAsyncTransportHandle&& h) {
        auto buf_ptr = TrySerializeMsgWithCache(
            msg_wrapper, internal::kDebugLogSerializationFormat);

        if (buf_ptr) {
          auto msg_str = buf_ptr->JoinToString();

          MW_INFO("Transport send a new msg. msg type: {}, topic name: {}, context: {}, msg: {}",
                     msg_wrapper.info.msg_type,
                     msg_wrapper.info.topic_name,
                     msg_wrapper.ctx_ref.ToString(),
                     msg_str);
        } else {
          MW_INFO("Transport send a new msg. msg type: {}, topic name: {}, context: {}",
                     msg_wrapper.info.msg_type,
                     msg_wrapper.info.topic_name,
                     msg_wrapper.ctx_ref.ToString());
        }

        h(msg_wrapper);
      });

  RegisterSubscribeFilter(
      "debug_log",
      [this](MessageFrame& msg_wrapper, FrameworkAsyncTransportHandle&& h) {
        auto buf_ptr = TrySerializeMsgWithCache(
            msg_wrapper, internal::kDebugLogSerializationFormat);

        if (buf_ptr) {
          auto msg_str = buf_ptr->JoinToString();

          MW_INFO("Transport message handlerr handle a new msg. msg type: {}, topic name: {}, context: {}, msg: {}",
                     msg_wrapper.info.msg_type,
                     msg_wrapper.info.topic_name,
                     msg_wrapper.ctx_ref.ToString(),
                     msg_str);
        } else {
          MW_INFO("Transport message handlerr handle a new msg. msg type: {}, topic name: {}, context: {}",
                     msg_wrapper.info.msg_type,
                     msg_wrapper.info.topic_name,
                     msg_wrapper.ctx_ref.ToString());
        }

        h(msg_wrapper);
      });
}

}  // namespace ibmw::runtime::core::transport
