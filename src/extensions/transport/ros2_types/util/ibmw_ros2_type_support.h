// Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
// All rights reserved.

#pragma once

#include <memory>
#include <string>

#if __has_include(<fastdds/config.hpp>)
#include <fastdds/config.hpp>
#define IBMW_ROS2_HAS_MODERN_FASTDDS 1
#else
#define IBMW_ROS2_HAS_MODERN_FASTDDS 0
#endif

// FastDDS 2.x / legacy FastRTPS still needs MEMBER_ID_INVALID defined before
// TypeSupport.hpp pulls in DynamicData.h. FastDDS 3.x has a real constexpr of
// the same name, so avoid defining the macro there.
#if !IBMW_ROS2_HAS_MODERN_FASTDDS
#ifndef MEMBER_ID_INVALID
#define MEMBER_ID_INVALID 0X0FFFFFFF
#define IBMW_ROS2_MEMBER_ID_INVALID_DEFINED 1
#endif
#endif

#include <fastdds/dds/topic/TypeSupport.hpp>

#include "c/util/type_support_base.h"
#include "cpp/util/string.h"

// Forward declaration — full definition is in ros2_type_support_factory.h.
// GetIbmwTypeSupport needs this, but including the factory header would
// create a circular include (factory → ros2_type_support → ibmw_ros2_type_support → factory).
namespace ibmw::ros2 {
class ROS2TypeSupportFactory;
}

namespace ibmw::ros2 {

/**
 * @brief Create an ibmw TypeSupport for a ROS2 message type
 *
 * @tparam ROS2MsgType ROS2 message type
 * @param ros2_type_name ROS2 type name (e.g., "std_msgs/msg/String")
 * @return const ibmw_type_support_base_t* ibmw TypeSupport pointer
 *
 * NOTE: This function is declared here but its body requires the full
 * ROS2TypeSupportFactory definition. The implementation is deferred below
 * with a forward-declared factory, and the actual body uses the factory.
 * The template is instantiated only from translation units that include
 * ros2_type_support_factory.h (which defines the factory class).
 */
template <typename ROS2MsgType>
const ibmw_type_support_base_t* GetIbmwTypeSupport(const std::string& ros2_type_name);

}  // namespace ibmw::ros2

#ifdef IBMW_ROS2_MEMBER_ID_INVALID_DEFINED
#undef MEMBER_ID_INVALID
#undef IBMW_ROS2_MEMBER_ID_INVALID_DEFINED
#endif
