# Pulp Linux-hosted macOS arm64 cross toolchain (osxcross).
#
# Usage:
#   PULP_MACOS_SDK_ROOT=/opt/apple-sdks/MacOSX.sdk \
#   CC=/opt/osxcross/bin/oa64-clang \
#   CXX=/opt/osxcross/bin/oa64-clang++ \
#   OBJC=/opt/osxcross/bin/oa64-clang \
#   OBJCXX=/opt/osxcross/bin/oa64-clang++ \
#   CMAKE_C_COMPILER=/opt/osxcross/bin/oa64-clang \
#   CMAKE_CXX_COMPILER=/opt/osxcross/bin/oa64-clang++ \
#   CMAKE_AR=/opt/osxcross/bin/arm64-apple-darwin24-ar \
#   CMAKE_RANLIB=/opt/osxcross/bin/arm64-apple-darwin24-ranlib \
#   CMAKE_INSTALL_NAME_TOOL=/opt/osxcross/bin/arm64-apple-darwin24-install_name_tool \
#   CMAKE_STRIP=/opt/osxcross/bin/arm64-apple-darwin24-strip \
#   MACOSX_DEPLOYMENT_TARGET=15.0 \
#   cmake -S . -B build-macos-cross \
#     -DCMAKE_TOOLCHAIN_FILE=tools/cmake/toolchains/macos-arm64-osxcross.cmake \
#     -DCMAKE_BUILD_TYPE=Release
#
# The exact osxcross prefix names depend on the SDK version used to
# bootstrap osxcross; substitute `arm64-apple-darwin24` for whichever
# triple your osxcross install produced.
#
# Design notes and full architecture: see
# planning/2026-05-24-linux-hosted-macos-arm64-cross-lane.md.

cmake_minimum_required(VERSION 3.24)

set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)

if(NOT DEFINED ENV{PULP_MACOS_SDK_ROOT})
    message(FATAL_ERROR
        "PULP_MACOS_SDK_ROOT must point to a materialized MacOSX.sdk. "
        "See planning/2026-05-24-linux-hosted-macos-arm64-cross-lane.md "
        "for SDK provisioning policy.")
endif()

set(CMAKE_OSX_SYSROOT "$ENV{PULP_MACOS_SDK_ROOT}" CACHE PATH "macOS SDK sysroot" FORCE)
set(CMAKE_OSX_ARCHITECTURES arm64 CACHE STRING "macOS architectures" FORCE)

# The chrome/m149 darwin-arm64 Skia/Dawn assets currently shipped with
# Pulp encode macOS 15.0 as their minimum OS. Honor the env override
# when set explicitly so a host can target a different deployment OS
# for non-GPU prototype builds.
if(DEFINED ENV{MACOSX_DEPLOYMENT_TARGET} AND NOT "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "$ENV{MACOSX_DEPLOYMENT_TARGET}"
        CACHE STRING "macOS deployment target" FORCE)
endif()

# Compiler/binutils overrides — accept them from the environment so the
# infra repo can point at whatever osxcross prefix it has on disk
# without baking absolute paths into Pulp.
foreach(_var
        CMAKE_C_COMPILER CMAKE_CXX_COMPILER
        CMAKE_OBJC_COMPILER CMAKE_OBJCXX_COMPILER
        CMAKE_AR CMAKE_RANLIB
        CMAKE_INSTALL_NAME_TOOL CMAKE_STRIP)
    if(DEFINED ENV{${_var}} AND NOT "$ENV{${_var}}" STREQUAL "")
        set(${_var} "$ENV{${_var}}" CACHE FILEPATH "" FORCE)
    endif()
endforeach()

# Keep finders confined to the Darwin sysroot for libraries/headers
# while letting CMake reach host programs (cmake, ninja, python, cargo,
# rcodesign) from the Linux PATH.
set(CMAKE_FIND_ROOT_PATH "${CMAKE_OSX_SYSROOT}" CACHE STRING "Darwin find root" FORCE)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Signal to downstream Pulp CMake that this is a non-macOS host
# building Darwin artifacts. Helpers like PulpAppIcon.cmake check this
# (in concert with PULP_MACOS_CROSS_ALLOW_MISSING_ICON_TOOLS) to skip
# steps that depend on macOS-only host tools such as `sips`/`iconutil`.
set(PULP_MACOS_CROSS ON CACHE BOOL
    "Building macOS artifacts from a non-macOS host" FORCE)
