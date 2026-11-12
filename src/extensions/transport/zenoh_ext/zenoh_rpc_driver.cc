// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "zenoh_ext/zenoh_rpc_driver.h"

#include <chrono>
#include <thread>

#include "cpp/service/rpc_handle.h"

namespace YAML {
template <>
struct convert<ibmw::extensions::zenoh_extension::ZenohRpcDriver::Options> {
  using Options = ibmw::extensions::zenoh_extension::ZenohRpcDriver::Options;

  static Node encode(const Options& rhs) {
    Node node;

    node["timeout_executor"] = rhs.timeout_executor;

    node["clients"] = YAML::Node();
    for (const auto& client_options : rhs.clients_options) {
      Node client_options_node;
      client_options_node["func_name"] = client_options.func_name;
      client_options_node["shm_enabled"] = client_options.shm_enabled;
      node["clients"].push_back(client_options_node);
    }

    node["servers"] = YAML::Node();
    for (const auto& server_options : rhs.servers_options) {
      Node server_options_node;
      server_options_node["func_name"] = server_options.func_name;
      server_options_node["shm_enabled"] = server_options.shm_enabled;
      node["servers"].push_back(server_options_node);
    }

    return node;
  }

  static bool decode(const Node& node, Options& rhs) {
    if (node["timeout_executor"])
      rhs.timeout_executor = node["timeout_executor"].as<std::string>();

    if (node["clients"] && node["clients"].IsSequence()) {
      for (const auto& client_options_node : node["clients"]) {
        auto client_options = Options::ClientOptions{
            .func_name = client_options_node["func_name"].as<std::string>(),
            .shm_enabled = client_options_node["shm_enabled"].as<bool>(),
        };

        rhs.clients_options.emplace_back(std::move(client_options));
      }
    }

    if (node["servers"] && node["servers"].IsSequence()) {
      for (const auto& server_options_node : node["servers"]) {
        auto server_options = Options::ServerOptions{
            .func_name = server_options_node["func_name"].as<std::string>(),
            .shm_enabled = server_options_node["shm_enabled"].as<bool>(),
        };

        rhs.servers_options.emplace_back(std::move(server_options));
      }
    }

    return true;
  }
};
}  // namespace YAML

namespace ibmw::extensions::zenoh_extension {

void ZenohRpcDriver::Initialize(YAML::Node options_node) {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::Init) == State::PreInit,
      "Zenoh Service driver can only be initialized once.");

  if (options_node && !options_node.IsNull())
    options_ = options_node.as<Options>();

  client_tool_ptr_ =
      std::make_unique<runtime::core::util::RpcClientTool<std::shared_ptr<runtime::core::service::ServiceMessageFrame>>>();

  if (!options_.timeout_executor.empty()) {
    MW_REQUIRE(
        get_executor_func_,
        "Get executor function is not set before initialize.");

    auto timeout_executor = get_executor_func_(options_.timeout_executor);

    MW_REQUIRE(
        timeout_executor,
        "Get timeout executor '{}' failed.", options_.timeout_executor);

    client_tool_ptr_->RegisterTimeoutExecutor(timeout_executor);
    client_tool_ptr_->RegisterTimeoutHandle(
        [](auto&& client_call_wrapper_ptr) {
          CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_TIMEOUT));
        });

    MW_TRACE("zenoh service driver enable the timeout function, use '{}' as timeout executor.",
                options_.timeout_executor);
  } else {
    MW_TRACE("zenoh service driver does not enable the timeout function.");
  }
}

void ZenohRpcDriver::Start() {
  MW_REQUIRE(
      std::atomic_exchange(&state_, State::Start) == State::Init,
      "Method can only be called when state is 'Init'.");
  zenoh_bus_ptr_->Freeze();
  zenoh_bus_ptr_->StartServiceDiscovery(limit_domain_);
}

void ZenohRpcDriver::Shutdown() {
  if (std::atomic_exchange(&state_, State::Shutdown) == State::Shutdown)
    return;

  zenoh_bus_ptr_->StopServiceDiscovery();
  // The handlers use this driver and its client tool: none may run once they are destroyed.
  // Servers go first: one waiting for an answer still gets it. So does a client
  // of an answer already being sent; later answers are dropped, their clients time out.
  auto unregister = [this](std::string_view prefix) {
    for (const auto& keyexpr : sub_keyexprs_)
      if (keyexpr.starts_with(prefix)) zenoh_bus_ptr_->UnregisterSubscriber(keyexpr);
  };
  unregister("req/");
  CloseReplyGate();
  unregister("rsp/");
  // client_tool_ptr_ stays until destruction: a Call() past its state check
  // still reaches it. Calls still waiting then get their result from the destructor.
}

