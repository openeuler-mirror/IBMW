# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

# Get the current folder name
string(REGEX REPLACE ".*/\(.*\)" "\\1" CUR_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Get namespace
get_namespace(CUR_SUPERIOR_NAMESPACE)
string(REPLACE "::" "_" CUR_SUPERIOR_NAMESPACE_UNDERLINE ${CUR_SUPERIOR_NAMESPACE})

# Set target name
set(CUR_TARGET_NAME ${CUR_SUPERIOR_NAMESPACE_UNDERLINE}_${CUR_DIR})
set(CUR_TARGET_ALIAS_NAME ${CUR_SUPERIOR_NAMESPACE}::${CUR_DIR})

# Set file collection
file(GLOB_RECURSE head_files ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

# Add target
add_library(${CUR_TARGET_NAME} INTERFACE)
add_library(${CUR_TARGET_ALIAS_NAME} ALIAS ${CUR_TARGET_NAME})

# Set include path of target
target_include_directories(
  ${CUR_TARGET_NAME}
  INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
            $<INSTALL_INTERFACE:include/ibmw>)

# Set head files of target
target_sources(${CUR_TARGET_NAME} INTERFACE FILE_SET HEADERS BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR} FILES ${head_files})

# Set installation of target
if(IBMW_INSTALL)
  set_property(TARGET ${CUR_TARGET_NAME} PROPERTY EXPORT_NAME ${CUR_TARGET_ALIAS_NAME})
  install(
    TARGETS ${CUR_TARGET_NAME}
    EXPORT ${INSTALL_CONFIG_NAME}
    FILE_SET HEADERS
    DESTINATION include/ibmw/${CUR_DIR})
endif()

# Every C ABI header must compile on its own as C11 (API-025).
if(IBMW_BUILD_TESTS)
  set(c_header_test_srcs)
  foreach(head_file ${head_files})
    file(RELATIVE_PATH rel_path ${CMAKE_CURRENT_SOURCE_DIR}/.. ${head_file})
    string(MAKE_C_IDENTIFIER ${rel_path} test_name)
    set(test_src ${CMAKE_CURRENT_BINARY_DIR}/c_header_test/${test_name}.c)
    file(CONFIGURE OUTPUT ${test_src} CONTENT "#include \"${rel_path}\"\n")
    list(APPEND c_header_test_srcs ${test_src})
  endforeach()
  add_library(ibmw_c_header_self_contained_test OBJECT ${c_header_test_srcs})
  target_link_libraries(ibmw_c_header_self_contained_test PRIVATE ${CUR_TARGET_NAME})
  set_target_properties(ibmw_c_header_self_contained_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
                                                                     C_EXTENSIONS OFF)
endif()
