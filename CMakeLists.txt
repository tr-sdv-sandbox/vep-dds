cmake_minimum_required(VERSION 3.16)
project(vdr_ecosystem VERSION 0.1.0 LANGUAGES C CXX)

# C++17 standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# Compiler warnings
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Werror=return-type
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_options(-g -O0)
else()
    add_compile_options(-O2)
endif()

# Find required packages
find_package(CycloneDDS REQUIRED)

# Google logging
find_package(glog REQUIRED)

# yaml-cpp
find_package(yaml-cpp REQUIRED)

# nlohmann_json
find_package(nlohmann_json 3.2.0 REQUIRED)

# Mosquitto (optional, for MQTT sink reference implementation)
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(MOSQUITTO libmosquitto)
endif()
if(MOSQUITTO_FOUND)
    message(STATUS "libmosquitto found - building MqttSink")
else()
    message(STATUS "libmosquitto not found - MqttSink will not be built")
endif()

# Protobuf and gRPC for OTLP bridge
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG QUIET)
if(NOT gRPC_FOUND)
    # Fallback for systems without gRPC cmake config
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GRPC REQUIRED grpc++ grpc)
    set(GRPC_LIBRARIES ${GRPC_LIBRARIES})
    set(GRPC_INCLUDE_DIRS ${GRPC_INCLUDE_DIRS})
endif()

# GoogleTest (optional, for tests)
find_package(GTest)

# libvssdag (optional, for CAN-to-VSS probe)
find_package(vssdag QUIET)
if(vssdag_FOUND)
    message(STATUS "libvssdag found - building vssdag probe")
else()
    message(STATUS "libvssdag not found - vssdag probe will not be built")
endif()

# Open1722 (optional, for IEEE 1722 AVTP CAN bridge)
find_package(Open1722 QUIET)
if(Open1722_FOUND)
    message(STATUS "Open1722 found - building AVTP probe")
else()
    message(STATUS "Open1722 not found - AVTP probe will not be built")
endif()

# IDL compilation
# Find idlc compiler
find_program(IDLC_EXECUTABLE idlc REQUIRED)

# IDL output directory
set(IDL_OUTPUT_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${IDL_OUTPUT_DIR})

# Function to compile IDL files
function(compile_idl IDL_FILE)
    get_filename_component(IDL_NAME ${IDL_FILE} NAME_WE)
    set(IDL_SOURCE ${IDL_OUTPUT_DIR}/${IDL_NAME}.c)
    set(IDL_HEADER ${IDL_OUTPUT_DIR}/${IDL_NAME}.h)

    add_custom_command(
        OUTPUT ${IDL_SOURCE} ${IDL_HEADER}
        COMMAND ${IDLC_EXECUTABLE} -l c ${IDL_FILE}
        WORKING_DIRECTORY ${IDL_OUTPUT_DIR}
        DEPENDS ${IDL_FILE}
        COMMENT "Compiling IDL: ${IDL_FILE}"
    )

    # Export variables to parent scope
    set(IDL_GENERATED_SOURCE ${IDL_SOURCE} PARENT_SCOPE)
    set(IDL_GENERATED_HEADER ${IDL_HEADER} PARENT_SCOPE)
endfunction()

# Compile our telemetry IDL
compile_idl(${CMAKE_SOURCE_DIR}/idl/telemetry.idl)

# Create IDL library
add_library(telemetry_idl STATIC
    ${IDL_OUTPUT_DIR}/telemetry.c
)

target_include_directories(telemetry_idl PUBLIC
    ${IDL_OUTPUT_DIR}
)

target_link_libraries(telemetry_idl PUBLIC
    CycloneDDS::ddsc
)

# Ensure IDL is generated before building
add_custom_target(generate_idl
    DEPENDS ${IDL_OUTPUT_DIR}/telemetry.c ${IDL_OUTPUT_DIR}/telemetry.h
)

add_dependencies(telemetry_idl generate_idl)

# Common library (DDS wrappers, utilities)
add_library(vdr_common STATIC
    src/common/dds_wrapper.cpp
    src/common/qos_profiles.cpp
    src/common/time_utils.cpp
)

target_include_directories(vdr_common PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${IDL_OUTPUT_DIR}
)

target_link_libraries(vdr_common PUBLIC
    telemetry_idl
    CycloneDDS::ddsc
    glog::glog
)

# VDR sinks library (output sink implementations)
set(VDR_SINKS_SOURCES
    src/vdr/sinks/log_sink.cpp
    src/vdr/sinks/capture_sink.cpp
)

if(MOSQUITTO_FOUND)
    list(APPEND VDR_SINKS_SOURCES src/vdr/sinks/mqtt_sink.cpp)
endif()

add_library(vdr_sinks STATIC ${VDR_SINKS_SOURCES})

target_include_directories(vdr_sinks PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${IDL_OUTPUT_DIR}
)

target_link_libraries(vdr_sinks PUBLIC
    vdr_common
    nlohmann_json::nlohmann_json
)

