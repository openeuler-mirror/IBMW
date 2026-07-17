# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set(IBMW_TINYXML2_SOURCE_URL
    "https://github.com/leethomason/tinyxml2/archive/refs/tags/10.0.0.tar.gz"
    CACHE STRING "TinyXML2 source archive URL for IBMW source-prefix builds.")
set(IBMW_TINYXML2_SOURCE_DIR
    ""
    CACHE PATH "Optional local TinyXML2 source tree for IBMW source-prefix builds.")

ibmw_add_source_prefix_external_project(ibmw_source_tinyxml2
  URL "${IBMW_TINYXML2_SOURCE_URL}"
  SOURCE_DIR "${IBMW_TINYXML2_SOURCE_DIR}"
  CMAKE_ARGS
    -Dtinyxml2_BUILD_TESTING=OFF)
