# Coverage Compliance Status

Last reviewed: 2026-04-26 00:57 EDT

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
- `dsl` is a real component on `main` (`63.38%`).
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

- commit: `c4e6ad0930a7871046225b84fcfb130263ff98aa`
- workflow: Coverage run `24946798292`, completed successfully
- overall tracked coverage: `44.77%` over `70,284` lines in `556` files
- covered lines: `31,470`
- missed lines: `37,442`
- partial lines: `1,372`
- current component coverage from the Codecov API:
  - `audio`: `26.86%`
  - `canvas`: `65.12%`
  - `dsl`: `78.68%`
  - `events`: `50.64%`
  - `format`: `51.64%`
  - `host`: `46.6%`
  - `midi`: `49.12%`
  - `osc`: `68.88%`
  - `platform`: `39.25%`
  - `render`: `61.03%`
  - `runtime`: `52.21%`
  - `signal`: `66.89%`
  - `state`: `65.21%`
  - `view`: `44.38%`
  - `android`: `13.83%`
  - `apple`: `25.36%`
  - `linux`: `7.96%`
  - `windows`: `0.63%`
  - `cli`: `33.15%`
  - `ship`: `53.77%`
  - `tools`: `39.98%`

Post-`#794` file-level proof point: `core/audio/src/aiff_reader.cpp`
is now `72.01%` covered with `61` misses, up from `64.22%` with `78`
misses at the prior `e2af9c4d` baseline.

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

Open Phase 3 PRs:

- `#795` `test(events): cover volume detector lifecycle edges`, branch
  `feature/events-volume-coverage-642`, commit `2d684f4b`, worktree
  `/Users/danielraffel/Code/pulp-events-volume-coverage-642`.
  Scope: deterministic `test/test_network_service_discovery.cpp`
  coverage for `NetworkServiceDiscovery` backend removal, cached-service
  eviction, no-backend registration behavior,
  `MountedVolumeListChangeDetector` mounted-volume snapshot plus
  start/restart/stop lifecycle paths, and
  `LockingAsyncUpdater::trigger_and_wait()`. Local validation:
  after rebasing onto current `origin/main`,
  `pulp-test-network-service-discovery` passed with `43` assertions in
  `10` test cases; focused CTest `NSD|MountedVolume|LockingAsyncUpdater`
  passed `10/10`; `git diff --check` clean; skill sync found no mapped
  paths; version bump report says no bump needed. The earlier
  `core/events/src/volume_detector.cpp` Windows guard was removed from the
  final PR diff because the Windows coverage XML reports that translation
  unit at `0%` even though the CTest cases run and pass there. The final
  PR diff is test-only, and the mounted-volume enumeration tests skip on
  Windows while retaining POSIX coverage. GitHub checks were rerunning on
  `2d684f4b` with no failures at the last poll. The PR is labeled
  `codecov` and is using the direct GitHub/Namespace path instead of
  `shipyard pr` because `#794` exposed a local Shipyard mac configure
  stall after GitHub/Namespace was already clean.
- `#798` `test(audio): cover format registry dispatch paths`, branch
  `feature/audio-format-registry-coverage-640`, commit `0b35bb83`,
  worktree
  `/Users/danielraffel/Code/pulp-audio-system-volume-coverage-640`.
  Scope: deterministic `FormatRegistry` unsupported read/read_info
  dispatch, custom reader/writer registration, normalized extension lookup,
  read_info/read/write dispatch through registered handlers, and supported
  extension collection for `.aifc`. Local validation:
  after rebasing across `#797`, `pulp-test-audio-file` passed with `339`
  assertions in `20` test cases; focused CTest
  `FormatRegistry|audio.*file|StreamingWriter` passed `7/7`;
  `git diff --check` clean; skill sync found no mapped paths; version bump
  report says no bump needed.
  The PR is labeled `codecov` and is using the same direct
  GitHub/Namespace path as `#795`.
