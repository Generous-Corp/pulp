# Coverage Compliance Status

Last reviewed: 2026-04-26 22:49 EDT

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

Latest complete Codecov `main` report observed while updating this doc
(`main` head `62ad9870` has a newer Coverage run queued):

- commit: `5b8e4529cfc553fd98b65f265ed1e9c0487b3d66`
- workflow: Coverage run `24970413173`, completed successfully
- overall tracked coverage: `46.83%` over `70,453` lines in `556` files
- covered lines: `32,996`
- missed lines: `36,165`
- partial lines: `1,292`
- current component coverage from the Codecov API:
  - `audio`: `37.36%`
  - `canvas`: `64.74%`
  - `dsl`: `68.3%`
  - `events`: `49.78%`
  - `format`: `51.72%`
  - `host`: `43.07%`
  - `midi`: `54.56%`
  - `osc`: `66.26%`
  - `platform`: `40.16%`
  - `render`: `62.77%`
  - `runtime`: `49.11%`
  - `signal`: `73.87%`
  - `state`: `63.56%`
  - `view`: `44.0%`
  - `android`: `13.83%`
  - `apple`: `25.36%`
  - `linux`: `3.31%`
  - `windows`: `0.0%`
  - `cli`: `40.86%`
  - `ship`: `53.99%`
  - `tools`: `45.32%`

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
- `#818` MPE synth voice / allocator coverage -> `3d4560d1`
- `#817` UMP conversion edge coverage -> `a38216fd`
- `#821` audio reader helper edge coverage -> `7aff17f9`
- `#822` audio file helper edge coverage -> `87ef6d90`
- `#819` MIDI keyboard/sequence edge coverage -> `7a646f33`
- `#820` signal spectrogram edge coverage -> `1a1b7f07`
- `#823` platform helper edge coverage -> `f69338eb`
- `#824` signal math helper coverage and Mat3 fix -> `94e3ef2a`
- `#825` signal DSP helper edge coverage -> `d0262a1b`
- `#826` OGG reader edge coverage -> `6ae4d487`
- `#827` signal multi-channel meter edge coverage -> `d5efea57`
- `#828` raw MIDI parser edge coverage and malformed-data hardening -> `5b8e4529`
- `#831` signal utility helper edge coverage -> `b5e7a463`
- `#836` CLI sync checker Python tooling coverage -> `6ef8ca8b`
- `#837` package registry validation tools coverage -> `bdc406cc`
- `#830` RPN parser edge coverage -> `d02c9ec9`
- `#834` events event-loop lifecycle edge coverage -> `455c0a9d`
- `#829` MIDI-CI edge coverage -> `eaf991b0`
- `#833` signal filter-design edge coverage -> `8fc5dd2d`
- `#838` CLI release packager coverage -> `3d47dbd2`
- `#839` GPU graph render helper hardening/coverage -> `62ad9870`

Open Phase 3 PRs:

- `#832` platform child-process edge coverage for `#640`, branch
  `feature/platform-child-process-coverage-640`, head `7496ba0a`;
  opened from `main` at `d5efea57`, updated after the first Windows
  Namespace run exposed a cmd/PowerShell portability issue in the test
  commands, then rebased onto `87ef6d90` and updated after Windows
  exposed the fast-exit no-newline `cmd` probe returning nonzero, then
  rebased onto `b5e7a463` and updated after review flagged a fixed
  100ms fast-exit deadline. This tranche also uses the
  GitHub/Namespace path while the SSH `windows` target remains
  unreachable.
- `#835` CLI ship command validation coverage for `#643`, branch
  `feature/cli-ship-coverage-643`, head `b8f22af9`; opened from
  `main` at `5b8e4529`, then rebased onto `b5e7a463` and updated after
  Linux Namespace exposed that the Android validation test created
  `artifacts/` before asserting the missing-artifacts check path. It was
  updated again to isolate Android signing config via a scratch
  `PULP_HOME` and cleared password env vars. This tranche also uses the
  GitHub/Namespace path while the SSH `windows` target remains
  unreachable.
- `#840` events child-process manager coverage for `#642`, branch
  `feature/events-child-process-coverage-642`, head `410a0371`; opened
  from `main` at `455c0a9d`, then rebased onto `eaf991b0` after `#829`
  merged. The first Linux Namespace and Linux coverage failures were
  dependency-bootstrap failures before configure/tests because a cold
  FetchContent cache hit a GitHub 403 cloning `Tracktion/choc.git`. The
  first diff-coverage failure was fixed by pushing `410a0371`, which
  keeps the production change focused on the covered invalid-PID guard
  and removes unrelated POSIX `waitpid` hardening from this tranche.
  This tranche also uses the GitHub/Namespace path while the SSH
  `windows` target remains unreachable.
- `#842` inspect Codecov classification for `#841` / parent `#641`,
  branch `feature/inspect-codecov-component-841`, head `0541fd69`;
  promotes `inspect/**` to a first-class Codecov flag/component and
  classifies it in the existing user-facing coverage tier. Local
  validation passed the Codecov config test, coverage-tier check,
  docs-sync report, diff check, skill-sync report, and version-bump
  report.
- `#843` MPE tracker UMP edge-path coverage for `#645`, branch
  `feature/midi-mpe-tracker-extra-coverage-645-next`, head `fcdbd278`;
  opened from `main` at `eaf991b0`. This test-only tranche covers
  unmatched MIDI 2.0 per-note expression packets, MIDI 2.0 member
  expression fan-out across active channel notes, and supported-zone UMP
  packets that should be consumed without mapped side effects.
- `#845` host automation queue coverage for `#493`, branch
  `feature/format-host-coverage-493-next`, head `c3c5cfce`; covers
  `ParameterEventQueue` sample-offset sorting and clear/reuse lifecycle.
