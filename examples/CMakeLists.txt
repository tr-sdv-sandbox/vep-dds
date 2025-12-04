# Examples CMakeLists.txt
# This builds example probes using example IDL
# The IDL and IDL-dependent libraries here are for demonstration only -
# not exported as part of vdr-light.

# VEP_DDS_ROOT points to vep-dds directory (works when built as subdirectory)
set(VEP_DDS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

# ============================================================================
# Dependencies for examples
# ============================================================================

find_package(yaml-cpp REQUIRED)
find_package(nlohmann_json 3.2.0 REQUIRED)
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG QUIET)
if(NOT gRPC_FOUND)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GRPC REQUIRED grpc++ grpc)
endif()

# Optional dependencies
find_package(PkgConfig)
if(PkgConfig_FOUND)
    pkg_check_modules(MOSQUITTO libmosquitto)
endif()

# Check if targets already exist (when built from top-level CMake)
if(NOT TARGET vss::dag)
    find_package(vssdag QUIET)
endif()
if(NOT TARGET open1722)
    find_package(Open1722 QUIET)
endif()
if(NOT TARGET GTest::gtest)
    find_package(GTest QUIET)
endif()

# ============================================================================
# IDL compilation for examples
# ============================================================================
# telemetry.idl includes vss_signal.idl from libvss-types
# We need to set the include path for the IDL compiler

find_program(IDLC_EXECUTABLE idlc REQUIRED)

# Find libvss-types IDL directory
set(VSS_TYPES_IDL_DIR "${VEP_DDS_ROOT}/../libvss-types/idl")
if(NOT EXISTS "${VSS_TYPES_IDL_DIR}/vss_types.idl")
    message(FATAL_ERROR "libvss-types IDL not found at ${VSS_TYPES_IDL_DIR}")
endif()

set(EXAMPLE_IDL_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${EXAMPLE_IDL_OUTPUT_DIR})

# Source IDL files
set(EXAMPLE_TELEMETRY_IDL ${CMAKE_CURRENT_SOURCE_DIR}/idl/telemetry.idl)
set(VSS_TYPES_IDL ${VSS_TYPES_IDL_DIR}/vss_types.idl)
set(VSS_SIGNAL_IDL ${VSS_TYPES_IDL_DIR}/vss_signal.idl)

# Generated files - we compile all IDL files together
set(EXAMPLE_IDL_SRCS
    ${EXAMPLE_IDL_OUTPUT_DIR}/telemetry.c
    ${EXAMPLE_IDL_OUTPUT_DIR}/vss_types.c
    ${EXAMPLE_IDL_OUTPUT_DIR}/vss_signal.c
)
set(EXAMPLE_IDL_HDRS
    ${EXAMPLE_IDL_OUTPUT_DIR}/telemetry.h
    ${EXAMPLE_IDL_OUTPUT_DIR}/vss_types.h
    ${EXAMPLE_IDL_OUTPUT_DIR}/vss_signal.h
)

# Compile all IDL files with include paths
add_custom_command(
    OUTPUT ${EXAMPLE_IDL_SRCS} ${EXAMPLE_IDL_HDRS}
    COMMAND ${IDLC_EXECUTABLE} -l c -I ${VSS_TYPES_IDL_DIR} -I ${CMAKE_CURRENT_SOURCE_DIR}/idl ${VSS_TYPES_IDL}
    COMMAND ${IDLC_EXECUTABLE} -l c -I ${VSS_TYPES_IDL_DIR} -I ${CMAKE_CURRENT_SOURCE_DIR}/idl ${VSS_SIGNAL_IDL}
    COMMAND ${IDLC_EXECUTABLE} -l c -I ${VSS_TYPES_IDL_DIR} -I ${CMAKE_CURRENT_SOURCE_DIR}/idl ${EXAMPLE_TELEMETRY_IDL}
    WORKING_DIRECTORY ${EXAMPLE_IDL_OUTPUT_DIR}
    DEPENDS ${EXAMPLE_TELEMETRY_IDL} ${VSS_TYPES_IDL} ${VSS_SIGNAL_IDL}
    COMMENT "Compiling telemetry.idl with libvss-types"
)

