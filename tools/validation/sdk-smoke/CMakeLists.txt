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
        audio dsl events format host playback signal signal-modal-spec state timebase timeline view standalone gpu-audio)
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

if(NOT TARGET Pulp::timebase)
    message(FATAL_ERROR
        "Pulp SDK smoke: installed SDK does not expose Pulp::timebase")
endif()
get_target_property(_pulp_timebase_features Pulp::timebase INTERFACE_COMPILE_FEATURES)
if(NOT "cxx_std_20" IN_LIST _pulp_timebase_features)
    message(FATAL_ERROR
        "Pulp SDK smoke: Pulp::timebase does not propagate cxx_std_20")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/timebase_export_probe.cpp"
"#include <pulp/timebase/compiled_tempo_map.hpp>\n"
"\n"
"int main() {\n"
"    const pulp::timebase::TempoPoint point{{0}, 120.0};\n"
"    auto compiled = pulp::timebase::CompiledTempoMap::compile(\n"
"        std::span<const pulp::timebase::TempoPoint>(&point, 1), {48'000, 1});\n"
"    return compiled && compiled->ticks_to_samples(\n"
"        {pulp::timebase::kTicksPerQuarter}).value == 24'000\n"
"        ? 0 : 1;\n"
"}\n")
add_executable(PulpSDKSmokeTimebaseProbe
    "${CMAKE_CURRENT_BINARY_DIR}/timebase_export_probe.cpp")
target_link_libraries(PulpSDKSmokeTimebaseProbe PRIVATE Pulp::timebase)

# Raw-byte WAV decoder export probe. Browser and storage-backed consumers
# receive media bytes rather than native paths, so the installed SDK must
# expose and link this no-file-I/O surface through Pulp::audio.
if(NOT TARGET Pulp::audio)
    message(FATAL_ERROR
        "Pulp SDK smoke: installed SDK does not expose Pulp::audio")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/wav_decoder_export_probe.cpp"
"#include <pulp/audio/wav_decoder.hpp>\n"
"\n"
"int main() {\n"
"    return pulp::audio::decode_wav({}).has_value() ? 1 : 0;\n"
"}\n")
add_executable(PulpSDKSmokeWavDecoderProbe
    "${CMAKE_CURRENT_BINARY_DIR}/wav_decoder_export_probe.cpp")
target_link_libraries(PulpSDKSmokeWavDecoderProbe PRIVATE Pulp::audio)

if(NOT TARGET Pulp::playback)
    message(FATAL_ERROR
        "Pulp SDK smoke: installed SDK does not expose Pulp::playback")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/playback_export_probe.cpp"
"#include <pulp/playback/program_compiler.hpp>\n"
"#include <pulp/playback/stable_renderer_shell.hpp>\n"
"#include <pulp/playback/transport.hpp>\n"
"#include <pulp/timebase/compiled_tempo_map.hpp>\n"
"#include <array>\n"
"\n"
"int main() {\n"
"    const std::array points{pulp::timebase::TempoPoint{{0}, 120.0}};\n"
"    auto compiled = pulp::timebase::CompiledTempoMap::compile(points, {48'000, 1});\n"
"    if (!compiled) return 1;\n"
"    const auto& map = compiled.value();\n"
"    pulp::playback::PlaybackProgramStore program_store;\n"
"    pulp::playback::DeferredCompileExecutor compile_executor;\n"
"    pulp::playback::PlaybackProgramCompiler compiler(\n"
"        program_store, compile_executor, std::chrono::microseconds(0));\n"
"    pulp::playback::PlaybackProgramBlockLatch latch;\n"
"    (void)latch;\n"
"    pulp::playback::MasterTransport transport;\n"
"    const auto error = transport.prepare(map, {.max_buffer_size = 64});\n"
"    pulp::playback::TransportSnapshot snapshot;\n"
"    return error == pulp::playback::TransportError::None &&\n"
"                   transport.begin_block(64, snapshot) ==\n"
"                       pulp::playback::TransportError::None\n"
"               ? 0 : 1;\n"
"}\n")
add_executable(PulpSDKSmokePlaybackProbe
    "${CMAKE_CURRENT_BINARY_DIR}/playback_export_probe.cpp")
target_link_libraries(PulpSDKSmokePlaybackProbe PRIVATE Pulp::playback)

if(NOT TARGET Pulp::timeline)
    message(FATAL_ERROR
        "Pulp SDK smoke: installed SDK does not expose Pulp::timeline")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/timeline_export_probe.cpp"
