# Copyright (c) 2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# CO-26: the defined dynamic symbols of LIB (nm -D --defined-only) must be
# exactly the allow-list ALLOW, and none may be STB_GNU_UNIQUE.
# Usage: cmake -DNM=<nm> -DLIB=<library> -DALLOW=<allow-list> -P check_exports.cmake

execute_process(
  COMMAND ${NM} -D --defined-only -C ${LIB}
  OUTPUT_VARIABLE nm_out
  ERROR_VARIABLE nm_err
  RESULT_VARIABLE nm_rc)
if(NOT nm_rc EQUAL 0)
  message(FATAL_ERROR "${NM} -D --defined-only ${LIB} failed (${nm_rc}): ${nm_err}")
endif()

string(REPLACE "\n" ";" nm_lines "${nm_out}")
set(actual "")
set(unique "")
foreach(line IN LISTS nm_lines)
  # "<value> <type> <name>"; the name may contain spaces.
  if(line MATCHES "^[0-9a-fA-F]* ([A-Za-z]) (.+)$")
    set(type "${CMAKE_MATCH_1}")
    string(REGEX REPLACE "std::__[A-Za-z0-9]+::" "std::" name "${CMAKE_MATCH_2}")
    list(APPEND actual "${name}")
    if(type STREQUAL "u")
      list(APPEND unique "${name}")
    endif()
  endif()
endforeach()
list(REMOVE_DUPLICATES actual)
list(SORT actual)

file(STRINGS ${ALLOW} allow_lines)
set(expected "")
foreach(line IN LISTS allow_lines)
  if(NOT line STREQUAL "" AND NOT line MATCHES "^#")
    list(APPEND expected "${line}")
  endif()
endforeach()
list(SORT expected)

set(unexpected ${actual})
list(REMOVE_ITEM unexpected ${expected})
set(missing ${expected})
list(REMOVE_ITEM missing ${actual})

list(LENGTH actual n)
string(REPLACE ";" "\n  " actual_text "${actual}")
message(STATUS "${LIB}: ${n} exported symbols:\n  ${actual_text}")
if(unexpected OR missing OR unique)
  string(REPLACE ";" "\n  " unexpected_text "${unexpected}")
  string(REPLACE ";" "\n  " missing_text "${missing}")
  string(REPLACE ";" "\n  " unique_text "${unique}")
  message(FATAL_ERROR "FAIL: exported symbols differ from ${ALLOW}\n"
                      "unexpected:\n  ${unexpected_text}\nmissing:\n  ${missing_text}\n"
                      "STB_GNU_UNIQUE:\n  ${unique_text}")
endif()
message(STATUS "PASS")
