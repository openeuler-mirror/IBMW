# Copyright (c) 2025-2026, IB-Robot Group & openEuler Embedded SIG & openharmony-robot sig_RoboFrame.
# The complete entry fails configuration for missing backends instead of skipping them.
foreach(feature DDS ROS2 MQTT ZENOH GRPC ICEORYX)
  if(NOT IBMW_BUILD_${feature}_EXTENSION)
    message(FATAL_ERROR "Complete functional regression requires IBMW_BUILD_${feature}_EXTENSION=ON")
  endif()
endforeach()

add_test(NAME ibmw_diag_installed_test
  COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_SOURCE_DIR}/installed.py"
    --build "${CMAKE_BINARY_DIR}" --config "$<CONFIG>"
    --artifacts "${CMAKE_CURRENT_BINARY_DIR}/results/$<CONFIG>/installed")
set_tests_properties(ibmw_diag_installed_test PROPERTIES
  LABELS "ibmw_diag_functional;ibmw_diag_full;integration;installed" TIMEOUT 2400 RUN_SERIAL TRUE)
if(NOT IBMW_BUILD_PROTOCOLS OR NOT IBMW_INSTALL)
  message(FATAL_ERROR "Complete functional regression requires IBMW_BUILD_PROTOCOLS=ON and IBMW_INSTALL=ON")
endif()
find_program(IBMW_DIAG_MOSQUITTO mosquitto REQUIRED)
execute_process(COMMAND ${Python3_EXECUTABLE} -c "import rclpy; from std_msgs.msg import Int32; from example_interfaces.srv import AddTwoInts"
  RESULT_VARIABLE ros_python_status ERROR_VARIABLE ros_python_error)
if(NOT ros_python_status EQUAL 0)
  message(FATAL_ERROR "Complete functional regression needs a ROS-sourced Python: ${ros_python_error}")
endif()
find_package(std_msgs REQUIRED)
find_package(example_interfaces REQUIRED)
add_library(ibmw_diag_ros_types SHARED ../fixtures/ros_types.cc)
target_link_libraries(ibmw_diag_ros_types PRIVATE ibmw::api::ros2_type_support
  std_msgs::std_msgs__rosidl_generator_cpp
  std_msgs::std_msgs__rosidl_typesupport_fastrtps_cpp
  std_msgs::std_msgs__rosidl_typesupport_introspection_cpp
  example_interfaces::example_interfaces__rosidl_generator_cpp
  example_interfaces::example_interfaces__rosidl_typesupport_fastrtps_cpp
  example_interfaces::example_interfaces__rosidl_typesupport_introspection_cpp)
add_werror(ibmw_diag_ros_types)
list(APPEND functional_dependencies ibmw_diag_ros_types ibmw_schema_common_ts
  ibmw_dds_extension ibmw_extensions_ros2_ext ibmw_extensions_mqtt_ext
  ibmw_extensions_zenoh_ext ibmw_extensions_grpc_ext ibmw_extensions_iceoryx_ext
  ibmw_discovery_dds ibmw_discovery_ros2 ibmw_discovery_zenoh iox-roudi)
# cmake --install exports the SDK as well as the CLI. Build its installable
# artifacts, including fetched dependencies that are not direct CLI link inputs.
foreach(target fmt tbbmalloc iceoryx_posh_gateway iceoryx_binding_c
    paho-mqtt3c-static paho-mqtt3a-static paho-mqtt3cs-static)
  if(TARGET ${target})
    list(APPEND functional_dependencies ${target})
  endif()
endforeach()
list(APPEND functional_dependencies ibmw_schema_geometry_idl_gencode ibmw_schema_sensor_idl_gencode
  ibmw_schema_actuator_idl_gencode_codegen ibmw_msgs ros2_ext_proto
  ibmw_msgs__py ros2_ext_proto__py ibmw_schema_ibmw_msgs_ts ibmw_schema_ros2_ext_proto_ts
  ibmw_schema_ros2_ext_proto_ibmw_rpc_gencode dds_ext_proto_codegen)
foreach(package ibmw_msgs ros2_ext_proto)
  list(APPEND functional_dependencies ament_cmake_python_build_${package}_egg
    ${package}__rosidl_generator_py ${package}__rosidl_typesupport_c__pyext
    ${package}__rosidl_typesupport_fastrtps_c__pyext ${package}__rosidl_typesupport_introspection_c__pyext)
endforeach()
add_dependencies(ibmw_diag_functional ${functional_dependencies})

set(control_args --cli "$<TARGET_FILE:ibmw_core_main>"
  --server "$<TARGET_FILE:ibmw_diag_runtime_fixture>"
  --lifecycle-server "$<TARGET_FILE:ibmw_diag_lifecycle_fixture>"
  --types "$<TARGET_FILE:ibmw_diag_functional_types>"
  --net "$<TARGET_FILE:ibmw_extensions_net_ext>"
  --parameter "$<TARGET_FILE:ibmw_extensions_parameter_ext>"
  --log "$<TARGET_FILE:ibmw_extensions_log_control_ext>"
  --dds "$<TARGET_FILE:ibmw_dds_extension>"
  --grpc "$<TARGET_FILE:ibmw_extensions_grpc_ext>"
  --mqtt "$<TARGET_FILE:ibmw_extensions_mqtt_ext>"
  --zenoh "$<TARGET_FILE:ibmw_extensions_zenoh_ext>" --mosquitto "${IBMW_DIAG_MOSQUITTO}")
