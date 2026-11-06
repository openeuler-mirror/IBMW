# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.

# Reuse topic discovery argument semantics without linking another CLI provider.
set(doctor_cli_sources doctor_command.cc doctor_cli_provider.cc topic_check.cc qos_check.cc
    ../topic/discovery_options.cc)
add_library(ibmw_doctor_cli_provider SHARED ${doctor_cli_sources})
target_include_directories(ibmw_doctor_cli_provider PRIVATE ../topic)
target_link_libraries(ibmw_doctor_cli_provider
  PRIVATE ibmw::cli::command ibmw::core::extension_support)
set_target_properties(ibmw_doctor_cli_provider PROPERTIES OUTPUT_NAME "ibmw_doctor_cli"
  INSTALL_RPATH "$ORIGIN;$ORIGIN/../lib")
add_werror(ibmw_doctor_cli_provider)
set_property(GLOBAL APPEND PROPERTY IBMW_CLI_PROVIDER_TARGETS ibmw_doctor_cli_provider)
if(IBMW_INSTALL)
  install(TARGETS ibmw_doctor_cli_provider LIBRARY DESTINATION bin)
endif()