- `#799` `test(cli): cover docs shellout reader paths`, branch
  `feature/cli-docs-coverage-643`, commit `4877cb1b`, worktree
  `/Users/danielraffel/Code/pulp-cli-docs-coverage-643`.
  Scope: deterministic `test/test_cli_shellout.cpp` coverage for
  `pulp docs` local reader usage, index, open, search, support/command/
  CMake/style `show` paths, and error diagnostics. Local validation:
  `pulp-cli` and `pulp-test-cli-shellout` built successfully; direct
  Catch2 tag run `[cli][shellout][docs][issue-643]` passed with `38`
  assertions in `1` test case; focused CTest
  `pulp docs covers local reader` passed `1/1`; `git diff --check`
  clean; skill sync found no mapped paths; version bump report says no
  bump needed. The PR is labeled `codecov` and is using the direct
  GitHub/Namespace path.
- `#800` `test(tools): cover version and skill gate helpers`, branch
  `feature/python-tooling-coverage-643`, commit `acadc5af`, worktree
  `/Users/danielraffel/Code/pulp-python-tooling-coverage-643`.
  Scope: deterministic `tools/scripts/test_gates.py` coverage for
  `version_bump_check.py` version-file readers/writers, semver
  arithmetic, conventional-commit classification, and `Version-Bump`
  trailer parsing; plus `skill_sync_check.py` trailer parsing,
  generated-file filtering, path-map self-check failures, nested
  `SKILL.md` recognition, and bypass propagation. Local validation:
  `python3 tools/scripts/test_gates.py` passed with `22` tests;
  `git diff --check` clean; skill sync found no mapped paths; version
  bump report says no bump needed. Local `run_python_coverage.py` was not
  run because this local Python lacks `coverage.py`; the CI coverage
  workflow installs `coverage>=7.10` before running the Python tooling
  coverage lane. The PR is labeled `codecov` and is using the direct
  GitHub/Namespace path.

Local Phase 3 draft worktrees:

- `#640` AIFF reader edge-path worktree
  `/Users/danielraffel/Code/pulp-audio-aiff-coverage-640`, branch
  `feature/audio-aiff-coverage-640`; merged via PR `#794` as
  `c4e6ad09`. The remote branch was deleted after merge.
- `#642` events volume/service-discovery worktree
  `/Users/danielraffel/Code/pulp-events-volume-coverage-642`, branch
  `feature/events-volume-coverage-642`, commit `ab941cd9`; open as PR
  `#795`.
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
  `0b35bb83`; open as PR `#798`.
- `#643` docs-reader CLI/tools worktree
  `/Users/danielraffel/Code/pulp-cli-docs-coverage-643`, branch
  `feature/cli-docs-coverage-643`, commit `4877cb1b`; open as PR
  `#799`.
- `#643` Python gate-helper tooling worktree
  `/Users/danielraffel/Code/pulp-python-tooling-coverage-643`, branch
  `feature/python-tooling-coverage-643`, commit `acadc5af`; open as PR
  `#800`.

Open supporting PR:

- `#774` refreshes this durable handoff/status document, branch
  `docs/coverage-status-2026-04-25`. The branch is updated as this
  tracker changes; use the PR head SHA in GitHub as the live value.
  The branch has been rebased onto `origin/main` after `#796` merged and
  remains docs-only.

Local environment note:

- Pulp's Shipyard source pin is `v0.46.0` in `tools/shipyard.toml`
  and the release workflows. On 2026-04-25, PATH had drifted to
  `shipyard, version 0.50.0`; rerunning `./tools/install-shipyard.sh`
  restored the pinned binary, and `shipyard --version` prints
  `shipyard, version 0.46.0`.

Next recovery actions:

1. Keep `#774` docs-only and let its latest status-update checks drain.
2. Monitor `#795`, `#798`, `#799`, and `#800` and address any Codecov,
   build, sanitizer, or Namespace feedback.
3. If `#795`, `#798`, `#799`, or `#800` is green but GitHub reports it
   behind `main`, rebase that branch onto `origin/main`, push with lease,
   and let checks rerun.
4. Continue Phase 3 from the tranche issues below, prioritizing
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
