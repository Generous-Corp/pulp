# PulpDependencies.cmake — third-party dependency setup for the Pulp
# in-tree build.
#
# Extracted from the root CMakeLists.txt in the 2026-05 Phase 3 (C2)
# refactor. Owns FetchContent declarations, optional-SDK probing
# (VST3 / AU / CLAP / LV2 / AAX / WebGPU / Skia), and CHOC inclusion.
#
# Called once from the root CMakeLists.txt after platform detection
# and options are set; produces the cache variables and IMPORTED
# targets the core subsystems depend on. Mutates PULP_HAS_* feature
# flags so downstream subsystems can branch on availability.
#
# Codex C2 risk callout: configure-output + installed SDK manifest
# must be byte-stable before/after — every line preserved verbatim
# from the original location (L107-435 of CMakeLists.txt at
# d1df9788a).

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

# pulp-internal #62 — LOUD perf-regression guard. PULP_ENABLE_GPU=ON
# but no Skia found silently falls back to CoreGraphics CPU rendering.
# That fallback is ~5-10× slower than Skia/Dawn for live JS-driven UIs
# (Spectr's editor.js drag went from 7% CPU on Skia to 100% CPU on CG
# fallback — 2026-05-13 regression). The fallback is the right
# behavior for offline tooling and CI bot builds that don't render
# windows, but for ANY local interactive developer session it produces
# a UX that looks broken with no obvious build-output explanation.
#
# Print a screaming WARNING banner whenever GPU is requested but
# Skia is not linked, with concrete instructions on how to recover.
# The release path keeps the hard FATAL_ERROR below
# (PULP_REQUIRE_GPU_FOR_SDK); local dev gets the loud warning instead
# so an incremental rebuild without skia-build still works.
if(PULP_ENABLE_GPU AND NOT PULP_HAS_SKIA)
    message(WARNING
        "\n"
        "  ╔══════════════════════════════════════════════════════════════════════╗\n"
        "  ║  PERF REGRESSION RISK — Skia is NOT linked into this build           ║\n"
        "  ║                                                                      ║\n"
        "  ║  PULP_ENABLE_GPU=ON but FindSkia.cmake could not locate libskia.a    ║\n"
        "  ║  at the configured SKIA_DIR ('${SKIA_DIR}').                          \n"
        "  ║                                                                      ║\n"
        "  ║  Live windows (pulp-design-tool, pulp-ui-preview, pulp-screenshot    ║\n"
        "  ║  with use_gpu=true) will silently fall back to CoreGraphics CPU      ║\n"
        "  ║  rendering — ~5-10× slower for animated / JS-driven UIs.             ║\n"
        "  ║                                                                      ║\n"
        "  ║  Recover:                                                            ║\n"
        "  ║    1. Run tools/build-skia.sh (or fetch the pinned tarball) to       ║\n"
        "  ║       populate external/skia-build/build/<platform>-gpu/lib/Release/ ║\n"
        "  ║    2. Or pass -DSKIA_DIR=/abs/path/to/external/skia-builder when     ║\n"
        "  ║       the libs live at external/skia-builder/build/<plat>-gpu/...    ║\n"
        "  ║    3. Or pass -DPULP_ENABLE_GPU=OFF if you genuinely want a          ║\n"
        "  ║       CoreGraphics-only build (offline tooling, CI bots).            ║\n"
        "  ╚══════════════════════════════════════════════════════════════════════╝")
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

