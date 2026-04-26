# Coverage Compliance Status

Last reviewed: 2026-04-26 16:52 EDT

This is the durable tracker for the repo-wide coverage compliance
program under `#641`. Phase 1 representation is complete, Phase 2 gap
planning has been seeded from the corrected Codecov baseline, and Phase
3 gap closure is active through small, reviewable PR tranches.

## Goal

Get Codecov as close as practical to the intended first-party local
source surface, then use that corrected baseline to plan and close the
real measured test gaps.

Target tiers remain unchanged in `ci/coverage-targets.yaml`:

- `80%`: `audio`, `format`, `host`, `midi`, `signal`, `platform`
- `70%`: `render`, `view`, `cli`
- `50%`: `events`, `runtime`, `state`, `canvas`, `osc`, `ship`, `tools`

## Three-phase program

### Phase 1 - Representation / Codecov truth

Status: complete.

Finish line state:

- `windows` is non-null on `main` (`0.0%` at the current baseline).
- `dsl` is a real component on `main` (`68.3%`).
- `apple/Sources/**` materializes on Codecov for the macOS Swift package
  lane and the native `PulpBridge.cpp` lane.
- `android/app/src/main/kotlin/**` materializes on Codecov.
- `bindings/python/bindings.cpp` materializes on Codecov.
- the clearly known remaining "still not on Codecov" buckets are listed
  below as explicit follow-up or out-of-scope surfaces.
- this status document is rebaselined from the corrected `main` surface.

### Phase 2 - Gap planning

Status: complete enough to execute, with the roadmap tracked in `#641`
and the tranche issues listed below.

Finish line:

- counted components are ranked from the corrected `main` baseline
- major low-coverage files and components are grouped into tranche issues
- remaining out-of-scope surfaces are either accepted for now or have
  explicit expansion issues
- the repo has an actionable issue / PR roadmap for closing measured
  gaps

### Phase 3 - Gap closure

Status: active.

Finish line:

- planned coverage tranches are implemented and merged
- target components move materially toward or to their configured
  thresholds
- deferred or exceptional surfaces stay documented explicitly instead of
  silently omitted

## Current live state

Latest complete Codecov `main` report observed while updating this doc:

- commit: `957a073523e3aabcd7b11972acaf98065909bf67`
- workflow: Coverage run `24964746369`, completed successfully
- overall tracked coverage: `45.96%` over `70,376` lines in `556` files
- covered lines: `32,349`
- missed lines: `36,895`
- partial lines: `1,132`
- current component coverage from the Codecov API:
  - `audio`: `34.45%`
  - `canvas`: `63.96%`
  - `dsl`: `63.38%`
  - `events`: `49.49%`
  - `format`: `51.03%`
  - `host`: `43.61%`
  - `midi`: `47.77%`
  - `osc`: `62.22%`
  - `platform`: `43.18%`
  - `render`: `59.74%`
  - `runtime`: `50.48%`
  - `signal`: `69.4%`
  - `state`: `62.99%`
  - `view`: `43.72%`
  - `android`: `13.83%`
  - `apple`: `25.36%`
  - `linux`: `0.22%`
  - `windows`: `6.42%`
  - `cli`: `38.92%`
  - `ship`: `53.77%`
  - `tools`: `44.36%`

Post-`#794` file-level proof point: `core/audio/src/aiff_reader.cpp`
is now `72.01%` covered with `61` misses, up from `64.22%` with `78`
misses at the prior `e2af9c4d` baseline.

Post-`#813` proof point: the `signal` component is now `69.4%` covered,
up from `66.13%` at the prior `810743d4` baseline.

Merged after the Phase 1 closeout / `#723` baseline:

