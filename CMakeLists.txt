cmake_minimum_required(VERSION 3.24)
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpMacOsArch.cmake)  # macOS arch knob (PULP_MACOS_ARCH) — before PulpMinOs/project

# Pin the macOS deployment target from tools/deps/min_os.json BEFORE project(),
# so Pulp's binaries target the max minimum of the prebuilt libs they link
# (Skia/Dawn always; V8 only under PULP_JS_ENGINE=v8) instead of drifting with
# the build machine's SDK. No-op when a toolchain/user already set it.
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpMinOs.cmake)

project(Pulp
    VERSION 0.780.0
    DESCRIPTION "Cross-platform audio plugin and application framework"
    HOMEPAGE_URL "https://github.com/Generous-Corp/pulp"
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
option(PULP_ENABLE_PROJECT_PACKAGE
       "Build and export the durable project-package subsystem and dependent C++ timeline authoring tools"
       ON)
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
# Tier-0 Intel canary. When ON, a sub-second static lint runs at configure
# time and FAILS the configure on any of five cross-arch (x86_64) portability
# regressions (raw NEON outside a guard, ARM-gated SIMD with no x86 fallback,
# hardcoded darwin-arm64/aarch64 asset strings, CMAKE_SYSTEM_PROCESSOR driving
# an Apple target-arch decision, hardcoded CMAKE_OSX_ARCHITECTURES=arm64).
# Default OFF so external cloners who never ship Intel pay nothing; pulp opts
# in via the `PULP_INTEL_CANARY` repo variable in CI (mirrors the
# PULP_REQUIRE_GPU_FOR_SDK "set ON in CI" convention above). This is a canary,
# not a substitute for the Tier-2 nightly Intel build — see
# docs/guides/intel-support.md for the honest catch/miss list.
option(PULP_INTEL_CANARY
       "Cross-arch (Intel) portability lint at configure time. Default OFF; \
        pulp sets it ON via a repo variable. See docs/guides/intel-support.md."
       OFF)
if(PULP_INTEL_CANARY)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(NOT Python3_Interpreter_FOUND)
        find_program(Python3_EXECUTABLE NAMES python3 python)
    endif()
    if(NOT Python3_EXECUTABLE)
        message(FATAL_ERROR
            "PULP_INTEL_CANARY=ON but no python3 interpreter was found to run "
            "tools/scripts/intel_canary_lint.py.")
    endif()
    execute_process(
        COMMAND "${Python3_EXECUTABLE}"
                "${PULP_ROOT_DIR}/tools/scripts/intel_canary_lint.py"
                --mode=tree --root "${PULP_ROOT_DIR}"
        RESULT_VARIABLE _pulp_intel_canary_rc
        OUTPUT_VARIABLE _pulp_intel_canary_out
        ERROR_VARIABLE _pulp_intel_canary_err)
    if(NOT _pulp_intel_canary_rc EQUAL 0)
        message(FATAL_ERROR
            "PULP_INTEL_CANARY: cross-arch portability lint failed:\n"
            "${_pulp_intel_canary_out}${_pulp_intel_canary_err}")
    endif()
    message(STATUS "Pulp: Intel canary lint clean.")
endif()
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
# Plugin packaging model — the centralized single-vs-bundle default.
# `single` (the default) = one plugin per binary via pulp_add_plugin. `bundle`
# = one binary hosting N plugins via pulp_add_plugin_bundle (Expert Sleepers
# Silent Way style). This is an opinionated default: a project opts into
# bundling in ONE place rather than per-macro. It records intent and is surfaced
# in `pulp status`; the actual choice is which CMake helper a target calls, so
# both models stay fully supported regardless. See
# docs/reference/opinionated-defaults.md and docs/guides/plugin-bundles.md.
set(PULP_PLUGIN_PACKAGING "single" CACHE STRING
    "Default plugin packaging model: 'single' (one plugin per binary) or 'bundle' (N plugins per binary).")
set_property(CACHE PULP_PLUGIN_PACKAGING PROPERTY STRINGS single bundle)
if(NOT PULP_PLUGIN_PACKAGING STREQUAL "single" AND NOT PULP_PLUGIN_PACKAGING STREQUAL "bundle")
    message(FATAL_ERROR "PULP_PLUGIN_PACKAGING must be 'single' or 'bundle', got '${PULP_PLUGIN_PACKAGING}'.")
endif()

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
# Optional development-inspector SDK components. This controls build presence
# only: protocol/client/runtime libraries and the desktop visual overlay are
# available when ON, while every network endpoint still defaults to runtime
# profile `off`. Ordinary plugin targets never link these components.
option(PULP_ENABLE_INSPECTOR
       "Build optional development-inspector SDK components"
       ON)
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
option(PULP_ENABLE_ABLETON_LINK "Enable optional desktop Ableton Link support with a developer-supplied SDK" OFF)
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
set(PULP_ABLETON_LINK_SDK_DIR "" CACHE PATH "Path to a developer-supplied desktop Ableton/link SDK kept outside the Pulp source tree")
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

    # Two designated-initializer diagnostics promoted to ERRORS, because the
    # class of defect they catch is one GCC rejects outright while clang only
    # warns — and Pulp's REQUIRED gate is macOS/clang.
    #
    # This is not hypothetical. A duplicate `.custom_latency_for` designator in
    # `signal_graph.cpp` reached main: clang emitted BOTH of these warnings on
    # the required gate and nobody reads a green job's log, while the Linux job
    # (the only GCC lane, and advisory) failed and was merged past. The defect
    # was detected twice, displayed twice, and shipped anyway.
    #
    # Promoting them costs nothing — clang already emits them by default, so
    # this cannot fail on anything that was not already a real defect — and it
    # moves the catch onto the gate that actually blocks, at compile time,
    # instead of relying on someone reading a warning or on an advisory lane's
    # verdict being honoured.
    #
    # clang-only: GCC spells the override diagnostic `-Woverride-init` and
    # rejects duplicate designators as a hard error regardless, so it needs no
    # help here.
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options(
            "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-Werror=reorder-init-list>"
            "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-Werror=initializer-overrides>")
    endif()
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
# Coverage-guided fuzz targets via PULP_ENABLE_FUZZING=ON. Off by default
# because -fsanitize=fuzzer needs a compiler runtime Apple's clang does not
# ship; the properties those targets check are also driven by deterministic
# replays in the normal test suite, which need no special toolchain.
option(PULP_ENABLE_FUZZING "Build libFuzzer targets (requires a libFuzzer runtime)" OFF)
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/Sanitizers.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpInstrumentation.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpAAX.cmake)
pulp_configure_aax_sdk()
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpAbletonLink.cmake)
pulp_configure_ableton_link_sdk()

