// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#pragma once

#include <deque>
#include <string>
#include <vector>

#include "cli/service/call_client.h"
#include "gtest/gtest.h"
#include "nlohmann/json.hpp"

namespace ibmw::diag {
struct RpcRequest {
  std::string backend;
  std::string function;
  std::string serialization;
  std::string diagnostic;
  nlohmann::json payload;
  std::chrono::milliseconds timeout;
  std::chrono::milliseconds discovery_timeout;
};

struct RpcFake {
  std::deque<cli::service::CallResult> replies;
  std::vector<RpcRequest> requests;
};

RpcFake& Rpc();

class RpcTest : public testing::Test {
 protected:
  void SetUp() override { Rpc() = {}; }
  void TearDown() override {
    EXPECT_TRUE(Rpc().replies.empty()) << "Expected RPC response was not consumed";
    Rpc() = {};
  }
  void Reply(nlohmann::json value, int code = 0) {
    Rpc().replies.push_back({code, value.dump()});
  }
};
}  // namespace ibmw::diag
