# Audio Scope Project Plan

## Status

Draft for review. This is a planning artifact, not an implementation
commitment. The goal is to decide whether a scope adds enough value beyond the
live Audio Inspector before building it.

## Where this plan lives

This file lives in `docs/analysis/` because this work is a product and
architecture proposal that needs review before implementation. The current
worktree's `planning/` directory is empty/uninitialized, and local checks report
planning fixtures as optional missing paths. `docs/analysis/` already contains
repo-owned project plans, is available to CI and reviewers, and can be linked
from PRs, issues, MCP guidance, and agent skills without depending on a separate
planning checkout.

If the planning submodule later becomes the preferred durable roadmap location,
this document can be copied there as the canonical plan and this file can
become a short pointer.

## Why this exists

The current Audio Inspector answers a live diagnostic question:

> Is audio flowing through this host boundary, and is the buffer obviously
> broken?

That tool should stay honest and cheap. It shows observed samples and scalar
health counters with minimal display manipulation. It is useful for silence,
clipping, NaN/Inf, callback progress, stale probes, DC offset, discontinuities,
dropouts, and device stress.

An oscilloscope answers a different question:

> What is the shape, timing, and stability of this signal over a controlled
> acquisition window?

That can be valuable when a developer or agent needs to understand periodic or
event-like audio, not just determine whether the audio path is alive. The scope
is justified only if it becomes an analysis surface with reusable CLI, MCP, and
developer primitives, not just a nicer moving sine wave.

## Product distinction

Use **Signal Inspector** for host health and buffer truth:

- raw current buffer display by default
- no invented samples
- no temporal smoothing
- no fake persistence
- focused counters: peak, RMS, clip, NaN/Inf, silence run, callbacks, xruns
- fast live debugging while a standalone host runs
- one-shot JSON for agents via `pulp run --audio-probe-json`

Use **Scope** for signal analysis:

- triggered acquisition
- stable timebase
- configurable horizontal and vertical scale
- optional rendering interpolation for display only
- optional persistence/phosphor, clearly labeled
- grid, cursors, and measurements
- frequency, period, peak-to-peak, RMS, DC offset, crest factor
- freeze/export for reviewable artifacts

The important boundary: Signal Inspector should show what the host published.
Scope may choose and render an acquisition window to make a signal readable,
but it must label any display processing and keep raw data available.

## Recommended user-facing shape

Start with one developer window that contains two modes:

- **Signal**: the current Audio Inspector surface and default mode.
- **Scope**: an analysis mode backed by the same probe data but with scope
  semantics.

This keeps discovery simple because developers already know how to open the
Audio Inspector. It also avoids proliferating floating windows before there is
enough feature depth to justify a separate tool.

Persist the selected mode in developer settings. If a user prefers Scope, the
window should reopen in Scope until they change it. This preference is UI state
only; it must not affect probe JSON, renders, plugin state, or shipped builds.

Keep the implementation modular enough that Scope can become a separate window
later if the tool grows. The shared pieces should be named around audio
observation and analysis, not around a single window.

## Alternative considered: separate tools

A separate `Audio Scope` window is cleaner conceptually and may be appropriate
later if Scope gains multiple channels, protocol-specific trigger modes,
modular-synth workflows, or heavy controls. The downside is discoverability and
duplicated host plumbing today.

The recommended first step is one window with separate modes because the data
source, lifetime, and dev-only gating are shared. The architecture should still
keep the mode views independent so splitting the UI later is mechanical.

## Non-goals

- Do not turn the Signal Inspector into an oscilloscope by default.
- Do not add a signal generator just to have something to look at.
- Do not ship customer-facing debugging UI in release plugin builds.
- Do not make live scope analysis the offline Audio Doctor path.
- Do not require audible output for headless agent tests.
- Do not add spectral analysis, THD, transfer-function analysis, MIDI-CV
  calibration, or modular-synth protocol features in the first slice.

## Useful scenarios

Developer debugging:

- A synth voice emits clicks; Scope can freeze a transient and show the
  discontinuity around note-on.
- A modulation path produces a sub-audio wobble; Scope can zoom horizontally
  and measure period.
- A stereo effect unexpectedly inverts one channel; Scope can overlay channels
  and show phase relationship.
- A compressor or waveshaper clips internally; Scope can show peak-to-peak and
  crest-factor changes at the observed stage.

Agent workflows:

- An agent can run a headless scope capture after a code change and compare
  measurements without opening a window.
- MCP can return structured facts like frequency estimate, DC offset,
  zero-crossing stability, clipping, and captured window metadata.
- Claude/Codex plugin workflows can decide whether to run a deeper offline
  Audio Doctor render based on scope anomalies.

Developer primitives:

- Plugin authors can reuse a scope widget in dev-only standalone tools.
- Test harnesses can capture bounded waveform windows as artifacts.
- Future hardware-facing plugins could use the same primitives to visualize
  control-rate or audio-rate signals from external equipment, if the host
  exposes a safe input/probe source.

## CLI and MCP value

The UI is only one consumer. The valuable primitive is a bounded acquisition
plus measurements.

Potential CLI:

