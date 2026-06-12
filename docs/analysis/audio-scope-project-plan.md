# Audio Scope Project Plan

## Status

Implementation plan and progress log, hardened against the code at
`fix/audio-run-audible-notice` (which includes the Audio Inspector waveform
polish). Reviewed with RepoPrompt context-building on 2026-06-12. The plan was
originally shaped as five independently shippable phases; the implementation
branch is intentionally carrying the slices together so CLI/MCP/UI/offline
behavior can be reviewed as one coherent Audio Scope surface.

## Progress

- 2026-06-12: Preserved this hardened plan in the `pulp-planning` repo on
  `main` as `2026-06-12-audio-scope-project-plan.md` (`92e1960`).
- 2026-06-12: Started Phase 1 in `core/audio`: added shared scope acquisition,
  trigger, measurement, and JSON primitives plus tests in the existing
  `pulp-test-audio-probe` binary. Validated in the Release
  `build-audio-scope` tree with
  `cmake --build build-audio-scope --target pulp-test-audio-probe -j8` and
  `build-audio-scope/test/pulp-test-audio-probe "[audio][scope]"`.
- 2026-06-12: Implemented Phase 2 plumbing for live one-shot scope JSON:
  low-level `pulp run --audio-scope-json`, standalone scope JSON helper,
  `pulp audio scope`, and MCP `pulp_audio_scope`. Focused validation passed in
  `build-audio-scope` with parser, MCP, scope-core, and non-audible standalone
  helper tests. The existing `build`/`build-cov` trees currently reconfigure
  with `PULP_ENABLE_GPU=ON` and fail before compile because local Skia is
  missing; validation is therefore using the dedicated GPU-off Release tree.
- 2026-06-12: Implemented Phase 3 Signal/Scope mode in the existing Audio
  Inspector window. Signal remains the default diagnostic view; Scope uses the
  shared `core/audio` acquisition/measurement helpers on copied samples and
  persists `audio_inspector.mode` through `ApplicationProperties` /
  `PropertiesFile`.
- 2026-06-12: Implemented Phase 4 multichannel capture in `AudioProbe` while
  preserving the legacy channel-0 reader. The new multichannel reader drains the
  same single-consumer FIFO and feeds live Scope JSON/MCP acquisition without a
  second trigger implementation.
- 2026-06-12: Implemented Phase 5 speakerless offline source for
  `pulp audio scope --input-wav`, plus optional offline `--png` trace artifact.
  MCP `pulp_audio_scope` mirrors the live/offline split with `target` and
  `input_wav` kept mutually exclusive.
- 2026-06-12: Focused headless validation passed in `build-audio-scope`:
  `pulp-test-audio-probe "[audio][scope]"`,
  `pulp-test-audio-probe "[audio-probe][audio-scope]"`,
  `pulp-test-audio-inspector-window "[audio-scope]"`,
  `pulp-test-mcp-server "[scope]"`,
  `pulp-test-cli-run-options "[audio-scope]"`, and
  `pulp-test-standalone-editor-chrome "[audio-scope]"`. Remaining local caveat:
  `cmd_audio.cpp` is still not compiled by the GPU-off tree because `pulp-cli`
  is only added when `PULP_ENABLE_GPU=ON`; the local GPU-on trees are blocked by
  the missing Skia bundle before the CLI target can compile.

## Where this plan lives

This file intentionally lives in two places:

- `docs/analysis/audio-scope-project-plan.md` in the main repo is the public
  technical proposal tied to the code under review. It can be linked from PRs,
  issues, docs, CLI/MCP guidance, and skills without requiring a separate
  planning checkout.
- `planning/2026-06-12-audio-scope-project-plan.md` in the planning repo is the
  durable roadmap/handoff copy. It protects the idea if the implementation
  branch is rewritten and gives future agents a stable planning-repo anchor.

The main-repo copy should stay close to the implementation while this branch is
active. The planning copy should be kept in sync at handoff/commit points, not
used as a divergent spec.

## Relationship to the audio observability roadmap

This plan extends — and must not fork — the architecture established by
`planning/2026-06-09-audio-observability-and-validation-harness-plan.md` and
`planning/2026-06-09-audio-inspector-separate-tool-window-proposal.md` (both in
the private planning submodule):

- The harness build-out (offline generators → metrics → assertions → Doctor;
  live RT probe → `AudioInspectorWindow`) is **merged to main**. Scope builds
  on that substrate; it does not re-architect it.
