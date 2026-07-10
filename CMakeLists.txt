cmake_minimum_required(VERSION 3.24)

# Pin the macOS deployment target from tools/deps/min_os.json BEFORE project(),
# so Pulp's binaries target the max minimum of the prebuilt libs they link
# (Skia/Dawn always; V8 only under PULP_JS_ENGINE=v8) instead of drifting with
# the build machine's SDK. No-op when a toolchain/user already set it.
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpMinOs.cmake)

project(Pulp
    VERSION 0.643.0
    DESCRIPTION "Cross-platform audio plugin and application framework"
    HOMEPAGE_URL "https://github.com/danielraffel/pulp"
    LANGUAGES CXX C
)

# Pulp's own source-tree root. Use this — NOT CMAKE_SOURCE_DIR — when referencing
# Pulp's own files (tools/cmake helpers, external SDKs, bundled fonts, git dir).
# CMAKE_SOURCE_DIR is the *top-level* project's dir, which is Pulp only when Pulp
# is the top-level build; when Pulp is consumed via add_subdirectory() (e.g. a
# plugin repo vendoring Pulp as a submodule) CMAKE_SOURCE_DIR is the CONSUMER's
# root and those paths don't exist. PULP_ROOT_DIR always resolves to this file's
# directory in both cases. (The find_package(Pulp) path instead keys off
# CMAKE_CURRENT_LIST_DIR in the installed helpers.)
set(PULP_ROOT_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "Pulp source tree root")

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
# Tests default OFF on iOS and Android: both mobile targets ship plugins or
# apps, not a CTest harness, and many tests depend on desktop-only libraries
# (pulp::host, the CLI binary, plugin scanners) that we deliberately skip on
# mobile. Forcing tests OFF on iOS also prevents downstream example
# CMakeLists from invoking `catch_discover_tests` when the Catch2 include
# was never loaded for that target.
if(CMAKE_SYSTEM_NAME STREQUAL "iOS" OR ANDROID)
    option(PULP_BUILD_TESTS "Build unit tests" OFF)
else()
    option(PULP_BUILD_TESTS "Build unit tests" ON)
endif()
option(PULP_BUILD_EXAMPLES "Build example projects" ON)
option(PULP_ENABLE_GPU "Enable GPU rendering (Dawn/Skia)" ON)
option(PULP_ENABLE_SCENE3D "Enable opt-in native no-JS 3D scene/glTF renderer spikes" OFF)
# Opt-in lane for the native-component Rust contract proof: builds a Cargo
# `staticlib` reference core (with an RT-checking global allocator) and links
# the test/test_rust_dsp_ffi.cpp round-trip + RT-safety self-test. OFF by
# default so default builds NEVER require a Rust toolchain (cargo/rustc). The
# C ABI surface itself (core/native-components) always builds without Rust.
option(PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS
       "Build the opt-in Rust staticlib reference core + FFI/RT-safety tests \
        (requires a Rust toolchain). OFF by default; no Rust for default builds."
       OFF)
# SDK-release safety net. When ON, configure FAILS if PULP_ENABLE_GPU
# is set but Skia binaries weren't located. The release
# workflow flips this ON so an SDK tarball can never ship with the GPU
# code path silently #ifdef'd out. Local developer builds leave it OFF
# (the default) so a no-Skia incremental rebuild still configures and
# transparently falls back to the CG path.
option(PULP_REQUIRE_GPU_FOR_SDK
       "Fail configure if PULP_ENABLE_GPU=ON but Skia was not found. \
        Set ON in .github/workflows/release-cli.yml to prevent silently \
        shipping CG-only SDKs."
       OFF)
option(PULP_TEXT_SHAPING "Enable SkParagraph text shaping (HarfBuzz/ICU via Skia). Default: ON when GPU enabled." ON)
# Lottie / Bodymovin JSON animation playback via the bundled skottie module.
# Opt-in (default OFF): builds that don't use it link nothing extra and the
# LottieView/LottieAnimation symbols compile to no-ops. Auto-disabled at
# configure time if the active Skia build ships no skottie module headers.
option(PULP_LOTTIE "Enable Lottie animation playback via the bundled skottie module. Default: OFF." OFF)
# Design-import authoring subsystem (importers, codegen, lock-to-source,
# JSX-lock, runtime design-import). Default ON for dev/examples; release/ship
# builds set OFF to strip the authoring cluster from shipped plugins (smaller
# binary + attack surface). The runtime W3C token pair (importDesignTokens /
# exportDesignTokens) stays compiled regardless (see core/view/src/w3c_tokens.cpp).
# When OFF, WidgetBridge::install_runtime_import_handlers() becomes a no-op.
option(PULP_ENABLE_DESIGN_IMPORT "Compile the design-import authoring subsystem. Default ON; release/ship OFF." ON)
option(PULP_ENABLE_SWIFT
       "Reserved compatibility toggle; currently no effect. SwiftPM and target helpers enable Swift where needed."
       OFF)
