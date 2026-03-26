cmake_minimum_required(VERSION 3.24)

project(Pulp
    VERSION 0.1.0
    DESCRIPTION "Cross-platform audio plugin and application framework"
    HOMEPAGE_URL "https://github.com/danielraffel/pulp"
    LANGUAGES CXX
)

# ── C++20 Standard ──────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Required on Linux: static libraries linked into shared objects (.clap, .so) need PIC
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Platform Detection ──────────────────────────────────────────────────────
if(APPLE)
    if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
        set(PULP_IOS TRUE)
        set(PULP_PLATFORM "ios")
    else()
        set(PULP_MACOS TRUE)
        set(PULP_PLATFORM "mac")
    endif()
    set(PULP_APPLE TRUE)
elseif(WIN32)
    set(PULP_WINDOWS TRUE)
    set(PULP_PLATFORM "win")
elseif(UNIX)
    set(PULP_LINUX TRUE)
    set(PULP_PLATFORM "linux")
elseif(ANDROID)
    set(PULP_ANDROID TRUE)
    set(PULP_PLATFORM "android")
endif()

message(STATUS "Pulp ${PROJECT_VERSION} — platform: ${PULP_PLATFORM}")

# ── Options ─────────────────────────────────────────────────────────────────
option(PULP_BUILD_TESTS "Build unit tests" ON)
option(PULP_BUILD_EXAMPLES "Build example projects" ON)
option(PULP_ENABLE_GPU "Enable GPU rendering (Dawn/Skia)" ON)
option(PULP_ENABLE_SWIFT "Enable Swift layer (Apple only)" OFF)

# ── Compiler Warnings ──────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /wd4100)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# ── Sanitizers (optional) ──────────────────────────────────────────────────
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/Sanitizers.cmake)

# ── Third-Party Dependencies ────────────────────────────────────────────────
include(FetchContent)

# CHOC: header-only C++ utilities (ISC license)
# Used for: JS engine abstraction, MIDI utilities
FetchContent_Declare(
    choc
    GIT_REPOSITORY https://github.com/Tracktion/choc.git
    GIT_TAG main
)
FetchContent_MakeAvailable(choc)

# WebGPU (Dawn) — cross-platform GPU abstraction (BSD-3-Clause)
# Uses eliemichel/WebGPU-distribution for CMake-friendly integration
if(PULP_ENABLE_GPU)
    FetchContent_Declare(
        webgpu
        GIT_REPOSITORY https://github.com/eliemichel/WebGPU-distribution.git
        GIT_TAG main
    )
    FetchContent_MakeAvailable(webgpu)
    set(PULP_HAS_WEBGPU TRUE)
    message(STATUS "Pulp: WebGPU (Dawn) enabled")

    # Skia Graphite — GPU-accelerated 2D rendering (BSD-3-Clause)
    # Pre-built via skia-builder (https://github.com/olilarkin/skia-builder)
    include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/FindSkia.cmake)
    if(SKIA_FOUND)
        set(PULP_HAS_SKIA TRUE)
    endif()
endif()

# SDL3 — cross-platform windowing, input, GPU context (zlib license)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.12
    GIT_SHALLOW TRUE
)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3)
set(PULP_HAS_SDL3 TRUE)
message(STATUS "Pulp: SDL3 enabled")