void ZenohRpcDriver::CloseReplyGate() {
  reply_gate_->closed.store(true);
  // The caller of a shutdown from a bus callback keeps the driver until its answers ended.
  if (ZenohBus::InCallback()) [[unlikely]] {
    MW_WARN("Zenoh RPC shutdown from a bus callback: not waiting for running answers.");
    return;
  }
  auto warn_at = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (reply_gate_->running.load() != 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (std::chrono::steady_clock::now() >= warn_at) {
      MW_WARN("Zenoh RPC shutdown: still waiting for {} running answer(s).", reply_gate_->running.load());
      warn_at += std::chrono::seconds(5);
    }
  }
}

ZenohRpcDriver::~ZenohRpcDriver() {
  ZenohRpcDriver::Shutdown();
  if (!client_tool_ptr_) return;
  client_tool_ptr_->AbandonAll([](auto&& client_call_wrapper_ptr) {
    CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
  });
}

bool ZenohRpcDriver::RegisterServiceFunc(
    const runtime::core::service::ServiceFuncWrapper& service_func_wrapper) noexcept {
  try {
    if (state_.load() != State::Init) {
      MW_ERROR("Service func can only be registered when state is 'Init'.");
      return false;
    }

    namespace util = ibmw::common::util;
    const auto& info = service_func_wrapper.info;

    bool shm_enabled = false;

    auto find_option_itr = std::find_if(
        options_.servers_options.begin(), options_.servers_options.end(),
        [func_name = info.func_name](const Options::ServerOptions& server_option) {
          try {
            auto real_func_name = std::string(rpc::GetFuncNameWithoutPrefix(func_name));
            return std::regex_match(func_name.begin(), func_name.end(),
                                    std::regex(server_option.func_name, std::regex::ECMAScript)) ||
                   std::regex_match(real_func_name.begin(), real_func_name.end(),
                                    std::regex(server_option.func_name, std::regex::ECMAScript));
          } catch (const std::exception& e) {
            MW_WARN("Regex get exception, expr: {}, string: {}, exception info: {}",
                       server_option.func_name, func_name, e.what());
            return false;
          }
        });

    if (find_option_itr != options_.servers_options.end()) {
      shm_enabled = find_option_itr->shm_enabled;
    }

    std::string pattern = std::string("ibmw_rpc/") +
                          util::UrlEncode(rpc::GetFuncNameWithoutPrefix(info.func_name)) +
                          key_suffix_;

    auto handle = [this, &service_func_wrapper, pattern](const z_loaned_sample_t* message) {
      // The subscriber is live from registration on. Before Start other
      // publishers are still being registered and the answer could not be
      // looked up safely: the request is dropped, the client times out.
      if (state_.load() != State::Start) [[unlikely]] return;
      try {
        // create service invoke wrapper
        auto service_invoke_wrapper_ptr = std::make_shared<runtime::core::service::ServiceMessageFrame>(
            runtime::core::service::ServiceMessageFrame{.info = service_func_wrapper.info});
        const auto& info = service_invoke_wrapper_ptr->info;

        // create ctx
        auto ctx_ptr = std::make_shared<ibmw::rpc::Context>(ibmw_rpc_context_type_t::IBMW_RPC_SERVER_CONTEXT);
        service_invoke_wrapper_ptr->ctx_ref = ctx_ptr;

        const z_loaned_bytes_t* payload = z_sample_payload(message);
        size_t serialized_size = z_bytes_len(payload);
        z_bytes_reader_t reader = z_bytes_get_reader(payload);
        std::vector<char> serialized_data(serialized_size);

        // read data from payload
        serialized_data.resize(
            z_bytes_reader_read(&reader, reinterpret_cast<uint8_t*>(serialized_data.data()), serialized_size));

        // The length prefix comes from the peer: validate it against the bytes actually received.
        auto frame = util::GetLengthPrefixedPayload(serialized_data.data(), serialized_data.size());
        util::ConstBufferOperator buf_oper(frame.data(), frame.size());

        // deserialize type
        std::string serialization_type(buf_oper.GetString(util::BufferLenType::kUInt8));
        ctx_ptr->SetMetaValue(IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE, serialization_type);

        // The answer key is the registered one: a request naming another
        // function would get its answer onto that function's answer key.
        if (buf_oper.GetString(util::BufferLenType::kUInt8) != pattern) [[unlikely]] {
          MW_WARN_INTERVAL(5000, "Drop zenoh rpc request for '{}': it names another answer key.", info.func_name);
          return;
        }
        char req_id_buf[4];
        buf_oper.GetBuffer(req_id_buf, 4);

        size_t ctx_num = buf_oper.GetUint8();
        for (size_t ii = 0; ii < ctx_num; ++ii) {
          auto key = buf_oper.GetString(util::BufferLenType::kUInt16);
          auto val = buf_oper.GetString(util::BufferLenType::kUInt16);
          ctx_ptr->SetMetaValue(key, val);
        }
        ctx_ptr->SetFunctionName(info.func_name);
        ctx_ptr->SetMetaValue(IBMW_RPC_CONTEXT_KEY_DRIVER, Name());

        // deserialize msg
        auto remaining_buf = buf_oper.GetRemainingBuffer();
        ibmw_buffer_view_t buffer_view{
            .data = remaining_buf.data(),
            .len = remaining_buf.size()};

        ibmw_buffer_array_view_t buffer_array_view{
            .data = &buffer_view,
            .len = 1};

        std::shared_ptr<void> service_req_ptr = info.req_type_support_ref.CreateSharedPtr();
        service_invoke_wrapper_ptr->req_ptr = service_req_ptr.get();

        bool deserialize_ret = info.req_type_support_ref.Deserialize(
            serialization_type, buffer_array_view, service_req_ptr.get());

        MW_REQUIRE(deserialize_ret, "Zenoh req deserialize failed.");

        // create service rsp
        std::shared_ptr<void> service_rsp_ptr = info.rsp_type_support_ref.CreateSharedPtr();
        service_invoke_wrapper_ptr->rsp_ptr = service_rsp_ptr.get();

        // set callback
        service_invoke_wrapper_ptr->callback =
            [this,
             reply_gate = reply_gate_,
             service_invoke_wrapper_ptr,
             ctx_ptr,
             service_req_ptr,
             service_rsp_ptr,
             serialization_type{std::move(serialization_type)},
             pattern,
             req_id_buf](ibmw::rpc::Status status) {
              const ReplyPass pass(*reply_gate);
              if (!pass.Open()) [[unlikely]] return;

              if (!status.OK()) [[unlikely]] {
                ReturnRspWithStatusCode(
                    pattern, serialization_type, req_id_buf, status.Code());
                return;
              }

              std::string node_pub_topic = "rsp/" + pattern;

              // find node's publisher with pattern
              const auto& zenoh_pub_catalog_ptr = zenoh_bus_ptr_->GetPublisherRegisterMap();
              auto z_node_pub_iter = zenoh_pub_catalog_ptr.find(node_pub_topic);
              if (z_node_pub_iter == zenoh_pub_catalog_ptr.end()) [[unlikely]] {
                MW_ERROR("Can not find publisher with pattern: {}", pattern);
                return;
              }

              auto z_pub_ctx_ptr = z_node_pub_iter->second;

              // shm enabled
              if (z_pub_ctx_ptr->shm_enabled) {
                unsigned char* z_pub_loaned_shm_ptr = nullptr;
                std::shared_ptr<ibmw::util::BufferArrayView> buffer_array_cache_ptr = nullptr;

                bool is_shm_loan_size_enough = true;
                bool is_shm_pool_size_enough = true;

                uint64_t msg_size = 0;
                uint64_t header_len = 0;
                ShmLoan loan;
                const uint64_t pool_size = zenoh_bus_ptr_->ShmPoolSize();

                do {
                  // loan a new size shm (a loan that was too small is dropped)
                  const uint64_t loan_size = shm_loan_sizes_.Get(node_pub_topic, shm_init_loan_size_);
                  z_pub_loaned_shm_ptr = loan.Alloc(z_loan(zenoh_bus_ptr_->shm_provider_), loan_size, zenoh_bus_ptr_->alignment_);

                  // if shm pool is not enough, use net buffer instead
                  if (z_pub_loaned_shm_ptr == nullptr) {
                    is_shm_pool_size_enough = false;
                    break;
                  }

                  // write info pkg on loaned shm : the first FIXED_LEN bytes needs to write the length of pkg
                  util::BufferOperator buf_oper(reinterpret_cast<char*>(z_pub_loaned_shm_ptr) + 4, loan_size - 4);

                  // write serialization type on loaned shm
                  buf_oper.SetString(serialization_type, util::BufferLenType::kUInt8);

                  // write req id on loaned shm
                  buf_oper.SetBuffer(req_id_buf, sizeof(req_id_buf));

                  // write an zero on loaned shm
                  buf_oper.SetUint32(0);

                  header_len = 1 + serialization_type.size() + 4 + 4;

                  // write msg on loaned shm: should start at the (FIXED_LEN + header_len)-th byte
                  ibmw::util::ZenohBufferArrayAllocator z_allocator(buf_oper.GetRemainingSize(), z_pub_loaned_shm_ptr + header_len + 4);

                  if (buffer_array_cache_ptr == nullptr) {
                    try {
                      auto result = SerializeRspSupportedZenoh(*service_invoke_wrapper_ptr, serialization_type, ibmw::util::BufferArrayAllocatorRef(z_allocator.NativeHandle()));
                      msg_size = result.second;
                      buffer_array_cache_ptr = result.first;
                      if (buffer_array_cache_ptr == nullptr) {
                        // in this case means no cache is set, then do nomal serialization(if size is small will throw exception)
                        is_shm_loan_size_enough = true;
                      } else {
                        if (msg_size > buf_oper.GetRemainingSize()) {
                          // in this case means the msg has serialization cache but the size is too large, then expand suitable size
                          is_shm_loan_size_enough = false;
                          if (!shm_loan_sizes_.Need(node_pub_topic, uint64_t{4} + header_len + msg_size, pool_size)) {
                            is_shm_pool_size_enough = false;
                            break;
                          }
                        } else {
                          // in this case means the msg has serialization cache and the size is suitable, then use cachema
                          is_shm_loan_size_enough = true;
                        }
                      }

                    } catch (const std::exception& e) {
                      if (!z_allocator.IsShmEnough()) {
                        // the shm is not enough, need to expand a double size (within the pool)
                        is_shm_loan_size_enough = false;
                        if (!shm_loan_sizes_.Grow(node_pub_topic, loan_size, pool_size)) {
                          is_shm_pool_size_enough = false;
                          break;
                        }
                      } else {
                        MW_ERROR(
                            "Msg serialization failed, serialization_type {}, pattern: {}, exception: {}",
                            serialization_type, pattern, e.what());
                        return;
                      }
                    }
                  } else {
                    // the cache was loaned its size (Need above): it fits now
                    is_shm_loan_size_enough = true;
                  }

                } while (!is_shm_loan_size_enough);

                // too long for its length prefix: refused by the network path below
                if (is_shm_pool_size_enough && !FitsFrame(header_len + msg_size)) [[unlikely]]
                  is_shm_pool_size_enough = false;

                if (is_shm_pool_size_enough) {
                  // if has cache, the copy it to shm to replace the serialization
                  if (buffer_array_cache_ptr != nullptr) {
                    CopyBuffers(*buffer_array_cache_ptr, z_pub_loaned_shm_ptr + 4 + header_len);

                    buffer_array_cache_ptr = nullptr;
                  }

                  // write the frame length (header + msg) on loaned shm, like the non-shm path
                  util::SetBufFromUint32(reinterpret_cast<char*>(z_pub_loaned_shm_ptr), header_len + msg_size);

                  z_owned_bytes_t z_payload;
                  loan.MoveTo(&z_payload);
                  z_publisher_put(z_loan(z_pub_ctx_ptr->z_pub), z_move(z_payload), &zenoh_bus_ptr_->z_pub_options_);

                  // collect shared memory whose reference counting is zero (defragment only when short)
                  z_shm_provider_garbage_collect(z_loan(zenoh_bus_ptr_->shm_provider_));

                  MW_TRACE("Zenoh Invoke req  with '{}'", pattern);
                  return;
                }
                // the loan that did not fit (up to the whole pool) is collected now, not at the next shortage
                loan.Reset();
                z_shm_provider_garbage_collect(z_loan(zenoh_bus_ptr_->shm_provider_));
                MW_WARN_INTERVAL(5000, "Zenoh Extension shm pool is not enough, use net buffer instead.");
              }

              // shm disabled
              // serivice rsp serialize
              auto buffer_array_view_ptr = ibmw::runtime::core::service::TrySerializeRspWithCache(
                  *service_invoke_wrapper_ptr, serialization_type);
              if (!buffer_array_view_ptr) [[unlikely]] {
                ReturnRspWithStatusCode(
                    pattern, serialization_type, req_id_buf, IBMW_RPC_STATUS_SVR_SERIALIZATION_FAILED);

                return;
              }
              const auto* buffer_array_data = buffer_array_view_ptr->Data();
              const size_t buffer_array_len = buffer_array_view_ptr->Size();
              size_t rsp_size = buffer_array_view_ptr->BufferSize();

              size_t z_data_size = 1 + serialization_type.size() + 4 + 4 + rsp_size;
              if (!FitsFrame(z_data_size)) [[unlikely]] {
                MW_ERROR("Zenoh rpc answer of {} bytes is too long, pattern: {}", rsp_size, pattern);
                ReturnRspWithStatusCode(
                    pattern, serialization_type, req_id_buf, IBMW_RPC_STATUS_SVR_SERIALIZATION_FAILED);
                return;
              }
              size_t pkg_size = z_data_size + 4;

              // get buf to store data
              std::vector<char> msg_buf_vec(pkg_size);
              util::BufferOperator buf_oper(msg_buf_vec.data(), pkg_size);

              // full data_size
              buf_oper.SetUint32(z_data_size);

              // full serialize type
              buf_oper.SetString(serialization_type, util::BufferLenType::kUInt8);

              // full req_id
              buf_oper.SetBuffer(req_id_buf, sizeof(req_id_buf));

              // full an 0
              buf_oper.SetUint32(0);

              // full rsp_size
              for (size_t ii = 0; ii < buffer_array_len; ++ii) {
                buf_oper.SetBuffer(
                    static_cast<const char*>(buffer_array_data[ii].data),
                    buffer_array_data[ii].len);
              }

              // server send rsp
              z_owned_bytes_t z_payload;
              z_bytes_from_buf(&z_payload, reinterpret_cast<uint8_t*>(msg_buf_vec.data()), pkg_size, nullptr, nullptr);
              z_publisher_put(z_loan(z_pub_ctx_ptr->z_pub), z_move(z_payload), &zenoh_bus_ptr_->z_pub_options_);
            };
        // call service
        service_func_wrapper.service_func(service_invoke_wrapper_ptr);

      } catch (const std::exception& e) {
        MW_WARN("Handle zenoh rpc msg failed, exception info: {}", e.what());
      }
    };
    shm_loan_sizes_.Set("rsp/" + pattern, shm_init_loan_size_);
    const std::string request_key = "req/" + pattern;
    // A function registered again keeps its first server (RegisterRpcNode
    // declares nothing then); only a node this call declares is undone below.
    // This driver is the only RPC user of its bus.
    const bool declared_here = !sub_keyexprs_.contains(request_key);
    if (!zenoh_bus_ptr_->RegisterRpcNode(pattern, std::move(handle), "server", shm_enabled)) return false;
    try {
      sub_keyexprs_.insert(request_key);
      zenoh_bus_ptr_->RecordService(info.func_name, info.req_type_support_ref.TypeName(),
                                    info.rsp_type_support_ref.TypeName());
    } catch (...) {
      // A failed registration serves nothing: undeclare its node again. Its
      // answer publisher stays until the bus shuts down; nothing sends on it.
      if (declared_here) {
        zenoh_bus_ptr_->UnregisterSubscriber(request_key);
        sub_keyexprs_.erase(request_key);
      }
      throw;
    }
    return true;
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
}

bool ZenohRpcDriver::RegisterClientFunc(
    const runtime::core::service::ClientFuncWrapper& client_func_wrapper) noexcept {
  try {
    if (state_.load() != State::Init) {
      MW_ERROR("Client func can only be registered when state is 'Init'.");
      return false;
    }

    namespace util = ibmw::common::util;

    const auto& info = client_func_wrapper.info;

    bool shm_enabled = false;
    auto find_option_itr = std::find_if(
        options_.clients_options.begin(), options_.clients_options.end(),
        [func_name = info.func_name](const Options::ClientOptions& client_option) {
          try {
            auto real_func_name = std::string(rpc::GetFuncNameWithoutPrefix(func_name));
            return std::regex_match(func_name.begin(), func_name.end(),
                                    std::regex(client_option.func_name, std::regex::ECMAScript)) ||
                   std::regex_match(real_func_name.begin(), real_func_name.end(),
                                    std::regex(client_option.func_name, std::regex::ECMAScript));
          } catch (const std::exception& e) {
            MW_WARN("Regex get exception, expr: {}, string: {}, exception info: {}",
                       client_option.func_name, func_name, e.what());
            return false;
          }
        });

    if (find_option_itr != options_.clients_options.end()) {
      shm_enabled = find_option_itr->shm_enabled;
    }

    std::string pattern = std::string("ibmw_rpc/") +
                          util::UrlEncode(rpc::GetFuncNameWithoutPrefix(info.func_name)) +
                          key_suffix_;
    auto handle = [this, func = std::string(rpc::GetFuncNameWithoutPrefix(info.func_name))](
                      const z_loaned_sample_t* message) {
      std::shared_ptr<runtime::core::service::ServiceMessageFrame> client_call_wrapper_ptr;
      try {
        // read data
        const z_loaned_bytes_t* payload = z_sample_payload(message);
        size_t serialized_size = z_bytes_len(payload);
        z_bytes_reader_t reader = z_bytes_get_reader(payload);
        std::vector<char> serialized_data(serialized_size);

        serialized_data.resize(
            z_bytes_reader_read(&reader, reinterpret_cast<uint8_t*>(serialized_data.data()), serialized_size));

        // The length prefix comes from the peer: validate it against the bytes actually received.
        auto frame = util::GetLengthPrefixedPayload(serialized_data.data(), serialized_data.size());
        util::ConstBufferOperator buf_oper(frame.data(), frame.size());

        std::string serialization_type(buf_oper.GetString(util::BufferLenType::kUInt8));
        uint32_t req_id = buf_oper.GetUint32();
        uint32_t code = buf_oper.GetUint32();

        // Only an answer on the key of the called function completes the call.
        auto msg_recorder = client_tool_ptr_->GetRecordIf(req_id, [&func](const auto& record) {
          return rpc::GetFuncNameWithoutPrefix(record->info.func_name) == func;
        });
        if (!msg_recorder) [[unlikely]] {
          // can't find recorder, which means timeout
          MW_TRACE("Can not get req id {} from recorder.", req_id);
          return;
        }

        // find record
        client_call_wrapper_ptr = std::move(*msg_recorder);

        if (code) [[unlikely]] {
          CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(code));
          return;
        }

        const auto& info = client_call_wrapper_ptr->info;

        // client rsp deserialize
        auto remaining_buf = buf_oper.GetRemainingBuffer();
        ibmw_buffer_view_t buffer_view{
            .data = remaining_buf.data(),
            .len = remaining_buf.size()};

        ibmw_buffer_array_view_t buffer_array_view{
            .data = &buffer_view,
            .len = 1};

        bool deserialize_ret = info.rsp_type_support_ref.Deserialize(
            serialization_type, buffer_array_view, client_call_wrapper_ptr->rsp_ptr);

        if (!deserialize_ret) {
          // deserialize failed
          CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DESERIALIZATION_FAILED));
          return;
        }

        CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_OK));
        return;
      } catch (const std::exception& e) {
        MW_WARN("Handle zenoh rpc msg failed, exception info: {}", e.what());
      }

      if (client_call_wrapper_ptr)
        CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
    };

    shm_loan_sizes_.Set("req/" + pattern, shm_init_loan_size_);
    if (!zenoh_bus_ptr_->RegisterRpcNode(pattern, std::move(handle), "client", shm_enabled)) return false;
    sub_keyexprs_.insert("rsp/" + pattern);
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
    return false;
  }
  return true;
}

