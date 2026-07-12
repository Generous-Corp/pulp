# macOS target architecture — the easy knob for building a plugin/app for Intel
# Macs, Apple Silicon, or both. Included from the root CMakeLists BEFORE
# PulpMinOs/project() so the min-OS floor and every dependency's arch selection
# see the final CMAKE_OSX_ARCHITECTURES. (Kept in its own module so the root
# CMakeLists still declares `project(Pulp ...)` within its first lines — the
# source-tree-pollution gate requires that.)
#
#   -DPULP_MACOS_ARCH=host       (default) build for the build machine's arch —
#                                fast local dev iteration (arm64 on Apple Silicon)
#   -DPULP_MACOS_ARCH=universal  fat arm64+x86_64 binary — runs on every Mac
#                                (recommended for DISTRIBUTION: build with
#                                `pulp build --arch universal` before `pulp ship`)
#   -DPULP_MACOS_ARCH=arm64      thin Apple Silicon only
#   -DPULP_MACOS_ARCH=x86_64     thin Intel only
#
# An explicit -DCMAKE_OSX_ARCHITECTURES=... on the command line always wins (this
# module is a no-op then). Note: PULP_JS_ENGINE=v8 cannot build universal (no
# universal libv8) — use a single arch there; QuickJS/JSC (the defaults) are fine.
if(APPLE AND NOT CMAKE_OSX_ARCHITECTURES)
    set(PULP_MACOS_ARCH "host" CACHE STRING
        "macOS target arch: host | universal | arm64 | x86_64")
    set_property(CACHE PULP_MACOS_ARCH PROPERTY STRINGS host universal arm64 x86_64)
    if(PULP_MACOS_ARCH STREQUAL "universal")
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Pulp: universal macOS build" FORCE)
    elseif(PULP_MACOS_ARCH STREQUAL "arm64" OR PULP_MACOS_ARCH STREQUAL "x86_64")
        set(CMAKE_OSX_ARCHITECTURES "${PULP_MACOS_ARCH}" CACHE STRING "Pulp: thin macOS build" FORCE)
    elseif(NOT PULP_MACOS_ARCH STREQUAL "host")
        message(FATAL_ERROR "PULP_MACOS_ARCH must be one of: host, universal, arm64, x86_64 (got '${PULP_MACOS_ARCH}')")
    endif()
    # host: leave CMAKE_OSX_ARCHITECTURES unset → CMake uses the host arch.
endif()
