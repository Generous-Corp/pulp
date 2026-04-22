# Coverage Compliance Status

Last reviewed: 2026-04-22

This is the durable tracker for the repo-wide coverage compliance push.
It records the live Codecov baseline, the tranche plan, the issue trail,
and the control-plane invariants that keep the dashboard trustworthy
across sessions.

## Goal

Reach compliance against the current coverage expectations in
`ci/coverage-targets.yaml` as quickly as possible without weakening the
gates:

- `80%`: `audio`, `format`, `host`, `midi`, `signal`, `platform`
- `70%`: `render`, `view`, `cli`
- `50%`: `events`, `runtime`, `state`, `canvas`, `osc`, `ship`, `tools`

## Live baseline

Source of truth: Codecov API on `main` at commit `bb4a1bc62a636876c1b54b7b74cead335699b96e`
after the Swift and Android lane merges.

- Overall tracked coverage: `56.63%` over `57,294` lines
- Current blocking drift on the dashboard:
  - `windows` component is `null` because the live repo uses `win/`
    paths that were not yet included in `codecov.yml`
  - `dsl` exists under `core/dsl/` but was not yet represented in the
    live subsystem slicing on `main`

## Below-target components on `main`

| Component | Coverage | Target | Tracking |
| --- | ---: | ---: | --- |
| `audio` | `39.12%` | `80%` | #640 |
| `platform` | `53.87%` | `80%` | #640 |
| `host` | `55.45%` | `80%` | #493 |
| `format` | `58.37%` | `80%` | #493 |
| `midi` | `63.82%` | `80%` | #645 |
| `signal` | `74.77%` | `80%` | #645 |
| `render` | `59.87%` | `70%` | #646 |
| `view` | `61.27%` | `70%` | #493 |
| `cli` | `45.07%` | `70%` | #643 |
| `tools` | `45.05%` | `50%` | #643 |
| `events` | `46.65%` | `50%` | #642 |
| `ship` | `47.63%` | `50%` | #644 |

## Issue map

- `#641` umbrella: repo-wide compliance program and tranche sequencing
- `#639` control plane: Codecov path slicing, dashboard drift, status doc
- `#640` audio/platform tranche
- `#642` events tranche
- `#643` CLI/tools tranche
- `#644` ship tranche
- `#645` midi/signal tranche
- `#646` render tranche
- `#493` existing high-leverage host/format/view coverage work

## In progress on `feature/coverage-compliance`

- Add the missing `dsl` subsystem slice to `codecov.yml`
- Fix the `windows` platform slice to match the live `core/**/win/**`
  path convention
- Tighten `tools/scripts/test_codecov_config.py` so new `core/*`
  subsystems and platform-path regressions fail locally
- Publish this status document and index it in docs
- Add low-risk coverage tests in the current tranche:
  - `test/test_state_tree.cpp`
  - `test/test_appcast.cpp`
  - `test/test_nsis_installer.cpp`
  - `test/test_codesign.cpp`

## Next tranches

1. Land the current control-plane + `state`/`ship` tranche.
2. Drive the queued area issues in this order unless CI or review changes
   the risk picture:
   - `#642` events
   - `#643` CLI/tools
   - `#640` audio/platform
   - `#645` midi/signal
   - `#646` render
   - existing `#493` host/format/view

## Control-plane invariants

- Every top-level `core/*` directory must have matching subsystem entries in
  `codecov.yml` `flags:` and `component_management:`.
- Platform axes must match the live repo path conventions, including
  `core/**/win/**` for Windows.
- Upload-only flags stay `os-linux`, `os-macos`, and `os-windows`.
- `codecov.yml` `ignore:` must stay aligned with
  `scripts/run_coverage.sh` `COVERAGE_IGNORE_REGEX`.
- Keep the control-plane validation narrow for this slice:
  `python3 tools/scripts/test_codecov_config.py`
  and `tools/check-docs.sh`.

## Follow-ups not solved by this doc

- If another top-level `core/*` subsystem is added, update `codecov.yml`
  in the same change; the structural test should fail immediately if that
  mapping is missed.
- `docs/reference/modules.md` still lacks a `#dsl` section even though
  `docs/status/modules.yaml` points at one. That is real docs drift, but it
  is outside this coverage-compliance slice.