# Phase iOS-D.1 — `PULP_REQUIRE_GPU_FOR_SDK=ON + PULP_ENABLE_GPU=OFF` is a
# misconfiguration: the consumer asked the release lane to enforce GPU but
# then disabled it. Without this guard the configure succeeded, the
# resulting tarball silently #ifdef'd out the GPU code path, and downstream
# consumers got the same CG-only fallback the option exists to prevent —
# defeating the whole purpose of the flag. Fail loudly at configure time.
# (planning/2026-05-28-ios-d-gpu-auv3-crosscheck.md, hard-fail-on-CPU rule.)
if(PULP_REQUIRE_GPU_FOR_SDK AND NOT PULP_ENABLE_GPU)
    message(FATAL_ERROR
        "PULP_REQUIRE_GPU_FOR_SDK=ON but PULP_ENABLE_GPU=OFF — these "
        "options contradict each other. PULP_REQUIRE_GPU_FOR_SDK exists to "
        "guarantee the SDK release ships with the GPU code path linked, "
        "and PULP_ENABLE_GPU=OFF #ifdef's that exact path out. Either set "
        "PULP_ENABLE_GPU=ON (and let the existing FATAL_ERROR above catch "
        "a missing Skia archive) or set PULP_REQUIRE_GPU_FOR_SDK=OFF for a "
        "deliberate CG-only build.")
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
# Pinned to the exact SHA of the 1.2.0 release (verified via
# `git ls-remote https://github.com/google/highway.git refs/tags/1.2.0`).
# Pinning the SHA instead of the tag avoids intermittent
# `fatal: invalid reference: 1.2.0` failures when GIT_SHALLOW combines
# with tag-name resolution under load (Pulp #1930). SHA pins are also
# more cache-friendly: FetchContent's UPDATE_DISCONNECTED check sees a
# stable ref and skips the re-fetch on every reconfigure.
set(HWY_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(HWY_ENABLE_CONTRIB OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
pulp_register_fetchcontent_source(highway REF 1.2.0)
FetchContent_Declare(
    highway
    GIT_REPOSITORY https://github.com/google/highway.git
    # 1.2.0 release commit
    GIT_TAG 457c891775a7397bdb0376bb1031e6e027af1c48
)
FetchContent_MakeAvailable(highway)
set(PULP_HAS_HIGHWAY TRUE)
message(STATUS "Pulp: Highway SIMD library enabled")

# Mbed TLS (Apache 2.0) — cryptographic primitives (SHA-256, RSA, AES)
# Pinned to the exact SHA for v3.6.2. Keep this as a full clone: CMake's shallow
# FetchContent path can fetch only the remote tip and then fail to checkout this
# non-tip commit during fresh coverage configures.
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    mbedtls
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG 107ea89daaefb9867ea9121002fbbdf926780e98
)
FetchContent_MakeAvailable(mbedtls)
set(PULP_HAS_MBEDTLS TRUE)
message(STATUS "Pulp: Mbed TLS crypto library enabled")

# SheenBidi (Apache-2.0) — Unicode Bidirectional Algorithm implementation.
# Item 6.8 of the 2026-05-24 macOS plugin-authoring plan. Adds a Pulp-owned
# bidi engine independent of system ICU; consumed by
# core/canvas::BidiAnalyzer to derive paragraph-level bidi runs for
# Arabic / Hebrew shaping in non-Skia / non-ICU builds (iOS, headless,
# minimal-toolchain hosts). The Skia-linked path in text_run_planner.cpp
# continues to prefer SkShaper's ICU iterators when available; SheenBidi
# is the canonical fallback and the always-available unit-test surface.
pulp_register_fetchcontent_source(SheenBidi REF v3.0.0)
set(SB_CONFIG_UNITY ON CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_GENERATOR OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SheenBidi
    GIT_REPOSITORY https://github.com/Tehreer/SheenBidi.git
    GIT_TAG c0aa79da5b5ff44bd2cbd9ce92963f81e4de6b1e  # v3.0.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(SheenBidi)
set(PULP_HAS_SHEENBIDI TRUE)
message(STATUS "Pulp: SheenBidi bidi engine enabled (v3.0.0)")

# yaml-cpp (MIT license) — YAML parser used by the DESIGN.md import pipeline
# Note: yaml-cpp 0.8.0's CMakeLists declares cmake_minimum_required(3.4),
# which CMake 4.x refuses. Set CMAKE_POLICY_VERSION_MINIMUM in the
# subdirectory scope so the dependency configures cleanly.
set(YAML_CPP_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS  OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL      OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
FetchContent_Declare(
    yaml-cpp
    GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
    GIT_TAG 0.8.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(yaml-cpp)
unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)
set(PULP_HAS_YAMLCPP TRUE)
message(STATUS "Pulp: yaml-cpp 0.8.0 available (DESIGN.md import)")

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
