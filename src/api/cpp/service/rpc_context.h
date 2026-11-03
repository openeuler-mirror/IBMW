// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#if IBMW_HAS_STD_MEMORY_RESOURCE
  #include <memory_resource>
#endif

#include "c/service/rpc_context_base.h"
#include "cpp/util/meta_views.h"
#include "cpp/util/string.h"
#include "base/exception.h"
#include "base/string_util.h"

namespace ibmw::rpc {

class ContextRef;

class Context {
 public:
  explicit Context(ibmw_rpc_context_type_t type = ibmw_rpc_context_type_t::IBMW_RPC_CLIENT_CONTEXT)
      : meta_data_map_(MakeMetaDataMap(
#if IBMW_HAS_STD_MEMORY_RESOURCE
            &default_pool_
#endif
            )),
        type_(type),
        base_(ibmw_service_context_base_t{
            .ops = GenOpsBase(),
            .ctx = this}) {}
  ~Context() = default;

  Context(const Context& other)
      : meta_data_map_(MakeMetaDataMap(
#if IBMW_HAS_STD_MEMORY_RESOURCE
            &default_pool_
#endif
            )),
        timeout_ns_(other.timeout_ns_),
        type_(other.type_),
        base_(ibmw_service_context_base_t{
            .ops = GenOpsBase(),
            .ctx = this}) {
    for (const auto& [key, value] : other.meta_data_map_) {
      if (auto [it, inserted] = meta_data_map_.emplace(key, value); inserted) {
        meta_views_.Add(it->first, it->second);
      }
    }
  }

  Context(Context&& other) noexcept
      : meta_data_map_(MakeMetaDataMap(
#if IBMW_HAS_STD_MEMORY_RESOURCE
            &default_pool_
#endif
            )),
        timeout_ns_(other.timeout_ns_),
        type_(other.type_),
        base_(ibmw_service_context_base_t{
            .ops = GenOpsBase(),
            .ctx = this}) {
    for (auto& [key, value] : other.meta_data_map_) {
      if (auto [it, inserted] = meta_data_map_.emplace(std::move(key), std::move(value)); inserted) {
        meta_views_.Add(it->first, it->second);
      }
    }
  }

  explicit Context(ContextRef ref);

  Context& operator=(const Context& other) = delete;
  Context& operator=(Context&& other) = delete;

  const ibmw_service_context_base_t* NativeHandle() const { return &base_; }

  bool CheckUsed() const { return used_.load(std::memory_order_acquire); }
  void SetUsed() { used_.store(true, std::memory_order_release); }
  // True if this call marked the context used (COMM-020).
  bool TrySetUsed() { return !used_.exchange(true, std::memory_order_acq_rel); }

  void Reset() {
    used_.store(false, std::memory_order_release);
    timeout_ns_ = 0;

    meta_data_map_.clear();
    meta_views_.Clear();
  }

  ibmw_rpc_context_type_t GetType() const {
    return type_;
  }

  // Timeout
  std::chrono::nanoseconds Timeout() const {
    return std::chrono::nanoseconds(timeout_ns_);
  }

  void SetTimeout(std::chrono::nanoseconds timeout) {
    timeout_ns_ = timeout.count();
  }

  // Some frame fields
  std::string_view GetMetaValue(std::string_view key) const {
    auto finditr = meta_data_map_.find(key);
    if (finditr != meta_data_map_.end()) return finditr->second;
    return "";
  }

  void SetMetaValue(std::string_view key, std::string_view val) {
    auto finditr = meta_data_map_.find(key);
    if (finditr == meta_data_map_.end()) {
      if (auto [it, inserted] = meta_data_map_.emplace(key, val); inserted) {
        meta_views_.Add(it->first, it->second);
      }
    } else {
      finditr->second = std::string(val);
      meta_views_.Update(finditr->first, finditr->second);
    }
  }

  std::vector<std::string_view> GetMetaKeys() const {
    std::vector<std::string_view> result;
    result.reserve(meta_data_map_.size());
    for (const auto& it : meta_data_map_) result.emplace_back(it.first);
    return result;
  }

  std::unordered_map<std::string_view, std::string_view> GetMetaKeyVals() const {
    std::unordered_map<std::string_view, std::string_view> result;
    for (const auto& it : meta_data_map_) result.emplace(it.first, it.second);
    return result;
  }