- `#846` audio focus coverage for `#640`, branch
  `feature/audio-platform-coverage-640-next`, head `97568e27`; covers
  `AudioFocusRegistry` token move-assignment, default-token reset, and
  publish-without-subscribers state behavior.
- `#847` events NSD / async helper coverage for `#642`, branch
  `feature/events-volume-coverage-642-next`, head `8365c969`; covers
  `LambdaAsyncUpdater` empty-callback processing plus
  `NetworkServiceDiscovery` dispatcher and service-keying behavior.
- `#848` runtime Base64 padding validation for parent `#641`, branch
  `feature/runtime-coverage-641-next`, head `36aa5c71`; hardens
  `base64_decode` terminal-padding validation and carries a patch-level
  SDK metadata trailer.
- `#849` view label paint coverage for `#493`, branch
  `feature/view-widget-coverage-493-next`, head `7f6deaf6`; covers
  label text transforms, multiline/decorations, and vertical text
  direction transform wrapping.
- `#850` CLI/tools runner resolver coverage for `#643`, branch
  `feature/cli-tools-coverage-643-next2`, head `b8e7b22a`; covers
  optional Namespace fallback plus provider/default argument validation.
- `#851` signal meter channel-count guards for `#645`, branch
  `feature/signal-meter-coverage-645-next`, head `ad9c3d81`; hardens
  negative/invalid `MultiChannelMeter` and `MultiChannelBallistics`
  channel counts and carries a patch-level SDK metadata trailer.
- `#852` canvas rectangle-list helper coverage for parent `#641`,
  branch `feature/runtime-helper-coverage-641-next2`, head `05c85d55`;
  consolidates overlapping rectangle-list worker output into one
  test-only PR covering empty-rectangle filtering, intersection
  semantics, subtract no-op/full-cover, bounding-box/area, and clipped
  intersections.
- `#853` audio frame-fill invalid-dimension coverage for `#640`,
  branch `feature/audio-helper-coverage-640-next2`, head `53bc6020`;
  covers `zero_fill_short_read` negative channel-count and negative
  total-frame no-op guards.
- `#854` ViewBridge helper coverage for `#493`, branch
  `feature/host-view-helper-coverage-493-next2`, head `5c9eeeba`;
  covers idempotent open/release behavior, secondary-view null/bounds
  cleanup, and null remote-channel diagnostics.
- `#855` signal convolver edge coverage for `#645`, branch
  `feature/midi-signal-helper-coverage-645-next2`, head `2d8b0bc3`;
  covers rounded/zero block-size setup, empty IR pass-through, and
  wrong-block-size pass-through behavior.
- `#856` CLI/tools LCOV/Cobertura helper coverage for `#643`, branch
  `feature/cli-tools-coverage-643-next3`, head `96d20a3a`; covers
  package exclusions before summary-rate calculation, duplicate
  source-record function-hit merging, and relpath fallback paths.
- `#857` events async-helper coverage for `#642`, branch
  `feature/events-helper-coverage-642-next2`, head `4cbfd67c`; adds a
  focused async-helper target covering reentrant `AsyncUpdater`,
  `LambdaAsyncUpdater` empty-callback handling, `ActionBroadcaster`
  missing-removal behavior, and `MultiTimer` stop-all/restart behavior.

Local Phase 3 draft worktrees:

- `#640` AIFF reader edge-path worktree
  `/Users/danielraffel/Code/pulp-audio-aiff-coverage-640`, branch
  `feature/audio-aiff-coverage-640`; merged via PR `#794` as
  `c4e6ad09`. The remote branch was deleted after merge.
- `#642` events volume/service-discovery worktree
  `/Users/danielraffel/Code/pulp-events-volume-coverage-642`, branch
  `feature/events-volume-coverage-642`, commit `357fc429`; merged via PR
  `#795` as `f1a5aa84`. The remote branch was deleted after merge.
- `#642` events event-loop lifecycle worktree
  `/Users/danielraffel/Code/pulp-events-loop-coverage-642`, branch
  `feature/events-loop-coverage-642`, commit `ee3894cb`; merged via PR
  `#834` as `455c0a9d`. The remote branch was deleted after merge.
  Scope: test-only coverage for `EventLoop` thread identity, explicit
  stop and idempotent stop state, delayed work dropped after stop,
  multiple delayed tasks, plus `Timer` interval getter/setter and
  idempotent stop-state behavior. Local validation: no-GPU/no-examples
  configure, `pulp-test-events` build, direct `[issue-642]` run passed
  `14` assertions in `4` cases, full binary passed `74` assertions in
  `20` cases, precise CTest selector passed `18/18`, `git diff --check`,
  skill-sync report, and version-bump report.