# ── Third-Party Dependencies ────────────────────────────────────────────────
# Shared dependency setup lives in tools/cmake/PulpDependencies.cmake.
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpDependencies.cmake)
# After PulpDependencies: the runtime-staging entry point DELEGATES the wgpu
# copy to the upstream FetchContent package's target_copy_webgpu_binaries(),
# so that function has to exist first.
include(${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpRuntimeStaging.cmake)

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
add_subdirectory(core/timebase)
add_subdirectory(core/timeline)
if(PULP_ENABLE_PROJECT_PACKAGE)
    add_subdirectory(core/project_package)
else()
    message(STATUS
        "Pulp: project-package disabled; atomic Timeline authoring targets and "
        "the corresponding pulp-cpp/pulp-mcp commands will not be built")
endif()
add_subdirectory(core/timeline_agent_view)
add_subdirectory(core/timeline_editor)
add_subdirectory(core/playback)
add_subdirectory(core/events)
add_subdirectory(core/sample_bank_manifest)
add_subdirectory(core/state)
add_subdirectory(core/audio)
add_subdirectory(core/interchange)
add_subdirectory(core/dawproject)
add_subdirectory(core/smf)
add_subdirectory(core/midi)
add_subdirectory(core/signal)
add_subdirectory(core/graph)
add_subdirectory(core/format)
add_subdirectory(core/sequence)
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
# The arranger rung sits above both the editor kernel and the view stack, so it
# is added after core/view rather than beside core/timeline_editor.
add_subdirectory(core/timeline_view)
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
    core/format/src/standalone_musical_typing.cpp
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

# The inspector protocol/session/client foundation is CPU-only and installable
# when the optional SDK component is requested. inspect/CMakeLists.txt adds the
# GPU/view overlay target only where that stack exists.
if(PULP_ENABLE_INSPECTOR)
    add_subdirectory(inspect)
endif()

# The visual Cmd+I overlay belongs only to the standalone authoring target.
# Keeping this dependency off pulp-format is what prevents ordinary plugin
# targets from inheriting inspector archives through the format SDK.
if(PULP_ENABLE_INSPECTOR AND TARGET pulp::inspect AND NOT IOS)
    target_sources(pulp-standalone PRIVATE
        core/format/src/standalone_inspector.cpp)
    target_link_libraries(pulp-standalone PRIVATE pulp::inspect)
    target_compile_definitions(pulp-standalone PRIVATE
        PULP_ENABLE_INSPECTOR=1
        PULP_HAS_INSPECT=1)
else()
    target_compile_definitions(pulp-standalone PRIVATE
        PULP_ENABLE_INSPECTOR=0)
endif()
add_subdirectory(ship)
# tools/audio (offline Audio Doctor CLI) is a dev tool — only when top-level.
# tools/timeline publishes through core/project_package, so the target does not
# exist in a package-stripped build.
if(PROJECT_IS_TOP_LEVEL)
    add_subdirectory(tools/audio)
    add_subdirectory(tools/design-ab)
    if(PULP_ENABLE_PROJECT_PACKAGE)
        add_subdirectory(tools/timeline)
    endif()
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
    include("${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpCatch.cmake")
    set(_CATCH_DISCOVER_TESTS_SCRIPT
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpCatchAddTests.cmake"
        CACHE INTERNAL "Pulp path to CatchAddTests.cmake helper file" FORCE)
    add_subdirectory(test)
endif()

# ── CMake Module Path (for pulp_add_plugin, pulp_add_app) ──────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake")
include(PulpUtils)

# Inbound link-floor check. Reads the resolved link graph, so it has to be
# included after the module targets exist and before the consumers that assert
# a bound over them (examples/, tools/).
include(PulpLinkFloor)

# ── CLI Tool (desktop only) ────────────────────────────────────────────────
# The CLI already requires the desktop GPU stack for render/audio authoring
# commands. Inspector commands use the CPU-only client/authoring targets when
# the optional component exists and compile explicit unavailable stubs when it
# does not; the CLI never links the visual pulp::inspect overlay. Its optional
# Timeline authoring commands follow PULP_ENABLE_PROJECT_PACKAGE inside tools/cli.
if(NOT ANDROID AND NOT IOS AND PULP_ENABLE_GPU AND PROJECT_IS_TOP_LEVEL)
    add_subdirectory(tools/cli)
endif()

# ── Rust CLI (experimental desktop bridge) ────────────────────────────────
# Bridges `cargo build` for the experimental Rust CLI port into the
# CMake build graph. Coexists with pulp-cli (the C++ binary) — both
# install side-by-side under `${CMAKE_INSTALL_BINDIR}`.
#
# Mirror the desktop/GPU guard used for tools/cli above. The Rust binary itself
# has no link dependency on pulp::tool-timeline or project-package: delegated
# commands already degrade explicitly when the optional pulp-cpp sibling is
# absent, so a package-stripped build still keeps the unrelated native Rust CLI.
# PROJECT_IS_TOP_LEVEL: Pulp's own experimental CLI builds only when Pulp is the
# top-level project. A consumer that vendors Pulp via add_subdirectory() wants the
# libraries + pulp_add_plugin(), not Pulp's dev CLIs.
if(NOT ANDROID AND NOT IOS AND PULP_ENABLE_GPU AND PROJECT_IS_TOP_LEVEL
   AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/experimental/pulp-rs/CMakeLists.txt")
    add_subdirectory(experimental/pulp-rs)
endif()

# ── MCP Server (desktop only) ─────────────────────────────────────────────
# pulp-mcp keeps its package-independent tools in package-stripped builds; its
# optional Timeline registrations follow PULP_ENABLE_PROJECT_PACKAGE internally.
# Dev tools build only when Pulp is top-level (see PROJECT_IS_TOP_LEVEL).
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
#
# Also gated on PULP_ENABLE_DESIGN_IMPORT: this tool links the `pulp::view`
# design-IR sources, and those are themselves behind that option (see
# core/view/CMakeLists.txt). Adding the executable while its implementation is
# compiled out leaves the `all` target unlinkable — which broke the option's
# own documented "release/ship OFF" configuration. The tool wraps the
# subsystem, so building it without the subsystem has no meaning.
if(PROJECT_IS_TOP_LEVEL AND PULP_ENABLE_DESIGN_IMPORT)
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
    foreach(_pulp_clap_host_test IN ITEMS
            pulp-test-host
            pulp-test-host-regression
            pulp-test-timeline-graph-binding)
        if(TARGET ${_pulp_clap_host_test})
            target_compile_definitions(${_pulp_clap_host_test} PRIVATE
                PULP_TEST_CLAP_PATH="${CMAKE_BINARY_DIR}/CLAP/PulpGain.clap")
            add_dependencies(${_pulp_clap_host_test} PulpGain_CLAP)
        endif()
    endforeach()

    if(TARGET pulp-cli AND TARGET PulpTone_CLAP)
        add_dependencies(pulp-cli PulpGain_CLAP PulpTone_CLAP)
        add_test(
            NAME plugin-lab-clap-roundtrip
            COMMAND ${CMAKE_COMMAND}
                -DCLI=$<TARGET_FILE:pulp-cli>
                -DEFFECT=${CMAKE_BINARY_DIR}/CLAP/PulpGain.clap
                -DINSTRUMENT=${CMAKE_BINARY_DIR}/CLAP/PulpTone.clap
                -DOUTPUT_DIR=${CMAKE_BINARY_DIR}/plugin-lab-test
                -P ${CMAKE_SOURCE_DIR}/test/cmake/plugin_lab_clap_roundtrip.cmake)
        set_tests_properties(plugin-lab-clap-roundtrip PROPERTIES
            LABELS "validation;clap;cli;plugin-lab"
            TIMEOUT 60)
    endif()
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
    include("${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpCollectTargets.cmake")
    pulp_collect_targets_recursive(
        _all_executables
        "${PULP_ROOT_DIR}"
        TYPES EXECUTABLE
    )
    foreach(_exec ${_all_executables})
        pulp_link_fontconfig_after_skia(${_exec})
    endforeach()
endif()
