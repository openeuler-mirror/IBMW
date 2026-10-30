# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

include(FetchContent)

message(STATUS "get opentelemetry_cpp ...")

set(opentelemetry_cpp_DOWNLOAD_URL
    "https://github.com/open-telemetry/opentelemetry-cpp/archive/v1.16.1.tar.gz"
    CACHE STRING "")
set(opentelemetry_cpp_DOWNLOAD_SHA256
    "b8a78bb2a3a78133dbb08bcd04342f4b1e03cb4a19079b8416d408d905fffc37"
    CACHE STRING "SHA256 of opentelemetry_cpp_DOWNLOAD_URL (update both together)")

if(opentelemetry_cpp_LOCAL_SOURCE)
  FetchContent_Declare(opentelemetry_cpp SOURCE_DIR ${opentelemetry_cpp_LOCAL_SOURCE})
else()
  FetchContent_Declare(
    opentelemetry_cpp
    URL ${opentelemetry_cpp_DOWNLOAD_URL}
    URL_HASH SHA256=${opentelemetry_cpp_DOWNLOAD_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

# Wrap it in a function to restrict the scope of the variables
function(get_opentelemetry_cpp)
  FetchContent_GetProperties(opentelemetry_cpp)
  if(NOT opentelemetry_cpp_POPULATED)
    set(BUILD_TESTING OFF)
    set(WITH_BENCHMARK OFF)
    set(WITH_EXAMPLES OFF)
    set(WITH_FUNC_TESTS OFF)
    set(WITH_NO_DEPRECATED_CODE ON)
    set(WITH_DEPRECATED_SDK_FACTORY OFF)
    set(WITH_OTLP_HTTP ON)

    set(WITH_STL
        "CXX20"
        CACHE STRING "")

    set(OTELCPP_PROTO_PATH
        ${opentelemetry_proto_SOURCE_DIR}
        CACHE PATH "")

    set(BUILD_SHARED_LIBS OFF)

    FetchContent_MakeAvailable(opentelemetry_cpp)

    if(TARGET opentelemetry_api)
      add_library(opentelemetry-cpp::api ALIAS opentelemetry_api)
    endif()
    if(TARGET opentelemetry_sdk)
      add_library(opentelemetry-cpp::sdk ALIAS opentelemetry_sdk)
    endif()
    if(TARGET opentelemetry_ext)
      add_library(opentelemetry-cpp::ext ALIAS opentelemetry_ext)
    endif()
    if(TARGET opentelemetry_version)
      add_library(opentelemetry-cpp::version ALIAS opentelemetry_version)
    endif()
    if(TARGET opentelemetry_common)
      add_library(opentelemetry-cpp::common ALIAS opentelemetry_common)
    endif()
    if(TARGET opentelemetry_trace)
      add_library(opentelemetry-cpp::trace ALIAS opentelemetry_trace)
    endif()
    if(TARGET opentelemetry_metrics)
      add_library(opentelemetry-cpp::metrics ALIAS opentelemetry_metrics)
    endif()
    if(TARGET opentelemetry_logs)
      add_library(opentelemetry-cpp::logs ALIAS opentelemetry_logs)
    endif()
    if(TARGET opentelemetry_exporter_ostream_span)
      add_library(opentelemetry-cpp::ostream_span_exporter ALIAS opentelemetry_exporter_ostream_span)
    endif()
    if(TARGET opentelemetry_exporter_ostream_metrics)
      add_library(opentelemetry-cpp::ostream_metrics_exporter ALIAS opentelemetry_exporter_ostream_metrics)
    endif()
    if(TARGET opentelemetry_exporter_ostream_logs)
      add_library(opentelemetry-cpp::ostream_log_record_exporter ALIAS opentelemetry_exporter_ostream_logs)
    endif()
    if(TARGET opentelemetry_otlp_recordable)
      add_library(opentelemetry-cpp::otlp_recordable ALIAS opentelemetry_otlp_recordable)
    endif()
    if(TARGET opentelemetry_exporter_otlp_http_client)
      add_library(opentelemetry-cpp::otlp_http_client ALIAS opentelemetry_exporter_otlp_http_client)
    endif()
    if(TARGET opentelemetry_exporter_otlp_http)
      add_library(opentelemetry-cpp::otlp_http_exporter ALIAS opentelemetry_exporter_otlp_http)
    endif()
    if(TARGET opentelemetry_exporter_otlp_http_log)
      add_library(opentelemetry-cpp::otlp_http_log_record_exporter ALIAS opentelemetry_exporter_otlp_http_log)
    endif()
    if(TARGET opentelemetry_exporter_otlp_http_metric)
      add_library(opentelemetry-cpp::otlp_http_metric_exporter ALIAS opentelemetry_exporter_otlp_http_metric)
    endif()
    if(TARGET opentelemetry_http_client_curl)
      add_library(opentelemetry-cpp::http_client_curl ALIAS opentelemetry_http_client_curl)
    endif()
  endif()
endfunction()

get_opentelemetry_cpp()
