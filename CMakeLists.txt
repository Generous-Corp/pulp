cmake_minimum_required(VERSION 3.24)

project(Pulp
    VERSION 0.96.0
    DESCRIPTION "Cross-platform audio plugin and application framework"
    HOMEPAGE_URL "https://github.com/danielraffel/pulp"
    LANGUAGES CXX C
)

# ── C++20 Standard ──────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(APPLE)
    include(CheckLanguage)
    check_language(OBJCXX)
    if(CMAKE_OBJCXX_COMPILER)
        enable_language(OBJCXX)
        set(CMAKE_OBJCXX_STANDARD 20)
        set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
    endif()
endif()

# Required on Linux: static libraries linked into shared objects (.clap, .so) need PIC
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ── Platform Detection ──────────────────────────────────────────────────────
# Note: ANDROID must be checked before UNIX because Android is also UNIX.
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
elseif(ANDROID)
    set(PULP_ANDROID TRUE)
    set(PULP_PLATFORM "android")
elseif(UNIX)
    set(PULP_LINUX TRUE)
    set(PULP_PLATFORM "linux")
endif()

message(STATUS "Pulp ${PROJECT_VERSION} — platform: ${PULP_PLATFORM}")

# ── Options ─────────────────────────────────────────────────────────────────
option(PULP_BUILD_TESTS "Build unit tests" ON)
option(PULP_BUILD_EXAMPLES "Build example projects" ON)
option(PULP_ENABLE_GPU "Enable GPU rendering (Dawn/Skia)" ON)
# SDK-release safety net for pulp #1817. When ON, configure FAILS if
# PULP_ENABLE_GPU is set but Skia binaries weren't located. The release
# workflow flips this ON so an SDK tarball can never ship with the GPU
# code path silently #ifdef'd out. Local developer builds leave it OFF
# (the default) so a no-Skia incremental rebuild still configures and
# transparently falls back to the CG path.
option(PULP_REQUIRE_GPU_FOR_SDK
       "Fail configure if PULP_ENABLE_GPU=ON but Skia was not found. \
        Set ON in .github/workflows/release-cli.yml to prevent silently \
        shipping CG-only SDKs (pulp #1817)."
       OFF)
option(PULP_TEXT_SHAPING "Enable SkParagraph text shaping (HarfBuzz/ICU via Skia). Default: ON when GPU enabled." ON)
option(PULP_ENABLE_SWIFT "Enable Swift layer (Apple only)" OFF)
option(PULP_ENABLE_AAX "Enable optional AAX support with a developer-supplied SDK" OFF)
option(PULP_ENABLE_ARA "Enable optional ARA 2.x support with a developer-supplied Celemony SDK" OFF)
option(PULP_FETCHCONTENT_UPDATES_DISCONNECTED "Avoid implicit FetchContent updates during configure" ON)
option(PULP_USE_SHARED_FETCHCONTENT_SOURCES "Prefer machine-local shared source caches for FetchContent dependencies" ON)
option(PULP_BENCHMARK "Enable zero-copy benchmark perf counters (Slice 0 of #516). Tooling-only; OFF by default so production builds see zero overhead." OFF)
if(PULP_BENCHMARK)
    add_compile_definitions(PULP_BENCHMARK)
endif()
set(PULP_SHARED_FETCHCONTENT_SOURCE_DIR "" CACHE PATH "Machine-local shared source cache root for FetchContent dependencies")
set(PULP_AAX_SDK_DIR "" CACHE PATH "Path to a developer-supplied AAX SDK kept outside the Pulp source tree")
set(PULP_ARA_SDK_DIR "" CACHE PATH "Path to a developer-supplied Celemony ARA SDK. Enables ARA 2.x when PULP_ENABLE_ARA=ON.")

# ── Compiler Warnings ──────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /wd4100)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

if(WIN32)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ── Build acceleration (optional) ──────────────────────────────────────────
# Use ccache when available. Must be included BEFORE the first target
# is declared so the CMAKE_<LANG>_COMPILER_LAUNCHER setting applies.
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/Ccache.cmake)

# ── Instrumentation (optional) ─────────────────────────────────────────────
# Sanitizers via PULP_SANITIZER=address|thread|undefined|memory|realtime
# Coverage via PULP_ENABLE_COVERAGE=ON (Clang only, mutually exclusive
# with PULP_SANITIZER).
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/Sanitizers.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpInstrumentation.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpAAX.cmake)
pulp_configure_aax_sdk()

# ── Third-Party Dependencies ────────────────────────────────────────────────
include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ${PULP_FETCHCONTENT_UPDATES_DISCONNECTED})
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpFetchContent.cmake)

# CHOC: header-only C++ utilities (ISC license)
# Used for: JS engine abstraction, MIDI utilities
pulp_register_fetchcontent_source(choc REF f0f5cdf5a938b8b779fea6c083571cce5ccab925)
FetchContent_Declare(
    choc
    GIT_REPOSITORY https://github.com/Tracktion/choc.git
    GIT_TAG f0f5cdf5a938b8b779fea6c083571cce5ccab925
)
FetchContent_MakeAvailable(choc)

