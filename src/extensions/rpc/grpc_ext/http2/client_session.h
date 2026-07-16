// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <forward_list>

#include "grpc_ext/http2/session.h"

namespace ibmw::extensions::grpc_extension::http2 {
// The following source code is from trpc-cpp.
// Copied and modified from
// https://github.com/trpc-group/trpc-cpp/blob/v1.2.0/trpc/codec/grpc/http2/client_session.h

class ClientSession : public Session {
 public:
  int InitSession(Http2Settings settings) override;

  int SubmitRequest(const RequestPtr& request_ptr) override;

  void OnFullResponse(const ResponsePtr& response_ptr);
  [[nodiscard]] std::forward_list<ResponsePtr> GetFullResponseList();

  bool ResponseListEmpty() const { return full_response_list_.empty(); }

 private:
  int SubmitSettings(Http2Settings settings);

 private:
  std::forward_list<ResponsePtr> full_response_list_;
};
// End of source code from trpc-cpp.

}  // namespace ibmw::extensions::grpc_extension::http2
