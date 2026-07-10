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

# example-plugins (8): instruments, MIDI FX, stateful/util effects.
pulp_add_wclap(MonoSynth        ENTRY ${_wclap_plugins_dir}/mono_synth_wclap.cpp         INCLUDES ${PULP_EXAMPLE_PLUGINS_DIR}/mono-synth)
pulp_add_wclap(SynthWithPresets ENTRY ${_wclap_plugins_dir}/synth_with_presets_wclap.cpp INCLUDES ${PULP_EXAMPLE_PLUGINS_DIR}/synth-with-presets)
pulp_add_wclap(Gain             ENTRY ${_wclap_plugins_dir}/gain_wclap.cpp               INCLUDES ${PULP_EXAMPLE_PLUGINS_DIR}/gain)
pulp_add_wclap(StateMemo        ENTRY ${_wclap_plugins_dir}/state_memo_wclap.cpp         INCLUDES ${PULP_EXAMPLE_PLUGINS_DIR}/state-memo)
pulp_add_wclap(MidiTranspose    ENTRY ${_wclap_plugins_dir}/midi_transpose_wclap.cpp     INCLUDES ${PULP_EXAMPLE_PLUGINS_DIR}/midi-transpose)
pulp_add_wclap(MpeSpreader      ENTRY ${_wclap_plugins_dir}/mpe_spreader_wclap.cpp       INCLUDES ${PULP_EXAMPLE_PLUGINS_DIR}/mpe-spreader)
pulp_add_wclap(MidiInspector    ENTRY ${_wclap_plugins_dir}/midi_inspector_wclap.cpp     INCLUDES ${PULP_EXAMPLE_PLUGINS_DIR}/midi-inspector)
pulp_add_wclap(SysexEcho        ENTRY ${_wclap_plugins_dir}/sysex_echo_wclap.cpp         INCLUDES ${PULP_EXAMPLE_PLUGINS_DIR}/sysex-echo)

# classic-effects (15): stereo audio effects.
pulp_add_wclap(Chorus             ENTRY ${_wclap_plugins_dir}/chorus_wclap.cpp              INCLUDES ${PULP_CLASSIC_EFFECTS_DIR}/chorus)
pulp_add_wclap(CompressorExpander ENTRY ${_wclap_plugins_dir}/compressor_expander_wclap.cpp INCLUDES ${PULP_CLASSIC_EFFECTS_DIR}/compressor-expander)
pulp_add_wclap(Delay              ENTRY ${_wclap_plugins_dir}/delay_wclap.cpp               INCLUDES ${PULP_CLASSIC_EFFECTS_DIR}/delay)
pulp_add_wclap(Distortion         ENTRY ${_wclap_plugins_dir}/distortion_wclap.cpp          INCLUDES ${PULP_CLASSIC_EFFECTS_DIR}/distortion)
pulp_add_wclap(Flanger            ENTRY ${_wclap_plugins_dir}/flanger_wclap.cpp             INCLUDES ${PULP_CLASSIC_EFFECTS_DIR}/flanger)
pulp_add_wclap(Panning            ENTRY ${_wclap_plugins_dir}/panning_wclap.cpp             INCLUDES ${PULP_CLASSIC_EFFECTS_DIR}/panning)
pulp_add_wclap(ParametricEq       ENTRY ${_wclap_plugins_dir}/parametric_eq_wclap.cpp       INCLUDES ${PULP_CLASSIC_EFFECTS_DIR}/parametric-eq)
pulp_add_wclap(Phaser             ENTRY ${_wclap_plugins_dir}/phaser_wclap.cpp              INCLUDES ${PULP_CLASSIC_EFFECTS_DIR}/phaser)
pulp_add_wclap(PingPong           ENTRY ${_wclap_plugins_dir}/ping_pong_wclap.cpp           INCLUDES ${PULP_CLASSIC_EFFECTS_DIR}/ping-pong)
pulp_add_wclap(PitchShift         ENTRY ${_wclap_plugins_dir}/pitch_shift_wclap.cpp         INCLUDES ${PULP_CLASSIC_EFFECTS_DIR}/pitch-shift)
pulp_add_wclap(RingMod            ENTRY ${_wclap_plugins_dir}/ring_mod_wclap.cpp            INCLUDES ${PULP_CLASSIC_EFFECTS_DIR}/ring-mod)
pulp_add_wclap(Robotization       ENTRY ${_wclap_plugins_dir}/robotization_wclap.cpp        INCLUDES ${PULP_CLASSIC_EFFECTS_DIR}/robotization)
pulp_add_wclap(Tremolo            ENTRY ${_wclap_plugins_dir}/tremolo_wclap.cpp             INCLUDES ${PULP_CLASSIC_EFFECTS_DIR}/tremolo)
pulp_add_wclap(Vibrato            ENTRY ${_wclap_plugins_dir}/vibrato_wclap.cpp             INCLUDES ${PULP_CLASSIC_EFFECTS_DIR}/vibrato)
pulp_add_wclap(Wah                ENTRY ${_wclap_plugins_dir}/wah_wclap.cpp                 INCLUDES ${PULP_CLASSIC_EFFECTS_DIR}/wah)