# WebGPU (Dawn) — cross-platform GPU abstraction (BSD-3-Clause)
# Uses eliemichel/WebGPU-distribution for CMake-friendly integration
# On Android + iOS, Dawn is bundled in Skia's pre-built libs — skip
# FetchContent. wgpu-native doesn't publish iOS prebuilds (tracked in
# #359); pulp's render path is Dawn-only whenever Skia is the backend
# anyway (pulp::render::GpuSurface uses Dawn's C++ API under PULP_HAS_SKIA).
if(PULP_ENABLE_GPU AND NOT ANDROID AND NOT IOS)
    pulp_register_fetchcontent_source(webgpu REF 17dcd42a7683355e7a40ac4e97e77f36dff5b5ab)
    pulp_register_wgpu_native_precompiled_source(v24.0.3.1)
    FetchContent_Declare(
        webgpu
        GIT_REPOSITORY https://github.com/eliemichel/WebGPU-distribution.git
        GIT_TAG 17dcd42a7683355e7a40ac4e97e77f36dff5b5ab
    )
    set(_pulp_restore_arch FALSE)
    if(WIN32)
        # Use the TARGET platform for wgpu arch selection, not the host
        # processor. CMAKE_SYSTEM_PROCESSOR reports the HOST arch on
        # Windows, which is wrong when cross-compiling via -A <platform>.
        # An ARM64 host building with -A x64 needs x64 wgpu binaries,
        # not ARM64 — otherwise the linker fails with
        # "library machine type 'ARM64' conflicts with target 'x64'."
        if(CMAKE_GENERATOR_PLATFORM)
            string(TOLOWER "${CMAKE_GENERATOR_PLATFORM}" _pulp_webgpu_processor)
        else()
            string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _pulp_webgpu_processor)
        endif()
        if(_pulp_webgpu_processor STREQUAL "arm64" OR _pulp_webgpu_processor STREQUAL "aarch64")
            if(DEFINED ARCH)
                set(_pulp_saved_arch "${ARCH}")
            endif()
            set(ARCH "aarch64")
            set(_pulp_restore_arch TRUE)
            message(STATUS "Pulp: forcing WebGPU prebuilt architecture to aarch64 for ARM64")
        elseif(_pulp_webgpu_processor STREQUAL "x64" AND CMAKE_SYSTEM_PROCESSOR MATCHES "[Aa][Rr][Mm]64")
            # Cross-compiling x64 on an ARM64 host. wgpu-native's
            # detect_system_architecture() sees the HOST processor
            # (ARM64) and fails with "Unknown architecture" because
            # it doesn't map Windows ARM64 to a prebuilt arch string.
            # Explicitly set ARCH=x86_64 so it downloads x64 binaries.
            if(DEFINED ARCH)
                set(_pulp_saved_arch "${ARCH}")
            endif()
            set(ARCH "x86_64")
            set(_pulp_restore_arch TRUE)
            message(STATUS "Pulp: forcing WebGPU prebuilt architecture to x86_64 for x64 cross-compile on ARM64 host")
        endif()
    endif()
    FetchContent_MakeAvailable(webgpu)
    if(_pulp_restore_arch)
        if(DEFINED _pulp_saved_arch)
            set(ARCH "${_pulp_saved_arch}")
        else()
            unset(ARCH)
        endif()
        unset(_pulp_saved_arch)
    endif()
    unset(_pulp_restore_arch)
    unset(_pulp_webgpu_processor)
    set(PULP_HAS_WEBGPU TRUE)
    message(STATUS "Pulp: WebGPU (Dawn) enabled")

endif()

# Skia Graphite — GPU-accelerated 2D rendering (BSD-3-Clause)
# Pre-built via skia-builder (desktop) or build-skia-android.sh (Android)
set(PULP_HAS_SKIA FALSE)
if(PULP_ENABLE_GPU)
    include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/FindSkia.cmake)
    if(SKIA_FOUND)
        set(PULP_HAS_SKIA TRUE)
        # On Android + iOS, Dawn is bundled in Skia's pre-built libs.
        # Set PULP_HAS_WEBGPU so the render subsystem builds without
        # needing the desktop wgpu-native FetchContent path. iOS in
        # particular has no wgpu-native prebuild and won't until #359
        # is resolved — Dawn is the only viable backend, which matches
        # how pulp::render uses Dawn when PULP_HAS_SKIA is set.
        if(ANDROID OR IOS)
            set(PULP_HAS_WEBGPU TRUE)
        endif()

        # Text shaping via SkParagraph (HarfBuzz + ICU, bundled in Skia pre-built)
        if(PULP_TEXT_SHAPING)
            set(PULP_HAS_TEXT_SHAPING TRUE)
            message(STATUS "Pulp: SkParagraph text shaping enabled (HarfBuzz/ICU via Skia)")
        endif()
    else()
        if(PULP_TEXT_SHAPING)
            message(STATUS "Pulp: Text shaping disabled (Skia not found)")
            set(PULP_TEXT_SHAPING OFF)
        endif()
    endif()
