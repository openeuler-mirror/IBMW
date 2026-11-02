// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace ibmw::extensions::grpc_extension::http2 {

class SimpleBuffer {
 public:
  explicit SimpleBuffer(size_t capacity) {
    data_.reserve(capacity);
  }
  ~SimpleBuffer() = default;

  void Write(const uint8_t* data, size_t len) {
    data_.append(reinterpret_cast<const char*>(data), len);
  }

  // Consumes at most the readable bytes.
  void Consume(size_t len) {
    read_pos_ += std::min(len, GetReadableSize());
  }

  // Copies at most the readable bytes and returns how many it copied.
  size_t Read(uint8_t* data, size_t len) {
    len = std::min(len, GetReadableSize());
    std::memcpy(data, data_.data() + read_pos_, len);
    return len;
  }

  [[nodiscard]] std::string_view GetStringView() const {
    return {data_.data() + read_pos_, data_.size() - read_pos_};
  }

  [[nodiscard]] size_t GetReadableSize() const { return data_.size() - read_pos_; }

  [[nodiscard]] bool Empty() const { return GetReadableSize() == 0; }

 private:
  std::string data_;
  size_t read_pos_ = 0;
};

}  // namespace ibmw::extensions::grpc_extension::http2