# VST3 SDK (MIT license) — cloned into external/vst3sdk/
set(VST3_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/vst3sdk")
if(EXISTS "${VST3_SDK_DIR}/pluginterfaces")
    set(PULP_HAS_VST3 TRUE)

    # Build the minimal VST3 SDK base library
    file(GLOB_RECURSE VST3_BASE_SOURCES
        "${VST3_SDK_DIR}/base/source/*.cpp"
        "${VST3_SDK_DIR}/base/thread/source/*.cpp"
    )
    file(GLOB_RECURSE VST3_PLUGIF_SOURCES
        "${VST3_SDK_DIR}/pluginterfaces/base/*.cpp"
    )
    file(GLOB_RECURSE VST3_SDK_SOURCES
        "${VST3_SDK_DIR}/public.sdk/source/vst/vstaudioeffect.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/vst/vstbus.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/vst/vstcomponent.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/vst/vstcomponentbase.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/vst/vstinitiids.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/vst/vstparameters.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/vst/vstsinglecomponenteffect.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/common/pluginfactory.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/common/pluginview.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/common/commoniids.cpp"
    )

    add_library(vst3-sdk STATIC
        ${VST3_BASE_SOURCES}
        ${VST3_PLUGIF_SOURCES}
        ${VST3_SDK_SOURCES}
    )
    target_include_directories(vst3-sdk PUBLIC
        ${VST3_SDK_DIR}
    )
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        target_compile_definitions(vst3-sdk PUBLIC RELEASE=1)
    else()
        target_compile_definitions(vst3-sdk PUBLIC DEVELOPMENT=1)
    endif()
    # Suppress warnings in third-party code
    target_compile_options(vst3-sdk PRIVATE -w)

    message(STATUS "Pulp: VST3 SDK found at ${VST3_SDK_DIR}")
else()
    set(PULP_HAS_VST3 FALSE)
    message(STATUS "Pulp: VST3 SDK not found — VST3 format disabled")
endif()

# CLAP (MIT license) — header-only plugin format
FetchContent_Declare(
    clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG 1.2.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(clap)
set(PULP_HAS_CLAP TRUE)
message(STATUS "Pulp: CLAP SDK available")

# LV2 (ISC license) — header-only plugin format (Linux-first)
FetchContent_Declare(
    lv2
    GIT_REPOSITORY https://github.com/lv2/lv2.git
    GIT_TAG v1.18.10
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(lv2)
# LV2 headers are in lv2/include/
add_library(lv2-headers INTERFACE)
target_include_directories(lv2-headers INTERFACE
    $<BUILD_INTERFACE:${lv2_SOURCE_DIR}/include>
)
set(PULP_HAS_LV2 TRUE)
message(STATUS "Pulp: LV2 SDK available")

# Apple AudioUnitSDK (Apache 2.0) — for AU v2 plugin support
set(AUSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/AudioUnitSDK")
if(APPLE AND EXISTS "${AUSDK_DIR}/include/AudioUnitSDK/AUBase.h")
    set(PULP_HAS_AUSDK TRUE)

    file(GLOB AUSDK_SOURCES "${AUSDK_DIR}/src/AudioUnitSDK/*.cpp")
    add_library(ausdk STATIC ${AUSDK_SOURCES})
    target_include_directories(ausdk PUBLIC "${AUSDK_DIR}/include")
    target_link_libraries(ausdk PUBLIC
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework CoreAudio"
    )
    target_compile_options(ausdk PRIVATE -w)  # Suppress third-party warnings
    target_compile_features(ausdk PUBLIC cxx_std_23)  # AudioUnitSDK 1.4 requires std::expected

    message(STATUS "Pulp: AudioUnitSDK found — AU v2 support enabled")
else()
    set(PULP_HAS_AUSDK FALSE)
    message(STATUS "Pulp: AudioUnitSDK not found — AU v2 format disabled")
endif()

# ── Core Subsystems ─────────────────────────────────────────────────────────
# Order matters: dependencies first
add_subdirectory(core/platform)
add_subdirectory(core/runtime)
add_subdirectory(core/events)
add_subdirectory(core/state)
add_subdirectory(core/audio)
add_subdirectory(core/midi)
add_subdirectory(core/signal)
add_subdirectory(core/format)
add_subdirectory(core/canvas)
add_subdirectory(core/view)
add_subdirectory(core/osc)

# ── Optional Subsystems ────────────────────────────────────────────────────
if(PULP_ENABLE_GPU)
    add_subdirectory(core/render)
endif()

if(PULP_ENABLE_GPU)
    add_subdirectory(inspect)
endif()
add_subdirectory(ship)

# ── Testing ─────────────────────────────────────────────────────────────────
if(PULP_BUILD_TESTS)
    enable_testing()
    include(FetchContent)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    add_subdirectory(test)
endif()

# ── CMake Module Path (for pulp_add_plugin, pulp_add_app) ──────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake")
include(PulpUtils)

# ── CLI Tool ───────────────────────────────────────────────────────────────
add_subdirectory(tools/cli)

# ── MCP Server ─────────────────────────────────────────────────────────────
add_subdirectory(tools/mcp)

# ── Screenshot Tool ────────────────────────────────────────────────────────
add_subdirectory(tools/screenshot)

# ── Python Bindings ────────────────────────────────────────────────────────
option(PULP_BUILD_PYTHON "Build Python bindings via pybind11" OFF)
if(PULP_BUILD_PYTHON)
    add_subdirectory(bindings/python)
endif()

# ── Node.js Bindings ──────────────────────────────────────────────────────
option(PULP_BUILD_NODEJS "Build Node.js bindings via Node-API" OFF)
if(PULP_BUILD_NODEJS)
    add_subdirectory(bindings/nodejs)
endif()

# ── Examples ────────────────────────────────────────────────────────────────
if(PULP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