- `#642` events child-process manager worktree
  `/Users/danielraffel/Code/pulp-events-child-process-coverage-642`,
  branch `feature/events-child-process-coverage-642`, commit
  `47e38795`; open as PR `#840`.
  Scope: covers unlaunched `ConnectedChildProcess` default state,
  disconnected send, idempotent kill, and `wait_for_exit()` return
  behavior; covers empty `ChildProcessManager` active-count, wait, kill,
  and cleanup no-op lifecycle; and hardens
  `ConnectedChildProcess::wait_for_exit()` so unlaunched/invalid PID
  state returns `-1` instead of probing an invalid process id. Local
  validation before opening: no-GPU/no-examples configure,
  `pulp-test-ipc` build, direct `[issue-642]` run passed `7`
  assertions in `2` cases, full IPC binary passed `44` assertions in
  `12` cases, focused CTest `ConnectedChildProcess|ChildProcessManager|IPC`
  passed `12/12`, diff check, skill-sync report, and version-bump
  report. Follow-up validation after rebasing onto `eaf991b0` passed the
  direct issue selector, focused CTest, diff check, skill-sync report,
  version-bump report, and pre-push gates.
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
- `#643` CLI ship command validation worktree
  `/Users/danielraffel/Code/pulp-cli-ship-coverage-643`, branch
  `feature/cli-ship-coverage-643`, commit `b8f22af9`; open as PR
  `#835`.
  Scope: test-only `pulp ship` shellout coverage for fake project roots
  with missing build-cache guidance, Android signing/package/check
  validation before external tools run, appcast feed generation, and
  remote `--sign-key` rejection without real signing material. The
  follow-up commit moves the Android `ship check --target android`
  missing-artifacts assertion before package probes create `artifacts/`
  as part of their validation path. The second follow-up sets `PULP_HOME`
  to a scratch directory and clears Android password env vars before
  asserting the missing-keystore branch, so the test is not affected by a
  developer or runner config file. Local validation after rebasing onto
  `5b8e4529`: no-GPU/no-examples
  configure, `pulp-test-cli-ship-shellout` build, direct `[issue-643]`
  run passed `3` assertions in `3` cases, full binary passed `10`
  assertions in `10` cases, focused CTest `pulp ship` passed `10/10`,
  `git diff --check HEAD~1..HEAD`, `git diff --check`, skill-sync
  report, and version-bump report with `Version-Bump: sdk=skip` for
  the test-only coverage tranche. Follow-up validation after rebasing
  onto `b5e7a463` and applying both follow-ups used a real built CLI path
  and passed the focused Android validation test with `12` assertions,
  the `[issue-643]` slice with `25` assertions in `3` cases, the full
  binary with `45` assertions in `10` cases, diff check, and pre-push
  gates. The two Codex review threads on the ordering/config isolation
  issues were resolved.
- `#643` CLI sync checker worktree
  `/Users/danielraffel/Code/pulp-cli-sync-coverage-643`, branch
  `feature/cli-sync-coverage-643`, commit `86048a3f`; merged via PR
  `#836` as `6ef8ca8b`. The remote branch was deleted after merge.
  Scope: test-only Python coverage for `tools/scripts/cli_sync_check.py`
  repo-root discovery, command-table/YAML/slash-command/skill-reference
  extraction, strict JSON mismatch exits, warning-only success behavior,
  and non-repo diagnostics. Local validation: direct
  `python3 tools/scripts/test_cli_sync_check.py` passed `7` cases,
  `git diff --check`, skill-sync report, and version-bump report. The
  local Python coverage runner could not run because this interpreter
  lacks `coverage.py >= 7.10`; hosted CI remains the coverage proof for
  this tranche.
- `#643` package registry validation tools worktree
  `/Users/danielraffel/Code/pulp-package-validator-coverage-643`,
  branch `feature/package-validator-coverage-643`, commit `8a2edbe9`;
  merged via PR `#837` as `bdc406cc`. The remote branch was deleted
  after merge.
  Scope: test-only Python coverage for
  `tools/packages/validate_registry.py` JSON load/error paths,
  structural and license classification, and CLI success output, plus
  `tools/packages/freshness_check.py` GitHub URL parsing, `gh api`
  failure handling, issue aggregation, and package-filtered JSON output.
  Local validation: direct
  `python3 tools/packages/test_package_validation_tools.py` passed `9`
  cases, `git diff --check HEAD~1..HEAD`, skill-sync report with
  `Skill-Update: skip skill=packages`, and version-bump report. The
  local Python coverage runner could not run because this interpreter
  lacks `coverage.py >= 7.10`; hosted CI remains the coverage proof for
  this tranche.
- `#646` GPU graph render helper worktree
  `/Users/danielraffel/Code/pulp-render-gpu-graph-coverage-646`,
  branch `feature/render-gpu-graph-coverage-646`, commit `fa3deb3b`;
  merged via PR `#839` as `62ad9870`. The remote branch was deleted
  after merge.
  Scope: hardens `GpuGraphRenderer`, `GpuHeatMapRenderer`, and
  `GpuBarRenderer` raw-pointer `set_data()` paths for null, zero-sized,
  and overflowing inputs; adds focused `test_gpu_graph.cpp` edge
  coverage; and moves the header-only dirty-tracker and GPU-graph tests
  out of the GPU-only CMake block so GPU-off sanitizer/local coverage
  builds can exercise them without linking `pulp::render`. Local
  validation after rebasing onto `d02c9ec9`: GPU-off configure,
  `pulp-test-gpu-graph` and `pulp-test-dirty-tracker` builds, direct
  `[issue-646]` run passed `39` assertions in `7` cases, full
  gpu-graph binary passed `52` assertions in `11` cases, full
  dirty-tracker binary passed `52` assertions in `18` cases, focused
  CTest passed `29/29`, diff check, skill-sync report, version-bump
  report with an SDK patch trailer, and pre-push gates.