  std::string_view GetToAddr() const {
    return GetMetaValue(IBMW_RPC_CONTEXT_KEY_TO_ADDR);
  }
  void SetToAddr(std::string_view val) {
    SetMetaValue(IBMW_RPC_CONTEXT_KEY_TO_ADDR, val);
  }

  std::string_view GetSerializationType() const {
    return GetMetaValue(IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE);
  }
  void SetSerializationType(std::string_view val) {
    SetMetaValue(IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE, val);
  }

  std::string_view GetFunctionName() const {
    return GetMetaValue(IBMW_RPC_CONTEXT_KEY_FUNCTION_NAME);
  }
  void SetFunctionName(std::string_view val) {
    SetMetaValue(IBMW_RPC_CONTEXT_KEY_FUNCTION_NAME, val);
  }

  std::string ToString() const {
    std::stringstream ss;
    if (type_ == ibmw_rpc_context_type_t::IBMW_RPC_CLIENT_CONTEXT) {
      ss << "Client context, ";
    } else if (type_ == ibmw_rpc_context_type_t::IBMW_RPC_SERVER_CONTEXT) {
      ss << "Server context, ";
    } else {
      ss << "Unknown context, ";
    }

    ss << "timeout: " << timeout_ns_ / 1000000 << "ms, meta: {";

    bool first_meta = true;
    for (const auto& [key, value] : meta_data_map_) {
      const std::string key_copy(key);
      const std::string value_copy(value);
      if (!first_meta) ss << ",";
      first_meta = false;
      ss << "{\"" << key_copy << "\":\"" << value_copy << "\"}";
    }

    ss << "}";

    return ss.str();
  }

 private:
#if IBMW_HAS_STD_MEMORY_RESOURCE
  using MetaDataString = std::pmr::string;
  using MetaDataMap = std::pmr::unordered_map<
      std::pmr::string,
      std::pmr::string,
      ibmw::common::util::StringHash,
      std::equal_to<>>;
#else
  using MetaDataString = std::string;
  using MetaDataMap = std::unordered_map<
      std::string,
      std::string,
      ibmw::common::util::StringHash,
      std::equal_to<>>;
#endif

  static MetaDataMap MakeMetaDataMap(
#if IBMW_HAS_STD_MEMORY_RESOURCE
      std::pmr::memory_resource* resource)
  {
    return MetaDataMap(resource);
  }
#else
      )
  {
    return MetaDataMap();
  }
#endif

  static const ibmw_rpc_context_base_ops_t* GenOpsBase() {
    static constexpr ibmw_rpc_context_base_ops_t kOps{
        .check_used = [](void* ctx) -> bool {
          return static_cast<Context*>(ctx)->CheckUsed();
        },
        .set_used = [](void* ctx) {
          static_cast<Context*>(ctx)->SetUsed();
        },
        .get_type = [](void* ctx) -> ibmw_rpc_context_type_t {
          return static_cast<Context*>(ctx)->type_;
        },
        .get_timeout_ns = [](void* ctx) -> uint64_t {
          return static_cast<Context*>(ctx)->timeout_ns_;
        },
        .set_timeout_ns = [](void* ctx, uint64_t timeout) {
          static_cast<Context*>(ctx)->timeout_ns_ = timeout;  //
        },
        .get_meta_val = [](void* ctx, ibmw_string_view_t key) -> ibmw_string_view_t {
          return ibmw::util::ToIbmwStringView(
              static_cast<Context*>(ctx)->GetMetaValue(ibmw::util::ToStdStringView(key)));
        },
        .set_meta_val = [](void* ctx, ibmw_string_view_t key, ibmw_string_view_t val) {
          static_cast<Context*>(ctx)->SetMetaValue(
              ibmw::util::ToStdStringView(key), ibmw::util::ToStdStringView(val));  //
        },
        .get_meta_keys = [](void* ctx) -> ibmw_string_view_array_t {
          return static_cast<const Context*>(ctx)->meta_views_.Keys();
        },
        .get_meta_key_vals = [](void* ctx) -> ibmw_string_view_array_t {
          return static_cast<const Context*>(ctx)->meta_views_.KeyVals();
        },
        .try_set_used = [](void* ctx) -> bool {
          return static_cast<Context*>(ctx)->TrySetUsed();
        }};

    return &kOps;
  }

