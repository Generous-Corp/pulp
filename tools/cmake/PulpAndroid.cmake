# PulpAndroid.cmake — Android NDK toolchain integration
#
# Configures Oboe audio, ADPF performance hints, Android MIDI, JNI bridge,
# and platform-specific sources when building for Android.
#
# The NDK toolchain file sets ANDROID=TRUE and provides the cross-compilation
# environment. This module layers Pulp-specific configuration on top.

if(NOT ANDROID)
    return()
endif()

message(STATUS "Pulp: Configuring Android platform (API ${ANDROID_NATIVE_API_LEVEL})")

# ── Minimum API Level ──────────────────────────────────────────────────────
# AAudio requires API 26. We enforce this as the floor.
if(ANDROID_NATIVE_API_LEVEL LESS 26)
    message(WARNING "Pulp: ANDROID_NATIVE_API_LEVEL (${ANDROID_NATIVE_API_LEVEL}) < 26. "
                    "Forcing to 26 for AAudio support.")
    set(ANDROID_NATIVE_API_LEVEL 26)
endif()

# ── NEON SIMD ──────────────────────────────────────────────────────────────
# Highway uses NEON on ARM. Ensure it's enabled.
if(NOT DEFINED ANDROID_ARM_NEON OR NOT ANDROID_ARM_NEON)
    set(ANDROID_ARM_NEON TRUE)
endif()

# ── Oboe — Google's C++ audio library (Apache 2.0) ────────────────────────
# Wraps AAudio (API 26+) with automatic fallback to OpenSL ES.
# Single audio backend for Pulp on Android.
set(OBOE_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    oboe
    GIT_REPOSITORY https://github.com/google/oboe.git
    GIT_TAG 1.9.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(oboe)
# Suppress warnings in third-party Oboe code (NDK 30 Clang is stricter)
if(TARGET oboe)
    target_compile_options(oboe PRIVATE -w)
endif()
set(PULP_HAS_OBOE TRUE)
message(STATUS "Pulp: Oboe audio library enabled (AAudio-first)")

# ── Android-Specific Compile Definitions ───────────────────────────────────
# These are available to all Pulp targets via the platform library.
# Individual subsystems use these to conditionally compile Android code.
add_compile_definitions(PULP_ANDROID_TARGET=1)

# ── Skip Desktop-Only Dependencies on Android ─────────────────────────────
# VST3, AU, LV2 plugin formats are not supported on Android.
# ALSA, JACK, CoreAudio are desktop-only audio backends.
# These are handled by the individual CMakeLists.txt files via ANDROID guards.

# ── Function: Wire Android Sources Into Subsystem Targets ─────────────────
# Called after add_subdirectory() for each subsystem, so targets exist.
function(pulp_wire_android_sources)
    if(NOT ANDROID)
        return()
    endif()

    # -- Audio: Oboe device + ADPF performance hints --
    if(TARGET pulp-audio)
        set(_android_audio_dir "${CMAKE_SOURCE_DIR}/core/audio/platform/android")
        if(EXISTS "${_android_audio_dir}/oboe_device.cpp")
            target_sources(pulp-audio PRIVATE
                ${_android_audio_dir}/oboe_device.cpp
            )
        endif()
        if(EXISTS "${_android_audio_dir}/adpf_hints.cpp")
            target_sources(pulp-audio PRIVATE
                ${_android_audio_dir}/adpf_hints.cpp
            )
        endif()
        if(EXISTS "${_android_audio_dir}/tone_generator.cpp")
            target_sources(pulp-audio PRIVATE
                ${_android_audio_dir}/tone_generator.cpp
            )
        endif()
        if(EXISTS "${_android_audio_dir}/demo_synth.cpp")
            target_sources(pulp-audio PRIVATE
                ${_android_audio_dir}/demo_synth.cpp
            )
        endif()
        target_link_libraries(pulp-audio PRIVATE oboe)
        # Link android lib for ADPF (APerformanceHint*) and ANativeWindow
        target_link_libraries(pulp-audio PRIVATE android log)
        # Platform JNI headers needed by tone_generator.cpp
        target_include_directories(pulp-audio PRIVATE
            ${CMAKE_SOURCE_DIR}/core/platform/include
        )
    endif()

    # -- MIDI: Android MIDI API --
    if(TARGET pulp-midi)
        set(_android_midi_dir "${CMAKE_SOURCE_DIR}/core/midi/platform/android")
        if(EXISTS "${_android_midi_dir}/android_midi.cpp")
            target_sources(pulp-midi PRIVATE
                ${_android_midi_dir}/android_midi.cpp
            )
        endif()
        # AMidi NDK library — only available at API 29+. For minSdk 26, we use
        # the Java MIDI API via JNI instead. Only link amidi if building for API 29+.
        if(ANDROID_NATIVE_API_LEVEL GREATER_EQUAL 29)
            target_link_libraries(pulp-midi PRIVATE amidi)
        endif()
        target_link_libraries(pulp-midi PRIVATE log)
    endif()

    # -- Platform: permissions, lifecycle, file provider, clipboard --
    # Note: jni_bridge.cpp is NOT added here — it's compiled into the
    # pulp-jni shared library target instead (see root CMakeLists.txt).
    if(TARGET pulp-platform)
        set(_android_plat_dir "${CMAKE_SOURCE_DIR}/core/platform/src/android")
        file(GLOB _android_plat_sources "${_android_plat_dir}/*.cpp")
        list(FILTER _android_plat_sources EXCLUDE REGEX "jni_bridge\\.cpp$")
        if(_android_plat_sources)
            target_sources(pulp-platform PRIVATE ${_android_plat_sources})
        endif()
        target_link_libraries(pulp-platform PRIVATE android log)
        target_include_directories(pulp-platform PUBLIC
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/core/platform/include>
        )
    endif()

    # -- Render: Vulkan backend + AChoreographer + View hierarchy --
    if(TARGET pulp-render AND PULP_ENABLE_GPU)
        set(_android_render_dir "${CMAKE_SOURCE_DIR}/core/render/platform/android")
        if(EXISTS "${_android_render_dir}/choreographer_android.cpp")
            target_sources(pulp-render PRIVATE
                ${_android_render_dir}/choreographer_android.cpp
            )
        endif()
        if(EXISTS "${_android_render_dir}/gpu_surface_android.cpp")
            target_sources(pulp-render PRIVATE
                ${_android_render_dir}/gpu_surface_android.cpp
            )
        endif()
        target_link_libraries(pulp-render PRIVATE android log)
        # View headers needed for rendering the widget hierarchy on Android
        target_include_directories(pulp-render PRIVATE
            ${CMAKE_SOURCE_DIR}/core/view/include
            ${CMAKE_SOURCE_DIR}/core/platform/include
            ${CMAKE_SOURCE_DIR}/core/signal/include
            ${CMAKE_SOURCE_DIR}/core/state/include
            ${CMAKE_SOURCE_DIR}/core/audio/include
            ${CMAKE_SOURCE_DIR}/core/events/include
            ${CMAKE_SOURCE_DIR}/core/midi/include
            ${CMAKE_SOURCE_DIR}/core/format/include
            ${CMAKE_SOURCE_DIR}/core/runtime/include
            ${CMAKE_SOURCE_DIR}/core/canvas/include
            ${CMAKE_SOURCE_DIR}/external/choc
        )
        target_compile_definitions(pulp-render PRIVATE PULP_VULKAN_BACKEND=1)
    endif()

    message(STATUS "Pulp: Android platform sources wired")
endfunction()