"#include <pulp/timeline/command.hpp>\n"
"#include <pulp/timeline/document_session.hpp>\n"
"#include <pulp/timeline/journal.hpp>\n"
"#include <pulp/timeline/model.hpp>\n"
"#include <pulp/timeline/transaction.hpp>\n"
"#include <pulp/timeline/undo.hpp>\n"
"#include <pulp/timeline/serialize.hpp>\n"
"\n"
"int main() {\n"
"    using namespace pulp::timeline;\n"
"    auto track = Track::create({3}, \"track\", {});\n"
"    if (!track) return 1;\n"
"    auto sequence = Sequence::create({2}, \"sequence\", std::nullopt,\n"
"                                     {std::move(track).value()});\n"
"    if (!sequence) return 2;\n"
"    auto project = Project::create({{1}, \"project\", 4, {2}, {},\n"
"                                    {std::move(sequence).value()}});\n"
"    if (!project) return 3;\n"
"    SetTempoMap tempo_edit{project->tempo_map(), project->tempo_map()};\n"
"    SetMeterMap meter_edit{project->meter_map(), project->meter_map()};\n"
"    if (!equivalent(Command{tempo_edit}, Command{tempo_edit}) ||\n"
"        !equivalent(Command{meter_edit}, Command{meter_edit})) return 7;\n"
"    auto session = DocumentSession::create(std::move(project).value());\n"
"    if (!session) return 4;\n"
"    auto registry = make_builtin_timeline_registry();\n"
"    if (!registry) return 5;\n"
"    auto serialized = serialize_project(*session.value()->snapshot(), registry.value());\n"
"    return serialized ? 0 : 6;\n"
"}\n")
add_executable(PulpSDKSmokeTimelineProbe
    "${CMAKE_CURRENT_BINARY_DIR}/timeline_export_probe.cpp")
target_link_libraries(PulpSDKSmokeTimelineProbe PRIVATE Pulp::timeline)

# Installed host/state/dsl header + link probe. These targets were exported
# before all of their public headers were reliably copied into the SDK, so a
# target-existence check alone produced a linkable-looking but unusable package.
if(TARGET Pulp::host)
    if(NOT TARGET Pulp::state OR NOT TARGET Pulp::dsl)
        message(FATAL_ERROR
            "Pulp SDK smoke: installed host/state/dsl probe surfaces are incomplete")
    endif()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/host_export_probe.cpp"
"#include <pulp/dsl/dsl_processor.hpp>\n"
"#include <pulp/events/message_loop_integration.hpp>\n"
"#include <pulp/host/plugin_slot.hpp>\n"
"#include <pulp/state/preset_manager.hpp>\n"
"\n"
"int main() {\n"
"    const auto pump =\n"
"        pulp::events::MessageLoopIntegration::pump_main_loop_for(\n"
"            std::chrono::milliseconds(0));\n"
"    pulp::host::PluginInfo info;\n"
"    auto slot = pulp::host::PluginSlot::load(info);\n"
"    return slot || pump == pulp::events::MainLoopPumpResult::WrongThread\n"
"        ? 1 : 0;\n"
"}\n")
    add_executable(PulpSDKSmokeHostProbe
        "${CMAKE_CURRENT_BINARY_DIR}/host_export_probe.cpp")
    target_link_libraries(PulpSDKSmokeHostProbe PRIVATE
        Pulp::dsl Pulp::events Pulp::host Pulp::state)
endif()

# Source-bearing modal-spec parser export probe. The public header alone is not
# enough: parse_modal_spec()/to_json() live in a separate archive, so force a
# downstream link through the installed target.
if(TARGET Pulp::signal-modal-spec)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/modal_spec_export_probe.cpp" [=[
#include <pulp/signal/modal_spec.hpp>

int main() {
    pulp::signal::ModalSpecDiagnostics diagnostics;
    const auto spec = pulp::signal::parse_modal_spec(
        R"({"schema_version":1,"name":"probe","modes":[
          {"freq_hz":440.0,"t60_s":1.0,"gain":1.0}]})",
        &diagnostics);
    return spec && !pulp::signal::to_json(*spec).empty() ? 0 : 1;
}
]=])
    add_executable(PulpSDKSmokeModalSpecProbe
        "${CMAKE_CURRENT_BINARY_DIR}/modal_spec_export_probe.cpp")
    target_link_libraries(PulpSDKSmokeModalSpecProbe PRIVATE
        Pulp::signal-modal-spec)
else()
    message(FATAL_ERROR
        "Pulp SDK smoke: installed SDK does not export Pulp::signal-modal-spec")
endif()

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

if(TARGET Pulp::timeline)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/timeline_cxx20_export_probe.cpp"
"#include <pulp/timeline/model.hpp>\n"
"\n"
"int main() {\n"
"    constexpr pulp::timeline::ClipPlaybackProperties properties;\n"
"    return properties == pulp::timeline::ClipPlaybackProperties{} ? 0 : 1;\n"
"}\n")
    add_executable(PulpSDKSmokeTimelineCxx20Probe
        "${CMAKE_CURRENT_BINARY_DIR}/timeline_cxx20_export_probe.cpp")
    target_link_libraries(PulpSDKSmokeTimelineCxx20Probe PRIVATE Pulp::timeline)
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
