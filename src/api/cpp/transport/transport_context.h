// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

#if IBMW_HAS_STD_MEMORY_RESOURCE
  #include <memory_resource>
#endif

#include "c/transport/transport_context_base.h"
#include "cpp/util/meta_views.h"
#include "cpp/util/string.h"
#include "base/exception.h"
#include "base/string_util.h"

namespace ibmw::transport {

class Context {
 public:
  explicit Context(ibmw_transport_context_type_t type = ibmw_transport_context_type_t::IBMW_TRANSPORT_PUBLISHER_CONTEXT)
      : meta_data_map_(MakeMetaDataMap(
#if IBMW_HAS_STD_MEMORY_RESOURCE
            &default_pool_
#endif
            )),
        type_(type),
        base_(ibmw_transport_context_base_t{
            .ops = GenOpsBase(),
            .ctx = this}) {}
  ~Context() = default;

  // The map must be rebuilt on this object's own pool: copying/moving it would
  // keep (or default) the allocator, and the source's pool dies with the source.
  Context(const Context& other)
      : meta_data_map_(MakeMetaDataMap(
#if IBMW_HAS_STD_MEMORY_RESOURCE
            &default_pool_
#endif
            )),
        type_(other.type_),
        base_(ibmw_transport_context_base_t{
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
        type_(other.type_),
        base_(ibmw_transport_context_base_t{
            .ops = GenOpsBase(),
            .ctx = this}) {
    for (auto& [key, value] : other.meta_data_map_) {
      if (auto [it, inserted] = meta_data_map_.emplace(key, std::move(value)); inserted) {
        meta_views_.Add(it->first, it->second);
      }
    }
  }

  Context& operator=(const Context& other) = delete;
  Context& operator=(Context&& other) = delete;

  const ibmw_transport_context_base_t* NativeHandle() const { return &base_; }

  bool CheckUsed() const { return used_.load(std::memory_order_acquire); }
  void SetUsed() { used_.store(true, std::memory_order_release); }
  // True if this call marked the context used (COMM-020).
  bool TrySetUsed() { return !used_.exchange(true, std::memory_order_acq_rel); }

  void Reset() {
    used_.store(false, std::memory_order_release);

    meta_data_map_.clear();
    meta_views_.Clear();
  }

  ibmw_transport_context_type_t GetType() const { return type_; }

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

  std::string_view GetSerializationType() const {
    return GetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_SERIALIZATION_TYPE);
  }
  void SetSerializationType(std::string_view val) {
    SetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_SERIALIZATION_TYPE, val);
  }

  std::string ToString() const {
    std::stringstream ss;
    if (type_ == ibmw_transport_context_type_t::IBMW_TRANSPORT_PUBLISHER_CONTEXT) {
      ss << "Publisher context, ";
    } else if (type_ == ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT) {
      ss << "Subscriber context, ";
    } else {
      ss << "Unknown context, ";
    }

    ss << "meta: {";

    for (auto itr = meta_data_map_.begin(); itr != meta_data_map_.end(); ++itr) {
      if (itr != meta_data_map_.begin()) ss << ",";
      ss << "{\"" << itr->first << "\":\"" << itr->second << "\"}";
    }

    ss << "}";

    return ss.str();
  }

 private:
#if IBMW_HAS_STD_MEMORY_RESOURCE
  using MetaDataMap = std::pmr::unordered_map<
      std::pmr::string,
      std::pmr::string,
      ibmw::common::util::StringHash,
      std::equal_to<>>;
#else
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

  static const ibmw_transport_context_base_ops_t* GenOpsBase() {
    static constexpr ibmw_transport_context_base_ops_t kOps{
        .check_used = [](void* ctx) -> bool {
          return static_cast<Context*>(ctx)->CheckUsed();
        },
        .set_used = [](void* ctx) {
          static_cast<Context*>(ctx)->SetUsed();
        },
        .get_type = [](void* ctx) -> ibmw_transport_context_type_t {
          return static_cast<Context*>(ctx)->type_;
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

  std::atomic<bool> used_{false};

  const ibmw_transport_context_type_t type_;
  const ibmw_transport_context_base_t base_;
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
  explicit ContextRef(const ibmw_transport_context_base_t* base_ptr)
      : base_ptr_(base_ptr) {}
  ~ContextRef() = default;

  explicit operator bool() const { return (base_ptr_ != nullptr); }

  const ibmw_transport_context_base_t* NativeHandle() const {
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

  ibmw_transport_context_type_t GetType() const {
    MW_ASSERT(base_ptr_ && base_ptr_->ops, "Reference is null.");
    return base_ptr_->ops->get_type(base_ptr_->ctx);
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

  std::string_view GetSerializationType() const {
    return GetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_SERIALIZATION_TYPE);
  }
  void SetSerializationType(std::string_view val) {
    SetMetaValue(IBMW_TRANSPORT_CONTEXT_KEY_SERIALIZATION_TYPE, val);
  }

  std::string ToString() const {
    MW_ASSERT(base_ptr_ && base_ptr_->ops, "Reference is null.");

    std::stringstream ss;

    auto type = base_ptr_->ops->get_type(base_ptr_->ctx);
    if (type == ibmw_transport_context_type_t::IBMW_TRANSPORT_PUBLISHER_CONTEXT) {
      ss << "Publisher context, ";
    } else if (type == ibmw_transport_context_type_t::IBMW_TRANSPORT_SUBSCRIBER_CONTEXT) {
      ss << "Subscriber context, ";
    } else {
      ss << "Unknown context, ";
    }

    ss << "meta: {";

    auto [str_array, len] = base_ptr_->ops->get_meta_key_vals(base_ptr_->ctx);
    for (size_t ii = 0; ii + 1 < len; ii += 2) {
      if (ii != 0) ss << ",";

      auto key = ibmw::util::ToStdStringView(str_array[ii]);
      auto val = ibmw::util::ToStdStringView(str_array[ii + 1]);

      ss << "{\"" << key << "\":\"" << val << "\"}";
    }

    ss << "}";

    return ss.str();
  }

 private:
  const ibmw_transport_context_base_t* base_ptr_ = nullptr;
};

}  // namespace ibmw::transport