endif()

# SDK-release safety net for pulp #1817. The release workflow turns this
# option ON via -DPULP_REQUIRE_GPU_FOR_SDK=ON so a missing Skia tarball
# is a hard configure error instead of a silent CG-only build that ships
# downstream to every SDK consumer.
if(PULP_ENABLE_GPU AND PULP_REQUIRE_GPU_FOR_SDK AND NOT PULP_HAS_SKIA)
    message(FATAL_ERROR
        "PULP_REQUIRE_GPU_FOR_SDK=ON but Skia binaries were not found at "
        "external/skia-build/build/<platform>-gpu/lib/Release/libskia.a. "
        "The release tarball would ship without MacGpuWindowHost and "
        "consumers passing use_gpu=true would silently fall back to "
        "CoreGraphics CPU rendering. Either fetch the Skia binaries (see "
        ".github/workflows/release-cli.yml 'Fetch Skia binaries' step, or "
        "tools/scripts/fetch_skia_for_release.py for the manifest-driven "
        "fetch) or set PULP_ENABLE_GPU=OFF if you intend a CG-only build.")
endif()

# Text shaping requires GPU/Skia — validate even when GPU is off
if(PULP_TEXT_SHAPING AND NOT PULP_ENABLE_GPU)
    message(WARNING "PULP_TEXT_SHAPING requires PULP_ENABLE_GPU — disabling text shaping")
    set(PULP_TEXT_SHAPING OFF)
endif()

# SDL3 — cross-platform windowing, input, GPU context (zlib license)
pulp_register_fetchcontent_source(SDL3 DIR sdl3 REF release-3.2.12)
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

