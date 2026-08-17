# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# All rights reserved.

if(NOT DEFINED INPUT_FILE OR NOT EXISTS "${INPUT_FILE}")
  message(FATAL_ERROR "Generated RPC source does not exist: ${INPUT_FILE}")
endif()

file(READ "${INPUT_FILE}" _source)

function(count_literal_occurrences haystack needle output_var)
  set(_count 0)
  set(_remaining "${haystack}")
  string(LENGTH "${needle}" _needle_length)
  while(1)
    string(FIND "${_remaining}" "${needle}" _position)
    if(_position EQUAL -1)
      break()
    endif()
    math(EXPR _next_position "${_position} + ${_needle_length}")
    string(SUBSTRING "${_remaining}" ${_next_position} -1 _remaining)
    math(EXPR _count "${_count} + 1")
  endwhile()
  set(${output_var} ${_count} PARENT_SCOPE)
endfunction()

set(_rclcpp_include "#include \"rclcpp/rclcpp.hpp\"")
set(_serialization_include "#include \"rclcpp/serialization.hpp\"")
set(_service_type_support
  "rosidl_typesupport_cpp::get_service_type_support_handle<slam_toolbox::srv::SerializePoseGraph>()")
set(_constants_anchor
  "static constexpr std::string_view kRpcType = \"ros2\";\nstatic constexpr std::string_view kServiceName = \"slam_toolbox/srv\";\nstatic constexpr std::string_view kFuncName = \"SerializePoseGraph\";")
set(_constants_replacement
  "${_constants_anchor}\nstatic constexpr char kRos2ServiceName[] = \"slam_toolbox/srv/SerializePoseGraph\";")

string(FIND "${_source}" "${_rclcpp_include}\n" _rclcpp_position)
string(FIND "${_source}" "${_serialization_include}\n" _serialization_position)
if(_rclcpp_position EQUAL -1 OR _serialization_position EQUAL -1)
  message(FATAL_ERROR
    "Generated RPC source no longer contains both expected rclcpp include lines")
endif()

count_literal_occurrences("${_source}" "${_service_type_support}" _type_support_count)
if(NOT _type_support_count EQUAL 4)
  message(FATAL_ERROR
    "Expected exactly four generated ROS service type support expressions, found ${_type_support_count}")
endif()

count_literal_occurrences("${_source}" "${_constants_anchor}" _constants_anchor_count)
if(NOT _constants_anchor_count EQUAL 1)
  message(FATAL_ERROR
    "Expected exactly one unmodified generated RPC constants anchor, found ${_constants_anchor_count}")
endif()

string(FIND "${_source}" "kRos2ServiceName" _ros2_service_name_position)
if(NOT _ros2_service_name_position EQUAL -1)
  message(FATAL_ERROR
    "Generated RPC source already contains kRos2ServiceName")
endif()

string(REPLACE "${_rclcpp_include}\n" "" _source "${_source}")
string(REPLACE "${_serialization_include}\n" "" _source "${_source}")
# Adapt generated code to IBMW DDS RPC's custom pointer contract; this is not a global generator change.
string(REPLACE "${_constants_anchor}" "${_constants_replacement}" _source "${_source}")
string(REPLACE "${_service_type_support}" "kRos2ServiceName" _source "${_source}")
file(WRITE "${INPUT_FILE}" "${_source}")
