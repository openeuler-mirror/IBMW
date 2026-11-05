// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#include "engine/module/parameter_handle.h"

namespace ibmw::runtime::core::parameter {

std::shared_ptr<const std::string> ParameterHandle::GetParameter(std::string_view key) {
  std::lock_guard<std::mutex> lck(parameter_map_mutex_);

  auto find_itr = parameter_map_.find(key);
  if (find_itr != parameter_map_.end()) {
    MW_TRACE("Get parameter '{}'", key);

    return find_itr->second;
  }

  MW_TRACE("Can not get parameter '{}'", key);
  return std::shared_ptr<std::string>();
}

void ParameterHandle::SetParameter(
    std::string_view key, const std::shared_ptr<std::string>& value_ptr) {
  std::lock_guard<std::mutex> lck(parameter_map_mutex_);

  auto find_itr = parameter_map_.find(key);
  if (find_itr != parameter_map_.end()) {
    if (!value_ptr || value_ptr->empty()) [[unlikely]] {
      MW_TRACE("Erase parameter '{}'", key);
      parameter_map_.erase(find_itr);
      return;
    }

    MW_TRACE("Update parameter '{}'", key);
    find_itr->second = value_ptr;

    return;
  }

  MW_TRACE("Set parameter '{}'", key);
  parameter_map_.emplace(key, value_ptr);
}

std::vector<std::string> ParameterHandle::ListParameter() const {
  std::lock_guard<std::mutex> lck(parameter_map_mutex_);

  std::vector<std::string> result;
  result.reserve(parameter_map_.size());

  for (const auto& itr : parameter_map_) {
    result.emplace_back(itr.first);
  }

  return result;
}

void ParameterHandle::SetParameterValue(std::string_view key, std::string value) {
  auto ptr = std::make_shared<std::string>(std::move(value));
  std::lock_guard<std::mutex> lck(parameter_map_mutex_);
  parameter_map_.insert_or_assign(std::string(key), std::move(ptr));
}

}  // namespace ibmw::runtime::core::parameter
