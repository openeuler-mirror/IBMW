cmake_minimum_required(VERSION 3.24)
project(ibmw_installed_consumer LANGUAGES C CXX)

find_package(ibmw REQUIRED CONFIG)

add_executable(reporter_c11 reporter_c11.c)
set_target_properties(reporter_c11 PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
target_link_libraries(reporter_c11 PRIVATE ibmw::core::extension_api)

add_library(reporter_plugin MODULE reporter_plugin.cc)
target_link_libraries(reporter_plugin PRIVATE ibmw::core::extension_api ${CMAKE_DL_LIBS})
set_target_properties(reporter_plugin PROPERTIES CXX_VISIBILITY_PRESET hidden
                                               VISIBILITY_INLINES_HIDDEN YES)

add_executable(installed_consumer installed_consumer.cc)
target_compile_features(installed_consumer PRIVATE cxx_std_20)
target_compile_definitions(installed_consumer PRIVATE
  IBMW_EXPECT_REPORTER=$<BOOL:${IBMW_EXPECT_REPORTER}>
  REPORTER_PLUGIN_PATH="$<TARGET_FILE:reporter_plugin>")
target_link_libraries(installed_consumer PRIVATE
  ibmw::core::reporter_host_abi ibmw::core::engine ${CMAKE_DL_LIBS})
target_link_options(installed_consumer PRIVATE -Wl,--export-dynamic)

add_executable(abi_golden abi_golden.cc)
target_compile_features(abi_golden PRIVATE cxx_std_20)
target_link_libraries(abi_golden PRIVATE ibmw::core::engine)
