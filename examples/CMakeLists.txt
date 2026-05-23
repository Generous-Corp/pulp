# Pulp example projects
# Each example validates a phase of the framework

# Local-only dev: chainer-synth lives in this dir gitignored when present,
# used for AU v3 / GPU editor validation. Not shipped on main.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/chainer-synth/CMakeLists.txt")
    add_subdirectory(chainer-synth)
endif()

# Phase 2-3: PulpGain (trivial gain plugin)
add_subdirectory(pulp-gain)

# Phase 4: PulpTone (oscillator synth + musical typing)
add_subdirectory(pulp-tone)

# Phase 5: PulpEffect (filter with parameters and state)
add_subdirectory(pulp-effect)

# Phase 5: PulpCompressor (sidechain compressor validating multi-bus)
add_subdirectory(pulp-compressor)

# Phase 6: UI Preview (validates rendering pipeline)
add_subdirectory(ui-preview)

# Phase 6: PulpDrums (generative drum sequencer MIDI effect)
add_subdirectory(PulpDrums)

# Phase 7: PulpSynth (macro oscillator with DSP library)
add_subdirectory(PulpSynth)

# MPE demo: MPE-aware sine synth validating the per-note expression path
add_subdirectory(mpe-synth)

# Phase 17: PulpSampler (audio file sampler with MIDI + ADSR)
add_subdirectory(PulpSampler)

# Phase 18: PulpPluck (Karplus-Strong synth — same code runs native + WASM)
add_subdirectory(pulp-pluck)

# FAUST DSL examples (Phase 3: offline codegen → Processor)
add_subdirectory(faust-gain)
add_subdirectory(faust-filter)
add_subdirectory(faust-tremolo)

# ViewBridge demo: custom create_view() + secondary-view attach
add_subdirectory(view-bridge-demo)

# GPU Validation Demo: Modulation Matrix with animated cables
add_subdirectory(gpu-demo)

# SDF Text Demo: Resolution-independent text with bloom post-effect
add_subdirectory(sdf-text-demo)

# Feature 5 Phase 1: plugin hosting validation — loads a real CLAP plugin
add_subdirectory(plugin-host-demo)

# AI Style Designer: JS-defined design tool with hot reload
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/design-tool/CMakeLists.txt)
    add_subdirectory(design-tool)
endif()

# TextEditor keyboard input test (Phase 21.1)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/text-input-test/CMakeLists.txt)
    add_subdirectory(text-input-test)
endif()

# Phase 7: simple floating WebView palette proof
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/webview-palette/CMakeLists.txt)
    add_subdirectory(webview-palette)
endif()

# Phase 7: plugin-editor WebView proof (macOS hosts only)
if(APPLE AND NOT PULP_IOS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/webview-plugin/CMakeLists.txt)
    add_subdirectory(webview-plugin)
endif()

# Phase 7: optional Monaco WebView proof (requires prebundled Monaco assets)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/webview-monaco/CMakeLists.txt)
    add_subdirectory(webview-monaco)
endif()

# Phase 13: Three.js native GPU demo (requires GPU + three.js)
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

# iOS AUv3 example (workstream 05 slice 5.1) — gated on CMAKE_SYSTEM_NAME=iOS
if(IOS)
    add_subdirectory(ios-auv3-synth)
endif()

# ARA pitch tracker example (workstream 06 slice A3)
if(NOT ANDROID AND NOT IOS)
    add_subdirectory(ara-pitch-tracker)
endif()
