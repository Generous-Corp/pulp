# Coverage Compliance Status

Last reviewed: 2026-04-22

This is the durable tracker for the repo-wide coverage compliance push.
It records the live Codecov baseline, the represented local-source
perimeter, the issue trail, and the control-plane invariants that keep
the dashboard trustworthy across sessions.

## Goal

Make Codecov trustworthy for the declared local-source perimeter first,
then use that corrected baseline to drive the ordinary coverage-lift
tranches in `ci/coverage-targets.yaml` without weakening the gates:

- `80%`: `audio`, `format`, `host`, `midi`, `signal`, `platform`
- `70%`: `render`, `view`, `cli`
- `50%`: `events`, `runtime`, `state`, `canvas`, `osc`, `ship`, `tools`

## Representation baseline

Source of truth: public Codecov API on `main` at commit
`5291810fb57ec89fae8595d3c0d10b4cc7c2a62a` on 2026-04-22.

- Overall tracked coverage on the current head: `56.74%` over `56,822`
  lines.
- Current blocking truth gaps on `main`:
  - `windows` is `null` because `main` still lacks the
    `core/**/win/**` mapping now queued on this branch.
  - `dsl` exists under `core/dsl/` but is still absent from the live
    component taxonomy on `main`.
  - The Swift lane uploads successfully, but the current LLVM JSON
    artifact is not materializing `apple/Sources/**` files into Codecov
    repo totals.
  - The Android JaCoCo lane works only on commits that trigger
    `android.yml`; unrelated `main` pushes currently fall back to
    `android = null` at the branch head.

## Declared local-source perimeter after this branch

The intended represented surface after this control-plane tranche lands:

- native Clang coverage from the first-party C/C++ build graph on
  macOS, Linux, and Windows
- Python under `tools/scripts/**`, `tools/deps/**`, and
  `tools/local-ci/**`
- Swift package sources under `apple/Sources/**`
- Android JVM-unit-testable Kotlin under
  `android/app/src/main/kotlin/**`
- subsystem/platform/surface slicing from `codecov.yml`, including
  `dsl` and the live `win/` path convention

Still explicitly outside the represented perimeter after this branch:

- iOS-only Swift that does not compile in the macOS SwiftPM lane yet
  (for example `apple/Sources/PulpSwift/PulpAudioSession.swift`)
- Python outside the current Linux tooling lane, including top-level
  `tools/*.py`, `tools/packages/**`, and `core/view/js/embed_js.py`
- Swift outside the `apple/` package lane, including
  `tools/local-ci/macos_window_probe.swift`
- authored JavaScript assets
- Android emulator/device instrumentation coverage
- optional bindings under `bindings/python/**` and `bindings/nodejs/**`
- shell and PowerShell scripts

## Coverage baseline after representation

These numbers stay useful, but only after the representation checklist
above is green.

## Below-target components on `main`

| Component | Coverage | Target | Tracking |
| --- | ---: | ---: | --- |
| `audio` | `39.08%` | `80%` | #640 |
| `platform` | `56.39%` | `80%` | #640 |
| `host` | `55.35%` | `80%` | #493 |
| `format` | `58.03%` | `80%` | #493 |
| `midi` | `63.24%` | `80%` | #645 |
| `signal` | `75.53%` | `80%` | #645 |
| `render` | `59.71%` | `70%` | #646 |
| `view` | `60.92%` | `70%` | #493 |
| `cli` | `44.43%` | `70%` | #643 |
| `tools` | `44.50%` | `50%` | #643 |
| `events` | `46.65%` | `50%` | #642 |
| `ship` | `47.74%` | `50%` | #644 |

## Issue map

- `#641` umbrella: repo-wide program and tranche sequencing
- `#639` control plane: path slicing, language-lane ingestion, status doc
- `#640` audio/platform tranche
- `#642` events tranche
- `#643` CLI/tools tranche
- `#644` ship tranche
- `#645` midi/signal tranche
- `#646` render tranche
- `#493` existing high-leverage host/format/view coverage work

## Open PR status

- `#647` `feature/coverage-compliance`: keep and finish first. This is
  the control-plane branch and now owns the representation fixes for
  `dsl`, `windows`, Swift ingestion, and always-on Android Kotlin
  coverage on the main Coverage workflow.
- `#648` `feature/events-coverage-642`: keep open as a parked draft.
  It is a real low-risk `events` tranche, but it should land only after
  Codecov truth is fixed.
- `#649` `feature/cli-coverage-643`: keep open as a parked draft. It is
  a real `CLI/tools` tranche, but it is no longer on the critical path
  ahead of representation fidelity.

## In progress on `feature/coverage-compliance`

Representation work:

- add the missing `dsl` subsystem slice to `codecov.yml`
- fix the `windows` platform slice to match the live `core/**/win/**`
  path convention
- tighten `tools/scripts/test_codecov_config.py` so new `core/*`
  subsystems and platform-path regressions fail locally
- export Swift coverage as repo-relative LCOV so `apple/Sources/**`
  materializes on Codecov
- move the Android Kotlin upload onto the always-on Coverage workflow
  so `main` does not lose Android files on unrelated commits
- publish this status document and index it in docs

Coverage-lift work that remains bundled here after representation:

- keep the low-risk coverage tests in the current tranche:
  - `test/test_state_tree.cpp`
  - `test/test_appcast.cpp`
  - `test/test_nsis_installer.cpp`
  - `test/test_codesign.cpp`

## Next sequence

1. Land the current control-plane tranche and verify that `main` shows:
   - non-null `windows`
   - real `dsl` coverage
   - `apple/Sources/**` files in Codecov totals
   - non-null Android coverage on the branch head
2. Rebaseline this status doc from that corrected surface.
3. Drive the queued area issues in this order unless CI or review changes
   the risk picture:
   - `#642` events
   - `#643` CLI/tools
   - `#640` audio/platform
   - `#645` midi/signal
   - `#646` render
   - existing `#493` host/format/view

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
- Keep the control-plane validation narrow for this slice:
  `python3 tools/scripts/test_codecov_config.py`
  `python3 tools/scripts/test_run_swift_coverage.py`
  `python3 tools/scripts/test_coverage_diff_comment.py`
  and `tools/check-docs.sh`.

## Follow-ups not solved by this doc

- If another top-level `core/*` subsystem is added, update `codecov.yml`
  in the same change; the structural test should fail immediately if that
  mapping is missed.
- The next perimeter expansions after this branch are broader Python,
  iOS-only Swift, JS assets, Android instrumentation, optional
  bindings, and shell/PowerShell coverage.
- `docs/reference/modules.md` still lacks a `#dsl` section even though
  `docs/status/modules.yaml` points at one. That is real docs drift, but
  it is outside this coverage-compliance slice.
