# Pulp example projects
# Each example exercises a framework path or integration surface.

# Local-only dev: chainer-synth lives in this dir gitignored when present,
# used for AU v3 / GPU editor validation. Not shipped on main.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/chainer-synth/CMakeLists.txt")
    add_subdirectory(chainer-synth)
endif()

# PulpGain: minimal gain effect plugin
add_subdirectory(pulp-gain)

# PulpRustGain — a Rust DSP core packaged as a CLAP plugin (native-components
# capstone). Opt-in: needs a Rust toolchain and the Cargo staticlib lane.
if(PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS AND NOT WIN32)
    add_subdirectory(rust-gain)
endif()

# DAW bench harness — load in real DAWs to graduate host-quirk rows from
# Speculative → Validated. See docs/validation/daw-bench/.
add_subdirectory(host-bench-plugin)

# PulpTone: oscillator synth with musical typing
add_subdirectory(pulp-tone)

# PulpEffect: filter with parameters and state
add_subdirectory(pulp-effect)
add_subdirectory(super-convolver)
add_subdirectory(spectral-lab)
# GPU NAM (Neural Amp Modeler player) lives in its own repo and consumes Pulp as
# an SDK: https://github.com/danielraffel/pulp-gpu-nam
# Bendr (pitch / formant / freeze effect) likewise lives in its own repo:
# https://github.com/danielraffel/pulp-bendr

# PulpCompressor: sidechain compressor validating multi-bus
add_subdirectory(pulp-compressor)

# Hot-Reload Demo: a reloadable shell plugin whose DSP lives in a separate
# .dylib that hot-swaps live in a DAW (the Phase 1b DAW-integration example).
add_subdirectory(hot-reload-demo)
# Hot-Reload Synth: the INSTRUMENT (aumu) variant — hot-swap the synth oscillator
# live while holding notes (M2 UI+DSP demo companion).
add_subdirectory(hot-reload-synth)

# Hot-Reload Morph: one reload swaps BOTH the DSP and the editor (sine/blue
# "WARM" <-> square/red "HARSH") — the create_view-forwarding showcase.
add_subdirectory(hot-reload-morph)

# UI Preview and Ink & Signal examples exercise the rendering pipeline and
# design-system imports. These depend on the design-import authoring cluster,
# so they are skipped in stripped (PULP_ENABLE_DESIGN_IMPORT=OFF) builds.
if(PULP_ENABLE_DESIGN_IMPORT)
    add_subdirectory(ui-preview)
    add_subdirectory(widget-gallery)
    add_subdirectory(ink-signal-sampler)
    add_subdirectory(component-shots)
    add_subdirectory(ink-signal-showcase)
    add_subdirectory(mtk-demo)
endif()

# PulpDrums: generative drum sequencer MIDI effect
add_subdirectory(PulpDrums)

# StepSequencer: synth-class step sequencer (UI <-> channel <-> Processor proof)
add_subdirectory(StepSequencer)

# PulpSynth: macro oscillator with DSP library
add_subdirectory(PulpSynth)

# MPE demo: MPE-aware sine synth validating the per-note expression path
add_subdirectory(mpe-synth)

# PulpSampler: sample-buffer sampler with MIDI + ADSR
add_subdirectory(PulpSampler)
add_subdirectory(virtual-list-sample-manager)

# OfflineStretch dev harness: stretchcli (file in -> render -> file out, --analyze)
add_subdirectory(offline-stretch)

# PulpTempoSampler — tempo-matching sampler built on the offline stretch engine
add_subdirectory(PulpTempoSampler)

# PulpPluck: Karplus-Strong synth reused by the WAM web demo
add_subdirectory(pulp-pluck)

# FAUST DSL examples: offline codegen to Processor
add_subdirectory(faust-gain)
add_subdirectory(faust-filter)
add_subdirectory(faust-tremolo)

# ViewBridge demo: custom create_view() + secondary-view attach
add_subdirectory(view-bridge-demo)

# Touch gesture recognizer proof: arbiter-first pan, pinch, and nested scroll.
add_subdirectory(gesture-touch-demo)

# Live Audio Inspector harness: run_with_editor plus steady output tone.
if(NOT IOS)
    add_subdirectory(audio-inspector-demo)
endif()

# Experimental native import preview: generated C++ view tree, no scripted UI.
if(PULP_ENABLE_DESIGN_IMPORT AND APPLE AND NOT PULP_IOS AND EXISTS "${CMAKE_SOURCE_DIR}/planning/artifacts/native-ui/nv0/reports/generated/chainer-phase-f-hybrid.cpp")
    add_subdirectory(native-ui-phase-f-preview)
