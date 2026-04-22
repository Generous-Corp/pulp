# Coverage

Coverage is measured per-subsystem, per-platform, and per-surface on
macOS, Linux, and Windows for the native C/C++ lane, with an additional
Python tooling lane on Linux covering `tools/scripts/**`,
`tools/deps/**`, and `tools/local-ci/**`, plus a Swift package lane on
macOS covering the Apple Swift sources that compile in `apple/`.
All reports are uploaded to Codecov on every push.
[#566](https://github.com/danielraffel/pulp/issues/566) tracks the
broader coverage initiative.

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

Native C/C++ coverage:

```bash
scripts/run_coverage.sh
```

Python tooling coverage
(`tools/scripts/**`, `tools/deps/**`, `tools/local-ci/**`):

```bash
python3 -m pip install 'coverage>=7.10' PyYAML
python3 tools/scripts/run_python_coverage.py
```

Apple Swift coverage (macOS only):

```bash
python3 tools/scripts/run_swift_coverage.py
```

Output:

- `build-coverage/coverage/index.html` — per-file HTML drilldown (open
  in a browser).
- `build-coverage/coverage/summary.txt` — text summary matching the CI
  artifact.
- `build-coverage/coverage/coverage.lcov` — LCOV produced by
  `llvm-cov export`.
- `build-coverage/coverage.cobertura.xml` — Cobertura XML converted
  from the LCOV via the vendored `tools/scripts/lcov_cobertura.py`.
- `build-coverage/python/html/index.html` — HTML drilldown for
  the Python tooling lane.
- `build-coverage/python/summary.txt` — text summary for the Python
  tooling lane.
- `build-coverage/python/coverage.python.xml` — Cobertura XML emitted
  by coverage.py for the Python tooling lane.
- `build-coverage/apple/summary.txt` — text summary for the Apple
  Swift source files under `apple/Sources/`.
- `build-coverage/apple/coverage.apple.json` — Xcode/SwiftPM Codecov
  JSON emitted by `swift test --enable-code-coverage`.

The native script requires Clang (not gcc) because we use Clang
source-based coverage, not gcov. See
`tools/cmake/PulpInstrumentation.cmake` for the flag configuration.
`llvm-profdata` and `llvm-cov` must be on PATH — on macOS, export the
Xcode toolchain:
`export PATH="$(xcrun -f llvm-cov | xargs dirname):$PATH"`.

The Python tooling lane requires `coverage.py >= 7.10` because
subprocess coverage support (`[run] patch = subprocess`) is what lets
tests like `test_resolve_runs_on.py`, `test_audit.py`, and
`test_local_ci.py` measure the actual script they spawn or import, not
just the test harness. `PyYAML` is also required because
`tools/scripts/test_codecov_config.py` parses `codecov.yml` as part of
the lane.

The Apple Swift lane currently runs on macOS only because it uses
SwiftPM's native coverage support (`swift test --enable-code-coverage`)
inside `apple/`. The staged Codecov JSON is source-only by policy:
`apple/Tests/**` and generated `.build/**` files are ignored in
`codecov.yml` so the Apple component reflects package sources, not the
test harness.

Optional flags:

```bash
scripts/run_coverage.sh --jobs 16                 # parallelism
scripts/run_coverage.sh --tests '^pulp-test-audio' # regex filter
python3 tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_resolve_runs_on.py'
python3 tools/scripts/run_python_coverage.py \
  --pattern 'tools/scripts/test_resolve_runs_on.py' \
  --pattern 'tools/deps/test_audit.py'
python3 tools/scripts/run_swift_coverage.py
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
Native:
Source → Clang -fprofile-instr-generate -fcoverage-mapping
         ↓ (at runtime, LLVM_PROFILE_FILE=...)
       .profraw  × N test binaries
         ↓ (llvm-profdata merge)
       pulp.profdata
         ↓                               ↓
    llvm-cov report/show        llvm-cov export --format=lcov
    (human-readable HTML                 ↓
     + text summary)             lcov_cobertura.py
                                         ↓
                                Cobertura XML (Codecov + diff-cover)

Python tooling:
Source → coverage.py run (with subprocess patch)
         ↓
     .coverage.* shards
         ↓ (coverage combine)
       .coverage
         ↓                ↓
    coverage report/html  coverage xml
         ↓                ↓
      summary + HTML   Cobertura XML (Codecov)

Apple Swift:
Source → swift test --enable-code-coverage
         ↓
  .build/.../codecov/PulpSwift.json
         ↓
  run_swift_coverage.py
         ↓
  summary + staged Codecov JSON (Codecov)
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
- **Report**: `llvm-cov show` produces the HTML locally;
  `llvm-cov export --format=lcov` plus the vendored
  `tools/scripts/lcov_cobertura.py` converter emit Cobertura XML for
  Codecov + diff-cover.
- **`-object` list**: llvm-cov only reports translation units linked
  into the binaries given via `-object`. The script passes every test
  executable **plus** every first-party static archive (`libpulp-*.a`)
  **plus** first-party non-test executables (CLI, standalone, inspect).
  Passing only test binaries — as the original implementation did —
  hides any subsystem no test transitively links, so a PR touching
  that subsystem slides past the diff-cover gate with no rows to
  score. Codex sanity-check + LLVM docs confirm `.a` archives are
  accepted as `-object` inputs:
  <https://llvm.org/docs/CommandGuide/llvm-cov.html>.
- **Exclusions**: a canonical regex in `scripts/run_coverage.sh`
  excludes `_deps/`, `external/`, `test/`, `catch2/`, `build/`,
  `build-coverage/`, `examples/`, and `fetchcontent-src/`. Codecov's
  `ignore` list in `codecov.yml` mirrors this set.

### Why not gcovr?

The initial pipeline used `gcovr --llvm-cov-binary <each_bin>` to
bridge llvm-cov → Cobertura. Once the `-object` set widened from "just
test binaries" to "every first-party library + executable," `llvm-cov
report` correctly saw ~110k tracked lines across ~580 files but gcovr
8.6 emitted a Cobertura XML with ~150 lines across 4 files — silently
dropping ~99% of the data. The direct
`llvm-cov export --format=lcov` + `lcov_cobertura.py` pipeline
preserves the full surface. gcovr is no longer installed in CI.

### Language coverage

Today the live dashboard includes:

- **Clang C/C++** coverage from the native build graph on macOS, Linux,
  and Windows.
- **Python** coverage for `tools/scripts/**`, `tools/deps/**`, and
  `tools/local-ci/**` on Linux via
  `tools/scripts/run_python_coverage.py`.
- **Swift** coverage for the Apple Swift package sources under
  `apple/Sources/PulpSwift/**` on macOS via
  `tools/scripts/run_swift_coverage.py`.

Still out of scope today:

- iOS-only Apple code that does not compile in the macOS SwiftPM lane
  yet (for example `apple/Sources/PulpSwift/PulpAudioSession.swift`).
- Python outside the current Linux lane (for example top-level
  `tools/*.py`, `tools/packages/**`, and any future repo-root Python
  scripts) — follow-up after the current tooling surfaces stabilize.
- Kotlin/Android coverage.

## Cross-platform matrix

The coverage workflow runs on
`{ubuntu-latest, macos-latest, windows-latest}` for the native lane.
Each OS produces its own `coverage.cobertura.xml` and uploads to
Codecov with an OS-tag flag. The Linux leg also uploads the Python
tools XML for `tools/scripts/**`, `tools/deps/**`, and
`tools/local-ci/**`, and the macOS leg also uploads the staged Apple
Swift Codecov JSON from `build-coverage/apple/coverage.apple.json`.
We do NOT merge profdata across
architectures —
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

## Related reading

- Issue #566 — umbrella initiative
- Issue #615 — Apple Swift coverage lane
- Issue #290 — coverage hardening (test surface growth)
- `planning/coverage-tooling-decision-2026-04-21.md` — Phase 0 spike
  findings, stack rationale, perf numbers
- `planning/coverage-sanitizers-spec-2026-04-16.md` — the earlier
  spec that landed `PULP_ENABLE_COVERAGE`