- `#649` CLI/tools tranche 1 -> `8449b6e8`
- `#648` events tranche 1 -> `357eded1`
- `#666` ship/state deterministic hardening -> `75439c89`
- `#734` audio/platform tranche 1 -> `2224d11d`
- `#741` midi/signal tranche 1 -> `63fd5ad5`
- `#757` scan-worker CLI coverage -> `9053e0b`
- `#755` LV2 host/entry coverage -> `aed8acb`
- `#756` validation harness edge coverage -> `22d1e69`
- `#765` TextEditor multiline navigation coverage -> `7257a154`
- `#766` OSC UDP sender/receiver edge coverage -> `31aa80e1`
- `#771` WidgetBridge extended-controls coverage -> `3d03c0fe`
- `#777` real CLAP `PluginSlot` coverage -> `478f489c`
- `#782` VST3 adapter process-path coverage -> `648c2d27`
- `#786` render DirtyTracker / RenderLoop edge coverage -> `26dd396d`
- `#789` Android ship/package helper coverage -> `e61dce8d`
- `#788` events socket-IPC coverage -> `23d3ed1d`
- `#793` package-registry CLI/tools coverage -> `e2af9c4d`
- `#794` AIFF reader edge-path coverage -> `c4e6ad09`
- `#796` tool-registry CLI/tools coverage -> `11c10a7e`
- `#797` StreamingWriter audio edge coverage -> `64ba65e3`
- `#798` FormatRegistry audio dispatch coverage -> `443ca260`
- `#799` docs-reader CLI shellout coverage -> `22438468`
- `#800` Python gate-helper tooling coverage -> `55b6dfab`
- `#795` events volume/service-discovery coverage -> `f1a5aa84`
- `#802` CLI-create shellout coverage -> `2de79ff3`
- `#803` signal matrix helper coverage -> `57285797`
- `#801` render texture-atlas coverage and LCOV converter fix -> `ae4fdab8`
- `#804` signal Oversampler helper coverage -> `c6600f9`
- `#805` CLI common helper coverage -> `5844c6ac`
- `#806` audio mapped-reader/offline processing coverage -> `1bcb23e3`
- `#807` audio ChannelSet helper coverage -> `46e0fdeb`
- `#808` MIDI file edge round-trip coverage -> `aae5d5d0`
- `#809` signal FFT/convolver helper coverage -> `453bcec8`
- `#810` platform Environment diff-edge coverage -> `f712702a`
- `#811` CLI project-command dispatch coverage -> `810743d4`
- `#813` signal dynamics helper coverage -> `957a0735`
- `#814` CLI `pulp pr` shellout coverage -> `8bd4a3e9`
- `#815` MPE tracker edge-path coverage -> `019130bd`
- `#816` MIDI running-status parser edge coverage -> `c9620a65`

Open Phase 3 PRs:

- `#817` UMP conversion edge coverage for `#645`, branch
  `feature/midi-ump-conversion-coverage-645`, head `31a53432`; opened
  from `main` at `957a0735`, then updated with scan-cache temp-file
  isolation after macOS ASan exposed a parallel CTest collision. `shipyard
  pr` was attempted first but stopped before PR creation because the SSH
  `windows` backend timed out, so this tranche is using the
  GitHub/Namespace path instead of skipping that lane.
- `#818` MPE synth voice / allocator edge coverage for `#645`, branch
  `feature/midi-mpe-synth-voice-coverage-645`, head `21eb74e1`; opened
  from `main` at `957a0735`. This tranche is also using the
  GitHub/Namespace path while the SSH `windows` target remains
  unreachable.
- `#819` MIDI keyboard/sequence edge coverage for `#645`, branch
  `feature/midi-keyboard-sequence-coverage-645`, head `60676709`;
  opened from `main` at `019130bd`. This tranche also uses the
  GitHub/Namespace path while the SSH `windows` target remains
  unreachable.
- `#820` signal spectrogram edge coverage for `#645`, branch
  `feature/signal-spectrogram-coverage-645`, head `0fd8aa5a`; opened
  from `main` at `c9620a65`. This tranche also uses the
  GitHub/Namespace path while the SSH `windows` target remains
  unreachable.
- `#821` audio reader helper edge coverage for `#640`, branch
  `feature/audio-reader-helpers-coverage-640`, head `be7d604f`; opened
  from `main` at `c9620a65`. This tranche also uses the
  GitHub/Namespace path while the SSH `windows` target remains
  unreachable.

Local Phase 3 draft worktrees:

- `#640` AIFF reader edge-path worktree
  `/Users/danielraffel/Code/pulp-audio-aiff-coverage-640`, branch
  `feature/audio-aiff-coverage-640`; merged via PR `#794` as
  `c4e6ad09`. The remote branch was deleted after merge.
