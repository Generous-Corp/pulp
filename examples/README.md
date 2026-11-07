# Examples

Every directory under `examples/` is listed below, in one of two groups.

- **Curated examples** show a feature you can copy into your own plugin or app.
- **Proof consumers and test-only projects** exist to prove a contract, for example that an installed SDK builds and runs a small program. They are tested by CI. They are not the place to learn the framework.

The proof consumers are also listed with their intended future home under `test/consumers/`. That move is not done yet; see the end of this page.

## Building and running

Examples are off in a default source build. Add `--examples`:

```bash
pulp build --examples --target <target>
```

Plugin examples build one target per format: `<Name>_Standalone`, `<Name>_CLAP`, `<Name>_VST3` and `<Name>_AU`. Which formats an example builds is set in its `CMakeLists.txt`. Some examples build only when a platform, option or SDK is present; the condition is in `examples/CMakeLists.txt` or the example's own file.

Projects marked **SDK project** are standalone CMake projects that use only an installed Pulp SDK. Configure one with:

```bash
cmake -S examples/<dir> -B build/<dir> -DPulp_DIR=<sdk>/lib/cmake/Pulp
```

## Curated examples

### Effects and instruments

| Directory | What it shows | Build and run |
|---|---|---|
| `pulp-gain` | Smallest plugin: gain, bypass, all formats, a golden-file audio test | `pulp build --examples --target PulpGain_Standalone` |
| `pulp-effect` | Biquad filter with parameters and state save and restore | `pulp build --examples --target PulpEffect_CLAP` |
| `pulp-compressor` | Sidechain compressor, multi-bus routing | `pulp build --examples --target PulpCompressor_CLAP` |
| `pulp-delay` | Stereo character delay with tempo sync and an in-loop tone control | `pulp build --examples --target PulpDelay_Standalone` |
| `pulp-tone` | Oscillator synth with MIDI input and musical typing | `pulp build --examples --target PulpTone_Standalone` |
| `PulpSynth` | Macro oscillator synth with filter, ADSR and presets | `pulp build --examples --target PulpSynth_CLAP` |
| `PulpSampler` | Sample-buffer sampler with MIDI triggering and ADSR | `pulp build --examples --target PulpSampler_CLAP` |
| `PulpTempoSampler` | Tempo-matching sampler built on the offline time-stretch engine | `pulp build --examples --target PulpTempoSampler_Standalone` |
| `PulpDrums` | Generative drum sequencer as a MIDI effect | `pulp build --examples --target PulpDrums_CLAP` |
| `PulpSequence` | Sequence demo processor shipped in every plugin format | `pulp build --examples --target PulpSequence_CLAP` |
| `StepSequencer` | Synth-class step sequencer | `pulp build --examples --target StepSequencer_CLAP` |
| `pulp-pluck` | Karplus-Strong plucked string | `pulp build --examples --target PulpPluck_CLAP` |
| `pulp-transpose` | MIDI effect: notes in, transposed notes out, no audio | `pulp build --examples --target PulpTranspose_CLAP` |
| `pulp-multi-out` | Eight-voice instrument with one output bus per voice | `pulp build --examples --target PulpMultiOut_CLAP` |
| `mpe-synth` | MPE-aware sine synth | `pulp build --examples --target PulpMpeSynth_CLAP` |
| `eq-curve-demo` | Six-band parametric EQ with the interactive curve editor | `pulp build --examples --target EqCurveDemo_Standalone` |
| `super-convolver` | Convolution reverb and impulse processor | `pulp build --examples --target SuperConvolver_Standalone` |
| `spectral-lab` | Layered GPU spectral cloud with freeze and morph | `pulp build --examples --target SpectralLab_Standalone` |
| `combined-bundle-demo` | One binary per format exposing two distinct plugins | `pulp build --examples --target CombinedBundleDemo_CLAP` |
| `sample-region-allpass` | Mono processor that owns an editable signal graph (**SDK project**) | `pulp build --examples --target SampleRegionAllpass_CLAP` |
| `ara-pitch-tracker` | Smallest ARA-aware CLAP plugin, end to end | `pulp build --examples --target AraPitchTracker_CLAP` |
| `cv-control` | Provider-neutral control-voltage slice; capture and inference stay in the app | `pulp build --examples --target CvFaceTone_Standalone` |
| `moonbase-activation` | Plugin that gates audio on a license activation; needs `PULP_BUILD_MOONBASE_EXAMPLE` | `pulp build --examples --target MoonbaseActivation_Standalone` |

