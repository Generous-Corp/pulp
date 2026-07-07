cmake_minimum_required(VERSION 3.24)

# Pulp SDK smoke project.
#
# Verifies a `cmake --install`-produced Pulp SDK is consumable end-to-end
# through find_package(Pulp). The smoke project enumerates every plugin
# format the installed SDK advertises and builds a single probe plugin
# (PulpSDKSmokeProbe) against them.
#
# Two opt-in tightenings keep production SDK packaging checks from
# silently passing when a consumer-required format is missing:
#
#   PULP_SDK_SMOKE_REQUIRE_FORMATS — semicolon-separated list of formats
#       that MUST be present in the installed SDK. Configure-time
#       FATAL_ERROR if any required format is missing. Example for the
#       Chainer release lane:
#         -DPULP_SDK_SMOKE_REQUIRE_FORMATS="Standalone;VST3;CLAP;AU;AUv3"
#       Default empty (permissive) so existing callers don't break.
#
#   PULP_SDK_SMOKE_INCLUDE_AUV3 — when ON on Apple platforms, wires the
#       AUv3 format into the probe (requires PLUGIN_CODE +
#       MANUFACTURER_CODE). Default OFF because not every SDK install
#       intends to ship AUv3 macOS, but ON is the production posture for
#       the Chainer lane and exercises PulpAuv3.cmake's macOS framework /
#       appex / container-app helpers against the installed SDK tree.

if(APPLE)
    project(PulpSDKSmoke LANGUAGES CXX OBJCXX)
    set(CMAKE_OBJCXX_STANDARD 20)
    set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
else()
    project(PulpSDKSmoke LANGUAGES CXX)
endif()

option(PULP_SDK_SMOKE_INCLUDE_AUV3
    "Include AUv3 in the SDK smoke (Apple only; exercises macOS container helper)"
    OFF)
set(PULP_SDK_SMOKE_REQUIRE_FORMATS ""
    CACHE STRING
    "Formats that must be present in the installed SDK (semicolon-separated)")

find_package(Pulp REQUIRED CONFIG)

foreach(_pulp_alias_target
        format signal view standalone gpu-audio)
    if(TARGET "Pulp::${_pulp_alias_target}"
       AND NOT TARGET "pulp::${_pulp_alias_target}")
        message(FATAL_ERROR
            "Pulp SDK smoke: installed SDK exposes Pulp::${_pulp_alias_target} "
            "but not the matching source-tree-compatible "
            "pulp::${_pulp_alias_target} alias. Downstream examples and "
            "templates must be able to switch between add_subdirectory(Pulp) "
            "and find_package(Pulp) without changing link target names.")
    endif()
endforeach()
unset(_pulp_alias_target)

# C++ baseline export probe.
#
# This project intentionally does not set CMAKE_CXX_STANDARD. Pulp public
# headers use C++20-era language/library constructs, so installed SDK targets
# must carry that requirement through their INTERFACE_COMPILE_FEATURES. This
# catches downstream failures where a consumer links only pulp::signal and then
# compiles Pulp headers with the compiler's pre-C++20 default.
if(TARGET Pulp::signal)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/signal_cxx20_export_probe.cpp"
"#include <pulp/signal/fft.hpp>\n"
"\n"
"int main() {\n"
"    pulp::signal::FftT<double> fft;\n"
"    return fft.size();\n"
"}\n")
    add_executable(PulpSDKSmokeSignalCxx20Probe
        "${CMAKE_CURRENT_BINARY_DIR}/signal_cxx20_export_probe.cpp")
    target_link_libraries(PulpSDKSmokeSignalCxx20Probe PRIVATE Pulp::signal)
endif()

set(PULP_SDK_SMOKE_FORMATS)
if(TARGET Pulp::standalone)
    list(APPEND PULP_SDK_SMOKE_FORMATS Standalone)
endif()
if(TARGET Pulp::clap)
    list(APPEND PULP_SDK_SMOKE_FORMATS CLAP)
endif()
if(TARGET Pulp::vst3-sdk)
    list(APPEND PULP_SDK_SMOKE_FORMATS VST3)
endif()
if(APPLE AND TARGET Pulp::ausdk)
    list(APPEND PULP_SDK_SMOKE_FORMATS AU)
endif()
if(PULP_SDK_SMOKE_INCLUDE_AUV3 AND APPLE AND TARGET Pulp::ausdk)
    # AUv3 doesn't have its own imported target; it reuses AU SDK + the
    # PulpAuv3.cmake helpers shipped in lib/cmake/Pulp. Gate it on the
    # AU SDK being installed so the smoke fails loud when AU was
    # filtered out of the SDK.
    list(APPEND PULP_SDK_SMOKE_FORMATS AUv3)
endif()
if((APPLE OR WIN32) AND PULP_HAS_AAX)
    list(APPEND PULP_SDK_SMOKE_FORMATS AAX)
endif()

if(NOT PULP_SDK_SMOKE_FORMATS)
    message(FATAL_ERROR "Pulp SDK smoke project found no supported installed formats")
endif()

