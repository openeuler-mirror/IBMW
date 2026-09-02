# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

if(IBMW_DDS_PROVIDER STREQUAL "SOURCE_FASTDDS2")
  set(_ibmw_fastcdr_source_url
      "https://codeload.github.com/eProsima/Fast-CDR/tar.gz/959ff6cc637c940d3deca3d8398920242120daee")
  set(_ibmw_fastcdr_source_url_hash
      "SHA256=c79a9b68df7abe9fed7f897f8c9f5168e46a1835b8c75a9c8ee309784df90366")
else()
  set(_ibmw_fastcdr_source_url
      "https://github.com/eProsima/Fast-CDR/archive/refs/tags/v2.3.0.tar.gz")
  set(_ibmw_fastcdr_source_url_hash
      "SHA256=d85ee9e24e105581b49de3e2b6b2585335a8dc98c4cabd288232fffc4b9b6a24")
endif()

set(IBMW_FASTCDR_SOURCE_URL
    "${_ibmw_fastcdr_source_url}"
    CACHE STRING "Fast CDR source archive URL for IBMW source-prefix builds.")
set(IBMW_FASTCDR_SOURCE_URL_HASH
    "${_ibmw_fastcdr_source_url_hash}"
    CACHE STRING "Fast CDR source archive hash for IBMW source-prefix builds.")
set(IBMW_FASTCDR_SOURCE_DIR
    ""
    CACHE PATH "Optional local Fast CDR source tree for IBMW source-prefix builds.")

ibmw_add_source_prefix_external_project(ibmw_source_fastcdr
  URL "${IBMW_FASTCDR_SOURCE_URL}"
  URL_HASH "${IBMW_FASTCDR_SOURCE_URL_HASH}"
  SOURCE_DIR "${IBMW_FASTCDR_SOURCE_DIR}"
  CMAKE_ARGS
    -DBUILD_DOCUMENTATION=OFF
    -DBUILD_TESTING=OFF
    -DCOMPILE_EXAMPLES=OFF)
