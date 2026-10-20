cmake_minimum_required(VERSION 3.20)
project(mcp-cpp
    VERSION 0.1.0
    DESCRIPTION "C++20 SDK for the Model Context Protocol"
    HOMEPAGE_URL "https://github.com/Neumann-Labs/mcp-cpp"
    LANGUAGES CXX
)

# ----- Standards & top-level options ---------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Treat us-as-subdirectory differently from us-as-root.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(MCP_IS_TOP_LEVEL ON)
else()
    set(MCP_IS_TOP_LEVEL OFF)
endif()

option(MCP_BUILD_TESTS    "Build the mcp-cpp test suite"     ${MCP_IS_TOP_LEVEL})
option(MCP_BUILD_EXAMPLES "Build the mcp-cpp examples"        ${MCP_IS_TOP_LEVEL})
option(MCP_WARNINGS_AS_ERRORS "Treat warnings as errors"      ${MCP_IS_TOP_LEVEL})
option(MCP_ENABLE_HTTP    "Build the Streamable HTTP transport (depends on Qt Network + HttpServer)" ON)
option(MCP_ENABLE_ASAN    "Build with AddressSanitizer + UBSan" OFF)
option(MCP_ENABLE_TSAN    "Build with ThreadSanitizer"           OFF)
option(MCP_ENABLE_COVERAGE "Build with code-coverage flags"      OFF)

if(MCP_ENABLE_ASAN AND MCP_ENABLE_TSAN)
    message(FATAL_ERROR "MCP_ENABLE_ASAN and MCP_ENABLE_TSAN are mutually exclusive")
endif()

if(MCP_ENABLE_ASAN)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()
if(MCP_ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()
if(MCP_ENABLE_COVERAGE)
    add_compile_options(--coverage -O0 -g)
    add_link_options(--coverage)
endif()

# Shared object/static library export bookkeeping:
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ----- Dependencies --------------------------------------------------------
#
# The SDK's only dependencies are Qt 6 and the C++ standard library. QtCore is
# a PUBLIC dependency (the wire types are exposed as QJsonValue/QJsonObject).
# The optional HTTP transport additionally uses QtNetwork + QtHttpServer; the
# test suite uses QtTest.

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network)

if(MCP_ENABLE_HTTP)
    find_package(Qt6 6.4 REQUIRED COMPONENTS HttpServer)
endif()

if(MCP_BUILD_TESTS)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
endif()

# Qt's moc/rcc/uic are needed for the QObject-based HTTP transports.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# ----- Library -------------------------------------------------------------

# Shared warning set, applied to every first-party target (mcp, mcp-http).
function(mcp_target_warnings tgt)
    target_compile_options(${tgt} PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused
            -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion
            -Wformat=2
        >
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
    )
    if(MCP_WARNINGS_AS_ERRORS)
        target_compile_options(${tgt} PRIVATE
            $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:-Werror>
            $<$<CXX_COMPILER_ID:MSVC>:/WX>
        )
    endif()
endfunction()

# Core library: stdio transport + protocol/session/server/client. Depends only
# on Qt6::Core — deliberately free of QtNetwork/QtHttpServer so stdio-only
# servers stay lean. The HTTP transport lives in a separate `mcp::http` target
# (see src/CMakeLists.txt).
add_library(mcp)
add_library(mcp::mcp ALIAS mcp)

target_compile_features(mcp PUBLIC cxx_std_20)

# The headers live flat in src/ for the build. Expose them under an `mcp/`
# prefix from a generated include dir so the public spelling `<mcp/...>` works
# identically for build-tree consumers (add_subdirectory / FetchContent) and
# installed consumers.
set(MCP_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated-include")
file(GLOB _mcp_public_headers CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.h")
foreach(_header IN LISTS _mcp_public_headers)
    get_filename_component(_header_name "${_header}" NAME)
    file(GENERATE
        OUTPUT  "${MCP_GENERATED_INCLUDE_DIR}/mcp/${_header_name}"
        CONTENT "#pragma once\n#include \"${_header}\"\n")
endforeach()

target_include_directories(mcp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<BUILD_INTERFACE:${MCP_GENERATED_INCLUDE_DIR}>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(mcp PUBLIC Qt6::Core)

mcp_target_warnings(mcp)

# Sources are added by include via add_subdirectory of src/, which also defines
# the optional mcp-http target.
add_subdirectory(src)

# ----- Tests / examples ----------------------------------------------------

# Examples first so the calculator_server target exists when tests pick
# it up for the subprocess integration test.
if(MCP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(MCP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ----- Install -------------------------------------------------------------

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(_mcp_install_targets mcp)
if(MCP_ENABLE_HTTP)
    list(APPEND _mcp_install_targets mcp-http)
endif()

install(TARGETS ${_mcp_install_targets}
    EXPORT  mcp-targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY src/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/mcp
    FILES_MATCHING PATTERN "*.h"
)

install(EXPORT mcp-targets
    FILE        mcp-targets.cmake
    NAMESPACE   mcp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcp
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/mcp-config-version.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/mcp-config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/mcp-config.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcp
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/mcp-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/mcp-config-version.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/mcp
)