- `#643` CLI release packager worktree
  `/Users/danielraffel/Code/pulp-package-cli-coverage-643`, branch
  `feature/package-cli-coverage-643`, commit `09666f46`; merged via PR
  `#838` as `3d47dbd2`. The remote branch was deleted after merge.
  Scope: test-only Python coverage for `tools/scripts/package_cli.py`
  wgpu library discovery, macOS/Linux rpath command selection,
  tar/zip archive writer layout, missing-binary and missing-library
  exits, and successful Linux/Windows package staging with external
  tools mocked. Follow-up repair after rebasing onto `d02c9ec9` isolates
  Android package test temp dirs with process id plus an atomic
  per-process counter, fixing the macOS Namespace failure where
  concurrently discovered Catch2 test processes could read a neighboring
  fake SDK and report `35.0.1` instead of `10.0.0`. Local validation:
  direct `python3 tools/scripts/test_package_cli.py` passed `10` cases,
  no-GPU/no-examples configure, `pulp-test-android-package` build, direct
  `Android SDK discovery compares tool revisions numerically` run passed
  `5` assertions in `1` case, matching CTest passed, parallel CTest
  `Android SDK discovery` passed `3/3`, broader parallel CTest `Android`
  passed `8/8`, `git diff --check origin/main...HEAD`, skill-sync report,
  and version-bump report. The local Python coverage runner could not
  run because this interpreter lacks `coverage.py >= 7.10`; hosted CI
  remains the coverage proof for this tranche.
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
  merged via PR `#817` as `a38216fd`. The remote branch was deleted
  after merge.
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
  `21eb74e1`; merged via PR `#818` as `3d4560d1`. The remote branch was
  deleted after merge.
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
  `60676709`; merged via PR `#819` as `7a646f33`. The remote branch was
  deleted after merge.
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
  merged via PR `#820` as `1a1b7f07`. The remote branch was deleted
  after merge.
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
  `be7d604f`; merged via PR `#821` as `7aff17f9`. The remote branch was
  deleted after merge.
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
- `#640` audio file helper worktree
  `/Users/danielraffel/Code/pulp-audio-file-helper-coverage-640`,
  branch `feature/audio-file-helper-coverage-640`, head `2db6be4a`;
  merged via PR `#822` as `87ef6d90`. The remote branch was deleted
  after merge.
  Scope: coverage for packed int24 conversion, int32 full-scale
  conversion, float-to-int32 clamping, zero-count conversion no-ops, CHOC
  WAV helper output via RIFF chunk parsing, malformed WAV rejection,
  invalid OGG registry dispatch, `Buffer` resize/view ownership, and
  `AudioProcessLoadMeasurer` smoothing/zero-available-time guards.
  Follow-up `8aa7a78d` fixes a production `float_to_int32` UBSan issue
  by handling `+/-1` endpoints explicitly and using double precision for
  the midrange cast. Follow-up `fc7a1364` fixes a macOS Namespace
  full-suite blocker by comparing Android build-tools and NDK revisions
  numerically, with `2db6be4a` carrying the required skill/version
  metadata trailers. Local validation: no-GPU/no-examples configure,
  `pulp-test-audio-file`, `pulp-test-audio`, and
  `pulp-test-load-measurer` build, later `pulp-test-android-package`
  build, direct `[issue-640]` audio-file run passed `217` assertions in
  `11` cases, full audio-file binary passed `450` assertions in `26`
  cases, full audio binary passed `586` assertions in `8` cases, full
  load-measurer binary passed `20` assertions in `6` cases, full Android
  package binary passed `69` assertions in `8` cases, focused CTest
  passed `11/11` before the Android fix and `9/9` for the Android/load
  measurer rerun after the fix, local UBSan focused CTest for `sample
  conversion handles packed 24-bit and 32-bit edges` passed,
  skill-sync report passed with the `ship` bypass trailer, and
  version-bump report classified the SDK change as `patch`.
- `#640` platform helper worktree
  `/Users/danielraffel/Code/pulp-platform-helper-coverage-640`, branch
  `feature/platform-helper-coverage-640`, commit `6ec4ff2e`; merged via
  PR `#823` as `f69338eb`. The remote branch was deleted after merge.
  Scope: test-only coverage for file-dialog no-handler backend failure
  paths, popup-menu item metadata and non-Apple stub behavior, audio
  excerpt value/default helpers, and non-Apple `AudioWorkgroup` fallback
  idempotency. Local validation: no-GPU/no-examples configure,
  `pulp-test-file-dialog`, `pulp-test-audio-excerpt`, and
  `pulp-test-workgroup` build, direct `[issue-640]` file-dialog run
  passed `13` assertions in `1` case, direct
  `[audio][excerpt][issue-640]` audio-excerpt run passed `14`
  assertions in `1` case, full workgroup binary passed `4` assertions in
  `5` cases, full file-dialog binary passed `15` assertions in `3`
  cases, full audio-excerpt binary passed `33` assertions in `5` cases,
  focused CTest passed `9/9`, `git diff --check`, skill-sync report, and
  version-bump report.
- `#645` signal math helper worktree
  `/Users/danielraffel/Code/pulp-signal-math-helper-coverage-645`,
  branch `feature/signal-math-helper-coverage-645`, commits `b304e0f4`
  and metadata tip `1b7f1cfd`; merged via PR `#824` as `94e3ef2a`. The
  remote branch was deleted after merge.
  Scope: fixes `Mat3::operator*` so multiplication accumulates into a
  zero matrix instead of adding the product onto the identity default,
  and adds coverage for polynomial empty/constant helpers, empty
  polynomial add, singular `Mat2` inverse, non-identity `Mat3`
  multiplication, FastMath large-phase wrapping and guard inputs,
  Gain linear buffer processing, SimpleMixer clamp/buffer paths, and
  special-function wrappers. The metadata tip records
  `Version-Bump: sdk=patch` because this is an inline public-header bug
  fix without an SDK API signature change. Local validation:
  no-GPU/no-examples configure, `pulp-test-poly-math`,
  `pulp-test-fast-math`, `pulp-test-signal`, and
  `pulp-test-dsp-enhancements` builds, direct issue-645 slices passed
  `30`, `11`, `128`, and `39` assertions respectively, full binaries
  passed `62`, `55`, `895`, and `71` assertions respectively, focused
  CTest passed `11/11`, `git diff --check`, skill-sync report, and
  version-bump report.
- `#645` signal DSP helper worktree
  `/Users/danielraffel/Code/pulp-signal-dsp-helper-coverage-645`,
  branch `feature/signal-dsp-helper-coverage-645`, commit `da05acaf`;
  merged via PR `#825` as `d0262a1b`. The remote branch was deleted
  after merge.
  Scope: test-only coverage for `AlignedBuffer` move assignment,
  same-size resize, empty clear, and copy truncation paths;
  `BallisticsFilter` time-constant clamp and buffer processing paths;
  `LogRampedValue` non-positive endpoint jump paths; and all
  `WaveShaper` curve branches plus in-place buffer processing. Local
  validation: no-GPU/no-examples configure, `pulp-test-simd` and
  `pulp-test-dsp-expansion` builds, direct issue-645 slices passed `12`
  assertions in `3` SIMD cases and `22` assertions in `4` DSP cases,
  full binaries passed `2927` assertions in `23` cases and `75`
  assertions in `34` cases, focused CTest passed `7/7`,
  `git diff --check`, skill-sync report, and version-bump report.