if(PULP_ENABLE_SWIFT)
    message(WARNING
        "PULP_ENABLE_SWIFT is reserved and currently has no effect. "
        "Swift support is built through apple/Package.swift and target "
        "helpers such as pulp_add_ios_host_app(), which enable Swift when "
        "Swift sources are present.")
endif()
# Dev inspector (Cmd+I overlay) in the standalone host. Default ON for
# dev/examples builds. Release / standalone-ship builds should configure
# with -DPULP_ENABLE_INSPECTOR=OFF so a shipped standalone app does not
# expose the developer inspector to end users. Plugin formats never link
# the inspector, so this is standalone-only. The inspector also requires
# GPU + desktop (PULP_HAS_INSPECT), which gates linking; this flag gates
# the source-side wiring on top of that.
#
# Authoring-strip policy (§6a): the inspector is the in-plugin authoring /
# MCP-reachable surface. The SDK release build (`pulp` CLI cli_sdk.cpp) and the
# scaffolded project templates default it OFF so shipped plugins don't carry it
# (smaller binary, smaller attack surface). A developer who deliberately wants an
# inspectable / MCP-reachable plugin opts back in with -DPULP_ENABLE_INSPECTOR=ON
# in their own plugin build. The standalone `pulp` CLI and the MCP server are
# separate binaries and are NOT affected by this flag — bundling them alongside a
# plugin distribution is a packaging choice, not a compile-time strip.
option(PULP_ENABLE_INSPECTOR "Wire the dev inspector into the standalone host (dev/examples ON, release-ship OFF; the in-plugin authoring/MCP surface)" ON)
# RT output-boundary audio probe in the standalone host — the data source for
# the dev Audio Inspector window. Default ON for dev/examples; release /
# standalone-ship builds configure -DPULP_ENABLE_AUDIO_PROBES=OFF. The probe's
# analyze_output() is RT-safe (no alloc/lock/FFT); plugin formats never link
# the standalone tap. NOTE: it gates MEMBER declarations in the public
# standalone.hpp, so core/format defines it PUBLIC (consistent for every
# includer) — a PRIVATE def would give the library and its consumers a
# different sizeof(StandaloneApp) (ODR).
option(PULP_ENABLE_AUDIO_PROBES "Wire the RT output-boundary probe into the standalone host (dev/examples ON, release-ship OFF)" ON)
option(PULP_ENABLE_AAX "Enable optional AAX support with a developer-supplied SDK" OFF)
option(PULP_ENABLE_ARA "Enable optional ARA 2.x support with a developer-supplied Celemony SDK" OFF)
option(PULP_ENABLE_MTS_ESP "Enable optional ODDSound MTS-ESP microtuning client integration" OFF)
option(PULP_ENABLE_SCALA_TUNING "Enable optional Scala .scl/.kbm microtuning support via Surge Synth Team tuning-library" OFF)
option(PULP_FETCHCONTENT_UPDATES_DISCONNECTED "Avoid implicit FetchContent updates during configure" ON)
option(PULP_USE_SHARED_FETCHCONTENT_SOURCES "Prefer machine-local shared source caches for FetchContent dependencies" ON)
option(PULP_BENCHMARK "Enable zero-copy benchmark perf counters. Tooling-only; OFF by default so production builds see zero overhead." OFF)
if(PULP_BENCHMARK)
    add_compile_definitions(PULP_BENCHMARK)
endif()
# Opt-in suite that drives real third-party plugin binaries
# (Surge XT / Vital / OB-Xd / Dexed) through the host stack. OFF by default
# so the public CI matrix never depends on third-party download mirrors;
# developers and dedicated lanes flip it on with
#   cmake -DPULP_REAL_PLUGIN_TESTS=ON ...
# and populate the fixture cache with tools/scripts/fetch_real_plugins.py.
option(PULP_REAL_PLUGIN_TESTS "Build the opt-in real-plugin integration suite (item 4.2)" OFF)
set(PULP_SHARED_FETCHCONTENT_SOURCE_DIR "" CACHE PATH "Machine-local shared source cache root for FetchContent dependencies")
set(PULP_AAX_SDK_DIR "" CACHE PATH "Path to a developer-supplied AAX SDK kept outside the Pulp source tree")
set(PULP_ARA_SDK_DIR "" CACHE PATH "Path to a developer-supplied Celemony ARA SDK. Enables ARA 2.x when PULP_ENABLE_ARA=ON.")

