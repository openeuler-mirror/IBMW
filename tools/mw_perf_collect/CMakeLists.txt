option(IBMW_BUILD_OHOS_CAPTURE_AGENT
  "Build the native OHOS remote-managed capture candidate." OFF)

if(IBMW_BUILD_OHOS_CAPTURE_AGENT AND (CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
                                      CMAKE_SYSTEM_NAME STREQUAL "OHOS"))
  add_subdirectory(native)
endif()

if(NOT IBMW_INSTALL)
  return()
endif()

set(_mw_perf_package_parent
    "${CMAKE_INSTALL_LIBEXECDIR}/${PROJECT_NAME}")
file(RELATIVE_PATH _mw_perf_package_relative
     "/${CMAKE_INSTALL_BINDIR}"
     "/${_mw_perf_package_parent}")

file(CONFIGURE
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mw-perf-collect"
  CONTENT [=[#!/usr/bin/env python3
import sys
from pathlib import Path

package_parent = (Path(__file__).resolve().parent / "@_mw_perf_package_relative@").resolve()
# Never allow the checkout's tools package (for example via cwd or an inherited
# test runner path) to shadow the package selected by this installed launcher.
package_name = "mw_perf_collect"
sys.path[:] = [entry for entry in sys.path
               if (Path(entry or ".").resolve() == package_parent
                   or not (Path(entry or ".").resolve() / package_name / "core.py").is_file())]
sys.path.insert(0, str(package_parent))

from mw_perf_collect.cli import main

raise SystemExit(main())
]=]
  @ONLY)

install(
  PROGRAMS "${CMAKE_CURRENT_BINARY_DIR}/mw-perf-collect"
  DESTINATION "${CMAKE_INSTALL_BINDIR}"
  COMPONENT mw_perf_collect)

install(
  FILES
    __init__.py
    __main__.py
    campaign.py
    cli.py
    core.py
    dds_trace.py
    exact_trace.py
    fault_evaluation.py
    runtime_fault_evaluation.py
    rollback_evaluation.py
    json_strict.py
    formal_anchor.py
    metric_qualification.py
    ohos_capture.py
  DESTINATION "${_mw_perf_package_parent}/mw_perf_collect"
  COMPONENT mw_perf_collect)

install(
  FILES
    adapters/__init__.py
    adapters/ibmw.py
    adapters/csv_latency.py
    adapters/ros2.py
  DESTINATION "${_mw_perf_package_parent}/mw_perf_collect/adapters"
  COMPONENT mw_perf_collect)

install(
  DIRECTORY schemas/
  DESTINATION "${_mw_perf_package_parent}/mw_perf_collect/schemas"
  COMPONENT mw_perf_collect
  FILES_MATCHING PATTERN "*.json")

install(
  DIRECTORY contracts/
  DESTINATION "${_mw_perf_package_parent}/mw_perf_collect/contracts"
  COMPONENT mw_perf_collect
  FILES_MATCHING PATTERN "*.json")