- `#640` OGG reader worktree
  `/Users/danielraffel/Code/pulp-audio-ogg-reader-coverage-640`,
  branch `feature/audio-ogg-reader-coverage-640`, commit `6b721c22`;
  merged via PR `#826` as `6ae4d487`. The remote branch was deleted
  after merge.
  Scope: dedicated `pulp-test-ogg-reader` target covering direct OGG
  reader factory construction, `.ogg` / `.oga` extension support,
  unsupported extension behavior, format-name reporting, and
  missing/malformed input failure paths. This intentionally avoids
  `test_audio_file.cpp` while `#822` is open. Local validation:
  no-GPU/no-examples configure, `pulp-test-ogg-reader` build, direct
  `[audio][ogg][issue-640]` run passed `12` assertions in `2` cases,
  full binary passed `12` assertions in `2` cases, focused CTest passed
  `2/2`, `git diff --check`, skill-sync report, and version-bump report.
- `#645` signal multi-channel meter worktree
  `/Users/danielraffel/Code/pulp-signal-meter-coverage-645`, branch
  `feature/signal-meter-coverage-645`, commit `ed51248f`; merged via PR
  `#827` as `d5efea57`. The remote branch was deleted after merge.
  Scope: dedicated `pulp-test-multi-channel-meter` target covering
  prepared-channel clamping, empty process blocks, correlation window
  replacement after reset, integrated LUFS running-average behavior,
  ballistics release/noise-floor clamping, and held-peak refresh paths.
  This intentionally avoided `test_signal.cpp` while `#824` was open.
  Local validation: no-GPU/no-examples configure,
  `pulp-test-multi-channel-meter` build, direct `[issue-645]` run
  passed `20` assertions in `6` cases, full binary passed `20`
  assertions in `6` cases, focused CTest passed `6/6`,
  `git diff --check`, skill-sync report, and version-bump report.
- `#645` raw MIDI parser worktree
  `/Users/danielraffel/Code/pulp-midi-raw-parser-coverage-645`, branch
  `feature/midi-raw-parser-coverage-645`, commit `4e2e6092`; merged via
  PR `#828` as `5b8e4529`. The remote branch was deleted after merge.
  Scope: fixes `parse_raw_midi_bytes` so malformed short messages do not
  emit status bytes as data, then adds coverage for remaining
  short-message forms, stray data plus incomplete short messages,
  omitted callbacks, and recovery after the runaway sysex cap resets the
  accumulator. Local validation: no-GPU/no-examples configure,
  `pulp-test-raw-midi-parser` build, direct `[issue-645]` run passed
  `16` assertions in `3` cases, full binary passed `53` assertions in
  `9` cases, focused CTest passed `4/4`, `git diff --check`,
  skill-sync report, and version-bump report with
  `Version-Bump: sdk=patch` because the inline header is an internal
  transport helper despite living under `include`.
- `#645` MIDI-CI worktree
  `/Users/danielraffel/Code/pulp-midi-ci-coverage-645`, branch
  `feature/midi-ci-coverage-645`, commit `1d3cd1ce`; merged via PR
  `#829` as `eaf991b0`. The remote branch was deleted after merge.
  Scope: test-only coverage for profile inquiry destination encoding,
  malformed and unhandled CI messages, discovery inquiry destination
  filtering, discovery reply storage plus callback dispatch, unknown
  profile enable/disable no-op paths, and enabled/disabled profile reply
  payload layout. Local validation: no-GPU/no-examples configure,
  `pulp-test-midi-ci` build, direct `[issue-645]` run passed `38`
  assertions in `6` cases, full binary passed `65` assertions in `15`
  cases, focused CTest passed `6/6`, `git diff --check`, skill-sync
  report, and version-bump report. Rebase recovery validation on
  2026-04-26 after `#822` merged also passed the build, direct/full
  binary runs, diff whitespace checks, skill-sync report, and
  version-bump report; the focused CTest pattern found no local tests in
  that incremental build but exited cleanly.
- `#645` MPE tracker UMP edge-path worktree
  `/Users/danielraffel/Code/pulp-midi-mpe-tracker-extra-coverage-645-next`,
  branch `feature/midi-mpe-tracker-extra-coverage-645-next`, commit
  `fcdbd278`; open as PR `#843`.
  Scope: test-only coverage for unmatched MIDI 2.0 per-note expression
  handling without seeding channel state, MIDI 2.0 member expression
  fan-out across active channel notes, and supported-zone UMP packets
  that are consumed without mapped side effects. Local validation:
  no-GPU/no-examples configure, `pulp-test-mpe-voice-tracker` build,
  direct `[issue-645]` run passed `195` assertions in `9` cases, full
  binary passed `268` assertions in `26` cases, focused CTest
  `MpeVoiceTracker` passed `26/26`, `git diff --check`, skill-sync
  report, and version-bump report.
- `#645` RPN parser worktree
  `/Users/danielraffel/Code/pulp-midi-rpn-coverage-645`, branch
  `feature/midi-rpn-coverage-645`, commit `d1edf1e1`; merged via PR
  `#830` as `d02c9ec9`. The remote branch was deleted after merge.
  Scope: test-only coverage for incomplete selections, unrelated CCs,
  omitted callbacks on otherwise complete RPN/NRPN sequences, and NRPN
  increment/decrement metadata. Local validation: no-GPU/no-examples
  configure, `pulp-test-midi-expansion` build, direct
  `[midi][rpn][issue-645]` run passed `6` assertions in `3` cases, full
  binary passed `57` assertions in `23` cases, focused CTest passed
  `3/3`, `git diff --check`, skill-sync report, and version-bump
  report.