bool ZenohRpcDriver::IsClientReady(std::string_view, std::string_view function) const noexcept {
  try {
    if (state_.load() != State::Start) return false;
    const auto key = "req/ibmw_rpc/" +
                     common::util::UrlEncode(rpc::GetFuncNameWithoutPrefix(function)) + key_suffix_;
    const auto& publishers = zenoh_bus_ptr_->GetPublisherRegisterMap();
    const auto publisher = publishers.find(key);
    if (publisher == publishers.end()) return false;
    zc_matching_status_t status{};
    return zc_publisher_get_matching_status(z_loan(publisher->second->z_pub), &status) == 0 && status.matching;
  } catch (...) { return false; }
}

void ZenohRpcDriver::Call(
    const std::shared_ptr<runtime::core::service::ServiceMessageFrame>& client_call_wrapper_ptr) noexcept {
  try {
    if (state_.load() != State::Start) [[unlikely]] {
      MW_WARN("Method can only be called when state is 'Start'.");
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
      return;
    }

    namespace util = ibmw::common::util;
    const auto& info = client_call_wrapper_ptr->info;

    std::string pattern = std::string("ibmw_rpc/") +
                          util::UrlEncode(rpc::GetFuncNameWithoutPrefix(info.func_name)) +
                          key_suffix_;

    std::string node_pub_topic = "req/" + pattern;

    // find node's publisher with pattern
    const auto& zenoh_pub_catalog_ptr = zenoh_bus_ptr_->GetPublisherRegisterMap();
    auto z_node_pub_iter = zenoh_pub_catalog_ptr.find(node_pub_topic);
    if (z_node_pub_iter == zenoh_pub_catalog_ptr.end()) [[unlikely]] {
      MW_ERROR("Can not find publisher with pattern: {}", pattern);
      return;
    }

    auto z_pub_ctx_ptr = z_node_pub_iter->second;

    // get req id
    uint32_t cur_req_id = req_id_++;

    // get serialization type
    auto serialization_type =
        client_call_wrapper_ptr->ctx_ref.GetMetaValue(IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE);

    if (serialization_type.size() > 255) [[unlikely]] {
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_UNKNOWN));
      return;
    }

    // get meta data
    auto [meta_key_vals_array, meta_key_vals_array_len] = client_call_wrapper_ptr->ctx_ref.GetMetaKeyValsArray();
    if (meta_key_vals_array_len / 2 > 255) [[unlikely]] {
      MW_WARN("Too much context meta, require less than 255, but actually {}.", meta_key_vals_array_len / 2);
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
      return;
    }

    size_t context_meta_kv_size = 1;
    for (size_t ii = 0; ii < meta_key_vals_array_len; ++ii) {
      context_meta_kv_size += (2 + meta_key_vals_array[ii].len);
    }

    auto timeout = client_call_wrapper_ptr->ctx_ref.Timeout();
    auto record_ptr = client_call_wrapper_ptr;

    // record this req with timeout
    bool ret = client_tool_ptr_->Record(cur_req_id, timeout, std::move(record_ptr));
    if (!ret) [[unlikely]] {
      ret = client_tool_ptr_->RetryRecord(
          cur_req_id, timeout, client_call_wrapper_ptr, [this] { return req_id_++; });
    }

    if (!ret) [[unlikely]] {
      MW_ERROR("Failed to record msg.");
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_DRIVER_INTERNAL_ERROR));
      return;
    }

    // shm enabled
    if (z_pub_ctx_ptr->shm_enabled) {
      unsigned char* z_pub_loaned_shm_ptr = nullptr;
      std::shared_ptr<ibmw::util::BufferArrayView> buffer_array_cache_ptr = nullptr;

      bool is_shm_loan_size_enough = true;
      bool is_shm_pool_size_enough = true;

      uint64_t msg_size = 0;
      uint64_t header_len = 0;
      ShmLoan loan;
      const uint64_t pool_size = zenoh_bus_ptr_->ShmPoolSize();

      do {
        // loan a new size shm (a loan that was too small is dropped)
        const uint64_t loan_size = shm_loan_sizes_.Get(node_pub_topic, shm_init_loan_size_);
        z_pub_loaned_shm_ptr = loan.Alloc(z_loan(zenoh_bus_ptr_->shm_provider_), loan_size, zenoh_bus_ptr_->alignment_);

        // if shm pool is not enough, use net buffer instead
        if (z_pub_loaned_shm_ptr == nullptr) {
          is_shm_pool_size_enough = false;
          break;
        }

        // write info pkg on loaned shm : the first FIXED_LEN bytes needs to write the length of pkg
        util::BufferOperator buf_oper(reinterpret_cast<char*>(z_pub_loaned_shm_ptr) + 4, loan_size - 4);

        // write serialization type on loaned shm
        buf_oper.SetString(serialization_type, util::BufferLenType::kUInt8);

        // write pattern on loaned shm
        buf_oper.SetString(pattern, util::BufferLenType::kUInt8);

        // write req id on loaned shm
        buf_oper.SetUint32(cur_req_id);

        // write context meta on loaned shm
        buf_oper.SetUint8(static_cast<uint8_t>(meta_key_vals_array_len / 2));
        for (size_t ii = 0; ii < meta_key_vals_array_len; ++ii) {
          buf_oper.SetString(ibmw::util::ToStdStringView(meta_key_vals_array[ii]), util::BufferLenType::kUInt16);
        }

        header_len = 1 + serialization_type.size() +
                     1 + pattern.size() +
                     4 +
                     context_meta_kv_size;

        // write msg on loaned shm: should start at the (FIXED_LEN + header_len)-th byte
        ibmw::util::ZenohBufferArrayAllocator z_allocator(buf_oper.GetRemainingSize(), z_pub_loaned_shm_ptr + header_len + 4);

        if (buffer_array_cache_ptr == nullptr) {
          try {
            auto result = SerializeReqSupportedZenoh(*client_call_wrapper_ptr, serialization_type, ibmw::util::BufferArrayAllocatorRef(z_allocator.NativeHandle()));
            msg_size = result.second;
            buffer_array_cache_ptr = result.first;
            if (buffer_array_cache_ptr == nullptr) {
              // in this case means no cache is set, then do nomal serialization(if size is small will throw exception)
              is_shm_loan_size_enough = true;
            } else {
              if (msg_size > buf_oper.GetRemainingSize()) {
                // in this case means the msg has serialization cache but the size is too large, then expand suitable size
                is_shm_loan_size_enough = false;
                if (!shm_loan_sizes_.Need(node_pub_topic, uint64_t{4} + header_len + msg_size, pool_size)) {
                  is_shm_pool_size_enough = false;
                  break;
                }
              } else {
                // in this case means the msg has serialization cache and the size is suitable, then use cachema
                is_shm_loan_size_enough = true;
              }
            }

          } catch (const std::exception& e) {
            if (!z_allocator.IsShmEnough()) {
              // the shm is not enough, need to expand a double size (within the pool)
              is_shm_loan_size_enough = false;
              if (!shm_loan_sizes_.Grow(node_pub_topic, loan_size, pool_size)) {
                is_shm_pool_size_enough = false;
                break;
              }
            } else {
              MW_ERROR(
                  "Msg serialization failed, serialization_type {}, pkg_path: {}, module_name: {}, func_name: {}, exception: {}",
                  serialization_type, info.pkg_path, info.module_name, info.func_name, e.what());
              return;
            }
          }
        } else {
          // the cache was loaned its size (Need above): it fits now
          is_shm_loan_size_enough = true;
        }
      } while (!is_shm_loan_size_enough);

      // too long for its length prefix: refused by the network path below
      if (is_shm_pool_size_enough && !FitsFrame(header_len + msg_size)) [[unlikely]]
        is_shm_pool_size_enough = false;

      if (is_shm_pool_size_enough) {
        // if has cache, the copy it to shm to replace the serialization
        if (buffer_array_cache_ptr != nullptr) {
          CopyBuffers(*buffer_array_cache_ptr, z_pub_loaned_shm_ptr + 4 + header_len);

          buffer_array_cache_ptr = nullptr;
        }

        // write the frame length (header + msg) on loaned shm, like the non-shm path
        util::SetBufFromUint32(reinterpret_cast<char*>(z_pub_loaned_shm_ptr), header_len + msg_size);

        z_owned_bytes_t z_payload;
        loan.MoveTo(&z_payload);
        z_publisher_put(z_loan(z_pub_ctx_ptr->z_pub), z_move(z_payload), &zenoh_bus_ptr_->z_pub_options_);

        // collect shared memory whose reference counting is zero (defragment only when short)
        z_shm_provider_garbage_collect(z_loan(zenoh_bus_ptr_->shm_provider_));

        MW_TRACE("Zenoh Invoke req  with '{}'", pattern);
        return;
      }
      // the loan that did not fit (up to the whole pool) is collected now, not at the next shortage
      loan.Reset();
      z_shm_provider_garbage_collect(z_loan(zenoh_bus_ptr_->shm_provider_));
      MW_WARN_INTERVAL(5000, "Zenoh Extension shm pool is not enough, use net buffer instead.");
    }

    // shm disabled
    // client req serialize
    auto buffer_array_view_ptr = ibmw::runtime::core::service::TrySerializeReqWithCache(
        *client_call_wrapper_ptr, serialization_type);
    if (!buffer_array_view_ptr) [[unlikely]] {
      // serialize failed
      CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_SERIALIZATION_FAILED));
      return;
    }

    const auto* buffer_array_data = buffer_array_view_ptr->Data();
    const size_t buffer_array_len = buffer_array_view_ptr->Size();
    size_t req_size = buffer_array_view_ptr->BufferSize();

    // padding zenoh pkg (serialization_type + pattern +  req_id + context_meta + req)
    size_t z_data_size = 1 + serialization_type.size() +
                         1 + pattern.size() +
                         4 +
                         context_meta_kv_size +
                         req_size;
    if (!FitsFrame(z_data_size)) [[unlikely]] {
      MW_ERROR("Zenoh rpc request of {} bytes is too long, func_name: {}", req_size, info.func_name);
      // Taking the record releases the cached request and races no timeout.
      if (client_tool_ptr_->GetRecord(cur_req_id))
        CallResultCallback(*client_call_wrapper_ptr, ibmw::rpc::Status(IBMW_RPC_STATUS_CLI_SERIALIZATION_FAILED));
      return;
    }

    size_t pkg_size = z_data_size + 4;
    // create buffer for serialization
    std::vector<char> msg_buf_vec(pkg_size);
    util::BufferOperator buf_oper(msg_buf_vec.data(), pkg_size);

    // full data_size
    buf_oper.SetUint32(z_data_size);

    // full serialization_type
    buf_oper.SetString(serialization_type, util::BufferLenType::kUInt8);

    // full pattern
    buf_oper.SetString(pattern, util::BufferLenType::kUInt8);

    // full req id
    buf_oper.SetUint32(cur_req_id);

    // full context meta
    buf_oper.SetUint8(static_cast<uint8_t>(meta_key_vals_array_len / 2));
    for (size_t ii = 0; ii < meta_key_vals_array_len; ++ii) {
      buf_oper.SetString(ibmw::util::ToStdStringView(meta_key_vals_array[ii]), util::BufferLenType::kUInt16);
    }

    // full client req
    for (size_t ii = 0; ii < buffer_array_len; ++ii) {
      buf_oper.SetBuffer(
          static_cast<const char*>(buffer_array_data[ii].data),
          buffer_array_data[ii].len);
    }

    // send req
    z_owned_bytes_t z_payload;
    z_bytes_from_buf(&z_payload, reinterpret_cast<uint8_t*>(msg_buf_vec.data()), pkg_size, nullptr, nullptr);
    z_publisher_put(z_loan(z_pub_ctx_ptr->z_pub), z_move(z_payload), &zenoh_bus_ptr_->z_pub_options_);
  } catch (const std::exception& e) {
    MW_ERROR("{}", e.what());
  }
}