### Editors and user interface

| Directory | What it shows | Build and run |
|---|---|---|
| `design-panel-plugin` | Effect whose editor is an imported design; builds only with Skia and the design file present | `pulp build --examples --target PulpDesignPanel_CLAP` |
| `design-panel-instrument` | Instrument whose editor is an imported design | `pulp build --examples --target PulpDesignSynth_CLAP` |
| `design-panel-midi-effect` | MIDI effect whose editor is an imported design | `pulp build --examples --target PulpDesignArp_CLAP` |
| `design-import-standalone` | Imports a design archive at runtime and renders it in a GPU window | `pulp build --examples --target pulp-design-import-standalone` |
| `design-tool` | Style designer written in JavaScript with hot reload | `pulp build --examples --target pulp-design-tool` |
| `webview-plugin` | Plugin with a web view editor; macOS only | `pulp build --examples --target PulpWebViewPlugin_Standalone` |
| `webview-monaco` | Code editor in a web view; needs `PULP_BUILD_WEBVIEW` and `PULP_MONACO_BUNDLE_DIR` | `pulp build --examples --target pulp-webview-monaco` |
| `webview-palette` | Palette picker in a web view | `pulp build --examples --target pulp-webview-palette` |
| `ui-preview` | Full rendering pipeline: JavaScript, widgets, layout, paint, window | `pulp build --examples --target pulp-ui-preview` |
| `widget-gallery` | Every widget in one board, rendered headless to PNG | `pulp build --examples --target pulp-widget-gallery` |
| `ink-signal-showcase` | Live GPU window of every design-system primitive | `pulp build --examples --target pulp-ink-signal-showcase` |
| `ink-signal-sampler` | Sampler UI assembled from the design component catalog, rendered to PNG | `pulp build --examples --target pulp-ink-signal-sampler` |
| `component-shots` | Each core widget alone, light and dark, rendered to PNG | `pulp build --examples --target pulp-component-shots` |
| `gpu-demo` | Animated modulation matrix on the GPU render path | `pulp build --examples --target pulp-gpu-demo` |
| `sdf-text-demo` | Resolution-independent text with a bloom effect | `pulp build --examples --target pulp-sdf-text-demo` |
| `sdf-vs-msdf-demo` | Text quality comparison between two distance-field methods | `pulp build --examples --target pulp-sdf-vs-msdf-demo` |
| `sdf-effects-demo` | Shadow, outline and glow presets from the design tokens | `pulp build --examples --target pulp-sdf-effects-demo` |
| `mtk-demo` | Musical typing keyboard widget in a GPU window | `pulp build --examples --target pulp-mtk-demo` |
| `gesture-touch-demo` | Touch gesture recognizers, rendered headless | `pulp build --examples --target pulp-gesture-touch-demo` |
| `virtual-list-sample-manager` | Virtual list with a large sample library, rendered to PNG | `pulp build --examples --target pulp-virtual-list-sample-manager` |
| `threejs-native-demo` | A WebGPU 3D library running on the native GPU bridge | `pulp build --examples --target pulp-threejs-native-demo` |
| `view-bridge-demo` | Custom plugin view and multi-view attach, headless | `pulp build --examples --target pulp-view-bridge-demo` |
| `audio-inspector-demo` | Editor window plus continuous output audio | `pulp build --examples --target pulp-audio-inspector-demo` |
| `showcase` | Script-only widget showcase with no target of its own | `./build/examples/ui-preview/pulp-ui-preview --script examples/showcase/showcase.js` |

### Hot reload

| Directory | What it shows | Build and run |
|---|---|---|
| `hot-reload-demo` | Plugin shell whose DSP lives in a separate library that swaps live | `pulp build --examples --target HotReloadDemo_Standalone` |
| `hot-reload-synth` | The same idea for an instrument, swapping while notes are held | `pulp build --examples --target HotReloadSynth_Standalone` |
| `hot-reload-morph` | One reload changes both the DSP and the editor | `pulp build --examples --target HotReloadMorph_Standalone` |

### DSP languages and source-only lanes

