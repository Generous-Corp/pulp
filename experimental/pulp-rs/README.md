# pulp-rs (experimental)

Rust prototype built to evaluate whether the Pulp CLI
(`tools/cli/*.cpp`) should be rewritten in Rust. **Not shipping. Not
user-facing. Not wired into any Pulp build.** See GitHub issue
[#686](https://github.com/danielraffel/pulp/issues/686) for the full
evaluation framework and decision criteria.

This crate lives on `explore/rust-cli-prototype` and will never merge
to `main`. It exists so reviewers can clone one branch, run a handful
of commands, and form an opinion about whether a phased C++ → Rust
CLI migration is worth the investment.

---

## Architecture

```text
  ┌─────────────────────────────────────────────────────────┐
  │  bin/pulp-rs  (src/main.rs)                              │
  │  ─ clap parser + subcommand dispatch, ~130 LOC           │
  └─────────────────┬───────────────────────────────────────┘
                    │ calls
                    ▼
  ┌─────────────────────────────────────────────────────────┐
  │  cmd/  (orchestrators — one module per subcommand)      │
  │  ├─ doctor.rs    → pulp doctor --versions [--json]       │
  │  └─ projects.rs  → pulp projects list [--json]           │
  └─────────────────┬───────────────────────────────────────┘
                    │ composes
                    ▼
  ┌─────────────────────────────────────────────────────────┐
  │  diag::   VersionDiag snapshot + findings rules + emitter│
  │  parse::  cmake, plugin.json, pulp.toml, SemverCompat    │
  │  registry::  ~/.pulp/projects.json reader                │
  │  color::  TTY / NO_COLOR-aware terminal attributes       │
  │  error::  typed domain errors (thiserror)                │
  └─────────────────────────────────────────────────────────┘
```

**Library + binary split.** `main.rs` is a thin clap wrapper over
`pulp_rs::cmd::*`. Integration tests link directly against the
library; benchmarks drive `diag::` and `parse::` APIs without
shelling out.

**Invariants the tests pin:**

- Byte-for-byte parity with the C++ writer for the doctor JSON
  lane. Captured `expected.json` fixtures are the oracle.
- Zero production coupling: no CMake discovery, no Pulp C++
  libraries, no FFI. Pure-Rust deps only.
- Graceful degradation: malformed input files → empty version →
  diagnostic continues. Mirrors the C++ "registry is best-effort"
  rule.

---

## Build and run

```bash
cd experimental/pulp-rs

# Fast dev cycle
cargo build
cargo run -- doctor --versions --json
cargo run -- projects list
cargo run -- projects list --json

# Release build (LTO, strip, panic=abort → ~1.9 MB mac binary)
cargo build --release
./target/release/pulp-rs doctor --versions --json
```

---

## Test

```bash
cargo test                                    # unit + integration + parity + snapshots
cargo test --test parity_test                 # doctor JSON parity only
cargo test --test projects_parity_test        # projects list parity only
cargo test --test snapshot_test               # insta snapshots

# Update snapshots after a deliberate shape change
cargo insta review                            # interactive
cargo insta accept                            # non-interactive

# Live parity against a built C++ CLI (optional):
PULP_CLI_PATH=/path/to/pulp/build/tools/cli/pulp \
  cargo test --test parity_test
```

| Lane                | What it exercises                                    |
|---------------------|------------------------------------------------------|
| unit tests          | Every pure-function module; proptest on parsers.     |
| doctor parity test  | Rust JSON vs captured C++ `expected.json` fixtures.  |
| projects parity     | Rust human vs captured C++ `expected_human.txt`; JSON vs `expected.json`. |
| snapshot tests      | insta snapshots of the JSON shape for regression.    |
| smoke tests         | Spawn the binary, assert top-level JSON shape.       |

---

## What's ported

| C++ command                                  | Rust status          | Notes                                     |
|----------------------------------------------|----------------------|-------------------------------------------|
| `pulp doctor --versions --json`              | Parity-tested        | Phase 2. Rules 1 / 1b / 2a / 2b / 3.      |
| `pulp doctor --versions` (human)             | Stub                 | Future phase.                             |
| `pulp doctor --scan-parents`                 | Not ported           | Future phase.                             |
| `pulp doctor --fix`                          | Not ported           | Future phase.                             |
| `pulp projects list`                         | Parity-tested        | Phase 4. Human + `--json` lane.           |
| `pulp projects list --json`                  | **Added by Rust**    | Phase 4. C++ has no `--json` flag today.  |
| `pulp projects add / remove`                 | Not ported           | Future phase.                             |
| everything else                              | Not ported           | Use the C++ binary.                       |

Five doctor fixtures and four projects fixtures drive the parity
tests. `expected_human.txt` was captured from the live C++ CLI and
normalised (the registry path is replaced with `<REGISTRY>` so diffs
are portable across machines).

---

## Decisions

- **`toml` crate over hand-rolling.** The C++ side walks `pulp.toml`
  line-by-line to keep its test-binary link surface small. We don't
  share that constraint — the `toml` crate handles quote/escape/key-
  boundary edge cases for free.
- **`thiserror` for domain errors, `anyhow` only at the `main`
  boundary.** Library functions return typed `CliError`; the binary
  flattens into `anyhow` for display. Reviewers can pattern-match on
  variants in tests instead of grepping strings.
- **`serde_json` `preserve_order` feature.** Required to reproduce
  C++ key ordering byte-for-byte without hand-writing a JSON encoder.
- **Library + binary split.** `main.rs` stays at ~130 LOC; every
  other surface is testable without subprocess spawn.
- **No FFI into the Pulp C++ library.** Criterion #4 of #686
  (zero production coupling) is a hard constraint: the prototype
  either matches C++ behaviour using Rust-only code, or it isn't a
  fair evaluation.
- **Inject overrides instead of mutating env in tests.** `diag::CollectOpts`
  and `projects::run_with_registry` exist so parallel-test runs don't
  race on `PULP_HOME` / `PULP_RS_CLI_VERSION`.

### Rejected alternatives

- **`fat` LTO for the release profile** — 3× compile time for ~2%
  binary-size gain. Not worth it for a prototype; `thin` LTO is the
  sweet spot.
- **`termcolor` / `owo-colors` dependencies** — four attributes
  (`reset`, `dim`, `yellow`, `green`) don't justify a dependency.
  Hand-rolled `color.rs` with `is-terminal` + `NO_COLOR` detection is
  ~50 LOC.
- **Re-implementing `Project` registry writes in Rust** — C++ still
  owns `add` / `remove`. Writing schema-compatible JSON from two
  places is a maintenance hazard; Phase 4 is read-only by design.

---

## Benchmarks

`cargo bench --bench hot_paths` drives Criterion over the four hot
paths that matter in a doctor run:

| Benchmark                       | Measurement (Apple M1 Pro, macOS 15.3) |
|---------------------------------|----------------------------------------|
| `parse_pulp_toml_1kb`           | ~3.65 µs                               |
| `parse_semver_clean_triple`     | ~108 ns                                |
| `parse_semver_prerelease`       | ~83 ns                                 |
| `compose_findings_100_projects` | ~5.25 µs                               |
| `emit_json_empty_diag`          | ~2.55 µs                               |

**Cold-start** of `target/release/pulp-rs doctor --versions --json`
on a realistic fixture: **4–5 ms** median across 20 runs. Target was
<100 ms on mac; we have two orders of magnitude of headroom.

**Binary size** (release, `strip=true`, `lto=thin`,
`codegen-units=1`, `panic=abort`): **~1.9 MB** on aarch64 mac.

Raw Criterion HTML reports land in
`target/criterion/<bench>/report/index.html`.

---

## How to extend — porting another C++ command in 4 steps

1. **Read the C++ source.** Identify the orchestrator (`cmd_X.cpp`)
   and the data-layer modules it depends on (`X_registry.cpp`,
   parsers under `cli_common.cpp`). Note every flag, exit code, and
   error message.
2. **Port the data layer first.** Add or extend a module under
   `src/parse/` or `src/registry.rs`. Keep every function returning
   `CliError`; add proptest fuzzers over parser inputs.
3. **Write the orchestrator** under `src/cmd/<command>.rs` with a
   `run(args, out: &mut impl Write)` signature. Take an explicit
   output sink so tests don't shell out.
4. **Lock behaviour down with fixtures.** Capture the C++ output for
   each interesting state into `tests/fixtures/<command>/<case>/`,
   normalise machine-specific bits, then add a test under
   `tests/<command>_parity_test.rs` that diffs Rust output against
   the captured reference. Snapshot-test any Rust-introduced shape
   with `insta`.

`src/cmd/projects.rs` is a complete worked example of the pattern.

---

## Quality gates

Every commit must pass, at minimum:

```bash
cargo fmt --check
cargo clippy --all-targets --all-features -- -D warnings -W clippy::pedantic -W clippy::nursery
cargo test
cargo bench --no-run
cargo doc --no-deps
```

CI on `.github/workflows/pulp-rs-experiment.yml` runs the same gates
on macOS / Ubuntu / Windows.

---

## Evaluation snapshot (six criteria, post-Phase-4)

| # | Criterion                  | Reading |
|---|----------------------------|---------|
| 1 | Parity                     | Doctor JSON: 5/5 fixtures byte-equal to C++. Projects human: 4/4 fixtures byte-equal after `<REGISTRY>` normalisation. |
| 2 | Lower complexity           | ~1000 production LOC Rust vs ~1100 LOC C++ across `version_diag.cpp + projects_registry.cpp + cmd_projects.cpp + cmd_doctor.cpp` slices. Not shorter by much, but every non-trivial function has a test, and the error surface is typed. |
| 3 | Test ergonomics            | 50+ tests run in <1s. Catch2 parity needs CMake test-binary wiring; Rust gets insta snapshots + proptest + `assert_cmd` out of the box. |
| 4 | No production coupling     | Crate sits under `experimental/pulp-rs/`. No FFI, no CMake discovery, no Pulp dep. Cold-clone + `cargo test` works. |
| 5 | Cross-platform CI          | Matrix workflow green locally on macOS. Linux + Windows runs pending first CI push. |
| 6 | Clear migration boundary   | Two commands ported cleanly using the same pattern. The boundary is "two commands; a larger cutover would repeat the pattern N times." |

---

## Why this directory isn't in CMake

Deliberate. The prototype runs entirely via `cargo`. CMake never
discovers it. No Pulp build, test, or ship flow is affected by
anything under `experimental/pulp-rs/`. The only way to exercise this
crate is to `cd experimental/pulp-rs && cargo <subcommand>` — which
keeps the main Pulp CI and release pipeline completely untouched
while the evaluation is in progress.
