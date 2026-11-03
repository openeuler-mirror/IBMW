// Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

namespace ibmw::util {

// True when a provider's C function table is at least as large as the struct
// this consumer was compiled against (API-024). Checked when a table is
// obtained, never per call.
template <class Table>
constexpr bool IsCompleteAbiTable(const Table* table) noexcept {
  return table != nullptr && table->struct_size >= sizeof(Table);
}

}  // namespace ibmw::util
