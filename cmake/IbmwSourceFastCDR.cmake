# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set(IBMW_FASTCDR_SOURCE_URL
    "https://github.com/eProsima/Fast-CDR/archive/refs/tags/v2.2.6.tar.gz"
    CACHE STRING "Fast CDR source archive URL for IBMW source-prefix builds.")
set(IBMW_FASTCDR_SOURCE_DIR
    ""
    CACHE PATH "Optional local Fast CDR source tree for IBMW source-prefix builds.")

# Backport of upstream Fast-CDR fix: guard optional<T> copy/move constructors on
# engaged_. Without it, copying a disengaged optional<RTPSEndpointQos> (FastDDS
# SubscriptionBuiltinTopicData::endpoint) constructs the contained type from an
# uninitialized union member, reading a garbage std::vector size and aborting with
# std::length_error("vector") inside create_participant() on OHOS/musl.
set(IBMW_FASTCDR_OPTIONAL_PATCH
    "${CMAKE_CURRENT_LIST_DIR}/patches/fastcdr-2.2.6-optional-engaged-guard.patch")

ibmw_add_source_prefix_external_project(ibmw_source_fastcdr
  URL "${IBMW_FASTCDR_SOURCE_URL}"
  SOURCE_DIR "${IBMW_FASTCDR_SOURCE_DIR}"
  PATCH_COMMAND patch -p1 --forward --input "${IBMW_FASTCDR_OPTIONAL_PATCH}"
  CMAKE_ARGS
    -DBUILD_DOCUMENTATION=OFF
    -DBUILD_TESTING=OFF
    -DCOMPILE_EXAMPLES=OFF)