- `#645` signal utility helper worktree
  `/Users/danielraffel/Code/pulp-signal-utility-coverage-645`, branch
  `feature/signal-utility-coverage-645`, commits `d021a35c` and
  `1b8169aa`; merged via PR `#831` as `b5e7a463`. The remote branch was
  deleted after merge.
  Scope: test-only coverage for `DelayLine` empty/wraparound/reset
  paths, `SmoothedValue` one-sample clamp and partial skip paths, `Svf`
  bandpass/notch/buffer/reset paths, `Panner` clamp plus in-place stereo
  processing, `Phaser` stage/feedback clamp safety plus buffer/reset/dry
  paths, and `Bias` getter plus separate input/output buffer paths. The
  follow-up commit compares Phaser reset output with tolerance instead
  of exact float-array equality for MSVC/math-library variance. Local
  validation: no-GPU/no-examples configure, `pulp-test-signal` and
  `pulp-test-v3-gaps` builds, direct signal `[issue-645]` run passed
  `167` assertions in `21` cases, direct v3-gaps `[issue-645]` run
  passed `44` assertions in `4` cases, full signal binary passed `934`
  assertions in `74` cases, full v3 gaps binary passed `131` assertions
  in `27` cases, focused CTest passed `6/6`, `git diff --check`, skill-sync
  report, and version-bump report. Cloud checks were clean before merge.
- `#640` platform child-process worktree
  `/Users/danielraffel/Code/pulp-platform-child-process-coverage-640`,
  branch `feature/platform-child-process-coverage-640`, commits
  `3018c8d5`, `a361c8b9`, `c07dd268`, and `7496ba0a`; open as PR
  `#832`.
  Scope: test-only coverage for pre-start wait/read defaults,
  working-directory launch, stdout/stderr max-output byte caps,
  stderr-line callbacks, and fast-exit output preservation after
  `is_running()` observes process completion. The follow-up commit
  removed a PowerShell dependency and trims stderr callback comparisons
  so the test is resilient to `cmd` redirection spacing on Windows; the
  second follow-up forces the fast-exit Windows `cmd` probe to exit zero
  while still proving no-newline cached output is preserved. The third
  follow-up replaces the fixed 100ms fast-exit polling loop with a 5s
  deadline so loaded/virtualized runners do not fail before normal
  process launch/teardown completes.
  Local validation: no-GPU/no-examples configure,
  `pulp-test-child-process` build, direct `[issue-640]` run passed `25`
  assertions in `5` cases, full binary passed `46` assertions in `17`
  cases, focused CTest passed for the fast-exit case,
  `git diff --check HEAD~1..HEAD`, `git diff --check`, skill-sync
  report, and version-bump report after rebasing onto `87ef6d90`.
  Follow-up validation after rebasing onto `b5e7a463` passed the focused
  fast-exit test with `4` assertions, the `[issue-640]` slice with `25`
  assertions in `5` cases, the full child-process binary with `46`
  assertions in `17` cases, diff check, skill-sync report, version-bump
  report, and pre-push gates. The Codex review thread on the fast-exit
  deadline was resolved.
- `#645` signal filter-design worktree
  `/Users/danielraffel/Code/pulp-signal-biquad-coverage-645`, branch
  `feature/signal-biquad-coverage-645`, commit `e3795fca`; merged via
  PR `#833` as `8fc5dd2d`. The remote branch was deleted after merge.
  Scope: test-only coverage for peaking boost/cut coefficients, low/high
  shelf cut paths, shelf slope variants, and Butterworth empty/high-order
  section behavior. This intentionally touches only
  `test_filter_design.cpp` to avoid overlap with open `test_signal.cpp`
  tranches. Local validation: no-GPU/no-examples configure,
  `pulp-test-filter-design` build, direct `[issue-645]` run passed `100`
  assertions in `3` cases, full binary passed `116` assertions in `14`
  cases, focused CTest passed `14/14`, `git diff --check HEAD~1..HEAD`,
  `git diff --check`, skill-sync report, and version-bump report.
  Follow-up validation after rebasing onto `b5e7a463` passed the same
  focused filter-design build/direct/CTest checks, built
  `pulp-test-android-package`, passed the direct `Android SDK discovery
  compares tool revisions numerically` selector with `5` assertions in
  `1` case, passed the matching CTest selector, passed
  `git diff --check origin/main...HEAD`, and cleared pre-push gates.
- `#493` host automation queue worktree
  `/Users/danielraffel/Code/pulp-format-host-coverage-493-next`,
  branch `feature/format-host-coverage-493-next`, commit `c3c5cfce`;
  open as PR `#845`. Scope: test-only coverage for
  `ParameterEventQueue` sample-offset sorting and clear/reuse lifecycle.
  Local validation passed the no-GPU/no-examples configure, affected
  target build, direct `[issue-493]` run, full host-regression binary,
  focused CTest selector, diff check, skill-sync report, and
  version-bump report.
- `#640` audio focus worktree
  `/Users/danielraffel/Code/pulp-audio-platform-coverage-640-next`,
  branch `feature/audio-platform-coverage-640-next`, commit
  `97568e27`; open as PR `#846`. Scope: test-only coverage for
  `AudioFocusRegistry` token move-assignment replacement, default token
  reset no-op behavior, and publish-without-subscribers state updates.
  Local validation passed the no-GPU/no-examples configure, affected
  target build, direct `[issue-640]` run, full binary, focused CTest,
  diff checks, skill-sync report, and version-bump report.
- `#642` events NSD / async helper worktree
  `/Users/danielraffel/Code/pulp-events-volume-coverage-642-next`,
  branch `feature/events-volume-coverage-642-next`, commit `8365c969`;
  open as PR `#847`. Scope: test-only coverage for
  `LambdaAsyncUpdater` empty-callback processing and
  `NetworkServiceDiscovery` backend dispatcher / service-name-plus-type
  keying behavior. Local validation passed the no-GPU/no-examples
  configure, affected target builds, direct `[issue-642]` runs for
  events and NSD, full binaries, focused CTest, diff check, skill-sync
  report, and version-bump report.
