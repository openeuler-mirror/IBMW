// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include "c/util/buffer_base.h"

#include "rclcpp/rclcpp.hpp"
#include "rclcpp/serialization.hpp"

namespace ibmw {

class Ros2RclSerializedMessageAdapter {
 public:
  explicit Ros2RclSerializedMessageAdapter(ibmw_buffer_array_with_allocator_t* bawa)
      : serialized_msg_(rcutils_uint8_array_t{
            .buffer = nullptr,
            .buffer_length = 0,
            .buffer_capacity = 0,
            .allocator = rcutils_allocator_t{
                .allocate = [](size_t size, void* state) -> void* {
                  auto* bawa = static_cast<ibmw_buffer_array_with_allocator_t*>(state);
                  const ibmw_buffer_array_allocator_t* allocator = bawa->allocator;
                  ibmw_buffer_t buffer = allocator->allocate(allocator->ctx, bawa->buffer_array, size);
                  return buffer.data;
                },
                .deallocate = [](void* pointer, void* state) {
                  if (pointer == nullptr) [[unlikely]]
                    return;

                  auto* bawa = static_cast<ibmw_buffer_array_with_allocator_t*>(state);
                  for (size_t ii = 0; ii < bawa->buffer_array->len; ++ii) {
                    if (bawa->buffer_array->data[ii].data == pointer) {
                      bawa->buffer_array->data[ii].len = 0;
                      return;
                    }
                  }
                  fprintf(stderr, "Invalid pointer for deallocate. Warning by Ros2RclSerializedMessageAdapter.\n");
                },
                .reallocate = [](void* pointer, size_t size, void* state) -> void* {
                  auto* bawa = static_cast<ibmw_buffer_array_with_allocator_t*>(state);

                  if (pointer == nullptr) [[unlikely]] {
                    const ibmw_buffer_array_allocator_t* allocator = bawa->allocator;
                    ibmw_buffer_t buffer = allocator->allocate(allocator->ctx, bawa->buffer_array, size);
                    return buffer.data;
                  }

                  for (size_t ii = 0; ii < bawa->buffer_array->len; ++ii) {
                    if (bawa->buffer_array->data[ii].data == pointer) {
                      bawa->buffer_array->data[ii].len = 0;

                      const ibmw_buffer_array_allocator_t* allocator = bawa->allocator;
                      ibmw_buffer_t buffer = allocator->allocate(allocator->ctx, bawa->buffer_array, size);
                      return buffer.data;
                    }
                  }

                  fprintf(stderr, "Invalid pointer for deallocate. Warning by Ros2RclSerializedMessageAdapter.\n");
                  return nullptr;
                },
                .zero_allocate = [](size_t number_of_elements, size_t size_of_element, void* state) -> void* {
                  auto* bawa = static_cast<ibmw_buffer_array_with_allocator_t*>(state);
                  const ibmw_buffer_array_allocator_t* allocator = bawa->allocator;
                  ibmw_buffer_t buffer = allocator->allocate(allocator->ctx, bawa->buffer_array, number_of_elements * size_of_element);
                  memset(buffer.data, 0, buffer.len);
                  return buffer.data;
                },
                .state = bawa}}) {}
  ~Ros2RclSerializedMessageAdapter() = default;

  Ros2RclSerializedMessageAdapter(const Ros2RclSerializedMessageAdapter&) = delete;
  Ros2RclSerializedMessageAdapter& operator=(const Ros2RclSerializedMessageAdapter&) = delete;

  rcl_serialized_message_t* GetRclSerializedMessage() { return &serialized_msg_; }

 private:
  rcl_serialized_message_t serialized_msg_;
};

}  // namespace ibmw