- `#642` events volume/service-discovery worktree
  `/Users/danielraffel/Code/pulp-events-volume-coverage-642`, branch
  `feature/events-volume-coverage-642`, commit `357fc429`; merged via PR
  `#795` as `f1a5aa84`. The remote branch was deleted after merge.
- `#643` package-registry CLI/tools worktree
  `/Users/danielraffel/Code/pulp-package-registry-coverage-643`, branch
  `feature/package-registry-coverage-643`, commit `75a529ef`; merged via
  PR `#793`.
- `#643` tool-registry CLI/tools worktree
  `/Users/danielraffel/Code/pulp-cli-tool-registry-coverage-643`, branch
  `feature/cli-tool-registry-coverage-643`, commit `cdfbbcec`; merged via
  PR `#796` as `11c10a7e`. The remote branch was deleted after merge.
- `#640` streaming-writer audio/platform worktree
  `/Users/danielraffel/Code/pulp-audio-streaming-writer-coverage-640`,
  branch `feature/audio-streaming-writer-coverage-640`, commit
  `29a3f26d`; merged via PR `#797` as `64ba65e3`. The remote branch was
  deleted after merge.
- `#640` format-registry audio/platform worktree
  `/Users/danielraffel/Code/pulp-audio-system-volume-coverage-640`,
  branch `feature/audio-format-registry-coverage-640`, commit
  `0b35bb83`; merged via PR `#798` as `443ca260`. The remote branch was
  deleted after merge.
- `#643` docs-reader CLI/tools worktree
  `/Users/danielraffel/Code/pulp-cli-docs-coverage-643`, branch
  `feature/cli-docs-coverage-643`, commit `4877cb1b`; merged via PR
  `#799` as `22438468`. The remote branch was deleted after merge.
- `#643` Python gate-helper tooling worktree
  `/Users/danielraffel/Code/pulp-python-tooling-coverage-643`, branch
  `feature/python-tooling-coverage-643`, commit `acadc5af`; merged via PR
  `#800` as `55b6dfab`. The remote branch was deleted after merge.
- `#643` CLI-create shellout worktree
  `/Users/danielraffel/Code/pulp-cli-create-coverage-643`, branch
  `feature/cli-create-coverage-643`, commit `222d0f88`; merged via PR
  `#802` as `2de79ff3`. The remote branch was deleted after merge.
- `#646` texture-atlas render worktree
  `/Users/danielraffel/Code/pulp-render-texture-atlas-coverage-646`,
  branch `feature/render-texture-atlas-coverage-646`, commit
  `325485bc`; merged via PR `#801` as `ae4fdab8`. The remote branch was
  deleted after merge.
- `#645` signal matrix helper worktree
  `/Users/danielraffel/Code/pulp-signal-matrix-coverage-645`, branch
  `feature/signal-matrix-coverage-645`, commit `b1f5a125`; merged via PR
  `#803` as `57285797`. The remote branch was deleted after merge.
- `#645` signal oversampling helper worktree
  `/Users/danielraffel/Code/pulp-signal-oversampling-coverage-645`,
  branch `feature/signal-oversampling-coverage-645`, commit `bccaec0f`;
  merged via PR `#804` as `c6600f9`. The remote branch was deleted after
  merge.
- `#643` CLI common helper worktree
  `/Users/danielraffel/Code/pulp-cli-common-coverage-643`, branch
  `feature/cli-common-coverage-643`, commit `17b42f0f`; merged via PR
  `#805` as `5844c6ac`. The remote branch was deleted after merge.
- `#640` mapped-reader/offline audio worktree
  `/Users/danielraffel/Code/pulp-audio-reader-coverage-640`, branch
  `feature/audio-reader-coverage-640`, commit `c1d61e3a`; merged via PR
  `#806` as `1bcb23e3`. The remote branch was deleted after merge.
- `#640` ChannelSet audio worktree
  `/Users/danielraffel/Code/pulp-audio-channel-set-coverage-640`,
  branch `feature/audio-channel-set-coverage-640`, commit `89b969c9`;
  merged via PR `#807` as `46e0fdeb`. The remote branch was deleted after
  merge.
