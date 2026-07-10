# WebCLAP build for Pulp web demo plugins.
# Compiles PulpGain to a WebAssembly CLAP (.wasm) with wasi-sdk.
#
# Build:
#   cd examples/web-demos/wclap-build
#   cmake -S . -B build \
#       -DCMAKE_TOOLCHAIN_FILE=../../../tools/cmake/wasi-toolchain.cmake \
#       -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#
# Output:
#   build/PulpGain.wasm        — the bare WebCLAP module (exports clap_entry +
#                                malloc/free/cabi_realloc + a growable function
#                                table + shared memory). Used by the probe/host.
#   build/PulpGain.wclap/      — the distributable bundle (module.wasm + any
#                                resources), per free-audio/web-clap. Emitted as
#                                a post-build step by pulp_add_wclap when node is
#                                available.
#   build/PulpGain.wclap.tar.gz — the web-distribution archive of the bundle.
#
# Validate the module is live (instantiate, run _initialize, resolve clap_entry):
#   node ../../../core/format/src/wasm/wclap_probe.mjs build/PulpGain.wclap/module.wasm
#
# Host it for real — full CLAP lifecycle + audio + parameter control (plain node;
# the host vtable uses trampolines, no WebAssembly.Function):
#   node ../../../core/format/src/wasm/wclap_host_runner.mjs build/PulpGain.wasm --gain-db 6
#
# The WebCLAP build logic (portable DSP/adapter source set, link flags, the
# clap_entry/allocator export contract) lives in tools/cmake/PulpWclap.cmake —
# this file only declares which plugins to build. See that helper and
# wasi-toolchain.cmake for why the heavy pulp::format / pulp::state libraries
# are not linked directly and why -fno-exceptions is mandatory.

