# Coverage

Coverage is measured per-subsystem, per-platform, and per-surface on
macOS, Linux, and Windows, and uploaded to Codecov on every push.
It's measurement-only today; per-PR gates land in a later phase
(see [#566](https://github.com/danielraffel/pulp/issues/566)).

## Where the numbers live

Coverage is reported to [Codecov](https://app.codecov.io/gh/danielraffel/pulp).
The repo is public, so the dashboard is public and tokenless — no login
required.

- **Overall**: <https://app.codecov.io/gh/danielraffel/pulp>
- **Per-subsystem / per-platform / per-surface breakdown**:
  <https://app.codecov.io/gh/danielraffel/pulp/components/main>
- **Per-OS breakdown**:
  <https://app.codecov.io/gh/danielraffel/pulp/flags/main>
- **REST API for agents**:
  `https://api.codecov.io/api/v2/github/danielraffel/repos/pulp/totals/`

The dashboard supports per-file drilldown (click any file in the tree
view) and historical trend graphs.

## Axes

Codecov splits each upload along three axes so you can cross-filter —
for example, "how's one subsystem covered on one OS." The current list
of axes and the mechanism that populates them lives in `codecov.yml`
at the repo root; the dashboard is the source of truth for the current
set of names. Subsystem / platform / surface slicing happens via the
`component_management` block (path-globbed from a single upload).
Per-OS slicing happens via upload flags — one upload per operating
system in the matrix.

## How to run coverage locally

```bash
scripts/run_coverage.sh
```

Output:

- `build-coverage/coverage/index.html` — per-file HTML drilldown (open
  in a browser).
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
Fix: `rm -rf build-coverage/` and re-run. This guard exists because the
silent version of this failure produced empty profdata that looked like
a test/instrumentation bug. Issue #570 has the details.

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
  most common way to regress coverage.
- **flags** — per-OS coverage (total and delta).
- **tree** — per-file drilldown. Click through to see exactly which
  lines are uncovered.

Project-level and patch-level coverage numbers are informational — they
surface on every PR but don't block merges on their own.

**Diff-coverage gate (required).** Every PR also receives a "Diff
coverage (required)" comment from the `coverage-diff-gate` job. It
answers "of the lines this PR adds or modifies, how many are covered?"
at a **75%** floor. Sub-threshold diff coverage hard-fails this check
and blocks the merge — adding untested code means either adding tests
or splitting the untested portion into its own PR.

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
  `LLVM_PROFILE_FILE` pointing at a per-test-binary template. Each
  test writes its own profraw shard.
- **Merge**: `llvm-profdata merge -sparse` unions them into a single
  profdata.
- **Report**: `llvm-cov show` produces the HTML locally; `gcovr`
  emits Cobertura XML for Codecov + diff-cover.
- **Exclusions**: a canonical regex in `scripts/run_coverage.sh`
  excludes `_deps/`, `external/`, `test/`, `catch2/`, `build/`, and
  `build-coverage/`. Codecov's `ignore` list in `codecov.yml`
  mirrors this set.

## Cross-platform matrix

The coverage workflow runs on
`{ubuntu-latest, macos-latest, windows-latest}`. Each OS produces its
own `coverage.cobertura.xml` and uploads to Codecov with an OS-tag
flag. We do NOT merge profdata across architectures —
`llvm-profdata merge` is not architecture-portable
(`planning/coverage-tooling-decision-2026-04-21.md` §7); Codecov does
the cross-OS union at the flag layer.

Per-OS flags let the dashboard answer "what's covered on a specific
OS." Per-subsystem components answer "how's a specific piece of the
codebase covered." Cross-filter the two on the dashboard when a
question needs both axes.

Each OS uses Clang source-based coverage — MSVC is rejected by
`tools/cmake/PulpInstrumentation.cmake` because MSVC's
`/fsanitize-coverage` and llvm-cov emit incompatible profile shapes.
On Windows runners that means `clang-cl` from the bundled LLVM
(`C:\Program Files\LLVM\bin`), not `cl.exe`. macOS uses Apple Clang
from the active Xcode toolchain (resolved via `xcrun` when the bin
is not already on PATH).

The per-OS legs do NOT fail-fast: a flake on one OS does not cancel
the others, so the Codecov dashboard still gets partial cross-OS
coverage when one leg hits a transient toolchain issue. The
`coverage-diff-gate` job downstream stays pinned to the Linux
Cobertura XML — diff-cover is a single-XML tool and running it
against three XMLs would produce three PR comments with
slightly-different numbers for the same metric.

The per-OS runs-on labels resolve through the shared
`tools/scripts/resolve_runs_on.py` helper (same pattern as
`sanitizers.yml`). Set `PULP_COVERAGE_<OS>_RUNS_ON_JSON` as a repo
variable to route any one leg to Namespace or a self-hosted runner
without a workflow edit.

## Phase roadmap (#566)

- **Phase 0** — spike & stack decision. Landed.
- **Phase 1** — measurement baseline (Codecov upload, diff-cover
  advisory gate, cross-platform matrix). Landed.
- **Phase 2** — per-tier coverage targets encoded in
  `ci/coverage-targets.yaml`.
- **Phase 3** — diff-cover flipped from advisory to required.
- **Phase 4** — docs cleanup + generalized doc-sync enforcement
  (tracked in #567).

## Related reading

- Issue #566 — umbrella initiative
- Issue #290 — coverage hardening (test surface growth)
- `planning/coverage-tooling-decision-2026-04-21.md` — Phase 0 spike
  findings, stack rationale, perf numbers
- `planning/coverage-sanitizers-spec-2026-04-16.md` — the earlier
  spec that landed `PULP_ENABLE_COVERAGE`