- `#640` platform Environment diff-edge worktree
  `/Users/danielraffel/Code/pulp-platform-environment-coverage-640`,
  branch `feature/platform-environment-coverage-640`, commit
  `bc1e96af`; merged via PR `#810` as `f712702a`. The remote branch was
  deleted after merge.
- `#645` MIDI file edge round-trip worktree
  `/Users/danielraffel/Code/pulp-midi-file-coverage-645`, branch
  `feature/midi-file-coverage-645`, commit `d02e853e`; merged via PR
  `#808` as `aae5d5d0`. The remote branch was deleted after merge.
- `#645` signal FFT/convolver helper worktree
  `/Users/danielraffel/Code/pulp-signal-fft-coverage-645`, branch
  `feature/signal-fft-coverage-645`, commit `11c83082`; merged via PR
  `#809` as `453bcec8`. The remote branch was deleted after merge.
- `#643` CLI project-command dispatch worktree
  `/Users/danielraffel/Code/pulp-cli-project-coverage-643`, branch
  `feature/cli-project-coverage-643`, commit `46dca652`; merged via PR
  `#811` as `810743d4`. The remote branch was deleted after merge. Scope:
  direct `cmd_project.cpp` coverage for top-level help/no-arg/unknown
  dispatch, bump option parsing, `--all` empty-registry handling,
  registry-backed `--all --dry-run` reporting without undo files, a
  single-project bump plus newest-batch undo round-trip, and undo
  no-batch/missing-batch/malformed-batch diagnostics. Local validation:
  no-GPU/no-examples configure, `pulp-test-cli-project-command` build,
  `[cli][project-command][issue-643]` passed `75` assertions in `6`
  cases, full binary passed `318` assertions in `22` cases, focused
  CTest `cmd_project` passed `8/8`, `git diff --check`, skill-sync
  report, and version-bump report.
- `#645` signal dynamics helper worktree
  `/Users/danielraffel/Code/pulp-signal-dynamics-coverage-645`, branch
  `feature/signal-dynamics-coverage-645`, commit `fb0505f7`; merged via
  PR `#813` as `957a0735`. The remote branch was deleted after merge.
  Scope: test-only coverage for `DryWetMixer` mix
  clamping/equal-power/latency/reset paths, `Compressor`
  hard-knee/soft-knee/buffer/reset paths, `Limiter` buffer/reset paths,
  `WindowFunction` hamming/flat-top/kaiser branches, and
  `MultiChannelMeter` channel clamping/reset/silent-correlation/block
  threshold edges. Local validation: no-GPU/no-examples configure,
  `pulp-test-dsp-enhancements` and `pulp-test-signal` builds, targeted
  issue tags passed `9` assertions in `2` DSP cases and `35`
  assertions in `8` signal cases, full DSP binary passed `62`
  assertions in `20` cases, full signal binary passed `885` assertions
  in `67` cases, focused CTest passed `23/23`, `git diff --check`,
  skill-sync report, and version-bump report.
- `#643` CLI `pulp pr` shellout worktree
  `/Users/danielraffel/Code/pulp-cli-pr-coverage-643`, branch
  `feature/cli-pr-coverage-643`, commit `615bd756`; merged via PR
  `#814` as `8bd4a3e9`. The remote branch was deleted after merge.
  Scope: shellout coverage for missing-Shipyard install guidance,
  native fallback help, outside-project refusal, and POSIX delegation to
  a fake `shipyard pr` executable without invoking real Shipyard. Follow-
  up `615bd756` fixed Linux Namespace by resolving the CLI binary before
  changing cwd in the native outside-project test. Local validation:
  no-GPU/no-examples configure, `pulp-test-cli-shellout` build, direct
  `[cli][shellout][pr][issue-643]` passed `18` assertions in `4` cases,
  single failed cloud case passed `3` assertions in `1` case after the
  fix, focused CTest `pulp pr` passed `7/7`, `git diff --check`,
  skill-sync report, and version-bump report.
