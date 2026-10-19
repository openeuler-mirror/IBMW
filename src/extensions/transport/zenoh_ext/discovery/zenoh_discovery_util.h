// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <filesystem>
#include <stdexcept>
#include <string>
#include <zenoh.h>

#include "zenoh_discovery_protocol.h"

namespace ibmw::discovery::zenoh_detail {

template <typename T>
struct Owned {
  T value;
  Owned() { z_internal_null(&value); }
  ~Owned() { z_drop(z_move(value)); }
  Owned(const Owned&) = delete;
  Owned& operator=(const Owned&) = delete;
};

inline void Check(z_result_t code, const char* operation) {
  if (code < 0) throw std::runtime_error(std::string(operation) + " failed (" + std::to_string(code) + ").");
}

inline std::string ReadBytes(const z_loaned_bytes_t* bytes) {
  if (z_bytes_len(bytes) > zenoh_protocol::kMaxSnapshotBytes) throw std::runtime_error("Zenoh discovery reply exceeds size limit.");
  Owned<z_owned_string_t> text;
  Check(z_bytes_to_string(bytes, &text.value), "Read Zenoh reply");
  return {z_string_data(z_loan(text.value)), z_string_len(z_loan(text.value))};
}

inline void OpenSession(z_owned_session_t& session, const std::filesystem::path& path) {
  Owned<z_owned_config_t> config;
  if (path.empty()) z_config_default(&config.value);
  else Check(zc_config_from_file(&config.value, path.c_str()), "Load Zenoh configuration");
  Check(z_open(&session, z_move(config.value), nullptr), "Open Zenoh discovery session");
}

inline std::string SessionId(const z_loaned_session_t* session) {
  const auto id = z_info_zid(session);
  Owned<z_owned_string_t> text;
  z_id_to_string(&id, &text.value);
  return {z_string_data(z_loan(text.value)), z_string_len(z_loan(text.value))};
}

}  // namespace ibmw::discovery::zenoh_detail