cmake_minimum_required(VERSION 3.20)
project(pulp-wclap-demos LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PULP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
include(${PULP_ROOT}/tools/cmake/PulpWclap.cmake)

pulp_add_wclap(PulpGain
    ENTRY    ${CMAKE_CURRENT_SOURCE_DIR}/pulp_gain_wclap.cpp
    INCLUDES ${PULP_ROOT}/examples/pulp-gain)

# ── WebCLAP 1:1 demo gallery ────────────────────────────────────────────────
# A WebCLAP module for EVERY plugin that already has a WAM demo, so the WebCLAP
# site mirrors both existing WAM galleries one-for-one. INCLUDES point at the
# SAME headless-curated plugin source dirs the WAM build compiles — the copies
# living in the two *-web-demos / *-web repos (siblings of their web/ dir), NOT
# the native implementation repos: those web-demos copies gate every editor /
# core-view include behind `#if !PULP_HEADLESS`, which is exactly what a wasm
# CLAP needs. Override the two roots with -D if the checkouts are elsewhere; the
# defaults assume the repos are siblings of the pulp checkout.
set(PULP_EXAMPLE_PLUGINS_DIR "${PULP_ROOT}/../pulp-example-plugins-web-demos"
    CACHE PATH "Checkout of danielraffel/pulp-example-plugins-web-demos (MonoSynth…SysexEcho)")
set(PULP_CLASSIC_EFFECTS_DIR "${PULP_ROOT}/../pulp-classic-effects-web"
    CACHE PATH "Checkout of danielraffel/pulp-classic-effects-web (Chorus…Wah)")

set(_wclap_plugins_dir ${CMAKE_CURRENT_SOURCE_DIR}/plugins)

# The two plugin-source repos are EXTERNAL to pulp (siblings of the checkout).
# A CI job that checks out only pulp — e.g. .github/workflows/web-plugins.yml,
# which builds this whole project with `cmake --build build` — does NOT have
# them, so their plugin headers are absent. Add each gallery target only when
# its plugin header is present; otherwise skip it (and record the name) so CI
# builds exactly PulpGain, as it did before the gallery existed. With both repos
# checked out (the gallery deploy), all 23 build. This keeps the in-repo build
# green without CI needing the external repos.
set(_wclap_gallery_skipped "")
macro(pulp_add_wclap_demo NAME HEADER DIR ENTRY)
    if(EXISTS "${DIR}/${HEADER}")
        pulp_add_wclap(${NAME} ENTRY ${ENTRY} INCLUDES ${DIR})
    else()
        list(APPEND _wclap_gallery_skipped ${NAME})
    endif()
endmacro()

# example-plugins (8): instruments, MIDI FX, stateful/util effects.
pulp_add_wclap_demo(MonoSynth        mono_synth.hpp         ${PULP_EXAMPLE_PLUGINS_DIR}/mono-synth          ${_wclap_plugins_dir}/mono_synth_wclap.cpp)
pulp_add_wclap_demo(SynthWithPresets synth_with_presets.hpp ${PULP_EXAMPLE_PLUGINS_DIR}/synth-with-presets  ${_wclap_plugins_dir}/synth_with_presets_wclap.cpp)
pulp_add_wclap_demo(Gain             gain.hpp               ${PULP_EXAMPLE_PLUGINS_DIR}/gain                ${_wclap_plugins_dir}/gain_wclap.cpp)
pulp_add_wclap_demo(StateMemo        state_memo.hpp         ${PULP_EXAMPLE_PLUGINS_DIR}/state-memo          ${_wclap_plugins_dir}/state_memo_wclap.cpp)
pulp_add_wclap_demo(MidiTranspose    midi_transpose.hpp     ${PULP_EXAMPLE_PLUGINS_DIR}/midi-transpose      ${_wclap_plugins_dir}/midi_transpose_wclap.cpp)
pulp_add_wclap_demo(MpeSpreader      mpe_spreader.hpp       ${PULP_EXAMPLE_PLUGINS_DIR}/mpe-spreader        ${_wclap_plugins_dir}/mpe_spreader_wclap.cpp)
pulp_add_wclap_demo(MidiInspector    midi_inspector.hpp     ${PULP_EXAMPLE_PLUGINS_DIR}/midi-inspector      ${_wclap_plugins_dir}/midi_inspector_wclap.cpp)
pulp_add_wclap_demo(SysexEcho        sysex_echo.hpp         ${PULP_EXAMPLE_PLUGINS_DIR}/sysex-echo          ${_wclap_plugins_dir}/sysex_echo_wclap.cpp)

# classic-effects (15): stereo audio effects.
pulp_add_wclap_demo(Chorus             chorus.hpp              ${PULP_CLASSIC_EFFECTS_DIR}/chorus              ${_wclap_plugins_dir}/chorus_wclap.cpp)
pulp_add_wclap_demo(CompressorExpander compressor_expander.hpp ${PULP_CLASSIC_EFFECTS_DIR}/compressor-expander ${_wclap_plugins_dir}/compressor_expander_wclap.cpp)
pulp_add_wclap_demo(Delay              delay.hpp               ${PULP_CLASSIC_EFFECTS_DIR}/delay               ${_wclap_plugins_dir}/delay_wclap.cpp)
pulp_add_wclap_demo(Distortion         distortion.hpp          ${PULP_CLASSIC_EFFECTS_DIR}/distortion          ${_wclap_plugins_dir}/distortion_wclap.cpp)
pulp_add_wclap_demo(Flanger            flanger.hpp             ${PULP_CLASSIC_EFFECTS_DIR}/flanger             ${_wclap_plugins_dir}/flanger_wclap.cpp)
pulp_add_wclap_demo(Panning            panning.hpp             ${PULP_CLASSIC_EFFECTS_DIR}/panning             ${_wclap_plugins_dir}/panning_wclap.cpp)
pulp_add_wclap_demo(ParametricEq       parametric_eq.hpp       ${PULP_CLASSIC_EFFECTS_DIR}/parametric-eq       ${_wclap_plugins_dir}/parametric_eq_wclap.cpp)
pulp_add_wclap_demo(Phaser             phaser.hpp              ${PULP_CLASSIC_EFFECTS_DIR}/phaser              ${_wclap_plugins_dir}/phaser_wclap.cpp)
pulp_add_wclap_demo(PingPong           ping_pong.hpp           ${PULP_CLASSIC_EFFECTS_DIR}/ping-pong           ${_wclap_plugins_dir}/ping_pong_wclap.cpp)
pulp_add_wclap_demo(PitchShift         pitch_shift.hpp         ${PULP_CLASSIC_EFFECTS_DIR}/pitch-shift         ${_wclap_plugins_dir}/pitch_shift_wclap.cpp)
pulp_add_wclap_demo(RingMod            ring_mod.hpp            ${PULP_CLASSIC_EFFECTS_DIR}/ring-mod            ${_wclap_plugins_dir}/ring_mod_wclap.cpp)
pulp_add_wclap_demo(Robotization       robotization.hpp        ${PULP_CLASSIC_EFFECTS_DIR}/robotization        ${_wclap_plugins_dir}/robotization_wclap.cpp)
pulp_add_wclap_demo(Tremolo            tremolo.hpp             ${PULP_CLASSIC_EFFECTS_DIR}/tremolo             ${_wclap_plugins_dir}/tremolo_wclap.cpp)
pulp_add_wclap_demo(Vibrato            vibrato.hpp             ${PULP_CLASSIC_EFFECTS_DIR}/vibrato             ${_wclap_plugins_dir}/vibrato_wclap.cpp)
pulp_add_wclap_demo(Wah                wah.hpp                 ${PULP_CLASSIC_EFFECTS_DIR}/wah                 ${_wclap_plugins_dir}/wah_wclap.cpp)

if(_wclap_gallery_skipped)
    list(LENGTH _wclap_gallery_skipped _n)
    message(STATUS "PulpWclap gallery: ${_n} target(s) skipped — plugin sources not checked out "
                   "(set -DPULP_EXAMPLE_PLUGINS_DIR / -DPULP_CLASSIC_EFFECTS_DIR): ${_wclap_gallery_skipped}")
endif()