- `#645` MPE tracker worktree
  `/Users/danielraffel/Code/pulp-mpe-tracker-coverage-645`, branch
  `feature/mpe-tracker-coverage-645`, commit `b7c72711`; merged via PR
  `#815` as `019130bd`. The remote branch was deleted after merge.
  Scope: test-only coverage for `MpeVoiceTracker` bend-range rejection,
  config reset, manager pressure/timbre state, cached member expression,
  bounded snapshots, and full-table overflow behavior. Local validation:
  no-GPU/no-examples configure, `pulp-test-mpe-voice-tracker` build,
  direct `[midi][mpe][issue-645]` passed `166` assertions in `6` cases,
  full binary passed `239` assertions in `23` cases, and focused CTest
  `MpeVoiceTracker` passed `23/23`, `git diff --check`, skill-sync
  report, and version-bump report.
- `#645` MIDI running-status parser worktree
  `/Users/danielraffel/Code/pulp-midi-running-status-coverage-645`,
  branch `feature/midi-running-status-coverage-645`, commit `94b93a6e`;
  merged via PR `#816` as `c9620a65`. The remote branch was deleted
  after merge.
  Scope: test-only coverage for one-byte and two-byte channel running
  status, system-common data lengths, real-time bytes inside sysex,
  status restart inside sysex, empty sysex / unknown status ignore
  behavior, and null sink / empty feed inputs. Local validation:
  no-GPU/no-examples configure, `pulp-test-running-status` build, direct
  `[midi][running-status][issue-645]` passed `29` assertions in `6`
  cases, full binary passed `56` assertions in `14` cases, focused CTest
  selection passed `5/5`, `git diff --check`, skill-sync report, and
  version-bump report.
- `#645` MIDI UMP conversion worktree
  `/Users/danielraffel/Code/pulp-midi-ump-conversion-coverage-645`,
  branch `feature/midi-ump-conversion-coverage-645`, commit `31a53432`;
  open as PR `#817`.
  Scope: test-only coverage for velocity-zero note-on aliasing,
  program-change fallback packets, group masking, sample-offset
  preservation, MIDI 2 edge-value down-conversion, skipped packets
  without MIDI 1 equivalents, and scale endpoint preservation. Follow-up
  `31a53432` isolates scan-cache test temp files with process id plus a
  per-process counter after macOS ASan showed the previous
  steady-clock-only stem could collide under parallel CTest. Local
  validation: no-GPU/no-examples configure, `pulp-test-ump-buffer-
  conversion` build, direct `[midi][ump][issue-645]` passed `46`
  assertions in `4` cases, full binary passed `97` assertions in `15`
  cases, focused CTest conversion selection passed `6/6`, normal
  `pulp-test-scan-cache` `[scan_cache]` passed, normal and ASan CTest
  repeats for `put + get round-trip on an existing file` and `get
  invalidates when file changes` passed `10/10`, `git diff --check`,
  skill-sync report, and version-bump report. A broader CTest regex was
  intentionally not used for the final summary because the lite build
  exposes `*_NOT_BUILT_*` placeholder tests that match generic `ump`
  patterns.
- `#645` MPE synth voice worktree
  `/Users/danielraffel/Code/pulp-midi-mpe-synth-voice-coverage-645`,
  branch `feature/midi-mpe-synth-voice-coverage-645`, commit
  `21eb74e1`; open as PR `#818`.
  Scope: test-only coverage for `MpeSynthVoice` smoothing clamp,
  reset/release state, timbre routing, all non-oldest `MpeVoiceAllocator`
  steal policies, unknown expression events, `reset_all`, and
  zero-polyphony dispatch tolerance. Local validation: no-GPU/no-examples
  configure, `pulp-test-mpe-synth-voice` build, direct
  `[midi][mpe][issue-645]` passed `51` assertions in `4` cases, full
  binary passed `75` assertions in `12` cases, focused CTest passed
  `12/12`, `git diff --check`, skill-sync report, and version-bump
  report.
- `#645` MIDI keyboard/sequence worktree
  `/Users/danielraffel/Code/pulp-midi-keyboard-sequence-coverage-645`,
  branch `feature/midi-keyboard-sequence-coverage-645`, commit
  `60676709`; open as PR `#819`.
  Scope: test-only coverage for `MidiKeyboardState` invalid queries,
  ignored non-note events, release callbacks, and retrigger velocity
  updates; plus `MidiMessageSequence` helper field masking,
  classifier aliases, zero-velocity note-on note-off behavior,
  channel-specific note-off lookup, range boundaries, timestamp offsets,
  iteration, duration, and clear behavior. Local validation:
  no-GPU/no-examples configure, `pulp-test-midi-expansion` and
  `pulp-test-v3-gaps` build, direct `[midi][keyboard][issue-645]`
  passed `19` assertions in `3` cases, direct
  `[midi][sequence][issue-645]` passed `39` assertions in `3` cases,
  full binaries passed `51` assertions in `20` cases and `94`
  assertions in `24` cases, focused CTest passed `20/20`,
  `git diff --check`, skill-sync report, and version-bump report.
