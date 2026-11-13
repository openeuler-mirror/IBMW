// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

// Regression tests for EXT-013: the parameter service validates the whole
// request before applying it and always answers, also on failure.

#include <gtest/gtest.h>

#include <stdexcept>

#include "parameter_ext/service.h"

namespace ibmw::extensions::parameter_extension {
namespace {

namespace proto = ibmw::protocols::parameter_extension;

class TestableService : public ParameterServiceImpl {
 public:
  template <typename Req, typename Rsp>
  Rsp Call(void (ParameterServiceImpl::*handler)(const Req&, Rsp&), const Req& req) {
    Rsp rsp;
    Invoke(handler, &req, &rsp);
    return rsp;
  }
  using ParameterServiceImpl::HandleDump;
  using ParameterServiceImpl::HandleGet;
  using ParameterServiceImpl::HandleLoad;
  using ParameterServiceImpl::HandleSet;

  // Fills part of the answer, then fails.
  void FillThenThrow(const proto::DumpParameterReq&, proto::DumpParameterRsp& rsp) {
    rsp.module_parameter_map["mod"];
    throw std::runtime_error("failed part way");
  }
};

class ParameterServiceRegressionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    store_.Initialize(YAML::Node());
    store_.GetParamsAccess("mod");
    store_.Start();
    service_.SetParameterStore(&store_);
  }

  std::string Get(const std::string& key) {
    auto ptr = store_.GetParams("mod")->GetParameter(key);
    return ptr ? *ptr : "<unset>";
  }

  runtime::core::parameter::ParameterStore store_;
  TestableService service_;
};

// EXT-014: a call that arrives without a parameter store gets an answer.
TEST(ParameterServiceNotReadyTest, MissingStoreIsReported) {
  TestableService service;
  auto set = service.Call(&TestableService::HandleSet, proto::SetParameterReq{"mod", "k", "v"});
  EXPECT_EQ(set.msg, "NOT_READY");
  EXPECT_NE(set.code, 0u);
  EXPECT_EQ(service.Call(&TestableService::HandleGet, proto::GetParameterReq{"mod", "k"}).msg, "NOT_READY");
}

TEST_F(ParameterServiceRegressionTest, SetRejectsUnknownModuleAndEmptyKey) {
  auto ok = service_.Call(&TestableService::HandleSet, proto::SetParameterReq{"mod", "k", "v"});
  EXPECT_EQ(ok.code, 0u);
  EXPECT_EQ(Get("k"), "v");

  auto bad_module = service_.Call(&TestableService::HandleSet, proto::SetParameterReq{"nope", "k", "x"});
  EXPECT_EQ(bad_module.msg, "INVALID_MODULE_NAME");
  auto bad_key = service_.Call(&TestableService::HandleSet, proto::SetParameterReq{"mod", "", "x"});
  EXPECT_EQ(bad_key.msg, "INVALID_PARAMETER_NAME");
  EXPECT_EQ(store_.GetParams("mod")->ListParameter().size(), 1u);
}

TEST_F(ParameterServiceRegressionTest, LoadIsAllOrNothing) {
  proto::LoadParameterReq req;
  req.module_parameter_map["mod"].value = {{"a", "1"}};
  req.module_parameter_map["zzz_unknown"].value = {{"b", "2"}};  // after "mod" in the map
  auto rsp = service_.Call(&TestableService::HandleLoad, req);
  EXPECT_EQ(rsp.msg, "INVALID_MODULE_NAME");
  EXPECT_EQ(Get("a"), "<unset>");

  req.module_parameter_map.erase("zzz_unknown");
  req.module_parameter_map["mod"].value = {{"a", "1"}, {"", "2"}};
  rsp = service_.Call(&TestableService::HandleLoad, req);
  EXPECT_EQ(rsp.msg, "INVALID_PARAMETER_NAME");
  EXPECT_EQ(Get("a"), "<unset>");

  req.module_parameter_map["mod"].value = {{"a", "1"}, {"b", "2"}};
  rsp = service_.Call(&TestableService::HandleLoad, req);
  EXPECT_EQ(rsp.code, 0u);
  EXPECT_EQ(Get("a"), "1");
  EXPECT_EQ(Get("b"), "2");
}

TEST_F(ParameterServiceRegressionTest, FailedDumpReturnsNoPartialResult) {
  proto::DumpParameterReq req;
  req.module_names = {"mod", "nope"};
  auto rsp = service_.Call(&TestableService::HandleDump, req);
  EXPECT_EQ(rsp.msg, "INVALID_MODULE_NAME");
  EXPECT_TRUE(rsp.module_parameter_map.empty());
}

// A store that is not started makes GetParams throw: the service answers
// with an error instead of letting the exception skip the callback.
TEST(ParameterServiceNotStarted, ExceptionBecomesErrorCode) {
  runtime::core::parameter::ParameterStore store;
  store.Initialize(YAML::Node());
  TestableService service;
  service.SetParameterStore(&store);
  proto::GetParameterRsp rsp;
  EXPECT_NO_THROW(rsp = service.Call(&TestableService::HandleGet, proto::GetParameterReq{"mod", "k"}));
  EXPECT_EQ(rsp.msg, "INTERNAL_ERROR");
}

TEST(ParameterServiceNotStarted, ExceptionDiscardsPartialAnswer) {
  runtime::core::parameter::ParameterStore store;
  TestableService service;
  service.SetParameterStore(&store);
  const auto handler = static_cast<void (ParameterServiceImpl::*)(const proto::DumpParameterReq&,
                                                                  proto::DumpParameterRsp&)>(
      &TestableService::FillThenThrow);
  proto::DumpParameterRsp rsp;
  EXPECT_NO_THROW(rsp = service.Call(handler, proto::DumpParameterReq{}));
  EXPECT_EQ(rsp.msg, "INTERNAL_ERROR");
  EXPECT_TRUE(rsp.module_parameter_map.empty());
}

}  // namespace
}  // namespace ibmw::extensions::parameter_extension