# ── Compiler Warnings ──────────────────────────────────────────────────────
# Gate to C/C++/Obj-C(++) compile languages only. Swift's driver rejects
# clang-style `-Wall` / `-Wextra` / `-Wpedantic` (iOS HostApp + any future
# Swift target would otherwise fail with "Driver threw unknown argument:
# '-Wall' without emitting errors"). The `$<COMPILE_LANGUAGE:...>` genex
# scopes warnings to languages that actually understand them.
if(MSVC)
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:/W4>"
        "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:/wd4100>")
else()
    add_compile_options(
        "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-Wall>"
        "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-Wextra>"
        "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-Wpedantic>"
        "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-Wno-unused-parameter>")
endif()

# ── Windows runtime library ────────────────────────────────────────────────
# Statically link the MSVC runtime so redistributed Pulp binaries don't
# require the user to have the matching Visual C++ Redistributable
# installed. Without this, the default is the dynamic /MD runtime and
# end users see "vcruntime140.dll not found" when launching the plugin.
#
# `MultiThreaded` selects /MT (release) or /MTd (debug) via the
# generator expression — these are the static-link variants. CMake
# 3.15+ honors this via CMP0091 (set automatically by our minimum
# version 3.24).
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        CACHE STRING "MSVC runtime library variant (static)" FORCE)
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
# Shared dependency setup lives in tools/cmake/PulpDependencies.cmake.
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpDependencies.cmake)

# ── Android NDK Support ───────────────────────────────────────────────────
if(ANDROID)
    include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpAndroid.cmake)
endif()

# ── Tracing (Perfetto, dev-only, OFF by default) ────────────────────────────
# Defines the pulp::tracing INTERFACE target consumed by core/runtime. When
# PULP_TRACING=OFF (default/shipping) it adds nothing; ON fetches + builds the
# pinned Perfetto amalgamation. Must precede the core subsystems that link it.
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpTracing.cmake)

# ── Core Subsystems ─────────────────────────────────────────────────────────
# Order matters: dependencies first
add_subdirectory(core/platform)
add_subdirectory(core/runtime)
add_subdirectory(core/events)
add_subdirectory(core/state)
add_subdirectory(core/audio)
add_subdirectory(core/midi)
add_subdirectory(core/signal)
add_subdirectory(core/graph)
add_subdirectory(core/format)
add_subdirectory(core/canvas)
if(PULP_ENABLE_SCENE3D)
    add_subdirectory(core/scene)
endif()
# Render must come before view so pulp-view can link pulp-render via if(TARGET)
if(PULP_ENABLE_GPU)
    add_subdirectory(core/render)
endif()
# After core/render so the GPU convolver node can link pulp::render when GPU is
# enabled. The transport itself is GPU-agnostic and builds regardless.
add_subdirectory(core/gpu_audio)
add_subdirectory(core/view)
add_subdirectory(core/osc)
add_subdirectory(core/native-components)
# pulp-host is a plugin HOST library — iOS App Store policy disallows dlopen
# of arbitrary third-party plugins, so iOS builds don't ship hosting at all.
# Skipping the subdirectory also dodges a libc++ availability error on the
# iPhoneSimulator SDK: `scanner_clap.cpp` calls `runtime::log_warn(...)`,
# which `std::format`-instantiates over arithmetic types — including
# `long double to_chars`, marked unavailable on iOS. Keep host support out of
# iOS until the AUv3 validation path has an iOS-safe scanner/link strategy.
if(NOT IOS)
    add_subdirectory(core/host)
endif()
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
# tools/audio (offline Audio Doctor CLI) is a dev tool — only when top-level.
if(PROJECT_IS_TOP_LEVEL)
    add_subdirectory(tools/audio)
endif()

