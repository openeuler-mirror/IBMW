// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
#include "log_control_ext/log_control_type_support.h"
#include "cpp/util/json_type_support.h"
#include <stdexcept>

namespace ibmw::extensions::log_control_extension {
const ibmw_type_support_base_t* FindLogControlTypeSupport(std::string_view name) {
  using namespace ibmw::protocols::log_control_extension;
  if (name == GetModuleLogLevelReq::kTypeName) return ibmw::json::GetJsonMessageTypeSupport<GetModuleLogLevelReq>();
  if (name == GetModuleLogLevelRsp::kTypeName) return ibmw::json::GetJsonMessageTypeSupport<GetModuleLogLevelRsp>();
  if (name == SetModuleLogLevelReq::kTypeName) return ibmw::json::GetJsonMessageTypeSupport<SetModuleLogLevelReq>();
  if (name == SetModuleLogLevelRsp::kTypeName) return ibmw::json::GetJsonMessageTypeSupport<SetModuleLogLevelRsp>();
  throw std::invalid_argument("Unknown log control protocol type.");
}
}  // namespace ibmw::extensions::log_control_extension
