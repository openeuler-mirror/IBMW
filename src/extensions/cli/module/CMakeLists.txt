# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

add_library(ibmw_module_cli_provider SHARED
            module_command.cc module_cli_provider.cc list_command.cc info_command.cc)
# Registry queries use the existing shared runtime support, without an Engine.
target_link_libraries(ibmw_module_cli_provider
  PRIVATE ibmw::cli::command ibmw::core::extension_support)
set_target_properties(ibmw_module_cli_provider PROPERTIES OUTPUT_NAME "ibmw_module_cli")
add_werror(ibmw_module_cli_provider)

set_property(GLOBAL APPEND PROPERTY IBMW_CLI_PROVIDER_TARGETS ibmw_module_cli_provider)
if(IBMW_INSTALL)
  install(TARGETS ibmw_module_cli_provider LIBRARY DESTINATION bin)
endif()
