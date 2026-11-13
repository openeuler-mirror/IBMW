// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <cstdint>
#include <string_view>

#include "grpc_ext/http2/buffer.h"
#include "grpc_ext/http2/header.h"
#include "base/url_parser.h"

namespace ibmw::extensions::grpc_extension::http2 {

// Header the server sets to the real peer address of a request.
inline constexpr std::string_view kRemoteEndpointKey = "Remote-Endpoint";

class Request {
 public:
  using OnDataCallback = std::function<void(const uint8_t* data, size_t len)>;
  using OnEofCallback = std::function<void()>;
  using OnHeaderCallback = std::function<void(const Headers& headers)>;
  using OnRstCallback = std::function<void(uint32_t error_code)>;

 public:
  explicit Request(size_t capacity = 8192)
      : body_(capacity) {}

  [[nodiscard]] int32_t GetStreamId() const { return stream_id_; }
  void SetStreamId(int32_t stream_id) { stream_id_ = stream_id; }

  void SetUrl(const common::util::Url<std::string>& url) { url_ = url; }
  [[nodiscard]] const common::util::Url<std::string>& GetUrl() const { return url_; }
  [[nodiscard]] common::util::Url<std::string>& GetMutableUrl() { return url_; }

  [[nodiscard]] const std::string& GetMethod() const { return method_; }
  void SetMethod(std::string_view method) { method_ = std::string(method); }

  [[nodiscard]] const Headers& GetHeaders() const { return headers_; }
  [[nodiscard]] const SimpleBuffer& GetBody() const { return body_; }
  [[nodiscard]] SimpleBuffer& GetMutableBody() { return body_; }

  void AddHeader(std::string_view name, std::string_view value) {
    headers_.emplace(name, value);
  }

  // Overwrites any peer-supplied header of the same name.
  void SetHeader(std::string_view name, std::string_view value) {
    headers_.insert_or_assign(std::string(name), std::string(value));
  }

  void Write(const uint8_t* data, size_t len) {
    body_.Write(data, len);
  }

  void Write(const std::string& data) {
    body_.Write(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  }

 private:
  int32_t stream_id_ = -1;
  common::util::Url<std::string> url_;
  std::string method_;
  std::string scheme_;
  Headers headers_;
  SimpleBuffer body_;
};
using RequestPtr = std::shared_ptr<Request>;

}  // namespace ibmw::extensions::grpc_extension::http2
