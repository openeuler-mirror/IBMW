# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

include(FetchContent)

message(STATUS "get nlohmann_json ...")

set(nlohmann_json_DOWNLOAD_URL
    "https://github.com/nlohmann/json/archive/v3.11.3.tar.gz"
    CACHE STRING "")

if(nlohmann_json_LOCAL_SOURCE)
  FetchContent_Declare(
    nlohmann_json
    SOURCE_DIR ${nlohmann_json_LOCAL_SOURCE}
    OVERRIDE_FIND_PACKAGE)
else()
  FetchContent_Declare(
    nlohmann_json
    URL ${nlohmann_json_DOWNLOAD_URL}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    OVERRIDE_FIND_PACKAGE)
endif()

# Wrap it in a function to restrict the scope of the variables
function(get_nlohmann_json)
  FetchContent_GetProperties(nlohmann_json)
  if(NOT nlohmann_json_POPULATED)
    FetchContent_MakeAvailable(nlohmann_json)
  endif()
endfunction()

get_nlohmann_json()

# JSON is part of the public TypeSupport contract. Export it independently of
# the upstream JSON_Install option (OFF by default when fetched as a subproject).
if(IBMW_INSTALL)
  get_target_property(_ibmw_json_imported nlohmann_json::nlohmann_json IMPORTED)
  if(NOT _ibmw_json_imported)
    set(_ibmw_json_build_config "${CMAKE_BINARY_DIR}/build-tree-config/lib/cmake/nlohmann_json")
    if(JSON_Install)
      # Upstream already owns the install/export set. A second export would
      # make references from ibmw-targets ambiguous, so forward to that config.
      file(MAKE_DIRECTORY "${_ibmw_json_build_config}")
      file(WRITE "${_ibmw_json_build_config}/nlohmann_jsonConfig.cmake"
        "include(\"${nlohmann_json_BINARY_DIR}/nlohmann_jsonConfig.cmake\")\n")
    else()
      install(TARGETS nlohmann_json EXPORT ibmw-json-targets)
      install(EXPORT ibmw-json-targets
        FILE nlohmann_jsonTargets.cmake NAMESPACE nlohmann_json::
        DESTINATION lib/cmake/nlohmann_json)
      if(JSON_MultipleHeaders)
        set(_ibmw_json_headers "${nlohmann_json_SOURCE_DIR}/include/nlohmann")
      else()
        set(_ibmw_json_headers "${nlohmann_json_SOURCE_DIR}/single_include/nlohmann")
      endif()
      install(DIRECTORY "${_ibmw_json_headers}" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
      install(FILES "${nlohmann_json_BINARY_DIR}/nlohmann_jsonConfig.cmake"
                    "${nlohmann_json_BINARY_DIR}/nlohmann_jsonConfigVersion.cmake"
        DESTINATION lib/cmake/nlohmann_json)
      export(EXPORT ibmw-json-targets NAMESPACE nlohmann_json::
        FILE "${_ibmw_json_build_config}/nlohmann_jsonTargets.cmake")
      configure_file("${nlohmann_json_BINARY_DIR}/nlohmann_jsonConfig.cmake"
                     "${_ibmw_json_build_config}/nlohmann_jsonConfig.cmake" COPYONLY)
    endif()
    configure_file("${nlohmann_json_BINARY_DIR}/nlohmann_jsonConfigVersion.cmake"
                   "${_ibmw_json_build_config}/nlohmann_jsonConfigVersion.cmake" COPYONLY)
  endif()
endif()

# import targets:
# nlohmann_json::nlohmann_json