```bash
pulp audio scope TARGET --frames 120 --trigger rising-zero --window 2048 \
  --json scope.json --png scope.png
```

Potential MCP method:

```json
{
  "target": "pulp-audio-inspector-demo",
  "frames": 120,
  "trigger": "rising-zero",
  "window_samples": 2048,
  "measure": ["frequency", "period", "dc_offset", "peak_to_peak"]
}
```

Potential structured result:

```json
{
  "stage": "standalone_output_boundary",
  "sample_rate": 48000,
  "channel_count": 2,
  "window_samples": 2048,
  "trigger": "rising-zero",
  "trigger_sample": 317,
  "measurements": {
    "frequency_hz": 440.1,
    "period_samples": 109.1,
    "peak_to_peak": 0.72,
    "dc_offset": 0.001,
    "rms": 0.254,
    "crest_factor": 1.41
  },
  "warnings": []
}
```

This gives agents something more useful than a screenshot: facts they can
reason over, plus optional visual artifacts for human review.

## Architecture

Keep the system split into small layers:

- **Probe source**: existing live `AudioProbeSnapshot` and waveform ring data.
- **Acquisition**: selects a bounded window from raw samples, applies trigger
  rules, and records metadata about the chosen window.
- **Measurement**: computes numeric facts from the acquired raw samples.
- **Rendering model**: prepares grid, trace, cursor, and persistence display
  data without changing the underlying samples.
- **View**: draws Signal and Scope modes as separate components.
- **CLI/MCP adapters**: call acquisition and measurement without needing a
  visible window.

Suggested source boundaries:

- `core/audio/`: acquisition structs, trigger logic, measurement helpers.
- `core/view/`: scope widgets and mode container.
- `tools/cli/` and MCP command handlers: one-shot scope capture wrappers.
- `test/`: pure acquisition/measurement tests, view rendering smoke tests,
  CLI/MCP contract tests.

The acquisition and measurement layer should have no dependency on the UI
toolkit. The UI should be a consumer, not the owner of scope semantics.

## State and preferences

Persist only UI preferences:

- selected mode: `signal` or `scope`
- scope grid visible
- trigger mode
- horizontal scale/timebase
- vertical gain
- channel display mode

Do not persist captured samples in normal app settings. Captures should be
explicit artifacts when requested by CLI, MCP, or a human export action.

The selected mode should be remembered for the developer window because that is
user preference. CLI and MCP commands should be explicit and stateless.

## First implementation slice

Build the smallest useful Scope that proves the value:

1. Add pure acquisition helpers:
   - raw window
   - rising-zero trigger
   - fallback to raw window when no trigger exists
   - metadata for trigger position and source sequence
2. Add pure measurements:
   - peak-to-peak
   - RMS
   - DC offset
   - zero-crossing frequency estimate for periodic signals
3. Add Scope mode in the existing Audio Inspector window:
   - mode segmented control: Signal / Scope
   - remembered selected mode
   - grid
   - trigger off / rising-zero
   - horizontal scale
   - freeze
4. Add CLI/MCP one-shot scope capture:
   - JSON measurements first
   - PNG artifact optional if rendering support is cheap
5. Add docs:
   - when to use Signal vs Scope
   - audible/headless guidance
   - limitations and non-goals

## Later slices

- channel overlay and channel selection
- falling-edge and level trigger
- trigger holdoff
- cursor measurements
- persistence/phosphor display
- export captured raw window as WAV/JSON
- compare two captures for regression detection
- input-side or named-stage probes, if the host exposes them safely
- richer hardware-facing workflows after a concrete user story exists

## Testing plan

Pure tests:

- trigger chooses only existing sample indices
- no-trigger fallback returns the raw window
- measurements handle silence, DC, sine, clipped signal, NaN/Inf, and short
  buffers
- frequency estimator reports unavailable rather than guessing on non-periodic
  data

UI tests:

- Signal remains the default for new users
- selected mode persists across reopen
- Scope mode renders a non-empty headless snapshot
- freeze stops display updates without stopping probe publication

CLI/MCP tests:

- scope JSON schema is stable
- headless scope capture exits without requiring a visible window
- no audible-output path is used unless explicitly running the live host
- warnings explain unavailable measurements

Regression guard:

- `--audio-probe-json` output must not change just because Scope exists.

## Open questions for review

- Should the window title remain `Audio Inspector`, or should it become
  `Audio Tools` with `Signal` and `Scope` modes?
- Is `Scope` clear enough, or should the UI say `Oscilloscope` even if the CLI
  uses shorter command names?
- Do we need PNG output in the first CLI/MCP slice, or is structured JSON
  enough?
- Should the first scope source be output-boundary only, or should the plan
  include named probe stages before implementation?
- Is there a concrete hardware/modular-synth scenario that justifies input-side
  probing, or should that remain deferred?

## Decision criteria

Proceed only if the first slice can deliver at least two of these:

- faster diagnosis of real plugin signal-shape bugs than Signal Inspector alone
- useful MCP/CLI measurements that agents can act on
- reusable acquisition/measurement primitives independent of UI
- clear docs that prevent Signal Inspector and Scope from being confused

If the work only produces a nicer waveform animation, do not build it.