- "Live spectrum + freeze/export in the inspector" was explicitly deferred from
  the harness Phase 6. **Scope is the vehicle for the time-domain half of that
  deferred work** (freeze, triggered acquisition, export). Live spectrum stays
  deferred — it needs an off-callback FFT path and is out of scope here.
- Controlled-stimulus analysis (THD, response curves, transfer functions)
  belongs to the offline **Audio Doctor**, not Scope. Scope observes what is
  flowing; Doctor stimulates and measures offline.
- The harness plan's "Phase 7-live" (flight-recorder ring + `pulp audio
  validate` live verbs) is separate, un-started work. Scope's multi-channel
  capture extension (Phase 4 below) should be designed so Phase 7-live can
  reuse it, but neither blocks the other.

## Why this exists

The current Audio Inspector answers a live diagnostic question:

> Is audio flowing through this host boundary, and is the buffer obviously
> broken?

That tool stays honest and cheap: observed samples, scalar health counters,
minimal display manipulation. An oscilloscope answers a different question:

> What is the shape, timing, and stability of this signal over a controlled
> acquisition window?

The scope is justified only if it becomes an analysis surface with reusable
CLI, MCP, and developer primitives — not just a nicer moving sine wave.

## Product distinction

Use **Signal Inspector** (the existing Audio Inspector surface) for host health
and buffer truth:

- raw current buffer display by default; no invented samples, no temporal
  smoothing, no fake persistence
- focused counters: peak, RMS, clip, NaN/Inf, silence run, callbacks, drops
- fast live debugging while a standalone host runs
- one-shot JSON for agents via `pulp run --audio-probe-json` (schema frozen)

Use **Scope** for signal analysis:

- triggered acquisition over a bounded window
- stable timebase, configurable horizontal/vertical scale
- grid, freeze, measurements: frequency, period, peak-to-peak, RMS, DC offset,
  crest factor
- versioned JSON capture artifacts for agents and regression comparison

The boundary: Signal Inspector shows what the host published. Scope may choose
and render an acquisition window to make a signal readable, but it must label
any display processing and keep raw data available. Neither surface ever
fabricates samples.

## Verified current state (build on this, don't duplicate it)

Verified against the code at this branch:

| Surface | What exists | File |
|---|---|---|
| RT producer | `AudioProbe::analyze_output()` — strict RT ABI (no alloc/locks/log/FFT), publishes `AudioProbeSnapshot` via `TripleBuffer`, optional last-N capture ring (AbstractFifo, drop-on-full, `dropped_capture_frames` accounting). **`read_capture()` drains channel 0 only.** | `core/audio/include/pulp/audio/audio_probe.hpp`, `core/audio/src/audio_probe.cpp` |
| Shared schema | `AudioProbeSnapshot` — POD, identity tuple `{sample_rate, block_size, channel_count, stage_id, sequence_number}`, per-channel peak/RMS, content counters, drop/stale accounting | `core/audio/include/pulp/audio/audio_probe_snapshot.hpp` |
| Probe JSON | `audio_probe_snapshot_to_json()` — flat schema consumed by `pulp run --audio-probe-json` and MCP `pulp_audio_probe_json`. **Frozen; Scope must not change it.** | `core/audio/{include/pulp/audio/audio_probe_json.hpp,src/audio_probe_json.cpp}` |
| Inspector window | Polls `latest()` once per UI tick, sequence-based stale detection, drains channel-0 capture, pushes into panel. Display-only env knobs `PULP_AUDIO_INSPECTOR_{TRIGGER,GRID,SCALE}`. | `core/view/src/audio_inspector_window.cpp` |
| Waveform view | `AudioWaveformView` already has display-side `TriggerMode {kRaw, kRisingZero}`, grid, horizontal scale, `display_start_index()`, live/stale honesty | `core/view/include/pulp/view/audio_inspector_panel.hpp` |
| Standalone host | `output_probe_` prepared behind `PULP_ENABLE_AUDIO_PROBES`, fed post-`process()`, delayed headless probe-JSON one-shot; probe-off builds reject probe JSON rather than faking silence | `core/format/src/standalone.cpp`, `core/format/include/pulp/format/detail/standalone_{environment,audio_probe_json}.hpp` |
| CLI | `pulp run --audio-inspector` / `--audio-probe-json` (implies UI-headless but still opens the live audio device; pre-launch audible notice) | `tools/cli/cmd_run*.cpp` |
| MCP | `pulp_audio_probe_json` shells out to `pulp run --audio-probe-json` into a private temp file and returns normalized structuredContent | `tools/mcp/mcp_tools.cpp`, `tools/mcp/pulp_mcp.cpp` |
| Off-RT analysis precedent | RMS window / zero-crossing / correlation helpers for style reference | `core/audio/src/audio_analysis_detail.hpp`, `loop_point_analyzer.hpp` |
| UI prefs precedent | `state::ApplicationProperties` (platform-standard paths, per-app-name) and `view::AppSettings` | `core/state/include/pulp/state/properties_file.hpp`, `core/view/include/pulp/view/app_framework.hpp` |
| CLI family | `pulp audio` subcommand family exists (model / excerpt / validate) — `pulp audio scope` slots in naturally | `docs/status/cli-commands.yaml` |

Two consequences:

1. The draft plan's "first slice" UI items (trigger, grid, horizontal scale)
   **already exist as display-side logic** in `AudioWaveformView`. The work is
   to promote acquisition/trigger/measurement into UI-independent `core/audio`
   helpers, then have both surfaces consume them — not to build the UI twice.
2. Channel overlay cannot be truthful until the capture ring carries
   per-channel data. That gap is sequenced explicitly (Phase 4); earlier
   phases label their source as channel 0.

## Recommended user-facing shape

One developer window, two modes:

- **Signal**: the current Audio Inspector surface and default mode.
- **Scope**: an analysis mode backed by the same probe data with scope
  semantics.

The selected mode persists in developer settings (UI preference only; never
plugin state, never probe JSON, never shipped builds). Keep the mode views
independent so splitting Scope into its own window later is mechanical.

## Non-goals

- Do not turn the Signal Inspector into an oscilloscope by default.
- Do not add a signal generator just to have something to look at (offline
  stimulus belongs to Audio Doctor).
- Do not ship customer-facing debugging UI in release plugin builds —
  everything stays behind `PULP_ENABLE_AUDIO_PROBES`.
- Do not make live scope analysis the offline Audio Doctor path.
- Do not require audible output for headless agent tests (offline source lands
  in Phase 5; until then the live path carries the standard audible notice).
- Do not add spectral analysis, THD, transfer-function analysis, MIDI-CV
  calibration, or modular-synth protocol features in any phase of this plan.
- Do not change the `--audio-probe-json` schema, ever, as part of this work.

## Resolved design questions

Decisions made during this review (do not relitigate without new evidence):

1. **Window title stays `Audio Inspector`**; the modes are labeled `Signal` and
   `Scope`. No new window, no new shortcut work (the command-registry
   integration already landed with the harness).
2. **UI says `Scope`**, CLI says `pulp audio scope`. "Oscilloscope" appears
   once in docs as a synonym for discoverability.
3. **JSON first, PNG deferred to Phase 5.** Structured measurements are the
   agent-facing value; a PNG is a derived display artifact.
4. **First scope source is the standalone output boundary only**
   (`AudioProbeStage::kStandaloneOutputBoundary`). Named-stage probes ride on
   the existing `stage_id` plumbing whenever later work adds more stages; this
   plan does not add stages.
5. **Input-side / hardware probing stays deferred** until a concrete user
   story exists. The acquisition/measurement helpers are source-agnostic
   (they take copied sample buffers), so nothing here blocks it.
6. **`pulp audio scope` wraps the live standalone launch path** (same model as
   `--audio-probe-json`: UI-headless, but the audio device opens and the
   audible notice prints). A genuinely speakerless source is Phase 5
   (`--input-wav`), not a Phase 2 promise.
7. **Scope JSON is a new, versioned schema** (`pulp.audio.scope.v1`), separate
   from probe JSON. Unavailable measurements serialize as `null`, not omitted.

## Architecture

Small layers, UI last:

```
AudioProbe (RT producer, existing)
   │  TripleBuffer<AudioProbeSnapshot> + capture ring (copied, non-RT reads)
   ▼
