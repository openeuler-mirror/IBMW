if(NOT DEFINED READELF OR NOT EXISTS "${READELF}")
  message(FATAL_ERROR "READELF must name an available ELF symbol reader")
endif()
if(NOT DEFINED ARTIFACTS OR ARTIFACTS STREQUAL "")
  message(FATAL_ERROR "ARTIFACTS must contain at least one ELF artifact")
endif()

set(required_typeinfo
  "_ZTIN4ibmw7runtime4core9transport15TransportDriverE"
  "_ZTIN4ibmw7runtime4core9transport8internal22CommTraceSendOverlayV1E")

foreach(artifact IN LISTS ARTIFACTS)
  if(NOT EXISTS "${artifact}")
    message(FATAL_ERROR "RTTI audit artifact does not exist: ${artifact}")
  endif()
  execute_process(
    COMMAND "${READELF}" --dyn-syms --wide "${artifact}"
    RESULT_VARIABLE readelf_result
    OUTPUT_VARIABLE symbols
    ERROR_VARIABLE readelf_error)
  if(NOT readelf_result EQUAL 0)
    message(FATAL_ERROR "readelf failed for ${artifact}: ${readelf_error}")
  endif()

  foreach(symbol IN LISTS required_typeinfo)
    set(symbol_matches 0)
    set(visible_matches 0)
    string(REPLACE "\n" ";" symbol_lines "${symbols}")
    foreach(symbol_line IN LISTS symbol_lines)
      string(STRIP "${symbol_line}" symbol_line)
      if(symbol_line MATCHES "[ \t]${symbol}$")
        math(EXPR symbol_matches "${symbol_matches} + 1")
        if(symbol_line MATCHES
            "^[0-9]+:[ \t]+[^ \t]+[ \t]+[0-9]+[ \t]+[^ \t]+[ \t]+(GLOBAL|WEAK)[ \t]+DEFAULT[ \t]+[0-9]+[ \t]+${symbol}$")
          math(EXPR visible_matches "${visible_matches} + 1")
        endif()
      endif()
    endforeach()
    if(symbol_matches EQUAL 0)
      message(FATAL_ERROR "Required cross-DSO RTTI is missing from ${artifact}: ${symbol}")
    elseif(NOT symbol_matches EQUAL 1)
      message(FATAL_ERROR
        "Required cross-DSO RTTI is ambiguous in ${artifact}: ${symbol} has ${symbol_matches} dynamic symbol entries")
    elseif(NOT visible_matches EQUAL 1)
      message(FATAL_ERROR
        "Required cross-DSO RTTI is not GLOBAL/WEAK DEFAULT in ${artifact}: ${symbol}")
    endif()
  endforeach()

  foreach(symbol IN ITEMS
      "_ZTVN4ibmw7runtime4core9transport15TransportDriverE"
      "_ZTVN4ibmw7runtime4core9transport8internal22CommTraceSendOverlayV1E")
    foreach(symbol_line IN LISTS symbol_lines)
      string(STRIP "${symbol_line}" symbol_line)
      if(symbol_line MATCHES "[ \t]${symbol}$" AND NOT symbol_line MATCHES
          "^[0-9]+:[ \t]+[^ \t]+[ \t]+[0-9]+[ \t]+[^ \t]+[ \t]+(GLOBAL|WEAK)[ \t]+DEFAULT[ \t]+[0-9]+[ \t]+${symbol}$")
        message(FATAL_ERROR
          "Cross-DSO interface vtable is present but not GLOBAL/WEAK DEFAULT in ${artifact}: ${symbol_line}")
      endif()
    endforeach()
  endforeach()
endforeach()