 private:
#if IBMW_HAS_STD_MEMORY_RESOURCE
  alignas(std::max_align_t) std::array<std::byte, 512> buffer_;
  std::pmr::monotonic_buffer_resource default_pool_{buffer_.data(), buffer_.size()};
#endif

  MetaDataMap meta_data_map_;

  // Arrays returned by the get_meta_keys/get_meta_key_vals ops (API-026).
  ibmw::util::MetaViews meta_views_;

  uint64_t timeout_ns_ = 0;

  std::atomic<bool> used_{false};

  const ibmw_rpc_context_type_t type_;
  const ibmw_service_context_base_t base_;
};

class ContextRef {
 public:
  ContextRef() = default;
  ContextRef(const Context& ctx)
      : base_ptr_(ctx.NativeHandle()) {}
  ContextRef(const Context* ctx_ptr)
      : base_ptr_(ctx_ptr ? ctx_ptr->NativeHandle() : nullptr) {}
  ContextRef(const std::shared_ptr<Context>& ctx_ptr)
      : base_ptr_(ctx_ptr ? ctx_ptr->NativeHandle() : nullptr) {}
  explicit ContextRef(const ibmw_service_context_base_t* base_ptr)
      : base_ptr_(base_ptr) {}
  ~ContextRef() = default;

  explicit operator bool() const { return (base_ptr_ != nullptr); }

  const ibmw_service_context_base_t* NativeHandle() const {
    return base_ptr_;
  }

  bool CheckUsed() const {
    MW_ASSERT(base_ptr_ && base_ptr_->ops, "Reference is null.");
    return base_ptr_->ops->check_used(base_ptr_->ctx);
  }

  void SetUsed() {
    MW_ASSERT(base_ptr_ && base_ptr_->ops, "Reference is null.");
    base_ptr_->ops->set_used(base_ptr_->ctx);
  }

  // True if this call marked the context used (COMM-020). A table without
  // try_set_used cannot claim the context atomically and is rejected.
  bool TrySetUsed() {
    MW_ASSERT(base_ptr_ && base_ptr_->ops, "Reference is null.");
    return base_ptr_->ops->try_set_used != nullptr && base_ptr_->ops->try_set_used(base_ptr_->ctx);
  }

  ibmw_rpc_context_type_t GetType() const {
    MW_ASSERT(base_ptr_ && base_ptr_->ops, "Reference is null.");
    return base_ptr_->ops->get_type(base_ptr_->ctx);
  }

  // Timeout bus
  std::chrono::nanoseconds Timeout() const {
    MW_ASSERT(base_ptr_ && base_ptr_->ops, "Reference is null.");
    return std::chrono::nanoseconds(base_ptr_->ops->get_timeout_ns(base_ptr_->ctx));
  }

  void SetTimeout(std::chrono::nanoseconds timeout) {
    MW_ASSERT(base_ptr_ && base_ptr_->ops, "Reference is null.");
    base_ptr_->ops->set_timeout_ns(base_ptr_->ctx, timeout.count());
  }

  // Some frame fields
  std::string_view GetMetaValue(std::string_view key) const {
    MW_ASSERT(base_ptr_ && base_ptr_->ops, "Reference is null.");
    return ibmw::util::ToStdStringView(
        base_ptr_->ops->get_meta_val(base_ptr_->ctx, ibmw::util::ToIbmwStringView(key)));
  }

  void SetMetaValue(std::string_view key, std::string_view val) {
    MW_ASSERT(base_ptr_ && base_ptr_->ops, "Reference is null.");
    base_ptr_->ops->set_meta_val(
        base_ptr_->ctx, ibmw::util::ToIbmwStringView(key), ibmw::util::ToIbmwStringView(val));
  }

  ibmw_string_view_array_t GetMetaKeysArray() const {
    MW_ASSERT(base_ptr_ && base_ptr_->ops, "Reference is null.");
    return base_ptr_->ops->get_meta_keys(base_ptr_->ctx);
  }

  ibmw_string_view_array_t GetMetaKeyValsArray() const {
    MW_ASSERT(base_ptr_ && base_ptr_->ops, "Reference is null.");
    return base_ptr_->ops->get_meta_key_vals(base_ptr_->ctx);
  }

