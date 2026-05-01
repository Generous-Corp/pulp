# Coverage

Coverage is measured per-subsystem, per-platform, and per-surface on
macOS, Linux, and Windows for the native C/C++ lane, with an additional
Python tooling lane on Linux covering `tools/scripts/**`,
`tools/deps/**`, `tools/local-ci/**`, top-level `tools/*.py`,
`tools/packages/**`, and `core/view/js/embed_js.py`, plus a Swift
package lane on macOS covering the Apple Swift sources that compile in
`apple/`, a Linux native binding lane that represents
`bindings/python/bindings.cpp`, and a Kotlin/Android lane for
JVM-unit-testable Android sources. Coverage percentages are only
actionable after the represented
local-source surface is trustworthy on Codecov; path/classification
drift and language-lane ingestion bugs come before ordinary test-gap
work.
[#641](https://github.com/danielraffel/pulp/issues/641) is the
authoritative tracker for the current three-phase program: first get
the intended source surface onto Codecov, then rank the real measured
gaps, then close them tranche by tranche.

## Representation comes first

For Pulp, a coverage number only means what it looks like after two
questions are answered in order:

1. Is this first-party source surface represented on Codecov at all?
2. If it is, how much of that represented surface is covered?

The control plane in `codecov.yml`, `.github/workflows/coverage.yml`,
`.github/workflows/android.yml`, and the helper scripts under
`tools/scripts/` defines what currently counts. The target tiers in
`ci/coverage-targets.yaml` come after that. A low percentage on a
correctly represented surface is a test-gap problem; a missing or `null`
surface is a Codecov-truth problem.

The Phase 1 representation stack is rebaselined in
`docs/reports/coverage-compliance-status.md`. Use that corrected
`main` snapshot for Phase 2 ranking; do not rank ordinary tranche work
from older pre-`#715` Codecov numbers.

## Phase 3 operating loop

Phase 3 is the measured gap-closure phase. The default loop is two
lanes running together:

1. Monitor the active `codecov` PR queue and merge only when branch
   protection is green and `mergeStateStatus` is `CLEAN`.
2. While checks are pending, keep moving the next small, non-overlapping
   tranche from the tracker map instead of waiting idle.

Each tranche should stay narrow: one subsystem slice, the smallest
deterministic tests that exercise the missing path, focused local
validation, a `codecov` label, and tracker comments on the relevant
component issue plus `#641`. Before opening or refreshing a PR, inspect
the active `codecov` queue and avoid files already owned by another
open tranche.

Use Namespace as the default outer validation target. For the current
workflow, `shipyard pr` remains the PR orchestrator, but local VM lanes
are deliberately skipped and the Namespace build is dispatched
explicitly:

```bash
source "$(git rev-parse --show-toplevel)/tools/scripts/cli_version_check.sh"
pulp_cli_version_check
shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows
shipyard cloud run build <branch>
```

If `shipyard pr` creates or updates the PR and then exits because no
local targets remain, that is expected for this Namespace-first loop.
Only fall back to local VMs when Namespace is unavailable, and use
GitHub-hosted lanes as the last fallback.

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
system in the matrix. First-party non-core surfaces are represented as
their own components too; for example, `inspect/**` appears under the
`inspect` surface rather than falling through as uncategorized code.

## How to run coverage locally

Native C/C++ coverage:

```bash
scripts/run_coverage.sh
```

Python tooling coverage
(`tools/scripts/**`, `tools/deps/**`, `tools/local-ci/**`, top-level
`tools/*.py`, `tools/packages/**`, and `core/view/js/embed_js.py`):

```bash
python3 -m pip install 'coverage>=7.10' PyYAML
python3 tools/scripts/run_python_coverage.py
```

Apple Swift coverage (macOS only):

```bash
python3 tools/scripts/run_swift_coverage.py
```

Android Kotlin coverage (requires Java 17 + Android SDK/NDK):

```bash
export ANDROID_HOME="$HOME/Library/Android/sdk"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
cd android
./gradlew :app:testDebugUnitTest :app:jacocoDebugUnitTestReport
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
  by coverage.py and normalized by the Python tooling lane.
- `build-coverage/apple/summary.txt` — text summary for the Apple
  Swift source files under `apple/Sources/`.
- `build-coverage/apple/coverage.apple.json` — SwiftPM LLVM coverage
  JSON kept for local summary/debugging.
- `build-coverage/apple/coverage.apple.lcov` — repo-relative LCOV
  exported from SwiftPM coverage data for Codecov upload.
- `android/app/build/reports/jacoco/jacocoDebugUnitTestReport/` —
  HTML + XML output for the Android Kotlin lane.
- `android/app/build/reports/jacoco/jacocoDebugUnitTestReport/jacocoDebugUnitTestReport.xml`
  — JaCoCo XML uploaded to Codecov from the Coverage workflow.

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
the lane. The generated coverage config also omits Python test modules
from the reported source set so the lane reflects first-party tooling
code rather than the test harness.

After `coverage combine`, `tools/scripts/run_python_coverage.py`
explicitly inventories the configured non-test `.py` source roots and
marks unexecuted files as measured before report generation. This keeps
zero-hit files visible as 0% rows instead of letting coverage.py's
package discovery hide non-package helper directories such as
`tools/packages/`. The generated Cobertura XML is also normalized to
repo-relative filenames with `<source>.</source>` so Codecov attributes
entries like `tools/audit.py`, `tools/deps/audit.py`, and
`core/view/js/embed_js.py` unambiguously.

The Apple Swift lane currently runs on macOS only because it uses
SwiftPM's native coverage support (`swift test --enable-code-coverage`)
inside `apple/`. The workflow keeps SwiftPM's LLVM JSON for the
human-readable summary, then exports repo-relative LCOV so Codecov can
attribute `apple/Sources/**` files against the git tree instead of
runner-local absolute paths. `apple/Tests/**` and generated `.build/**`
are still ignored in `codecov.yml` so the Apple component reflects
package sources, not the test harness.

The Android Kotlin lane runs through Gradle/JaCoCo rather than
the native Clang matrix. The always-on Coverage workflow provisions
Java + the Android SDK/NDK, runs `:app:testDebugUnitTest` plus
`:app:jacocoDebugUnitTestReport`, and uploads the resulting JaCoCo XML
to Codecov so unrelated `main` commits do not silently drop the Android
surface back to `null`. The dedicated Android workflow still owns APK
builds and emulator/device smoke.

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

### Phase 3 PR cadence

Phase 3 is tranche work, not a queue stall. While a Codecov remediation
PR is pending and blocked only on CI, keep moving with another small,
focused coverage tranche. Check the open PR list and GitHub checks
before starting each tranche, watch them again after pushing it, debug
new failures as soon as they appear, and manually merge PRs once they
are green.

Use Namespace-backed GitHub checks and Codecov comments as the default
evidence path. Local VMs are a fallback for reproducing or isolating
failures, not the normal proof that a tranche is ready.

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
  summary + repo-relative LCOV (Codecov)

Android Kotlin:
Source → Gradle `testDebugUnitTest`
         ↓
     JaCoCo execution data
         ↓
  `jacocoDebugUnitTestReport`
         ↓
   JaCoCo XML + HTML (Codecov)
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

Today the intended represented surface is:

- **Clang C/C++** coverage from the native build graph on macOS, Linux,
  and Windows.
- **Python** coverage for `tools/scripts/**`, `tools/deps/**`,
  `tools/local-ci/**`, top-level `tools/*.py`, `tools/packages/**`,
  and `core/view/js/embed_js.py` on Linux via
  `tools/scripts/run_python_coverage.py`.
- **Swift** coverage for the Apple Swift package sources under
  `apple/Sources/PulpSwift/**` on macOS via
  `tools/scripts/run_swift_coverage.py`.
- **Python bindings** native coverage for
  `bindings/python/bindings.cpp` through the Linux coverage lane.
- **Kotlin/Android** coverage for `android/app/src/main/kotlin/**`
  via the Coverage workflow's JaCoCo upload.

Still out of scope today:

- iOS-only Apple code that does not compile in the macOS SwiftPM lane
  yet (for example `apple/Sources/PulpSwift/PulpAudioSession.swift`);
  classified by `#656` and deferred unless simulator/runtime coverage is
  added through `#77`.
- Swift outside the `apple/` package lane (for example
  `tools/local-ci/macos_window_probe.swift`); classified by `#656`.
- Authored JavaScript assets (for example `core/view/js/**`,
  `android/app/src/main/assets/*.js`,
  `core/format/src/wasm/wam-plugin.js`, and
  `tools/browser-host/plugins/*.js`) — `#659`.
- Android emulator / device instrumentation coverage and any Android
  runtime behavior not hit by JVM unit tests — `#77`.
- Node bindings under `bindings/nodejs/**`; the Python binding surface
  is represented separately through `bindings/python/bindings.cpp` -
  `#657`.
- Shell and PowerShell scripts; they are tested indirectly today but do
  not surface as first-class Codecov lines — `#657`.

## Cross-platform matrix

The coverage workflow runs on
`{ubuntu-latest, macos-latest, windows-latest}` for the native lane.
Each OS produces its own `coverage.cobertura.xml` and uploads to
Codecov with an OS-tag flag. The Linux leg also uploads the Python
tools XML for `tools/scripts/**`, `tools/deps/**`,
`tools/local-ci/**`, top-level `tools/*.py`, `tools/packages/**`, and
`core/view/js/embed_js.py`, and the macOS leg also uploads the staged
Apple Swift LCOV from `build-coverage/apple/coverage.apple.lcov`.
We do NOT merge profdata across
architectures —
`llvm-profdata merge` is not architecture-portable
(`planning/coverage-tooling-decision-2026-04-21.md` §7); Codecov does
the cross-OS union at the flag layer.

Android/Kotlin coverage runs alongside that matrix in the same Coverage
workflow. It emits JaCoCo XML from Gradle and uploads that XML directly
to Codecov so the `android` component sees JVM-only Kotlin coverage
even though the native coverage matrix is Clang-specific. APK builds
and emulator smoke stay in `.github/workflows/android.yml`.

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
`coverage-diff-gate` job downstream consumes a **merged Cobertura
XML** built from all three OS artifacts via
`tools/scripts/merge_cobertura.py` (#635). Earlier the gate was
pinned to the Linux artifact only, which silently skipped Apple-only
(`au_adapter.mm`, `au_v2_*`) and Windows-only files — diff-cover
never saw them and the 75% gate was bypassed for any platform-
specific change. Merging takes `max(hits)` per `(filename, line)`
across the inputs, so a line covered on any OS counts as covered
overall. diff-cover stays a single-XML tool (one PR comment, one
number) but the silent-skip is closed.

**Local equivalent caveat.** `scripts/run_coverage.sh` produces only
the host-OS Cobertura XML. A local `diff-cover` invocation against
that single XML still has the silent-skip behaviour for files not
compiled on the host. Use it as a fast sanity check; the
authoritative cross-platform coverage gate is the merged XML
produced by CI's `coverage-diff-gate` job.

The per-OS runs-on labels resolve through the shared
`tools/scripts/resolve_runs_on.py` helper (same pattern as
`sanitizers.yml`). Set `PULP_COVERAGE_<OS>_RUNS_ON_JSON` as a repo
variable to route any one leg to Namespace or a self-hosted runner
without a workflow edit.

## Related reading

- Issue #641 — authoritative coverage-compliance tracker
- Issue #568 — historical multi-language expansion umbrella
- Issue #615 — Apple Swift coverage lane
- Issue #290 — coverage hardening (test surface growth)
- Issue #633 — Android/Kotlin JaCoCo coverage lane
- `planning/coverage-tooling-decision-2026-04-21.md` — Phase 0 spike
  findings, stack rationale, perf numbers
- `planning/coverage-sanitizers-spec-2026-04-16.md` — the earlier
  spec that landed `PULP_ENABLE_COVERAGE`
