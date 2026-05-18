cmake_minimum_required(VERSION 3.24)

project(Pulp
    VERSION 0.133.1
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
# Extracted to tools/cmake/PulpDependencies.cmake in the 2026-05 Phase 3
# (C2) refactor.
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpDependencies.cmake)

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
        core/platform/src/android/jni_motion.cpp
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
# Extracted to tools/cmake/PulpInstallRules.cmake in the 2026-05 Phase 3
# (C2) refactor.
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpInstallRules.cmake)

# ── Linux fontconfig post-pass ──────────────────────────────────────────
# GNU ld processes static archives left-to-right and won't re-scan.
# `libskia.a(SkFontMgr_fontconfig.o)` references Fc* symbols and
# requires -lfontconfig AFTER it on the link line. Multiple
# incremental fixes (#1986 pulp-cli, #2018 pulp-import-design,
# #2056 _pulp_add_standalone, #2058 canvas INTERFACE) have addressed
# specific targets one by one, but each release-cli cycle reveals a
# new failing target (most recently examples/view-bridge-demo).
#
# This final pass walks every executable defined in the project tree
# and applies `pulp_link_fontconfig_after_skia` so fontconfig appears
# at the END of every consumer's link line. No-op on macOS/Windows/Android.
if(UNIX AND NOT APPLE AND NOT ANDROID)
    include("${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpLinkFontconfig.cmake")

    function(_pulp_collect_executables_recursive out_var dir)
        get_property(_sub_dirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
        get_property(_dir_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
        set(_acc "")
        foreach(_t ${_dir_targets})
            if(TARGET ${_t})
                get_target_property(_ttype ${_t} TYPE)
                if(_ttype STREQUAL "EXECUTABLE")
                    list(APPEND _acc ${_t})
                endif()
            endif()
        endforeach()
        foreach(_s ${_sub_dirs})
            _pulp_collect_executables_recursive(_inner "${_s}")
            list(APPEND _acc ${_inner})
        endforeach()
        set(${out_var} "${_acc}" PARENT_SCOPE)
    endfunction()

    _pulp_collect_executables_recursive(_all_executables "${CMAKE_SOURCE_DIR}")
    foreach(_exec ${_all_executables})
        pulp_link_fontconfig_after_skia(${_exec})
    endforeach()
endif()