void ZenohRpcDriver::RegisterGetExecutorFunc(
    const std::function<ibmw::executor::ExecutorHandle(std::string_view)>& get_executor_func) {
  MW_REQUIRE(
      state_.load() == State::PreInit,
      "Method can only be called when state is 'PreInit'.");
  get_executor_func_ = get_executor_func;
}

void ZenohRpcDriver::ReturnRspWithStatusCode(
    const std::string& pattern,
    std::string_view serialization_type,
    const char* req_id_buf,
    uint32_t code) {
  namespace util = ibmw::common::util;

  const uint32_t data_size = 1 + serialization_type.size() + 4 + 4;
  const size_t pkg_size = 4 + data_size;
  std::vector<char> msg_buf_vec(pkg_size);
  util::BufferOperator buf_oper(msg_buf_vec.data(), msg_buf_vec.size());

  // Error responses use the same length-prefixed frame as successful replies.
  buf_oper.SetUint32(data_size);
  buf_oper.SetString(serialization_type, util::BufferLenType::kUInt8);
  buf_oper.SetBuffer(req_id_buf, 4);
  buf_oper.SetUint32(code);

  zenoh_bus_ptr_->Send("rsp/" + pattern, msg_buf_vec.data(), pkg_size);
}

}  // namespace ibmw::extensions::zenoh_extension
