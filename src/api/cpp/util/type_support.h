// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <algorithm>
#include <memory>
#include <span>

#include "c/util/type_support_base.h"
#include "cpp/util/buffer.h"
#include "cpp/util/string.h"

namespace ibmw::util {

/**
 * @brief TypeSupportRef
 * @note Basic components, priority is given to performance protection. Please check the legality of base_ptr by yourself
 *
 */
class TypeSupportRef {
 public:
  TypeSupportRef() = default;
  explicit TypeSupportRef(const ibmw_type_support_base_t* base_ptr)
      : base_ptr_(base_ptr) {}
  ~TypeSupportRef() = default;

  explicit operator bool() const { return (base_ptr_ != nullptr); }

  const ibmw_type_support_base_t* NativeHandle() const {
    return base_ptr_;
  }

  std::string_view TypeName() const {
    return ToStdStringView(base_ptr_->type_name(base_ptr_->ctx));
  }

  void* Create() const {
    return base_ptr_->create(base_ptr_->ctx);
  }

  void Destroy(void* msg) const {
    base_ptr_->destroy(base_ptr_->ctx, msg);
  }

  std::shared_ptr<void> CreateSharedPtr() const {
    return std::shared_ptr<void>(
        base_ptr_->create(base_ptr_->ctx),
        [base_ptr{this->base_ptr_}](void* ptr) {
          base_ptr->destroy(base_ptr->ctx, ptr);
        });
  }

  void Copy(const void* from, void* to) const {
    base_ptr_->copy(base_ptr_->ctx, from, to);
  }

  void Move(void* from, void* to) const {
    base_ptr_->move(base_ptr_->ctx, from, to);
  }

  bool Serialize(
      std::string_view serialization_type,
      const void* msg,
      const ibmw_buffer_array_allocator_t* allocator,
      ibmw_buffer_array_t* buffer_array) const {
    return base_ptr_->serialize(
        base_ptr_->ctx,
        ToIbmwStringView(serialization_type),
        msg,
        allocator,
        buffer_array);
  }

  bool Deserialize(
      std::string_view serialization_type,
      ibmw_buffer_array_view_t buffer_array_view,
      void* msg) const {
    return base_ptr_->deserialize(
        base_ptr_->ctx,
        ToIbmwStringView(serialization_type),
        buffer_array_view,
        msg);
  }

  size_t SerializationTypesSupportedNum() const {
    return base_ptr_->serialization_types_supported_num(base_ptr_->ctx);
  }

  const ibmw_string_view_t* SerializationTypesSupportedList() const {
    return base_ptr_->serialization_types_supported_list(base_ptr_->ctx);
  }

  std::span<const ibmw_string_view_t> SerializationTypesSupportedListSpan() const {
    return std::span<const ibmw_string_view_t>(
        base_ptr_->serialization_types_supported_list(base_ptr_->ctx),
        base_ptr_->serialization_types_supported_num(base_ptr_->ctx));
  }

  const void* CustomTypeSupportPtr() const {
    return base_ptr_->custom_type_support_ptr(base_ptr_->ctx);
  }

  std::string_view DefaultSerializationType() const {
    return ToStdStringView(base_ptr_->serialization_types_supported_list(base_ptr_->ctx)[0]);
  }

  bool CheckSerializationTypeSupported(std::string_view serialization_type) const {
    auto serialization_type_span = SerializationTypesSupportedListSpan();
    auto finditr = std::find_if(
        serialization_type_span.begin(), serialization_type_span.end(),
        [serialization_type](ibmw_string_view_t s) {
          return serialization_type == ToStdStringView(s);
        });

    return (finditr != serialization_type_span.end());
  }

  std::string SimpleSerialize(std::string_view serialization_type, const void* msg) const {
    ibmw::util::BufferArray buffer_array;

    bool ret = base_ptr_->serialize(
        base_ptr_->ctx,
        ToIbmwStringView(serialization_type),
        msg,
        buffer_array.AllocatorNativeHandle(),
        buffer_array.BufferArrayNativeHandle());

    if (ret)
      return buffer_array.JoinToString();

    return "";
  }

 private:
  const ibmw_type_support_base_t* base_ptr_ = nullptr;
};

}  // namespace ibmw::util