# ── Testing ─────────────────────────────────────────────────────────────────
# Tests are not built on iOS or Android — they assume a desktop POSIX shell,
# linkable hosting libraries (pulp::host scans VST3/CLAP plugins on disk),
# and a CTest harness that doesn't run inside a sandboxed iOS .app. Mobile
# CI runs the test suite on macOS / Linux runners and the iPhoneSimulator
# configure smoke separately.
if(PULP_BUILD_TESTS AND NOT ANDROID AND NOT IOS)
    # The design-import test cluster (~40 targets) links the authoring symbols
    # gated by PULP_ENABLE_DESIGN_IMPORT, and they are interleaved with other
    # view tests. Rather than risk silently un-gating via a stray link, require
    # the option ON to build the test suite. A stripped/shipped build uses
    # PULP_BUILD_TESTS=OFF (and PULP_BUILD_EXAMPLES=OFF); run the full suite with
    # the default PULP_ENABLE_DESIGN_IMPORT=ON.
    if(NOT PULP_ENABLE_DESIGN_IMPORT)
        message(FATAL_ERROR
            "PULP_BUILD_TESTS=ON requires PULP_ENABLE_DESIGN_IMPORT=ON. "
            "For a stripped build set -DPULP_BUILD_TESTS=OFF -DPULP_BUILD_EXAMPLES=OFF.")
    endif()
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
if(NOT ANDROID AND NOT IOS AND PULP_ENABLE_GPU AND PROJECT_IS_TOP_LEVEL)
    add_subdirectory(tools/cli)
endif()

# ── Rust CLI (experimental desktop bridge) ────────────────────────────────
# Bridges `cargo build` for the experimental Rust CLI port into the
# CMake build graph. Coexists with pulp-cli (the C++ binary) — both
# install side-by-side under `${CMAKE_INSTALL_BINDIR}`.
#
# Mirror the EXACT desktop guard used for tools/cli above — both the
# `NOT ANDROID AND NOT IOS` mobile-skip AND the `PULP_ENABLE_GPU`
# desktop-CLI-lane guard. Without the GPU guard, sanitizer-style
# GPU-off jobs still pull cargo into the default `all` build whenever
# cargo happens to be on PATH, introducing unrelated Rust-toolchain
# / network failures (and minutes of cargo build time) in lanes that
# intentionally disable the CLI/GPU paths.
# PROJECT_IS_TOP_LEVEL: Pulp's own experimental CLI builds only when Pulp is the
# top-level project. A consumer that vendors Pulp via add_subdirectory() wants the
# libraries + pulp_add_plugin(), not Pulp's dev CLIs.
if(NOT ANDROID AND NOT IOS AND PULP_ENABLE_GPU AND PROJECT_IS_TOP_LEVEL
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/experimental/pulp-rs/CMakeLists.txt")
    add_subdirectory(experimental/pulp-rs)
endif()

# ── MCP Server (desktop only) ─────────────────────────────────────────────
# Dev tools below build only when Pulp is top-level (see PROJECT_IS_TOP_LEVEL).
if(NOT ANDROID AND NOT IOS AND PROJECT_IS_TOP_LEVEL)
    add_subdirectory(tools/mcp)
endif()

# ── Screenshot Tool ────────────────────────────────────────────────────────
if(PROJECT_IS_TOP_LEVEL)
    add_subdirectory(tools/screenshot)
endif()

# ── Out-of-process plugin scan worker ─────────────────────────────────────
if(NOT IOS AND NOT ANDROID AND PROJECT_IS_TOP_LEVEL)
    add_subdirectory(tools/scan-worker)
endif()

# ── Design Tool ───────────────────────────────────────────────────────────
if(PROJECT_IS_TOP_LEVEL AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/design/CMakeLists.txt)
    add_subdirectory(tools/design)
endif()

# ── Import Design Tool ────────────────────────────────────────────────────
# Dev tool — only when Pulp is the top-level project (see PROJECT_IS_TOP_LEVEL
# note above); an embedding consumer doesn't build Pulp's authoring CLIs.
if(PROJECT_IS_TOP_LEVEL)
    add_subdirectory(tools/import-design)
endif()

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
        ${PULP_ROOT_DIR}/core/platform/include
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
    # not whole-archived — doing so pulls in SDL3's JNI_OnLoad and
    # duplicates ours. Instead, force-reference
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
# Shared SDK install/export rules live in tools/cmake/PulpInstallRules.cmake.
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

    _pulp_collect_executables_recursive(_all_executables "${PULP_ROOT_DIR}")
    foreach(_exec ${_all_executables})
        pulp_link_fontconfig_after_skia(${_exec})
    endforeach()
endif()