add_library(example_telemetry_idl STATIC ${EXAMPLE_IDL_SRCS})
set_target_properties(example_telemetry_idl PROPERTIES LINKER_LANGUAGE C)
target_include_directories(example_telemetry_idl PUBLIC ${EXAMPLE_IDL_OUTPUT_DIR})
target_link_libraries(example_telemetry_idl PUBLIC CycloneDDS::ddsc)

add_custom_target(generate_example_idl DEPENDS ${EXAMPLE_IDL_SRCS} ${EXAMPLE_IDL_HDRS})
add_dependencies(example_telemetry_idl generate_example_idl)

# ============================================================================
# Example Libraries (IDL-dependent)
# ============================================================================

# VDR sinks library (output sink implementations)
set(VDR_SINKS_SOURCES
    vdr/sinks/log_sink.cpp
    vdr/sinks/capture_sink.cpp
)

if(MOSQUITTO_FOUND)
    list(APPEND VDR_SINKS_SOURCES vdr/sinks/mqtt_sink.cpp)
endif()

add_library(example_vdr_sinks STATIC ${VDR_SINKS_SOURCES})

target_include_directories(example_vdr_sinks PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VEP_DDS_ROOT}/src
)

target_link_libraries(example_vdr_sinks PUBLIC
    vdr_common
    example_telemetry_idl
    nlohmann_json::nlohmann_json
)

if(MOSQUITTO_FOUND)
    target_link_libraries(example_vdr_sinks PUBLIC ${MOSQUITTO_LIBRARIES})
    target_include_directories(example_vdr_sinks PUBLIC ${MOSQUITTO_INCLUDE_DIRS})
    target_compile_definitions(example_vdr_sinks PUBLIC VDR_HAS_MQTT_SINK)
endif()

# VDR core library (subscriber)
add_library(example_vdr_core STATIC
    vdr/subscriber.cpp
)

target_include_directories(example_vdr_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VEP_DDS_ROOT}/src
)

target_link_libraries(example_vdr_core PUBLIC
    vdr_common
    example_vdr_sinks
)

# Testing library (test fixtures)
add_library(example_vdr_testing STATIC
    testing/test_probe.cpp
    testing/test_vdr.cpp
)

target_include_directories(example_vdr_testing PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${VEP_DDS_ROOT}/src
)

target_link_libraries(example_vdr_testing PUBLIC
    example_vdr_core
)

# ============================================================================
# OTLP Proto Library (for OTEL bridge)
# ============================================================================

set(PROTO_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated_proto)
file(MAKE_DIRECTORY ${PROTO_OUTPUT_DIR})

find_program(PROTOC_EXECUTABLE protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)

set(OTLP_PROTO_FILES
    ${VEP_DDS_ROOT}/proto/opentelemetry/proto/common/v1/common.proto
    ${VEP_DDS_ROOT}/proto/opentelemetry/proto/resource/v1/resource.proto
    ${VEP_DDS_ROOT}/proto/opentelemetry/proto/metrics/v1/metrics.proto
    ${VEP_DDS_ROOT}/proto/opentelemetry/proto/logs/v1/logs.proto
    ${VEP_DDS_ROOT}/proto/opentelemetry/proto/collector/metrics/v1/metrics_service.proto
    ${VEP_DDS_ROOT}/proto/opentelemetry/proto/collector/logs/v1/logs_service.proto
)

set(PROTO_SRCS)
set(PROTO_HDRS)
set(GRPC_SRCS)
set(GRPC_HDRS)

