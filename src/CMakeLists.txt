# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set_root_namespace("ibmw")

add_subdirectory(base)
add_subdirectory(internal)
add_subdirectory(api)

if(IBMW_BUILD_RUNTIME)
  add_subdirectory(core)
endif()

add_subdirectory(tools)

if(IBMW_BUILD_RUNTIME)
  add_subdirectory(extensions)
endif()

add_subdirectory(schema)

if(IBMW_BUILD_EXAMPLES OR IBMW_BUILD_ROS2_ACTION_FIBONACCI_SAMPLES OR
   IBMW_BUILD_RETAINED_LOAN_E2E)
  add_subdirectory(samples)
endif()