foreach(backend dds grpc mqtt zenoh)
  foreach(component param log doctor)
    add_test(NAME ibmw_diag_${component}_${backend}_functional_test
      COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_SOURCE_DIR}/scenarios.py"
        --case ${component} --rpc-backend ${backend} ${control_args}
        --artifacts "${CMAKE_CURRENT_BINARY_DIR}/results/$<CONFIG>/${component}-${backend}")
    set_tests_properties(ibmw_diag_${component}_${backend}_functional_test PROPERTIES
      LABELS "ibmw_diag_functional;ibmw_diag_full;integration" TIMEOUT 240 RUN_SERIAL TRUE)
  endforeach()
endforeach()

set(network_args --cli "$<TARGET_FILE:ibmw_core_main>"
  --server "$<TARGET_FILE:ibmw_diag_runtime_fixture>"
  --parameter "$<TARGET_FILE:ibmw_extensions_parameter_ext>"
  --typed-server "$<TARGET_FILE:ibmw_diag_typed_fixture>"
  --types "$<TARGET_FILE:ibmw_diag_functional_types>"
  --dds-types "$<TARGET_FILE:ibmw_schema_common_ts>"
  --ros-types "$<TARGET_FILE:ibmw_diag_ros_types>"
  --native-ros "${CMAKE_CURRENT_SOURCE_DIR}/../fixtures/native_ros.py"
  --net "$<TARGET_FILE:ibmw_extensions_net_ext>"
  --dds "$<TARGET_FILE:ibmw_dds_extension>"
  --ros2 "$<TARGET_FILE:ibmw_extensions_ros2_ext>"
  --grpc "$<TARGET_FILE:ibmw_extensions_grpc_ext>"
  --mqtt "$<TARGET_FILE:ibmw_extensions_mqtt_ext>"
  --zenoh "$<TARGET_FILE:ibmw_extensions_zenoh_ext>"
  --iceoryx "$<TARGET_FILE:ibmw_extensions_iceoryx_ext>"
  --log "$<TARGET_FILE:ibmw_extensions_log_control_ext>"
  --mosquitto "${IBMW_DIAG_MOSQUITTO}" --roudi "$<TARGET_FILE:iox-roudi>")
foreach(component topic service qos)
  if(component STREQUAL "topic")
    set(backends dds ros2 mqtt zenoh iceoryx)
  elseif(component STREQUAL "service")
    set(backends dds grpc mqtt zenoh ros2 dds_ros2)
  else()
    set(backends dds ros2)
  endif()
  foreach(backend IN LISTS backends)
    add_test(NAME ibmw_diag_${component}_${backend}_network_test
      COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_SOURCE_DIR}/network.py"
        --case ${component} --backend ${backend} ${network_args}
        --artifacts "${CMAKE_CURRENT_BINARY_DIR}/results/$<CONFIG>/${component}-${backend}")
    set_tests_properties(ibmw_diag_${component}_${backend}_network_test PROPERTIES
      LABELS "ibmw_diag_functional;ibmw_diag_full;integration" TIMEOUT 180 RUN_SERIAL TRUE)
  endforeach()
endforeach()

find_program(IBMW_DIAG_OPENSSL openssl REQUIRED)
foreach(tls server mutual)
  foreach(component topic service)
    add_test(NAME ibmw_diag_${component}_mqtt_${tls}_network_test
      COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_SOURCE_DIR}/network.py"
        --case ${component} --backend mqtt --mqtt-tls ${tls} ${network_args}
        --artifacts "${CMAKE_CURRENT_BINARY_DIR}/results/$<CONFIG>/${component}-mqtt-${tls}")
    set_tests_properties(ibmw_diag_${component}_mqtt_${tls}_network_test PROPERTIES
      LABELS "ibmw_diag_functional;ibmw_diag_full;integration" TIMEOUT 180 RUN_SERIAL TRUE)
  endforeach()
  foreach(component param log doctor)
    add_test(NAME ibmw_diag_${component}_mqtt_${tls}_functional_test
      COMMAND ${Python3_EXECUTABLE} -B "${CMAKE_CURRENT_SOURCE_DIR}/scenarios.py"
        --case ${component} --rpc-backend mqtt --mqtt-tls ${tls} ${control_args}
        --artifacts "${CMAKE_CURRENT_BINARY_DIR}/results/$<CONFIG>/${component}-mqtt-${tls}")
    set_tests_properties(ibmw_diag_${component}_mqtt_${tls}_functional_test PROPERTIES
      LABELS "ibmw_diag_functional;ibmw_diag_full;integration" TIMEOUT 240 RUN_SERIAL TRUE)
  endforeach()
endforeach()