endif()

# GPU Validation Demo: Modulation Matrix with animated cables
add_subdirectory(gpu-demo)

# SDF Text Demo: Resolution-independent text with bloom post-effect
add_subdirectory(sdf-text-demo)

# Plugin hosting validation: loads a real CLAP plugin.
# iOS App Store policy disallows dlopen of arbitrary third-party plugins,
# so `pulp::host` is intentionally omitted from iOS builds (root
# CMakeLists.txt gates `add_subdirectory(core/host)` with NOT IOS).
# Mirror that gate here so iOS Simulator / device configures don't
# fail Generate on a missing `pulp::host` target.
if(NOT IOS)
    add_subdirectory(plugin-host-demo)
endif()

# AI Style Designer: JS-defined design tool with hot reload (design-import only)
if(PULP_ENABLE_DESIGN_IMPORT AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/design-tool/CMakeLists.txt)
    add_subdirectory(design-tool)
endif()

# Design-import standalone: imports a figma-plugin .pulp.zip natively
# (sprite-knob hoist) and renders it in a GPU window — launchable demo. The
# default fixture is a private dev-only test design (not shipped).
if(PULP_ENABLE_DESIGN_IMPORT)
    add_subdirectory(design-import-standalone)
endif()

# TextEditor keyboard input test
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/text-input-test/CMakeLists.txt)
    add_subdirectory(text-input-test)
endif()

# Simple floating WebView palette proof
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/webview-palette/CMakeLists.txt)
    add_subdirectory(webview-palette)
endif()

# Plugin-editor WebView proof (macOS hosts only)
if(APPLE AND NOT PULP_IOS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/webview-plugin/CMakeLists.txt)
    add_subdirectory(webview-plugin)
endif()

# Optional Monaco WebView proof (requires prebundled Monaco assets)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/webview-monaco/CMakeLists.txt)
    add_subdirectory(webview-monaco)
endif()

# Three.js native GPU demo (requires GPU + three.js)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/threejs-native-demo/CMakeLists.txt)
    add_subdirectory(threejs-native-demo)
endif()

# Advanced SDF vs MSDF text: console atlas dumper for side-by-side quality comparison
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdf-vs-msdf-demo/CMakeLists.txt)
    add_subdirectory(sdf-vs-msdf-demo)
endif()

# SDF effects showcase (outline, glow, shadow, bevel)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/sdf-effects-demo/CMakeLists.txt)
    add_subdirectory(sdf-effects-demo)
endif()

# Streams feature demo (unified Stream / AsyncStream / HttpStream)
add_subdirectory(stream-demo)

# trace-demo: offline DSP tracing show-off (the "load meter said 40%, the trace
# said WHY" reveal). Perfetto-only, so gated on the dev-only PULP_TRACING option.
if(PULP_TRACING)
    add_subdirectory(trace-demo)
    add_subdirectory(trace-plugin-chain)
endif()

# iOS AUv3 examples — gated on CMAKE_SYSTEM_NAME=iOS
if(IOS)
    add_subdirectory(ios-auv3-synth)
    # Minimal 2D GPU AUv3 proof.
    add_subdirectory(ios-auv3-gpu-smoke)
    # Chainer-style JS-UI shape (title + Knob + Meter) rendered through
    # the same iPad GPU path, with the AUv3 editor
    # mounted inside the SwiftUI HostApp via UIViewControllerRepresentable.
    add_subdirectory(ios-auv3-chainer)
    # Three.js rotating-cube AUv3 demo. Uses the iOS GPU editor path
    # (GpuSurface attach, three.iife.js bundler, presentable swapchain
    # flag, queue.submit marker) to paint a real three.webgpu.js scene
    # inside the editor pane.
    add_subdirectory(ios-auv3-jsc-threejs)
endif()

# ARA pitch tracker example
if(NOT ANDROID AND NOT IOS)
    add_subdirectory(ara-pitch-tracker)
endif()

# Moonbase licensing: optional copy-protection reference. Builds a loadable
# plugin (CLAP + Standalone always; VST3/AU when their SDKs are present) with an
# interactive activation editor, plus tests and a headless screenshot tool. Off
# by default (fetches the third-party Moonbase SDK and needs OpenSSL at configure
# time). Enable with -DPULP_BUILD_MOONBASE_EXAMPLE=ON. See
# docs/guides/copyright-protection.md.
option(PULP_BUILD_MOONBASE_EXAMPLE
       "Build the optional Moonbase licensing example" OFF)
if(PULP_BUILD_MOONBASE_EXAMPLE)
    add_subdirectory(moonbase-activation)
endif()
