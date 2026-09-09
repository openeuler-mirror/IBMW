# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

function(ibmw_resolve_iceoryx_roudi_target output_variable)
  if(TARGET iox-roudi)
    set(${output_variable} iox-roudi PARENT_SCOPE)
  elseif(TARGET iceoryx_posh::iox-roudi)
    set(${output_variable} iceoryx_posh::iox-roudi PARENT_SCOPE)
  else()
    set(${output_variable} "" PARENT_SCOPE)
  endif()
endfunction()

function(ibmw_add_iceoryx_roudi_dependency consumer)
  ibmw_resolve_iceoryx_roudi_target(_ibmw_roudi_target)
  if(NOT _ibmw_roudi_target)
    message(FATAL_ERROR "No iceoryx RouDi executable target is available")
  endif()
  get_target_property(_ibmw_roudi_imported ${_ibmw_roudi_target} IMPORTED)
  if(NOT _ibmw_roudi_imported)
    add_dependencies(${consumer} ${_ibmw_roudi_target})
  endif()
endfunction()

function(ibmw_generate_iceoryx_roudi_path output_file)
  ibmw_resolve_iceoryx_roudi_target(_ibmw_roudi_target)
  if(NOT _ibmw_roudi_target)
    message(FATAL_ERROR "No iceoryx RouDi executable target is available")
  endif()
  file(GENERATE OUTPUT "${output_file}"
    CONTENT "$<TARGET_FILE:${_ibmw_roudi_target}>\n")
endfunction()
