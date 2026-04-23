# Coverage Compliance Status

Last reviewed: 2026-04-23

This is the durable tracker for the repo-wide coverage compliance
program under `#641`. The repo is still in the "get the intended source
surface onto Codecov" phase; ordinary test-gap ranking and closure stay
parked until that represented surface is stable on `main`.

## Goal

Get Codecov as close as practical to the intended first-party local
source surface, then use that corrected baseline to plan and close the
real measured test gaps.

Target tiers remain unchanged in `ci/coverage-targets.yaml`:

- `80%`: `audio`, `format`, `host`, `midi`, `signal`, `platform`
- `70%`: `render`, `view`, `cli`
- `50%`: `events`, `runtime`, `state`, `canvas`, `osc`, `ship`, `tools`

## Three-phase program

### Phase 1 — Representation / Codecov truth

Finish line:

- `windows` is not `null` on `main`
- `dsl` is a real component on `main`
- `apple/Sources/**` materializes on Codecov
- `android/app/src/main/kotlin/**` materializes on Codecov
- the clearly known remaining "still not on Codecov" bucket is
  explicitly listed and tracked
- this status document is rebaselined from the corrected `main` surface

### Phase 2 — Gap planning

Finish line:

- counted components are ranked from the corrected `main` baseline
- major low-coverage files and components are grouped into tranche issues
- remaining out-of-scope surfaces are either accepted for now or have
  explicit expansion issues
- the repo has an actionable issue / PR roadmap for closing measured
  gaps

### Phase 3 — Gap closure

Finish line:

- planned coverage tranches are implemented and merged
- target components move materially toward or to their configured
  thresholds
- deferred or exceptional surfaces stay documented explicitly instead of
  silently omitted

## Current state

- `#647` merged to `main` at `773c2be332995483248fe82d497b832080c320df`
  on 2026-04-23.
- GitHub Actions for `#647` were green across the required hosted build
  and coverage checks.
- Shipyard validation for the same SHA passed on `mac` and `ubuntu`.
  The `windows` Shipyard target failed before validation started because
  `C:\\Users\\danielraffel\\shipyard.bundle` was present but locked on the
  remote host. That is tracked separately in `#671` as CI infra, not as
  a verdict on `#647`.
- Public Codecov `main` has now moved to `773c2be3`.
  Current observed `main` snapshot:
  - overall tracked coverage: `56.77%` over `57,503` lines
  - `dsl`: `86.88%`
  - `windows`: `20.54%`
  - `android`: `14.17%`
  - `linux`: `25.58%`
  - `apple`: `33.41%`
- `android/app/src/main/kotlin/**` is now visible on Codecov `main`.
- `windows` is no longer `null` on `main`.
- `dsl` is now a real component on `main`.
- `apple/Sources/PulpSwift/**` is still not visible on Codecov `main`
  even though the merged PR branch did materialize
  `PulpBridge.swift`, `PulpParameter.swift`, and `PulpViews.swift`.
  Phase 1 therefore remains open on Apple ingestion fidelity.

## Clearly known still not on Codecov

Raw LOC counts here are approximate and are only used to size the
remaining Phase 1 work. They are not exact Codecov denominators.

- broader authored JavaScript: about `23.9k` raw lines — `#659`
- broader Python outside the current tooling lane: about `5.2k` raw
  lines — `#658`
- shell and PowerShell scripts: about `2.5k` raw lines — `#657`
- optional bindings under `bindings/**`: about `0.6k` raw lines — `#657`
- Swift / Apple-adjacent roots still outside the surfaced package
  subset:
  - `apple/Sources/PulpSwift/PulpAudioSession.swift`
  - `apple/Sources/PulpSwift/PulpBridge.cpp`
  - `apple/Sources/PulpSwift/PulpBridge.h`
  - `tools/local-ci/macos_window_probe.swift`
  tracked by `#656`
- mobile runtime coverage outside the current source-only lanes:
  - Android emulator / device instrumentation coverage
  - iOS simulator / runtime app coverage where we decide it belongs in
    the represented surface
  tracked by `#77`

The rough remainder beyond this list is mostly metric mismatch between
raw local LOC and Codecov executable-line accounting, not a clean
backlog bucket.

## Active Phase 1 issue map

### Control plane and verification

- `#641` authoritative umbrella and phase tracker
- `#639` Codecov control plane and dashboard truth follow-up
- `#647` merged representation tranche
- `#671` Windows Shipyard bundle-lock infra failure during post-merge
  validation

### Perimeter-expansion work

- `#656` expand Swift / Apple perimeter beyond the current package lane
- `#658` expand Python perimeter beyond the current tooling lane
- `#659` add a JavaScript source lane for authored repo assets
- `#657` classify optional bindings and shell / PowerShell surfaces in
  the true-source baseline
- `#77` decide and, if in-scope, add mobile runtime coverage from
  Android instrumentation and iOS simulator / runtime app paths

### Supporting infrastructure

- `#655` provision Android SDK / NDK on `ssh ubuntu` and `ssh windows`
  validation hosts. This is useful supporting infra but not a blocker
  for hosted Codecov truth on `main`.
- `#568` historical multi-language expansion umbrella. Active execution
  now rolls up through `#641`.
- `#632` first Python widening to `tools/deps/**` and
  `tools/local-ci/**`

## Parked until Phase 1 completes

Do not start ordinary tranche ranking or closure work from the stale
pre-`#647` main snapshot.

### Phase 2 / Phase 3 issues

- `#640` audio / platform tranche
- `#642` events tranche
- `#643` CLI / tools tranche
- `#644` ship tranche
- `#645` midi / signal tranche
- `#646` render tranche
- `#493` host / format / view and related hardening gaps

### Parked draft PRs

- `#648` draft events tranche
- `#649` draft CLI / tools tranche
- `#666` draft state / ship hardening split from `#647`

## Rebaseline checklist after `#647`

1. Keep the corrected `main` snapshot above as the current baseline for
   everything that already landed.
2. Investigate why `apple/Sources/PulpSwift/**` materialized on the PR
   branch but not on merged `main`.
3. Do not start Phase 2 ranking until Apple package sources are either
   visible on `main` or explicitly accepted as still out of scope.
4. Once Apple is resolved, refresh the final corrected `main` baseline
   and only then begin measured gap planning.

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
