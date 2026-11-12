// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "parameter_ext/parameter_type_support.h"
#include "cpp/util/json_type_support.h"
#include <stdexcept>

namespace ibmw::extensions::parameter_extension {
const ibmw_type_support_base_t* FindParameterTypeSupport(std::string_view name) {
  using namespace ibmw::protocols::parameter_extension;
  if (name == SetParameterReq::kTypeName) return ibmw::json::GetJsonMessageTypeSupport<SetParameterReq>();
  if (name == SetParameterRsp::kTypeName) return ibmw::json::GetJsonMessageTypeSupport<SetParameterRsp>();
  if (name == GetParameterReq::kTypeName) return ibmw::json::GetJsonMessageTypeSupport<GetParameterReq>();
  if (name == GetParameterRsp::kTypeName) return ibmw::json::GetJsonMessageTypeSupport<GetParameterRsp>();
  if (name == ListParameterReq::kTypeName) return ibmw::json::GetJsonMessageTypeSupport<ListParameterReq>();
  if (name == ListParameterRsp::kTypeName) return ibmw::json::GetJsonMessageTypeSupport<ListParameterRsp>();
  if (name == DumpParameterReq::kTypeName) return ibmw::json::GetJsonMessageTypeSupport<DumpParameterReq>();
  if (name == DumpParameterRsp::kTypeName) return ibmw::json::GetJsonMessageTypeSupport<DumpParameterRsp>();
  if (name == LoadParameterReq::kTypeName) return ibmw::json::GetJsonMessageTypeSupport<LoadParameterReq>();
  if (name == LoadParameterRsp::kTypeName) return ibmw::json::GetJsonMessageTypeSupport<LoadParameterRsp>();
  if (name == ListModulesReq::kTypeName) return ibmw::json::GetJsonMessageTypeSupport<ListModulesReq>();
  if (name == ListModulesRsp::kTypeName) return ibmw::json::GetJsonMessageTypeSupport<ListModulesRsp>();
  throw std::invalid_argument("Unknown parameter protocol type.");
}
}  // namespace ibmw::extensions::parameter_extension
