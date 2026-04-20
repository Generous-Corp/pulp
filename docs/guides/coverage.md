# Coverage

Pulp tracks test coverage per-subsystem, per-platform, and per-surface as
a first-class metric alongside sanitizers and CI matrix results. The goal
is to tell at any moment whether any subsystem's test surface is tracking
its quality bar as the codebase grows — not to chase a vanity "100%"
number.

This guide covers the Phase 1 measurement baseline. Phase 2 adds per-tier
targets, Phase 3 adds a per-PR diff-coverage gate, Phase 4 adds doc-sync
enforcement. See issue #566 for the full initiative.

## Where the numbers live

All coverage is reported to [Codecov](https://app.codecov.io/gh/danielraffel/pulp).
The repo is public, so the dashboard is public and tokenless. No login
required. Three views worth knowing:

- **Overall**: <https://app.codecov.io/gh/danielraffel/pulp>
- **Per-flag breakdown**: <https://app.codecov.io/gh/danielraffel/pulp/flags>
- **REST API for agents**:
  `https://api.codecov.io/api/v2/github/danielraffel/repos/pulp/totals/`

The dashboard also supports per-file drilldown (click any file in the
tree view) and historical trend graphs.

## The 20 flags

Codecov splits coverage into 20 flags along three independent axes so you
can cross-filter:

| Axis | Flags | What it tells you |
|------|-------|-------------------|
| Core subsystem (13) | `audio`, `canvas`, `events`, `format`, `host`, `midi`, `osc`, `platform`, `render`, `runtime`, `signal`, `state`, `view` | Subsystem quality |
| Platform (4)        | `android`, `apple`, `linux`, `windows` | Per-platform shim coverage |
| Surface (3)         | `cli`, `ship`, `tools` | Non-core first-party code |

Examples of useful cross-filters:

- `audio AND linux` — "what's audio coverage on Linux?" Catches ALSA-only
  regressions that a single aggregate `audio` number would hide.
- `host AND windows` — "how well-tested is the Windows host layer?"
- `format` alone — "is the VST3/AU/CLAP adapter layer covered uniformly?"

The flag definitions are in `codecov.yml` at the repo root. Adding a new
subsystem means adding a flag there, a row in
`docs/reference/modules.md`, and (in Phase 2) a target row in
`ci/coverage-targets.yaml`. The Phase 4 doc-sync gate will keep those in
lockstep; until then treat it as checklist discipline.

## How to run coverage locally

```bash
scripts/run_coverage.sh
```

Output:

- `build-coverage/coverage/index.html` — per-file HTML drilldown (open in
  a browser).
- `build-coverage/coverage/summary.txt` — text summary matching the CI
  artifact.
- `build-coverage/coverage.cobertura.xml` — Cobertura XML (only if
  `gcovr` is on PATH; `pip install gcovr` if not).

The script requires Clang (not gcc) because we use Clang source-based
coverage, not gcov. See `tools/cmake/PulpInstrumentation.cmake` for the
flag configuration.

Optional flags:

```bash
scripts/run_coverage.sh --jobs 16                 # parallelism
scripts/run_coverage.sh --tests '^pulp-test-audio' # regex filter
```

### Troubleshooting

If `scripts/run_coverage.sh` fails with
`ERROR: PULP_ENABLE_COVERAGE=ON did not stick in the CMake cache`, the
`build-coverage/` directory was previously configured without coverage.
The fix is in the error message: `rm -rf build-coverage/` and re-run. This
guard exists because the silent version of this failure produced empty
profdata that looked like a test/instrumentation bug. Issue #570 has the
details.

If you see `_deps/catch2-build/src/src/catch2/...: No such file or
directory` spam on stderr, you're on an older checkout — the canonical
exclude regex in `scripts/run_coverage.sh` filters these. Update to the
latest `main`. Issue #569 has the details.

## How to interpret "my PR's coverage"

Every PR gets a comment from Codecov once the Coverage workflow
finishes. The comment layout is `reach, diff, flags, tree`:

- **reach** — lines this PR's tests exercised.
- **diff** — of the N lines this PR adds/modifies, K are covered by
  tests. This is the number to focus on — adding untested code is the
  most common way to regress a subsystem's coverage.
- **flags** — per-flag coverage (both total and delta). If you touched
  `core/audio/` and the `audio` flag shows −2%, something in your PR
  dropped the subsystem's coverage; check the tree view to see which
  file.
- **tree** — per-file drilldown. Click through to see exactly which
  lines are uncovered.

In Phase 1 all of this is advisory — nothing fails a PR on coverage.

> **Diff-cover advisory window (2026-04-21 → 2026-05-04).** Every PR now
> receives an additional "Diff coverage (Phase 1 advisory)" comment
> rendered by `.github/workflows/coverage.yml` (`coverage-diff-gate`
> job). It answers the single question "of the lines this PR adds or
> modifies, how many are covered?" at a **75%** floor. During the
> 2-week advisory window the percentage is **informational only** —
> even 0% will not block a merge. **On 2026-05-04 the gate flips to
> required**; after that date a PR whose diff coverage falls below 75%
> will hard-fail this check and block the merge. File an issue if the
> flip date needs to slip.

Phase 2 narrows the gate further, moving from a flat 75% to the
tier-based targets tracked in `ci/coverage-targets.yaml` (audio-critical
80% / user-facing 70% / infra 50%).

## How the collection works

```
Source → Clang -fprofile-instr-generate -fcoverage-mapping
         ↓ (at runtime, LLVM_PROFILE_FILE=...)
       .profraw  × N test binaries
         ↓ (llvm-profdata merge)
       pulp.profdata
         ↓                               ↓
    llvm-cov report/show           gcovr --cobertura
    (human-readable HTML          (Cobertura XML for
     + text summary)                Codecov + diff-cover)
```

- **Instrumentation**: enabled via `-DPULP_ENABLE_COVERAGE=ON` at
  configure time. See `tools/cmake/PulpInstrumentation.cmake`. Only
  enabled when Clang is the compiler — gcov/gcc output shapes are
  incompatible with our llvm-cov pipeline.
- **Collection**: `scripts/run_coverage.sh` runs the test suite with
  `LLVM_PROFILE_FILE` pointing at a per-test-binary template. Each test
  writes its own profraw shard.
- **Merge**: `llvm-profdata merge -sparse` unions them into a single
  profdata.
- **Report**: `llvm-cov show` produces the HTML locally; `gcovr` emits
  Cobertura XML for Codecov + diff-cover.
- **Exclusions**: a canonical regex in `scripts/run_coverage.sh`
  excludes `_deps/`, `external/`, `test/`, `catch2/`, `build/`, and
  `build-coverage/`. Codecov's `ignore` list in `codecov.yml` mirrors
  this set.

## Multi-OS coverage

Phase 1 PR 4 extends the coverage workflow to run on
`{ubuntu-latest, macos-latest, windows-latest}`. Each OS produces its
own `coverage.cobertura.xml` and uploads to Codecov with an OS-tag
flag (`os-linux`, `os-macos`, `os-windows`). Codecov unions same-file
coverage across OSes using the flag mechanism — we do NOT try to merge
profdata across architectures (llvm-profdata merge is not
architecture-portable, see
`planning/coverage-tooling-decision-2026-04-21.md` §7).

## Phase roadmap (#566)

- **Phase 0** — spike & stack decision (done, see planning doc above).
- **Phase 1** (this phase) — measurement baseline
  - PR 1: infra fixes (#569, #570 — landed)
  - PR 2: Codecov integration (landed)
  - PR 3: diff-cover advisory gate (this change — advisory until
    2026-05-04)
  - PR 4: cross-platform matrix
- **Phase 2** — per-tier targets encoded in
  `ci/coverage-targets.yaml`. Audio-critical subsystems (`audio`,
  `format`, `host`, `midi`, `signal`, `platform`) ≥ 80% line / 70%
  branch; user-facing (`view`, `render`, `cli`) ≥ 70% line;
  infrastructure (`tools`, `ship`, `events`, `runtime`, `state`,
  `canvas`, `osc`) ≥ 50% line.
- **Phase 3** — diff-cover flipped from advisory to required.
- **Phase 4** — docs cleanup + doc-sync enforcement (#567).

## Related reading

- Issue #566 — umbrella initiative
- Issue #290 — coverage hardening (test surface growth)
- `planning/coverage-tooling-decision-2026-04-21.md` — Phase 0 spike
  findings, stack rationale, perf numbers.
- `planning/coverage-sanitizers-spec-2026-04-16.md` — the earlier
  spec that landed `PULP_ENABLE_COVERAGE`.
