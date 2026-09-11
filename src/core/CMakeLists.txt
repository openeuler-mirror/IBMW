# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

set_namespace()

add_subdirectory(extension_api)
add_subdirectory(engine)
# The host CLI is not part of the target runtime. Its reporter host ABI link
# uses a generator expression unsupported by the OHOS cross-linker.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "OHOS")
  add_subdirectory(main)
endif()
