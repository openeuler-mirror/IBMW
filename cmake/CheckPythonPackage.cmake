# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Sets result_var to ON if Python3_EXECUTABLE can import module_name. Checks the
# import name, so it works without pip and for distribution packages whose
# package name differs (python3-yaml provides "yaml") (TOOL-047).
function(check_python_package module_name result_var)
  execute_process(
    COMMAND ${Python3_EXECUTABLE} -c "import importlib.util, sys; sys.exit(importlib.util.find_spec(sys.argv[1]) is None)" ${module_name}
    RESULT_VARIABLE result
    OUTPUT_QUIET ERROR_QUIET)
  if(NOT ${result} EQUAL 0)
    set(${result_var}
        OFF
        PARENT_SCOPE)
    message(WARNING "Cannot import ${module_name} in your Python environment!")
  else()
    set(${result_var}
        ON
        PARENT_SCOPE)
  endif()
endfunction()
