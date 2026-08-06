# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

add_executable(retained_loan_e2e retained_loan_e2e.cc)

target_link_libraries(retained_loan_e2e PRIVATE
  ibmw::core::engine
  ibmw::extensions::iceoryx_ext
  iceoryx_posh::iceoryx_posh)

include(AddWerror)
add_werror(retained_loan_e2e)