  std::vector<std::string_view> GetMetaKeys() const {
    MW_ASSERT(base_ptr_ && base_ptr_->ops, "Reference is null.");
    auto [str_array, len] = base_ptr_->ops->get_meta_keys(base_ptr_->ctx);

    std::vector<std::string_view> result;
    result.reserve(len);
    for (size_t ii = 0; ii < len; ++ii)
      result.emplace_back(ibmw::util::ToStdStringView(str_array[ii]));

    return result;
  }

  std::unordered_map<std::string_view, std::string_view> GetMetaKeyVals() const {
    MW_ASSERT(base_ptr_ && base_ptr_->ops, "Reference is null.");
    auto [str_array, len] = base_ptr_->ops->get_meta_key_vals(base_ptr_->ctx);

    std::unordered_map<std::string_view, std::string_view> result;
    for (size_t ii = 0; ii + 1 < len; ii += 2) {
      auto key = ibmw::util::ToStdStringView(str_array[ii]);
      auto val = ibmw::util::ToStdStringView(str_array[ii + 1]);
      result.emplace(key, val);
    }

    return result;
  }

  std::string_view GetToAddr() const {
    return GetMetaValue(IBMW_RPC_CONTEXT_KEY_TO_ADDR);
  }
  void SetToAddr(std::string_view val) {
    SetMetaValue(IBMW_RPC_CONTEXT_KEY_TO_ADDR, val);
  }

  std::string_view GetSerializationType() const {
    return GetMetaValue(IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE);
  }
  void SetSerializationType(std::string_view val) {
    SetMetaValue(IBMW_RPC_CONTEXT_KEY_SERIALIZATION_TYPE, val);
  }

  std::string_view GetFunctionName() const {
    return GetMetaValue(IBMW_RPC_CONTEXT_KEY_FUNCTION_NAME);
  }
  void SetFunctionName(std::string_view val) {
    SetMetaValue(IBMW_RPC_CONTEXT_KEY_FUNCTION_NAME, val);
  }

  std::string ToString() const {
    MW_ASSERT(base_ptr_ && base_ptr_->ops, "Reference is null.");

    std::stringstream ss;

    auto type = base_ptr_->ops->get_type(base_ptr_->ctx);
    if (type == ibmw_rpc_context_type_t::IBMW_RPC_CLIENT_CONTEXT) {
      ss << "Client context, ";
    } else if (type == ibmw_rpc_context_type_t::IBMW_RPC_SERVER_CONTEXT) {
      ss << "Server context, ";
    } else {
      ss << "Unknown context, ";
    }

    auto timeout = base_ptr_->ops->get_timeout_ns(base_ptr_->ctx);
    ss << "timeout: " << timeout / 1000000 << "ms, meta: {";

    auto [str_array, len] = base_ptr_->ops->get_meta_key_vals(base_ptr_->ctx);
    for (size_t ii = 0; ii + 1 < len; ii += 2) {
      if (ii != 0) ss << ",";

      auto key = ibmw::util::ToStdStringView(str_array[ii]);
      auto val = ibmw::util::ToStdStringView(str_array[ii + 1]);

      const std::string key_copy(key);
      const std::string val_copy(val);

      ss << "{\"" << key_copy << "\":\"" << val_copy << "\"}";
    }

    ss << "}";

    return ss.str();
  }

 private:
  const ibmw_service_context_base_t* base_ptr_ = nullptr;
};

inline Context::Context(ContextRef ref)
    : meta_data_map_(MakeMetaDataMap(
#if IBMW_HAS_STD_MEMORY_RESOURCE
          &default_pool_
#endif
          )),
      type_(ref.GetType()),
      base_(ibmw_service_context_base_t{
          .ops = GenOpsBase(),
          .ctx = this}) {
  const auto* base_ptr = ref.NativeHandle();
  MW_ASSERT(base_ptr && base_ptr->ops, "Reference is null.");

  timeout_ns_ = base_ptr->ops->get_timeout_ns(base_ptr->ctx);

  auto [str_array, len] = base_ptr->ops->get_meta_key_vals(base_ptr->ctx);
  for (size_t ii = 0; ii + 1 < len; ii += 2) {
    auto key = ibmw::util::ToStdStringView(str_array[ii]);
    auto val = ibmw::util::ToStdStringView(str_array[ii + 1]);
    if (auto [it, inserted] = meta_data_map_.emplace(key, val); inserted) {
      meta_views_.Add(it->first, it->second);
    }
  }
}

}  // namespace ibmw::rpc
