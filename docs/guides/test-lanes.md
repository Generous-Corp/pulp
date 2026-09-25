# Test lanes — what runs where, and why

Pulp runs its test suite in a few distinct **lanes**. Knowing which lane a test
lands in — and how to route a new test — is the difference between a fast,
trustworthy required gate and one that flakes on unrelated work. This is the
single source of truth for that model.

## The lanes

| Lane | Trigger | Gates the PR? | Builds examples? | What it runs |
|------|---------|---------------|------------------|--------------|
| **Required core gate** (`macos`) | every PR + every merge group | **yes** (blocking) | Actions: no; Shipyard: yes until promotion | merge group: all core tests **except** the `validation`, `slow`, `performance`, `bench`, and `quality-lab` labels; PR head: the build plus only the `pr-fast` tier (see below); an unchanged exact PR merge tree may reuse its artifact-bound result after protected-base verification |
| **Source selftests** (`Enforce version & skill sync`, step *Source-only selftests*) | every PR + every merge group | **yes** (blocking) | no build at all | the ~140 Python registrations in `tools/ci/source_selftests.json` (label `source-selftest`), which the `macos` gate excludes on gate events; see [below](#the-source-selftest-lane) |
| **Example-validation** (`example-validation`) | PRs touching `examples/**`, state/format headers, core CMake, or shared dependency infrastructure | advisory pending promotion (see status below) | yes — Linux + macOS | Linux compiles every example artifact; hosted macOS runs auval + built-in CLAP dlopen checks; pluginval/clap-validator require an operator-dispatched advisory image |
| **API contracts** (`api-contracts`) | every PR + every merge group | advisory pending promotion (see below) | no | the Doxygen strict pass over the catalogued public headers, ~3 s of work |
| **Nightly full build** | schedule (nightly) | no — **informational** | yes | everything, including all five excluded label groups; results eyeballed, build failures file an issue |
| **cross-platform-check** | per PR (x86-64 Linux, arm64 Linux, x86-64 Windows) | advisory | no | core tests, excludes `validation` + `slow` **only** — so the timing group does run here, off the reference platform |

The required gate is **serialized on self-hosted macOS runners** and takes
about 40 min (median 39.8 min of wall-clock over 45 sampled `pull_request`
runs). Keeping it
lean is why the two label groups below are excluded from it.

### Where the required gate's wall-clock goes

Measured per-step medians over the same sample, across the 40 runs that
allocated a native macOS build (the other 5 resolved to the no-native-build
alias and are excluded here, since they run no build or test step). The step
medians sum to 39.5 min, slightly under the 39.8 min job median — the
difference is per-run scheduling overhead, not a missing step:

| Step | Median | Share |
|------|--------|-------|
| `Build` | 21.3 min | 54 % |
| `Test (non-Windows)` | 14.8 min | 37 % |
| `Configure` | 1.3 min | 3 % |
| everything else (checkout, brew, ccache install, bootstrap, receipts) | ~2.1 min | 5 % |

Two properties of that profile are worth knowing before optimizing it. `Build`
already runs against a warm build dir at a 92-94 % ccache hit rate, so it is
dominated by linking rather than by recompilation. `Test` is **not** uniformly
parallel: under `-j8` the run reaches concurrency 5-8 for its first ~4 min and
then drops to a single test at a time for a median 10.0 min (n=6), because
`RUN_SERIAL` and `PROCESSORS 8` tests can only be scheduled alone and CTest
defers them until the parallel queue drains. `pulp-browser-capture-node-integration`
alone is ~6 min of that tail and is always the last test to finish; it is
`RUN_SERIAL` deliberately, because a real-Chrome CDP screenshot crosses its
bounded deadline when unrelated CTest work shares the machine. The serial set,
not the test count, is what sets the floor on the test phase.
For a merge group whose base, head, tree, policy, toolchain record, and tested
artifact identities exactly match a successful PR receipt, the protected-base
verifier derives a new merge-group-bound decision and the native repetition is
omitted. Any missing or changed binding retains the ordinary full gate.

## The label taxonomy (how routing works)

Routing is driven entirely by CTest `LABELS`, set in each test's
`set_tests_properties(... PROPERTIES LABELS "...")`:

- **`validation`** — a real-host format-validator (`pluginval-*`, `auval-*`,
  `clap-dlopen-*`). **Every user of this label lives under `examples/`** — it is,
  in practice, "an example plugin's runtime validation." Slow (a `pluginval` run
  is ~25-30 s) and flaky under concurrent load. **Excluded from the required
  gate**; reported by the advisory `example-validation` lane and also run
  nightly. They do not block merges until that context is promoted.
- **`slow`** — a genuinely long test (e.g. `cmake-ios-auv3-configure`, a
  ~25-30 min iOS try-compile). **Excluded from the ordinary required corpus**;
  run nightly. A slow test that must gate affected changes needs an explicit
  affected-surface step in the required job.
- **`performance` / `bench` / `quality-lab`** — a relative-timing, CPU-budget,
  or benchmark measurement (e.g. the sampler heritage suite's "Representative
  chain stays within the shipping CPU budget", which asserts a ratio against an
  in-run baseline). **Excluded from the required gate** since 2026-07-21. These
  are robust to *steady* load but not to the load **variance** produced when the
  Studio runs its two concurrent build VMs: a sibling VM's bursty compile
  inflates the ratio past threshold and the verdict tracks runner load rather
  than the code. They still run on push, on the nightly, and on
  `cross-platform-check`. A timing test that must gate belongs in a dedicated
  cap=1 perf lane, not on the merge path.
- **`pr-fast`** — additive, never exclusive: a static repository contract
  (lint, drift, registry-completeness, generated-manifest check) that also runs
  on the pull request head, where the rest of the suite does not. Members are
  listed in `test/cmake/pr_fast_tests.cmake`, which reports a listed name
  that this configuration did not register, and `pr-fast-tier-contract` fails if the label
  selects fewer than 50 tests or loses a pinned member. `build.yml` selects it
  with `ctest -L '^pr-fast$' --no-tests=error`, about 15 s for ~115 tests. A
  member must be deterministic, finish in seconds on a loaded gate VM, and
  assert no wall-clock or load-dependent bound: a forgotten regeneration then
  fails on the PR that caused it rather than ejecting a merge-queue batch, and
  the timing flakes that moved the full suite off the PR head stay off it.
  Tests carrying `pr-fast` still run in the full suite everywhere else.
- **`source-selftest`** — applied from `tools/ci/source_selftests.json`, never by
  hand: a Python registration that reads only the checkout. **Excluded from the
  required `macos` gate on gate events** and run instead by the build-free
  required `Enforce version & skill sync` context. `push` and every other lane
  still run it. See [the source-selftest lane](#the-source-selftest-lane).
- **no special label** — a normal unit/integration test. Runs on the **required
  gate**. This is where the vast majority of tests belong.

The required gate excludes these groups with one CTest filter,
`--label-exclude "validation|slow|performance|bench|quality-lab|source-selftest"`
— the filter `build.yml`'s ctest uses on `pull_request`, `workflow_dispatch`, and
`merge_group` (`tools/ci/ctest_gate_args.py`; `protected_merge_receipt.py` pins
the same string). The other lanes filter differently and deliberately: a macOS
`push` to `main` excludes `validation|slow|performance|bench|quality-lab` and so
still runs `source-selftest`, a Linux or Windows push excludes only
`validation`, and `cross-platform-check.yml` excludes only `validation|slow`. Read the lane you mean; the filters are not uniform. It is set in
[`.shipyard/config.toml`](../../.shipyard/config.toml) (`[validation.default]`,
`test =`).

### Affected slow proofs

`agent-capability-installed-sdk` installs Pulp and builds an independent
consumer for every exported capability and typed binding. It runs as its own
step on the required macOS job, where it measures a median 129 s (n=17,
range 104-174 s); charging that to every unrelated PR made a meaningful share
of the required test phase one irrelevant proof. It carries
`slow;agent-capability-installed-sdk` and is restored by `build.yml` only when
the exact diff touches the capability skill, installed manifest/schemas,
capability history, registry/generator, compile projection, CMake target/export
definitions, or their tests. The restored run executes on the required macOS
job and the parallel Linux matrix leg so platform-specific exports retain their
pre-merge proof; even an otherwise skip-safe selected documentation change
allocates those jobs. Relevant changes therefore still fail before merge, while unrelated PRs
and merge groups do not pay its cost. An unknown diff fails closed and runs it.

Read that selector honestly before treating it as narrow: the pattern list in
`tools/scripts/classify_changes.py` includes a bare `*.cmake` / `**/*.cmake`,
so **any** CMake file anywhere selects the proof — including a
`test/cmake/*_tests.cmake` manifest edited only to register an unrelated test.
Because "tests ship with fixes" makes such an edit routine, the proof is
selected by roughly half of the open PR population at any time (20 of 41 in one
census), and `*.cmake` is what selects it in the large majority of those.
Narrowing that glob would be a fail-closed weakening of a deliberately
conservative classifier, so it wants an explicit owner decision about which
`test/cmake` manifests genuinely reach the installed export surface — several
of them (SDK-consumer and smoke manifests) do.
The explicit restoration is limited to reduced PR, merge-group, and Shipyard
dispatch corpora; unfiltered main/nightly runs already include the proof and do
not run it a second time.

## The source-selftest lane

Roughly a sixth of the ctest registrations the gate configures are Python
scripts that read nothing but the checkout: CI-tooling selftests, source lints,
drift checks. Measured on five merge-group `macos` jobs on 2026-09-24
(107757339345, 107743043020, 107730781122, 107729256710, 107666303648), the 137
now in the lane cost ~580 s of the ~2,700 serial test-seconds, and five of them
(`PROCESSORS 8`, e.g. `gpu-dpr-v2-evidence-selftest`) ran alone in the serial
tail for ~100 s per run. None of that needs the build that
dominates the gate.

So they run in the required `Enforce version & skill sync` job instead, which
has no build, reports on `merge_group`, and takes minutes. Three pieces share one
manifest, `tools/ci/source_selftests.json`:

- `test/cmake/source_selftest_lane_tests.cmake` labels each listed registration
  `source-selftest` (included last in `test/CMakeLists.txt`).
- `tools/ci/ctest_gate_args.py` excludes that label on the gate events.
- `version-skill-check.yml` runs every entry with
  `tools/ci/source_selftests.py run`, in parallel, from an empty scratch
  directory unless the registration sets a source `WORKING_DIRECTORY`, with the
  registration's `TIMEOUT` (120 s default) and `RESOURCE_LOCK`, retrying once as
  the gate's `--repeat until-pass:2` does.

`source-selftest-lane-contract` (a ctest that stays on the gate, because it
needs the configured tree) runs `source_selftests.py check`. It fails when a
labelled test is missing from the manifest, a listed test is not registered or
not labelled, a manifest command no longer matches its registration, an entry
reaches the build tree or a path outside the checkout, an entry carries
`pr-fast` or an excluded label, an entry's script is platform-gated or imports
an optional third-party module (see below), or either half of the wiring is
gone. A test
can therefore leave the gate only by being run on another required context.

**Joining the lane.** A registration qualifies when it is `python3 <script>`
over source paths only, imports only the standard library (the lane has no
numpy, Pillow or PyYAML), is not platform-gated, is not a `pr-fast` member (that tier runs on the PR
head with the same exclusion), names no marker that
`control_product_b_absence_check.py` forbids (it scans `tools/`, so such a
test stays on the gate), and is
registered inside `test/` (a
`set_property(TEST)` from `test/` cannot label a test another directory
registered). Add it with
`python3 tools/ci/source_selftests.py write --build-dir build --add <name>`, and
refresh after editing a moved registration's arguments with the same command
without `--add`. The contract test tells you which one you need.

**Platform-gated tests stay on the gate.** A selftest that branches on the host
being macOS (`sys.platform == "darwin"`, `platform.system()`), or probes for
`codesign`, `lipo`, `xcrun`, `security` and similar, skips that half on Linux
and still exits 0. On the lane it would report a pass it never earned. The
contract scans each entry's script for those markers and for `yaml`, `numpy`,
`PIL`, `skimage` and `scipy` imports, and rejects any hit. The scan is textual
and broad on purpose: a false positive only keeps a test on the gate, where it
already ran.

**What it costs.** The required `Enforce version & skill sync` job grows from
about 2 minutes to several; it runs in parallel with the ~28-minute `macos`
job, so it does not lengthen a merge. The lane runs on Linux with Python 3.12,
where the gate ran macOS with Python 3.14. Every entry passed on the Linux
CI leg and under Python 3.12 with no build tree before it moved, and a macOS
`push` to `main` still runs them all.

## Why example validators are off the required gate

An example plugin's `pluginval`/`auval` run has real value — a plugin that fails
validation is broken in a real DAW — but it has **no business gating an unrelated
core PR**. Historically `pluginval-SuperConvolver-VST3` (an *example*) flaked ~30 %
of the time on the required gate and cost unrelated PRs hours (see
`planning/friction/2026-07-15-*`). Two things follow:

1. **Compile is checked on relevant changes.** `build.yml`'s required `macos`
   Actions job configures examples OFF. Shipyard's separate blocking
   `[validation.default]` temporarily keeps `PULP_BUILD_EXAMPLES=ON` until the
   always-reporting context below is promoted to a required check. The
   `example-validation` workflow compiles the full examples tree on Linux and
   macOS whenever an example, watched state/format header, core CMake surface,
   or shared dependency
   infrastructure changes, so a failure is visible on the
   relevant PR. Only the runtime *validators* are macOS-specific. This remains
   advisory until the status below is promoted.
2. **Available hosted validation runs on the PR that changes the example.** The
   `example-validation` lane
   ([`.github/workflows/examples-validation.yml`](../../.github/workflows/examples-validation.yml))
   runs the registered `validation`-labeled tests whenever a PR touches
   `examples/**`. Hosted macOS supplies `auval` and the built-in CLAP dlopen
   checks; `pluginval` and `clap-validator` run only on an operator-dispatched
   isolated advisory image that installs them.
   It is deliberately **not** a nightly-only deferral: a broken example
   validator is reported on the PR that introduced it. The nightly is only a
   backstop.

### example-validation lane status

The lane ships **not yet in `required_status_checks`**. It always runs and
reports a stable `example-validation` status (it internally skips the heavy work
on non-`examples/**` PRs), so it is **required-safe** — it can be added to branch
protection without the "Expected — waiting for status" dead-lock GitHub imposes
on a `paths:`-filtered required check. Promote it to required after one green
real-runner run on an `examples/**` PR. Until then it is visible-but-advisory.

## The API-contract lane

A public symbol under a catalogued module root (`core/timeline/include`,
`core/music/include`, `core/timeline_editor/include`, `core/timeline_view/include`)
must carry a doc comment. `tools/build-api-docs.sh --contract-only` runs Doxygen's
strict pass and `tools/scripts/timeline_api_docs_check.py` over the result, and
nothing else — about three seconds after checkout.

It has its own workflow rather than a step inside the docs preview build, and the
split is the point. The same check used to run only inside `docs-material.yml`,
which is not a required context. On 2026-08-16 it detected an undocumented public
typedef, reported FAILURE **before** the PR merged, and the PR merged anyway; main's
docs build then failed for eight hours and four unrelated PRs carried a red `build`
none of them caused. A check that can name a main-breaking defect but not prevent it
converts one bad merge into N misleading reds, which teaches everyone to ignore red.

Two properties of the workflow exist solely so it can be promoted to a required
context, and both fail silently if removed — `tools/scripts/test_api_contracts_workflow.py`
pins them:

- **It reports on `merge_group`.** A required context that does not fire for a
  queued group leaves the queue waiting on a result that never arrives.
- **It has no `paths` filter.** GitHub treats a required context that never
  reports as permanently pending, so a path-filtered required check blocks every
  PR outside its filter forever. The check is cheap enough to run unconditionally,
  so it does. (`merge_group` does not support `paths` at all.)

The published HTML render stays out of this lane deliberately: it is roughly an
order of magnitude more work and produces a preview artifact, not a verdict. It
continues to run in `docs-material.yml`, which re-checks the contract on its way to
the render. Putting the render back on this lane would repeat the mistake that put
example validators on the required gate.

Its one external dependency is Doxygen from the runner image's apt mirror, and
that install retries. A mirror hiccup is the most common hosted-Linux flake class,
and a lane meant to gate merges cannot fail on one. The version is deliberately
**not** pinned: `build-api-docs.sh` documents that CI's Ubuntu package and a
developer's Homebrew build disagree on some diagnostics, so pinning this lane
alone would make one runner image's version the contract while `docs-material.yml`
and `docs-deploy.yml` drifted from it. If image drift ever does flip this check,
pin all three together rather than just this one.

**Status: advisory until promoted.** Until `api-contracts` is added to `main`'s
`required_status_checks`, this lane reports the same defect the old one did and is
equally unable to stop it. Promotion is a branch-protection change:

```bash
ghapp api -X PATCH repos/Generous-Corp/pulp/branches/main/protection/required_status_checks \
    -f 'contexts[]=Enforce version & skill sync' \
    -f 'contexts[]=Build + prove + (owner-gated) deploy' \
    -f 'contexts[]=Vellum trusted freeze' \
    -f 'contexts[]=Vellum freeze' \
    -f 'contexts[]=macos' \
    -f 'contexts[]=api-contracts'
```

## When a test's premise cannot hold in a lane

Labels route tests that are *slow* or *flaky* in a lane. A third case is neither:
a test whose **premise is false** there, so it can only ever report a red that
means nothing.

The worked example is the trusted-host launch tests.
`loaded_runtime_closure_matches_policy()` rejects any image mapped into a launched
child that is neither an Apple platform image nor a pinned inventory file — that
rejection *is* the security property under test. A sanitizer build injects exactly
such an image into every process it produces: `clang++ -fsanitize=undefined` links
`@rpath/libclang_rt.ubsan_osx_dynamic.dylib`, which dyld resolves under the Xcode
toolchain, and Apple's clang has no static sanitizer runtime on macOS to avoid it.
So those tests cannot pass under a sanitizer, and the correct response is neither
to retry them nor to relax the production check — relaxing it would delete the
property the test exists to prove.

Express this **in the test source**, not with a label:

- Guard the affected `TEST_CASE`s on `PULP_TEST_WITH_SANITIZER`, which a target
  picks up via `$<$<BOOL:${PULP_SANITIZER}>:PULP_TEST_WITH_SANITIZER=1>`.
- Skip with a stated reason: `SKIP("…")`, never `SUCCEED`, `WARN` or a bare
  `return;`. Those three leave the Catch2 case **passing**, so the lane records
  nothing and the suite's pass count is identical whether the case ran or its
  precondition vanished. `SKIP()` is what ctest surfaces as `***Skipped`
  (`tools/cmake/PulpCatch.cmake` sets `SKIP_RETURN_CODE 4` on every discovered
  case) and what the required gate's non-run summary lists.
  `tools/scripts/check_skip_not_pass.py` checks this mechanically.
- Keep the explanation in one place — `test/support/control_runtime_closure_sanitizer.hpp`
  holds it for this case, including the measured dylib path.

Source-level guarding is deliberate. A label excludes a whole target: for these
files it would have dropped 15 passing tests to silence 7 impossible ones. And
coverage is preserved where it counts — the guarded cases still run, and still
gate, on every non-sanitizer lane including the required `macos` gate.

**Point the guard the right way, and prove it.** An inverted guard is silent in
both directions: the sanitizer lane goes red exactly as before, while every other
lane quietly stops exercising the code. `pulp-test-control-runtime-closure-sanitizer-guard`
asserts whichever direction is true for the build it is compiled into, so each
lane checks its own.

## Adding a test — where will it land?

- **A core unit/integration test** → add it with no special label. It runs on the
  required gate. Keep it fast (< a few seconds) and non-flaky. Catch2 suites
  compile against a shared precompiled header of Catch2 plus the common standard
  headers (`PULP_TEST_PCH`, Clang only); a suite that must see a `#define`
  before `<catch2/...>` or a standard header (a `CATCH_CONFIG_*` or `_LIBCPP_*`
  switch) opts out with `pulp_add_test_suite(... NO_PCH)`. ObjC++ sources and
  per-target `-f`/`-std` options opt out automatically; see
  `<build>/pulp-test-pch.tsv` for every decision. A grouped executable (below)
  is one decision, recorded under the group's name: `NO_PCH` goes on
  `pulp_add_test_group()`, and a member that must opt out alone stays
  ungrouped.
- **A new example plugin** → its `clap-dlopen`/`auval`/`pluginval` validators
  should carry `LABELS "validation;<format>"` (match the existing examples). That
  automatically keeps them off the required gate and onto the example-validation
  lane. Give `pluginval` a `TIMEOUT` comfortably above its real runtime (e.g.
  `120` — SuperConvolver runs ~25-30 s; 30 s was too tight and flaked).
- **A genuinely long test** (minutes) → `LABELS "slow"`, and make sure something
  (nightly, or a dedicated lane) actually runs it — do **not** rely on the
  informational nightly alone if it must be enforced.
- **A long proof needed only for a bounded source contract** → give it a
  descriptive label in addition to `slow`, add a fail-closed affected-diff
  classifier, and restore it explicitly in the required job on that surface.

## The trap to avoid

Labeling a test `slow`, `validation`, `performance`, `bench`, or `quality-lab`
**removes it from the required gate**. (`source-selftest` is the exception by
construction: its contract test fails unless the required source lane runs it.) If
nothing else runs it as a *gate*, you have silently disabled it — the nightly
runs it but does **not** fail on it. Before moving a test off the required gate,
make sure it is enforced somewhere. During the staged rollout,
`example-validation` reports example-validator failures but remains advisory;
promotion to a required context is what turns that signal into enforcement.
Use a dedicated gating lane for anything that must block before then. "It runs
nightly" is a backstop, not enforcement.

### A label is not the only way to leave the gate

Labels are the *visible* exit. The quieter one is an **opt-in CMake flag**: a
test registered inside `if(PULP_ENABLE_<FEATURE>)` does not run on a lane that
never sets the flag — it is not skipped, it is never registered, so it appears
in no ctest output at all and no label names it. `PULP_ENABLE_SCENE3D` defaults
OFF, and for a long time nothing in `.github/workflows/` or
`.shipyard/config.toml` set it, so the whole Renderer3D and scene3d surface ran
nowhere while the required gate stayed green. `.github/workflows/scene3d-advisory.yml`
is the lane that now covers it.

So when you add a test behind an opt-in flag, or add a flag that gates existing
tests, name the lane that sets it. The one-line check:

```bash
grep -rn "PULP_ENABLE_<FEATURE>" .github/workflows/ .shipyard/config.toml
```

Zero hits means zero coverage. Pair it with a flag you know is wired — for
example `grep -rc "PULP_ENABLE_GPU" .github/workflows/build.yml` returns a
non-zero count — so that an empty result reads as "not wired" rather than "my
grep was wrong".

More traps worth knowing before you write the lane — every one of them
returns a clean, confident, empty answer rather than an error:

- **`ctest -R` is case-sensitive.** `-R 'renderer3d|scene3d'` selects 144 of the
  gated tests and silently drops the capitalized Catch2 case names; the
  character-class form `-R '[Rr]enderer3[Dd]|[Ss]cene3[Dd]'` selects 205. A
  regex that matches less than you meant still exits 0.
- **A selection that matches nothing exits 0.** Pass `--no-tests=error`, and
  assert a floor on the selected count as well — the first catches an empty
  selection, the second catches one that merely shrank.
- **`-R` is a regex, so a literal `(` in a case name is a group.** A Catch2
  case whose name contains `run()` is selected zero times by
  `ctest -N -R 'run() clamps'` and once by `-R 'run\(\) clamps'`. Control for
  it with a pattern you know matches — a bare `-R 'LV2'` selecting 18 tests
  proves the instrument works while the specific pattern selects none.
- **A comma in a Catch2 case name makes that name unusable as a filter**, and
  the way it fails is worse than a miss. Catch2 splits a test spec on commas,
  so `./binary "A, B"` matches nothing, prints `No tests ran` and exits 2 —
  while the same binary exits 0 on a comma-free name. `confirm_failure.sh`
  reads that exit as `INCONCLUSIVE — the test already fails before any edit`,
  so a working test reports as one that does not cover its code and the honest
  next move, rewriting the test, is exactly wrong. Name new cases without
  commas; `\,` escapes one in an existing name.

### A test whose premise cannot hold in CI

No workflow in this repo checks out submodules
(`submodules: false` throughout `.github/workflows/`), so the private
`planning/` submodule is unavailable to every hosted lane by construction. A
test that reads a file from it can only be excluded, never fixed, on such a
lane — and the exclusion should say so, because a bare test name in an
`--exclude-regex` reads as a suppressed failure rather than a structural one.

Check the *transitive* dependency, not just the ctest arguments. Both
`scene3d-native-slice-handoff-contract` and its negative twin need the plan
file, but only the first names it in its arguments; the second reaches it
through a verifier that hardcodes the path. Excluding only the obvious one
leaves a permanent red.

## Grouped test executables (one binary, many suites)

Every Catch2 executable statically links the whole `pulp::view` stack, so a
manifest of N one-file suites costs N links of the same ~150 MiB of archives,
N copies on disk, and N relinks whenever one core `.cpp` changes. A **test
group** is one executable that several suites compile into, declared in the
manifest with `pulp_add_test_group()` and joined with `GROUP` on each
`pulp_add_test_suite()` call (`tools/cmake/PulpTestSuite.cmake`):

```cmake
pulp_add_test_group(pulp-test-group-view-widgets LIBRARIES pulp::view pulp::state)
pulp_add_test_suite(pulp-test-widgets GROUP pulp-test-group-view-widgets
    LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-text-editor-mouse GROUP pulp-test-group-view-widgets
    LIBRARIES pulp::view
    PROPERTIES RESOURCE_LOCK system-clipboard)
```

Nothing about routing changes. Each member keeps its own discovery call, so
its LABELS, TIMEOUT, RESOURCE_LOCK, ENVIRONMENT, TEST_SPEC and TEST_PREFIX
apply to exactly the cases it always applied to: the group runs Catch2 with
`--filenames-as-tags`, which tags every case `[#<source stem>]`, and each
member's `--list-tests` is scoped to its own sources' tags. The registered
command stays `<binary> "<case name>"`; only the binary is shared. A member
whose tag expression lists nothing fails the **build** (`FAIL_IF_EMPTY` in
`PulpCatch.cmake`) rather than silently registering no tests. Hidden cases
(`[.tag]`) stay out as they did on their own: Catch2 admits them whenever a spec
has a positive pattern, and the `[#<stem>]` term is one, so a member whose own
`TEST_SPEC` has no positive pattern is listed with `~[.]` appended.

### Converting a manifest

1. **Group by compile line, not by link line.** Members share one set of
   compile flags. Read them from `compile_commands.json` for the manifest's
   targets (strip `-o`/`-c`/`-MF`, ignore per-target `-D<path>` defines) and
   group targets whose flag *set* is identical. A member may only name
   `LIBRARIES` the group already links; the configure fails otherwise, so put
   the union on the group.
2. **Leave out anything that needs its own process**: a custom `main()`
   (`Catch2::Catch2` without `WithMain`), a `codesign` POST_BUILD on the test
   binary (identity tests sign *themselves*), a fixture path baked in with
   `$<TARGET_FILE:...>`, `-fno-exceptions`, RT allocation probes,
   `PASS_REGULAR_EXPRESSION` probes, and any test source compiled together
   with a library `.cpp` that the group's libraries also contain (duplicate
   symbols at link). A member whose whole TU sits behind a platform or build
   option (`#if defined(__APPLE__)`, `#if PULP_ENABLE_AUDIO_PROBES`) must be
   registered behind that same condition: standalone it silently listed no
   cases there, grouped it fails discovery.
3. **Find duplicate case names across the group** (`TEST_CASE`, `SCENARIO`,
   `TEST_CASE_METHOD`) and rename one side with a short suffix. Catch2 aborts
   at startup on a duplicate with equal tags, and CTest would register the
   name twice either way. Record every rename as
   `{"from", "to", "executable"}` for the parity check.
4. **Rewrite the registrations.** `add_executable` +
   `target_link_libraries` + `catch_discover_tests(... PROPERTIES LABELS "a;b")`
   becomes `pulp_add_test_suite(NAME GROUP <group> LIBRARIES ... LABELS "a;b")`.
   A target that was registered twice with different `TEST_SPEC`s becomes two
   `pulp_add_test_suite` calls with the same NAME and GROUP. Per-target
   `COMPILE_DEFINITIONS` / `INCLUDE_DIRS` become per-source properties
   automatically, and the group still reuses the shared Catch2 PCH (the
   ledger lists it under the group's name; `NO_PCH` is a group option).
5. **Prove parity, not just green.** Snapshot before and after with
   `tools/scripts/ctest_inventory_parity.py snapshot --build-dir build --out
   <file>` (build the affected targets first, or the placeholders differ), then
   `compare before.json after.json --rename-map renames.json`. It compares the
   whole inventory as a multiset of (name, properties) and reports any missing,
   extra or drifted test.
6. **Prove isolation.** Run each group binary whole and with `--order rand`
   (three seeds), and run each member alone
   (`<group> -# "[#test_widgets]"`). A case that passes alone and fails in
   the group has a static-state dependency: keep that suite out and say why.

`ctest` output, `-R`/`-L` selection, `confirm_failure.sh --test`, and the
changed-surface selector all keep working, since they address tests by name.
The one visible difference is `--target`: build the group
(`pulp-test-group-view-widgets`), not the old per-suite target.