- `#641` runtime Base64 validation worktree
  `/Users/danielraffel/Code/pulp-runtime-coverage-641-next`, branch
  `feature/runtime-coverage-641-next`, commits `20626508` and metadata
  tip `36aa5c71`; open as PR `#848`. Scope: hardens
  `base64_decode` to reject malformed terminal padding and adds focused
  runtime coverage for malformed / whitespace-padded input. Local
  validation passed the no-GPU/no-examples configure, affected target
  build, direct `[issue-641]` run, full runtime-utils binary, focused
  CTest, diff check, skill-sync report, and version-bump report with an
  SDK patch trailer.
- `#493` view label worktree
  `/Users/danielraffel/Code/pulp-view-widget-coverage-493-next`, branch
  `feature/view-widget-coverage-493-next`, commit `7f6deaf6`; open as
  PR `#849`. Scope: test-only coverage for label text transforms,
  multiline/decorations, and vertical text-direction transform wrapping.
  Local validation passed the no-GPU/no-examples configure, widgets
  target build, full widgets binary, focused selectors, diff check,
  skill-sync report, and version-bump report.
- `#643` runner resolver worktree
  `/Users/danielraffel/Code/pulp-cli-tools-coverage-643-next2`, branch
  `feature/cli-tools-coverage-643-next2`, commit `b8e7b22a`; open as
  PR `#850`. Scope: test-only Python coverage for
  `resolve_runs_on.py` optional Namespace fallback and
  argument/provider validation branches. Local validation passed the
  direct Python test suite, compileall, and diff check; local
  `coverage.py` is not installed in this interpreter.
- `#645` signal meter guard worktree
  `/Users/danielraffel/Code/pulp-signal-meter-coverage-645-next`,
  branch `feature/signal-meter-coverage-645-next`, commits `e53b5152`
  and metadata tip `ad9c3d81`; open as PR `#851`. Scope: hardens
  negative/invalid channel-count handling in `MultiChannelMeter` and
  `MultiChannelBallistics` and adds focused signal meter coverage.
  Local validation passed the no-GPU/no-examples configure, affected
  target build, direct `[signal][meter][issue-645]` run, full signal
  binary, focused CTest, diff check, skill-sync report, and
  version-bump report with an SDK patch trailer.
- `#641` canvas rectangle-list helper worktree
  `/Users/danielraffel/Code/pulp-runtime-helper-coverage-641-next2`,
  branch `feature/runtime-helper-coverage-641-next2`, commits
  `3e1cfd51` and `05c85d55`; open as PR `#852`. Scope: test-only
  coverage for `RectangleList` empty-rectangle filtering, area-overlap
  intersection semantics, subtract no-op/full-cover behavior,
  bounding-box/area helpers, and clipped intersections. Local validation
  passed the no-GPU/no-text-shaping configure, affected target build,
  direct `[issue-641]` run with `25` assertions in `5` cases, full
  binary with `52` assertions in `14` cases, focused CTest `14/14`, and
  diff check. The overlapping `/Users/danielraffel/Code/pulp-render-helper-coverage-646-next`
  branch also touched `test/test_rectangle_list.cpp`; its unique
  subtract case was folded into `#852` and that duplicate branch should
  not be opened as-is.
- `#640` audio frame-fill helper worktree
  `/Users/danielraffel/Code/pulp-audio-helper-coverage-640-next2`,
  branch `feature/audio-helper-coverage-640-next2`, commit `53bc6020`;
  open as PR `#853`. Scope: test-only coverage for
  `zero_fill_short_read` negative channel-count and negative total-frame
  guards. Local validation passed diff check, no-GPU/no-examples
  configure, affected target build, focused CTest `11/11`, and full
  frame-fill binary with `81` assertions in `11` cases.
- `#493` ViewBridge helper worktree
  `/Users/danielraffel/Code/pulp-host-view-helper-coverage-493-next2`,
  branch `feature/host-view-helper-coverage-493-next2`, commit
  `5c9eeeba`; open as PR `#854`. Scope: test-only coverage for
  idempotent `open()`, primary role helpers, repeated `release_view()`,
  secondary view null/bounds cleanup, and null remote-channel diagnostics
  followed by successful open. Local validation passed diff check,
  no-GPU configure, affected target build, and focused ViewBridge CTest
  `10/10`.
- `#645` signal convolver worktree
  `/Users/danielraffel/Code/pulp-midi-signal-helper-coverage-645-next2`,
  branch `feature/midi-signal-helper-coverage-645-next2`, commit
  `2d8b0bc3`; open as PR `#855`. Scope: test-only coverage for
  `PartitionedConvolver` rounded/zero block-size setup, empty IR
  pass-through, and wrong-block-size process pass-through behavior.
  Local validation passed diff check, skill-sync report, version-bump
  report, no-GPU/no-examples configure, affected target build, full
  convolver binary with `91` assertions in `6` cases, direct
  `[issue-645]` run with `19` assertions in `2` cases, and focused CTest
  `6/6`.
- `#643` LCOV/Cobertura helper worktree
  `/Users/danielraffel/Code/pulp-cli-tools-coverage-643-next3`,
  branch `feature/cli-tools-coverage-643-next3`, commit `96d20a3a`;
  open as PR `#856`. Scope: test-only coverage for
  `tools/scripts/test_run_coverage.py` package exclusions before
  summary-rate calculation, duplicate source-record function-hit
  merging, and relpath `ValueError` fallback paths. Local validation
  passed `python3 tools/scripts/test_run_coverage.py`,
  `python3 tools/scripts/test_merge_cobertura.py`,
  `python3 tools/scripts/test_run_python_coverage.py`, and diff check;
  local `coverage.py >= 7.10` is not installed, so hosted CI remains the
  coverage proof.
