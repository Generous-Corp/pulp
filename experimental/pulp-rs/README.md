# pulp-rs (experimental)

Rust prototype built to evaluate whether the Pulp CLI (`tools/cli/*.cpp`)
should be rewritten in Rust. **Not shipping. Not user-facing. Not
wired into any Pulp build.**

See GitHub issue [#686](https://github.com/danielraffel/pulp/issues/686)
for the full evaluation framework and decision criteria.

## Build and run

```bash
cd experimental/pulp-rs
cargo build --release
cargo run -- doctor --versions --json
```

## Test

```bash
cd experimental/pulp-rs
cargo test                                    # unit + integration + parity tests
cargo test --test parity_test                 # parity only (vs captured expected.json)

# Live parity against a built C++ CLI (optional):
PULP_CLI_PATH=/Users/you/Code/pulp/build/tools/cli/pulp \
  cargo test --test parity_test
```

## Phase 2 — what's ported

The `doctor --versions --json` command is now functional and parity-
tested against the C++ implementation:

| Behavior                                                | Status |
|---------------------------------------------------------|--------|
| Read `CMakeLists.txt` `project(... VERSION X.Y.Z)`      | Yes    |
| Read `pulp.toml` `sdk_version` / `cli_min_version`      | Yes    |
| Read `.claude-plugin/plugin.json` version + min_cli     | Yes    |
| Walk `~/.pulp/projects.json` registered projects        | Yes    |
| Compose `findings[]` (rules 1, 1b, 2a, 2b, 3)           | Yes    |
| JSON shape byte-compatible with C++ `render_report_json`| Yes    |
| Human-readable output (`--versions` without `--json`)   | Stub   |
| `--scan-parents` ancestor walk                          | No     |

Rules 1 (`cli_min_version` check added in PR #691) and 1b (plugin
`min_cli_version`, PR #551) are both implemented. The parity test
compares against captured `expected.json` fixtures sourced from the
live C++ `pulp` binary.

## Fixtures

Five fixture projects under `tests/fixtures/` drive the parity test:

- `ok_plain/` — stock standalone project, triggers the Info finding
- `sdk_ahead/` — project SDK newer than CLI → rule 2a warn
- `cli_min_ahead/` — cli_min_version ahead → rule 1 warn + rule 2a warn
- `plugin_newer/` — plugin.json `min_cli_version` ahead → rule 1b
- `registered_projects/` — stale `~/.pulp/projects.json` entries

Each has an `expected.json` captured from the live C++ CLI at
`CLI=0.37.0`. The parity test pins `PULP_RS_CLI_VERSION=0.37.0` so
both lanes render on the same CLI reference.

## Evaluation snapshot (criterion-by-criterion)

| # | Criterion                  | Phase 2 reading |
|---|----------------------------|-----------------|
| 1 | Parity                     | Matches C++ byte-for-byte on 5/5 fixtures for `--versions --json`. Other `doctor` flags (`android`, `ios`, `--fix`, `--scan-parents`) not ported yet. |
| 2 | Lower complexity           | ~600 production LOC in Rust vs ~940 LOC in C++ (`version_diag.cpp` + `projects_registry.cpp` + the doctor slice of `cmd_doctor.cpp` + helpers in `cli_common.cpp`). **Rust is roughly 35% shorter**, plus you get enum-exhaustive match, Result-based error propagation, and a real TOML parser for free. |
| 3 | Better test ergonomics     | Three test lanes (`cargo test`) run unit + smoke + parity in under 1s cold, 50ms warm. Fixture-driven JSON diffing against captured C++ output was trivial to set up; the equivalent in the C++ Catch2 harness involves CMake test-binary wiring, shellout helpers, and a build for any fixture change. **Clear win**. |
| 4 | No production coupling     | Crate is under `experimental/pulp-rs/` only. Nothing else in the repo references it. Runs entirely via cargo. CMake never discovers it. No FFI, no vendored C++ deps — just stdlib + serde + regex + toml + clap. |
| 5 | Cross-platform CI          | Workflow added at `.github/workflows/pulp-rs-experiment.yml` with macos-latest / ubuntu-latest / windows-latest matrix. Gated to `branches: [explore/rust-cli-prototype]` so it only runs on this branch; never merges to main. Locally green on macOS. |
| 6 | Clear migration boundary   | Not yet demonstrated — Phase 2 ports ONE command. The boundary question is about landing a whole cutover, which Phases 3–N would need to explore. At today's scope, the boundary is "everything except `doctor --versions --json` still runs in C++." |

### What the numbers say so far

- **Parity**: works on the ported surface. Confidence high that the
  rest of `doctor` would port cleanly — the hardest bits (path
  normalisation, TOML line-based reading, regex for CMakeLists, JSON
  shape) are already solved.
- **LOC**: 600 prod / 281 tests / 978 including inline `#[cfg(test)]`.
  Slightly under the 800-line prod budget, slightly over if you
  count inline tests as "Rust code" — but those inline tests replace
  some of the Catch2 test surface that lives in the C++ side's
  `test/test_cli_version_diag.cpp`, so the comparison is roughly apples-
  to-apples.
- **Build time**: 15s clean cargo build, <2s incremental. C++ CMake
  configure + build of `pulp` alone is 60s+ clean.
- **Cross-platform**: not yet proven in CI. The workflow runs locally-
  green but needs a push to show green on Linux / Windows. Nothing in
  the code uses platform-specific APIs beyond the `HOME`/`USERPROFILE`
  split which is cfg-gated.

## Phase 3 (suggested)

- Write up the evaluation verdict for issue #686 against all six
  criteria. If the Go path wins: scope Phases 4–N as a port of the
  remaining `doctor` modes, then the other top-N commands.
- If Stop/Defer: archive this crate and capture the LOC / ergonomics
  learnings in the issue closeout.

## Why this directory isn't in CMake

Deliberate. The prototype runs entirely via `cargo`. CMake never
discovers it. No Pulp build, test, or ship flow is affected by
anything under `experimental/pulp-rs/`. The only way to exercise this
crate is to `cd experimental/pulp-rs && cargo <subcommand>` — which
keeps the main Pulp CI and release pipeline completely untouched
while the evaluation is in progress.