- `#645` signal spectrogram worktree
  `/Users/danielraffel/Code/pulp-signal-spectrogram-coverage-645`,
  branch `feature/signal-spectrogram-coverage-645`, commit `0fd8aa5a`;
  open as PR `#820`.
  Scope: test-only coverage for `ColorMapper` ramp switching/control
  points, `FrequencyAxis` clamping and bin/display conversions,
  `SpectrogramBuffer` column wraparound, row-to-bin mapping,
  degenerate dB ranges, and empty-row buffers. Local validation:
  no-GPU/no-examples configure, `pulp-test-stft` build, direct
  `[signal][spectrogram][issue-645]` passed `49` assertions in `4`
  cases, direct `[signal][spectrogram]` passed `79` assertions in `12`
  cases, full binary passed `390` assertions in `30` cases, focused
  CTest selection passed `12/12`, `git diff --check`, skill-sync
  report, and version-bump report.
- `#640` audio reader helper worktree
  `/Users/danielraffel/Code/pulp-audio-reader-helpers-coverage-640`,
  branch `feature/audio-reader-helpers-coverage-640`, commit
  `be7d604f`; open as PR `#821`.
  Scope: test-only coverage for `BufferingReader` source-exhaustion
  zero-fill and ring-wrap ordering, plus `AudioSubsectionReader` invalid
  reader, clamped-read, and start-past-end empty-view paths. Local
  validation: no-GPU/no-examples configure, `pulp-test-buffering-reader`
  and `pulp-test-v3-gaps` build, direct
  `[audio][buffering][issue-640]` passed `813` assertions in `2` cases,
  direct `[audio][subsection][issue-640]` passed `32` assertions in `2`
  cases, full buffering-reader binary passed `823` assertions in `8`
  cases, full v3-gaps binary passed `87` assertions in `23` cases,
  focused CTest passed `12/12`, `git diff --check`, skill-sync report,
  and version-bump report.

Open supporting PR:

- `#774` refreshes this durable handoff/status document, branch
  `docs/coverage-status-2026-04-25`. The branch is updated as this
  tracker changes; use the PR head SHA in GitHub as the live value.
  The branch has been rebased onto `origin/main` after `#813` merged,
  updated after `#816` merged, `#817` was repaired, `#820` opened, and
  `#821` opened, and remains docs-only.

Local environment note:

- Pulp's Shipyard source pin is `v0.46.0` in `tools/shipyard.toml`
  and the release workflows. On 2026-04-25, PATH had drifted to
  `shipyard, version 0.50.0`; rerunning `./tools/install-shipyard.sh`
  restored the pinned binary, and `shipyard --version` prints
  `shipyard, version 0.46.0`.
- On 2026-04-26, `shipyard pr` for the UMP conversion tranche failed
  pre-PR with exit code `3` because the SSH `windows` target timed out
  (`100.92.174.43:22`). For this Codecov batch, continue using the
  GitHub Actions / Namespace checks rather than spending time on the
  unreachable SSH backend, unless the user explicitly asks to repair the
  SSH target.

Next recovery actions:

1. Keep `#774` docs-only and let its latest status-update checks drain.
2. Monitor `#817` cloud checks; if a required lane fails, debug in
   `/Users/danielraffel/Code/pulp-midi-ump-conversion-coverage-645`,
   patch, validate locally, and push with lease.
3. Monitor `#818` cloud checks; if a required lane fails, debug in
   `/Users/danielraffel/Code/pulp-midi-mpe-synth-voice-coverage-645`,
   patch, validate locally, and push with lease.
4. Monitor `#819` cloud checks; if a required lane fails, debug in
   `/Users/danielraffel/Code/pulp-midi-keyboard-sequence-coverage-645`,
   patch, validate locally, and push with lease.
