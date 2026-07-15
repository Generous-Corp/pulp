# Interrogating and Comparing Third-Party Plugins

Pulp can inspect and render installed CLAP, VST3, Audio Unit, AUv3, and LV2
plugins without a DAW or audio device. This makes a plugin's **observable host
contract** available to scripts and agents: parameters, audio/MIDI behavior,
latency, tails, and deterministic output under controlled stimuli.

It does not reveal source code or prove which internal algorithm a plugin uses.
It lets you form and test useful black-box hypotheses: whether a control changes
pitch, time, transients, stereo width, spectral balance, latency, or tail
behavior, and how your own plugin differs under the same experiment.

## The core loop

First discover the API the plugin exposes to every host:

```bash
pulp audio plugin-inspect \
  --plugin "/path/to/Reference.component" --format au --json \
  > /tmp/reference-params.json
```

The result includes identity and bus metadata plus every host-visible parameter's
numeric ID, name, unit, plain-domain range, default/current value, and flags.
Use those IDs to run controlled captures:

```bash
# Effect: deterministic stimulus, initialized state, discarded settling, and tail.
pulp audio render \
  --plugin "/path/to/Reference.component" --format au \
  --input-signal noise:7 --duration-ms 2000 --tail-ms 500 \
  --warmup-ms 1000 --initial-param 12=0.75 --settle-ms 300 \
  --wav-format float32 --out /tmp/reference.wav --json

# Instrument: no input bus; MIDI drives the capture.
pulp audio render \
  --plugin "/path/to/Instrument.vst3" --format vst3 --in-channels 0 \
  --midi note:60,100,0,48000 --duration-frames 72000 \
  --wav-format float32 --out /tmp/instrument.wav --json
```

Render your plugin with the same sample rate, block size, stimulus, events,
duration, and output format. `pulp audio validate compare` is the stock,
deterministic residual gate for equivalent outputs. Rich perceptual A/B judgment
is an optional managed tool:

```bash
pulp tool install audio-quality-lab       # once, when rich comparison is wanted
pulp audio compare /tmp/reference.wav /tmp/candidate.wav \
  --profile transient-integrity --reference-role peer
```

Inspection, rendering, WAV metrics, and deterministic validation are part of
Pulp and do **not** require Audio Quality Lab. If the managed tool is absent,
`pulp audio compare` stops before analysis and prints the exact install command;
it never silently downloads a dependency.

## Useful experiments

- Sweep one parameter while holding everything else fixed, then compare output
  metrics or WAVs to map its audible effect.
- Probe freeze and time-stretch processors with impulses, clicks, tones, noise,
  speech, and rhythmic material. Vary capture length and `--tail-ms` to expose
  hold, release, smearing, pitch preservation, and temporal behavior.
- Render a reference plugin and your implementation from one scenario, then use
  residual checks for intended parity and Quality Lab axes for perceptual
  differences such as graininess or transient integrity.
- Repeat the same scenario across commits to create golden renders and catch
  regressions in sound, latency, state initialization, or automation timing.

A good experiment changes one independent variable at a time. Similar output is
evidence about behavior, not evidence that two implementations share an
algorithm. Respect plugin licenses and do not distribute vendor binaries or
captured assets you do not have permission to share.

## Automation and agents

The Claude Code plugin teaches the same workflow through `/audio-harness` and
`/audio-compare`. MCP clients can call `pulp_audio_plugin_inspect`,
`pulp_audio_render`, and `pulp_audio_compare` directly. The first call discovers
parameter IDs/ranges; later calls choose a stimulus and parameter state, render
reference and candidate WAVs, and interpret structured JSON. No GUI is required.

Plugins are loaded and processed in disposable child processes with bounded
timeouts. That contains ordinary crashes and hangs; it is **not** a security
sandbox for malicious code. AU workers also service bounded native event-loop
slices during warm-up and between blocks so asynchronous initialization can
advance. Warm-up and settling remain configurable because no generic host API
can declare when a particular licensed plugin is ready.

## What is not automated yet

Native vendor editor windows are not hosted or captured by this workflow, and
Pulp does not infer parameter gestures by watching mouse movements. Most plugin
controls already expose a host parameter API, so numeric control is the reliable
starting point. Editor launch, accessibility-driven interaction, screenshot
capture, and gesture recording/replay can be layered on later for controls that
are truly GUI-only; they should complement, not replace, the deterministic host
API path.

For embedding the host APIs in a C++ application, see [Hosting Plugins in
Pulp](hosting.md).
