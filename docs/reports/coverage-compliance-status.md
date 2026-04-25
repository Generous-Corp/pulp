# Coverage Compliance Status

Last reviewed: 2026-04-25 18:28 EDT

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

- commit: `26dd396da53e5ceab340eac5681f1b10d75ed56b`
- workflow: Coverage run `24930675870`, completed successfully
- overall tracked coverage: `44.35%` over `70,205` lines in `556` files
- covered lines: `31,140`
- current component coverage from the Codecov API:
  - `audio`: `24.8%`
  - `canvas`: `67.83%`
  - `dsl`: `77.04%`
  - `events`: `29.47%`
  - `format`: `52.61%`
  - `host`: `43.23%`
  - `midi`: `50.96%`
  - `osc`: `72.52%`
  - `platform`: `46.92%`
  - `render`: `61.35%`
  - `runtime`: `48.56%`
  - `signal`: `67.25%`
  - `state`: `67.26%`
  - `view`: `45.04%`
  - `android`: `13.83%`
  - `apple`: `25.72%`
  - `linux`: `0.0%`
  - `windows`: `16.83%`
  - `cli`: `32.96%`
  - `ship`: `36.48%`
  - `tools`: `39.89%`

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

Open Phase 3 PRs:

- `#788` events socket-IPC coverage, branch
  `feature/events-ipc-coverage-642`, head `4cb33f4e`. This tranche
  extends `test/test_ipc.cpp` with malformed socket endpoint rejection,
  non-throwing socket endpoint parsing in
  `core/events/src/interprocess_connection.cpp`, and a deterministic
  localhost client/server framed-message exchange. After `#771` merged, this
  branch was rebased onto `origin/main` and duplicate shared CLI support
  commits were skipped, leaving only the events tranche. Local validation is
  green: `pulp-test-ipc` (`34` assertions / `9` test
  cases), focused CTest `IPC` (`9/9`), and
  whitespace. A direct multi-filter Catch2 invocation was intentionally not
  used as validation because Catch2 treats multiple quoted filters as an AND
  expression; CTest was used for the focused multi-test slice. PR is labeled
  `codecov`. The earlier Linux Namespace and Linux coverage failures did not
  expose usable logs through `gh`. A rerun of Build/Test `24929736674` and
  Coverage `24929736678` then sat in progress for roughly 90 minutes with no
  logs and no timestamp movement, so both runs were canceled and rerun from
  the same head `4cb33f4e`. GitHub reran the full Build/Test and Coverage
  workflows, not only the Linux jobs; the current polling window shows no
  failures while those fresh checks drain.
- `#789` Android ship/package helper coverage, branch
  `feature/android-package-coverage-644`, head `a3f124a1`. This tranche
  adds a deterministic host-side fake Android SDK/toolchain harness for
  `ship/platform/android/package_android.cpp` without requiring a real SDK,
  device, keystore, Gradle install, bundletool, or network access. Scope:
  SDK/build-tools/NDK discovery, fake `zipalign`, fake `apksigner` signing and
  APK verification parsing, fake `jarsigner` AAB signing/verification, fake
  Gradle APK/AAB artifact collection, missing-wrapper/missing-artifact errors,
  and fake bundletool conversion with optional signing config. After `#786`
  merged, this branch was rebased onto `origin/main`, pushed, opened as a PR,
  and labeled `codecov`. The first GitHub Actions pass exposed a Windows
  Namespace-only failure in batch-backed Android tool invocation. The first
  follow-up fixed the skill-sync gap but still failed the same three Windows
  Android tests. The second follow-up preserved quoted batch executable paths
  for `cmd.exe /c` but still failed the same Windows tests. The current head
  additionally fixed the fake Windows `apksigner.bat` fixture's unescaped
  parenthesized echo text and quotes whole Gradle / bundletool `name=value`
  arguments instead of only quoted values; that narrowed the Windows Namespace
  failure to only the fake Gradle artifact-collection case. The current head
  runs Gradle through `ChildProcess::run` with `ProcessOptions::working_directory`
  set to the Gradle project directory instead of relying on inline
  `cd ... && gradlew` shell parsing; the `ship` skill documents these Windows
  batch gotchas. Local validation is green:
  `cmake --build build --target pulp-test-android-package -j4`, direct
  `pulp-test-android-package` (`64` assertions / `7` test cases), focused
  Android package CTest (`7/7`), adjacent ship CTest slice for
  Android/appcast/codesign/notarization/DMG (`20/20`), and whitespace. The
  current polling window shows no failures while fresh checks run on
  `a3f124a1`.

Local Phase 3 draft not yet opened as a PR:
- `#643` package-registry CLI/tools draft, worktree
  `/Users/danielraffel/Code/pulp-package-registry-coverage-643`, branch
  `feature/package-registry-coverage-643`, local commit `b1443bb9`. This draft is local-only and
  should not be opened until `#788` / `#789` resolve. Current scope adds
  `pulp-test-cli-package-registry` for pure local registry parsing,
  lock-file round trips, target TOML parsing/writing, semver, quality
  scoring, unsupported-target detection, and search ranking. Local validation
  is green in a no-GPU/no-examples build directory using the completed local
  `mbedtls` source tree: configure
  `cmake -S . -B build-package-registry-localdeps -DCMAKE_BUILD_TYPE=Debug
  -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF
  -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=$PWD/build/_deps/mbedtls-src`, build
  `cmake --build build-package-registry-localdeps --target
  pulp-test-cli-package-registry -j4`, direct binary (`88` assertions / `5`
  test cases), focused CTest (`5/5`), and whitespace.

Open supporting PR:

- `#774` refreshes this durable handoff/status document, branch
  `docs/coverage-status-2026-04-25`. The branch is updated as this
  tracker changes; use the PR head SHA in GitHub as the live value.
  The branch has been rebased onto `origin/main` after `#786` merged and
  remains docs-only.

Next recovery actions:

1. Poll `#788` and `#789`; merge manually when green because auto-merge is
   disabled.
2. Let `#788`'s fresh full Build/Test and Coverage workflow reruns drain.
3. Let `#789`'s post-Windows-batch-fix GitHub Actions checks drain; pull the
   exact failing job log first if anything turns red.
4. Keep `#774` docs-only and let its post-`#786` rebase checks drain.
5. After the active code PRs merge, refresh this section with the next
   complete Codecov `main` report.
6. If `#789` Windows Namespace fails again, pull the fresh completed-run log
   first and search for `test_android_package.cpp`, `Android signing helpers`,
   `Android Gradle packaging`, and `Android bundletool`; do not patch further
   without the new log.
7. If `#788` Linux Namespace or Linux coverage fails again, pull the fresh log
   first; the current branch already has local IPC validation green.
8. If a PR is green but GitHub reports it behind `main`, rebase that
   branch onto `origin/main`, push with lease, and let checks rerun.
9. After `#788` and `#789` resolve, push/open the local `#643` draft
   (`b1443bb9`) as the next CLI/tools tranche PR.
10. Continue Phase 3 from the tranche issues below, prioritizing
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