| Directory | What it shows | Build and run |
|---|---|---|
| `faust-gain`, `faust-filter`, `faust-tremolo` | Faust DSL effects compiled into Pulp processors | `pulp build --examples --target pulp-faust-gain-test` (likewise `-filter`, `-tremolo`) |
| `jsfx-gain`, `jsfx-delay`, `jsfx-tremolo` | Source-only examples for the bounded JSFX subset | `python3 tools/scripts/jsfx_subset.py doctor --file examples/jsfx-gain/PulpJsfxGain.jsfx` |
| `cmajor-gain` | Source-only Cmajor patch for the external-toolchain workflow | See `examples/cmajor-gain/README.md` |
| `rust-gain` | Rust DSP core packaged as a CLAP plugin; needs `PULP_BUILD_NATIVE_COMPONENT_RUST_TESTS` | `pulp build --examples --target PulpRustGain_CLAP` |
| `modal-specs` | Modal synthesis parameter files (JSON only), loaded from disk by the modal spec test | `pulp build --target pulp-test-modal-spec` |
| `offline-stretch` | Command-line harness for the offline time-stretch engine | `pulp build --examples --target stretchcli` |
| `audio-rate-processor-node` | Processor that consumes a per-sample control signal (**SDK project**) | See `examples/audio-rate-processor-node/README.md` |

### Graph, timeline and tuning

| Directory | What it shows | Build and run |
|---|---|---|
| `graph-hello` | Smallest signal-graph program (**SDK project**) | Configure as an SDK project, then run `pulp-graph-hello` |
| `graph-named-bus-cookbook` | Named buses through the format layer (**SDK project**) | Configure as an SDK project, then run `pulp-graph-named-bus-cookbook` |
| `graph-spatial-cookbook` | Fixed left and right placement of a mono signal (**SDK project**) | Configure as an SDK project, then run `pulp-graph-spatial-cookbook` |
| `g11-tuning-cookbook` | Asking the public tuning provider for a note's frequency (**SDK project**) | Configure as an SDK project, then run `pulp-graph-tuning-cookbook` |
| `timeline-phase1` | Three arrangements: decoded audio, a step pattern, multitrack with automation | `pulp build --examples --target pulp-timeline-audio-player` |
| `timeline-session` | Launch session, DAW-style project and project session, all headless | `pulp build --examples --target pulp-timeline-daw-project` |
| `timeline-plugin-proof` | Loadable plugin that owns a timeline document and a piano roll editor | `pulp build --examples --target TimelinePluginProof_CLAP` |

### Platforms, tooling and diagnostics

| Directory | What it shows | Build and run |
|---|---|---|
| `ios-auv3-synth` | Minimal AUv3 instrument for iOS and iPadOS | See `examples/ios-auv3-synth/README.md` |
| `ios-auv3-chainer` | AUv3 extension plus a SwiftUI host app that mounts its view | See its `CMakeLists.txt` |
| `ios-auv3-gpu-smoke` | Minimal 2D GPU editor in an AUv3 | See its `CMakeLists.txt` |
| `ios-auv3-jsc-threejs` | AUv3 whose editor draws a 3D scene through the JavaScriptCore engine | See `examples/ios-auv3-jsc-threejs/README.md` |
| `android-synth` | Scripted synth on Android with GPU rendering | See `examples/android-synth/README.md` |
| `web-demos` | Plugins compiled to WebAssembly and run in a browser | See `examples/web-demos/README.md` |
| `forge-modular` | Modular synth module set; needs the Rack SDK (`PULP_HAS_RACK`) | `pulp build --examples --target ForgeModularApp` |
| `plugin-host-demo` | Minimal host that scans and loads a plugin | `pulp build --examples --target pulp-plugin-host-demo` |
| `stream-demo` | File streams, event-loop async reads and HTTP GET | `pulp build --examples --target pulp-stream-demo` |
| `trace-demo` | Perfetto trace that exposes one expensive DSP node; needs `PULP_TRACING` | `pulp build --examples --target pulp-trace-demo` |
| `trace-plugin-chain` | Trace of a real three-plugin chain; needs `PULP_TRACING` | `pulp build --examples --target pulp-trace-plugin-chain` |
| `host-bench-plugin` | Plugin that logs host behavior for the DAW validation scripts | `pulp build --examples --target PulpHostBench_Standalone` |
| `capability-control` | Generated CLI and MCP walkthroughs for the control surface | `python3 examples/capability-control/generate_examples.py` |

