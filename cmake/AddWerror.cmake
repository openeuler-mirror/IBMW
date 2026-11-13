# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

macro(add_werror target)
  if(IBMW_BUILD_WITH_WERROR)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
      target_compile_options(
        ${target}
        PRIVATE -Wall
                -Wextra
                -Wno-unused-parameter
                -Wno-missing-field-initializers
                -Wno-implicit-fallthrough
                -Werror)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      if(CMAKE_CROSSCOMPILING AND DEFINED OHOS_PLATFORM AND NOT "${OHOS_PLATFORM}" STREQUAL "")
        target_compile_options(
          ${target}
          PRIVATE -Wall
                  -Wextra
                  -Wno-unused-parameter
                  -Wno-missing-field-initializers
                  -Wno-implicit-fallthrough
                  -Wno-unused-command-line-argument
                  -Werror)
      else()
        target_compile_options(
          ${target}
          PRIVATE -Wall
                  -Wextra
                  -Wno-unused-parameter
                  -Wno-missing-field-initializers
                  -Wno-implicit-fallthrough
                  -Werror)
      endif()
    endif()
  endif()
endmacro()
