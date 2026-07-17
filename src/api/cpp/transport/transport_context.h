// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "base/ibmw_config.h"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>

#if IBMW_HAS_STD_MEMORY_RESOURCE
  #include <memory_resource>
#endif

#include "c/transport/transport_context_base.h"
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

  Context(const Context& other)
      : meta_data_map_(other.meta_data_map_),
        type_(other.type_),
        base_(ibmw_transport_context_base_t{
            .ops = GenOpsBase(),
            .ctx = this}) {}

  Context(Context&& other) noexcept
      : meta_data_map_(std::move(other.meta_data_map_)),
        type_(other.type_),
        base_(ibmw_transport_context_base_t{
            .ops = GenOpsBase(),
            .ctx = this}) {}

  Context& operator=(const Context& other) = delete;
  Context& operator=(Context&& other) = delete;

  const ibmw_transport_context_base_t* NativeHandle() const { return &base_; }

  bool CheckUsed() const { return used_; }
  void SetUsed() { used_ = true; }

  void Reset() {
    used_ = false;

    meta_data_map_.clear();
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
      meta_data_map_.emplace(key, val);
    } else {
      finditr->second = std::string(val);
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
          return static_cast<Context*>(ctx)->used_;
        },
        .set_used = [](void* ctx) {
          static_cast<Context*>(ctx)->used_ = true;  //
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
          auto& self = *static_cast<Context*>(ctx);

          const auto& meta_data_map = self.meta_data_map_;
          auto& string_buffer_vec = self.string_buffer_vec_;

          string_buffer_vec.clear();
          string_buffer_vec.reserve(meta_data_map.size());

          for (const auto& it : meta_data_map)
            string_buffer_vec.emplace_back(ibmw::util::ToIbmwStringView(it.first));

          return ibmw_string_view_array_t{
              .str_array = string_buffer_vec.data(),
              .len = string_buffer_vec.size()};
        },
        .get_meta_key_vals = [](void* ctx) -> ibmw_string_view_array_t {
          auto& self = *static_cast<Context*>(ctx);

          const auto& meta_data_map = self.meta_data_map_;
          auto& string_buffer_vec = self.string_buffer_vec_;

          string_buffer_vec.clear();
          string_buffer_vec.reserve(meta_data_map.size() * 2);

          for (const auto& it : meta_data_map) {
            string_buffer_vec.emplace_back(ibmw::util::ToIbmwStringView(it.first));
            string_buffer_vec.emplace_back(ibmw::util::ToIbmwStringView(it.second));
          }

          return ibmw_string_view_array_t{
              .str_array = string_buffer_vec.data(),
              .len = string_buffer_vec.size()};
        }};

    return &kOps;
  }

 private:
#if IBMW_HAS_STD_MEMORY_RESOURCE
  alignas(std::max_align_t) std::array<std::byte, 512> buffer_;
  std::pmr::monotonic_buffer_resource default_pool_{buffer_.data(), buffer_.size()};
#endif

  MetaDataMap meta_data_map_;

  std::vector<ibmw_string_view_t> string_buffer_vec_;

  bool used_ = false;

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
    for (size_t ii = 0; ii < len; ii += 2) {
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
    for (size_t ii = 0; ii < len; ii += 2) {
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