if(MOSQUITTO_FOUND)
    target_link_libraries(vdr_sinks PUBLIC ${MOSQUITTO_LIBRARIES})
    target_include_directories(vdr_sinks PUBLIC ${MOSQUITTO_INCLUDE_DIRS})
    target_compile_definitions(vdr_sinks PUBLIC VDR_HAS_MQTT_SINK)
endif()

# VDR core library (subscriber, shared between vdr and tests)
add_library(vdr_core STATIC
    src/vdr/subscriber.cpp
)

target_include_directories(vdr_core PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${IDL_OUTPUT_DIR}
)

target_link_libraries(vdr_core PUBLIC
    vdr_common
    vdr_sinks
)

# Testing library (test fixtures)
add_library(vdr_testing STATIC
    src/testing/test_probe.cpp
    src/testing/test_vdr.cpp
)

target_include_directories(vdr_testing PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${IDL_OUTPUT_DIR}
)

target_link_libraries(vdr_testing PUBLIC
    vdr_core
)

# VDR executable
add_executable(vdr
    src/vdr/main.cpp
)

target_include_directories(vdr PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IDL_OUTPUT_DIR}
)

target_link_libraries(vdr PRIVATE
    vdr_core
    yaml-cpp::yaml-cpp
    glog::glog
)

# Sample probes
add_executable(probe_vss
    src/probes/vss_probe/main.cpp
)

target_include_directories(probe_vss PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IDL_OUTPUT_DIR}
)

target_link_libraries(probe_vss PRIVATE
    vdr_common
    glog::glog
)

add_executable(probe_metrics
    src/probes/metrics_probe/main.cpp
)

target_include_directories(probe_metrics PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IDL_OUTPUT_DIR}
)

target_link_libraries(probe_metrics PRIVATE
    vdr_common
    glog::glog
)

add_executable(probe_events
    src/probes/event_probe/main.cpp
)

target_include_directories(probe_events PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IDL_OUTPUT_DIR}
)

target_link_libraries(probe_events PRIVATE
    vdr_common
    glog::glog
)

# OTLP Bridge probe (requires protobuf and gRPC)
# Proto output directory
set(PROTO_OUTPUT_DIR ${CMAKE_BINARY_DIR}/generated_proto)
file(MAKE_DIRECTORY ${PROTO_OUTPUT_DIR})

# Find protoc and grpc_cpp_plugin
find_program(PROTOC_EXECUTABLE protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

# Proto files to compile
set(OTLP_PROTO_FILES
    ${CMAKE_SOURCE_DIR}/proto/opentelemetry/proto/common/v1/common.proto
    ${CMAKE_SOURCE_DIR}/proto/opentelemetry/proto/resource/v1/resource.proto
    ${CMAKE_SOURCE_DIR}/proto/opentelemetry/proto/metrics/v1/metrics.proto
    ${CMAKE_SOURCE_DIR}/proto/opentelemetry/proto/logs/v1/logs.proto
    ${CMAKE_SOURCE_DIR}/proto/opentelemetry/proto/collector/metrics/v1/metrics_service.proto
    ${CMAKE_SOURCE_DIR}/proto/opentelemetry/proto/collector/logs/v1/logs_service.proto
)

# Generate protobuf and gRPC sources
set(PROTO_SRCS)
set(PROTO_HDRS)
set(GRPC_SRCS)
set(GRPC_HDRS)

foreach(PROTO_FILE ${OTLP_PROTO_FILES})
    get_filename_component(PROTO_DIR ${PROTO_FILE} DIRECTORY)
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)

    # Calculate relative path from proto root
    file(RELATIVE_PATH PROTO_REL_DIR ${CMAKE_SOURCE_DIR}/proto ${PROTO_DIR})

    # Output paths
    set(PROTO_SRC ${PROTO_OUTPUT_DIR}/${PROTO_REL_DIR}/${PROTO_NAME}.pb.cc)
    set(PROTO_HDR ${PROTO_OUTPUT_DIR}/${PROTO_REL_DIR}/${PROTO_NAME}.pb.h)

    list(APPEND PROTO_SRCS ${PROTO_SRC})
    list(APPEND PROTO_HDRS ${PROTO_HDR})

    # Check if this is a service proto (has grpc)
    if(PROTO_FILE MATCHES "service\\.proto$")
        set(GRPC_SRC ${PROTO_OUTPUT_DIR}/${PROTO_REL_DIR}/${PROTO_NAME}.grpc.pb.cc)
        set(GRPC_HDR ${PROTO_OUTPUT_DIR}/${PROTO_REL_DIR}/${PROTO_NAME}.grpc.pb.h)
        list(APPEND GRPC_SRCS ${GRPC_SRC})
        list(APPEND GRPC_HDRS ${GRPC_HDR})

        add_custom_command(
            OUTPUT ${PROTO_SRC} ${PROTO_HDR} ${GRPC_SRC} ${GRPC_HDR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_OUTPUT_DIR}/${PROTO_REL_DIR}
            COMMAND ${PROTOC_EXECUTABLE}
                --proto_path=${CMAKE_SOURCE_DIR}/proto
                --cpp_out=${PROTO_OUTPUT_DIR}
                --grpc_out=${PROTO_OUTPUT_DIR}
                --plugin=protoc-gen-grpc=${GRPC_CPP_PLUGIN}
                ${PROTO_FILE}
            DEPENDS ${PROTO_FILE}
            COMMENT "Generating protobuf and gRPC: ${PROTO_FILE}"
        )
    else()
        add_custom_command(
            OUTPUT ${PROTO_SRC} ${PROTO_HDR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_OUTPUT_DIR}/${PROTO_REL_DIR}
            COMMAND ${PROTOC_EXECUTABLE}
                --proto_path=${CMAKE_SOURCE_DIR}/proto
                --cpp_out=${PROTO_OUTPUT_DIR}
                ${PROTO_FILE}
            DEPENDS ${PROTO_FILE}
            COMMENT "Generating protobuf: ${PROTO_FILE}"
        )
    endif()