5. Monitor `#820` cloud checks; if a required lane fails, debug in
   `/Users/danielraffel/Code/pulp-signal-spectrogram-coverage-645`,
   patch, validate locally, and push with lease.
6. Monitor `#821` cloud checks; if a required lane fails, debug in
   `/Users/danielraffel/Code/pulp-audio-reader-helpers-coverage-640`,
   patch, validate locally, and push with lease.
7. If any open Phase 3 PR is green but GitHub reports it behind `main`,
   rebase that branch onto `origin/main`, push with lease, and let
   checks rerun.
8. Continue Phase 3 from the tranche issues below, prioritizing
   represented high-miss files over adding new perimeter lanes.

## Phase 1 corrected baseline

Source:

- Codecov public totals and components API for `main` commit
  `abe2b07a820d9e705864a3ecd3f0350772f694d1`
- GitHub Actions Coverage run `24886403280`, completed successfully
  across Android/Kotlin, Linux/Clang, Windows/Clang, and macOS/Clang

This snapshot is the corrected Phase 1 component baseline used to seed
Phase 2 planning:

- overall tracked coverage: `39.28%` over `68,001` lines in `551` files
- `audio`: `20.95%`
- `canvas`: `64.06%`
- `dsl`: `63.38%`
- `events`: `24.14%`
- `format`: `43.86%`
- `host`: `33.38%`
- `midi`: `45.2%`
- `osc`: `60.25%`
- `platform`: `35.41%`
- `render`: `55.87%`
- `runtime`: `46.89%`
- `signal`: `61.85%`
- `state`: `50.94%`
- `view`: `42.41%`
- `android`: `13.83%`
- `apple`: `29.93%`
- `linux`: `0.0%`
- `windows`: `0.0%`
- `cli`: `21.38%`
- `ship`: `31.45%`
- `tools`: `34.07%`

Representation proof points from the same Codecov snapshot:

- Apple files now visible:
  - `apple/Sources/PulpSwift/PulpBridge.cpp`: `70.78%`
  - `apple/Sources/PulpSwift/PulpBridge.swift`: `98.7%`
  - `apple/Sources/PulpSwift/PulpParameter.swift`: `100.0%`
  - `apple/Sources/PulpSwift/PulpViews.swift`: `79.79%`
- Python perimeter files now visible, including top-level `tools/*.py`,
  `tools/packages/**`, `tools/deps/**`, `tools/local-ci/**`,
  `tools/scripts/**`, and `core/view/js/embed_js.py`.
- `bindings/python/bindings.cpp` is visible as a counted file
  (`0.0%`, `0/121` lines). It is represented but still a test gap.
- `tools/packages/freshness_check.py` and
  `tools/packages/validate_registry.py` are visible as counted files.
- Python test modules remain intentionally omitted from the reported
  source set.

## Clearly known still not on Codecov

These are explicit perimeter decisions after the corrected baseline.
They should not be treated as silent omissions while Phase 2 ranks the
represented surface.

- Authored JavaScript source roots remain out of the represented surface
  until a dedicated JS lane is added - tracked by `#659`.
- `bindings/nodejs/bindings.cpp` remains explicitly out of scope for now
  because the repo does not have a supported Node binding CI/test lane -
  tracked by `#657`.
- Shell and PowerShell scripts remain explicitly out of scope for now.
  They are tested indirectly where practical, but do not surface as
  first-class Codecov lines - tracked by `#657`.
- iOS-only Swift and standalone Swift outside the macOS SwiftPM lane stay
  outside the represented surface unless a later simulator/runtime lane
  pulls them in. Known examples:
  - `apple/Sources/PulpSwift/PulpAudioSession.swift`
  - `templates/ios-auv3/HostApp/ContentView.swift`
  - `templates/ios-auv3/HostApp/PulpHostApp.swift`
  - `tools/local-ci/macos_window_probe.swift`
  tracked by `#77`; the Apple perimeter classification work in `#656`
  is closed.
- `apple/Sources/PulpSwift/PulpBridge.h` is a declaration-only C header
  and is not materialized as an executable-line Codecov row. The
  implementation in `PulpBridge.cpp` is now represented and tested.
