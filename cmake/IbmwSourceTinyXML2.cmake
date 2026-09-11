# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set(IBMW_TINYXML2_SOURCE_URL
    "https://github.com/leethomason/tinyxml2/archive/refs/tags/10.0.0.tar.gz"
    CACHE STRING "TinyXML2 source archive URL for IBMW source-prefix builds.")
set(IBMW_TINYXML2_SOURCE_URL_HASH
    "SHA256=3bdf15128ba16686e69bce256cc468e76c7b94ff2c7f391cc5ec09e40bff3839"
    CACHE STRING "TinyXML2 source archive hash for IBMW source-prefix builds.")
set(IBMW_TINYXML2_SOURCE_DIR
    ""
    CACHE PATH "Optional local TinyXML2 source tree for IBMW source-prefix builds.")

ibmw_add_source_prefix_external_project(ibmw_source_tinyxml2
  URL "${IBMW_TINYXML2_SOURCE_URL}"
  URL_HASH "${IBMW_TINYXML2_SOURCE_URL_HASH}"
  SOURCE_DIR "${IBMW_TINYXML2_SOURCE_DIR}"
  CMAKE_ARGS
    -Dtinyxml2_BUILD_TESTING=OFF)
