# Example UI Screenshot Notes

Visual notes for the Pulp example plugin UIs plus the local demo/script tools
that generate screenshot artifacts.

## Generating Screenshots

`pulp-screenshot` renders the built-in demo UI or a JavaScript UI file. It
does not load a plugin by name. Use the standalone/plugin validation paths for
actual plugin editor screenshots.

```bash
# Capture the built-in demo UI
./build/tools/screenshot/pulp-screenshot --demo --output docs/examples/img/pulp-demo.png

# Capture the built-in demo at common gallery viewport sizes
./tools/scripts/capture-all-screenshots.sh

# Capture a scripted UI file with a specific theme
./build/tools/screenshot/pulp-screenshot --script path/to/ui.js --theme dark --output docs/examples/img/ui-dark.png
```

## PulpGain — Stereo Gain Effect

- **Type:** Effect (stereo in → stereo out)
- **Parameters:** Input Gain (dB), Output Gain (dB), Bypass
- **UI:** Two knobs + one toggle, auto-generated via AutoUi
- **Formats:** VST3, AU v2/AUv3, CLAP, Standalone, optional AAX

## PulpTone — Polyphonic Synth

- **Type:** Instrument (MIDI in → stereo out)
- **Parameters:** Waveform, Frequency, Attack, Release
- **UI:** Three knobs + one combo, auto-generated via AutoUi
- **Formats:** VST3, AU v2, CLAP, Standalone

## PulpEffect — Biquad Filter

- **Type:** Effect (stereo in → stereo out)
- **Parameters:** Frequency (Hz), Resonance, Gain (dB), Filter Type, Bypass
- **UI:** Three knobs + one combo + one toggle, auto-generated
- **Formats:** VST3, AU v2, CLAP

## PulpCompressor — Sidechain Compressor

- **Type:** Effect with sidechain (stereo + sidechain in → stereo out)
- **Parameters:** Threshold (dB), Ratio, Attack (ms), Release (ms), Makeup (dB), Bypass
- **UI:** Five knobs + one toggle, auto-generated
- **Formats:** VST3, AU v2, CLAP

## PulpSynth — Macro Oscillator Synth

- **Type:** Instrument (MIDI in → stereo out)
- **Parameters:** 10 parameters (oscillator, filter, envelope, output)
- **UI:** 10 knobs, auto-generated
- **Formats:** CLAP only

## PulpDrums — Generative Drum Sequencer

- **Type:** MIDI Effect (MIDI in → MIDI + audio out)
- **Parameters:** Pattern, Tempo, Swing, Density, Velocity, Randomize
- **UI:** Six knobs, auto-generated
- **Formats:** CLAP only

## PulpSampler — Streaming Sampler

- **Type:** Instrument (MIDI in → stereo out)
- **Parameters:** Gain, Attack, Decay, Sustain, Release, Pitch, Loop, Reverse,
  Interpolation
- **UI:** Gain, ADSR, Pitch, Loop, Reverse, and Interpolation controls,
  auto-generated
- **Engine:** Eight voices; resident or bounded ranged-file playback; forward
  and reverse one-shots/loops; selectable interpolation, octave mips,
  starvation fades, and optional synthetic heritage processing
- **Formats:** CLAP only

## GPU Demo — Modulation Matrix

- **Type:** Standalone GPU demo (not a plugin)
- **Features:** Animated patcher cables, hover labels, click-to-toggle
- **Rendering:** Dawn Metal → Skia Graphite → on-screen at 60fps
- **Purpose:** Validates GPU rendering pipeline end-to-end

## Screenshot Automation

Screenshot capture is currently a local/docs-maintenance workflow, not a
published `gh-pages` branch flow. The helper script renders demo viewport
presets only; it does not select or load plugin bundles.

1. Build the screenshot tool for demo/script captures
2. For plugin editor screenshots, build the relevant standalone or plugin bundle
3. Use `pulp run --headless --screenshot <file>` for standalone screenshots, or
   `pulp validate --screenshot` for validation-harness plugin screenshots
4. Save PNGs to `docs/examples/img/`
5. Publish the resulting assets through the docs-site path you are actually
using; there is no `gh-pages` branch deploy in the current repo setup