- Optional native surfaces that are not compiled by the current coverage
  configure, such as AAX/ARA runtime sources, Android native device
  shims, and Web/WASM-specific sources, remain outside the measured
  graph unless Phase 2 decides to add focused follow-up issues.

## Phase 1 issue map

### Control plane and verification

- `#641` authoritative umbrella and phase tracker
- `#639` Codecov control plane and dashboard truth follow-up
- `#647` merged representation tranche for `dsl`, `windows`, and the
  durable status report
- `#715` merged Python coverage normalization / zero-file visibility fix

### Completed perimeter-expansion work

- `#656` Swift / Apple perimeter classification, with
  `PulpBridge.cpp` represented through `#678`
- `#658` Python perimeter expansion, implemented through `#677` and
  normalized by `#715`
- `#679` / `#680` `bindings/python/bindings.cpp` representation
- `#633` Android/Kotlin JaCoCo lane
- `#615` Apple Swift package lane

### Remaining explicit perimeter follow-ups

- `#659` add a JavaScript source lane for authored repo assets
- `#657` classify optional bindings and shell / PowerShell surfaces in
  docs; after this rebaseline lands, the remaining implementation
  decision is whether to keep Node and scripts out of scope or file
  dedicated future lanes
- `#77` decide and, if in scope, add mobile runtime coverage from
  Android instrumentation and iOS simulator / runtime app paths

### Supporting infrastructure

- `#671` Windows Shipyard bundle-lock infra failure: closed
- `#692` Windows Namespace capacity fallback for PR lanes: closed
- `#655` Android SDK / NDK provisioning on SSH validation hosts remains
  useful supporting infra but is not a blocker for hosted Codecov truth
  on `main`
- `#568` historical multi-language expansion umbrella. Active execution
  now rolls up through `#641`.

## Phase 2 / Phase 3 working queue

Phase 2 started from the component snapshot above, not from older
pre-`#715` numbers. The current execution rule is:

1. Rank below-target counted components against `ci/coverage-targets.yaml`.
2. Separate represented but zero/low coverage files from intentionally
   unrepresented surfaces.
3. Land small, focused PRs that improve represented files and keep diff
   coverage above the required 75% floor.
4. Refresh this doc and `#641` after each batch of merges or after a
   meaningful Codecov `main` rebaseline.

### Phase 2 / Phase 3 tranche issues

- `#640` audio / platform tranche
- `#642` events tranche
- `#643` CLI / tools tranche
- `#644` ship tranche
- `#645` midi / signal tranche
- `#646` render tranche
- `#493` host / format / view and related hardening gaps
- `#737` optional native surfaces not compiled by the default coverage
  configuration

### Deferred perimeter follow-ups

- `#659` authored JavaScript source lane
- `#657` Node bindings plus shell / PowerShell classification, closed as
  explicitly out of scope for now
- `#77` mobile runtime / iOS simulator coverage

## Control-plane invariants

- Every top-level `core/*` directory must have matching subsystem
  entries in `codecov.yml` `flags:` and `component_management:`.
- Platform axes must match the live repo path conventions, including
  `core/**/win/**` for Windows.
- Swift upload artifacts must be repo-relative LCOV with `SF:` paths
  rooted under `apple/Sources/**`.
- Android Kotlin coverage must run on the always-on Coverage workflow,
  not only on path-gated Android workflow commits.
- Upload-only flags stay `os-linux`, `os-macos`, and `os-windows`.
- `codecov.yml` `ignore:` must stay aligned with
  `scripts/run_coverage.sh` `COVERAGE_IGNORE_REGEX`.
- The authoritative program tracker is `#641`, not the older language
  umbrella `#568`.

## Validation commands for this slice

- `python3 tools/scripts/test_codecov_config.py`
- `python3 tools/scripts/test_run_swift_coverage.py`
- `python3 tools/scripts/test_coverage_diff_comment.py`
- `tools/check-docs.sh`

## Follow-ups not solved by this doc

- If another top-level `core/*` subsystem is added, update `codecov.yml`
  in the same change; the structural test should fail immediately if the
  mapping is missed.
- `docs/reference/modules.md` still lacks a `#dsl` section even though
  `docs/status/modules.yaml` points at one. That is real docs drift, but
  it is outside this coverage-compliance slice.
