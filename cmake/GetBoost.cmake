# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

include(FetchContent)

message(STATUS "get boost...")

if(TARGET Boost::asio AND TARGET Boost::beast)
  return()
endif()

option(
  IBMW_USE_SYSTEM_BOOST
  "Prefer an installed Boost for the header-only Asio and Beast components."
  ON)

if(IBMW_USE_SYSTEM_BOOST)
  find_package(Boost 1.74 CONFIG QUIET)
  if(Boost_FOUND)
    if(TARGET Boost::headers)
      set(_ibmw_boost_headers_target Boost::headers)
    elseif(TARGET Boost::boost)
      set(_ibmw_boost_headers_target Boost::boost)
    endif()

    if(_ibmw_boost_headers_target)
      add_library(ibmw_boost_asio INTERFACE)
      add_library(Boost::asio ALIAS ibmw_boost_asio)
      target_link_libraries(
        ibmw_boost_asio
        INTERFACE ${_ibmw_boost_headers_target})
      if(TARGET Boost::system)
        target_link_libraries(ibmw_boost_asio INTERFACE Boost::system)
      endif()

      add_library(ibmw_boost_beast INTERFACE)
      add_library(Boost::beast ALIAS ibmw_boost_beast)
      target_link_libraries(ibmw_boost_beast INTERFACE Boost::asio)

      message(STATUS "Found system Boost: ${Boost_VERSION_STRING}")
      return()
    endif()
  endif()
endif()

set(boost_DOWNLOAD_URL
    "https://github.com/boostorg/boost/releases/download/boost-1.82.0/boost-1.82.0.tar.xz"
    CACHE STRING "")
set(boost_DOWNLOAD_SHA256
    "fd60da30be908eff945735ac7d4d9addc7f7725b1ff6fcdcaede5262d511d21e"
    CACHE STRING "SHA256 of boost_DOWNLOAD_URL (update both together)")

if(boost_LOCAL_SOURCE)
  FetchContent_Declare(
    boost
    SOURCE_DIR ${boost_LOCAL_SOURCE}
    OVERRIDE_FIND_PACKAGE)
else()
  FetchContent_Declare(
    boost
    URL ${boost_DOWNLOAD_URL}
    URL_HASH SHA256=${boost_DOWNLOAD_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    OVERRIDE_FIND_PACKAGE)
endif()

# Wrap it in a function to restrict the scope of the variables
function(get_boost)
  FetchContent_GetProperties(boost)
  if(NOT boost_POPULATED)
    set(BOOST_INCLUDE_LIBRARIES asio beast)

    set(Boost_USE_STATIC_LIBS ON)

    FetchContent_MakeAvailable(boost)
  endif()
endfunction()

get_boost()