# DRACO mesh compression decoder (Apache 2.0) — for glTF compressed geometry
option(PULP_ENABLE_DRACO "Enable DRACO mesh decompression for glTF" OFF)
if(PULP_ENABLE_DRACO)
    FetchContent_Declare(
        draco
        GIT_REPOSITORY https://github.com/google/draco.git
        GIT_TAG 1.5.7
        GIT_SHALLOW TRUE
    )
    set(DRACO_TRANSCODER_SUPPORTED OFF CACHE BOOL "" FORCE)
    set(DRACO_TESTS OFF CACHE BOOL "" FORCE)
    set(DRACO_JS_GLUE OFF CACHE BOOL "" FORCE)
    set(DRACO_WASM OFF CACHE BOOL "" FORCE)
    set(DRACO_UNITY_PLUGIN OFF CACHE BOOL "" FORCE)
    set(DRACO_MAYA_PLUGIN OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(draco)
    set(PULP_HAS_DRACO TRUE)
    message(STATUS "Pulp: DRACO mesh decompression enabled")
endif()

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
        "${VST3_SDK_DIR}/public.sdk/source/common/pluginview.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/common/commoniids.cpp"
        # Hosting helpers used by the VST3 plugin slot (workstream 03 3.5).
        "${VST3_SDK_DIR}/public.sdk/source/vst/hosting/parameterchanges.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/vst/hosting/eventlist.cpp"
    )

    add_library(vst3-sdk STATIC
        ${VST3_BASE_SOURCES}
        ${VST3_PLUGIF_SOURCES}
        ${VST3_SDK_SOURCES}
    )
    target_include_directories(vst3-sdk PUBLIC
        $<BUILD_INTERFACE:${VST3_SDK_DIR}>
        $<INSTALL_INTERFACE:external/vst3sdk>
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
pulp_register_fetchcontent_source(clap REF 1.2.2)
FetchContent_Declare(
    clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG 1.2.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(clap)
target_include_directories(clap INTERFACE
    $<BUILD_INTERFACE:${clap_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:external/clap/include>
)
set(PULP_HAS_CLAP TRUE)
message(STATUS "Pulp: CLAP SDK available")

# LV2 (ISC license) — header-only plugin format (Linux-first)
pulp_register_fetchcontent_source(lv2 REF v1.18.10)
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
    $<INSTALL_INTERFACE:external/lv2/include>
)
set(PULP_HAS_LV2 TRUE)
message(STATUS "Pulp: LV2 SDK available")

# Facebook Yoga (MIT license) — cross-platform CSS Flexbox/Grid layout engine
pulp_register_fetchcontent_source(yoga REF v3.2.1)
FetchContent_Declare(
    yoga
    GIT_REPOSITORY https://github.com/facebook/yoga.git
    GIT_TAG v3.2.1
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR yoga
)
FetchContent_MakeAvailable(yoga)
set(PULP_HAS_YOGA TRUE)
message(STATUS "Pulp: Yoga layout engine enabled")

# Google Highway (Apache 2.0) — portable SIMD abstraction (SSE/NEON/AVX)
set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_CONTRIB OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
pulp_register_fetchcontent_source(highway REF 1.2.0)
FetchContent_Declare(
    highway
    GIT_REPOSITORY https://github.com/google/highway.git
    GIT_TAG 1.2.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(highway)
set(PULP_HAS_HIGHWAY TRUE)
message(STATUS "Pulp: Highway SIMD library enabled")

# Mbed TLS (Apache 2.0) — cryptographic primitives (SHA-256, RSA, AES)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG v3.6.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(mbedtls)
set(PULP_HAS_MBEDTLS TRUE)
message(STATUS "Pulp: Mbed TLS crypto library enabled")

# three.js (MIT license) — native WebGPU bridge demos and tests
if(PULP_BUILD_TESTS AND PULP_ENABLE_GPU)
    pulp_register_fetchcontent_source(threejs REF 077dd13c0e869d9f3dbe55875686f920367de457)
    FetchContent_Declare(
        threejs
        GIT_REPOSITORY https://github.com/mrdoob/three.js.git
        GIT_TAG 077dd13c0e869d9f3dbe55875686f920367de457
    )
    FetchContent_MakeAvailable(threejs)
    set(PULP_HAS_THREEJS TRUE)
    message(STATUS "Pulp: three.js native bridge dependency available")
endif()

# Apple AudioUnitSDK (Apache 2.0) — for AU v2 plugin support (macOS only, not iOS)
set(AUSDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/AudioUnitSDK")
if(PULP_MACOS AND EXISTS "${AUSDK_DIR}/include/AudioUnitSDK/AUBase.h")
    set(PULP_HAS_AUSDK TRUE)

    file(GLOB AUSDK_SOURCES "${AUSDK_DIR}/src/AudioUnitSDK/*.cpp")
    add_library(ausdk STATIC ${AUSDK_SOURCES})
    target_include_directories(ausdk PUBLIC
        $<BUILD_INTERFACE:${AUSDK_DIR}/include>
        $<INSTALL_INTERFACE:external/AudioUnitSDK/include>
    )
    target_link_libraries(ausdk PUBLIC
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework CoreAudio"
    )
    target_compile_options(ausdk PRIVATE -w)  # Suppress third-party warnings
    # AudioUnitSDK 1.4's own .cpp files #include <expected> (C++23). Under
    # CMake 3.24's policy, the root's CMAKE_CXX_STANDARD=20 is authoritative
    # on every target and silently downgrades target_compile_features's
    # minimum (cxx_std_23) to whatever the root says. The target property
    # CXX_STANDARD bypasses that and forces C++23 for AU-SDK compilation.
    # PUBLIC compile_features stays so downstream consumers inherit the
    # minimum requirement if they rebuild with an older root standard.
    set_target_properties(ausdk PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
    )
    target_compile_features(ausdk PUBLIC cxx_std_23)

    message(STATUS "Pulp: AudioUnitSDK found — AU v2 support enabled")
else()
    set(PULP_HAS_AUSDK FALSE)
    message(STATUS "Pulp: AudioUnitSDK not found — AU v2 format disabled")
endif()

# ── Android NDK Support ───────────────────────────────────────────────────
if(ANDROID)
    include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpAndroid.cmake)
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
# Render must come before view so pulp-view can link pulp-render via if(TARGET)
if(PULP_ENABLE_GPU)
    add_subdirectory(core/render)
endif()
add_subdirectory(core/view)
add_subdirectory(core/osc)
add_subdirectory(core/host)
add_subdirectory(core/dsl)

# Wire Android-specific sources into subsystem targets (must come after add_subdirectory)
if(ANDROID)
    pulp_wire_android_sources()
endif()

add_library(pulp-standalone STATIC
    core/format/src/standalone.cpp
    core/format/src/test_signal.cpp
    core/format/src/settings_panel.cpp
)
add_library(pulp::standalone ALIAS pulp-standalone)
target_include_directories(pulp-standalone
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/format/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(pulp-standalone
    PUBLIC
        pulp::format
        pulp::view
        $<BUILD_INTERFACE:yogacore>
        $<INSTALL_INTERFACE:Pulp::yogacore>
        $<BUILD_INTERFACE:SDL3::SDL3-static>
        $<INSTALL_INTERFACE:Pulp::sdl3-static>
)

if(PULP_ENABLE_GPU AND NOT ANDROID)
    add_subdirectory(inspect)
endif()
add_subdirectory(ship)
add_subdirectory(tools/audio)

# ── Testing ─────────────────────────────────────────────────────────────────
if(PULP_BUILD_TESTS AND NOT ANDROID)
    enable_testing()
    include(FetchContent)
    pulp_register_fetchcontent_source(Catch2 DIR catch2 REF v3.7.1)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v3.7.1
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
    include(Catch)
    set(_CATCH_DISCOVER_TESTS_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpCatchAddTests.cmake"
        CACHE INTERNAL "Pulp path to CatchAddTests.cmake helper file" FORCE)
    add_subdirectory(test)
endif()

# ── CMake Module Path (for pulp_add_plugin, pulp_add_app) ──────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake")
include(PulpUtils)

# ── CLI Tool (desktop only, requires GPU stack via cmd_inspect) ───────────
# pulp-cli unconditionally links pulp::inspect (for `pulp inspect`), and
# pulp::inspect depends on pulp::render which is GPU-only. Without the
# GPU guard, PULP_ENABLE_GPU=OFF builds (sanitizer matrix in
# .github/workflows/sanitizers.yml) fail at CMake configure with
# "Target ... links to pulp::inspect but the target was not found".
if(NOT ANDROID AND NOT IOS AND PULP_ENABLE_GPU)
    add_subdirectory(tools/cli)
endif()

# ── Rust CLI (Phase 8 binary swap, issues #686 / #767) ────────────────────
# Bridges `cargo build` for the experimental Rust CLI port into the
# CMake build graph. Coexists with pulp-cli (the C++ binary) — both
# install side-by-side under `${CMAKE_INSTALL_BINDIR}`. PR #2 of the
# swap pair flips the user-facing default; this file is the no-op
# build-graph scaffolding (PR #1).
#
# Mirror the EXACT desktop guard used for tools/cli above — both the
# `NOT ANDROID AND NOT IOS` mobile-skip AND the `PULP_ENABLE_GPU`
# desktop-CLI-lane guard. Without the GPU guard, sanitizer-style
# GPU-off jobs still pull cargo into the default `all` build whenever
# cargo happens to be on PATH, introducing unrelated Rust-toolchain
# / network failures (and minutes of cargo build time) in lanes that
# intentionally disable the CLI/GPU paths. Codex review on PR #778
# (CMakeLists:523) flagged the missing guard as a P2.
if(NOT ANDROID AND NOT IOS AND PULP_ENABLE_GPU
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/experimental/pulp-rs/CMakeLists.txt")
    add_subdirectory(experimental/pulp-rs)
endif()

# ── MCP Server (desktop only) ─────────────────────────────────────────────
if(NOT ANDROID AND NOT IOS)
    add_subdirectory(tools/mcp)
endif()

# ── Screenshot Tool ────────────────────────────────────────────────────────
add_subdirectory(tools/screenshot)

# ── Out-of-process plugin scan worker (workstream 03 slice 3.3b) ──────────
if(NOT IOS AND NOT ANDROID)
    add_subdirectory(tools/scan-worker)
endif()

# ── Design Tool ───────────────────────────────────────────────────────────
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/design/CMakeLists.txt)
    add_subdirectory(tools/design)
endif()

# ── Import Design Tool ────────────────────────────────────────────────────
add_subdirectory(tools/import-design)

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
if(PULP_BUILD_EXAMPLES AND NOT ANDROID)
    add_subdirectory(examples)
endif()

if(PULP_BUILD_TESTS AND NOT ANDROID AND TARGET PulpGain_CLAP)
    foreach(_pulp_clap_host_test IN ITEMS pulp-test-host pulp-test-host-regression)
        if(TARGET ${_pulp_clap_host_test})
            target_compile_definitions(${_pulp_clap_host_test} PRIVATE
                PULP_TEST_CLAP_PATH="${CMAKE_BINARY_DIR}/CLAP/PulpGain.clap")
            add_dependencies(${_pulp_clap_host_test} PulpGain_CLAP)
        endif()
    endforeach()
endif()

# ── Android Shared Library ─────────────────────────────────────────────────
# Android apps load native code via System.loadLibrary("pulp") which expects
# a single libpulp.so shared library. This thin .so links all Pulp static
# libraries and exports the JNI_OnLoad entry point.
if(ANDROID)
    add_library(pulp-jni SHARED
        core/platform/src/android/jni_bridge.cpp
    )
    set_target_properties(pulp-jni PROPERTIES OUTPUT_NAME "pulp")
    target_include_directories(pulp-jni PRIVATE
        ${CMAKE_SOURCE_DIR}/core/platform/include
    )
    target_link_libraries(pulp-jni PRIVATE
        pulp::platform
        pulp::runtime
        pulp::events
        pulp::state
        pulp::audio
        pulp::midi
        pulp::signal
        pulp::format
        pulp::canvas
        pulp::view
        pulp::osc
        pulp::host
        pulp::dsl
        pulp::standalone
        oboe
        android
        log
    )
    # Ensure all static library symbols are included (needed for JNI registration)
    # Ensure JNI exports from static libs are included in the shared lib.
    # Without --whole-archive, the linker strips "unused" JNI_OnLoad and
    # Java_* functions since nothing in the .so itself references them.
    target_link_options(pulp-jni PRIVATE
        -Wl,--whole-archive
        $<TARGET_FILE:pulp-platform>
        $<TARGET_FILE:pulp-audio>
        $<$<TARGET_EXISTS:pulp-render>:$<TARGET_FILE:pulp-render>>
        -Wl,--no-whole-archive)

    # TalkBack accessibility JNI entry points live in pulp-view
    # (core/view/platform/android/accessibility_android.cpp). pulp-view is
    # not whole-archived — whole-archiving it pulls in SDL3's JNI_OnLoad
    # and duplicates ours (see closed PR #148). Instead, force-reference
    # each Java_com_pulp_accessibility_* symbol so the linker keeps the
    # containing object file. This preserves just the JNI exports without
    # dragging SDL3 into the JNI closure. Without this the app hits
    # UnsatisfiedLinkError the first time TalkBack walks the node tree.
    foreach(_a11y_sym IN ITEMS
        Java_com_pulp_accessibility_PulpAccessibilityDelegate_nativeGetAccessibilityNodeCount
        Java_com_pulp_accessibility_PulpAccessibilityDelegate_nativeGetNodeColumnCount
        Java_com_pulp_accessibility_PulpAccessibilityDelegate_nativeGetNodeLabel
        Java_com_pulp_accessibility_PulpAccessibilityDelegate_nativeGetNodeRangeMax
        Java_com_pulp_accessibility_PulpAccessibilityDelegate_nativeGetNodeRangeMin
        Java_com_pulp_accessibility_PulpAccessibilityDelegate_nativeGetNodeRole
        Java_com_pulp_accessibility_PulpAccessibilityDelegate_nativeGetNodeRowCount
        Java_com_pulp_accessibility_PulpAccessibilityDelegate_nativeGetNodeValue
        Java_com_pulp_accessibility_PulpAccessibilityDelegate_nativePerformAction
    )
        target_link_options(pulp-jni PRIVATE "-Wl,--undefined=${_a11y_sym}")
    endforeach()
endif()

# ── SDK Install Rules ──────────────────────────────────────────────────────
# cmake --install build --prefix <staging> produces a complete Pulp SDK
# Android builds use Gradle for packaging — skip CMake install rules.
if(ANDROID)
    return()
endif()
include(GNUInstallDirs)

# Core libraries for SDK packaging. Standalone projects need the same runtime
# surface as repo examples, so the SDK exports the view/standalone layers plus
# the bundled third-party pieces they depend on. Some targets, like
# `pulp-render`, are optional in smoke or non-GPU builds, so only export targets
# that were actually configured in this build tree.
set(PULP_SDK_TARGETS
    pulp-platform pulp-runtime pulp-events pulp-state
    pulp-audio pulp-midi pulp-signal pulp-format
    pulp-canvas pulp-view pulp-host pulp-standalone pulp-dsl
)

if(TARGET pulp-render)
    list(APPEND PULP_SDK_TARGETS pulp-render)
endif()

if(TARGET pulp-inspect)
    list(APPEND PULP_SDK_TARGETS pulp-inspect)
endif()

# pulp-canvas links pulp-bundled-fonts privately when Skia is on (#932).
# CMake exports the canvas target through PulpTargets, and refuses unless
# every PRIVATE-linked dependency is also in the export set.
if(TARGET pulp-bundled-fonts)
    list(APPEND PULP_SDK_TARGETS pulp-bundled-fonts)
endif()

foreach(_sdk_target IN LISTS PULP_SDK_TARGETS)
    string(REPLACE "pulp-" "" _sdk_export_name "${_sdk_target}")
    set_target_properties(${_sdk_target} PROPERTIES EXPORT_NAME "${_sdk_export_name}")
endforeach()

install(TARGETS ${PULP_SDK_TARGETS}
    EXPORT PulpTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Also install third-party targets that our exported targets depend on
if(PULP_HAS_VST3)
    install(TARGETS vst3-sdk
        EXPORT PulpTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION external/vst3sdk
    )
endif()
if(PULP_HAS_CLAP)
    install(TARGETS clap EXPORT PulpTargets)
endif()
if(PULP_HAS_OBOE AND TARGET oboe)
    install(TARGETS oboe
        EXPORT PulpTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()
if(PULP_HAS_LV2)
    install(TARGETS lv2-headers EXPORT PulpTargets)
endif()
if(PULP_HAS_AUSDK)
    install(TARGETS ausdk
        EXPORT PulpTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION external/AudioUnitSDK/include
    )
endif()
if(TARGET yogacore)
    set_target_properties(yogacore PROPERTIES
        EXPORT_NAME yogacore
        INTERFACE_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${yoga_SOURCE_DIR}>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    )
    install(TARGETS yogacore
        EXPORT PulpTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    if(DEFINED yoga_SOURCE_DIR AND EXISTS "${yoga_SOURCE_DIR}/yoga")
        install(DIRECTORY "${yoga_SOURCE_DIR}/yoga"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
    endif()
endif()
if(TARGET hwy)
    install(TARGETS hwy
        EXPORT PulpTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()
foreach(_mbed_target mbedcrypto p256m everest mbedx509 mbedtls)
    if(TARGET ${_mbed_target})
        install(TARGETS ${_mbed_target}
            EXPORT PulpTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif()
endforeach()
if(TARGET SDL3-static)
    set_target_properties(SDL3-static PROPERTIES EXPORT_NAME sdl3-static)
    install(TARGETS SDL3-static
        EXPORT PulpTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()
if(TARGET SDL3_Headers)
    set_target_properties(SDL3_Headers PROPERTIES EXPORT_NAME sdl3-headers)
    install(TARGETS SDL3_Headers EXPORT PulpTargets)
endif()

# Public headers for each SDK subsystem
foreach(subsystem platform runtime events state audio midi signal format canvas render view)
    set(_inc_dir "${CMAKE_CURRENT_SOURCE_DIR}/core/${subsystem}/include")
    if(EXISTS "${_inc_dir}")
        install(DIRECTORY "${_inc_dir}/pulp/"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/pulp"
            FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
        )
    endif()
endforeach()

# SDK version file
file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${PROJECT_VERSION}\n")
install(FILES "${CMAKE_BINARY_DIR}/version.txt" DESTINATION ".")

# SDK build-type marker (pulp-internal task #35).
#
# Records the CMAKE_BUILD_TYPE the SDK was built with so PulpConfig.cmake
# can refuse — or at least loudly warn — when a downstream plugin tries
# to consume a Debug-built Pulp SDK. Debug-built Skia + audio framework
# blow real-time deadlines on every reasonable host, so consumers who
# stumble into this on a local checkout deserve a clear "this SDK
# install is unusable for plugin work" message instead of a silently
# unusable plugin that drops audio.
#
# Multi-config generators (Visual Studio, Xcode) don't have a single
# CMAKE_BUILD_TYPE at configure time; the install step picks one via
# `cmake --install --config <Type>`. In that case CMAKE_BUILD_TYPE is
# empty here and the guard in PulpConfig.cmake.in falls back to
# "unknown" — which it interprets as "trust the user, do not warn".
file(WRITE "${CMAKE_BINARY_DIR}/sdk_build_type.txt"
     "${CMAKE_BUILD_TYPE}\n")
install(FILES "${CMAKE_BINARY_DIR}/sdk_build_type.txt" DESTINATION ".")

# CMake config files for find_package(Pulp)
install(EXPORT PulpTargets
    FILE PulpTargets.cmake
    NAMESPACE Pulp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pulp
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_BINARY_DIR}/PulpConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpConfig.cmake.in"
    "${CMAKE_BINARY_DIR}/PulpConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pulp
)

install(FILES
    "${CMAKE_BINARY_DIR}/PulpConfig.cmake"
    "${CMAKE_BINARY_DIR}/PulpConfigVersion.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpAAX.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpAppIcon.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpEmbedData.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpFonts.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpGenerateMacIcns.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpGenerateWindowsIcon.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpUtils.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpPlugin.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/FindSkia.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpInfoPlist.aax.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpInfoPlist.au.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpInfoPlist.vst3.in"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pulp
)

# pulp_add_binary_data resolves its Python encoder via
# ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/scripts/encode_binary_data.py — i.e.
# adjacent to whichever PulpUtils.cmake is sourced. That works in the
# source tree (tools/cmake/PulpUtils.cmake → tools/cmake/scripts/…) and
# must keep working in the installed tree (lib/cmake/Pulp/PulpUtils.cmake
# → lib/cmake/Pulp/scripts/…). Install the encoder alongside PulpUtils.cmake
# so find_package(Pulp) consumers can use the function (issue-905 follow-up).
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/scripts/encode_binary_data.py"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pulp/scripts
)

# Templates for standalone project scaffolding
install(DIRECTORY tools/templates/
    DESTINATION templates
    PATTERN "*.template"
)
install(DIRECTORY templates/ios-auv3
    DESTINATION templates
)

# Header-only third-party deps referenced by installed public headers
install(DIRECTORY "${choc_SOURCE_DIR}/choc"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

if(DEFINED SDL3_SOURCE_DIR AND EXISTS "${SDL3_SOURCE_DIR}/include/SDL3")
    install(DIRECTORY "${SDL3_SOURCE_DIR}/include/SDL3"
        DESTINATION external/SDL3/include
    )
    if(EXISTS "${SDL3_SOURCE_DIR}/include/build_config")
        install(DIRECTORY "${SDL3_SOURCE_DIR}/include/build_config"
            DESTINATION external/SDL3/include
        )
    endif()
endif()
if(DEFINED SDL3_BINARY_DIR AND EXISTS "${SDL3_BINARY_DIR}/include-revision/SDL3")
    install(DIRECTORY "${SDL3_BINARY_DIR}/include-revision/SDL3"
        DESTINATION external/SDL3/include
    )
endif()

# Format SDK headers (CLAP, LV2 — MIT/ISC compatible)
if(PULP_HAS_CLAP)
    install(DIRECTORY "${clap_SOURCE_DIR}/include/clap"
        DESTINATION external/clap/include
    )
endif()
if(PULP_HAS_LV2)
    install(DIRECTORY "${lv2_SOURCE_DIR}/include/lv2"
        DESTINATION external/lv2/include
    )
endif()
if(PULP_HAS_VST3)
    install(DIRECTORY
        "${VST3_SDK_DIR}/base"
        "${VST3_SDK_DIR}/pluginterfaces"
        "${VST3_SDK_DIR}/public.sdk"
        DESTINATION external/vst3sdk
        FILES_MATCHING PATTERN "*.h"
    )
    install(FILES
        "${VST3_SDK_DIR}/public.sdk/source/main/dllmain.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/main/linuxmain.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/main/macmain.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/main/moduleinit.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/main/pluginfactory.cpp"
        DESTINATION external/vst3sdk/public.sdk/source/main
    )
endif()
if(PULP_HAS_AUSDK)
    install(DIRECTORY "${AUSDK_DIR}/include/AudioUnitSDK"
        DESTINATION external/AudioUnitSDK/include
    )
endif()

# Helper sources needed by installed-SDK plugin targets. These are shared by
# the unified PulpUtils.cmake implementation when it is used from find_package.
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/aax_runtime.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/au_adapter.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/au_audio_unit.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/au_entry.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/au_v2_cocoa_view.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/au_view_controller_ios.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/vst3_plug_view.cpp"
    DESTINATION src/pulp/format
)

if(PULP_HAS_WEBGPU AND DEFINED WEBGPU_RUNTIME_LIB AND TARGET webgpu)
    install(IMPORTED_RUNTIME_ARTIFACTS webgpu
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )

    get_filename_component(_pulp_webgpu_lib_dir "${WEBGPU_RUNTIME_LIB}" DIRECTORY)
    get_filename_component(_pulp_webgpu_root "${_pulp_webgpu_lib_dir}" DIRECTORY)

    if(DEFINED FETCHCONTENT_SOURCE_DIR_WEBGPU
       AND EXISTS "${FETCHCONTENT_SOURCE_DIR_WEBGPU}/wgpu-native/include")
        install(DIRECTORY "${FETCHCONTENT_SOURCE_DIR_WEBGPU}/wgpu-native/include/"
            DESTINATION external/webgpu/include
        )
    endif()

    if(EXISTS "${_pulp_webgpu_root}/include")
        install(DIRECTORY "${_pulp_webgpu_root}/include/"
            DESTINATION external/webgpu/include
        )
    endif()

    # Install the wgpu_native import library alongside the runtime DLL.
    #
    # The WebGPU-distribution CMake sets WGPU_IMPLIB as a local variable
    # inside FetchWgpuNativePrecompiled.cmake, so by the time execution
    # returns to this file the variable is out of scope — the original
    # guard `if(DEFINED WGPU_IMPLIB ...)` always evaluated to FALSE and
    # the .lib was silently skipped from the SDK. That's why scaffolded
    # Windows plugins fail with "IMPORTED_IMPLIB not set for imported
    # target 'webgpu'" (see #94).
    #
    # Read the path from the imported target directly. Prefer the
    # config-specific property (some WebGPU-distribution versions only
    # populate IMPORTED_IMPLIB_RELEASE) and fall back to the plain
    # IMPORTED_IMPLIB.
    if(WIN32)
        set(_pulp_wgpu_implib_src "")
        get_target_property(_pulp_wgpu_implib_release webgpu IMPORTED_IMPLIB_RELEASE)
        get_target_property(_pulp_wgpu_implib_plain webgpu IMPORTED_IMPLIB)
        if(_pulp_wgpu_implib_release AND EXISTS "${_pulp_wgpu_implib_release}")
            set(_pulp_wgpu_implib_src "${_pulp_wgpu_implib_release}")
        elseif(_pulp_wgpu_implib_plain AND EXISTS "${_pulp_wgpu_implib_plain}")
            set(_pulp_wgpu_implib_src "${_pulp_wgpu_implib_plain}")
        elseif(DEFINED WGPU_IMPLIB AND EXISTS "${WGPU_IMPLIB}")
            # Legacy fallback for older WebGPU-distribution versions.
            set(_pulp_wgpu_implib_src "${WGPU_IMPLIB}")
        endif()
        if(_pulp_wgpu_implib_src)
            install(FILES "${_pulp_wgpu_implib_src}"
                DESTINATION ${CMAKE_INSTALL_LIBDIR}
            )
        else()
            message(WARNING
                "Pulp: could not locate wgpu_native.lib for the Windows SDK "
                "install. Downstream plugins will fail to link against webgpu. "
                "Check that the webgpu imported target has IMPORTED_IMPLIB set.")
        endif()
        unset(_pulp_wgpu_implib_src)
        unset(_pulp_wgpu_implib_release)
        unset(_pulp_wgpu_implib_plain)
    endif()
endif()

if(PULP_HAS_SKIA)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/external/skia-build"
        DESTINATION external
    )
endif()
