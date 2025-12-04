cmake_minimum_required(VERSION 3.16)
project(vdr-light VERSION 0.1.0 LANGUAGES C CXX)

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

# Build options
option(VDR_LIGHT_BUILD_EXAMPLES "Build example probes with example IDL" ON)

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

# ============================================================================
# Required Dependencies
# ============================================================================

find_package(CycloneDDS REQUIRED)
find_package(glog REQUIRED)

# ============================================================================
# Core Library (IDL-agnostic)
# ============================================================================
# This library provides DDS utilities, QoS profiles, and time utilities.
# It does NOT include any IDL types - those must be provided by the consuming
# project or by building the examples.
# ============================================================================

add_library(vdr_common STATIC
    src/common/dds_wrapper.cpp
    src/common/qos_profiles.cpp
    src/common/time_utils.cpp
)

add_library(vdr::common ALIAS vdr_common)

target_include_directories(vdr_common PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(vdr_common PUBLIC
    CycloneDDS::ddsc
    glog::glog
)

# ============================================================================
# Installation
# ============================================================================

include(GNUInstallDirs)

# Set export name for namespace
set_target_properties(vdr_common PROPERTIES EXPORT_NAME common)

# Install library
install(TARGETS vdr_common
    EXPORT vdr-light-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install headers
install(DIRECTORY src/common/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/vdr/common
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
)

# Export configuration
install(EXPORT vdr-light-targets
    FILE vdr-light-targets.cmake
    NAMESPACE vdr::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vdr-light
)

# Package config
include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/vdr-light-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/vdr-light-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vdr-light
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/vdr-light-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/vdr-light-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/vdr-light-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/vdr-light
)

# ============================================================================
# Examples (optional)
# ============================================================================
# Examples include IDL-dependent code: sinks, subscriber, test fixtures,
# and example probes. These demonstrate how to use vdr_common with your
# own IDL definitions.
# ============================================================================

if(VDR_LIGHT_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ============================================================================
# Summary
# ============================================================================

message(STATUS "")
message(STATUS "=== VDR-Light Build Configuration ===")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Cyclone DDS: ${CycloneDDS_VERSION}")
message(STATUS "")
message(STATUS "Core Library (IDL-agnostic):")
message(STATUS "  - vdr_common    (DDS wrappers, QoS profiles, utilities)")
message(STATUS "")
message(STATUS "Options:")
message(STATUS "  VDR_LIGHT_BUILD_EXAMPLES: ${VDR_LIGHT_BUILD_EXAMPLES}")
message(STATUS "")
if(VDR_LIGHT_BUILD_EXAMPLES)
message(STATUS "Examples will include:")
message(STATUS "  - telemetry_idl (example DDS IDL types)")
message(STATUS "  - vdr_sinks     (output sinks using example IDL)")
message(STATUS "  - vdr_core      (subscriber infrastructure)")
message(STATUS "  - Example probes")
message(STATUS "")
endif()