Acquisition  (core/audio, new, pure)   — bounded window selection + trigger
   ▼
Measurement  (core/audio, new, pure)   — numeric facts over acquired samples
   ▼
   ├── AudioScopePanel (core/view)     — draws grid/trace/readouts, owns freeze
   ├── Scope JSON writer (core/audio)  — versioned artifact
   └── CLI / MCP adapters (tools/)     — one-shot capture, no window required
```

Hard rules across every phase:

- **RT boundary**: the audio thread only does what `analyze_output()` does
  today — bounded scalar work and bounded ring writes. Acquisition, trigger
  scanning, measurement, JSON serialization, and rendering all run
  consumer-side on copied data. Phase 4 is the only phase that touches the RT
  path, and it must keep the existing allocation-counting test green.
- **Gating**: everything is dev-only behind `PULP_ENABLE_AUDIO_PROBES`.
  Probe-off builds compile, reject scope requests with a specific error, and
  pay nothing at runtime.
- **Honesty**: no invented samples anywhere. Trigger-not-found falls back to
  the raw tail window with a warning. Stale/no-probe states render explicitly.
- **Single-consumer capture**: the capture FIFO has one consumer. Until a
  fan-out design exists, a scope JSON one-shot and a visible inspector window
  must not compete for it (Phase 2 rejects the combination).

---

# Phased implementation plan

Each phase: one `feature/*` branch off `main`, one PR via `shipyard pr`, tests
in the same PR (non-negotiable), docs/status manifests updated in the same PR.
The "Per-phase shipping checklist" section below applies to all of them.

## Phase 1 — Pure scope primitives in `core/audio`

**Goal:** reusable, UI-independent acquisition + measurement + versioned JSON,
with zero behavior change to anything that exists. This is the phase that makes
Scope useful to plugin developers regardless of whether the UI mode ever ships:
the helpers take copied sample buffers, so a plugin author can point them at
their own dev-tool capture path.

**New files**

- `core/audio/include/pulp/audio/audio_scope.hpp`
- `core/audio/src/audio_scope.cpp`
- `core/audio/include/pulp/audio/audio_scope_json.hpp`
- `core/audio/src/audio_scope_json.cpp`

**API sketch** (match existing `pulp::audio` struct + free-helper style):

```cpp
namespace pulp::audio {

enum class AudioScopeTriggerMode : std::uint8_t { kNone, kRisingZero };

struct AudioScopeAcquisitionConfig {
    std::uint32_t window_samples = 2048;
    AudioScopeTriggerMode trigger_mode = AudioScopeTriggerMode::kRisingZero;
    std::uint32_t selected_channel = 0;
};

struct AudioScopeAcquisition {
    bool ok = false;
    double sample_rate = 0.0;
    std::uint32_t source_channel_count = 0;
    std::uint32_t source_frames = 0;
    std::uint32_t window_start = 0;
    std::uint32_t window_samples = 0;
    bool trigger_found = false;
    std::uint32_t trigger_sample = 0;       // index into source, not window
    std::uint64_t source_sequence_number = 0;
    std::vector<float> samples;             // copied window, off-RT owned
    std::vector<std::string> warnings;
};

struct AudioScopeMeasurements {
    // available flags + values; unavailable => flag false + JSON null
    bool peak_to_peak_available = false;  double peak_to_peak = 0.0;
    bool rms_available = false;           double rms = 0.0;
    bool dc_offset_available = false;     double dc_offset = 0.0;
    bool crest_factor_available = false;  double crest_factor = 0.0;
    bool frequency_available = false;     double frequency_hz = 0.0;
                                          double period_samples = 0.0;
    std::vector<std::string> warnings;
};

struct AudioScopeResult {
    AudioProbeStage stage = AudioProbeStage::kUnknown;
    AudioScopeAcquisition acquisition;
    AudioScopeMeasurements measurements;
};

AudioScopeAcquisition acquire_audio_scope_window(
    BufferView<const float> source,
    const AudioScopeAcquisitionConfig& config,
    const AudioProbeSnapshot* snapshot_metadata = nullptr);

AudioScopeMeasurements measure_audio_scope_window(
    const AudioScopeAcquisition& acquisition);

std::string_view audio_scope_trigger_mode_name(AudioScopeTriggerMode mode);
bool parse_audio_scope_trigger_mode(std::string_view text,
                                    AudioScopeTriggerMode& out);

// "pulp.audio.scope.v1"; unavailable measurements are null, never omitted.
std::string audio_scope_result_to_json(const AudioScopeResult& result,
                                       bool pretty = true);

}  // namespace pulp::audio
```

**Acquisition rules** (each rule is a test case):

- `window_samples == 0` → `ok=false`, warning `window_samples_must_be_positive`.
- Empty source → `ok=false`, warning `empty_source`.
- Source shorter than the window → `ok=true`, the whole source, warning
  `window_truncated_to_source`.
- Out-of-range `selected_channel` → clamp to 0, warning
  `selected_channel_out_of_range`.
- `kNone`: newest complete window — `start = max(0, frames - window)`.
- `kRisingZero`: scan backward from the latest start that still allows a full
  window; pick the most recent negative→non-negative crossing; if none, fall
  back to the raw tail window with warning `trigger_not_found`.
- Never invent or interpolate samples.

**Measurement rules**:

- Non-finite samples are excluded from scalar measurements, with warning
  `nonfinite_samples_ignored` (counts preserved in the warning text).
- Silence: peak-to-peak/RMS/DC available; frequency unavailable with warning
  `frequency_unavailable_silence`.
- Frequency: zero-crossing estimate only; require ≥ 2 consistent rising-zero
  intervals (consistency tolerance documented in the header); otherwise
  unavailable with `frequency_unavailable_nonperiodic`. Never guess.
- Crest factor unavailable when RMS is ~0 (avoid divide-by-noise).

**Scope JSON v1 shape** (stable contract; additive evolution only — new fields
may be added, existing fields never change meaning; breaking change ⇒ `v2`):

```json
{
  "schema": "pulp.audio.scope.v1",
  "version": 1,
  "source": { "kind": "live_probe", "stage": "standalone_output_boundary",
              "sample_rate": 48000, "channel_count": 1,
              "selected_channel": 0, "sequence_number": 42 },
  "acquisition": { "window_samples": 2048, "source_frames": 4096,
                   "window_start": 1024, "trigger_mode": "rising_zero",
                   "trigger_found": true, "trigger_sample": 1024 },
  "measurements": { "peak_to_peak": 0.72, "rms": 0.254, "dc_offset": 0.001,
                    "crest_factor": 1.41, "frequency_hz": 440.1,
                    "period_samples": 109.1 },
  "warnings": []
}
```

**Tests** — add to the existing `test/test_audio_probe.cpp` binary (no new
CMake target; respects the `test/CMakeLists.txt` hotspot size guard):

- raw-tail and rising-zero acquisition pick only existing sample indices
- trigger fallback emits `trigger_not_found` and returns the raw tail
- short-buffer, empty-source, zero-window, bad-channel edge cases
- measurements: silence, pure DC, 440 Hz sine (frequency within tolerance),
  clipped square (peak-to-peak, crest factor), NaN/Inf injection
- frequency reports unavailable on white noise
- JSON: unavailable → `null`; schema/version fields present
- regression: `audio_probe_snapshot_to_json()` output is byte-identical to a
  golden expectation (the guard that Scope never perturbs probe JSON)

**Acceptance criteria**

- No RT-path changes; no standalone/CLI/UI behavior change.
- All helpers usable with no UI toolkit, no audio device, no window.
- Scope JSON carries `pulp.audio.scope.v1` and round-trips through
  `choc::json` parsing.

## Phase 2 — Live one-shot `pulp audio scope` + MCP `pulp_audio_scope`

**Goal:** the agent-facing path: a machine-readable scope capture from the
live standalone output boundary, channel 0, explicitly labeled as such.

**Command shape**

```bash
pulp audio scope [target] --frames 90 --window 2048 \
  --trigger rising-zero --channel 0 --json scope.json
```

- `--json` omitted → normalized JSON to stdout (private temp file internally,
  same pattern as `handle_audio_probe_json`).
- `--png` / `--include-samples` rejected with a clear "not yet supported"
  error (reserved for Phase 5; rejecting now keeps the help text honest).
- Wraps the live standalone launch: UI-headless, audio device opens, the
  existing pre-launch audible notice prints (CLAUDE.md audio etiquette /
  issue #3173). Docs must say this plainly.

**Plumbing** (mirrors the `--audio-probe-json` pipeline exactly):

- `tools/cli/cmd_run_parse.cpp` / `cmd_run.hpp` / `cmd_run.cpp`: low-level
  `pulp run --audio-scope-json <file> --audio-scope-window N
  --audio-scope-trigger M --audio-scope-channel C`; implies `--headless`;
  forwarded as argv + `PULP_AUDIO_SCOPE_{JSON,WINDOW,TRIGGER,CHANNEL}` env
  vars (argv/env parity per the cli-maintenance skill). **Reject
  `--audio-scope-json` + `--audio-inspector`** — both would compete for the
  single-consumer capture FIFO.
- `core/format/include/pulp/format/detail/standalone_environment.hpp`: parse
  the env vars; scope JSON implies headless; allowed headless-without-
  screenshot; rejected in probe-off builds with a specific
  `PULP_ENABLE_AUDIO_PROBES=OFF` error (existing probe-JSON precedent).
- `core/format/include/pulp/format/detail/standalone_audio_scope_json.hpp`
  (new): drain capture → `acquire_audio_scope_window` →
  `measure_audio_scope_window` → `audio_scope_result_to_json` → write file.
- `core/format/src/standalone.cpp`: prepare a capture ring large enough for
  the requested window (cap and document a max, e.g. 16384 frames); delayed
  one-shot action after `--frames` renders, mirroring the probe-JSON delay.
  All scope members/includes stay inside `#if PULP_ENABLE_AUDIO_PROBES`.
- `tools/cli/cmd_audio_scope.{hpp,cpp}` (new) + registration in
  `tools/cli/pulp_cli.cpp` + `tools/cli/CMakeLists.txt`: the user-facing
  subcommand; validates options, shells to `pulp run`, normalizes output.
- `tools/mcp/mcp_tools.{hpp,cpp}` + `tools/mcp/pulp_mcp.cpp`: `pulp_audio_scope`
  tool — parameter validation (positive frames/window, known trigger,
  non-option target), shells to `pulp audio scope`, returns structuredContent.
- `tools/scripts/cli_mcp_parity_baseline.json`: add `pulp_audio_scope` with a
  note that it wraps the nested CLI command.

**Docs in the same PR**: `docs/reference/cli.md` (run plumbing + `audio scope`
section), `docs/status/cli-commands.yaml` (new subcommand + run flags),
`docs/guides/audio-inspector.md` (Signal vs Scope, audible-output warning),
`.agents/skills/cli-maintenance/SKILL.md` (new run flags — this path is mapped
in `tools/scripts/skill_path_map.json`, so the skill-sync gate requires it).

**Tests**

- `test/test_cli_run_options.cpp`: scope flag parsing, invalid
  window/trigger/channel, implies-headless, no auto-screenshot path,
  scope+inspector rejection.
- `test/test_cli_shellout.cpp`: `pulp run --help` and `pulp audio scope
  --help` advertise the flags; `pulp audio scope` against the demo target
  produces parseable v1 JSON and exit 0; probe-off rejection message (pattern:
  existing probe-JSON shellout tests).
- `test/test_standalone_audio_inspector.cpp`: standalone scope-JSON helper
  produces v1 schema from a prepared probe; honest failure with no capture.
- `test/test_mcp_server.cpp`: `tools/list` includes `pulp_audio_scope`;
  dispatch works; sorted expected-tool-list updated.
- Regression: probe-JSON golden test from Phase 1 still green.

**Acceptance criteria**

- An agent can run one command, get structured measurements, and act on them
  without opening a window.
- `--audio-probe-json` output unchanged (golden test).
- Probe-off builds reject with a specific error, exit nonzero.
- JSON `source.channel_count` and `selected_channel` honestly report the
  channel-0 limitation.

## Phase 3 — Scope mode in the Audio Inspector window

**Goal:** the human-facing surface: Signal / Scope segmented control in the
existing window, persisted preference, freeze.

**New files**

- `core/view/include/pulp/view/audio_scope_panel.hpp`
- `core/view/src/audio_scope_panel.cpp`

**Modified**: `audio_inspector_window.{hpp,cpp}` (mode enum, segmented control,
preference load/save, routing), `core/format/src/standalone.cpp` (pass the app
name through so preferences are per-app), `audio_inspector_panel.{hpp,cpp}`
(only if display helpers are shared — Signal behavior must not change).

**Design**

- `enum class AudioInspectorMode { kSignal, kScope };` owned by the window.
- `AudioScopePanel` consumes `AudioScopeResult` produced by the **Phase 1
  helpers** on the copied capture — the panel draws, it does not acquire.
  `AudioWaveformView`'s display-side trigger logic is not extended further;
  Scope-mode trigger semantics live in `core/audio`.
- Controls: trigger off / rising-zero, window size, horizontal scale, vertical
  gain, grid toggle, freeze. Measurement readouts under the trace.
- Freeze stops UI updates to the scope panel only; probe publication, Signal
  mode, probe JSON, and the CLI path are unaffected.
- Stale/no-probe honesty matches the Signal panel (dimmed baseline, explicit
  label, never zeros-that-look-live).

**Persistence** — `state::ApplicationProperties` (user scope), keyed per app
name; never `StateStore`, never `CachedProperty`:

- `audio_inspector.mode` = `signal` | `scope` (Signal is the default)
- `audio_scope.grid_visible`, `audio_scope.trigger_mode`,
  `audio_scope.window_samples`, `audio_scope.horizontal_scale`,
  `audio_scope.vertical_gain`, `audio_scope.channel`

No captured samples are ever persisted. The existing
`PULP_AUDIO_INSPECTOR_*` env knobs stay display-only overrides and win over
stored preferences for the session without writing them back.

**Tests** — extend `test/test_audio_inspector_window.cpp` (headless, existing
binary): default mode is Signal; mode switch updates the visible panel; mode
persists across window reconstruction; scope panel consumes the core helper
result (assert `trigger_sample`/`window_start` agreement); freeze holds the
display while the probe sequence advances; stale/no-probe rendering; Signal
panel behavior unchanged (existing tests stay green).

**Acceptance criteria**

- New users land in Signal; a developer who picks Scope gets Scope on reopen.
- Scope mode demonstrably uses the same acquisition/measurement code as the
  CLI (one source of truth — a UI-vs-CLI measurement mismatch is a bug class
  this architecture eliminates).
- No change to probe JSON, scope JSON, plugin state, or release builds.

## Phase 4 — Truthful multi-channel capture + overlay

**Goal:** remove the channel-0 ceiling at the probe, then let Scope select and
overlay channels. The only phase that touches the RT path.

**Probe API extension** (`audio_probe.{hpp,cpp}`) — old API preserved:

```cpp
struct AudioProbeCaptureConfig {
    int capture_frames = 0;
    int capture_channels = 1;   // new; clamped to prepared channel capacity
};

struct AudioProbeCaptureReadInfo {
    int frames = 0;
    int channels = 0;
    std::uint64_t dropped_capture_frames = 0;
    std::uint64_t sequence_number = 0;
};

// Channel-major rectangular copy; missing source channels zero-filled
// (deterministic, documented, tested). Existing read_capture() keeps
// returning channel 0.
int read_capture_channels(float* dst_channel_major, int dst_channels,
                          int max_frames,
                          AudioProbeCaptureReadInfo* info = nullptr);
```

RT constraints: all storage still allocated in `prepare()`;
`analyze_output()` does a bounded `frames × capture_channels` copy; the
existing allocation-counting test must stay green with multi-channel capture
enabled. Memory note: ring size is `capture_frames × capture_channels` floats —
document the budget in the header (e.g. 16384 × 2 ≈ 128 KB).

**Consumer changes**: window/panel channel mode
(`kSelected | kOverlay | kAverage` — overlay draws all captured channels,
measurements come from the selected channel unless mode is `kAverage`, which
labels itself); CLI/MCP `--channel N` now reaches real data and scope JSON
`source.channel_count` reports the captured count; `docs/guides/
audio-inspector.md` drops the channel-0 caveat.

**Tests**: probe — multi-channel ring correctness (distinct ramps per channel),
zero-fill determinism, legacy `read_capture()` equivalence, allocation test
with channels > 1, drop accounting; window — overlay draws distinct traces,
selected-channel measurement switches, out-of-range clamp+warn; CLI shellout —
`--channel 1` on a stereo demo target measures the right channel.

**Acceptance criteria**: overlay never fabricates channels; all existing
channel-0 tests pass unmodified; RT allocation tests green; this is also the
substrate the harness plan's Phase 7-live flight recorder can reuse.

## Phase 5 — Artifacts and a speakerless source

**Goal:** reviewable artifacts plus a genuinely no-audio path for CI/agents.
Two independent features; ship separately if either stalls.

**5a — offline WAV source (the CI/agent win, do this first)**

```bash
pulp audio scope --input-wav render.wav --window 2048 \
  --trigger rising-zero --json scope.json
```

- No standalone launch, no audio device, no `PULP_ENABLE_AUDIO_PROBES`
  dependency, no audible notice needed. Decodes via the existing
  `core/audio` format registry, feeds the Phase 1 helpers directly.
- Same `pulp.audio.scope.v1` schema with `source.kind = "wav_file"` (+
  `source.path`). This composes with the offline harness: render with the
  existing golden-file tooling, then scope the render — no speakers in CI.

**5b — PNG artifact**

```bash
pulp audio scope TARGET --json scope.json --png scope.png
```

- JSON stays authoritative; the PNG is derived. Render headlessly via the
  existing `render_to_png` path with the **Skia backend** (per the screenshot
  skill — CoreGraphics doesn't composite images faithfully). Any display
  interpolation used for the trace is labeled in the PNG's JSON sidecar
  metadata.

**Tests**: 5a — WAV fixture (generated sine) → measurements within tolerance;
runs with audio device code paths compiled out of the test; nonexistent/corrupt
WAV → clear error. 5b — PNG exists, non-empty, JSON byte-identical with and
without `--png`.

**Acceptance criteria**: an agent in CI can scope a rendered WAV with zero
audio-device involvement; live behavior and notices unchanged.

---

## Per-phase shipping checklist (repo gates)

Every phase PR runs through this list — it is the difference between "plan" and
"agent-executable plan":

1. Branch: `feature/audio-scope-phase-<n>` off fresh `origin/main`. Build
   Release (`pulp build`); verify with both `CMakeCache.txt` and `flags.make`
   checks if reporting build type.
2. Tests in the same PR, in **existing test binaries** where possible —
   `test/CMakeLists.txt` is frozen by `hotspot_size_guard`; net-zero line
   changes there (group new files under an existing comment, drop a blank
   line) or reuse a binary.
3. Docs manifests when CLI/MCP surfaces change: `docs/status/cli-commands.yaml`,
   `docs/reference/cli.md`, `tools/scripts/cli_mcp_parity_baseline.json`,
   `docs/guides/audio-inspector.md`. Run `pulp docs check`.
4. Skill-sync: changes under `tools/cli/` map to the `cli-maintenance` skill in
   `tools/scripts/skill_path_map.json` — update the SKILL.md or carry a
   `Skill-Update: skip skill=cli-maintenance reason="..."` trailer.
5. Version bump: Phases 2, 4, 5 add user-facing CLI/MCP surface → SDK/CLI
   minor bump (shipyard applies it; `fix:`-titled PRs need the manual
   `chore: bump versions` marker). Phases 1, 3 are
   `Version-Bump: sdk=skip`-eligible only if no public header lands — Phase 1
   adds public headers, so bump it too.
6. Diff coverage: `tools/scripts/local_diff_cover.sh <test-target>` before
   pushing (75% gate). The pure Phase 1 helpers should clear this trivially.
7. Audible-output etiquette: any test or verification step that launches the
   live standalone announces itself, caps duration (`--frames`), and tears
   down (this is why every live test in this plan uses bounded `--frames`).
8. Ship: `shipyard pr` — never `gh pr create` + separate validation.
9. Regression guard on every phase: the probe-JSON golden test and the
   `pulp run --audio-probe-json` shellout tests must be untouched and green.

## Risks and mitigations

| Risk | Mitigation |
|---|---|
| Scope work perturbs the frozen probe-JSON schema | Golden byte-comparison test added in Phase 1, asserted every phase |
| RT regression in Phase 4 | Only phase touching `analyze_output()`; allocation-counting test must pass with capture channels > 1; keep the change a bounded memcpy |
| Single-consumer FIFO contention (scope one-shot vs inspector window) | Parse-time rejection in Phase 2; revisit only if a fan-out capture design lands |
| Scope JSON schema churn | `schema`/`version` fields from day one; additive-only rule; breaking ⇒ `v2` |
| UI prefs leak into plugin state | `ApplicationProperties` only; tests assert `StateStore` untouched by mode switches |
| "Live" CLI surprises users with audio | Existing pre-launch notice covers scope; docs say it plainly; speakerless path is a named phase (5a), not a vague promise |
| Capture-ring memory growth (Phase 4) | Documented budget formula + hard cap in `prepare()` |
| Plan drifts from code (this branch isn't merged yet) | Phase 1 must start by re-verifying the "Verified current state" table against `origin/main` at implementation time |

## Decision criteria

Proceed past Phase 2 only if Phases 1–2 deliver at least two of:

- faster diagnosis of a real plugin signal-shape bug than Signal Inspector
  alone (record one concrete instance)
- MCP/CLI measurements an agent actually acted on (e.g. caught a DC offset or
  frequency regression in a workflow)
- the acquisition/measurement primitives get a second consumer beyond the CLI
  (the Phase 3 panel counts; so would a plugin author's dev tool)
- docs that keep Signal Inspector and Scope from being confused (validated by
  the guides update landing without review pushback)

If the work only produces a nicer waveform animation, stop after Phase 2 and
keep the CLI/MCP surface — the primitives still pay for themselves.

## Agent handoff notes

- Start with Phase 1; it has no dependencies and no behavior risk. Re-verify
  the "Verified current state" table against `origin/main` first — this plan
  was written against `fix/audio-run-audible-notice` before its merge.
- Each phase is one PR. Do not combine phases. Do not start Phase 4 before
  Phase 3 has proven the mode UI (overlay controls land in an existing panel).
- The full validation loop per phase:
  `pulp build && ctest --test-dir build -R "AudioScope|AudioProbe|AudioInspector" --output-on-failure`,
  then the shellout/MCP suites for Phases 2+
  (`ctest -R "cli_shellout|mcp_server"`), then the shipping checklist above.
- When in doubt about a behavior question, the precedence is: this plan's
  "Resolved design questions" → the harness plan in `planning/` → the code.
  Planning docs describe intent; the code wins on facts.