- `#642` events async-helper worktree
  `/Users/danielraffel/Code/pulp-events-helper-coverage-642-next2`,
  branch `feature/events-helper-coverage-642-next2`, commits `7b6f653e`
  and metadata tip `4cbfd67c`; open as PR `#857`. Scope: new focused
  test-only target for reentrant `AsyncUpdater`, `LambdaAsyncUpdater`
  empty-callback handling, `ActionBroadcaster` missing-removal behavior,
  and `MultiTimer` stop-all/restart behavior. Local validation after
  rebasing onto `origin/main` passed the no-GPU/no-examples configure,
  affected target build, direct `pulp-test-events-async-helpers` run
  with `16` assertions in `4` cases, focused CTest `12/12`, diff check,
  and pre-push gates with a `Version-Bump: sdk=skip` trailer for the
  test-only target registration.

Open supporting PR:

- `#774` refreshes this durable handoff/status document, branch
  `docs/coverage-status-2026-04-25`. The branch is updated as this
  tracker changes; use the PR head SHA in GitHub as the live value.
  The branch has been rebased onto `origin/main` after `#813` merged,
  updated after `#816` merged, `#817` was repaired, `#820` opened,
  `#821` opened, `#822` opened, `#818` merged, `#823` opened, `#817`
  merged, `#822` was repaired, `#824` opened, `#821` merged, and `#819`
  merged, `#825` opened, `#820` merged, `#826` opened, `#822` was
  repaired again for Android SDK discovery, `#827` opened, `#823`
  merged, `#824` merged, `#828` opened, `#829` opened, `#825` merged,
  `#830` opened, `#831` opened, `#827` merged, `#832` opened, `#826`
  merged, and `#832` was repaired for Windows cmd portability, and
  `#831` was repaired for Windows float tolerance, and `#822` merged,
  and `#833` opened, and `#829` was rebased onto `87ef6d90` to pick up
  the merged Android SDK discovery fix, and `#832` was rebased/repaired
  for the Windows fast-exit command, and `#834` opened, and `#828`
  merged, and `#835` opened, and `#836` opened, and `#837` opened, and
  `#831` merged, and `#838` opened, and `#835` was repaired for Linux
  Android-validation ordering plus config isolation, and `#832` was
  repaired for the fast-exit deadline review, and `#833` was rebased
  onto `b5e7a463` to clear the stale Android SDK discovery ASan failure,
  and `#836` merged, and `#837` merged, and `#839` opened,
  and `#830` merged, and `#838` was rebased onto `d02c9ec9` and repaired
  for the Android package parallel temp-dir collision, and `#834` merged,
  and `#840` opened, and `#841` was opened to classify the represented
  but currently uncomponentized `inspect/` Codecov bucket, and `#829`
  merged, and `#840` was rebased onto `eaf991b0`, and `#842` opened to
  promote `inspect/**` to a first-class Codecov component/tier, and
  `#843` opened for MPE tracker UMP edge-path coverage, and `#845`,
  `#846`, `#847`, `#848`, `#849`, `#850`, and `#851` opened from the
  parallel worker batch, and `#833` merged as `8fc5dd2d`, and `#840`
  was repaired for diff coverage with `410a0371`, and `#852`, `#853`,
  `#854`, and `#855` opened from the next worker batch, and `#838`
  merged as `3d47dbd2`, and `#839` merged as `62ad9870`, and `#856`
  and `#857` opened from the latest worker batch,
  and remains docs-only.

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
2. Monitor `#832` cloud checks; if a required lane fails, debug in
   `/Users/danielraffel/Code/pulp-platform-child-process-coverage-640`,
   patch, validate locally, and push with lease.
3. Monitor `#835` cloud checks; if a required lane fails, debug in
   `/Users/danielraffel/Code/pulp-cli-ship-coverage-643`, patch,
   validate locally, and push with lease.
4. Monitor `#840` cloud checks on head `410a0371`; if a required lane
   fails after dependency bootstrap succeeds, debug in
   `/Users/danielraffel/Code/pulp-events-child-process-coverage-642`,
   patch, validate locally, and push with lease.
5. Monitor `#842` cloud checks on head `0541fd69`; if a required lane
   fails, debug in
   `/Users/danielraffel/Code/pulp-inspect-codecov-component-841`,
   patch, validate locally, and push with lease.
6. Monitor `#843` cloud checks on head `fcdbd278`; if a required lane
   fails, debug in
   `/Users/danielraffel/Code/pulp-midi-mpe-tracker-extra-coverage-645-next`,
   patch, validate locally, and push with lease.
7. Monitor `#845` through `#857`; if a required lane fails, debug in
   the worktree named in the open-PR list above, patch, validate
   locally, and push with lease. Pay particular attention to `#848` and
   `#851` because they include patch-level production hardening.
8. If any open Phase 3 PR is green but GitHub reports it behind `main`,
   rebase that branch onto `origin/main`, push with lease, and let
   checks rerun.
9. Continue Phase 3 from the tranche issues below, prioritizing
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

## Represented inspect surface

- `inspect/` is represented on the Codecov website as a first-party
  top-level bucket (`1,505` lines, `10.10%` in the user-observed
  snapshot). It is tracked by `#841`, and PR `#842` promotes
  `inspect/**` to a first-class Codecov flag/component and assigns it to
  the existing user-facing coverage tier in `ci/coverage-targets.yaml`.

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
- `#841` classify represented `inspect/**` as a first-class Codecov
  component/tier; implemented by open PR `#842`

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
- `#841` inspect component / tier classification

### Deferred perimeter follow-ups

- `#659` authored JavaScript source lane
- `#657` Node bindings plus shell / PowerShell classification, closed as
  explicitly out of scope for now
- `#77` mobile runtime / iOS simulator coverage
- `#841` represented `inspect/**` classification

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