foreach(PROTO_FILE ${OTLP_PROTO_FILES})
    get_filename_component(PROTO_DIR ${PROTO_FILE} DIRECTORY)
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    file(RELATIVE_PATH PROTO_REL_DIR ${VEP_DDS_ROOT}/proto ${PROTO_DIR})

    set(PROTO_SRC ${PROTO_OUTPUT_DIR}/${PROTO_REL_DIR}/${PROTO_NAME}.pb.cc)
    set(PROTO_HDR ${PROTO_OUTPUT_DIR}/${PROTO_REL_DIR}/${PROTO_NAME}.pb.h)

    list(APPEND PROTO_SRCS ${PROTO_SRC})
    list(APPEND PROTO_HDRS ${PROTO_HDR})

    if(PROTO_FILE MATCHES "service\\.proto$")
        set(GRPC_SRC ${PROTO_OUTPUT_DIR}/${PROTO_REL_DIR}/${PROTO_NAME}.grpc.pb.cc)
        set(GRPC_HDR ${PROTO_OUTPUT_DIR}/${PROTO_REL_DIR}/${PROTO_NAME}.grpc.pb.h)
        list(APPEND GRPC_SRCS ${GRPC_SRC})
        list(APPEND GRPC_HDRS ${GRPC_HDR})

        add_custom_command(
            OUTPUT ${PROTO_SRC} ${PROTO_HDR} ${GRPC_SRC} ${GRPC_HDR}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTO_OUTPUT_DIR}/${PROTO_REL_DIR}
            COMMAND ${PROTOC_EXECUTABLE}
                --proto_path=${VEP_DDS_ROOT}/proto
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
                --proto_path=${VEP_DDS_ROOT}/proto
                --cpp_out=${PROTO_OUTPUT_DIR}
                ${PROTO_FILE}
            DEPENDS ${PROTO_FILE}
            COMMENT "Generating protobuf: ${PROTO_FILE}"
        )
    endif()
endforeach()

add_library(otlp_proto STATIC ${PROTO_SRCS} ${GRPC_SRCS})

target_include_directories(otlp_proto PUBLIC
    ${PROTO_OUTPUT_DIR}
    ${Protobuf_INCLUDE_DIRS}
)

if(gRPC_FOUND)
    target_link_libraries(otlp_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)
else()
    target_link_libraries(otlp_proto PUBLIC ${Protobuf_LIBRARIES} ${GRPC_LIBRARIES})
    target_include_directories(otlp_proto PUBLIC ${GRPC_INCLUDE_DIRS})
endif()

# ============================================================================
# Example Probes (simple demo probes for testing)
# ============================================================================

# NOTE: Production probes (vdr_vss_can_probe, vdr_otel_probe, vdr_avtp_probe)
# are now in vep-core/probes/ with their own per-probe IDL definitions.

# Metrics probe (simple demo)
add_executable(vdr_metrics_probe probes/vdr_metrics_probe/main.cpp)
target_include_directories(vdr_metrics_probe PRIVATE ${VEP_DDS_ROOT}/src)
target_link_libraries(vdr_metrics_probe PRIVATE vdr_common example_telemetry_idl glog::glog)

# Event probe (simple demo)
add_executable(vdr_event_probe probes/vdr_event_probe/main.cpp)
target_include_directories(vdr_event_probe PRIVATE ${VEP_DDS_ROOT}/src)
target_link_libraries(vdr_event_probe PRIVATE vdr_common example_telemetry_idl glog::glog)

# ============================================================================
# Tests (if GTest available)
# ============================================================================

if(TARGET GTest::gtest)
    enable_testing()

    add_executable(test_dds_wrapper ${VEP_DDS_ROOT}/tests/test_dds_wrapper.cpp)
    target_include_directories(test_dds_wrapper PRIVATE ${VEP_DDS_ROOT}/src)
    target_link_libraries(test_dds_wrapper PRIVATE vdr_common example_telemetry_idl GTest::gtest GTest::gtest_main)
    add_test(NAME test_dds_wrapper COMMAND test_dds_wrapper)

    add_executable(test_integration ${VEP_DDS_ROOT}/tests/test_integration.cpp)
    target_include_directories(test_integration PRIVATE ${VEP_DDS_ROOT}/src ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_integration PRIVATE vdr_common example_vdr_sinks example_vdr_testing GTest::gtest)
    add_test(NAME test_integration COMMAND test_integration)
endif()

# ============================================================================
# Installation (optional - examples not installed by default)
# ============================================================================

install(TARGETS vdr_metrics_probe vdr_event_probe
    RUNTIME DESTINATION bin
    COMPONENT examples
    OPTIONAL
)

install(FILES idl/telemetry.idl
    DESTINATION share/vdr-light/examples/idl
    COMPONENT examples
    OPTIONAL
)

# Install example source code for reference
install(DIRECTORY vdr/ testing/ probes/
    DESTINATION share/vdr-light/examples/src
    COMPONENT examples
    OPTIONAL
)
