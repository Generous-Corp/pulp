# Example Plugin Screenshots

Visual documentation of each Pulp example plugin's auto-generated UI.

## Generating Screenshots

Screenshots are captured via the headless screenshot tool:

```bash
# Capture a single plugin's UI
./build/tools/screenshot/pulp-screenshot --demo --plugin PulpGain --output docs/examples/img/pulp-gain.png

# Capture all examples
./tools/scripts/capture-all-screenshots.sh

# Capture with a specific theme
./build/tools/screenshot/pulp-screenshot --theme dark --plugin PulpGain --output pulp-gain-dark.png
```

## PulpGain — Stereo Gain Effect

- **Type:** Effect (stereo in → stereo out)
- **Parameters:** Input Gain (dB), Output Gain (dB), Bypass
- **UI:** Two knobs + one toggle, auto-generated via AutoUi
- **Formats:** VST3, AU, CLAP, Standalone

## PulpTone — Polyphonic Synth

- **Type:** Instrument (MIDI in → stereo out)
- **Parameters:** Waveform, Frequency, Attack, Release
- **UI:** Three knobs + one combo, auto-generated via AutoUi
- **Formats:** VST3, AU, CLAP, Standalone

## PulpEffect — Biquad Filter

- **Type:** Effect (stereo in → stereo out)
- **Parameters:** Frequency (Hz), Resonance, Gain (dB), Filter Type, Bypass
- **UI:** Three knobs + one combo + one toggle, auto-generated
- **Formats:** VST3, AU, CLAP

## PulpCompressor — Sidechain Compressor

- **Type:** Effect with sidechain (stereo + sidechain in → stereo out)
- **Parameters:** Threshold (dB), Ratio, Attack (ms), Release (ms), Makeup (dB), Bypass
- **UI:** Five knobs + one toggle, auto-generated
- **Formats:** VST3, AU, CLAP

## PulpSynth — Macro Oscillator Synth

- **Type:** Instrument (MIDI in → stereo out)
- **Parameters:** 10 parameters (oscillator shape, filter, envelope, effects)
- **UI:** Eight knobs + two toggles, auto-generated
- **Formats:** CLAP only

## PulpDrums — Generative Drum Sequencer

- **Type:** MIDI Effect (MIDI in → MIDI + audio out)
- **Parameters:** Pattern, Tempo, Swing, Density, Velocity, Randomize
- **UI:** Four knobs + two combos, auto-generated
- **Formats:** CLAP only

## PulpSampler — Audio File Sampler

- **Type:** Instrument (MIDI in → stereo out)
- **Parameters:** Volume, Attack, Decay, Sustain, Release, Pitch, Loop
- **UI:** Five knobs + two toggles, auto-generated
- **Formats:** CLAP only

## GPU Demo — Modulation Matrix

- **Type:** Standalone GPU demo (not a plugin)
- **Features:** Animated patcher cables, hover labels, click-to-toggle
- **Rendering:** Dawn Metal → Skia Graphite → on-screen at 60fps
- **Purpose:** Validates GPU rendering pipeline end-to-end

## Screenshot Automation

Screenshot capture is currently a local/docs-maintenance workflow, not a
published `gh-pages` branch flow:

1. Build all plugins and the screenshot tool
2. For each plugin, render the AutoUi at default size
3. Save PNGs to `docs/examples/img/`
4. Publish the resulting assets through the docs-site path you are actually
using; there is no `gh-pages` branch deploy in the current repo setup
