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

# ── Optional Subsystems ────────────────────────────────────────────────────
if(PULP_ENABLE_GPU)
    add_subdirectory(core/render)
endif()

add_subdirectory(inspect)

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

# ── Examples ────────────────────────────────────────────────────────────────
if(PULP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ── CMake Module Path (for pulp_add_plugin, pulp_add_app) ──────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake")
include(PulpUtils)
