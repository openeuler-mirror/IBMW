if(NOT NM OR NOT LIB OR NOT EXE)
  message(FATAL_ERROR "NM, LIB, and EXE are required")
endif()

set(forbidden_pattern
  "dds_comm_trace|comm_trace::|ibmw_dds_comm_trace|RuntimeFaultHooksForTest|Inject(Stage|Exact)DroppedForTest|runtime_fault_hooks_for_test")

foreach(artifact IN ITEMS "${LIB}" "${EXE}")
  foreach(nm_mode IN ITEMS all dynamic)
    if(nm_mode STREQUAL "all")
      set(nm_args -a -C)
    else()
      set(nm_args -D -C)
    endif()
    execute_process(
      COMMAND "${NM}" ${nm_args} "${artifact}"
      RESULT_VARIABLE nm_result
      OUTPUT_VARIABLE symbols
      ERROR_VARIABLE nm_error)
    if(NOT nm_result EQUAL 0)
      message(FATAL_ERROR "nm ${nm_args} failed for ${artifact}: ${nm_error}")
    endif()
    if(symbols MATCHES "${forbidden_pattern}")
      message(FATAL_ERROR
        "DDS communication trace source/API/hook symbol exists in OFF artifact ${artifact} (${nm_mode} symbols)")
    endif()
  endforeach()
endforeach()