endforeach()

# OTLP protobuf library
add_library(otlp_proto STATIC
    ${PROTO_SRCS}
    ${GRPC_SRCS}
)

target_include_directories(otlp_proto PUBLIC
    ${PROTO_OUTPUT_DIR}
    ${Protobuf_INCLUDE_DIRS}
)

if(gRPC_FOUND)
    target_link_libraries(otlp_proto PUBLIC
        protobuf::libprotobuf
        gRPC::grpc++
    )
else()
    target_link_libraries(otlp_proto PUBLIC
        ${Protobuf_LIBRARIES}
        ${GRPC_LIBRARIES}
    )
    target_include_directories(otlp_proto PUBLIC ${GRPC_INCLUDE_DIRS})
endif()

# OTLP-to-DDS Bridge
add_executable(probe_otel_bridge
    src/probes/otel_bridge/main.cpp
)

target_include_directories(probe_otel_bridge PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${IDL_OUTPUT_DIR}
    ${PROTO_OUTPUT_DIR}
)

target_link_libraries(probe_otel_bridge PRIVATE
    vdr_common
    otlp_proto
    glog::glog
)

# VSS DAG Probe (if libvssdag found)
if(vssdag_FOUND)
    add_executable(probe_vssdag
        src/probes/vssdag_probe/main.cpp
    )

    target_include_directories(probe_vssdag PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${IDL_OUTPUT_DIR}
    )

    target_link_libraries(probe_vssdag PRIVATE
        vdr_common
        vss::dag
        yaml-cpp::yaml-cpp
        glog::glog
    )
endif()

# AVTP Probe (if Open1722 found)
# IEEE 1722 AVTP CAN bridge for MCU <-> HPC communication
if(Open1722_FOUND)
    add_executable(probe_avtp
        src/probes/avtp_probe/main.cpp
    )

    target_include_directories(probe_avtp PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${IDL_OUTPUT_DIR}
    )

    target_link_libraries(probe_avtp PRIVATE
        vdr_common
        open1722
        glog::glog
    )
endif()

# Tests (if GTest found)
if(GTest_FOUND)
    enable_testing()

    add_executable(test_dds_wrapper
        tests/test_dds_wrapper.cpp
    )

    target_include_directories(test_dds_wrapper PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${IDL_OUTPUT_DIR}
    )

    target_link_libraries(test_dds_wrapper PRIVATE
        vdr_common
        GTest::gtest
        GTest::gtest_main
    )

    add_test(NAME test_dds_wrapper COMMAND test_dds_wrapper)

    # Integration tests
    add_executable(test_integration
        tests/test_integration.cpp
    )

    target_include_directories(test_integration PRIVATE
        ${CMAKE_SOURCE_DIR}/src
        ${IDL_OUTPUT_DIR}
    )

    target_link_libraries(test_integration PRIVATE
        vdr_common
        vdr_sinks
        vdr_testing
        GTest::gtest
    )

    add_test(NAME test_integration COMMAND test_integration)
endif()

# Installation
install(TARGETS vdr probe_vss probe_metrics probe_events probe_otel_bridge
    RUNTIME DESTINATION bin
)

install(FILES config/vdr_config.yaml
    DESTINATION etc/vdr
)

# Summary
message(STATUS "")
message(STATUS "=== VDR Ecosystem Build Configuration ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Cyclone DDS: ${CycloneDDS_VERSION}")
message(STATUS "IDL compiler: ${IDLC_EXECUTABLE}")
message(STATUS "IDL output: ${IDL_OUTPUT_DIR}")
message(STATUS "Optional dependencies:")
message(STATUS "  libvssdag: ${vssdag_FOUND}")
message(STATUS "  Open1722: ${Open1722_FOUND}")
message(STATUS "  libmosquitto: ${MOSQUITTO_FOUND}")
message(STATUS "  GTest: ${GTest_FOUND}")
message(STATUS "")