# Required-format gate. The Chainer release lane (and any production
# packaging path) should set PULP_SDK_SMOKE_REQUIRE_FORMATS so a silently
# under-packaged SDK fails at configure time instead of producing a
# smoke build that quietly skipped the format the consumer cares about.
if(PULP_SDK_SMOKE_REQUIRE_FORMATS)
    set(_pulp_sdk_smoke_missing "")
    foreach(_required IN LISTS PULP_SDK_SMOKE_REQUIRE_FORMATS)
        if(NOT _required IN_LIST PULP_SDK_SMOKE_FORMATS)
            list(APPEND _pulp_sdk_smoke_missing "${_required}")
        endif()
    endforeach()
    if(_pulp_sdk_smoke_missing)
        message(FATAL_ERROR
            "Pulp SDK smoke: required formats not available in the installed SDK: "
            "${_pulp_sdk_smoke_missing}\n"
            "Detected formats: ${PULP_SDK_SMOKE_FORMATS}\n"
            "Either rebuild the SDK with the missing formats enabled, or drop "
            "them from PULP_SDK_SMOKE_REQUIRE_FORMATS.")
    endif()
endif()

pulp_add_plugin(PulpSDKSmokeProbe
    FORMATS ${PULP_SDK_SMOKE_FORMATS}
    PLUGIN_NAME "PulpSDKSmokeProbe"
    BUNDLE_ID "com.pulp.sdksmoke"
    MANUFACTURER "Pulp"
    VERSION "0.1.0"
    CATEGORY Effect
    PLUGIN_CODE "SdkS"
    MANUFACTURER_CODE "Pulp"
    AAX_PRODUCT_CODE "SdSm"
    AAX_NATIVE_CODE "SdkS"
)

# GPU-audio transport export probe.
#
# find_package(Pulp) must expose Pulp::gpu-audio and its public headers so a
# downstream GPU plugin can drive a GpuAudioTransport. A STATIC probe would only
# prove the header compiles; an EXECUTABLE forces the linker to resolve a symbol
# defined in the archive (GpuAudioTransport::stats(), which lives in
# gpu_audio_transport.cpp, not the header), so a missing target export OR a
# missing header install fails this smoke at configure/build time instead of
# surfacing later as a broken downstream GPU plugin. Guarded because a non-GPU
# or minimally-configured SDK need not carry the transport.
if(TARGET Pulp::gpu-audio)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gpu_audio_export_probe.cpp"
"#include <pulp/gpu_audio/gpu_audio_transport.hpp>\n"
"\n"
"// Reference a header type and an archive-defined symbol (stats()) so the\n"
"// probe fails to link when Pulp::gpu-audio or its headers are absent from\n"
"// the installed SDK.\n"
"int main() {\n"
"    pulp::gpu_audio::GpuAudioTransport transport;\n"
"    const auto stats = transport.stats();\n"
"    return transport.is_prepared() ? 1 : (stats.miss_blocks == 0 ? 0 : 2);\n"
"}\n")
    add_executable(PulpSDKSmokeGpuAudioProbe
        "${CMAKE_CURRENT_BINARY_DIR}/gpu_audio_export_probe.cpp")
    target_link_libraries(PulpSDKSmokeGpuAudioProbe PRIVATE Pulp::gpu-audio)
endif()

# Bundle-relocatability helper reachability.
#
# A GPU plugin built against the installed SDK calls pulp_make_bundle_relocatable
# + pulp_validate_bundle_relocatable — the @loader_path fix that stops the
# libwgpu_native.dylib "false pass on the build machine, crash on any other Mac"
# footgun. Those helpers live in PulpBundleRelocatable.cmake, which the SDK must
# both INSTALL (to lib/cmake/Pulp, with its check script under scripts/) and
# INCLUDE from PulpConfig. Assert both commands are defined by find_package(Pulp)
# so a regression that drops the module from the installed SDK fails here rather
# than in a downstream GPU plugin's configure with "Unknown CMake command".
# (Deterministic — holds even in the GPU-off smoke, since the functions are
# always defined and self-guard on APPLE internally.) Driving a real GPU bundle
# through make/validate end-to-end is a separate GPU-build-gated check.
if(NOT COMMAND pulp_make_bundle_relocatable
   OR NOT COMMAND pulp_validate_bundle_relocatable)
    message(FATAL_ERROR
        "Pulp SDK smoke: pulp_make_bundle_relocatable / "
        "pulp_validate_bundle_relocatable are not defined by find_package(Pulp). "
        "PulpBundleRelocatable.cmake must be installed to lib/cmake/Pulp and "
        "included by PulpConfig.cmake for downstream GPU plugins to ship "
        "self-contained bundles.")
endif()

# End-to-end relocatable GPU bundle check (GPU SDK only).
#
# pulp_add_plugin already places libwgpu_native.dylib next to each format binary
# and bakes the @loader_path rpath (its internal target_copy_webgpu_binaries),
# for find_package(Pulp) consumers just as for a source build. What is NOT
# automatic is the *validation* — so add pulp_validate_bundle_relocatable on the
# probe's real bundles. A regression in that installed-SDK copy/rpath path then
# fails the smoke here instead of shipping a GPU plugin that passes every check
# on the build machine and crashes ("Library not loaded") on a customer's Mac.
#
# Allow-list of self-contained single-bundle formats only. AUv3 is deliberately
# excluded: its .appex is intentionally NOT relocatable in isolation — the wgpu
# dylib and the AUv3 framework live in the *container app's* Contents/Frameworks,
# and the appex references the framework by @rpath resolved only inside that
# container, so scanning the standalone appex would be a false failure (and the
# container app that IS the relocatability unit isn't a probe target here).
# Gated on the `webgpu` IMPORTED target so the GPU-off smoke skips it.
if(TARGET webgpu)
    foreach(_fmt Standalone CLAP VST3 AU)
        set(_pulp_gpu_bundle "PulpSDKSmokeProbe_${_fmt}")
        if(TARGET ${_pulp_gpu_bundle})
            pulp_validate_bundle_relocatable(${_pulp_gpu_bundle})
        endif()
    endforeach()
endif()
