# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

include(FetchContent)

message(STATUS "get opentelemetry_proto ...")

set(opentelemetry_proto_DOWNLOAD_URL
    "https://github.com/open-telemetry/opentelemetry-proto/archive/v1.3.2.tar.gz"
    CACHE STRING "")
set(opentelemetry_proto_DOWNLOAD_SHA256
    "c069c0d96137cf005d34411fa67dd3b6f1f8c64af1e7fb2fe0089a41c425acd7"
    CACHE STRING "SHA256 of opentelemetry_proto_DOWNLOAD_URL (update both together)")

if(opentelemetry_proto_LOCAL_SOURCE)
  FetchContent_Declare(opentelemetry_proto SOURCE_DIR ${opentelemetry_proto_LOCAL_SOURCE})
else()
  FetchContent_Declare(
    opentelemetry_proto
    URL ${opentelemetry_proto_DOWNLOAD_URL}
    URL_HASH SHA256=${opentelemetry_proto_DOWNLOAD_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
endif()

FetchContent_GetProperties(opentelemetry_proto)
if(NOT opentelemetry_proto_POPULATED)
  FetchContent_Populate(opentelemetry_proto)
endif()