## Proof consumers and test-only projects

These directories prove a contract and are exercised by CI. Do not copy them as a starting point.

| Directory | What it proves | Build and run | Planned location |
|---|---|---|---|
| `clap-identity-consumer` | Installed SDK exposes the CLAP identity surface (**SDK project**) | Configure as an SDK project | `test/consumers/clap-identity-consumer` |
| `faust-rt-sdk-consumer` | Faust runtime builds against the installed SDK (**SDK project**) | `ctest -R cmake-faust-rt-sdk-consumer` | `test/consumers/faust-rt-sdk-consumer` |
| `g10-routed-consumer` | Routed spectral ports work from an installed SDK (**SDK project**) | Configure as an SDK project | `test/consumers/g10-routed-consumer` |
| `g2-processor-restore-consumer` | A registered processor restores its state from an installed SDK (**SDK project**) | `ctest -R cmake-g2-processor-restore-sdk-consumer` | `test/consumers/g2-processor-restore-consumer` |
| `g3-io-gain-consumer` | Identity of an input and output gain graph, checked by a Python script | `python3 test/test_g3_io_gain_identity.py` | `test/consumers/g3-io-gain-consumer` |
| `g5-event-consumer` | Reference event trace for two legal partitions | `python3 examples/g5-event-consumer/test_check_trace.py` | `test/consumers/g5-event-consumer` |
| `g5-host-event-consumer` | Public host event boundary from an external program (**SDK project**) | `ctest -R g5-host-event-public-consumer` | `test/consumers/g5-host-event-consumer` |
| `g5-trigger-admission-consumer` | Graph planner admits trigger edges in a release build (**SDK project**) | `ctest -R trigger-admission-public-consumer` | `test/consumers/g5-trigger-admission-consumer` |
| `g5-trigger-runtime-consumer` | Graph-produced onsets render through a typed trigger edge (**SDK project**) | `ctest -R trigger-runtime-public-consumer` | `test/consumers/g5-trigger-runtime-consumer` |
| `g6-voice-contract-consumer` | Existing MIDI voice allocation state (**SDK project**) | `ctest -R cmake-g6-voice-contract-sdk-consumer` | `test/consumers/g6-voice-contract-consumer` |
| `g7-latency-pdc-consumer` | Public latency reporting on a graph (**SDK project**) | Configure as an SDK project | `test/consumers/g7-latency-pdc-consumer` |
| `spectral-frame-codec-consumer` | Spectral frame codec from an installed SDK (**SDK project**) | `ctest -R cmake-spectral-frame-codec-consumer` | `test/consumers/spectral-frame-codec-consumer` |
| `timeline-sdk-consumer` | Timeline SDK closure, cookbook and chord renderer from an installed SDK (**SDK project**) | `ctest -R cmake-timeline-sdk-consumer` | `test/consumers/timeline-sdk-consumer` |
| `graph-agent-loop` | Bounded agent loop over the public graph API (**SDK project**) | Configure as an SDK project | `test/consumers/graph-agent-loop` |
| `graph-edge-probe` | Graph edge kinds from an installed SDK (**SDK project**) | Configure as an SDK project | `test/consumers/graph-edge-probe` |
| `gpu-fault-continuity` | Plugin that injects GPU faults for the host smoke test; needs `PULP_GPU_AUDIO_FAULT_HOOKS` | `pulp build --examples --target PulpGpuFaultContinuity_CLAP` | `test/consumers/gpu-fault-continuity` |
| `text-input-test` | Text editor widget in a window | `pulp build --examples --target pulp-text-input-test` | `test/consumers/text-input-test` |
| `native-ui-phase-f-preview` | Preview of a generated native editor; builds only on macOS with the design-import option and a generated editor source present | `pulp build --examples --target pulp-native-ui-phase-f-preview` | `test/consumers/native-ui-phase-f-preview` |

### The move to `test/consumers/`

The planned-location column is a plan. The directories have not moved. Each one is named by CMake tests, documentation pages, the skill path map, the sequencer exposure ledger, and in one case a CI path filter and a web demo build, so the move needs those paths updated in the same change and a full run of the SDK consumer smoke tests. `tools/scripts/examples_index_guard.py` keeps this page in step with the directory in the meantime: it fails when a directory is missing from the tables or listed twice.
