---
name: ci
description: Local and cloud CI for Pulp — validate branches, create PRs, merge on green. Handles "push to main", "ship this", "run CI", "check PR", and "list PRs".
requires:
  scripts:
    - tools/local-ci/local_ci.py
  tools:
    - gh
---

# CI Skill

Validate branches and ship code safely. This skill handles all CI workflows for Pulp across local machines and VMs.

## A2T structural evidence is produced only by the required macOS PR job

An A2T evidence PR targeting `Generous-Corp/pulp` `main` that adds or modifies the exact tracked
`evidence/receipt.json` gets
one additional fail-closed step in the native `macos` matrix child. The step
runs `tools/scripts/a2t_structural_verification_ci.py`, which executes the
reviewed offline verifier with bounded stdout/stderr and uploads one immutable
`a2t-structural-verification-<PR-head>` attestation. The attestation is
structural and nonterminal: it binds the protected target repository/ref,
execution-time `S`/`E`, Git blobs,
digests, command, workflow revision, run attempt, job key, step, and result.
Its closed producer contract is
`docs/validation/gpu-trace-overhead/a2t-structural-verifier-attestation-v1.schema.json`;
the issuer validates every output against it, and the adjacent `fixtures/`
golden is the stable cross-repository consumer example. The verifier runs in a
detached checkout whose live `HEAD` is exactly `S`; the tracked `E` receipt is
mounted as a separate sibling input. The schema, issuer, issuer schema
validator, verifier, and every verifier-loaded support module are bound to the
reviewed `S` snapshot and must be byte-identical at `E` before any such
dependency is loaded or executed.
Regenerate the golden only with
`python3 tools/scripts/a2t_structural_verification_ci.py --write-golden`; the
producer test requires byte-exact equality with that canonical output. Do not
hand-edit derived fields such as `workflow.semantics_sha256`.
It intentionally cannot claim the future protected merge, its own Actions
artifact ID/digest/size, the final job conclusion, or terminal acceptance.
The planning validator must recover and authenticate those later from GitHub.

The event-pinned base/head tree diff, not path existence, gates this work. An
unrelated PR that merely inherits a historical receipt skips it, including a
later tool-only change. A receipt add/modify on a PR runs and publishes; the
same change targeting `develop/**` or another repository verifies without
issuing or uploading protected-main attestation authority. The
same change in a merge group or protected-main push reruns structural
verification but cannot issue or upload PR attestation authority. An exact
event revision that cannot be hydrated, an ambiguous receipt diff, or a
receipt deletion fails closed. A receipt that is untracked, symlinked,
different from the PR-head blob, bound to a different schema/issuer/verifier,
dynamically loaded dependency, trace, or source,
or produces noncanonical verifier output fails the required check. Linux and
Windows remain advisory and do not produce this authority.

## Current required-macOS truth (read before older incident notes)

Pulp's required PR and merge-queue macOS checks use the local M1/M3/M5 Tart
JIT pool. `build.yml` replaces the legacy base selector with exactly one
event-class-v2 label: `pulp-build-pr-head` for PR validation or
`pulp-build-merge-group` for merge groups. All three checked-in host profiles
can serve both classes; merge groups derive lease priority 110 and PR heads
derive 100. Do not reserve a host permanently for either class and do not set a
fixed profile-wide lease priority, because that disables class-derived
priority and can strand usable reserved capacity.
M1 is a deliberate delayed fallback and waits 10 minutes before taking Pulp
work; that affects latency, not its ability to serve either required class.

JIT runners exist in GitHub only while claiming a job, so an empty runner list
is healthy-idle as well as dead. Conversely, an organization-visible idle
runner is not proof that Pulp can assign it. Prove service with queue age,
healthy host supervisors, repository-visible registration, exact
job-to-runner assignment, and lease/VM reclamation. A checked-in profile says
what a host *can* serve, not whether its Pulp pool is enabled now; re-check M1,
M3, and M5 live rather than carrying an incident snapshot forward.

The names `pulp-gate-fast`, "local M1s", persistent Studio runners, and
busy-count overflow below describe superseded implementations or historical
incidents unless a section explicitly says otherwise. They are not the current
required-gate topology. The authoritative declarative surface is
`tools/scripts/runner_topology.json`; reconcile it with live variables using
`python3 tools/scripts/runner_topology_check.py --mode=report`.
The same checker accepts read-only `--fleet-profile`, `--fleet-receipt`, and
`--fleet-source-manifest` fixtures. Use them to prove that a TartCI source
profile serves both classes, the loaded receipt binds its exact digest, and the
private desired-fleet manifest agrees. Keep Pulp labels and host declarations in
those repo/private inputs, not generic Shipyard or TartCI code.

The canonical `.github/workflows/build.yml` `Build and Test` pull-request run
is excluded from broad stale-run and superseded-run janitors. Its narrow
old-predecessor/current-zero-job concurrency wedge is owned by Shipyard's
default-off, receipt-fenced recovery command. A generic age sweep or shell
cleanup must fail closed on a missing workflow path and must never cancel that
path; otherwise two cancellation actuators can race and make restart evidence
ambiguous. This exclusion does not authorize recovery by itself—the dedicated
Shipyard invocation remains separately enabled only after its pinned release,
receipt namespace protection, and canary are in place. Pulp pins the bounded
primitive at Shipyard v0.143.0; that pin supplies the command but does not turn
on apply mode. The protected-main worker must stay repository-serialized,
perform a dry run first, and prove one live exact-head canary before scheduled
activation.

## Runner timing metrics

When asked whether Pulp's local runners are fast, stuck, regressing, or worth
monitoring, query Shipyard's metrics surface before guessing. Pulp does not
mirror these records into `pulp` CLI or `pulp-mcp`; Shipyard is the metrics
store and tartci is an optional VM runtime emitter.

This metrics surface requires a Shipyard build that includes the
`shipyard metrics` subcommand. Pulp's pin in `tools/shipyard.toml` is `v0.83.0`,
which provides it, so the pinned binary is sufficient. That pin also makes
formal GitHub stacks fail closed at every Shipyard merge-queue mutation
boundary, including `shipyard runner steward`; use the native `gh stack`
lifecycle for an explicit pilot rather than routing stack members through the
unstacked enqueue path.

Use these commands as the normal agent loop:

```bash
shipyard metrics import github --repo Generous-Corp/pulp --limit 50 --json
tartci runtime export --repo Generous-Corp/pulp --since-days 14 \
  | shipyard metrics import tartci --json
shipyard metrics summary --project pulp --json
shipyard metrics watch --project pulp --since 14d --json
shipyard metrics advise --project pulp --json
```

Read `watch` as a triage signal, not as a hard failure. `insufficient_samples`
means the lane does not have enough history yet; drift, failure-rate, or slowest
findings are the useful prompts to inspect runner logs, VM boot behavior, cache
state, or GitHub queue time. If tartci is not installed for a repo, skip the
`tartci runtime export` import and still use the GitHub import plus Shipyard
summary/watch commands.

> **If a PR's required `macos` check has been queued >30 min** or the
> repo's PRs are all in `mergeable_state=blocked` with no movement,
> jump to **"Self-hosted runner ops"** near the end of this file.
> One-shot recovery is `shipyard rescue <PR>` (Shipyard v0.53.0+).
> Continuous prevention is `shipyard runner watch --kill-hung-workers`
> (v0.54.0+). Keep Shipyard itself current with `shipyard update`
> (v0.55.0+; Pulp currently pins v0.56.2+). All three replace the
> legacy `planning/scripts/runner-watchdog.sh --fix` workflow, which is
> now an anti-pattern (cancels queued runs but registers `failure` on
> required checks).

## Compiler coverage is asymmetric — GCC sees only `core/**`

Before you read a green PR as "this compiles everywhere": every Linux lane in PR
CI is **Clang** (public-headers, IWYU, RealtimeSanitizer). macOS is Clang.
Windows is MSVC. GCC used to appear in exactly one place —
`release-path-pr-gate.yml`, path-triggered on release files — so a GCC-only
error in `core/` could sit on `main` until an unrelated PR happened to touch a
Skia pin or a `Pulp*.cmake`.

`gcc-compile-gate.yml` now covers `core/**`: it compiles the core libraries with
`g++`, with `PULP_ENABLE_GPU=OFF` (no Dawn/Skia — that is what keeps it in
minutes) and tests/examples/design-import/inspector off.

Two things to know when it goes red:

- **It cannot flake.** No tests run, no hardware is touched, no timing is
  measured. A red result is a genuine compiler divergence. Do not re-run it
  hoping — read the error.
- **The classic offender is a duplicated designator** in a long
  designated-initializer list. Clang accepts a repeat and silently takes the
  last; GCC and MSVC reject it. `core/host/src/signal_graph.cpp`'s binder list
  has acquired one **four** separate times (`git log -S '.custom_latency_for'`),
  because it is long and sits where merges collide. If you resolve a conflict in
  a `{ .a = …, .b = … }` block, check you did not keep both sides of the same
  field.

What it does **not** cover is GCC *behavior* — nothing is executed, so a
construct both compilers accept but implement differently is still only caught
by the Clang test lanes.

Release-path configurations use `PULP_ENABLE_INSPECTOR=ON` starting at the
`inspector_sdk_floor` in `release_product_matrix.json`; earlier marker-era
backfills keep it OFF. The option controls whether the optional inspector SDK
archive family is built, while release runtime endpoints still default off.
Keep
`release-cli.yml`, `release-path-pr-gate.yml`, and the Windows MSVC
release-path configure in `build.yml` aligned; forcing the option OFF makes the
tag build succeed but the archive verifier fail after the expensive build.

**It also guards one option combination, for free.** The lane configures with
`PULP_ENABLE_DESIGN_IMPORT=OFF` — the option's own documented "release/ship"
setting — and that configuration was once unlinkable: `tools/import-design` was
added on `PROJECT_IS_TOP_LEVEL` alone while the `pulp::view` design-IR sources
it links sit behind the option, so `all` died in a wall of undefined
references. The discovery step now runs
`list_core_library_targets.py --assert-absent pulp-import-design` against the
codemodel the lane already produces, so a re-broken guard fails here instead of
in someone's release build. Because the guard lives in the top-level
`CMakeLists.txt`, that path is one of the lane's triggers alongside `core/**`.

If you add a target that links option-gated sources, gate the `add_subdirectory`
on the same option. A target whose implementation is compiled out still
participates in `all`.

## Test lanes — what gates the required `macos` check

For native pull requests, Shipyard `workflow_dispatch` validation, and merge
groups, the macOS matrix child publishes the literal required `macos` context
directly. It must not wait on the combined matrix: Linux and Windows are
advisory during the macOS-focused product phase and may continue after queue
admission. The `macos` and `macos-merge-group` bootstrap jobs own the required
name only for an intentional native skip or a fail-closed provider/classifier
failure; their inactive names end in `-unused`.
An exact unchanged merge group may also use `macos-merge-group` after
`protected-receipt-reuse` authenticates one unexpired successful PR artifact
and runs the verifier loaded from the exact protected-base parent. The receipt
binds base/head/tree, policy blobs, platform/toolchain, and hashed tested
executables; the bootstrap consumes only the newly derived merge-group
decision. Missing, ambiguous, expired, or mutated evidence must leave the
original matrix entry in place. Never make the PR receipt itself a required
context, and never execute the candidate's verifier as the authority.
`tools/scripts/test_required_macos_alias.py` and
`test_windows_runner_policy.py` pin this topology. Do not reintroduce a reporter
whose `needs` contains the combined `build` job.

### An advisory check that names a main-breaking defect is worse than none

`api-contracts.yml` runs the public-header doc-contract pass on its own
(`tools/build-api-docs.sh --contract-only`, ~3 s). It exists because that check
used to live only inside `docs-material.yml`, a preview-site build that is not a
required context. On 2026-08-16 it reported `FAILURE` on a PR **before** the
merge, naming the exact undocumented symbols, and the PR merged anyway. Main's
docs build then failed for eight hours and four unrelated PRs carried a red
`build` none of them caused — so every agent triaging a red check in that window
was debugging someone else's defect, and each one learned to trust red a little
less.

Two properties make it promotable to required, and both fail *silently* if
someone tidies them away — `tools/scripts/test_api_contracts_workflow.py` pins
them, so change the test deliberately or not at all:

- **`merge_group` trigger.** A required context that does not report for a queued
  group leaves the queue waiting on a result that never arrives.
- **No `paths` filter.** GitHub treats a required context that never reports as
  permanently pending, so a path-filtered required check blocks every PR outside
  its filter. Run it unconditionally; it is cheap.

It is still **advisory** until `api-contracts` is in `main`'s
`required_status_checks`, which means today it can name the defect and not stop
it — the same position the old check was in. The promotion command is in
[docs/guides/test-lanes.md](../../../docs/guides/test-lanes.md).

Do **not** promote the whole `docs-material.yml` workflow instead: its ~51 s
Material-site render is a preview artifact and has no business on the merge
critical path. That is the mistake this repo already made with example
validators.

### A documented guard may not run — check registration, not existence

`.shipyard/config.toml` stated the macro-gated-header ODR class was "Guarded by
`tools/scripts/test_odr_macro_gated_headers.py`". The script existed, worked, and
**ran nowhere**: not a ctest, not a workflow, not `tools/check-docs.sh`. The claim
was true of the file and false of its execution, and nothing in the repo could tell
the difference — a prose reference in a config comment reads exactly like
enforcement.

That left the class enforced only by the Shipyard mac lane's Debug build, which is
`backend = local` — so the signal existed on a host large enough to finish the
build and was simply absent on one that was not. The gate you got depended on which
machine typed `shipyard pr`, which is the same divergent-semantics class
`.agents/contract.toml` #6 was bought with, reached through host capacity instead
of tool version.

It is now `add_test(NAME odr-macro-gated-headers …)` in
`test/cmake/quality_tests.cmake`, so it rides the required `macos` gate and the
enforcement no longer depends on the shipping host.

**The generalizable check:** when a comment, doc, or config says a class is
"guarded by X", confirm X is *invoked* somewhere —

```bash
git grep -l "X" -- test tools .github   # references
git grep -n "X" -- test/cmake .github/workflows tools/check-docs.sh   # invocations
```

If every hit is prose, the guard is decorative. `tools/scripts/tools_registry_check.py`
enforces this property for the tools registry; nothing enforces it for guards named
in passing.

### The schema-fixture coverage gate runs on three surfaces, and only one has teeth on every PR

`tools/scripts/timeline_fixture_coverage_check.py` fails when a
`pulp.timeline.sequence` schema version lacks an indexed document fixture under
`test/fixtures/timeline/v<N>/` — the check a schema bump without fixtures used
to slip past. It executes in three places with different force:

- **ctest `timeline-fixture-coverage`** (registered in
  `test/cmake/timeline_tests.cmake`, no `LABELS`) — unlabeled, so it rides the
  required `macos` gate on every PR. This is the surface that blocks.
- **ctest `timeline-fixture-coverage-selftest`** — drives the script over
  synthetic corpora and proves it goes red on a missing version, on a version
  dir carrying only non-document kinds, and on an unindexed fixture, and that
  operational errors stay exit 2. Without it the gate could silently never
  fire, the same class as the ODR guard above.
- **job `fixture-coverage` in `timeline-hardening.yml`** — a no-build second
  opinion on the lane the corpus already cares about. That lane is advisory and
  `paths`-filtered (timeline
  cores, the fixture tree, the gate scripts, its own workflow file), so a PR
  outside the filter never runs it; the required-gate ctest is what every PR
  answers to.

### Running `gates.sh` before committing can be a false green

`config_doc_check.py`, `skill_sync_check.py`, and `version_bump_check.py` all diff a
**commit range** (`origin/main...HEAD`). Staged-but-uncommitted work is invisible to
them, so a pre-commit `gates.sh` reports `no mapped config paths touched` and exits
0 on a change that will fail the moment it is committed. Commit first, then run
gates — a green run over an empty range is not evidence about your change.

### Editing a `gpu-vellum-handoff.yaml`-pinned path is a TWO-commit operation

`docs/status/gpu-vellum-handoff.yaml` pins every referenced Pulp path to an
exact revision, object id and object type. Change one of those files and the
pinned row goes stale, so `gpu-recipe-catalog-selftest` and
`gpu-handoff-provenance-selftest` fail — **in CI, ~20 minutes later**. On
2026-09-05 three separate PRs each discovered it that way in one night, and one
of them additionally went `DIRTY` colliding with another PR's regenerated
receipt, because the ledger is a serialization point every such PR must pass
through.

The repair is a tool-generated identity refresh, never a hand edit:

```bash
python3 tools/scripts/gpu_handoff_provenance.py write   # regenerate
python3 tools/scripts/gpu_handoff_provenance.py check   # verify (~25s, git log per row)
```

**Hand-resolving the receipt is the trap**: it passes the merge conflict and
then fails the receipt's own checker. Merge first, then regenerate against the
merged tree.

`gates.sh` now runs `gpu_handoff_pin_freshness.py`, which is diff-scoped and
sub-second and fails the push when a pinned path changed without the ledger
being touched. It deliberately proves only *that* — it does not re-verify the
identity fields, because doing so costs ~25s per push. A green gate means "you
did not forget", not "the pins are correct"; the `check` command above is what
proves the latter.

### A changed-path preamble needs exact trees, not full repository history

Do not set the `build.yml` `classify` checkout back to `fetch-depth: 0`. GitHub
already supplies the immutable PR, merge-group, or push base SHA. The workflow
checks out the exact head at depth 1, fetches only that base object when absent,
and calls `classify_changes.py --comparison=trees`; an unavailable base fails
closed to the native build. This matters especially on a roaming reusable Mac:
a full-history preamble was observed entering a 70 GiB `.git` directory with
286 packs before doing any classification. Keep the 10-minute job timeout too.
Workflows whose actual release/audit algorithm traverses historical ranges may
still require full history; changed-path classification does not.

The preamble also cannot inherit its Python contract from an interactive shell.
macOS LaunchAgents default to `/usr/bin:/bin:/usr/sbin:/sbin`; on hosts where
`/usr/bin/python3` is 3.9, importing the classifier fails before it can request
the conservative full build because `.shipyard/config.toml` requires
`tomllib`. Keep the classifier step's explicit Homebrew/user-bin PATH, resolve
one interpreter through `tools/ci/find_python311.py`, and use that exact
interpreter for the base resolver, policy classifier, JSON extraction, and
protected-base version-bump verifier. A result that depends on whether M3 or M5
claimed `pulp-preamble` is a fleet fault, not a retryable check failure.

### Browser-source fidelity is a required dependency, not a skip

Generic agent HTML uses a real browser capture as its source reference before
Skia/native lowering.  The local ARM runner images deliberately do not retain a
mutable system browser, so `build.yml` installs the checksum-pinned Chrome for
Testing archive into the job temp directory and exports `PULP_DESIGN_BROWSER`.
When a browser-source test is added or changed, keep that bootstrap intact and
fail on download, checksum, extraction, or executable discovery.  Do **not**
convert a missing browser into a skipped fidelity test: that would let a PR
claim source-to-native validation that never occurred.

Browser-capture Node coverage has three distinct CTest entries. Keep the
dependency-free unit aggregate separate from the real-Chromium integration
file, whose serial browser cases have their own 600-second bound. The
esbuild-backed materialized-runtime canonicalization test is registered only
when `tools/import-design/jsx-runtime/node_modules/esbuild` exists, so the
required Linux `build.yml` leg must run the locked `npm ci --prefix
tools/import-design/jsx-runtime` step before CMake configure. Do not fold the
integration file back into the unit aggregate or remove that install step: the
former exhausts the aggregate deadline before later tests run, while the latter
turns canonicalization into either `ERR_MODULE_NOT_FOUND` or missing coverage.

Full model: **`docs/guides/test-lanes.md`**. Operationally, when a PR's required
`macos` check goes red on a test unrelated to the diff, check the label:

- `slow` and `validation`-labeled tests are **excluded** from the required gate
  (`.shipyard/config.toml` `test = "ctest ... --label-exclude \"validation|slow\""`,
  matching `build.yml`'s PR ctest and `cross-platform-check.yml`). The required
  gate also runs `--repeat until-pass:2`, so a single timing-flake self-heals.
  A slow proof can still gate an affected diff explicitly: the roughly
  12-minute `agent-capability-installed-sdk` test is restored by the
  fail-closed classifier on parallel macOS and Linux matrix legs for
  capability/install surfaces and CMake target/export definitions, while unrelated PRs and merge
  groups avoid that cost. A selected skip-safe documentation path must still
  force allocation of the native job that owns the proof. Do not remove
  that explicit affected step when maintaining the broad `slow` exclusion,
  and do not run it again on unfiltered main/nightly corpora that already own it.
- **`validation` is example-only** — the `pluginval-*` / `auval-*` /
  `clap-dlopen-*` validators under `examples/`. They do NOT gate core PRs; they
  run on the **`example-validation`** lane
  (`.github/workflows/examples-validation.yml`), which reports on relevant PRs
  and internally skips otherwise (required-safe: always reports). The lane
  remains advisory until the promotion described below.
- **The required `macos` gate does NOT compile the examples.** `build.yml` (and
  `cross-platform-check.yml`, the windows gates) configure with
  `PULP_BUILD_EXAMPLES=OFF`. The path-filtered `example-validation` workflow
  compiles the full examples tree on Linux and macOS for relevant changes;
  Shipyard's separate blocking `[validation.default]` temporarily retains
  `PULP_BUILD_EXAMPLES=ON` until that stable context is promoted to required.
  `release-cli.yml` also compiles them at release time. Do not assume a green
  required gate by itself means the examples build.

So a red required gate should NOT be an example `pluginval`/`auval` timeout — if
it is, the exclusion regressed. The `example-validation` lane is **not yet in
`required_status_checks`**; promote it once it is green on a real `examples/**`
run.

### Gotcha: a broken example goes green on `main` and breaks the RELEASE build

Because the required gate builds `PULP_BUILD_EXAMPLES=OFF` and only `release-cli`
builds examples, an example that fails to compile can merge green and then fail
every release build until it is fixed — silently making the SDK unpublishable.
The nastiest form is a compiler *disagreement*: an out-of-declaration-order
designated initializer (`.kind` before `.range`) is a **hard error under GCC**
but only a **warning under Clang**. It compiled clean on every macOS/Clang check,
then killed both Linux (GCC) legs of the release (`#6082`, the `.kind`/`.range`
swap; it stranded ~10 tags).

Guard (`build(examples)` PR): `examples/CMakeLists.txt` promotes
`-Wreorder-init-list` to an error under Clang (GCC already errors by default), so
the reorder now fails wherever examples compile — `release-cli` AND the
`example-validation` lane — on every compiler. `examples-validation.yml` also
triggers on `core/state/include/**` + `core/format/include/**`, because a field
reorder in a header the examples brace-initialize breaks their inits without
touching `examples/**`. Proven by the `cmake-examples-reorder-init-guard` ctest.
If you add a NEW example struct pattern that a compiler tolerates but the release
compiler rejects, extend that guard rather than discovering it at release time.

### Which workflows may set `PULP_BUILD_EXAMPLES=OFF` — and which must not

`PULP_BUILD_EXAMPLES` defaults **ON** (`CMakeLists.txt`), and `examples/` is 89
`add_executable` targets. A workflow that configures the ROOT project and then
builds with no narrowing `--target` compiles all 89 — usually to throw them away.
That is not free: every Linux, Linux ARM64 and Windows leg of
`cross-platform-check.yml` was cancelled at its 60-minute job limit, so the
cross-platform *backstop* delivered no cross-platform signal at all until
examples were turned off there.

Four rules before you add or remove the flag:

1. **Check the `-S` argument and the step's `working-directory` first.** Only a
   root configure is affected. `web-plugins.yml` (8 configures) and
   `wclap-cloudflare.yml` (5) look like the biggest wins and are not wins at
   all — every one targets an `examples/web-demos/*` tree that declares its own
   `project()` and never includes the Pulp root, so the option is simply unused
   there.
2. **Check whether the build narrows with `--target`.** If it does, the flag
   trims *configure* work only — say so rather than claiming a build-time win
   (`release-path-pr-gate.yml`, `non-skia-build-guard.yml`).
3. **Grep for example TARGET names before disabling.** Several workflows build
   an example by name and will hard-fail with the tree gone:
   `format-baseline-diff.yml` (`PulpEffect_AU|VST3|CLAP` — with none built it
   exits 1) and `nightly-intel.yml`'s universal cross-check
   (`PulpGain_VST3 PulpGain_AU PulpGain_CLAP`, feeding `lipo -archs` + `auval`).
4. **Grep for consumers of the built bundles.** `build/{VST3,CLAP,AU}/*` come
   from `pulp_add_plugin`, and every caller wired from the root lives under
   `examples/`. `validate.yml` globs those directories — with examples OFF the
   globs go empty and it validates *nothing*, silently and green.

Deliberately examples-ON, do not "fix": `examples-validation.yml` (its entire
purpose) and `nightly-full-build.yml` (whose configure step says so in a
comment). Shipyard's `[validation.default]` likewise keeps them ON on purpose.

## A test that "fails" on the required gate may only have run out of clock

Before debugging what a failing gate test *does*, check whether it failed on
**elapsed time** rather than behavior. A subprocess-spawning test that reddens
`macos` on PRs that cannot possibly have caused it — a docs-only or CI-only PR —
is the tell. `pulp ship sign discovers desktop bundles via env and config
identities` read as a signing/keychain break for exactly this reason; it was a
10s cap on a case that spawns `codesign` three times, runs ~2s unloaded, and
loses its margin under a `-j8` run over 13k tests. The symptom is a bare
`REQUIRE_FALSE(x.timed_out)` → `!true`, which names neither the subprocess nor
the duration — so it reads like a logic failure.

The layering rule that prevents this class:

- **`ctest --timeout` is the binding guard.** `build.yml` runs
  `ctest … --repeat until-pass:2 -j8 --timeout 120`: a per-test hang guard plus
  one automatic retry. An in-test subprocess cap must stay comfortably **looser**
  than it, so a slow machine fails at the outer layer — which reports the test
  name and elapsed time — instead of at the inner one, which reports neither. An
  inner cap tighter than the outer guard is strictly harmful: it fires first and
  diagnoses worse.
- **These caps are hang guards, not performance budgets**, so the costs are
  asymmetric. Too generous only delays a genuinely wedged child (already bounded
  at 120s). Too tight buys recurring false reds on unrelated PRs. Err generous.
- **Don't make them adaptive.** Self-calibrating or load-scaled timeouts make
  failures unreproducible and stretch to accommodate real perf regressions. A
  fixed generous default plus an env override is the design.

Root cause of the drift: the CLI shellout suites each hand-roll their own helper
and hardcode a timeout, so there is no shared default to inherit and a
codesign-heavy suite could sit at 10s while its siblings used 30-60s. When
adding a shellout test, reuse the shared helper rather than picking a number.

## Gate: framework-neutrality (`tools/scripts/framework_neutrality_check.py`)

Hard-fails a PR when Pulp's own source names another UI framework — in a
comment, a doc-comment, an identifier, or a test name — or adopts one of its
class names into a Pulp API. Runs in `gates.sh` (gate 14), in CI, and as a
ctest.

Two halves, and the second is the one that catches the deeper problem:

* **prose** — a foreign framework named in Pulp source.
* **names** — a foreign framework's *class name* adopted into Pulp's API. A
  comment is a liability; an adopted type name is the liability shipped in the
  public headers, where every downstream user inherits it.

It doubles as a design check: **if a doc-comment needs a foreign framework's
vocabulary to make sense, the API is a compatibility shim wearing a feature's
clothes** — redesign it rather than renaming it.

Three carve-outs, each of which would be a *bug* to "clean up":

* `core/format/host_quirks/**` and the tests that pin its `LessonOnly` rows.
  The prior-art citation IS the audit trail the `Reference-Lineage` trailer
  policy requires; removing it destroys the provenance it exists to record.
* `test/test_cli_import*.cpp` — those files carry the importer's own
  **denylist literals**. Strip the framework names and the check that rejects
  vendored foreign code silently passes everything.
* VST3 interface names Pulp implements (`IPlugView`, `IPluginFactory`). Word
  boundaries keep them out of the net — they are Steinberg's names, spoken by
  necessity.

Translation tables (`SomeFramework::Widget → pulp::view::Widget`) are *data* and
belong in the importer that owns them, never in `core/`.

If you hit it: rewrite the comment to say what the code *does*, in units a
reader who has never seen that framework can act on. Rename an adopted type and
ship a `[[deprecated]]` alias so downstream keeps compiling — lines containing
`[[deprecated` are skipped, since naming a foreign name in order to point
*away* from it is the mechanism that makes a rename shippable.

`--selftest` proves the gate can fail (8 cases). A gate that cannot fail is not
a gate.

## Pre-flight: plugin ↔ CLI skew check

Before shelling out to `pulp` (or `shipyard pr`, which ultimately
invokes `pulp`), source the shared skew-check helper so a user on an
outdated CLI sees a one-line hint rather than running into obscure
flag-missing errors mid-ship:

```bash
source "$(git rev-parse --show-toplevel)/tools/scripts/cli_version_check.sh"
pulp_cli_version_check
```

This is advisory only — the helper never blocks. See the `upgrade`
skill for the full banner contract and override knobs
(`PULP_SKEW_CHECK_DISABLE`, `PULP_SKEW_CHECK_CACHE`). Release-discovery
Slice 6 (#551).

## Diagnosing a slow / stuck PR — investigate before assuming runner saturation

When a PR sits without merging, don't ASSUME "the macOS CI pool is saturated"
from a long-queued run — investigate. Saturation IS possible (a genuine burst, or
a wedged/dead runner — see the `pulp-runner-ops` skill), so it's worth checking;
just don't conclude it without evidence. In the 2026-06-18 case the cause turned
out to be non-hardware (a misdiagnosis worth not repeating). Check in this order:

1. **Did the required checks even register?** A PR opened by the **Shipyard GitHub
   App** does NOT auto-trigger `pull_request` workflows, so the required `macos`
   and `Enforce version & skill sync` checks never appear on the PR head SHA until
   you dispatch them by hand:
   ```bash
   ghapp workflow run build.yml --ref <branch>             # posts the required `macos` check
   ghapp workflow run version-skill-check.yml --ref <branch>  # posts `Enforce version & skill sync`
   ```
   This is the most common reason a Shipyard PR "sits." (`shipyard pr` dispatches
   `build.yml` itself but you may still need `version-skill-check.yml`.) After a
   new push the head SHA changes — re-dispatch on the new SHA.
2. **Is it a version-bump race?** The other concurrent agent re-bumping `main`'s
   `CMakeLists.txt VERSION` makes the PR `DIRTY` (conflict on the VERSION line).
   Merge `origin/main` in, re-resolve the VERSION to one above main, push,
   re-dispatch the checks. (Longer-term fix for this whole class:
   planning/2026-07-07-parallel-merge-land-coordination.md. In progress: the
   `version-at-land.yml` bot — currently DRY-RUN — will make a single writer
   assign the version from `Version-Bump:` intent trailers post-merge, removing
   the per-PR race. Until it flips to `--apply`, keep hand-bumping as today.)
2b. **Did a gate reject the push?** Besides skill-sync / version-bump, the
   pre-push + CI `planning-gitlink` gate (`tools/scripts/planning_gitlink_guard.py`)
   fails if the PR moved the `planning` submodule pointer without a
   `Planning-Bump:` trailer — usually an accidental bump from a `git reset --hard`
   + `git add -A`. Drop it with
   `git restore --staged --worktree planning && git submodule update planning`,
   or add `Planning-Bump: reason="..."` for a deliberate re-pin.
2b1. **Config→doc drift?** The `config-doc` gate (`tools/scripts/config_doc_check.py`,
   map in `tools/scripts/config_doc_map.json`) fails if a mapped CI/release config
   surface (`.shipyard/config.toml`, the shipyard pin/installer, the
   `build`/`auto-release`/`version-skill-check`/`coverage` workflows) changed
   without its guide doc (`versioning.md` / `local-ci.md` / `release-watchdog.md`).
   Editing a workflow under `.github/workflows/` therefore usually needs a matching
   guide edit; a genuinely doc-irrelevant change bypasses with a
   `Config-Doc: skip reason="..."` trailer on any commit in the range.
2b1a. **Reaping a release tracker: SHA-keyed ones cannot be judged from the title.**
   The watchdog opens two shapes of tracking issue, and they are reaped
   differently. A *version-keyed* tracker ("release: stuck vX.Y.Z") names its
   version in the title, so the reaper closes it as soon as that tag exists.
   The *stranded fix/feat* detector instead opens one tracker per tip SHA and
   the title carries only the short SHA — a tag existing says nothing about
   whether that commit is in it. Reaping it requires parsing the issue BODY for
   the full tip SHA and the uncovered surfaces, then closing only once a *later*
   tag for **every** uncovered surface contains the commit
   (`git tag --contains <sha>`) — i.e. consumers can actually reach the change.
   Closing on "a tag appeared" would mark a still-unreleased change as shipped.

2b1b. **Byte-exact undo of a recent landing?** The `silent-revert` gate
   (`tools/scripts/silent_revert_guard.py`) fails if the push restores the
   pre-landing bytes of EVERY file a landing from the last 72h touched. That
   shape reads as ordinary work in review — a plausible diff, not an undo — and
   has landed on main and erased a real change; a stale worktree, a mis-resolved
   merge, or `git add -A` over old content all produce it. It is local-only
   (pre-push + `gates.sh`), pure blob-sha comparison against local git — no
   build, no network — and degrades to a pass when git cannot answer. The usual
   cause is a genuinely stale branch: rebase onto `origin/main` and re-check. A
   deliberate revert passes by stating intent — a `Revert "..."` subject, a
   `Revert-Of:` trailer, or `Silent-Revert: skip reason="..."`. Do not reach for
   the skip trailer to get past a revert you did not intend: the gate is the only
   thing that sees this class.
2b2. **Hotspot growth?** The `hotspot-size` gate is now net-delta vs merge-base:
   it fails only if THIS PR grows a frozen hotspot past its reference size (main
   growing the same file is NOT your fault and passes). If you must grow one,
   make the change net-neutral (extract to a sibling file) or add
   `Hotspot-Grow: <path> reason="..."` — do NOT *raise* `max_loc` in
   `hotspot_size_guard.json` (that counter no longer gates growth, and raising it
   races with other PRs).
2b3. **Hotspot *shrink*?** The ceiling is a one-way ratchet, and this is the
   opposite instruction to 2b2. If your PR makes a frozen hotspot **smaller**, the
   gate fails with `missing hotspot ceiling reduction(s)` until you *lower*
   `max_loc` to the new LOC. Lowering is safe (it cannot race a concurrent PR into
   a false pass); raising is not. So: never raise it, always lower it when you
   shrink. A PR that both grows one hotspot and shrinks another needs a
   `Hotspot-Grow:` trailer *and* a `max_loc` reduction.
2c. **Is a RED check even your fault?** Before investigating a failing check,
   run `python3 tools/scripts/pr_check_triage.py <PR#>` — it labels each red
   check REQUIRED vs advisory and PRE-EXISTING (also red / not run on main —
   not your change) vs REGRESSED (green on main, red here). Advisory +
   pre-existing red (e.g. a known-broken sanitizer lane on main) does NOT block
   the merge and is not yours to fix; only a REQUIRED + REGRESSED row needs
   action. This alone avoids chasing main-side breakage. Its check-run query
   must keep `gh api --paginate --slurp`, `filter=latest`, and `per_page=100`:
   bare `--paginate` concatenates page documents and breaks its single-document
   JSON decoding past 100 check runs.
3. **Only THEN consider capacity — and verify, don't assume.** Same-repository
   self-hosted PR and merge-group `macos` gates target the event-class-v2 local
   JIT VM pool, selected by the mutually exclusive `pulp-build-pr-head` and
   `pulp-build-merge-group` event classes. M1, M3, and M5 have compatible
   profiles, but a checked-in profile is not live capacity: only a host whose
   matching Pulp gate supervisors are enabled and healthy is participating.
   Fork, hosted, workflow-dispatch, and operator selectors retain their
   separately resolved shapes.
   Confirm currently registered runners with:
   ```bash
   ghapp api repos/Generous-Corp/pulp/actions/runners \
     | python3 -c "import sys,json;[print(r['name'],r['status'],'busy='+str(r['busy'])) for r in json.load(sys.stdin)['runners']]"
   ```
   A zero-runner result is consistent with healthy idle but proves neither
   health nor failure; use queue age plus host-side supervisor/lease/VM
   evidence and exact job-to-runner assignment. At the 2026-08-30 incident
   boundary M1 and M5 participate while M3's two compatible profiles are
   intentionally disabled pending the admission-fix canary. Re-check live
   supervisor state rather than carrying that incident snapshot forward. What
   does queue independently is the **GitHub-hosted advisory lanes** (Linux,
   Windows, sanitizers, coverage, android) on GitHub's shared pool; those are
   advisory, not the required gate, so a long queue there does not block merge.
4. **Is `shipyard run/ship` failing at `stage=configure` with `D
   external/skia-build/build`?** That's the Skia symlink loop (a tracked
   machine-specific absolute symlink autofetch deletes at configure → tree-drift),
   not capacity or a code failure — the local mac validation dies before posting
   its `macos` status, so the PR stays BLOCKED with the required check absent.
   Fixed at the repo level by PR #5588 (untracked + `.gitignore`d), but a
   pre-#5588 checkout keeps the stale looped symlink until it pulls main. Recovery
   + full mechanics: the **`skia-gpu-build`** skill's Gotchas section
   (`ln -sfn ~/.cache/pulp/skia-build/build <primary>/external/skia-build/build`).

**If you don't use Shipyard + the self-hosted Mac pool:** steps 1 and 3 are
specific to that setup (App-dispatched workflows; local runners) — skip them. The
tool-agnostic rule still holds: distinguish the *required* checks from *advisory*
lanes, and verify a runner is actually busy before blaming capacity.

**Caveat to step 3 — idle Studios do NOT prove the required gate is unblocked.**
The required `macos` check is gated by a routing preamble (`classify` →
`resolve-provider`) AND is *itself* an alias job that reports the leg's outcome —
all three default to `runs-on: ubuntu-latest`. So when GitHub's shared ubuntu
pool saturates (repo-wide `actions/runs?status=queued` in the hundreds during a
PR burst), the preamble can't get a slot, the macOS leg is never dispatched, and
the required gate sits `pending` *with the Studios idle*. Tell this apart from a
real failure and from a wedged runner: `queued` preamble + `online`/`busy=false`
Studios + high repo-wide queued count = **GitHub-hosted starvation**, not a code
or hardware problem. Immediate unblock is admin-merge after local validation
(`ghapp pr merge <n> --merge --admin`, `--merge` not `--squash` to preserve the
bump marker); merging also drains that PR's ~11-workflow fan-out from the queue.
The structural fix is the `PULP_PREAMBLE_RUNS_ON_JSON` repo var: set it to a
self-hosted Linux selector (e.g. `["self-hosted","Linux","ARM64",
"pulp-build-linux"]`) to move those three jobs off the saturated pool — but only
once that pool is confirmed always-on, or the gate just starves elsewhere. It
defaults to `ubuntu-latest` (no-op) until set. A tartci launchd detector watches
for this triad; full design in
`planning/2026-07-06-ci-queue-saturation-watchdog.md`.

**Caveat to step 3 (part 2) — a *different flaky test each re-run* is Studio
oversubscription, not a code bug.** The Studio runs up to `macos_vm_cap` (2, the
Apple guest limit) concurrent build VMs. RELATIVE-timing / CPU-budget / benchmark
tests (labels `performance`, `bench`, `quality-lab` — e.g. heritage-performance's
"Representative chain stays within the shipping CPU budget", a ratio ≤ 2.0×1.05
vs an in-run baseline) tolerate steady load but NOT the load *variance* a sibling
VM's bursty compile creates → they flake whenever 2 gate builds run at once. It is
NOT a real failure and re-running makes it worse (adds load). Those labels are now
excluded from the required PR/merge_group gate (build.yml `label_exclude`) — a
perf/ratio test cannot be a required gate on a cap=2 runner; it belongs in a
dedicated cap=1 nightly/perf lane. If you see one flaking on the gate, add its
label to that exclude, don't re-run. See `planning/org-flip-status.md` §A.

## A dead lane is only visible as queue age — never as a missing runner

`.github/workflows/runner-health-check.yml` sweeps every 30 min from
`ubuntu-latest` (off-fleet on purpose — a guard on the fleet dies with the
fleet) and opens/updates one tracking issue when a lane stops serving work. Read
its issue before hand-diagnosing a stuck PR: it names the labels the stalled
jobs asked for, which is the "which lane is sick" answer step 3 above otherwise
costs you a fleet probe to get.

**Do not "improve" this into a runner-label check.** The macOS lanes are
JIT/ephemeral: a runner registers with GitHub only while it serves a job.
`gh api .../actions/runners` returning zero runners for label `pulp-studio-01`
is therefore BOTH the healthy-idle state AND the dead-lane state — the two are
indistinguishable from GitHub's side. This is a trap that has already been
walked into: a label-satisfiability probe was recommended, built on, and used to
declare a perfectly healthy lane dead. An empty runner list at 3am is not
evidence of anything. Queue age is the only observable that separates alive from
dead, and it is cause-agnostic, so it catches failure modes nobody enumerated.

**The alarm needs two conditions, and the second one is the important one.** An
alarm requires age >= 45 min AND no sign of life on the lane (nothing comparable
`in_progress`, nothing comparable *started* since the job queued). Age alone is
not enough: the measured healthy baseline is median 5 min / oldest 31 min / 3
runs past 30 min, so a "queued > 30 min" rule fires every busy afternoon. It
also mis-reads one runner grinding a 90-min job with a queue behind it as death.
A busy runner is a live runner. If you retune the thresholds, do it in
`tools/scripts/queue_age_watchdog.py` — `test_queue_age_watchdog.py` pins the
measured baseline as a must-stay-quiet case and will fail a tuning that
re-introduces afternoon false alarms. Rationale + operator surface:
[docs/guides/local-ci.md](../../../docs/guides/local-ci.md) (the `config-doc`
gate maps the workflow and the script to that guide).

### Gotcha: a `*_RUNS_ON_JSON` variable read WITHOUT `fromJSON` becomes one literal label

Same silent-queue failure as below, but the black hole is created by the
*workflow*, not by fleet drift — so the topology checker cannot see it. The lane
reconciles green, every runner carries every contracted label, and the job still
queues forever.

The value of a `*_RUNS_ON_JSON` variable is a JSON **array**. Interpolating it
straight into `runs-on:` does not expand it; GitHub takes the whole string as a
**single label**:

```
labels: ["[\"self-hosted\",\"Linux\",\"X64\",\"pulp-build-linux-x64\",\"pulp-host-macpro\"]"]
```

No runner can ever carry a label whose text is a JSON array, so the job is
queued, not failed. Found live 2026-08-16: setting `PULP_LOCAL_LINUX_RUNS_ON_JSON`
left a dispatched `Enforce version & skill sync` job queued exactly like that
while an **idle** Mac Pro runner carried all five of those labels.
`version-skill-check.yml` and `vellum-freeze-check.yml` read the variable
directly; `build.yml`, `nightly-intel.yml`, and `examples-validation.yml` wrap it
in `fromJSON`. Both required gates are now wrapped too.

Two rules when touching a `runs-on` that references one of these variables:

- **Parse it.** `runs-on: ${{ fromJSON(...) }}`, never a bare interpolation.
- **Keep every branch JSON.** Inside `fromJSON(...)` a bare `'ubuntu-latest'`
  fails to parse and takes the gate down with it — write `'"ubuntu-latest"'`.

`tools/scripts/test_resolve_linux_route.py` enforces both: one test sweeps every
workflow for an unparsed selector, the other pins the JSON-quoted fallbacks.
Diagnose a suspected instance by reading the queued job's labels — a one-element
array containing bracket characters is conclusive:

```bash
ghapp api repos/Generous-Corp/pulp/actions/runs/<RUN>/jobs --jq '.jobs[]|{name,status,labels}'
```

### Gotcha: a lane pointed at a label NO runner carries is silent — and looks exactly like saturation

The trap behind step 3. Before concluding "the pool is saturated", check that the
lane can be served **at all**. GitHub does not validate `runs-on`: a job asking
for a label no runner carries is **not rejected, it is queued — forever**. No
error, no annotation, no failed check. The only symptom is jobs piling up while
the pool looks busy, which is indistinguishable from a genuine burst. So "18 runs
queued + every runner busy" is *not* evidence of saturation; it is equally
consistent with a lane routed into a black hole.

Found live 2026-07-16: `PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON` targeted
`["self-hosted","macOS","ARM64","pulp-build","pulp-build-vm"]`, and **zero
runners carried `pulp-build-vm`** — the intended Tart-VM topology had drifted to
bare-metal (`pulp-build-studio`) and the variable was never reconciled. The
relief valve had been routing into nothing for an unknown period, so the queue it
existed to drain simply grew behind it. A relief valve routed into a black hole
is worse than none: it reports healthy and relieves nothing.

Check it directly — a runner must carry **every** label in the array (subset
containment, not "any label overlaps"):

```bash
# Authoritative: reconciles every lane against the live fleet.
python3 tools/scripts/runner_topology_check.py --mode=report
```

Watch for three traps when reading this by hand:

- **Zero runners ≠ broken.** Tart runners register JIT/ephemeral and exist only
  while a job runs. An idle ephemeral lane has no registered runner and is
  perfectly healthy — the release lanes look "dead" between releases. Judge those
  on *service history* (did a job recently run with that exact label set?), not
  on the registry.
- **Offline ≠ absent.** A registered-but-offline runner may just be asleep (m1 is
  intermittent). A label *nothing owns* is always a black hole. Different
  failures; don't conflate them.
- **The fleet mutates while you look.** Runner count changed between two API
  calls during this investigation. Re-read before concluding.
- **Service history cannot expire — so it cannot prove a lane is alive NOW.**
  It is a claim about the past with no staleness notion: a lane served for weeks
  and dead for three hours still satisfies it. That is how the release lane
  reported "the provisioner is alive and idle" with two releases queued behind
  it. The only surface where a dead provisioner is visible *while it is
  happening* is the queue, so the checker also reads queued-job age
  (`queued_stall_seconds`, default 1800s) and reports `queue-stalled`. Read that
  finding as "work is arriving and nothing is answering it" — strictly stronger
  evidence than `black-hole`, which only says nothing has *arrived*. Note the
  signal is queue **age**, never queue presence: on a JIT lane the job queues
  first and the provisioner then boots, so "queued + no runner" is the ordinary
  transient and a presence-based check would fire on every burst.

Related instance of the same class: `build.yml`'s busy probe needs
`Administration: Read` to call `actions/runners`; the default `GITHUB_TOKEN`
lacks it, so the probe 403s and falls back to `BUSY=0` — **silently disabling
overflow**. Whenever routing "does nothing", suspect a silent read failure before
suspecting load.

The standing guard is `runner-topology-check.yml` (hourly, opens a tracking
issue) plus the `runner-topology-selftest` ctest. Lane→label intent lives in
`tools/scripts/runner_topology.json` — edit a routing variable and its lane
together, or the drift check fails. Full rationale:
`docs/guides/local-ci.md` → "Routing contract (checked)".

## A red advisory alias does not necessarily mean tests failed

`linux` and `windows` are **alias checks**: jobs that mirror real advisory
lanes. Native `macos` is not an alias; the macOS matrix child owns that required
context directly so advisory work cannot delay it. The macOS bootstrap jobs run
only for intentional native skips or fail-closed routing/classification errors.
Alias jobs run no build and produce no build output of their own — a `linux`
alias log is about 48 lines, and it says so outright:

```
Linux leg conclusion: cancelled
Linux leg cancelled — failing linux alias (advisory only; not required)
```

**An advisory alias exits non-zero on anything not green — including
`CANCELLED`.** So a cancelled leg surfaces on the alias as **FAILURE**, and a batch that got
interrupted produces a red alias beside its own `CANCELLED` entry in the same
check list. Reading that as a second, independent failure is the trap; it is one
event reported twice.

Before treating an advisory alias as a real break, fetch **the underlying leg's
job** (`Linux (x64) [github-hosted]`, for example) or the alias's own short log.
Do not infer from the alias name alone. A red direct `macos` context is the real
required macOS job (except the explicitly named bootstrap path) and must be
triaged as such.

Two more things that mislead here:

- **Neither `AddressSanitizer` nor `linux` is a required check.** Read the
  required list live from branch protection rather than from memory — a red
  advisory lane can sit beside a mergeable PR indefinitely.
- **An alias that is QUEUED is not evidence a stale failure was superseded.** It
  is evidence that nothing has run yet.

## Never wait on a signal you did not uniquely produce

Several agents share each dev Mac, and **the bounded-build rule is what makes
their build commands collide.** CLAUDE.md requires an explicit job count on every
build, so every agent emits a byte-identical `cmake --build build -j6`. The
discipline that prevents an OOM is the same discipline that guarantees the
command lines are indistinguishable — so this hazard gets *worse* the more
consistently everyone follows the rule, not better. Do not exempt your own
pattern on the grounds that it is precise.

```bash
# WRONG — fires when a STRANGER's build ends, in a different worktree.
until ! pgrep -f "cmake --build build -j6$"; do sleep 30; done
ctest --test-dir build          # …now running against a half-built tree
```

Seen 2026-08-01 on m3: the loop matched
`sh -c cmake --build build -j6 > /tmp/oklab-build-t5.log` from another lane. The
damage is not the early exit, it is what the early exit is fed into — **missing
binaries read as failures, or worse, stale binaries read as passes.** A partial
build in this repo exited 2 while the previously built test binary reported
`All tests passed (66 assertions)`.

The same defect has both signs, which is why they look unrelated and are not:

| symptom | cause |
|---|---|
| wait returns immediately | the pattern matches **nothing** — a typo, or the process was never named that |
| wait returns at the wrong time | the pattern matches **someone else's** identical command |

Both are a wait keyed on something that is not uniquely yours.

**Wait on an artifact only your own process can produce**: the completion marker
of a command you started (`…; echo "EXIT=$?" > mine.done`), or your own log's
mtime advancing. Same move as reading a build's own exit marker instead of a
waiter's exit code, and as deleting a test binary before rebuilding it so a run
cannot be served by an artifact the build did not produce.

`pkill`/`kill` need the same care with a worse failure: **resolve the PID by
working directory, not by name.**

```bash
for p in $(pgrep -f "cmake --build build"); do
    printf '%s -> %s\n' "$p" \
      "$(lsof -a -p "$p" -d cwd -Fn 2>/dev/null | grep '^n' | cut -c2-)"
done
```

That listing has shown **six** concurrent builds across worktrees on one host; a
name match would have killed someone else's.

**While `shipyard ship` is validating, treat the worktree as not yours.** The
local mac backend builds *in the checkout* rather than a copy, so a source edit
made while it runs — a mutation control, a quick experiment, a revert — can land
in the validation build, and committing moves HEAD underneath it. Run mutation
controls in a throwaway worktree, or wait for the lane to finish. Note that a
`shipyard` process sitting in the worktree is usually just waiting on GitHub;
confirm an actual compiler is running before concluding a build is in flight.

## Host-vitals preflight — back off before a saturating CI host reboots

The self-hosted Mac Studio that runs the required `macos` gate ALSO hosts the
interactive agent session and a heavy MCP stack (RepoPrompt, Figma,
chrome-devtools, several pulp-mcp). When RAM fills, macOS jetsam starts killing
processes, the window server crashes, and the host reboots uncleanly — taking any
in-flight required-gate CI job down with it and reddening the leg for reasons that
have nothing to do with the code (the 2026-07-01 incident). This whole class of
failure is **predictable ~20 minutes out** from cheap metrics, so the agent, the
CI pool, and Shipyard should all read one shared signal and back off.

`tools/scripts/host_vitals.sh` is that signal. It reports `green` / `warn` /
`critical` (exit `0` / `10` / `20`) keyed on **memory pressure first**
(`kern.memorystatus_vm_pressure_level` + fresh `JetsamEvent-*` reports), with load
only ever corroborating a warn — a healthy parallel build (load 1–2× cores, normal
pressure) never trips it. Use it before heavy work:

```bash
tools/scripts/host_vitals.sh            # one-line summary; exit code = level
tools/scripts/host_vitals.sh --json     # machine-readable
```

- **Agent admission control:** before launching a local build or a heavy MCP call
  on a CI-host session, check `host_vitals.sh`. If `critical`, don't pile on —
  ship via `shipyard pr` / GitHub-native auto-merge (survives a restart) instead
  of a foreground `shipyard`/`ci` watch, and shed idle load (close RepoPrompt/Figma
  /idle MCP) before building. `gates.sh` prints this banner advisorily on every
  pre-push.
- **A live validation build in your checkout is now visible — `gates.sh` says so.**
  Shipyard's `local` mac backend builds IN the checkout, so a validation run and
  an agent editing the tree share one directory. Editing under a running CMake
  does not fail cleanly: the build crawls and dies on the lane's `timeout_secs`,
  reporting `Validation timed out` — the target, not the cause. `governed-build.sh`
  writes `.pulp-build-active` at the source-tree root for the life of a build and
  `gates.sh` surfaces it advisorily on every pre-push. **Heed it before a merge,
  rebase, or branch switch** — a build dir can be wiped, a half-merged tree under
  a running CMake cannot be un-mutated. It is advisory and never fails a push, and
  because a push follows the damaging edit it *detects* rather than prevents; the
  value is knowing which edit invalidated which run instead of debugging a
  two-hour timeout. Distinct from `build-dir-sentinel.sh`, which guards the
  inverse invariant (do not reuse a dirty build dir).
- **A "hung"/"stuck" `git push` is almost always the pre-push diff-cover BUILD,
  not the network.** When the diff touches a coverage surface (`core/`,
  `tools/cli/`, `tools/scripts/`), `.githooks/pre-push` runs a full local
  configure + compile before the transfer. It prints a loud `DIFF-COVERAGE BUILD
  running` banner and a `…still running (Ns elapsed)` heartbeat every 30s so this
  is unmistakable — if you see those, it is a BUILD (minutes), NOT a transport
  hang. Tell: `git fetch`/`ls-remote` succeed while the push "hangs", so SSH/HTTPS
  are fine. Do NOT diagnose the network, and do NOT background-and-kill the push
  at a short timeout (that hides the banner/heartbeat). When the work is already
  validated (e.g. via `shipyard pr`), push with `PULP_SKIP_DIFF_COVER=1 git push`
  and it completes in seconds. (Learned 2026-07-07 after ~an hour lost mis-blaming
  MTU/SSH for a diff-cover build during a deps-bump push.)
- **Continuous sensor:** `tools/scripts/install_host_vitals_sensor.sh` installs a
  per-user launchd agent (`com.pulp.host-vitals`, 60 s) that publishes the latest
  reading to `~/.local/state/pulp/host_vitals.json` and a rotating
  `host_vitals.log`. It is observation-only — it never stops a runner or kills a
  process — so it is safe to run on the required-gate host. Installed on the m3/m5
  /m1 pool. `install_host_vitals_sensor.sh --status` shows the launchd + latest
  reading; `--uninstall` removes the agent.
- **A whole-pool-fails-at-once red leg is infra, not code** (per
  `macos-required-leg-timeout-saturation`): if `windows` + both `macos` legs fail
  together and the diff can't explain it, correlate against host reboot / jetsam
  time and **re-run the required leg** rather than debugging the change. Active
  back-off in the CI pool (tartci auto-yield) and Shipyard (host-health dispatch
  gate + infra-vs-code classification) consume this same `host_vitals` state.

## Fork PR routing is not a self-hosted trust boundary

`build.yml` currently blanks its self-hosted macOS selectors for a fork PR and
falls through to hosted `macos-15`. That is useful defense in depth for the
checked-in workflow, but it is not access control: pull-request workflow YAML is
part of the contributor-controlled merge commit and can remove its own guard.
Repo variables also resolve for fork runs.

The external trust boundary lives outside PR-controlled YAML. Pulp now ships two
distinct protected Proxmox provider roles in addition to the existing generic
repository-scoped pool:

- `pulp-trusted-ephemeral-pool@.service` loads
  `/etc/pulp/linux-trusted-runner-group.env`, selects the `trusted` policy,
  uses prefix `pulp-ci-ephemeral`, and adds `pulp-auto-linux-x64`. The live
  group must be exactly `pulp-trusted-build`, contain only
  `Generous-Corp/pulp`, and admit only protected-main `build.yml`,
  `pr-safe-linux.yml`, `vellum-freeze-check.yml`, and
  `version-skill-check.yml`.
- `pulp-pr-safe-ephemeral-pool@.service` loads
  `/etc/pulp/linux-pr-safe-runner-group.env`, selects `pr-safe`, uses prefix
  `pulp-pr-safe-ephemeral`, and adds `pulp-pr-safe-linux-x64`. Its exact
  `pulp-pr-safe-build` group admits only protected-main `pr-safe-linux.yml`
  for the same single repository.

Both roles fail closed unless `verify_linux_runner_group.py` proves the exact
name, repository, and selected workflows. Use a root-owned mode-0600
organization credential, or the exact root-owned non-group/world-writable
`/usr/local/bin/ghapp` helper. Capability labels without the verified
organization group are rejected.

Treat checked-out pull-request source as untrusted even when a protected
workflow orchestrates it. Runner inventory and cleanup must paginate the full
organization result set, reclaim only exact slot-scoped offline idle
registrations, and fail closed for online, busy, duplicate, or unknown states.
The organization runner group, registration, inventory, and deletion APIs need
the organization-capable controller credential; the repository runner token is
not that credential.

Protected clones require `configure-proxmox-ci-network.sh --apply` and
`--verify` before service activation. It creates no-uplink
`vmbr-ci200..202` bridges with controller `10.240.<VMID>.1/30`, guest
`10.240.<VMID>.2/30`, and source-scoped NAT while preserving `vmbr0`.
The supervisor proves controller-only SSH ingress, default-deny ingress,
private/reserved and IPv6 egress denial, and L2/IP source isolation before
registration. Stop the protected units and wait for their guests to disappear
before `--rollback`; rollback refuses attached bridges and restores the prior
forwarding state.

Do not alter the generic `pulp-ephemeral-pool@.service` contract: it remains
repository-scoped, uses the five generic labels and legacy network, and loads
only its optional per-slot `/etc/pulp/linux-runner-group-%i.env`. Retain
generic operator-dispatch capacity.

For another repository, use `proxmox-ephemeral-pool@.service` with a distinct
root-owned mode-0600 profile under `/etc/pulp/proxmox-runner/`. The profile must
set `TARTCI_RUNNER_REPO`, exact group id/name and protected-main workflow,
repository-specific labels and runner prefix, VM name prefix, golden, and a
disjoint VMID range, plus an explicit GitHub authentication mode and
repository-specific organization credential path. Generic profiles never
inherit Pulp's auth mode or credential paths; either omission fails before
provisioning.
`verify_linux_runner_group.py` then proves that the named
non-default group contains only that repository and selects exactly that
workflow. Cross-repository profiles cannot reuse `pulp-*` labels, and the Pulp
repository continues to accept only its built-in trusted or PR-safe policies.

The supervisor uses a generation-unique JIT registration, transfers the
mode-0600 encoded configuration over stdin, and bounds both GitHub-visible
readiness and broker heartbeat. Preserve the `/30` isolated bridge,
controller-only SSH ingress, L2 firewall proof, network-before-VMID lock order,
and generation-fenced deferred cleanup when extending this path. Enable a
repository's local selector only after a live eligible claim and exact
deregistration/VM/firewall teardown proof.

This is provider provisioning, not workflow routing. Keep protected PR and
merge-group Linux hosted until a separate routing change is reviewed and
enabled; an online role service is not authorization to set an automatic
selector. GitHub cannot retarget a queued local-label job, so hosted fallback
must be chosen before dispatch. Never route `pull_request_target` or
secret-bearing work to the generic pool. Exact installation, activation,
verification, and rollback commands live in `docs/guides/local-ci.md`.
The existing fork-routing regression test is defense in depth, not proof that a
runner is inaccessible to untrusted workflow revisions.
## Re-running a wedged required check

`macos` and `Enforce version & skill sync` can be re-dispatched
(`ghapp workflow run <workflow> --ref <branch>`). The two Vellum gates can be
recovered too — they take a `pr_number` input. The ordinary freeze gate uses a
separate hosted recovery workflow so the privileged dispatch event never
shares a workflow with an untrusted checkout:

```sh
ghapp workflow run vellum-freeze-recovery.yml --ref main -f pr_number=<N>
ghapp workflow run vellum-trusted-gate.yml  --ref main -f pr_number=<N>
```

Before that they declared only `pull_request(_target)` and `merge_group`, so a
wedged or cancelled run left a required check with no path back except pushing a
commit to fire `synchronize` — which rewrites the history under review to fix a
CI problem.

Both refuse a closed PR: the trusted gate posts a commit status, and putting a
fresh pending row on a merged PR helps nobody. The freeze recovery workflow is
checkout-free: it verifies the live PR base/source heads against an existing
restricted `pull_request` run and re-runs only that exact run. An in-progress or
already-successful run is refused, so recovery cannot create competing work.

The trusted gate also groups `pull_request_target` and manual dispatch runs by
PR number with `cancel-in-progress: false`. Editing a PR can fire repeatedly
without changing its head; GitHub therefore keeps the active required-check run
and only the newest pending run instead of accumulating one hosted job per edit.
Merge-group runs fall back to their queue ref, so separate queue entries never
share the PR group. Closed-PR `edited` events are filtered at the job boundary
and checked again by the resolver before checkout or commit-status mutation.
## Codecov "missing lines" is usually a leg that never uploaded

When Codecov shows fewer lines than the repo has, look for a coverage leg
that produced **no report at all** before suspecting the ignore list or the
flag/component config. The failure is quiet by construction:

- The build fails, so no instrumented binary is linked and no
  `coverage.cobertura.xml` is written.
- The upload step still runs. The Codecov CLI logs
  `Some files were not found` and returns **200** — `fail_ci_if_error` is
  `false`, so nothing turns red there.
- `after_n_builds` (4) then waits forever for an upload that will never
  arrive, so the report stays *incomplete* rather than visibly broken.

So the symptom appears in Codecov's UI while the cause is a compile error in
a workflow log. Check per-OS legs first:

```sh
gh run list --repo Generous-Corp/pulp --workflow coverage.yml --branch main --limit 10 \
  --json conclusion,headSha,createdAt
# then, on a failing run, find the step that actually failed:
gh api repos/Generous-Corp/pulp/actions/runs/<id>/jobs \
  --jq '.jobs[] | select(.conclusion=="failure") | "\(.name) :: \([.steps[]|select(.conclusion=="failure")|.name]|join(", "))"'
```

Concretely (2026-07-29): the Linux leg was missing `libfontconfig1-dev`.
Skia's `SkFontMgr_fontconfig.h` includes `<fontconfig/fontconfig.h>`, so
`core/canvas/src/sdf_atlas.cpp` failed to compile and the build exited 2.

**The underlying hazard is that thirteen workflows hand-maintain the same
Linux apt list with no shared source.** They drift silently, and a drift only
surfaces when some workflow happens to compile the translation unit that needs
the missing package. When adding a Linux dependency, grep the whole
`.github/workflows/` tree rather than editing the one lane in front of you:

```sh
grep -ln "libxkbcommon-dev" .github/workflows/*.yml   # every lane carrying the list
grep -L "libfontconfig1-dev" $(grep -ln "libxkbcommon-dev" .github/workflows/*.yml)
```

A run being `cancelled` on `main` is different and usually benign: coverage
sets `cancel-in-progress: false`, and GitHub queues only ONE run per group, so
a burst of merges replaces the queued run and only the latest head runs.

## An advisory lane can be red for days and still cost you merges

A red advisory check blocks nothing, so nobody acts on it — while it keeps
consuming the hosted-runner pool the *required* checks queue behind. The Tier-1
Rosetta lane sat red for seven consecutive runs at ~52 minutes of hosted macOS per
triggering PR, and the visible symptom was required checks waiting ~16 minutes for
a machine.

So when merges are slow, audit what advisory work is running and whether it is
even producing signal:

```sh
gh run list --repo Generous-Corp/pulp --workflow <name>.yml --limit 10 \
  --json conclusion,createdAt --jq '.[]|"\(.createdAt[11:16]) \(.conclusion)"'
```

An all-red history means the lane is spending capacity to tell you nothing.

Related trap: ctest label-exclusion lists drift between lanes. `build.yml` excludes
`validation|slow|windows-pr-quarantine|performance|bench|quality-lab` on PR runs;
the Rosetta lane excluded only `validation|slow`, which let wall-clock-budget tests
run under an emulator at a third of native speed. When adding a timing-sensitive
test or label, update every lane's `-LE`.
## Moving ADVISORY work is what shortens the required path

The instinct when merges are slow is to speed up the required checks. On Pulp the
required checks are mostly fast — they are *waiting*. Measured on one PR: three
required checks each sat ~16 minutes to do under 2 minutes of work, while advisory
lanes held the hosted pool.

So the lever is relocating advisory load, not optimising required jobs:

```
UndefinedBehaviorSanitizer (macOS)   81.6m wait + 75.4m run   advisory
x86_64 Rosetta                       43.3m wait + 51.7m run   advisory
Linux (x64)                          74.8m wait + 26.1m run   advisory
GCC compile (core, Linux)            63.2m wait + 11.3m run   advisory
Build + prove (wclap)                22.3m wait + 16.5m run   REQUIRED
```

**Route advisory lanes, never required ones, to home hardware.** `runs-on` has no
automatic fallback: once a variable points at a self-hosted pool, a power or ISP
outage makes jobs queue indefinitely rather than error. On an advisory lane that is
a slow check; on a required one it strands every merge, including for external
contributors.

Pattern for adding a routable lane — ship the plumbing inert, flip later:

```yaml
runs-on: ${{ fromJSON(vars.PULP_LOCAL_X_RUNS_ON_JSON || '"ubuntu-24.04"') }}
```

Declare it in `tools/scripts/runner_topology.json` **with `unset_fallback`** in the
same change, or the hourly topology check reports an unset lane as having no route
at all. Flip one variable at a time and watch a full cycle; rollback is unsetting
it.

The native Intel Mac mini follows this pattern through
`PULP_NATIVE_INTEL_RUNS_ON_JSON`. Its dedicated selector is
`self-hosted,macOS,X64,pulp-intel-native,pulp-host-macmini`, and the workflow
falls back to `macos-15-intel` while the variable is unset. The launchd
supervisor mints one JIT identity per job and cleans its exact work directory;
never add `pulp-build`, `pulp-build-vm`, or `pulp-gate-fast`, because those would
let advisory Intel work contend for the required ARM64 gate. FileVault requires
a human unlock after a cold power cycle; treat the lane as offline until login,
not as a reason to weaken disk encryption. Preflight verifies through GitHub's
organization API that the runner group contains only `Generous-Corp/pulp` and
is workflow-restricted only to `nightly-intel.yml@refs/heads/main`; a numeric
group ID by itself is not proof of an access boundary. The credentialed
controller and workflow process also use different OS accounts: `gh`/`ghapp`
stays in the login account, while `run.sh` executes as non-admin `pulp-ci`
through the root-owned fixed-operation worker. Never put GitHub auth in the
build account or let it modify the controller/worker; the one-time hidden
uid-499 account, Xcode, immutable runner/tool golden, protected read-only warm
ccache, shim, and sudoers setup is in `docs/guides/local-ci.md`. The worker must
give each job an ephemeral home/temp root, copy the golden, kill uid-owned
processes, and remove all uid-owned temp state every cycle; cleaning only
`_work` leaves attacker-modified runner executables behind. Ccache depend mode
stays off via `CCACHE_NODEPEND=1` per the decisions contract.
## Windows is nightly-only, and that is deliberate

Windows does not run on `pull_request` **or** `merge_group`. It runs on `schedule`
and `workflow_dispatch`.

Why: Windows is the single largest consumer of hosted runner *time* and **gates
nothing** — no Windows context appears in `main`'s required checks, so the merge
queue never waits for it. It was occupying the hosted pool the *required* checks
queue behind, and with `max_entries_to_build=2` it ran twice per cycle.

The argument is **minutes, not dollars** — the org's own July 2026 usage, which
corrects an earlier claim here that Windows was "~90% of billable spend":

| SKU | minutes | share of minutes | share of cost |
|---|---|---|---|
| Windows | 94,548 | 37% | 17% |
| Linux | 88,646 | 35% | 9% |
| macOS 3-core | 66,033 | 26% | **73%** |

macOS dominates *cost* (it bills at ~10x Linux per minute), Windows dominates
*occupancy*. Moving Windows off the per-merge path buys queue throughput, not a
smaller invoice — and the macOS gate is the thing to protect, precisely because
it is both the expensive lane and the only required one.

Coverage lives in `cross-platform-check.yml`: it builds and tests Windows nightly,
and its `tracking-issues` job find-or-creates a per-platform issue on failure,
reopens a closed one, and auto-closes on recovery. So a Windows regression becomes
a filed work item, not a queue tax.

Need Windows on a specific branch before the nightly:

```sh
ghapp workflow run build.yml --ref <branch>
```

**Before "fixing" this by putting Windows back on merge_group**, note the trade was
explicit: up to ~24 h of latency on a Windows regression, bought with merge-queue
capacity. Revisit only if Windows parity becomes an active workstream rather than a
background one.

Related, and rejected on measurement: moving the Ubuntu preamble jobs off the Macs.
`pulp-preamble-m5` and `pulp-studio-02` do also carry the `pulp-build` gate label,
so the starvation mechanism is real — but it is ~0.6 min of Mac time per run, and
the `linux`/`windows` alias jobs are `needs: build`, not pollers, so they never hold
a slot for a build's duration. Relocating them pushes four more jobs into the
contended hosted pool to reclaim half a minute.
## A cache that looks configured can be saving nothing

`actions/cache` reports success whether or not the path it was handed contains
anything. So a cache step can sit green for months while every build re-downloads
its dependencies. Verify the path a tool actually writes to, not the one the
workflow names.

`build.yml`'s FetchContent cache listed three paths and populated none:

```
Linux    cached ~/.cache/Pulp/...   CMake writes ~/.cache/pulp/...   (case)
Windows  cached ~/AppData/Local/Pulp/Cache/...   CMake uses $LOCALAPPDATA/Pulp/fc
macOS    path matched — but the sources never left <build>/_deps
```

The underlying cause is worth knowing before touching any of this:
`pulp_configure_fetchcontent_base_dir` in `tools/cmake/PulpFetchContent.cmake`
**returns early unless `WIN32`**. It is a MAX_PATH workaround for MSBuild, not a
cross-platform cache. Off Windows, `FETCHCONTENT_BASE_DIR` keeps its default of
`<build>/_deps`, so sources live inside the build tree and cannot survive it.

Cost when it is broken: **414 s cold configure vs 119 s warm**, measured on an
identical tree — roughly 295 s per clean build. three.js is the bulk of it, a
2.2 GB git clone fetched whenever `PULP_BUILD_TESTS` and `PULP_ENABLE_GPU` are both
ON, which is the default on `pull_request` and `merge_group`.

To check a cache is real rather than nominal:

```sh
# in the guest/runner, after a configure
du -sh "$FETCHCONTENT_BASE_DIR" 2>/dev/null || echo "nothing cached"
```

Same idea applies to the self-hosted golden images: bake with the flags CI actually
uses, or the golden warms a cache the real jobs never touch.

## Autonomous repair: the fences count, they do not judge

Every fence on the recovery lane bounds **how much** a repair changed — exact
head, epoch, fingerprint, force-with-lease, changed-file count, patch bytes,
control-plane path prefixes. On 2026-08-17 every one of them held while the
lane, asked to satisfy `static_assert(std::is_trivially_copyable_v<ParamValue>)`,
removed all five `std::atomic` members from `ParamValue` and rewrote its docs to
make thread-safety the caller's problem. `ParamValue` is the audio-thread to
UI-thread primitive; the repair introduced data races on the audio thread of a
real-time framework, and **it would have gone green**, because the assertion
passes once the atomics are gone.

`tools/scripts/shipyard_recovery_judgement.py` is the check that reads meaning.
Two independent tests, either of which escalates to `needs_human`:

- **Surface** — an ALLOWLIST (`test/` only today), not a denylist. A denylist
  fails open on the surface nobody named, and the dangerous surfaces are not
  only `core/`: a repair could make a failing gate pass by editing
  `.github/workflows/build.yml`, or disable a check in `tools/cmake`.
- **Invariant removal** — surface alone is insufficient. A test-only diff that
  merely deletes `REQUIRE` lines satisfies any surface rule and is the same
  failure: a model can make a check pass by making the claim true **or by
  deleting the claim**, and it chose to delete.

Two things to know before "fixing" it:

1. **It escalates the CORRECT repair too.** The right fix on 2026-08-17 was to
   delete the false assertion, and the invariant test refuses that as well.
   That is intended. The lane's job is to never land a catastrophic change
   unattended, not to land every correct one unattended.
2. **It is enforced in the publisher, not the worker.** The worker still
   uploads its artifact so an escalated repair stays inspectable; the publisher
   is the step that applies and pushes, so that is where refusing matters. On
   escalation it exits `3` (distinct from `1`, which means the check itself
   broke) and records the reason on the exact head as
   `shipyard/recovery-judgement`.

Like `shipyard_recovery_result_check.py`, it lives under
`tools/scripts/shipyard_recovery_`, which is in `FORBIDDEN_PREFIXES` in
`shipyard_recovery_repair.py` — so a fenced repair model cannot weaken the check
that constrains it. `test_shipyard_recovery_judgement.py` asserts that
containment rather than assuming it, and its central case is the verbatim diff
from `a08ec2d4abd8`.

## GitHub workflow gotchas

- **An `upload-artifact` with no `retention-days` inherits 90 days, and Actions
  storage is billed per ACCOUNT and shared across every repository in it.** That
  makes it the rare CI cost that becomes a *different repo's* outage: when the
  account's storage quota is exhausted, uploads start failing in repositories
  that never uploaded anything large, and the workflow whose artifacts filled it
  is not the one that goes red. Diagnosing from the failing repo alone leads
  nowhere, because nothing there is wrong.
  - **Read a storage-quota symptom as account-wide, not repo-local.** Check
    every repo in the account for large uncapped uploads, not just the one that
    failed.
  - **Cap by what the artifact is for, not uniformly.** A per-platform release
    binary that also ships as a GitHub Release asset is a job-to-job hand-off
    and wants days, not months. A failure-only fuzz reproducer wants the long
    retention, because a crash input that expires before anyone downloads it is a
    lost bug — and it costs almost nothing, since green runs upload nothing.
  - **Set the long retention explicitly when you mean it.** An inherited 90 and
    a deliberate 90 look identical in the YAML, so the next audit cannot tell a
    considered decision from a forgotten default and will "fix" it.
### Action major pins drift silently — the gate is a test, not a warning

GitHub retires the Node runtime under an action's *old* major and then forces
that major onto the newer runtime, emitting a deprecation annotation. That
annotation only exists on a live run, so a stale pin is invisible to every
local gate and is normally found by a human reading warnings. Pulp therefore
pins the major in one place and asserts it:

- `tools/scripts/test_workflow_lint.py::ActionMajorPinTests` scans every
  `.github/workflows/*.yml` and fails when any `actions/<name>@vN` falls below
  `MINIMUM_MAJOR`, or when one action is pinned to two different majors across
  workflows. The split-pin check is the one that catches real drift: a new
  workflow copies the current pin while the old ones keep the obsolete major.
- `actions/setup-python` is pinned at **v6**; v5 was forced onto Node 24 and
  began emitting a Node 20 deprecation annotation.
- Raise `MINIMUM_MAJOR` when bumping an action — do **not** add a second
  version assertion to an individual workflow's test. The per-workflow tests
  deliberately match `@v\d+` so a routine bump touches one invariant.

`actions/checkout`, `actions/cache`, `actions/setup-node`, and
`actions/upload-artifact` are still split across majors and are NOT yet under
this invariant; add them one action per PR so a runtime regression stays
bisectable.


- **A cache-save step gated on `github.event_name != 'pull_request' &&
  github.ref == 'refs/heads/main'` is dead code unless the workflow has a
  `push:` trigger.** GitHub's cloud cache scopes a PR-written entry to that
  PR's own ref, so PR runs can never warm each other and only a default-branch
  non-PR run publishes a shared entry. `build.yml` had that gate with no `push`
  trigger, so both `Save …` steps were unreachable and their `Restore …`
  partners were a permanent miss. Two traps when fixing this:

  1. **The base must be event-aware, or the fix no-ops.** `classify` diffs
     `<base>...HEAD`. On a push to main the checked-out HEAD *is* the new main
     tip, so `origin/main` resolves to HEAD and the diff is always empty. The
     classifier is fail-closed (empty ⇒ build), so this reads as "working"
     while never once distinguishing a docs merge from a core merge. Use
     `github.event.before` on push, `github.event.pull_request.base.sha` on PR
     — `tools/scripts/resolve_classify_base.py` owns that mapping, including
     the all-zero-sha fallback that `event.before` carries when a push creates
     a ref.
  2. **Never let the new trigger schedule work on the self-hosted Macs.** They
     serve the one required check and keep ccache + FetchContent on local disk,
     so a macOS leg on a push burns required-gate capacity to upload nothing.
     `resolve-provider` omits the macOS leg for push events; the aliases and
     `windows-*-gate` jobs skip too (otherwise a terminal reporter would search
     for a matrix leg that deliberately was not created). Scope saves to
     `runner.environment == 'github-hosted' && runner.os != 'macOS'` — the
     restore side's `runner.os != 'macOS' || …` disjunction is wrong here, it
     re-admits self-hosted non-macOS runners.

  Push runs are also exempt from `cancel-in-progress` (they share the
  `refs/heads/main` group; cancelling one discards the cache it exists to
  publish). Covered by `tools/scripts/test_resolve_classify_base.py`.

- **Every `on: schedule` workflow must carry the fork guard.** When someone forks
  the repo, GitHub copies all workflows and runs the scheduled ones on the fork's
  default branch — then emails the fork owner whenever one *fails*, which our
  monitors reliably do on a fork (they probe this repo's state or use secrets the
  fork lacks). So each entry job (a job with no `needs:` — dependents cascade-skip)
  of a scheduled workflow gets:

  ```yaml
  jobs:
    check:
      if: github.event_name != 'schedule' || github.repository == 'Generous-Corp/pulp'
  ```

  compose it with an existing condition as
  `if: (github.event_name != 'schedule' || github.repository == 'Generous-Corp/pulp') && (<existing>)`.
  It only suppresses the **schedule** event on forks — `push` / `pull_request` /
  `workflow_dispatch` are untouched, so PRs to this repo (which run in this repo's
  context) and manual dispatches behave exactly as before. A workflow that
  *should* run on forks' schedules opts out with a top-of-file
  `# fork-guard-exempt: <reason>` comment. This is **enforced**:
  `tools/scripts/scheduled_workflow_fork_guard_check.py` runs in `gates.sh`, the
  pre-push hook, and `workflow-lint.yml`, so a new scheduled workflow missing the
  guard fails the PR. Add the guard when you add the workflow.
- **Codecov "total lines/files dropped" is usually upload starvation, not config drift.** Three guard layers catch different failures: `test_coverage_surface_contract.py` plus the Codecov config tests guard the semantic producer/component inventory; semantic verifiers plus `.github/actions/upload-codecov-report` reject missing/empty inputs and emit a receipt only after Codecov transport succeeds; `coverage-upload-watchdog.yml` treats main as fresh only when one run has exact Linux, macOS, Python, Apple, Android, and React receipts that Codecov reports merged for that exact run. Windows is inventoried and monitored but remains best-effort while its bounded suite can exceed the cap. This catches a report failure, transport failure, cancellation, or Codecov processing stall without putting coverage in protected main's required-check set. When triaging, inspect the semantic manifest, exact receipts, and Codecov upload records before changing `codecov.yml`.
- **Parallel source coverage needs both an LLVM merge pool and a tolerant final merge.** `LLVM_PROFILE_FILE=...%Nm...` must use a pool at least as large as bounded CTest parallelism; plain `%m` is a one-file pool and parallel Linux exits have corrupted its header. A killed/timed-out test can still truncate one pooled shard, so both hosted and local/SSH scripts merge with `llvm-profdata --failure-mode=all` but fail closed when invalid shards exceed both 25 files and 5% of the pool. Do not revert either half to a single `%m` file or default `failure-mode=any`: one bad shard then blackholes every otherwise-valid report.
- **Native Linux apt dependencies have one owner.** Workflows that compile native Pulp use `.github/actions/install-linux-build-deps`, backed by `tools/ci/install_linux_build_deps.py` and capability profiles in `tools/ci/linux_build_deps.json`. Toolchains and lane-specific utilities are explicit `extra-packages`; do not add a workflow-named profile. `linux_build_deps_workflows.json` enumerates adopters and intentional direct-apt exclusions, and `test_install_linux_build_deps.py` fails workflow-lint when a new apt workflow is unclassified or an adopter copies canonical packages. Add a shared native dependency to the manifest once; add a one-lane tool at that lane's action call.
- **`control-shipping-native.yml` proves real installed-SDK artifacts.** Its
  path-scoped, advisory four-leg matrix builds production-stripped plug-ins on
  macOS universal, Linux x64/arm64, and Windows x64, then preserves and
  consolidates canonical scanner evidence. AAX proof is allowed only on a
  protected-`main` push with the external SDK secret; PRs must report it as
  unavailable rather than weakening the native matrix. Keep
  `test_control_shipping_native_workflow.py` and
  `test_verify_control_shipping_native_evidence.py` wired into
  `workflow-lint.yml` when changing this contract.
- **`web-plugins.yml` is the headless-browser web lane (advisory).** It builds
  Pulp's WAMv2 (Emscripten) and WebCLAP (wasi-sdk) web plugin formats on a Linux
  GitHub-hosted runner and runs every web validation, including the browser
  fixtures in headless Chrome (`browser-actions/setup-chrome` +
  `playwright-core`, which drives the system Chrome — no browser download). It is
  deliberately NOT on the self-hosted VMs: a headless-Linux browser lane needs
  emsdk + wasi-sdk + Chrome, all of which install cleanly on GitHub-hosted, so no
  golden VM or QEMU work is warranted. The WAM build vendors choc by cloning the
  pinned fork (PulpWam.cmake needs `PULP_WAM_CHOC_INCLUDE`; the WebCLAP build
  FetchContents choc/clap itself). The browser drivers are
  `examples/web-demos/*/{browser-test,browser-host}/validate.mjs`; run them
  locally with a system Chrome/Canary via `node validate.mjs --screenshot out.png`
  (set `CHROME_PATH` or pass `--browser`). The WAM node validations also cover
  the in-worklet **rack** path (`wam_rack_runner.mjs` against the
  `PulpPluckGainRack` target — the only committed consumer that compiles
  `wam_chain_entry.cpp`/`WamChainBridge`), the wire protocol + version-skew
  bridge contract (`core/format/src/wasm/wam-runtime.test.mjs`, pure JS), and
  hostile-state rejection (overflow/CRC/truncation) folded into
  `wam_feature_runner.mjs` — so a regression in the state codec or chain runtime
  fails here rather than only in a browser.
- **`web-plugins.yml` also hosts a non-browser job: `Timeline fixture corpus
  (WASM)`.** It builds `pulp-fixture-runner` through the Emscripten-only root
  `core/interchange/wasm` and runs the timeline conformance corpus under node
  (`tools/ci/wasm-fixture-lane.sh`). It lives in this file only to share the
  emsdk pin — it needs no Skia slice, no Chrome, no wasi-sdk, no npm, and
  finishes in about a minute, so it is a sibling JOB and not a step in the lane
  above. When adding another emsdk-only check, follow that shape rather than
  appending to the browser lane. The lane script runs the corpus twice on
  purpose (good corpus green, broken copy red); if you ever "simplify" it to one
  run, you have deleted the only thing proving the wasm build validates
  anything.
- **The GPU-audio proof inside `web-plugins.yml` needs a real GPU, so it is a
  macOS job.** The Linux leg has **no WebGPU adapter at all** (measured:
  `--use-webgpu-adapter=swiftshader` and `forceFallbackAdapter:true` both return
  null), so `validate-gpu.mjs` there **skips with a named reason and exits 0** —
  and a non-gating probe step prints what `requestAdapter()` actually returns, so
  "does Linux CI have a software adapter yet?" stays a measured question rather
  than an assumption. The real gate runs on macOS with `PULP_REQUIRE_WEBGPU=1`,
  which turns "no adapter" from a skip into a failure. If you see the GPU-audio
  proof "passing" on Linux, it did not run.
  Two traps when editing that job: `pulp_add_wclap(Foo)` declares the CMake target
  as **`Foo-wclap`** (the bare name is a hard "No rule to make target"), and the
  emsdk `llvm-nm` must be on `PATH` or `verify_wasm_skia_slice.py` — which exits 77
  to mean CTest-SKIP — kills the step under `set -e` instead of skipping.
- **`wclap-cloudflare.yml` is WebCLAP's canonical-host lane (Cloudflare Pages).**
  Separate from `web-plugins.yml`: it builds the threaded WebCLAP PulpGain module
  (pinned wasi-sdk), assembles a self-contained deploy dir, proves it headlessly
  under the real `_headers`, then owner-gated-deploys. Two pages are assembled +
  proven: the single-plugin isolation page (`assemble.mjs` → `validate-deploy.mjs`)
  and the **shared-player** WebCLAP demo (`assemble-player.mjs` → `player/` →
  `validate-player.mjs`) — the latter mounts the SAME `@danielraffel/web-player` shell the
  WAM demos use, driven by the WebCLAP adapter (a worklet-resident CLAP host), and
  asserts crossOriginIsolated, real-time render (worklet quanta ≈ `sampleRate/128`
  per wall-second), an audible param change that updates the shared widget, and a
  `clap.state` round-trip. The lane also runs the `@danielraffel/web-player` adapter unit
  tests (`packages/pulp-web-player` — `npm test`), which include an ABI-parity
  check that the worklet's inlined CLAP struct offsets match `wclap-abi.mjs`.
  Hard-won gotchas encoded there: posting a `WebAssembly.Module` INTO an
  AudioWorklet is silently dropped in Chrome (transfer the raw bytes and compile
  in-worklet), and transferring an `ArrayBuffer` OUT of an AudioWorklet is
  unreliable (the receiver gets a detached buffer) — clone small payloads (state,
  sysex) instead, and never transfer a caller-owned buffer (it detaches theirs).
  **It also needs TWO EXTERNAL CHECKOUTS, and forgetting them is a silent build
  with a loud, misleading failure.** The gallery's 23 plugins do not live in pulp —
  they are in the public `danielraffel/pulp-example-plugins` and
  `danielraffel/pulp-classic-effects` repos (the examples-out-of-core split), and
  `wclap-build/CMakeLists.txt` declares each gallery target ONLY IF its plugin
  header is present, skipping it quietly otherwise (deliberately — that is what
  keeps the in-repo build green without the externals). So a job that checks out
  pulp alone builds exactly PulpGain, skips all 23 without a word, and then dies
  hundreds of lines later in `assemble-gallery.mjs` with *"missing wasm for
  example-plugins/mono-synth"* — which reads like an assembler bug and is not one.
  The two roots default to SIBLINGS of the pulp checkout, which `actions/checkout`
  cannot produce (a `path:` cannot escape the workspace), so clone them under
  `_ext/` and pass `-DPULP_EXAMPLE_PLUGINS_DIR` / `-DPULP_CLASSIC_EFFECTS_DIR`.
  The lane now asserts a couple of gallery wasms exist right after the build, so
  the cause is reported where it happens rather than downstream.
  Because this is a required context, the workflow still starts on every PR;
  `webclap_relevance.py` only skips the expensive setup/build/proof steps for an
  unrelated diff. Relevance is evaluated with the classifier fetched from the
  trusted base SHA and includes both names of renamed files. Pull requests use
  the files API and fail closed at its 3,000-file cap; merge groups diff the
  complete speculative queue head against `merge_group.base_sha`, so an
  unrelated queued group can take the same fast path without dropping the
  required context. A missing base, failed fetch/diff, or workflow/classifier
  self-change still runs the full proof. Never expose `GH_TOKEN` to the
  PR-controlled checkout while resolving relevance. The same protected-base
  generated-version-bump verifier runs before ordinary WebCLAP relevance. It
  may fast-green an exact release-bot bump because only version scalars changed;
  branch names, labels, commit subjects, or path lists alone are never
  sufficient. The verifier itself is trusted code and is the only process in
  that decision path that receives `GH_TOKEN`.
- **`screenshot-sync` is a three-layer gate that mirrors skill-sync.** A repo opts
  in by committing a `.pulp/screenshots.toml` manifest (presence == opt-in);
  `tools/scripts/screenshot_sync_check.py` then diffs the manifest's `[trigger].paths`
  and fails when a triggered target's committed PNG/OG image wasn't refreshed.
  Wired identically to skill-sync: PostToolUse hint in
  `hooks/scripts/cli-plugin-sync.sh`, pre-push report in `.githooks/pre-push`, and
  authoritative CI step in `version-skill-check.yml`. Bypass a single commit with a
  `Screenshot-Sync: skip target=<id|all> reason="..."` trailer. Pulp core itself is
  NOT opted in (no manifest), so the gate no-ops here; it exists for downstream
  plugin repos (GPU NAM, example plugins, Bendr) and the WCLAP/WAM OG images.
- **Android native `.cxx` caches must be dependency-aware.** The Android workflow
  builds through Gradle's external native build, and `android/app/.cxx` can hold
  FetchContent checkouts under `_deps`. Do not cache `.cxx` under a Gradle-only
  key or a broad Gradle restore key: after a native dependency bump, CMake's
  `UPDATE_DISCONNECTED` path can see the stale local checkout and fail because
  the new git ref is not present. Keep the Gradle cache (`~/.gradle`) separate
  from the native CMake cache, key `.cxx` on the native dependency inputs
  (`CMakeLists.txt`, `tools/cmake/PulpAndroid.cmake`,
  `tools/cmake/PulpDependencies.cmake`, `tools/deps/manifest.json`, plus Android
  Gradle files), and do not give `.cxx` a restore key that ignores those inputs.
- **`version-at-land.yml` + `version_at_land.py` are the single-writer,
  post-merge half of the version-bump intent-trailer model, and the workflow
  runs LIVE (`--push`).** They exist to kill the version-bump merge treadmill
  (PRs editing `CMakeLists` VERSION / `plugin.json` / `marketplace.json`
  re-conflict every time main advances, and N parallel PRs endlessly re-bump the
  same shared counter). A PR declares `Version-Bump: <surface>=<level>` (or the
  level is inferred from its paths / conventional-commit subject) and touches NO
  version files; this bot assigns the exact number AFTER merge from main's
  current version — so no two PRs ever contend for the same number.
  - **Landing route — `--route {direct,pr}`, selected by the `PULP_BUMP_ROUTE`
    repo variable (unset ⇒ `direct`).**
    - `direct` (default, live today): `apply_and_push` pushes the
      `chore: bump versions` commit straight to `main` with `--ff-only`. This is
      what publishes releases now. It is INCOMPATIBLE with a "Require merge
      queue" branch rule (the rule blocks all direct pushes to main — that
      incompatibility caused the 2026-07-20 release drought).
    - `pr` (dormant until flipped): `apply_via_pr` opens a `chore: bump versions`
      PR on the fixed `release/version-bump` branch and arms
      `gh pr merge --auto --merge`, so the bump lands THROUGH the merge queue.
      Requires the `RELEASE_BOT_TOKEN` PAT (a GITHUB_TOKEN-created PR does not
      trigger checks). When `PULP_BUMP_ROUTE=pr`, the workflow's `concurrency`
      group becomes a single constant so all drains SERIALIZE (the PR-route's
      shared-branch reclaim is only race-free without a competing drain). Plan +
      rollout + validation evidence: `planning/2026-07-20-merge-queue-reenable-plan.md`.
      This is the path back to the merge queue we moved to an org for.
  - **Intent is read `--no-merges`-scoped.** `version_at_land.intent_trailers`
    reads `Version-Bump:` trailers only from the range's NON-merge commits
    (`git_range_trailers(..., no_merges=True)`). A "Merge origin/main into
    <branch>" re-sync commit can carry a stale intent trailer that was never
    meant to declare this range's release; honoring it would silently escalate
    the version. A PR's real intent lives on its own commits (or the squash
    commit, single-parent), so this keeps every genuine declaration while
    dropping re-sync noise. Do NOT change `git_range_trailers`' default
    (merge-walking) — the bypass-trailer gates depend on it; use the opt-in
    `no_merges=` flag.
  - **The `--push` transaction is race-safe by construction:** it recomputes
    from a fresh `origin/main` each attempt, pushes with no `--force` (git's
    default non-fast-forward rejection IS the `--ff-only` guarantee), and on
    rejection re-syncs to the new tip — where the drain range now starts after
    the winner's `Version-Bump-Applied` marker and collapses to empty, so the
    loser no-ops instead of double-bumping. **This is why the older
    `intent-bump-on-merge.yml` was DELETED:** it did a bare
    `git push origin HEAD:main` with no `--ff-only` + retry, so a second merge
    during its ~30s window silently discarded the bump (a SILENT RELEASE LOSS).
    Never reintroduce a force/unguarded push on this path.
  - **Before the `--push` flip** (a separate, reviewed change — see
    `docs/guides/version-at-land-cutover.md`): verify the release bot can push a
    *commit* to protected `main` (it already pushes tags from
    `auto-release.yml`; a commit needs the bot on the branch-protection bypass
    list, and the bot commit must be SSH-signed via
    `configure_release_bot_ssh_signing.sh`), that the `Version-Bump:` trailer
    survives squash-merge into main's message, and that the one-time in-flight
    straggler rule (PRs already carrying `chore: bump versions` commits) is
    applied. The `chore: bump versions` commit the bot pushes triggers
    `auto-release.yml` exactly like a PR-side bump.
- **`test/CMakeLists.txt` is now an include hub, not a registration
  manifest.** Add new `add_test`, `add_executable`, and
  `pulp_add_test_suite` blocks to the matching `test/cmake/*_tests.cmake`
  manifest instead of rebuilding the old monolith. If no existing manifest
  owns the subsystem, create a focused new `test/cmake/<area>_tests.cmake`
  file and include it from `test/CMakeLists.txt` in dependency order. Focused
  owner hubs may include smaller manifests for their own subsystem, but do not
  hide sibling manifests inside an unrelated owner just to keep the top-level
  file short. Keep the top-level `hotspot_size_guard.json` ceiling at the exact
  tiny hub LOC; raising it for ordinary test additions is a regression. Before
  burning a PR CI job on a manifest-only cleanup, run the local preflight:
  `python3 tools/scripts/docs_noise_lint.py --mode=report --base origin/main`,
  `python3 tools/scripts/hotspot_size_guard.py --base origin/main --config
  tools/scripts/hotspot_size_guard.json --mode=report
  --require-ceiling-reduction`, and a clean CMake configure/target-list compare
  against `origin/main`.
- **Source hotspots (e.g. `core/view/src/design_cpp_codegen.cpp`) are frozen
  too — bump the ceiling for a *coherent* feature, split when it's accretion.**
  `hotspot_size_guard.json` also freezes large source files. When a single,
  cohesive feature legitimately grows one (e.g. teaching the C++ codegen to emit
  `faithful_svg` as a `DesignFrameView`), raise that file's `max_loc` in the same
  PR — splitting a tightly-coupled emitter mid-feature would hurt readability
  more than the extra LOC. Reserve the split for genuine grab-bag growth.
  When a PR shrinks a tracked hotspot, lower that file's `max_loc` in
  `hotspot_size_guard.json` to the new exact LOC in the same PR; the
  `--require-ceiling-reduction` gate compares the merge-base blob against `HEAD`
  for branch-touched tracked hotspots, so leaving headroom after a shrink fails
  the pre-push/CI guard.
  **Exception — a hotspot already ABOVE its ceiling.** `check_hotspots` only
  fails the PR that itself grew a file, so `main` can drift past a `max_loc`
  while every individual PR stays net-neutral (e.g. `window_host_mac.mm` at 2780
  LOC against a 2770 ceiling). Shrinking such a file cannot lower the ceiling —
  setting `max_loc` to the new LOC would *raise* it — so the guard does not ask
  for a reduction there, and prints `still at/over ceiling; no reduction to
  make`. The ratchet is demanded only when the shrink lands strictly under the
  existing ceiling. If you see the guard demand a `max_loc` larger than the one
  in the config, that is the bug this rule fixed, not a config you should edit.
  Editing `hotspot_size_guard.json` itself trips the `ci` skill-sync gate; the
  ceiling bump is normally part of a non-`ci` feature PR, so either fold this
  note's rationale into that PR or skip the `ci` gate with a
  `Skill-Update: skip skill=ci reason="ceiling bump only"` trailer on the **tip**
  commit (note: a later `chore: bump versions` commit from `shipyard pr` displaces
  the tip, so updating this SKILL is the more robust path).
- **Inspector hotspots are frozen too.** `hotspot_size_guard.json` watches newly
  added `inspect/**` files and freezes the current inspector overlay, window,
  domain handler, and tweak-store hotspots. When an inspector extraction shrinks
  a tracked file, lower its `max_loc` to the exact new line count in the same
  change. When a new overlay companion file triggers the large-file warning,
  split it before it becomes another hotspot.
- **Modulation toolkit tests have explicit owners.** The contract, sources,
  tools, events, and voice/composition suites are separate frozen hotspots in
  `hotspot_size_guard.json`. Put new coverage in the matching owner and extract
  another behavioral domain before growing one past its recorded ceiling.
- **Reskinnability ratchet (`token-coverage-ratchet` ctest).** Driven by
  `tools/scripts/token_coverage_check.py`: fails if a `core/view/src` paint file
  gains a NEW colour literal that is not a `resolve_color(...)` fallback. Mark a
  deliberate material-effect literal `// token-lint:allow`; after intentionally
  lowering a file's count, re-freeze with `--update-baseline`.
- **SignalGraph single-backend governance (`single_backend_guard.py` +
  `processing_model_terms_lint.py`).** Whole-tree structural guards (not
  diff-scoped) that keep the DSP runtime convergence from regrowing a second
  surface: exactly one type (`GraphRuntimeExecutor`) may define
  `process_routed` / `process_parallel`; `pulp create` offers no graph-plugin
  authoring scaffold; the public generated-DSP ABI entry symbols stay the
  sanctioned pair (`pulp_native_core_entry_v1`, `pulp_node_v1_entry`); and the
  differential parity test stays registered as a built target. They run in
  `gates.sh`, the pre-push hook, and the `Versioning & Skill-Sync` workflow
  (hard-fail). To sanction a genuinely-new routing engine, authoring template,
  or generated-DSP ABI, widen the allowlist in `single_backend_guard.py` in the
  same PR — that diff is the architecture-review record. Both guards carry a
  `--selftest` run in the workflow's fixture-test step. Hardening notes (from the
  series adversarial review): a non-sanctioned `Rival::process_routed` is flagged
  even when defined *inside* `graph_runtime_executor.{hpp,cpp}` (no TU-filename
  escape), and the parity-registration check requires the test source to appear on
  a real `SOURCES`/`add_executable`/`target_sources`/`pulp_add_test_suite` line —
  a bare mention in a dead variable no longer counts.
- **Skill-path-map lint (`skill_path_map_lint.py`).** Checks the map that
  skill-sync reads. A pattern in `skill_path_map.json` that matches nothing does
  not report a problem — it reports nothing, which reads exactly like a clean
  run, so three dead patterns sat on main unnoticed while the file's own contract
  test (`test_skill_path_map.py`) was red *and imported by no CI entrypoint*.
  Four rules: `schema` (validates against `skill_path_map.schema.json`, which
  previously did not exist despite the map's `"$schema"` pointer — a bare-array
  entry, unknown key, or malformed pattern is now rejected); `submodule` (a
  pattern rooted in a git submodule can never fire, because the superproject's
  diff carries the `planning` gitlink and never a path beneath it — no
  annotation unlocks this); `empty` (a pattern matching no tracked file, unless
  the entry carries `_doc.empty-ok`, which is restricted to `external/` SDK paths
  and to entries claiming no paths at all); and `co-claim` (diff-scoped —
  *newly* claiming a whole `<root>/<sub>/**` subsystem another skill already
  claims needs `_doc.scope`, because otherwise every edit under it demands
  several SKILL.md updates, which trains reflexive `Skill-Update: skip`
  trailers). The first three are whole-tree, since the usual cause is the tree
  moving out from under a pattern nobody edited. Runs in `gates.sh`, the pre-push
  hook, and the `Versioning & Skill-Sync` workflow. Its schema is enforced by
  `json_schema_lite.py`, a stdlib validator whose defining property is that an
  unimplemented keyword *raises* rather than being skipped — so no schema
  constraint can be silently unchecked. **Adding a gate-script test file is not
  enough: `test_gates.py` must import its `TestCase`, or it runs nowhere.**
- **Yoga oracle/pin lockstep (`check_yoga_oracle_pin.py`).** Whole-tree
  invariant (not diff-scoped). The web-compat harness decides scope by looking a
  CSS property up **by name** in `tools/harness/oracles/yoga/yoga-supported.json`
  — a table hand-transcribed from one Yoga release — and
  `tools/harness/adapters/yoga.py` reports anything absent from it as
  `Status.OOS`, "out-of-scope", not as an uncovered gap. Yoga itself is pinned
  independently in `tools/cmake/PulpDependencies.cmake`. Bumping the pin without
  re-transcribing the table therefore reclassifies every property and enum value
  the new Yoga gained as out of scope, and compat coverage *improves* because the
  measurement went blind — the worst gate failure shape there is. The check reads
  both ends and fails when they disagree. It also rejects the two cmake pin sites
  (`pulp_register_fetchcontent_source ... REF` and the `FetchContent_Declare`
  `GIT_TAG`) disagreeing with each other, and a missing or uncited version stamp,
  so it cannot pass vacuously: exit `1` is drift, exit `2` is an unparseable or
  absent stamp. Runs in `gates.sh`, in the pre-push hook, and as the
  `yoga-oracle-pin-lockstep` ctest (with `yoga-oracle-pin-lockstep-selftest`
  covering the checker itself). The ctest is the authoritative lane: the
  pre-push hook shares `run_gate_captured`'s exit `2` with its own
  capture-directory error, so — like every other gate there — it treats `2` as
  an advisory internal error rather than a hard fail. Fixing it means restamping the oracle's `version`
  and its `source` citation against the new `facebook/yoga@<ref>` YGEnums.h
  *and* re-reading the property table — restamping alone re-hides the gap.
- **Conflict-marker guard (`conflict_marker_check.py`).** Whole-tree guard (not
  diff-scoped): no tracked file may carry a git conflict marker. Born from the
  incident where a squash-merge's stale side collided with an already-advanced
  `project() VERSION` line and wrote `<<<<<<< / ======= / >>>>>>>` straight into
  `CMakeLists.txt` (fixed in #5477), breaking every build until a human noticed.
  Keyed on the start/base/end markers (`<<<<<<<` / `|||||||` / `>>>>>>>` at
  column 0, followed by whitespace/EOL) — verified zero-false-positive across the
  whole tracked tree, `external/` included; the `=======` separator is reported
  only inside a real conflict block, so Markdown headings and ASCII banners stay
  clean. Runs in three layers: `gates.sh` + the pre-push hook (local), the
  `Versioning & Skill-Sync` workflow's **Conflict-marker guard** step (which scans
  the pull_request MERGE ref, so a marker born from the merge itself is caught,
  not only one on the PR head), and `conflict-marker-guard.yml` — a `push:main`
  backstop that reddens the branch and opens a tracking issue if a marker reaches
  main by any path (the squash case, where GitHub had no clean mergeable ref for
  the PR job to inspect). Carries a `--selftest` in the fixture-test step. A
  vendored fixture that legitimately ships markers is a reviewable `ALLOWLIST`
  edit in the script. If the `push:main` guard reddens main, run
  `python3 tools/scripts/conflict_marker_check.py` for the file:line list, resolve,
  and push — the tracker auto-closes on the next clean push. Exit codes are
  meaningful: `0` clean, `1` markers found (the backstop opens the tracker), `2+`
  internal error (the backstop fails the run *without* opening a wrong "markers
  committed" issue) — the selftest locks this contract. Documented scope
  limitations (deliberate, to keep zero false positives): only default
  seven-char markers (a non-default `.gitattributes` `conflict-marker-size` is
  missed), submodule *contents* are out of scope (the superproject sees a
  gitlink), and NUL-bearing/UTF-16 text is skipped as binary. The upstream fix
  for the whole class — the merge tool refusing to commit a conflicted result —
  is tracked in Shipyard #372; this guard is the consumer-side backstop.
- **Thread-safe-assertions guard (`thread_assert_check.py`).** Catch2 3.7.1's
  assertion macros are NOT thread-safe (thread-safe assertions are opt-in only
  from Catch2 3.9.0), so a `REQUIRE`/`CHECK`/`FAIL`/`SUCCEED` inside a
  `std::thread` / `std::jthread` / `std::async` lambda in `test/` is undefined
  behavior: it corrupts Catch's per-run assertion counters + section state. Bare
  metal tolerated it, but the move of the required macOS gate onto Tart VMs made
  the different scheduler timing trip it — a HotSwapSlot hammer test "failed" in
  0.00s though the code under test was race-free by design. The correct pattern:
  record the violation into an `atomic`/guarded value in the worker, `join()`,
  then assert on the test thread (`REQUIRE_FALSE(bad.load())`). The lint is a
  lexical scan (best-effort: it does not follow calls into helpers) with a
  same-length string/comment-blanking pass that also skips C++ digit separators
  (`10'000`); suppress a verified-safe line with a trailing
  `// thread-assert:allow`. Runs as the `thread-safe-assertions` ctest case and
  in `gates.sh`. When graduating any required lane to VMs, expect this class of
  latent UB to surface — fix at the source, don't suppress.
- **Release builds must pass `-DPULP_BUILD_EXAMPLES=OFF`.** The
  `pulp-design-tool` example hard-fails CMake configure when `PULP_HAS_SKIA`
  is FALSE (belt-and-suspenders, code 78). `sign-and-release.yml` builds on a
  GitHub-hosted macOS runner with no Skia, so configuring the full tree
  (examples included) aborts the entire run → **no GitHub Release publishes**
  even when `release-cli.yml` (the CLI build) is green. This silently broke
  Release publication (the release watchdog flagged the missing Releases). The
  release ships the CLI + plugins
  (`build/VST3`,`/CLAP`,`/AU`), never the example apps, so the release legs
  build with `-DPULP_BUILD_EXAMPLES=OFF` (matching `build.yml`). If you add a
  new release/packaging workflow, configure with examples OFF (or a populated
  `SKIA_DIR`), or the design-tool Skia gate will block it.
- **Release/SDK builds must pass `-DPULP_ENABLE_AUDIO_PROBES=OFF`.** The
  standalone audio probe is a dev/example inspection surface and defaults ON
  for local development, but shipped CLI, standalone, and SDK artifacts must
  not export it. Keep `release-cli.yml`, `release-path-pr-gate.yml`,
  `release-dry-run.yml`, `sign-and-release.yml`, `release-cli-local.sh`, and
  checkout-backed SDK configure paths aligned when touching release CMake
  flags.
- **Official release SDKs carry positive provenance.** Starting at the
  `sdk_provenance_floor` in `tools/scripts/release_product_matrix.json`,
  `release-cli.yml` stamps `sdk-provenance.json` only after proving the clean
  source checkout, exact `v<version>` tag commit, Release build, platform, and
  disabled audio-probe/inspector features. The downloaded-asset finalizer binds
  every marker back to that exact tag SHA and archive platform before publish,
  and parses the installed `include/pulp/runtime/build_info.hpp` to reject dirty
  or non-Release metadata and a version/short-SHA inconsistent with the marker.
  Build-info dirt means tracked source changes; untracked configure/build inputs
  do not change the identity of an otherwise clean release checkout.
  Manual-backfill compatibility helpers live under `RUNNER_TEMP`, outside the
  tagged checkout, because older tags' build-info probes include untracked files.
  The Linux dependency action is the exception because local actions must be
  checkout-relative; record only files materialized for an old tag and remove
  those exact files immediately after the action runs, before CMake configures.
  Keep the marker stamp, `PulpSdkProvenance.cmake` fail-closed consumer cache,
  archive verifier, and Forge preflight in lockstep. A manual marker-era
  `source_ref` substitution is forbidden; evaluate its floor from the trusted
  default-branch matrix before checking out the operator-selected ref.
- **The install-consumer smoke must compile against the installed prefix, not
  the source tree.** When an exported SDK target gains or exposes public
  headers, add representative `include/pulp/...` existence checks and include
  those headers from the generated consumer probe. Link the exported targets
  that own the APIs as well. This catches a target such as `Pulp::host` being
  exported with a library while its public headers are accidentally omitted
  from the install manifest. Keep `tools/validation/sdk-smoke` in sync so the
  same proof is runnable locally without GitHub Actions.
- **The release archive matrix must match every archive-bearing SDK target.**
  `test_release_artifact_contents.py` derives the installed target set from
  `tools/cmake/PulpInstallRules.cmake`, removes interface-only libraries, and
  requires exact equality with `release_product_matrix.json`. The
  `workflow-lint.yml` path filter deliberately covers every `CMakeLists.txt`
  plus `tools/cmake/**`, because target definitions also live in the repo root,
  `inspect/`, and CMake helpers—not only under `core/`. When adding an installed
  library, update the matrix in the same PR; when changing how
  `PULP_SDK_TARGETS` is assembled or consumed, keep the canonical literal
  `set` / `list(APPEND)` / `install(TARGETS ...)` forms or extend the
  fail-closed parser and its negative controls together.
- **The release archive matrix must match every packaged CLI product.**
  `cli_binary_stems` lists the shipped executables and `common_cli_members`
  lists non-executable resources such as the import-design browser-capture
  runtime. Keep those fields aligned with `package_cli.py` and
  `tools/import-design/browser_capture/runtime_manifest.txt`; the verifier
  rejects both missing and unexpected archive members.
- **`sign-and-release.yml` does NOT wait on the release any more — do not add the
  poll back.** It used to poll `gh release view "$TAG"` until release-cli created
  the release, so it could attach `appcast.xml`. That poll ran on the macOS
  notarize job, which holds the single release VM — the same VM release-cli's
  `darwin-arm64` leg needs to *produce* the release being waited for. It was a
  circular wait, and every tag where signing won the race deadlocked until the
  poll timed out.

  `release-cli.yml` now writes `appcast.xml` itself (the Sparkle feed is a pure
  function of the tag name, so nothing about it needed macOS signing) and is the
  sole owner of publication end to end. `sign-and-release.yml` holds
  `contents: read` and cannot write a release at all: it may fail, hang, or be
  cancelled without affecting whether the SDK ships.

  Widening a poll timeout was the old mitigation and it is exactly backwards — a
  longer wait squats the scarce VM for longer. If you find yourself raising a
  timeout on the release VM, stop: move the wait off the VM instead.
- **Hooks inherit `GIT_DIR` — tests that shell out to git can corrupt the live
  worktree.** Git exports `GIT_DIR`/`GIT_WORK_TREE` into hook environments, and
  a set `GIT_DIR` *overrides* `git -C <dir>` discovery. So when the pre-push
  hook (or `shipyard`) runs the full `ctest` and a test does
  `git -C <tempdir> init/commit/checkout`, those commands silently hit THIS
  worktree's repo instead — stray `initial` commits, throwaway branches, and a
  `core.bare=true` flip in the shared config (which makes the main checkout
  look "not a work tree"). `.githooks/pre-push` and
  `tools/scripts/local_diff_cover.sh` now `unset GIT_DIR GIT_WORK_TREE …`
  before running gates/ctest, and the git-shelling tests scrub the same vars
  in-process (see the regression test in `test_mcp_server.cpp`). If you add a
  test that shells out to git on a temp repo, clear the inherited git env first
  (or run from a context with no `GIT_DIR`), and never assume `-C` alone
  isolates it. Recovery if a worktree was hit: `git config core.bare false`,
  reset the branch off the stray `initial` commit, delete the throwaway branch.
- The required `macos` context in `.github/workflows/build.yml` is published
  directly by the native macOS matrix child for pull requests, Shipyard manual
  dispatches, and merge groups. It must not depend on the combined build matrix
  or a jobs-API reporter: advisory Linux and Windows work may continue without
  delaying queue admission. Event-specific bootstrap jobs own `macos` only for
  an intentional native skip or a fail-closed provider/classifier failure;
  inactive bootstraps use an `-unused` name so they cannot collide with or
  satisfy branch protection.
- **Inline Python in preamble jobs must start from system `/tmp`.** The
  `PULP_PREAMBLE_RUNS_ON_JSON` lane can execute below `/Volumes/Workshop`.
  `python3 -` resolves cwd while computing `sys.path[0]`, before it executes
  the supplied script; a wedged volume therefore freezes an otherwise healthy
  routing or aggregate poll. `RUNNER_TEMP` may live on that same work volume,
  so wrap each inline invocation with `cd /tmp` first (`/private/tmp` on
  macOS), and use `GITHUB_WORKSPACE` only as an absolute repo-path argument
  afterward. `test_preamble_python_stable_cwd.py` enumerates both
  `PULP_PREAMBLE_RUNS_ON_JSON` jobs and long-lived polling aliases routed by
  `PULP_ALIAS_RUNS_ON_JSON`; moving a job between those pools must preserve
  its stable-cwd classification.
- `.github/workflows/release-dry-run.yml` (P9-2, #2576) exercises the release
  build → `package_cli.py` → `pulp ship appcast` chain on a synthetic version
  (`0.0.0-dryrun`) WITHOUT publishing — no GitHub release, no signing/notarize,
  no appcast upload; artifacts are throwaway. It's additive (does not touch
  `release-cli.yml` / `sign-and-release.yml`) and runs weekly + on demand, so a
  build/packaging/appcast-generation regression surfaces BEFORE a real tag.
  Keep it credential-free (notarize/sign stay in the real path) so it can run
  on a schedule without secrets.
- **Release-bot source refs must be SSH-signed.** `auto-release.yml` creates
  signed annotated `v*` tags with `git tag -s`, and the bot commit workflow
  (`post-tag-sync.yml`, and `version-at-land.yml` once flipped to `--push`)
  configures the same SSH
  signing helper before committing. The required Actions secret is
  `RELEASE_BOT_SSH_SIGNING_KEY`; it is a file-backed OpenSSH private key backed
  up outside the repo. The workflow uses `25807+danielraffel@users.noreply.github.com`
  because GitHub verifies SSH signatures against the account that owns the
  uploaded signing key. If this secret is absent, the signing setup must fail
  closed rather than create unsigned release tags or bot commits. The helper
  writes repository-local Git config only; global signing config can outlive
  the temporary key and poison unrelated jobs on a shared runner.
- **Release-runner Xcode must be pinned (C++20 parity).** `sign-and-release.yml`
  runs on GitHub-hosted `macos-14`, whose DEFAULT Xcode is 15.4 — its Apple clang
  lacks C++20 **P0960** (parenthesized aggregate init, `Type p(arg)` for a
  ctor-less aggregate). The self-hosted PR `macos` lane uses a much newer clang
  that accepts it, so a CLI/import TU compiled on every PR but FAILED only in the
  release build — silently breaking GitHub Releases v0.372–v0.391 (tags kept
  being cut; only the release-cadence watchdog noticed). The job now selects the
  newest installed Xcode 16.x via `xcode-select` (shell, no third-party action so
  an actions-allowlist can't hold the release hostage), restoring C++20 parity
  with the PR lane. **When a release/packaging workflow builds C++ on a GitHub-
  hosted macOS runner, pin a modern Xcode** — the default lags and silently
  diverges from the PR toolchain. (Code-side defense: always brace aggregate init
  `Type p{arg}` in CLI/import code — see the import-design skill.)
- **The release build is NOT a test gate.** `sign-and-release.yml` no longer
  re-runs the unit suite (`ctest`). By the time a commit is tagged it has already
  passed the FULL suite on the PR/merge gate (self-hosted lane, real
  GPU/display/iOS-SDK). Re-running on the HEADLESS GitHub-hosted release runner is
  redundant and yields false failures from environment-only tests (Skia-raster
  screenshot → empty, cmake-require-gpu → timeout, cmake-ios-hostapp-links) that
  pass on real hardware — that blocked Releases AFTER the Xcode-pin let the build
  through. Principle: tests gate at PR on representative hardware; the release
  builds + signs + notarizes + packages the validated commit (the Build step is
  the release-config compile smoke; `validate.yml` gates format validators). The
  replacement gate is a built-ARTIFACT smoke (the "Smoke built plugins" step):
  it `nm`-reads each built `build/CLAP/*.clap` and FAILS only if a Mach-O was
  produced without its `clap_entry` C-ABI export (a real linkage regression),
  warning-and-passing when no artifact is found (a path/setup miss must never
  re-block the release). Static symbol read = NO execution, so it's headless-safe
  — never use dlopen+init (loads GPU/Skia libs the headless runner lacks) or
  re-run the hardware-dependent ctest suite.
- **Sandbox E2E macOS has a long cold C++ CLI build.**
  `.github/workflows/sandbox-e2e.yml` builds the `pulp-cli` target before
  running the Python sandbox harness. On GitHub-hosted `macos-latest`, a cold
  C++ CLI build can exceed 30 minutes and be reported as `cancelled` with
  `The operation was canceled` plus orphaned `clang` processes, before pytest
  starts. Treat that as a job timeout/build-cost issue, not a sandbox assertion
  failure. The workflow uses `timeout-minutes: 60` so the cold macOS build has
  room to finish while still bounding genuinely wedged runs.
- `.github/workflows/header-self-contained.yml` (pulp #2576) is a BLOCKING gate
  for the "compiles on Apple Clang, breaks on Linux" transitive-include class
  (e.g. `uint32_t` without `#include <cstdint>` — broke the v0.197.4 release).
  It compiles each PR-changed public header standalone with Linux Clang via
  `tools/scripts/check_headers_selfcontained.py`. Unlike the advisory IWYU gate
  it only fails on a header that genuinely won't compile alone (no "unused
  include" false positives), so it is safe to block on. Headers whose module
  isn't in the GPU-off compile DB are skipped, not failed.
- **Windows FetchContent subbuilds need a short base dir.** GitHub-hosted
  Windows runners can still route MSBuild metadata through the legacy
  260-character path limit. The wgpu prebuilt-populate subbuild has deeply
  nested stamp paths, so `build-windows/_deps/...` can exceed MAX_PATH during
  configure even before compilation. `build.yml` passes
  `-DFETCHCONTENT_BASE_DIR="$PWD/fc"` only on Windows to keep dependency
  subbuilds short while preserving the normal `build-windows` artifact/test
  directory.
- `.github/workflows/watchdog-reaper.yml` (pulp #2576) sweeps ALL open release
  watchdog trackers and closes any whose version is released or superseded
  — the existing watchdogs only auto-close inside a recent window, so historical
  per-version trackers orphaned (334 had accumulated). It only matches the exact
  auto-generated tracker titles and only closes objectively-resolved ones.
  SHA-keyed `release: stuck` trackers carry no version in their title; the
  reaper parses the body for the tip SHA + uncovered surfaces and closes once a
  later release tag for EVERY uncovered surface contains the stranded commit
  (`tools/scripts/reap_stranded_tracker.py`). It runs daily at 06:00 UTC AND
  immediately after every successful `Release CLI` run (via `workflow_run` —
  the publish uses `GITHUB_TOKEN`, whose events cannot trigger a `release:`
  workflow), because the resolving release otherwise lands just after the daily
  sweep and objectively-closed trackers sit open all day looking like live
  incidents. A pile of open `release: stuck` trackers therefore means the
  release pipeline is genuinely stuck NOW — check release-reconcile's single
  incident issue first, don't triage the trackers one by one.
- **NEVER delete a GitHub release or draft — deletion of a once-published
  release permanently burns its tag name.** GitHub reserves an immutable
  release's `tag_name` forever; every later publish attempt 422s with
  `tag_name was used by an immutable release`, and no re-dispatch can ever
  succeed (this burned v0.807.0 and v0.808.0). Deleting a draft destroys its
  attached assets, but the binaries survive as the building run's workflow
  artifacts (~90-day retention, `gh run download <run-id>`). Recovery from a
  failed publish is always a NEW patch tag, never a deletion.
  `release-deleted-tripwire.yml` files a tracking issue the minute any release
  is deleted, and release-cli's publish step names the burned-tag condition
  explicitly when it hits the 422.
- Keep watchdog/issue-maintenance workflows on REST `gh api` calls. Avoid
  `gh issue list` / `gh pr *` helpers in those paths because they can use the
  shared GraphQL quota; a watchdog must not fail while reporting that the
  watched workflow already recovered.
- Pulp's docs site is deployed by `.github/workflows/docs-deploy.yml`. If
  GitHub Pages is set to legacy `main`/`docs` builds, the generated Pages
  checkout tries to clone the private `planning` submodule with the default
  token and fails before docs deploys. Fix that at the Pages configuration
  layer (`build_type=workflow`), not by weakening normal submodule checkout.
- `.github/workflows/sanitizers.yml` runs broad ASan/UBSan matrices, but its
  exclude regex may carry narrow sanitizer-lane skips for non-memory-safety
  failures that remain covered by regular Build/Test. Keep those skips named
  exactly to the failing CTest cases and leave comments explaining the
  alternate coverage path; do not use sanitizer excludes to hide new targeted
  coverage tests.
- **ASan, TSan, and UBSan use `PULP_SANITIZER=<kind>`; ASan's
  example-bundle build does too.** Do not replace the named options with raw
  `CMAKE_*_FLAGS`: the option owns the compiler/linker flags and explicitly
  marks compiler-injected sanitizer runtimes as test-only for
  bundle-relocatability validation. Installed-SDK consumer fixtures propagate
  the matching runtime requirement because instrumented static libraries retain
  those references. Ordinary release bundles do not set the option, so the
  shipping guard remains strict.
- **UBSan is pinned to `macos-26`, not `macos-15`.** Xcode 16.4's Apple
  Clang/libc++ combination reported invalid `std::__shared_weak_count` vptrs
  while destroying ordinary persistent timeline trees, even though the same
  exact tests passed under ASan and under current Apple Clang UBSan. A full
  `macos-26` control ran every timeline model, persistence, graph-binding, and
  example test without that signature. Keep the lane on the current hosted
  toolchain; do not suppress `vptr` or rewrite the shared ownership model to
  accommodate the stale runtime.
- **UBSan uses `RelWithDebInfo`, not Debug/O0.** Keep
  `-fsanitize=undefined -fno-sanitize-recover=all`, symbols, and the complete
  CTest matrix, but run production-scale DSP spectral and stability
  certifications with optimization. Debug/O0 can hit their hang guards without
  emitting a sanitizer diagnostic; do not respond by excluding those tests or
  increasing their already-generous timeouts.
- **Flaky-lane retry (de-flaking).** The ASan/UBSan/TSan lanes
  (`sanitizers.yml`) and the coverage lanes (`scripts/run_coverage.sh`) run
  ctest with `--repeat until-pass:2`. Timing-sensitive tests intermittently
  fail under sanitizer slowdown (a *different* test each run — RenderLoop
  coalescing, ImageView fill, etc.), and a single failure aborts ctest and
  cascades into the diff-coverage gate (partial profile → 0% on the diff),
  leaving every PR `UNSTABLE` even when the required gates pass. `until-pass:2`
  retries a failed test once so a flake passes on retry while a *genuine*
  failure still fails both attempts (no masking). This complements the per-lane
  `--exclude-regex` flake lists by catching not-yet-listed flakes generically —
  prefer it over growing the exclude list for transient timing flakes.
- **Coverage upload eligibility follows the report, not the test exit code.**
  `scripts/run_coverage.sh` and `run_python_coverage.py` finish report generation
  after test assertion failures. The workflow uploads those reports only after
  semantic verification (present, parseable, positive `lines-valid`); configure,
  build, timeout, and report-generation failures still produce no eligible
  upload. On hosted macOS, keep instrumented build parallelism at two workers
  while the shared script defaults CTest to eight independently; this applies to
  GitHub-hosted, local, and SSH coverage. Coupling both to `--jobs 2` made the
  nearly-20k-test suite hit its budget and silently drop every native upload.

### `web-plugins.yml` — pin the wasm toolchain, and fetch Skia from the manifest

The WAM / WebCLAP / browser-UI lane (`WAMv2 + WebCLAP (Linux, headless Chrome)`)
has two rules that exist because breaking them produces failures on PRs that
touched nothing:

- **emsdk is PINNED, never `latest`.** emsdk floats, and has already shipped
  breaking WebGPU API changes and a change to how `SINGLE_FILE` embeds the wasm.
  Each landed here as a mystery red X on an unrelated PR. The lane pins the
  version it is verified against (6.0.2 today); 4.0.10 is the floor for the
  emdawnwebgpu port, so a bump stays at or above it. Bump it deliberately, in
  its own PR, and update `determinism.web_toolchain` in `tools/deps/manifest.json`
  in the same change.
- **The Skia wasm slice is fetched from `tools/deps/manifest.json`, not a
  hardcoded URL.** The lane reads the URL + sha256 out of the manifest, verifies
  the checksum, and keys the `actions/cache` entry on `hashFiles('tools/deps/manifest.json')`
  — so the CI lane and the dependency audit can never disagree about which binary
  is in use, and a pin bump self-invalidates the cache. Do not paste a release URL
  into the workflow. The lane then runs `tools/scripts/verify_wasm_skia_slice.py`
  to assert the slice really is Ganesh/WebGL2 (no Dawn, no Graphite) — the
  invariant `FindSkia.cmake`'s Emscripten arm is built on. See the
  `skia-gpu-build` skill.

Path-filter hygiene: this workflow is `paths:`-filtered, so a new web-facing
directory (a demo, a `packages/pulp-web-player/**` change, a new `PulpWeb*.cmake`
module) is **not covered until you add its glob**. A web demo that silently stops
being built is the failure mode.


### `post-tag-sync.yml` curl-installs its OWN Shipyard — pin it in lockstep

The post-tag changelog sync does NOT run on the local fleet CLI. `post-tag-sync.yml`
(Shipyard-owned, generated by `shipyard release-bot hook install`) fires on a `v*`
tag, `curl`-installs a Shipyard pinned by its OWN `SHIPYARD_VERSION` env on
ubuntu-latest, and runs `shipyard release-bot hook run`, which reads
`[release.post_tag_hook]` from `.shipyard/config.toml`.

So there are TWO Shipyard versions that matter, and they drift independently: the
one on each fleet Mac (`tools/shipyard.toml` + `install-shipyard.sh`), and the one
THIS workflow installs. If the workflow's pin is older than a config key it must
honour, it silently ignores the key. A stale pin at 0.70.0 reverted to a direct
push to `main` that the merge-queue ruleset refuses — defeating the config change
and the fleet upgrade both.

**The `push_mode = "pr"` floor is v0.79.0, NOT v0.78.0.** This distinction is
subtle and it cost nine unmergeable PRs. 0.78.0 honours `push_mode` far enough to
*open* the PR, so the pin looks correct and the branch appears — but the commit is
still stamped `docs: regenerate changelog for <tag> [skip ci]`. That marker is
right for the direct-push path it was written for and **fatal on the PR path**:
Actions skips every workflow, so the PR never obtains the required checks branch
protection demands, so it can never merge, and the next release opens another one.
They stacked from v0.751.0 to v0.759.0 and blocked the release pipeline, surfacing
as a run of `release: stuck — fix/feat merged without bump` issues rather than as
anything pointing at the changelog PRs. Shipyard split the two subjects in 0.79.0
(`release_bot_commit_subject`: `pr` omits the marker, direct keeps it).

Diagnosing this class: a PR whose required checks read **`MISSING`** (not
pending, not failing — absent) is almost always a workflow that never dispatched.
Check the tip commit for `[skip ci]` first, then whether the PR was opened by an
App token — GitHub does not dispatch `pull_request` workflows for App-token
actions, and neither `workflow_dispatch` (its runs do not attach to the PR as
checks) nor close/reopen from an App token will fix that. A commit pushed from a
**user** identity does.

Keep `SHIPYARD_VERSION` here `>=` the `tools/shipyard.toml` pin whenever a
post-tag-hook feature depends on it. (Durable fix is for `hook install` to pin
from `tools/shipyard.toml` — a Shipyard-side change.)

### Release platforms are a one-line knob: `active_platforms`

`release_product_matrix.json` carries two platform fields: `platforms` (the
full historical inventory — per-platform archive verification and
library-stem merging key off it for every era; do not shrink it) and
`active_platforms` (the subset a release currently SHIPS; absent = all).
Three consumers derive from `active_platforms`, which is the whole point —
they can never disagree:

1. `release-cli.yml`'s build/smoke `matrix.include` — expanded by
   `release_build_matrix.py` in the `resolve-macos-runner` job (leg configs —
   runner images, the darwin-x64 `macos-15-xcompile` sentinel, the linux-x64
   glibc container — live in that script, unit tested).
2. The publish-time `--all-platforms` content verification.
3. The finalizer's `--exact-required` asset list.

The publish step imports `active_platforms` from the DEFAULT branch's matrix
(like the policy floors), so a one-line edit on main governs every subsequent
run — tag pushes, re-dispatches, old-tag backfills. Flip back by deleting the
field, or re-add platforms piecemeal. Guardrails: the subset must be
non-empty, within the inventory, and contain a darwin platform (the appcast
needs a darwin min-OS floor); `release-platform-subset-check.yml` (daily)
opens a tracking issue once the subset is older than 7 days — a paused
platform's release leg does not compile, so regressions there land unnoticed
for exactly as long as the pause stands. Do NOT reintroduce a hardcoded
platform row in either matrix or a literal asset name in the finalizer;
`test_release_workflow_test_step.py::ActivePlatformsDeriveTheReleaseMatrix`
rejects both.

**Job-level `if:` cannot see the `matrix` context** — that is WHY the legs
are derived rather than gated: `if:
contains(..., matrix.platform)` on a matrix job evaluates with an empty
`matrix` and silently skips every leg. GitHub only exposes `github`, `needs`,
`vars`, and `inputs` to `jobs.<id>.if`.

### NEVER set `run-name:` on release-cli.yml (it stops all releases)

GitHub returns a workflow's `run-name` as **`workflow_run.name`** in the REST API —
it **REPLACES** the workflow name, it does not sit alongside it. The self-hosted
tartci supervisor that provisions release-cli's macOS VMs picks up its work with:

```
select(.name == "Release CLI")
```

So the moment a `run-name` is set, **every release run becomes invisible to the
supervisor**. Its log reads `queued=0 running_macos_vms=0/2` while release runs sit
with their required `darwin-arm64` leg queued forever. No macOS VM is booted, no
runner appears, and **no release can ever build** — silently, for every future tag.

This shipped once (a run-name was added so `release-reconcile.yml` could attribute
its repair runs, since a `workflow_dispatch` run's `head_branch` is the ref it was
dispatched FROM, not the tag it builds). The tag now travels in a **job name**
(`resolve-macos-runner`), which the API exposes as a separate field and the
supervisor does not key on. `tools/scripts/test_release_workflow_test_step.py`
asserts `run-name` never returns.

**The general rule:** a workflow's public identity (`name`, and therefore
`run-name`) is an **interface** that self-hosted infrastructure keys on. Renaming a
run is not cosmetic. Before changing it, grep the tartci supervisor config
(`TARTCI_RUNNER_WORKFLOW_NAME`) for anything matching on it.


### A bare `cmake --build` in a workflow is a SERIAL build, and no guard sees it

CI runners select no CMake generator, so `cmake --build` resolves to **Unix
Makefiles**, and with no `--parallel`/`-j` that runs `make` with **no job flag
at all — one translation unit at a time**. This is the *opposite* failure to
the one `build_parallelism_guard.py` polices (unbounded / whole-machine
counts), so the guard stays green while a lane quietly compiles ~1,700 TUs on
one core. release-cli.yml shipped exactly this for months: ~50 min of a ~55 min
release leg was a single-threaded compile (v0.806.1 logs), and nothing flagged
it.

How to tell from a log without guessing: Makefiles progress lines look like
`[ 37%] Building CXX object …` (Ninja's look like `[123/1700]`), and a **serial**
make emits those percentages strictly monotonically — a parallel make interleaves
them out of order. Counting out-of-order percentage lines is a two-minute
proof either way.

Release lanes (`release-cli.yml`, `release-dry-run.yml`) now derive an explicit
count (`--parallel "$jobs"` from the runner's visible cores — deliberate on
ephemeral single-tenant release runners/VMs, where admission control happens at
the VM level, not by throttling each build) and
`test_release_workflow_test_step.py::ReleaseBuildParallelismExplicit` fails any
`cmake --build` that re-loses its job count there. Other workflows are NOT
covered by that test — when adding a build step to any workflow, give it an
explicit count or route it through `tools/ci/governed-build.sh` (mandatory
anyway for legs that can resolve to the shared self-hosted Macs).

### `shipyard pr` can leave YOUR build dir at Debug — and a later "successful" build can be stale

The local validation backend builds Debug in the editing checkout. That is
already recorded below as a source of CI *false-fails*; the more dangerous
direction is the opposite one, and it lands on you rather than on CI:
**it silently converts your next local proof into a Debug proof.** You run
`shipyard pr`, then rebuild and re-run your tests, they pass, and you record a
green Release proof that was never Release. CLAUDE.md's warning that "something
in the shipyard / pre-push gate / rebase paths can silently reset the cache to
Debug" is exactly this; observed 2026-08-17 on a DSP cell whose post-reconcile
proof had to be thrown away.

**Detection is the two-way Release check, and only the two-way check works.**
CLAUDE.md already requires both; this is the failure it is for:

```bash
grep '^CMAKE_BUILD_TYPE' build/CMakeCache.txt                       # Release?
grep '^CXX_FLAGS ' build/<dir>/CMakeFiles/<target>.dir/flags.make   # -O3 -DNDEBUG?
```

Here both read Debug consistently (`Debug` and `-g`), so a cache-only check
reports the cache field it was given and tells you nothing — the flip moves both
together.

**A second, independent failure rides along: binaries that never got rebuilt.**
After the flip, a `cmake --build` that exits 0 can leave test binaries older than
the sources you just edited, because the reported success covers the targets make
decided were current. `Built target` is not evidence a target was compiled. So
check freshness, not just exit status:

```bash
stat -f '%Sm %N' -t '%H:%M:%S' build/CMakeCache.txt \
  build/test/<binary> test/<source>.cpp
# every binary must be NEWER than both the cache and its sources
```

Two of three binaries in the observed case predated the edit that was supposedly
under test, while the build reported success and the tests passed — they were
proving the previous revision.

**Recovery: delete the build directory.** Reconfiguring Release over a flipped
cache leaves mixed Debug/Release objects, which surfaced as an *unrelated-looking*
link error inside Catch2 —
`Undefined symbols: Catch::handleExceptionMatchExpr(...)` — that reads like a
Catch2 or dependency problem and is neither. Do not debug it; `rm -rf build`,
configure Release, build once. Budget for a full clean build.

**Prevention:** after any `shipyard pr` (or any pre-push gate run) in a checkout
you also build proofs in, re-verify the build type before trusting the next
result — or keep proof builds in a directory Shipyard never touches. Do not re-run
`shipyard pr` merely to refresh an already-published PR; push the branch instead.

### Keep ONE -O0 lane: it sees UB that -O3 provably hides

`.shipyard/config.toml`'s macOS validation lane builds **Debug (-O0)**. This
contradicts CLAUDE.md ("Release is the default") and reads like config drift — a
config audit will want to flip it. **Don't.** It is the only lane in CI that can see
a whole class of undefined behaviour.

2026-07-12 (#6081): a macro-gated **inline function template in a header**
(`snap_to_zero()`), plus a test TU that redefined that macro before including it, is
an ODR violation — two TUs emit the same mangled symbol with different bodies.

```
-O3   each TU inlines its own copy -> each behaves per its own macro
      the A/B test appears to work. RELEASE IS GREEN. Invisible by construction.
-O0   nothing inlines -> both emit a weak symbol, the linker keeps ONE arbitrarily
      the "disabled" reference silently ran the enabled code. DEBUG IS RED.
```

The red test was the *mild* outcome: had the linker kept the other definition, the
assertions would have PASSED while exercising a no-op — a null test asserting
nothing, green forever.

**The fix shape is not "delete the redefine"** — it is *give the variant its own
binary*, compiled consistently end to end, linking no default-built TU (see
`test/denormal_null_refgen.cpp`). Guarded by
`tools/scripts/test_odr_macro_gated_headers.py`, which only flags macros that gate an
inline/template **function body** (a macro guarding an `#include` or a platform block
cannot cause ODR).

**A perf gate failing on that lane is a MIS-CALIBRATED GATE, not a reason to flip the
lane.** `test_yoga_layout_bench`'s timing threshold was sized at ~11x a *Release*
baseline; in Debug the same pass is ~11.6x slower, eating the entire margin, so it
sat permanently at the edge and load merely tipped it. It is now `#ifdef NDEBUG`-gated
(the GitHub macOS lane is Release, so real coverage is preserved), while the
structural assertions still run in every build.

> A false red is worse than no gate: it trains everyone to wave away red as
> "probably the box" — which is exactly how a real bug gets dismissed.
### Never let a flaky advisory leg decide a run's conclusion

`nightly-intel` concluded **`cancelled` on every scheduled run** for a long time,
which reads as "this workflow produces no Intel coverage". That reading is wrong, and
the trap is worth internalising:

```
Universal + lipo + dual-arch auval (macos-15) : SUCCESS    <- every night
Native Intel build + test (macos-15-intel)    : cancelled  <- its 120m job TIMEOUT
Intel nightly watchdog                        : SUCCESS
```

`universal-crosscheck` — the arm64+Rosetta lipo + dual-arch auval signal that
`release-cli.yml` relies on after the per-tag universal gate was removed — **succeeds
every night**. The Intel signal was there the whole time, buried under a run-level
conclusion poisoned by a *different* leg.

For m153+, that job has a fourth required component before the universal build:
one JSON capability receipt compile/links/runs the universal provider's arm64
slice natively and its x86_64 slice through explicit Rosetta. `Compute result`
must count the capability status with build, lipo, and auval. A universal product
link can otherwise stay green while currently-unused m153 symbols are missing
from one slice, so neither a partial receipt nor a skipped probe is acceptable.

`native-intel` on `macos-15-intel` had **never once completed**: that image CPU-pegs,
so the job hit its 120-minute limit every run. **GitHub reports a job timeout as
`cancelled`, and a cancelled job cancels the whole RUN.** So a leg producing zero
signal was deciding the conclusion of the leg producing the real one — while burning
two hours of a scarce Intel runner nightly.

**Fix pattern: bound the work in the STEP, not with the job timeout.**

```bash
if timeout 75m cmake --build "$BUILD_DIR" -- -k 0 2>&1 | tee build.log; then
  echo "status=pass" >> "$GITHUB_OUTPUT"
elif [ "${PIPESTATUS[0]}" = "124" ]; then     # WE killed it, not GitHub
  echo "::warning::runner pegged — INFRA, not a product failure"
  echo "status=infra-timeout" >> "$GITHUB_OUTPUT"
fi
```

The job then finishes **normally** with a loud, explicit infra-skip, and the run can
reach a conclusive success/failure. Do **not** reach for job-level `continue-on-error`
here: GitHub documents it for a job that *fails*, and a timeout is reported as
*cancelled* — whether it covers that is a semantics gamble. Bounding the step is
correct by construction.

> A silent cancel is indistinguishable from "this workflow does nothing" — which is
> exactly how working coverage got written off as absent.

## Current Build-and-Test routing

As of the 2026-05-20 classify gate, `build.yml` runs a cheap
`classify` job before allocating the native matrix. PRs that touch only
skip-safe docs/planning paths report the native aliases green without
running macOS/Linux/Windows builds; PRs that touch C++/Swift/CMake or
workflow inputs must run the native matrix. A code PR whose native build
is skipped is a CI bug.

`workflow_dispatch` defaults `runner_provider` to `github-hosted`, not
Namespace; do not dispatch with `runner_provider=namespace`. Linux/Windows
use GitHub-hosted runners by default. Required macOS PR and merge-group work
routes through the M1/M3/M5 event-class-v2 JIT pool described at the top of
this skill; repository variables control any overflow.

Advisory macOS supervisors that share a host with the event-class-v2 gate must
yield to both required Build and Test classes. Their
`TARTCI_YIELD_TO_LABELS` selector includes the base gate labels plus both
`pulp-build-merge-group` and `pulp-build-pr-head`: the idle gate is
admission-only and cannot evict a sanitizer that started during PR validation
when a merge group arrives later. Base labels alone match neither v2 class.

The authoritative Windows x64 functional matrix is pinned to `windows-2022`
(Visual Studio 2022) by `.github/workflows/build.yml`. Shipyard's current ship
path does not send a `windows_runner_selector_json`, so the workflow fallback
is the operative selector. `.shipyard/ci-profiles/normal-local-fast.toml`
mirrors that PR policy with its PR-only `github.windows-x64-runtime` target for
profile inspection; the current profile planner is read-only and does not
override dispatch. Its shared coverage/scheduled `github.windows-x64` target
remains `windows-latest`. Keep those mirrors truthful, but never rely on them
to route a run. The standalone MSVC release-path, MIDI 2, and BLE compile gates
remain on `windows-latest`, as do release builds, so the newest hosted compiler
and SDK are still exercised without allowing an in-place runner-image
migration to change the CRT/toolchain beneath the complete runtime suite.
`tools/scripts/test_windows_runner_policy.py` reads every operative surface
(build, release, coverage, nightly, release resolver, and Shipyard profile)
independently and runs in the PR `workflow-lint` lane. Update that one
cross-surface invariant whenever the split changes; a profile-only or
workflow-only assertion is not enough because the two can self-agree while a
different production lane drifts.

Do not push empty commits just to churn queued macOS checks. Cancel
superseded SHAs, rebase or push only when a PR needs current `main`, and
wait unless a check has actually failed.

### Gotcha: `.shipyard.local` can silently route the macOS lane off the gate

`.shipyard/config.toml` declares `[targets.mac] backend = "local"`, and
Shipyard merges the gitignored `.shipyard.local/config.toml` on TOP of it. A
`[targets.mac]` block there overrides the backend — and the failure does not
look like a misconfiguration, it looks like a hang: `shipyard status` reports
`mac: cloud`, shipyard watches a redundant GitHub-hosted run and times out at
3600s, and the required `macos` check (posted ONLY by the local runner) never
appears. That is why the block in Pulp's own `.shipyard.local/config.toml` is
commented out and annotated `DISABLED 2026-07-09`.

**A missing `.shipyard.local/` is the CORRECT state, not a gap** — the repo
config's local mac target stands on its own, which is how the Mac Studio runs.
Do not "fix" a fresh worktree by copying a config in; that is what causes the
reroute. In particular never `cp -R` over `.shipyard.local/` — `config.toml` is
gitignored but `config.toml.example` next to it is TRACKED, so a recursive copy
clobbers a tracked file.

`tools/scripts/gates.sh` runs `shipyard_local_check.py` as an advisory that
reports an active non-local mac override before you push. It is read-only by
design: it never copies or repairs, because the repair instinct is the hazard.

`coverage.yml`'s macOS leg resolves its `runs-on` via
`resolve_runs_on.py --deny-labels pulp-build,pulp-build-vm`: a coverage
selector (repo var `PULP_COVERAGE_MACOS_RUNS_ON_JSON` or a dispatch input)
that names the shared gate pool **fails the resolver fast** rather than
letting a long advisory coverage run contend with the required `macos`
check. The dedicated coverage lane uses `pulp-coverage-vm-macos`; a bare
GitHub-hosted label (`macos-15`) is never denied. (Push-triggered coverage
on a busy `main` is *designed* to be superseded while queued — the
supersession-immune **scheduled** run, cron `17 */8 * * *`, is the one that
produces the green full-matrix upload that clears the coverage-stale watchdog.)

The **os-windows** coverage leg is explicitly inventoried but best-effort. The instrumented MSVC build +
~9k instrumented tests + `llvm-cov` over 1000+ objects exceeds the 210-min job
cap on GitHub-hosted `windows-latest` (it is ~1h on Linux/macOS), and the
staleness watchdog requires the other six reliable producer receipts and exact
Codecov processing, but reports Windows separately — so a missing Windows
receipt cannot make the freshness SLO permanently unattainable. **The
subtle trap (verified by canary):** job-level `continue-on-error` does NOT
neutralize a `timeout-minutes` *cancellation* — a cancelled job still makes the
run conclude `cancelled`. It DOES neutralize a normal job *failure*. So the
coverage suite step self-terminates at an **internal budget (180 min) below the
job cap**, turning the would-be cancellation into a normal non-zero exit that
the job-level `continue-on-error: matrix.os=='windows'` then absorbs → the run
concludes `success`. Don't "simplify" this to bare `continue-on-error`; it will
silently stop closing the watchdog. **And the watchdog that enforces the budget
must separate its steps with `;`, NOT `&&`** — if the kill is `&&`-gated behind
a `: > marker` write (which can fail on a Windows `RUNNER_TEMP` backslash path),
a failed marker write short-circuits the chain and the suite is never killed,
so the job hits the 210-min cap and is *cancelled* anyway. The kill is
mandatory; the marker is best-effort (cleanup of any partial Cobertura also
triggers on a 143/137 signal-kill exit, not just the marker). Real os-windows
*correctness* bugs are still worth fixing (the ARG_MAX response-file +
vanished-`-object` existence-filter + mass-drop guard in `run_coverage.sh` were
real); only the runtime/timeout is accepted as best-effort.

### `release-cli.yml` needs Rust; the VM golden does not have it

The macOS build gate and `release-cli.yml`'s darwin leg can share the Tart VM
golden (`pulp-build-runner`), but that golden is provisioned **only for the C++
"Build and Test" lane** — Xcode, homebrew cmake/ninja/node, baked Skia, and
**no Rust toolchain** (`manifests/pulp.macos.toml [brew]` has no rust/rustup).
`build.yml` stays green on it because it never builds the Rust `build/pulp` CLI
(`experimental/pulp-rs`, pulled into the CMake graph only when `PULP_ENABLE_GPU
&& top-level` — which `release-cli`'s GPU-enabled configure trips). So
**`release-cli` is the first VM workload that needs `cargo`**, and on a fresh
golden its "Ensure a working Rust toolchain on PATH" step exhausts every probe
(PATH `cargo`, `~/.cargo/bin`, `~/.rustup/toolchains/*`) and fails the darwin
release leg — while the bare-metal studios stay green because they have rustup
from operator setup. That step now **bootstraps rustup as a last resort**
(`curl … | sh -s -- -y --profile minimal --default-toolchain stable
--no-modify-path`, then front-loads `rustfmt`+`clippy` per
`rust-toolchain.toml`), so the lane is self-sufficient on any runner; the pin is
`stable`, so no reproducibility drift vs the studios. Before flipping
`PULP_RELEASE_MACOS_RUNS_ON_JSON` to a VM label, either that fallback must be in
place **or** the golden must bake rustup — otherwise the darwin publish leg
fails and the whole release never publishes (the `release` job `needs` the full
`build-cli` matrix). Steady-state fix is to bake rustup into the golden (tartci
`manifests/pulp.macos.toml`); the workflow fallback stays as the portability
belt for any un-baked runner.

### `release-cli.yml`'s Intel (`darwin-x64`) leg is CROSS-COMPILED, not native

`release-cli.yml`'s macOS matrix ships TWO slices: `darwin-arm64` (routed through
`resolve-macos-runner`) and `darwin-x64`, which **cross-compiles on an
Apple-Silicon runner** via the `macos-15-xcompile` sentinel. Its selector
priority is the per-leg override, `PULP_RELEASE_MACOS_RUNS_ON_JSON` (the
dedicated `pulp-build-vm-release` Tart pool), the legacy
`PULP_INTEL_RELEASE_MACOS_RUNS_ON_JSON`, then hosted `macos-15`. It builds with
`-DCMAKE_OSX_ARCHITECTURES=x86_64` plus
`-DPULP_RUST_CLI_TARGET=x86_64-apple-darwin`, and smoke-tests the thin binary
under Rosetta. It is a **REQUIRED** leg, the same reliability class as
`darwin-arm64`, so Intel ships in every release.

Do not route this required artifact build to the native Intel Mac Mini. Native
Intel remains a separate advisory/nightly portability canary; it must not become
release capacity or gate publication.

The native `macos-15-intel` image is **not** used: it CPU-pegs, queues for hours,
and never reliably shipped an artifact. Do not "restore" a native Intel leg, and
do not route Intel to the self-hosted studios (the `no Intel on the studios`
discipline in `docs/guides/intel-support.md`).

### A release starves behind advisory lanes — check contention before blaming a build

Three separate capacity walls have each, on their own, stopped a tagged SDK from
publishing while every platform binary built green. When a tag is stuck, work
down this list before touching build code:

1. **The macOS release VM is a capacity-1 pool.** It takes a 6-core lease and the
   host budget fits exactly one, so `release-cli`'s `darwin-arm64` leg cannot run
   alongside anything else that wants that VM. Anything that *waits* on another
   workflow while holding it deadlocks the release outright.
2. **Tagged releases and the release-path PR gate need distinct runner classes.**
   `release-cli.yml` and `sign-and-release.yml` use
   `PULP_RELEASE_MACOS_RUNS_ON_JSON` and the exclusive
   `pulp-release-tagged` label. `release-path-pr-gate.yml` prefers
   `PULP_RELEASE_PR_GATE_MACOS_RUNS_ON_JSON` and the exclusive
   `pulp-release-pr-gate` label; its legacy fallback to the tagged-release
   selector is migration-only and must remain until the separate selector has
   been deployed and proven. The TartCI release supervisor scans tagged-release
   demand before PR-gate demand, registers a runner with only the selected
   class label, and force-refreshes higher-priority demand immediately before
   minting a lower-priority runner. GitHub remains FIFO within each class. Do
   not collapse the selectors or put both exclusive class labels on one runner:
   that recreates the starvation incident where fresh PR jobs repeatedly claim
   the capacity-1 VM ahead of a tagged release.
3. **GitHub-hosted macOS concurrency is effectively ~1 job at a time.** The
   release's `darwin-x64` and universal-arch-gate legs both land on hosted
   `macos-15`, where they queue behind *advisory* lanes — sanitizers (×4 per PR),
   coverage, `sandbox-e2e`, Android, Intel-portability, consumer smoke. None of
   those are required checks; the release is. It loses to all of them.

**Diagnose, don't guess.** `tartci observe macos` on the VM host shows what the
runner is *actually* running (it prints the live `Running job:` line), and

```bash
ghapp api repos/Generous-Corp/pulp/actions/runners     # busy/idle per runner
```

distinguishes "saturated" from "wedged" from "starved by a lower-priority lane."
An offline-but-`busy=True` runner is a dead VM still holding its slot.

### The merge queue starves on the hosted pool, not on the Macs

When *nothing* merges — queue entries sit in `AWAITING_CHECKS` until the
ruleset's `check_response_timeout_minutes` evicts them — the instinct is to
blame the self-hosted Macs that serve the required `macos` gate. Check the
GitHub-hosted pool first. macOS runs on machines outside that pool and reports
in minutes; it is rarely the thing that is stuck.

The repo draws all hosted jobs from one fixed concurrency budget shared across
every workflow. Windows is the largest consumer by far — each build run carries
four hosted Windows jobs (the `Windows (x64)` matrix leg plus the
`windows-msvc-release-gate`, `windows-midi2-gate`, and `windows-ble-gate`
compile gates), and every one of them is *advisory*. A handful of open PRs is
enough for advisory Windows work to hold nearly the whole budget while the one
**required** hosted check — `Build + prove + (owner-gated) deploy`, on
`ubuntu-latest` — waits behind a queue dozens deep and never gets a runner.

This is why Windows runs on `merge_group` / `push` / `workflow_dispatch` but
**not** on `pull_request`. The queue's serial validation builds PR ∪ main, so
coverage is preserved and at most one run's worth of Windows jobs exists at a
time. Dispatch `build.yml` at a branch if you want Windows before enqueueing.

Census the pool before theorising — a run whose *overall* status is `queued` can
still hold running jobs, so counting runs undercounts badly. Count **jobs**:

```bash
(ghapp api 'repos/Generous-Corp/pulp/actions/runs?status=in_progress&per_page=100' --jq '.workflow_runs[].id'
 ghapp api 'repos/Generous-Corp/pulp/actions/runs?status=queued&per_page=100'      --jq '.workflow_runs[].id') \
| sort -u | while read -r r; do
    ghapp api "repos/Generous-Corp/pulp/actions/runs/$r/jobs" \
      --jq '.jobs[]|select(.status=="in_progress" or .status=="queued")
            |[.status,(([.labels[]]|join(","))|if test("self-hosted") then "SELF" else . end)]|@tsv'
  done | sort | uniq -c
```

A healthy repo shows hosted `in_progress` spread across runner classes. The
failure signature is one class (Windows) holding almost every in-progress slot
with `ubuntu-latest` pinned at one or two and a queue tens deep behind it.

**Never mass-dispatch `build.yml` to unstick a backlog.** Each dispatch adds
four more Windows jobs to the pool that is already the bottleneck, so the drain
gets slower, not faster — and the re-dispatched runs then wedge each other. If
the pool is already saturated with advisory work, cancel it (the macOS legs have
usually finished, so nothing is lost) rather than adding more.

### A step gated on `!= 'pull_request'` also fires on `merge_group`

`merge_group` is not `pull_request`, so `if: github.event_name != 'pull_request'`
is **true in a merge group**. A deploy / publish / release-side-effect step
written that way runs on the queue's synthetic `gh-readonly-queue/...` commit —
which has not merged and can still be evicted. `wclap-cloudflare.yml` deployed
production Cloudflare Pages this way; it was only ever masked because merge-group
runs starved before reaching the step, so enabling the queue's throughput
exposed it.

Gate any side-effecting step on an **allowlist** of the events that should cause
it, never a denylist:

```yaml
# not:  if: github.event_name != 'pull_request'
if: ${{ contains(fromJSON('["push","schedule","workflow_dispatch"]'), github.event_name) }}
```

A denylist silently changes meaning the day a trigger is added, and
`merge_group:` gets added to exactly the workflows a merge queue must run. When
auditing a workflow for queue-enablement, grep it for `!= 'pull_request'` and
confirm every hit is a reporting/validation step, not a deploy or publish.

### A PR wedged with NO checks at all — `pending` run, empty jobs array

A pull request that shows *no* check activity — not red, not queued, simply
nothing — after a push is usually not slow CI. It is a stuck `concurrency` group,
and it does not recover on its own.

`build.yml` groups by `build-<ref>` with `cancel-in-progress`. The reporting alias
jobs (`macos` / `linux` / `windows`) are the LAST jobs in the run — they wait for
their matrix leg and echo the outcome. If an alias cannot get a runner, it never
starts, so the run never reaches a terminal state; a non-terminal run holds the
group, and every later run on that ref sits at `pending` with **zero jobs**.
Pushing again does not clear it — the new run just queues behind the same held
group.

The tell is an aged `pending` or `queued` run whose jobs API remains exactly
`total_count: 0` and `jobs: []` across two reads around an exact-run identity
check, while sibling workflows from the same trigger dispatched normally. A
single empty response is not sufficient evidence, and a `pending` ↔ `queued`
transition is an ambiguous sweep rather than recovery.

```bash
ghapp api "repos/Generous-Corp/pulp/actions/runs/<run_id>" --jq '[.status,.conclusion]|@tsv'
ghapp api "repos/Generous-Corp/pulp/actions/runs/<run_id>/jobs" --jq '[.total_count, (.jobs|length)]'
# Re-read the run and jobs before acting. Clear only an exact superseded-head
# holder selected and revalidated by the merge steward:
ghapp api -X POST "repos/Generous-Corp/pulp/actions/runs/<old_run_id>/cancel"
```

Do not cancel a current-head run, rerun the obsolete head, or use `shipyard
rescue` on the stale run: rescue redispatches work, while this recovery is
cancel-only. The off-fleet `runner-health-check.yml` watchdog detects this
pre-expansion shape without mutating it; degraded API evidence cannot close or
maintain its tracker.

All three aliases now resolve their runner through `PULP_PREAMBLE_RUNS_ON_JSON`
(self-hosted) rather than a bare `ubuntu-latest`, which removes the mechanism:
an advisory lane can no longer strand a PR by being starved. Do not "simplify"
one back to `ubuntu-latest` —
`tools/scripts/test_windows_runner_policy.py` fails if you do.

### Advisory cross-lane workflow: `macos-cross-advisory.yml`

`.github/workflows/macos-cross-advisory.yml` is a path-scoped advisory
job for the Linux-hosted macOS arm64 cross lane (Phase 5 scaffolding,
see `planning/2026-05-24-linux-hosted-macos-arm64-cross-lane.md`). It
runs on `ubuntu-latest`, does **not** bootstrap osxcross, and does
**not** download a macOS SDK — it only confirms the Pulp-side cross
scaffolding (`tools/cmake/toolchains/macos-arm64-osxcross.cmake`,
`tools/scripts/verify_macos_cross_artifacts.py`, the
`PULP_RUST_CLI_TARGET` / `PULP_MACOS_CROSS_ALLOW_MISSING_ICON_TOOLS` /
`OTOOL` / `INSTALL_NAME_TOOL` hooks) stays wired and that the verifier
unit tests still pass. It is non-gating by design; do not promote it to
a required check until a self-hosted Linux runner with pinned osxcross
+ private SDK is provisioned and the matching full-build job lands.

### Advisory compile-gate: `windows-midi2-gate`

`build.yml`'s `windows-midi2-gate` job (`continue-on-error: true`, NOT a
required check, NOT part of the build matrix) compile-verifies Pulp's
opt-in WinRT MIDI 2.0 backend (`core/midi/platform/win/winrt_midi_device.cpp`,
gated by `PULP_HAS_WINRT_MIDI`). That backend consumes the
`Microsoft.Windows.Devices.Midi2` C++/WinRT projection, which ships
out-of-band with the Windows MIDI Services SDK — a GitHub-only NuGet, NOT in
the base Windows SDK, so no other lane can compile it. The job provisions it
through Microsoft's official **vcpkg port** `microsoft-windows-devices-midi2`
(it downloads the SDK NuGet, runs cppwinrt to generate the projection headers,
and exports the `Microsoft::Windows::Devices::Midi2` CMake target). Pins +
rationale live in `tools/ci/midi2/` (`vcpkg.json` + `README.md`). The default
Windows build never sets `PULP_HAS_WINRT_MIDI`, so this is purely additive.
Watch points: the port requires Windows SDK >= 10.0.26100.0 (windows-latest is
right at that floor), and the drafted backend's API surface may still drift
from the real `winrt::Microsoft::Windows::Devices::Midi2` namespace.

### Advisory compile-gate: `windows-ble-gate`

`build.yml`'s `windows-ble-gate` job (`continue-on-error: true`, NOT a required
check, NOT part of the build matrix) compile-verifies Pulp's WinRT Bluetooth-LE
scan backend (`core/midi/platform/win/ble_midi_win.cpp`). Unlike
`windows-midi2-gate`, the BLE GATT / advertisement APIs
(`Windows.Devices.Bluetooth*`) ship in the BASE Windows SDK cppwinrt
projection, so this gate needs NO vcpkg / out-of-band NuGet provisioning — the
default Windows build already compiles the backend (it links `WindowsApp` +
`runtimeobject`). The job is a fast, isolated `cmake --build … --target
pulp-midi` so a blind (macOS-authored) Windows TU gets a compile signal without
waiting on the full matrix. Watch point: the backend's
`winrt::Windows::Devices::Bluetooth::Advertisement` API surface is written
without a local Windows compiler, so signature drift surfaces here first.

### Advisory build-gate: `tracing-build.yml`

`.github/workflows/tracing-build.yml` (advisory, `ubuntu-latest`, NOT a required
check) is the only lane that builds the opt-in Perfetto tracing configuration
(`-DPULP_TRACING=ON`). Every other lane builds the default OFF config, so a
break in the ON path — the Perfetto amalgamation fetch/compile in
`tools/cmake/PulpTracing.cmake` or the trace macros lighting up in
`<pulp/runtime/trace.hpp>` — would otherwise land unnoticed. It configures
`PULP_TRACING=ON -DPULP_ENABLE_GPU=OFF` (Release), builds `pulp-test-tracing`,
`pulp-test-tracing-session`, `pulp-test-offline-tracing`, and
`pulp-test-ship-tracing-guard`, and runs them. GPU/Skia stay OFF because the
tracing tests only need `pulp::runtime` + `pulp::audio`, keeping the lane fast
and hostable on a stock GitHub runner. Watch point: `test_tracing.cpp`'s
"tracing is off by default" case asserts `kTracingEnabled == false` and is
designed to fail under ON, so the lane excludes exactly that case (Catch2
`~"tracing is off by default"`); the other suites are config-agnostic and must
fully pass under ON. `runs-on` is a hard-coded `ubuntu-latest` — never route it
to a self-hosted label or add it to branch protection.

## Governance is declared, and mirrors LIVE state — not aspiration

`.shipyard/config.toml` declares Pulp's branch-protection posture so
`shipyard governance {diff,apply}` can reconcile it, and it is checked in
alongside `.github/rulesets/main-protection.json`. Two hard rules:

- **The declared required checks must match the LIVE GitHub ruleset, not what
  we wish we enforced.** Live `main` requires exactly two contexts: `macos` and
  `Enforce version & skill sync`. Both the `[governance]
  required_status_checks` list in `.shipyard/config.toml` AND the
  `required_status_checks` array in `main-protection.json` are pinned to that
  two-context set. `[branch_protection."main"] require_strict_status = true`
  mirrors the ruleset's `strict_required_status_checks_policy`. Before editing
  either, run `shipyard governance diff` — a clean run prints
  `OK main: no changes`; any other output means the checked-in intent has
  drifted from live and you must reconcile (don't just re-declare).
- **Never promote `linux`/`windows` into the required set.** They validate as
  advisory GitHub-hosted lanes and deliberately do NOT gate merge. Making an
  advisory lane blocking craters throughput — a saturated or flaky advisory
  lane would wedge every merge — without adding signal. This is why the ruleset
  was trimmed from four contexts to two: so nobody can "fix drift" by pushing a
  config that flips the advisory lanes blocking. `test_ruleset_drift_config.py`
  asserts the two-context required set; keep it and the two config surfaces in
  lockstep.

## PR Review Thread Hygiene

Before opening a follow-up PR or declaring a phase complete, sweep review
threads for the PRs touched by that phase:

```bash
gh api graphql -f query='
query($owner:String!, $repo:String!, $number:Int!) {
  repository(owner:$owner, name:$repo) {
    pullRequest(number:$number) {
      reviewThreads(first:100) {
        nodes {
          isResolved
          isOutdated
          comments(first:20) {
            nodes { url body author { login } }
          }
        }
      }
    }
  }
}' -F owner=danielraffel -F repo=pulp -F number=<PR>
```

For every unresolved thread, either fix it in the follow-up branch or verify
that current `main` already fixed it. Leave a reply on the original thread with
the fixing commit/PR and the validation that proved it, so future sweeps can
distinguish addressed-but-unresolved GitHub state from actual pending work.

## Shipping a PR: route through `shipyard pr`

When the user says any of: **"push to main"**, **"ship this"**, **"ship it"**,
**"we're done"**, **"merge this"**, **"push it"**, **"run CI"**, **"push a PR"** —
run `shipyard pr` (not `gh pr create` + `shipyard ship` separately).

`shipyard pr` is the single orchestrator (Shipyard v0.19.1+; currently pinned
in `tools/shipyard.toml`). It:

1. Calls `tools/scripts/skill_sync_check.py` (resolved via Shipyard's
   `[validation]` path-discovery, explicit in `.shipyard/config.toml`) and
   hard-fails if a mapped skill path was touched without a `SKILL.md` update
   or a `Skill-Update:` trailer.
2. Calls `tools/scripts/version_bump_check.py --mode=apply` to bump SDK,
   Claude plugin, and marketplace versions consistently, honoring any
   `Version-Bump:` trailers. This applies `patch`/`minor`/`major` bumps —
   **including `patch`, which every `fix:` PR gets** (fixed in pulp #3626;
   before that, patch bumps were silently skipped and `fix:`/`feat:` PRs
   stranded at the gate, forcing a manual `chore: bump versions` commit — no
   longer needed).
3. Runs the no-build source-contract registry gate:
   `tools/import-validation/check-source-contracts.py --strict` plus
   `tools/import-validation/test_source_contracts.py`. This mirrors the
   GitHub `Versioning & Skill-Sync` workflow and the local pre-push hook.
4. Commits the bump (if any) as `chore: bump <surfaces>`.
5. Pushes the branch, creates the PR, and records Shipyard tracking state.
6. Runs cross-platform validate + merge on green.
7. Auto-release workflow (`.github/workflows/auto-release.yml`) tags and
   publishes binaries on merge. The full pipeline (tag → 5-platform build
   → sign + notarize → 11-asset publish) is documented end-to-end in
   [docs/guides/release-pipeline.md](../../../docs/guides/release-pipeline.md).
   **Keep that doc in sync when you touch any release workflow.**
   **There is NO supersede reaper, and you must not add one back.**
   `auto-release.yml` used to cancel in-flight `release-cli` / `sign-and-release`
   runs and DELETE the draft releases of any tag older than the latest published
   one, on the theory that an older SemVer is obsolete. Releases complete OUT OF
   ORDER here — the pipeline (70-165+ min) outlasts the gap between tags (~100
   min) — so that theory is false, and the reaper destroyed healthy releases whose
   binaries had all built green. It is why only 7% of tags published first-try in
   July 2026. `auto-release.yml` now has no `actions` scope, so it *cannot* cancel
   a run. Saving runner minutes is never worth losing a release.

Never run `gh pr create` + `shipyard ship` separately for a normal ship
cycle. Never invoke the two version/skill scripts by hand — `shipyard pr`
wires them together with the right flags.

For an agent-driven or unattended local ship, run the repository watchdog
instead of invoking the binary naked:

```bash
python3 tools/scripts/shipyard_pr_watchdog.py -- shipyard pr <args>
```

It passes output and exit status through unchanged. It only declares a stall
after five minutes without output, followed by a one-minute confirmation in
which the Shipyard process has no descendants and remains below 0.5% CPU. On
macOS it writes one `sample` diagnostic under `/tmp`, stops only Shipyard's
dedicated process group, and retries once so cached builds are reused. Missing
process telemetry fails open; a live quiet compiler or SSH child prevents a
restart. A second stall exits 124 instead of consuming another agent session.

**After opening/merging a material PR, sweep its review comments.** `merge on
green` fires before the automated reviewers (Codex, and cubic on Shipyard)
finish, so a PR can land with unaddressed P1s. For any logic-bearing or
destructive-path PR, follow the `pr-review-sweep` skill: read
`pulls/<n>/comments` (via `ghapp`), verify each finding against the code, and
ship a follow-up with a test for anything confirmed. Docs-only PRs can skip it.

Direct `gh pr create` is an explicit emergency/manual bypass only. If the
user asks for that path, state the tracking gap up front: the PR may not
appear in Shipyard-managed state or the macOS GUI until it is reconciled or
re-shipped through Shipyard.

`pulp pr` is a Pulp-side wrapper that delegates to `shipyard pr`; both are
valid, agents should prefer `shipyard pr` for directness. Humans can opt out
of Shipyard for their own checkout with `pulp config set pr.workflow github`
or `manual`, but agents should not choose those workflows unless the user
explicitly asks for a manual/emergency bypass. `pulp status` reports the
effective workflow and whether its required local tool is installed.

### Gotcha: amending after `shipyard pr` leaves the body describing the old commit

A PR body is written **once, at creation**, from the commit message — by
`shipyard pr`, `gh pr create`, and the web UI alike — and nothing updates it
afterwards. So an amend + force-push moves the commit and silently leaves the
body describing code that no longer exists. Reviewers do not read your commit;
they read that body.

It is worse than a stale description, because it meets the `COMMIT_OR_PR_TITLE`
squash policy documented below: on a **multi-commit** PR the body becomes the
landed commit message, so stale text is not merely misleading — it is written
into `main`'s history permanently. A one-commit PR squashes from the commit and
escapes this, which is exactly why the hazard stays invisible until the PR that
has two.

**After any force-push that changed a commit message, re-read the body and
refresh it** (`ghapp pr edit <n> --body-file …`), preserving any provenance
footer the original had. Nothing warns you; it is visible only if you go back
and check that your own edit still describes the branch.

### Gotcha: shipyard-merged PRs don't reliably auto-close linked issues

**Always verify an issue actually closed after a shipyard merge; close it
with `gh issue close` if it didn't.** Don't trust closing-keyword auto-close
to do it for you here.

What was actually observed (#3299, twice): both `Closes #3299` (no colon,
PR #3420) and `Closes: #3299` (with colon, PR #3413) were correctly
recognized as closing keywords — GitHub registered the closing reference in
**both** cases (`gh api graphql … closingIssuesReferences` returned
`[{number:3299}]` for each). Yet **neither** `shipyard-local[bot]` merge
auto-closed the issue. So:

- The **colon is a red herring** for auto-close. `Closes #N` and `Closes: #N`
  both register a closing reference; GitHub accepts either after the keyword.
  (An earlier version of this note wrongly blamed the colon — corrected here.)
- The real failure is that the **app-performed merge didn't trigger GitHub's
  issue-closing automation**. Verify with `gh issue view <N> --json state`
  after merge and `gh issue close <N> -r completed` if still open.

Separately — and this **is** a real colon rule, but for a *different* system
— git trailer bypasses (`Skill-Update:`, `Version-Bump:`,
`Release-Supersede:`) are parsed by `git interpret-trailers --parse`, which
requires the trailer paragraph to be **all** `Key: value` lines. A
colon-less `Closes #N` dropped into that paragraph breaks the parse and
silently voids every bypass trailer in it. So keep issue references like
`Closes #N` in the **body prose**, above an all-colon trailer block
(`Skill-Update: …`, `Co-Authored-By: …`); inside the trailer block, only
`Refs: #N` (colon) is safe. Dry-run `git log -1 --format='%(trailers)'` to
confirm the bypass trailers still parse — that part is unchanged and true.

Backward compatibility: raw `shipyard ship` / `shipyard run` still work for
diagnostics, experimental branches, existing Shipyard-managed PRs, or when
`shipyard pr` itself is being debugged. Do not use them as the primary ship
path.

### A ship must survive the session dying — arm GitHub auto-merge as a backstop

`shipyard pr` performs merge-on-green **inside the CLI/worker process**. If
that process dies before the merge — the cmux app relaunching under resource
starvation, or this Claude session running out of quota — the validated PR is
**stranded unmerged** and its ship-state record **orphans**. This is a recurring
real failure (521 orphaned records were reaped across the CI Macs on
2026-06-30). The merge must not depend on any interactive session staying alive.

**Standing policy — after creating/validating a pulp PR, arm GitHub-native
auto-merge as a server-side backstop:**

```bash
ghapp pr merge <PR> --auto              # NO strategy flag — the queue owns it
```

GitHub then merges the moment required checks go green, regardless of whether
`shipyard pr`, cmux, or this session survive.

**Pass no strategy flag.** `main` is merge-queue-governed, so the queue owns the
merge method and GitHub refuses any strategy outright:

```
! The merge strategy for main is set by the merge queue
```

That applies to `--merge` and `--squash` alike (verified on PR #6736,
2026-07-27), so the bare `--auto` above is the only form that arms. Historically
this section required `--merge` to keep a squash from folding the
`chore: bump versions` commit into the PR-title commit and tripping the
auto-release watchdog into a false "merged without bump" — that hazard is now the
queue's problem, not a flag you choose.

Arming is safe alongside `shipyard pr` — whichever merges first wins; the other
no-ops on already-merged.
(GitHub auto-merge works on the Shipyard repo too — verified arming it on
Shipyard PR #384 on 2026-07-27, which then merged on green. Use `--squash` there,
matching that repo's own history; the no-squash rule above is a pulp
auto-release-watchdog constraint, not a general one. The host-side queue janitor
below is a second layer, not the only one.)

**Host-side backstop (both repos + orphan reaping):** a launchd queue-tick on
each CI Mac (`tartci` `scripts/shipyard_queue_tick.sh`) periodically drives
in-flight ship-state to completion via shipyard's own fail-closed `auto-merge`
and reaps records whose PR GitHub reports merged/closed — independent of any
session. Reap a stale local pile by hand with `shipyard ship-state list` →
`shipyard ship-state discard <pr>` for each merged/closed PR (never discard an
OPEN one). Full design: pulp
`planning/2026-06-30-ship-queue-resilience-design.md`.

### The arm is not armed until you read it back — `update-branch` disarms it silently

An armed auto-merge is a backstop only if it is still armed. Two silent failures
sit between "I ran the command" and "the PR will land", and both were confirmed
on 2026-08-16 across two independent sessions:

1. **Clearing `BEHIND` disarms auto-merge.** `gh pr update-branch` on a PR with
   auto-merge armed leaves it `UNARMED` — observed on #7565, #7573, #7556, and
   again on #7572, #7569, #7557. The treadmill this repo already has (strict
   up-to-date protection, main advancing faster than the ~30-min gate) makes
   `update-branch` routine, so this fires often. It turns a delay into a trap: an
   agent walks away believing the PR lands on green, and it never does — sitting
   green and unmerged with no failing signal to attract attention.
2. **The obvious remedy also fails silently.** `update-branch` leaves the PR at
   `mergeable: UNKNOWN` while GitHub recomputes, and under a saturated queue that
   persists for minutes. Arming in that window **exits 0 and does nothing**:

   ```
   $ ghapp pr merge 7572 --auto ; echo "exit=$?"
   exit=0
   $ ghapp pr view 7572 --json mergeable,autoMergeRequest
   {"mergeable":"UNKNOWN","autoMergeRequest":null}     # the arm did NOT take
   ```

**So: never trust the exit code. Read the arm back.** After arming — and again
after any `update-branch` — verify, and retry while `mergeable` is `UNKNOWN`:

```bash
ghapp pr view <PR> --json mergeable,autoMergeRequest \
  --jq '{mergeable, armed: (.autoMergeRequest != null)}'
```

**Merge-queue enrollment is the durable form.** In both sessions the queue
enrollment survived `update-branch` (`already queued to merge`) where the
auto-merge arm did not. Prefer enrolling; treat a bare arm as the weaker
fallback, and one you must verify.

**`autoMergeRequest: null` is ambiguous — read `isInMergeQueue` before reacting.**
On a queue-governed branch, enrollment **supersedes** the arm, so a PR that is
queued and progressing normally reads `armed=false`. That is indistinguishable
from the disarm above if you look at `autoMergeRequest` alone, and re-arming on it
means fighting the queue rather than fixing anything. Query both:

```bash
ghapp api graphql -f query='query{repository(owner:"OWNER",name:"REPO"){
  pullRequest(number:NNNN){isInMergeQueue mergeQueueEntry{state position}}}}'
```

`isInMergeQueue: true` → armed=false is expected; leave it alone. `false` **and**
`armed=false` → genuinely unarmed, and the read-back rule above applies.

**A queued branch is push-locked, which matters when the fix is on your side.**
GitHub rejects a push to a branch with a queued PR (`GH006 … Branches that are
queued for merging cannot be updated`), so landing a correction means dequeuing
first. Note `gh pr merge --disable-auto` does **not** dequeue an already-queued PR
— it clears the arm and reports `already queued to merge`. The dequeue is a
GraphQL mutation, and its input field is `id`, not `pullRequestId`:

```bash
ghapp api graphql -f query='mutation{dequeuePullRequest(input:{id:"PR_kwDO..."}){
  mergeQueueEntry{state}}}'
```

Treat that as an authority action on someone's queued work, not a routine step —
`ghapp` guards it deliberately.

### Shipyard validated green but could NOT merge — the sanctioned fallback

Shipyard can validate every target and still fail its own merge call: a
malformed request (Shipyard ≤0.80.1 sent `autoMergeRequest{id}`, which GitHub's
schema rejects), an App-token permission gap (`Resource not accessible by
integration`), or a transient API failure. Its hand-back prints
`gh pr merge <n> --squash --auto` as the remedy, which raises a fair question:
does using `ghapp` here bypass the gates that `shipyard pr` owns?

**No — and this is already standing policy, not an exception.** Arming
GitHub-native auto-merge is a *request*, not a merge: GitHub still holds the PR
until every required check passes, and on a queue-governed branch the merge queue
still validates the merge result before landing. It bypasses nothing. The
backstop section above already *requires* arming it on every pulp PR for exactly
this reason. A handoff or prompt that says "do not use `ghapp`" is scoped to PR
**creation** and to anything that skips a gate (`--admin`, an immediate merge, a
hand-rolled `gh pr create`); read it that way, and if its wording is broader,
fix the wording rather than inventing a silent exception.

**Allowed** — arming auto-merge, nothing else:

```bash
ghapp pr merge <PR> --auto             # pulp main: no strategy flag
```

**Never** in this situation: `--admin`, an immediate (non-`--auto`) merge, a
force-push to "refresh" checks, or editing branch protection.

Ignore the strategy in Shipyard's hand-back. It suggests
`gh pr merge <n> --squash --auto`, and on pulp `main` any strategy flag is
rejected with `! The merge strategy for main is set by the merge queue` — the
command simply does not arm. Drop the flag.

**Before arming, prove the PR is actually green.** This is the step that was
missed on 2026-07-27, and it is the whole reason this rule is written down.
PR #6682 was reported as "genuinely green," and it was not: `Vellum freeze` and
`Vellum trusted freeze` were both red, and both are **required**. The mistake
came from checking rulesets, where nothing is required, instead of classic branch
protection, where five contexts are. `main`'s only branch ruleset
(`main-merge-queue`) carries a `merge_queue` rule and no `required_status_checks`
rule at all, so ruleset evidence alone always reads as "nothing is required."
Arming auto-merge on it was harmless (GitHub simply waited) but it did not merge
anything, and reporting it as a green PR blocked only by a Shipyard bug was
wrong. So:

```bash
ghapp api repos/Generous-Corp/pulp/branches/main/protection \
  --jq '.required_status_checks.contexts'      # what actually gates
ghapp pr checks <PR> --repo Generous-Corp/pulp # state of each
```

Every context in the first list must be `pass` in the second. If any is red,
there is nothing for auto-merge to unblock — fix the check. "Shipyard validated
its targets" is not the same claim as "every required check is green": Shipyard
supervises its own lanes, not GitHub-hosted or App-posted ones.

**Disclosure is mandatory.** In the report, state all four:

1. the Shipyard failure, quoted verbatim,
2. the exact command run,
3. the required-check evidence above — not "checks were green",
4. that gates were run by Shipyard and none were bypassed.

**A tracking issue is required** when the cause is a Shipyard defect (schema
error, malformed request, wrong exit code) — that is a bug that will recur for
every user until fixed, and `danielraffel/Shipyard` is where it gets fixed. It is
**not** required for a one-off transient API failure, or for a cause already
covered by an open issue; link the existing one instead.

Shipyard ≥0.80.2 also makes this classifiable without reading prose: the `--json`
envelope carries `status` and `merge_error`, and a malformed-request failure exits
`8` rather than masquerading as success. See the Shipyard `ci` skill's
status/exit-code table.

Shipyard v0.81.4 is the fleet floor for queue throughput and capacity health:
fleet status observes complete registered and expected-host inventories, Tart
disk/ccache admission problems, accidental hosted Linux routing, and stale
releases whose bounded commit scan lacks an oldest timestamp. The earlier
scheduler guarantee still refills
newly free worker slots as each target finishes instead of waiting for the whole
batch, and the release-version surface covers root-level `src/*.rs` so scheduler
fixes cannot merge without producing the CLI release the fleet pin expects. It
also keeps long Git-over-SSH pushes alive while Pulp's pre-push proof runs.
For post-tag reconciliation, v0.81.1 corrected shell tag extraction, v0.81.2
fully qualified branch push refspecs, and v0.81.3 attaches the deterministic
local PR branch before using Shipyard's supervised push. The last step is
required by repositories whose pre-push hook rejects detached HEAD or requires
`SHIPYARD_PR_RUNNING=1`.

### Stale-SHA merge race — DO NOT push onto a PR that's being shipped

**The failure mode (observed 2026-05-29):** Shipyard's `can_merge()`
validates the *exact merge-candidate SHA* and then merges that SHA. If a
developer/agent pushes new commits to the same branch while a `shipyard ship`
is mid-flight, Shipyard merges the **already-validated older SHA** and the new
commits are stranded on the branch — the PR squash-merges *without* them. In
the observed case a PR merged at its pre-fix SHA, dropping a whole review-fix
push; the fixes had to be re-landed via a fresh fast-follow PR.

`expected_head_oid` alone would NOT have caught it — the validated SHA *was*
GitHub's PR head at the merge instant; the new push only advanced the branch
ref moments later. The only true prevention is the merger re-checking the
branch tip immediately before merging and aborting if it advanced past the
validated SHA — that lives in Shipyard (tracked upstream as Shipyard issue
321). <!-- docs-noise-lint: skip — stable cross-repo tracking ref for the root fix -->.

**Operational rules (enforce these; they are the practical fix):**
1. **Never push commits onto a PR that has an in-flight `shipyard ship` or
   armed auto-merge.** Check `shipyard ship-state list` (shows PR/url/sha) and
   GitHub's auto-merge state first. If a ship is running, let it finish.
2. **Land post-review fixes as a fresh PR off `main`**, not as a push onto the
   already-green PR you just reviewed. (Review comes after green; the green PR
   is exactly when auto-merge fires.)
3. **After any ship, verify the merge actually carried your latest commits.**
   Don't trust "merged" — confirm the merge SHA's tree contains your changes.
   `git fetch origin main` then check a file you changed is present on
   `origin/main` (e.g. `git show origin/main:<path> | grep <marker>`). If
   you cross-check via `gh api repos/<o>/<r>/pulls/<n> --jq .merge_commit_sha`,
   note that the SHA alone does NOT prove your push made it — that field
   returns the PR's squash/merge commit on the base branch and exists for any
   merged PR, including the stale-SHA case. The SHA is only useful if you
   then inspect its tree/diff (`git show <sha>:<path> | grep <marker>` or
   `git show <sha> --stat | grep <expected-file>`). If your commits are
   missing, re-land them as a fresh PR immediately.
4. A degraded/rate-limited `gh pr view ... headRefOid` read can return a stale
   SHA — corroborate branch state with `git ls-remote` / a real `git fetch`
   before concluding the branch moved or was reset.

### Gotcha: a long-lived integration branch drifts from main's whole-tree gates

The hotspot-size guard (`tools/scripts/hotspot_size_guard.json` frozen ceilings)
and the codecov-config gates (`codecov.yml` flags/components mirroring the live
`core/*` tree) are **whole-tree, not diff-scoped** — they assert a property of the
entire checkout, so they fail on *any* push to a branch whose tree violates them,
regardless of what that push changed. A long-lived `develop/*` integration branch
accumulates this kind of drift relative to `main`:

- `main` may *lower* a frozen hotspot ceiling (e.g. after a comment-hygiene
  cleanup) while the integration branch has legitimately *grown* the same file
  (new test targets), so the merged tree exceeds the ceiling.
- The integration branch may *add a whole subsystem* (`core/<new>/`) and its
  upload flag without registering the matching `codecov.yml` component, so the
  flag/component-alignment and "every first-party file maps to a component" tests
  fail.

Either way the integration branch **cannot pass its own pre-push/CI gates**, and
the failure surfaces on the first unrelated PR that tries to land on it (looks
like the PR's fault; it is not). Fix by reconciling the gate config to the
branch's real state **in the landing PR**: raise the hotspot ceiling to the file's
current LOC, and add the missing codecov flag+component (`paths: core/<new>/**`).
Run `tools/scripts/gates.sh <integration-branch>` locally first — the fast gates
are sub-second and catch all of this before a multi-minute ship cycle. Reconciling
config you didn't author trips skill-sync/version-bump (config maps to the `ci`
skill; examples/config-only diffs still need a `Version-Bump: skip` trailer under
a `feat:`/`fix:` title) — expect to add those trailers too.

### Shipyard pin and behaviour notes

#### Shipyard cannot merge under a merge queue — it errors, and that is expected

`shipyard pr` is still the right way to create a PR: it runs the gates, applies
version bumps, pushes the branch, and opens and tracks the PR. It **cannot land
it**. Confirmed at tag v0.78.0, `src/wait.rs:189`:

```rust
if merge_state.contains("RULESET") || merge_state == "MERGE_QUEUED" {
    return Err(Box::new(UnsupportedScopeError(
        "Rulesets / merge-queue governance isn't supported by
         `shipyard wait pr --state green` yet …")))
}
```

So a ship that reaches the wait/merge step under the `main-merge-queue` ruleset
fails there. **That is not a broken branch and not a Shipyard bug to work
around** — the ruleset (`bypass_actors: []`, `current_user_can_bypass: never`)
is what makes the queue the real merge authority rather than theatre. Do not
admin-merge past it. Enqueue instead:

```bash
ghapp pr merge <n> --repo Generous-Corp/pulp --auto
```

The PR enters the queue once its required contexts are green. Note GitHub uses
the **latest** run for a required context, so a newly-queued re-run makes an
already-green context pending again and delays entry — that resolves itself.

#### The pin is load-bearing at v0.78.0 because of the post-tag hook

`[release.post_tag_hook]` runs `shipyard changelog regenerate` after every SDK
tag and pushes `CHANGELOG.md` directly to `main` — which the queue ruleset
refuses. The tag and binaries still publish; the changelog sync fails, retries
`max_push_attempts` times, and leaves a red run plus a stale CHANGELOG.

v0.78.0 adds `push_mode` to that hook; `push_mode = "pr"` opens a pull request so
the changelog lands *through* the queue. A fleet Mac still on an older Shipyard
silently ignores `push_mode` and reverts to the direct push, so the pin, the
installed binary on every host, and the config setting must move together.

#### `shipyard update` is an updater, not a converger — it will not go backwards

`shipyard update --to vX.Y.Z` silently does **nothing** when `X.Y.Z` is older
than the installed version: it reports `update_available: false` and exits 0.
Verified against v0.70.0 — asked for `--to v0.60.0` with 0.70.0 installed it
reports no update available, exactly as it does for an already-current
machine. The two outcomes are indistinguishable from the exit code.

Two consequences:

- **A bare `shipyard update` tracks `latest`, not the pin.** Run it on a fleet
  machine and that machine leaves the pin permanently — `latest` ran 7 minors
  ahead of the pin on 2026-07-16 (pin v0.70.0, latest v0.77.1). It is then
  running a Shipyard that was never validated against Pulp's CI matrix and
  that disagrees with every workflow's `SHIPYARD_VERSION` (the exact drift
  `check_shipyard_pin.py` exists to prevent). Always `--to` the pin.
- **Coming back to the pin needs `tools/install-shipyard.sh`**, which installs
  the pinned version unconditionally (via the upstream, checksum-verifying
  `install.sh`). Routing a downgrade through `shipyard update` is a silent
  no-op, so an ahead-of-pin machine would never converge.

Never trust either path's exit code alone — re-read `shipyard --version` and
compare it to the pin. `tools/scripts/shipyard_autoupdate.py` encodes all of
this (direction dispatch + outcome verification); `--check --json` reports
pin-vs-installed without touching anything.

#### Optional: keep a fleet Mac on the pin automatically

`tools/scripts/install_shipyard_autoupdate.sh` installs an hourly launchd agent
that converges this machine onto the pin when it is idle. Opt-in per machine
and irrelevant to public Pulp (which just runs `install-shipyard.sh` once).
Kill switch, no uninstall needed:

```bash
echo off > ~/.config/pulp/shipyard-autoupdate   # stop; `on` resumes
tools/scripts/install_shipyard_autoupdate.sh --status
```

Two gotchas worth knowing if you touch it:

- **The pin it obeys is `origin/main`'s, not the working tree's.** A dev
  checkout is routinely parked on a feature branch, and a branch may carry an
  experimental pin; converging the machine onto that would be a bug. Override
  with `PULP_SHIPYARD_AUTOUPDATE_PIN_REF=worktree`.
- **The idle probe must parse the `shipyard` command line, not substring-match
  it.** The persistent daemon runs as `shipyard --mode shipyard daemon run` —
  a substring match on `run` reads it as a live ship and the machine then never
  updates at all, while `--mode shipyard` puts the literal token `shipyard`
  where a subcommand would be.

Pin bumps must go through `shipyard pin bump --to vX.Y.Z`, not a hand edit.
Shipyard v0.50.0+ is Rust-backed and macOS ships as an Apple-Silicon-only
signed/notarized `.dmg`, so the version and asset metadata must move together.

- **Pinned at v0.68.0+: a killed `shipyard pr`/`ship` worker no longer wedges
  the PR.** Before v0.68.0, a worker that died (crash, `kill`, launching a
  second `shipyard pr` for the same PR) left its job stuck `running` in the
  durable queue, and every later same-PR ship was refused with
  `SamePrShipRunning` — recoverable only by hand-editing `queue.json`. At this
  pin the queue auto-reaps a dead-worker job (heartbeat stale >180s) at
  ship-submit time and on each drain pass. So if a same-PR ship is refused as
  "already running" and nothing is actually live, just **retry after ~180s**.
  Still: never run two `shipyard pr` for the same PR concurrently.
- **Installing/upgrading Shipyard on macOS uses the GitHub API**, which is
  rate-limited at 60/hr unauthenticated. A "No binary found for
  shipyard-macos-arm64" error from `install.sh` / `shipyard update` is almost
  always that rate limit, not a missing asset — re-run with `GITHUB_TOKEN` set
  (`./tools/install-shipyard.sh` runs in an authenticated context).

- **Release SDKs are expected to include desktop WebView symbols**
  (pulp #695). `.github/workflows/release-cli.yml` now configures the
  release build with `-DPULP_BUILD_WEBVIEW=ON`, installs Linux's
  `libgtk-3-dev` + `libwebkit2gtk-4.1-dev`, and verifies the staged
  `pulp-view-core` archive still contains `WebViewPanel` and
  `make_webview_embedded_resource_fetcher`. If you touch the release
  workflow or `tools/scripts/release-cli-local.sh`, preserve that
  contract or WebView-using downstream SDK consumers will link-fail.
- **Release bodies are composed in `release-cli.yml`.** The release job
  runs `tools/scripts/compose_release_notes.py` for grouped Highlights and
  calls GitHub's generated-notes API for the native "What's Changed" /
  "Full Changelog" block before the Install section. `CHANGELOG.md`
  remains Shipyard-owned via `shipyard changelog regenerate`; do not route
  the Release body back through the deleted in-tree changelog generator.
- **Phase 8 CLI flip ships two CLI binaries.** Release CLI jobs must
  preserve Rust `pulp` as the user-facing binary and C++ `pulp-cpp`
  as the fallthrough delegate in the same archive. Smoke both names:
  `pulp version --json` for the Rust path, and at least one C++-owned
  command through `PULP_RS_CPP_BINARY=/path/to/pulp-cpp pulp ...` or a
  direct `pulp-cpp ...` invocation. Do not resurrect `pulp-rs` as the
  shipped binary name.
- **macOS binary is signed + notarized** (Shipyard v0.29.0). On
  macOS 26.3+ XProtect skips the deep scan for notarized binaries,
  cutting `shipyard pr` cold-start ~4-5x (from ~5-6s to ~1-1.5s).
  No pulp-side action; transparent.
- **Format baseline diff is a plugin-only gate.** Preserve
  `-DPULP_ENABLE_GPU=OFF` in `.github/workflows/format-baseline-diff.yml`:
  the self-hosted macOS runner may not have the pinned Skia archive, and this
  workflow only needs the PulpEffect AU/VST3/CLAP bundles, not GPU examples
  such as `pulp-design-tool`.
- **Build-and-Test workflow_dispatch is Shipyard PR validation.** Preserve
  `-DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF` on the
  workflow_dispatch configure path in `.github/workflows/build.yml`: the local
  self-hosted macOS runner may not have the pinned Skia archive, and no-GPU
  dispatches must not link example bundles that require the GPU plugin view
  host. Pull-request validation also disables example bundles, while nightly /
  release workflows own full example/product and GPU coverage.
  When adding optional shell arguments in `build.yml` (for example macOS-only
  `-G Ninja`), use bash arrays and expand them as `"${args[@]}"`; scalar
  `$args` trips actionlint/shellcheck word-splitting checks.
  Shipyard's `workflow_dispatch` payload has no `pull_request.base.sha`, so its
  shallow build checkout must fetch `origin/main` before CTest runs the
  fail-closed agent-capability history gate. Do not remove that focused fetch
  unless the dispatch contract starts supplying an exact protected-base SHA.
- **Heartbeat line during long validation** (Shipyard v0.29.0). A
  20-minute lane now prints periodic progress instead of leaving a
  silent terminal. Helpful when watching `shipyard ship` interactively.
- **Backend errors are surfaced under the summary table** (Shipyard
  v0.28.0). A bare `ubuntu     error     ssh    12s` row used to
  give zero diagnostic signal — the captured stderr tail (bundle
  upload failure, remote `cmake` apply failure, SSH transport
  error, etc.) now prints below the table. Closes pulp #665's
  diagnosis-blind-spot complaint.
- **Worktree-local `.shipyard.local/` falls back to the primary
  checkout** (Shipyard v0.27.2). Pulp uses worktrees heavily for
  parallel agent work; without this every new worktree had to
  manually `cp .shipyard.local/config.toml` from the primary repo
  before `shipyard pr` could see the SSH host config. Now it
  inherits automatically.
- **Ship preflight runs BEFORE `gh pr create`** (Shipyard v0.27.1).
  Earlier the PR was opened first and ship aborted on unreachable
  SSH backend, leaving stranded PRs with no validation (the
  Apr 22 pattern that left several pulp PRs mid-flight). Now an
  unreachable target fails fast and the PR is never opened.
- **Daemon tunnel supervisor** (Shipyard v0.27.0). Tailscale Funnel
  transients no longer kill the daemon — the supervisor restarts
  the funnel on backoff. Periodic reconcile loop runs independently
  of per-PR polls. Both apply to the macOS GUI's webhook delivery.
- **Long-running daemons keep accepting fresh subscribers** (Shipyard
  v0.26.0). The subscribe-replay path now uses blocking `put()`
  instead of `put_nowait()`, so once the replay ring grows past 64
  events the daemon no longer silently stops handing new IPC clients
  their initial snapshot. This was the root cause of the macOS GUI
  falling back to "polling" mode and showing no active PRs after
  enough ship-state churn.
- **Daemon/CLI drift is now diagnosable over IPC** (Shipyard v0.26.0).
  IPC protocol is bumped to 2, daemon hello/status frames advertise
  the daemon's own `shipyard_version`, and `shipyard doctor` flags a
  daemon-vs-CLI version mismatch explicitly instead of leaving stale
  subscribers to fail mysteriously.
- **Auto-PR titles and bodies use the feature commit** (Shipyard v0.24.0 /
  Shipyard #151). The orchestrator walks past the mechanical
  `chore: bump versions` commit when composing the title/body, and scrubs
  the `Automated by shipyard pr.` tool-branding text. Pulp PR #624 was the
  canonical repro before the fix. Previously the auto-PR pointed at the
  bump commit — generic and uninformative. Shipped PRs now read as
  first-party.
- **`Version-Bump:` trailers are authoritative, not ceiling-raising**
  (Shipyard v0.25.0 / Shipyard #152). An author-declared
  `Version-Bump: <surface>=patch reason="..."` is no longer silently
  raised to `minor` when the conventional-commit heuristic on other
  subjects classifies the diff as `minor`. This matches the pulp-side
  behaviour in `tools/scripts/version_bump_check.py` at this pin.
- **`shipyard ship-state list` is served from the daemon via IPC** when
  `shipyard daemon` is running (Shipyard v0.25.0 / Shipyard #154). The
  PyInstaller cold-start (~5-6s) is bypassed. Callers that tight-loop
  over ship-state — the macOS GUI polls every 7s; `pulp pr` preflight
  calls it indirectly — see a meaningful CPU saving. Nothing to do at
  the pulp side; it's transparent.

### SSH preflight (v0.20.0+ / Shipyard #106)

Exit codes are distinct:

| Exit | Meaning |
|------|---------|
| 0 | Success |
| 1 | Validation failed |
| 2 | Configuration error |
| 3 | Backend unreachable (new; surfaces within 10s with classified reason) |

The unreachable-backend error names the failure class (auth / host_key /
network / timeout / configuration / unknown) and prints the last ssh stderr.

Flags:

- `--skip-target NAME` — **DELIBERATE** lane skip (no probe runs). Use when
  you know a target is irrelevant for this PR.
- `--allow-unreachable-targets` — proceed despite an unreachable backend.
  Prints a loud `⚠︎ VALIDATION GAP: <target> skipped` banner. Use only when
  you genuinely cannot reach a backend and accept the validation gap.

Automation (crons, agents) should branch on exit code 3 specifically rather
than parsing error strings.

## Tool selection: Shipyard (primary)

**Shipyard is Pulp's primary CI tool.** All merges, validations, and
ship cycles should use Shipyard. `local_ci.py` remains in the repo as
a fallback but is scheduled for removal after a 2-week observation
period (see Generous-Corp/pulp#120).

### Central merge steward (Shipyard v0.88.4+)

`.github/workflows/shipyard-merge-steward.yml` is the single logical
repository-wide PR landing controller. Manual dispatch is dry-run by default;
the ten-minute schedule is present but commented out until one manual apply +
recovery dispatch proves the recovery judgement wiring end to end. Once
enabled, it applies deterministic mutations and may dispatch at most one fenced
recovery exception per serialized tick. It runs on GitHub-hosted Ubuntu so
all three Macs may be offline, serializes mutations with one repository-scoped
concurrency group, restores a small bounded-retry cache, uses GitHub's durable
run-attempt counter to prevent retry-budget reset after cache loss, and configures its ephemeral
machine-global authority as `github-actions` before invoking
`shipyard runner steward`.

Queue and status mutations use a repository-scoped installation token minted
from `SHIPYARD_APP_ID` and `SHIPYARD_APP_PRIVATE_KEY`, not the workflow
`GITHUB_TOKEN`: GitHub suppresses downstream workflow events for mutations made
with `GITHUB_TOKEN`, which would prevent required `merge_group` checks from
starting. The job runs only from `refs/heads/main`, checks out `main` without
persisted credentials, and passes the short-lived App token only to Shipyard's
authority and reconciliation steps.

Only an exact head carrying both the `shipyard:managed` label and a successful
current-head `shipyard/steward-handoff` status is eligible for mutation. PRs
without that contract remain visible as `unmanaged` and are never adopted
implicitly. Routine checks, queue admission, merge confirmation, and cleanup
use no model. A code/test/conflict blocker receives one deduplicated
`shipyard:needs-agent` signal plus a failed `shipyard/steward-recovery` status;
the recovery dispatcher is a separate exception path.

`shipyard pr` does **not** imply this durable controller handoff. After the PR
exists and its remote head is final, the submitting agent must run:

```bash
shipyard runner steward-handoff \
  --repo Generous-Corp/pulp \
  --pr "$PR_NUMBER" \
  --head "$EXACT_REMOTE_HEAD" \
  --workstream-id "$WORKSTREAM_ID" \
  --context-url "$DURABLE_CONTEXT_URL" \
  --apply --json
```

Then re-read GitHub and verify that the same head has a successful
`shipyard/steward-handoff` commit status. The managed label by itself is not a
receipt and is not head-specific. An agent may stop watching only after this
server-owned receipt exists; local Shipyard state is never sufficient for
cross-machine continuation.

`--workstream-id` must be a **canonical `GEN-<n>` handle**, uppercase. Shipyard
validates it before it does anything else and rejects everything else with
`--workstream-id must be a canonical GEN-style handle`. Probed against
shipyard 0.155.2 in dry-run (dry-run is the default, so this is safe to repeat):
`GEN-7` and `GEN-8033` pass validation, while `pulp-pr-8033`, `PULP-8033` and
lowercase `gen-7` are all rejected. A PR-scoped handle is no longer accepted.

The handle is a *durable work item identifier*, so do not mint a `GEN-<n>` that
maps to no work item just to satisfy the validator: that is fabricated
provenance in the one field whose whole purpose is cross-machine continuation.
When a change genuinely has no work item, leave the handoff to the durable
controller rather than inventing a number.

**`shipyard pr` performs this handoff itself** when `.shipyard/config.toml` sets
`[merge_steward] auto_handoff = true`, and it is the *last* step. So its failure
is late: the branch is already pushed and the PR already open by the time the
error prints, and the non-zero exit reads like nothing shipped. Check with
`ghapp pr list --head <branch>` before doing anything else. To recover, resume
the existing PR:

```bash
shipyard ship --pr "$PR_NUMBER" --base main
```

Never re-run `shipyard pr` to recover: it targets a PR that already exists.

Each tick also reconciles one labeled GitHub issue containing every current PR
exception and control-plane error. The issue is updated in place, closes at
zero exceptions, and reopens on recurrence. This is the durable, model-free
operator outbox; Actions artifacts remain bounded evidence rather than the only
copy of work that needs attention.

Repository automation must never create an unlabeled PR. In particular,
`version_at_land.py --route pr` creates each `release/version-bump` PR with the
`automation` label in the same `gh pr create` transaction; a later label repair
is not an acceptable provenance window.

Do not add a second scheduled or per-Mac mutating controller. M1, M3, and M5 may
serve as fenced recovery workers only after disposable-runner proof; they do
not independently poll or mutate the merge queue.

The Shipyard CLI does not wake itself up. Once the schedule is enabled, GitHub
Actions is the durable clock: the scheduled controller re-reads GitHub truth,
reconciles the exact-head ledger/outbox, and invokes Shipyard without a model.
Recovery runners consume only the exceptional durable jobs that controller
emits; they are not a second polling authority.

Enable the ten-minute schedule only after live canaries prove an unmanaged
negative control, exact-head handoff, native queue landing without an agent,
one deduplicated recovery signal, and clearing that signal on a corrected head.

When a GitHub Actions controller mints a repository-scoped installation token
for `enqueuePullRequest`, request both `permission-merge-queues: write` and
`permission-contents: write`. The dedicated queue permission alone is not the
repository write access GitHub requires for queue enrollment; downscoping
contents to read fails closed with `Resource not accessible by integration`
even when the App installation itself has both permissions.

`.github/workflows/shipyard-recovery-worker-canary.yml` is the manual, read-only
precondition for recovery workers. Its one job requires the unique
`shipyard-recovery-canary-m5-20260814` label in addition to the disposable VM
labels, revalidates the open PR's exact head and a positive assignment epoch,
and launches no model. Serve it only with a one-shot Tart JIT runner carrying
that exact label. Do not generalize the label, add a persistent service, inject
Subrouter credentials, or enable M3/M5/M1 failover until this proof passes and
the runner is destroyed.

**Recovery dispatch must queue before a JIT runner exists.** Never select a
recovery worker from `actions/runners`: a healthy idle Tart JIT worker is absent
from that census, so preselection creates a circular wait in which no job is
queued and no runner registers. Dispatch one durable job to the shared
`shipyard-recovery-pool` first, record its exact Actions URL in the pending
recovery status, and let a fenced one-shot runner claim it. Derive the actual
worker only from the registered runner-name prefix inside the job. A pending
pool assignment is an offline-safe obligation, not an age-based retry signal;
do not dispatch a duplicate merely because it has waited. Prefer hosts without
encoding priority in GitHub labels by giving their TartCI supervisors increasing
minimum queued ages (M3 first, then M5, then M1). A returning faster host cannot
preempt a job that another runner has already claimed.

**Keep recovery routing and repair activation explicit.** The M3 recovery
endpoint is a non-secret repository variable
(`vars.SUBROUTER_RECOVERY_BASE_URL`); only the admin bearer belongs in the
protected `SUBROUTER_SESSION_LEASE_ADMIN_TOKEN` secret. Mint the short-lived
model-bound lease in trusted default-branch code before fetching the untrusted
PR head, then expose only the returned scoped broker credential to the model.
When the steward dispatches the recovery workflow it must set both
`publish_status=true` and `attempt_repair=true`: omitting the latter silently
turns the autonomous path into triage-only monitoring. This does not bypass the
cost or safety fence—Luna must still return the exact `needs_sol_fix`
classification before the single networkless Sol-medium repair step can run,
and the GitHub-hosted publisher independently revalidates the exact head,
assignment epoch, and blocker fingerprint before applying any patch.

**The recovery worker has two model lanes: Codex primary, Claude fallback.**
Codex Luna/low triage and Sol/medium repair run first and are unchanged. Each
now carries `continue-on-error` so that a failing model does not abort the job
before the bounded fallback can run. The fallback is deliberately *reactive*,
gated on `steps.triage.outcome == 'failure'` and
`steps.repair.outcome == 'failure'`, because **a Subrouter lease mints
successfully even when its account is out of quota** — exhaustion only surfaces
when the model actually runs, so no preflight probe can detect it. This was
proven on 2026-08-16 when all five Codex accounts hit their weekly limit and the
recovery canary failed 44 seconds into `luna-triage` with every fence, lease,
and teardown behaving correctly.

Four rules govern the fallback lane:

- **Mint it model-unbound.** Subrouter rejects any request whose body model
  differs from a bound lease, and Claude Code issues background calls on a small
  fast model. Send `provider:"claude"` with no `model` field; an empty model
  short-circuits that validation, but an empty provider *and* empty model
  defaults the lease back to Codex.
- **Consume the lease through the environment.** A Claude lease returns
  `ANTHROPIC_API_KEY`, `ANTHROPIC_AUTH_TOKEN`, `ANTHROPIC_BASE_URL`, and
  `CLOUDMUX_SUBROUTER_LEASE_TOKEN`. The base URL is bare — only Codex, Kimi, and
  ZAI leases get path suffixes. There is no `--base-url` flag.
- **Keep the context deliberately minimal.** A default Claude Code launch loads
  plugin and MCP context that dwarfs the bounded recovery prompt (roughly 60k
  cache-creation tokens versus 35k for an isolated launch), so the lane pins
  `--strict-mcp-config`, an empty `--mcp-config`, `--settings
  '{"disableAllHooks":true}'`, `--no-session-persistence`, and an isolated
  `CLAUDE_CONFIG_DIR`.
- **Install the platform package by name.** `@anthropic-ai/claude-code`'s
  `bin/claude` is a wrapper that resolves its native binary during
  `postinstall`, and the lane installs with `--ignore-scripts`, so the base
  package alone yields `claude native binary not installed` — this failed a live
  recovery job on 2026-08-16, 15 seconds in. Install
  `@anthropic-ai/claude-code-darwin-arm64` explicitly, integrity-pin it like the
  base package, and `ln -sf` the real binary onto `bin/claude`. Do **not** fix
  this by dropping `--ignore-scripts`: the install runs on a disposable runner
  moments before it holds a broker lease, so a script-free install is the point.
  Codex's own install is not a template here — it ships its platform binary
  differently.
- **Strip `$schema` before passing a schema to `--json-schema`.** The CLI's
  validator cannot resolve `"$schema": "https://json-schema.org/draft/2020-12/schema"`
  and rejects the committed file verbatim with `no schema with key or ref` —
  this failed a live recovery job on 2026-08-16. Pass
  `jq -c 'del(."$schema")' <file>`; every constraint is preserved and the fenced
  validator re-checks the payload against the full schema afterwards. Note the
  trap that hid it: a hand-written inline schema in a local probe has no
  `$schema` key, so rehearsing with a lookalike passes while the real artifact
  fails. Rehearse with the committed file.
- **Re-validate the output locally.** `claude --json-schema` validates upstream
  but leaves no trusted local proof, so
  `tools/scripts/shipyard_recovery_result_check.py` re-checks the payload. That
  path is inside the `FORBIDDEN_PREFIXES` fence on purpose: a validator living
  outside the fence could be weakened by the very repair model it constrains.

If both lanes fail the job errors rather than reporting a green tick with no
classification, and the publisher's commit trailers record the lane that
actually produced the patch (`Agent: codex` vs `Agent: claude`) so the audit
trail never misattributes a repair.

**The repair-publication path is the part no canary has exercised.** Every live
canary so far ended at triage (`no_action`), so the bundle/validate/commit/push
steps have never run against a real model patch. Seven defects were found by
reading it adversarially rather than by a red run, and each is a class that
would have shipped a *wrong* repair while every check reported success:

- **Diff against the fenced commit, never the index.** `git add
  --intent-to-add --all` fully stages a DELETION, so a worktree-vs-index
  `git diff` silently drops deleted and renamed files from *both* the patch and
  the declared path list — and the publisher's parity check still passes,
  because both lists are identically wrong. The lane force-pushes a "fix" that
  does not delete what the model deleted. Use `git reset --mixed
  "$EXPECTED_HEAD"` then `git diff HEAD`; resetting to the assignment's exact
  head rather than `HEAD` also survives a model that committed.
- **Reset the worktree before the fallback repair.** Both repair lanes edit the
  same checkout, and the primary lane runs under `continue-on-error` — it can
  fail *after* editing (a mid-task error, or tripping its own enum check).
  Without `git reset --hard "$EXPECTED_HEAD" && git clean -qfd`, a failed
  Codex attempt's partial edits are bundled into the fallback's patch and
  attributed to Claude by the commit trailers.
- **`--settings` does not stop project settings from loading.** cwd is inside
  the untrusted PR head, so a PR-supplied `.claude/settings.json` is read unless
  the lane pins `--setting-sources user`. Valid values are `user,project,local`;
  the CLI rejects anything else, so the flag fails loudly if it ever changes.
- **Keep the trailers one paragraph.** One `-m` per trailer makes each its own
  paragraph and `git interpret-trailers --parse` reads only the last, so the
  audit trail collapses to `Router: subrouter`. Build the message with `printf`
  into a variable — a literal multi-line `-m` inside a `run: |` block dedents to
  column zero and breaks the YAML block scalar.
- **Pin `LC_ALL=C` on BOTH sorted path lists.** The worker and the publisher
  run on different hosts and the parity check is a plain `diff -u` of the two.
  C and `en_US.UTF-8` genuinely order `-`, `_`, and case differently for
  ordinary source paths (`LC_ALL=C` gives `A-b a-b a_b ab`; `en_US.UTF-8` gives
  `a_b a-b A-b ab`), so an unpinned sort rejects a correct patch on locale
  alone. Pinning one side is worse than pinning neither.
- **Whitespace is a warning on BOTH sides, or on neither.** Trailing whitespace
  is not grounds to discard a repair that fixes a real blocker. Demoting only
  the worker's `--check` moves the rejection to the publisher — *after* the
  lease and model are already spent — instead of removing it. The security
  fences (control-plane prefixes, path escape, size caps, exact
  head/epoch/fingerprint revalidation, parity) are what protect this path;
  whitespace is not one of them.
- **`skipped` is not `failure` in the publisher's classification.** A repair the
  model marked `fixed` whose push step never *ran* leaves
  `steps.repair_push.outcome` as `skipped`; a bare `= failure` test falls
  through to a success classification and tells the steward the dispatch
  completed cleanly while the authorised repair evaporated. Gate the extra
  clause on `repair_outcome = fixed`, not a blanket `!= success` — `skipped` is
  legitimate when no repair was attempted.

`tools/scripts/test_shipyard_recovery_worker_workflow.py` pins all seven. They
are text assertions against a workflow that CI cannot execute end to end, so
verify them by mutation: revert a fix, confirm exactly one test fails.

Because the assertions are textual, they prove the workflow *says* the right
thing, not that the path *works*. Rehearse the semantics separately in a
scratch git repo: build a synthetic model patch that modifies, deletes,
renames, and adds; run the real bundle commands and the real
`shipyard_recovery_repair.py`; then apply the patch in a second checkout at the
fenced head and compare. Under the pre-fix commands that rehearsal reports
`validator: PASS` and `parity check: PASS` while leaving the deleted file in
place — which is precisely why no amount of green CI would have caught it.

### Why your macOS leg routed to `[github-hosted]`, and why re-running did not fix it

**Historical incident only.** This section describes the pre-event-class
busy-count overflow implementation. Live required-gate overflow is now
`local-only`; do not use the thresholds, M1-only labels, or dispatch pacing
below to reason about the current M1/M3/M5 JIT pool.

Two independent traps, found across three days and roughly six mis-routed
required legs on 2026-08-18. Together they are the whole story.

**1. `gh run rerun --failed` freezes the routing decision.** `build.yml` computes
routing in a `resolve-provider` job and bakes the answer into the downstream job
*name* (`macOS (ARM64) [local]` vs `[github-hosted]`). `--failed` re-runs only
**failed** jobs — and `resolve-provider` **succeeded**, so it is carried forward
and every retry re-uses the *original* dispatch's decision, however stale.

Verified: on one run's attempt 3, `resolve-provider` showed `2026-08-17` while
the macOS job showed `2026-08-18`. Same PR, same head, same host state; the only
variable was the re-run mode, and it produced opposite labels.

> **To change routing, use a full `ghapp run rerun <id>` — never `--failed`.**
> It re-executes `resolve-provider` against current conditions. It does not move
> the head, so head-exact Shipyard receipts survive. It costs re-running green
> advisory lanes, which is nothing next to a leg that never executes.

A run that is still `queued` refuses a re-run (`This workflow is already
running`) — cancel it first; cancelling does not move the head either.

**2. Every intuitive capacity check reads the wrong thing.**
`_count_busy_local_mac_runners()` counts **other `in_progress` "Build and Test"
runs whose macOS job is RIGHT NOW `in_progress` with the local self-hosted label**.
Overflow fires at `PULP_LOCAL_MAC_OVERFLOW_THRESHOLD` (3). Queued runs,
not-yet-registered jobs, and completed jobs are deliberately excluded — that
exclusion is scar tissue from an older pessimistic fallback that starved the
local runners during deep queues.

| what people reach for | why it is wrong |
| --- | --- |
| `tart list` VM count | measures the machine, not in-flight GitHub jobs |
| load average | **never a routing input**; affects duration only |
| org runner-group `busy` | wrong scope, and ephemeral runners are invisible while idle |

**All three measure the host. The selector measures in-flight jobs** — and
host-level signals cannot see merge-queue work at all, which is usually what is
holding the slots.

The probe that actually predicts routing:

```sh
for r in $(ghapp api "repos/OWNER/REPO/actions/runs?status=in_progress&per_page=100" \
      --jq '[.workflow_runs[]|select(.name=="Build and Test")|.id]|.[]'); do
  ghapp api "repos/OWNER/REPO/actions/runs/$r/jobs" \
    --jq '[.jobs[]|select((.name|test("macOS \\(ARM64\\)")) and .status=="in_progress")|.labels|join(",")]|first // empty'
done | grep -c self-hosted
```

Dispatch only when it reads **< 3**.

**Two consequences that bite in practice:**

- **Merge-queue runs consume the same budget.** They are "Build and Test" runs
  with local macOS legs, so a working queue holds slots and starves PR
  dispatches — often leaving only ~1 of 3 available.
- **Serial dispatch self-defeats.** Dispatching PR B right after PR A's leg
  *starts* guarantees B sees a higher count than A did. Wait for A's leg to
  **finish**, not to start. Two PRs three minutes apart routed oppositely for
  exactly this reason: A's leg started 11 seconds before B's resolver ran.

**The probe is read at the wrong moment by default.** The counter above is
correct; the *timing* is not. `resolve-provider` does not run at dispatch — it
runs roughly **two minutes later** and reads the counter then. Anything claiming
a slot in that gap changes the answer, including a dispatch you made moments
earlier:

```
[7679] BUSY=1  -- dispatched      23:38:03Z
[7679] resolver_started=          23:40:07Z   ->  [github-hosted]
```

One slot busy at dispatch; three by the time the resolver counted, because our
own previous dispatch had just gone `in_progress` and a merge-queue run started.
**The dispatch was starved by its own predecessor.** A threshold with no
headroom fails *intermittently* — it works whenever the gap happens to be quiet
— and intermittent failure gets misfiled as flakiness.

1. **Dispatch at `BUSY <= 1`, not `< 3`.** Headroom for one arrival during the lag.
2. **After dispatching, wait until that leg reports `in_progress` before probing
   again.** Until it does, the slot it will consume is invisible. This is what
   makes sequential dispatch safe; the threshold alone does not.

**The hosted lane fails *before* a test, not at one.** Legs routed
`[github-hosted]` run to completion (90-105 min) and fail on exactly one item out
of ~20,000: `cmake-control-sdk-consumer` — a **compile** failure, not a timeout
or assertion. That is what makes it deterministic rather than flaky, and it has
two consequences:

- **A PR routed hosted cannot pass the required gate at any duration.** Waiting,
  re-running, or catching a quiet window changes nothing. Local routing is not an
  optimisation; it is the only path to green.
- **The failure is independent of the diff.** When unrelated changes fail
  identically on one lane and pass on another, the lane is the variable. The
  cleanest evidence is a single PR whose **same commit failed hosted and passed
  local in 72 minutes** — one head, two lanes, opposite verdicts, diff removed as
  a variable entirely.

**Corollary for triage: hosted never cancelled anything.** It ran every leg and
returned a real verdict; the cancellations in that investigation were all
self-inflicted. "The hosted queue kills our jobs" is the wrong story and sends
the next person hunting a phantom. The true one is "hosted runs our jobs and
fails one specific compile, and `--failed` re-runs kept us routed there."

### Verify through the real harness, not a lookalike — five instances in one week

Every hard bug in this section shares one shape: **a check that passes against
something resembling the real artifact, and fails against the artifact.** It is
worth stating on its own because it is the cheapest to prevent and the most
expensive to diagnose.

| what was verified | what actually runs | result |
| --- | --- | --- |
| a hand-written inline JSON schema | the committed schema file (has `$schema`) | CLI rejected it; a live recovery job died |
| `rerun` observed twice re-routing | `rerun --failed`, which skips the resolver | ~6 legs mis-routed over 3 days |
| `tart list` VM count, host load | `resolve-provider`'s in-flight-legs counter | dispatch strategy built on the wrong variable |
| the busy counter read **at dispatch** | the resolver reads it **~2 min later** | a dispatch starved by its own predecessor |
| `bash test/foo.sh` run directly | `ctest -R foo` with CMake's registered path | script never executed: `0.00 sec`, `No such file or directory` |

The last one is the sharpest, because the test existed *specifically* to prove a
mechanism was genuinely enforced rather than merely configured — and it was
merely configured. A wrong `${CMAKE_CURRENT_SOURCE_DIR}/../` (the file is
`include()`d, so the variable is the *including* scope, not the file's own
directory) made ctest invoke a path that did not exist. It reports as `Failed`,
not `Timeout`, and takes `0.00 sec`.

**Why the pattern is seductive: the stand-in always passes.** That is what makes
"I verified it" feel true. A direct `bash test/foo.sh` run, a hand-written
schema, an observed `rerun` — each behaves exactly as hoped, which is precisely
why it never prompts a second look. The lookalike does not fail and then get
ignored; it succeeds and closes the question.

**So prefer a guard that makes the real artifact refuse, over a check that
confirms the stand-in.** The strongest fix found this week was not a better
test — it was a **configure-time `EXISTS` assertion** on the path a test is
registered with. A wrong path was previously invisible until run time, where it
surfaces as `***Failed 0.00 sec`, which reads like the test ran and disagreed.
The guard converts a silent run-time lie into a loud configure-time `FATAL_ERROR`.

Generalised: when something is *registered* now and *executed* later — a test
path, a runner label, a routing decision, a cached gate result — assert its
validity at registration time. Otherwise the gap between the two is where a
lookalike gets to vouch for the real thing.

**And the gap is invisible in the artifact you are looking at.** `add_test(...)`
looks complete on the line where it is written. `rerun --failed` looks like a
re-run. Nothing on the surface of either says "something else consumes this
later, and nobody re-checks it then" — so the question *is this still true at
consumption time?* never occurs to you unless you already know the answer.

That is why a guard beats discipline here. Being careful does not help, because
carefulness is applied to the thing in front of you and the defect is in a
relationship you cannot see. A configure-time assertion asks the question on
your behalf, at the one moment when the answer is cheap.

A useful tell: **any two-phase mechanism where phase one succeeds and phase two
consumes its output is a candidate.** `resolve-provider` → the build job.
`add_test` → `ctest`. A receipt → a publish step. If phase two never re-validates
what phase one handed it, a stale or wrong value survives every retry, and the
retry is precisely when someone will be relying on it.

**The rules that fall out:**

- **Run a new test through its real runner before claiming it passes.** `ctest -R
  <name>`, not the script directly. A registration bug is invisible to the
  script and fatal in CI.
- **`0.00 sec` plus `Failed` means it never ran.** Read that as a registration or
  path fault, never as a behavioural failure — and never as contention.
- **A green result in a simplified mode proves only what that mode exercises.**
  Script-mode arithmetic passing says nothing about whether CMake applied the
  property to a registered test. Say which half is proven.
- **When a remedy stops working, re-verify the remedy** rather than adding
  conditions around it.

**Two conditions, not one — they answer different questions.** This distinction
took a week to separate and is the single most useful thing here:

| condition | answers | why |
| --- | --- | --- |
| `BUSY <= 1` | **routing** — local or hosted | `resolve-provider` counts in-flight local legs; load is not an input |
| host `load` low | **passing** — whether a timing-sensitive test survives | timeouts fire under contention *after* routing is decided |

**Load does not affect where a job runs; it affects whether a job already in the
right place can pass.** Both prior conclusions stand — routing really is
load-independent — and that turns out to be only half the picture. Check both
before dispatching anything whose subject matter is itself timing-sensitive.

**Several test families are contention-sensitive, not just one.** Under a loaded
host, observed failures include `test_control_broker_daemon.cpp` waits (fixed by
deadline-bounding), the coverage FDN oracle, and `rack-acid-preflight` /
`rack-acid-runtime-gate` (both `Timeout`, `rack safety`). Before blaming a diff,
check whether the failure is a **timeout** and whether the host was loaded — a
timeout under contention on a test the diff does not touch is a lane/host
symptom, not a regression.

**A timeout ceiling equal to its enclosing budget is worse than no ceiling.** A
test clamped at the lane's own `job_timeout` can never fire first — the job is
killed at the same instant, and the run reports `cancelled` with **no failing
test named**. That inverts the purpose of the budget: finite per-test budgets
exist so a hung test *identifies itself* rather than taking the lane down
anonymously, and a ceiling at the job cap converts precisely those cases into
unattributed cancellations, which are the most expensive kind to diagnose.

Bound a per-test ceiling **strictly inside** the enclosing budget — here 3600s
against a 7200s `job_timeout` — and assert the inequality in a test so nobody
can raise it back silently. The general form: whenever a limit nests inside
another limit, the inner one must be able to fire first, or its diagnostics are
unreachable by construction.

**Prefer exact-value assertions over shape assertions in config code.** A
two-line `set(... CACHE STRING ...)` docstring reads as ordinary formatting and
silently folded a cache variable into a list
(`3600;CACHE;STRING;Hard upper bound…`). A test checking "is it a number" would
have passed; one comparing the exact expected string caught it immediately.
Config-language defects are usually invisible as defects, which is exactly when
a stricter-than-necessary assertion earns its keep.

**Diagnostic order.** A cancelled native macOS leg fails the direct required
`macos` context and is indistinguishable from a real test failure at PR level,
so read the job log and conclusion. A red bootstrap context instead means
routing or classification failed closed. And when a remedy stops working,
re-verify the remedy still does what it did when you first observed it, rather
than adding conditions around it: "re-running re-routes" was observed twice,
generalised, then applied through `--failed` where it silently does not hold.

**The authority for "why did CI route this way" is the workflow file, not the
API.** The API shows outcomes; the workflow states the rule.

**Prefer Shipyard for GitHub work — it dodges the personal `gh` rate
limit.** Shipyard authenticates with its own **GitHub App token**
(higher rate budget), so PR-create / check-watching / merge aren't bound
by the developer's personal 5,000/hr `gh` budget — which is *shared*
across interactive `gh`, Shipyard, and every self-hosted runner
authenticating as the same user. Operational rules:

- **`gh` "The token in keyring is invalid" / `gh auth status` failing /
  `gh api` 403 "rate limit exceeded" is almost always a rate-limit FALSE
  POSITIVE, not a broken token.** `gh auth status` validates by calling
  the API; a rate-limited 403 gets mislabeled as an invalid token. Do
  **not** `gh auth refresh` — it wastes a round-trip and fixes nothing.
  First run `gh api rate_limit --jq .resources`. High `core.remaining`
  (e.g. 4900/5000) *with* a 403 ⇒ the **secondary/abuse limit** (fires on
  bursts / concurrent / expensive calls like `gh api .../git/trees/
  ...?recursive=1`), which clears in ~1–5 min; a low `core.remaining` ⇒
  the primary limit, resets at the top of the hour.
- **`git push`/clone over HTTPS use a separate budget** and keep working
  when `gh api` 403s — so push branches with `git`, then let `shipyard
  pr` open the PR via its App token.
- **`shipyard pr`'s local target-reachability probe still shells out to
  local `gh auth status`,** so a rate-limited probe prints "Target 'mac'
  (cloud) is unreachable / gh auth status failed". Push past it with
  `--allow-unreachable-targets` (the required GitHub-Actions `macos` gate
  still guards the merge). Do **not** `--skip-target mac` to dodge it —
  `mac` is the only validation target, so skipping leaves none ("No
  targets remain after --skip-target filtering").
- Reduce burst: `git clone --depth 1` instead of recursive tree-API
  dumps; space `gh` calls; never tight-loop `gh` (back off / schedule).
- The local Shipyard CMake profile must resolve Python through
  `tools/ci/find_python311.py` and pass the result as
  `Python3_EXECUTABLE`. Apple's command-line tools still expose Python 3.9,
  which can configure most of Pulp but cannot run the `tomllib`-based decisions
  contract tests; allowing CMake to pick it produces a two-test false red after
  the entire Debug build has completed.

```bash
# Primary: Shipyard
shipyard run                              # validate current branch
python3 tools/scripts/shipyard_pr_watchdog.py  # create, track, validate, and merge on green; one safe stall restart
shipyard ship --resume                    # pick up an interrupted ship (v0.3.0+)
shipyard ship --no-resume                 # discard stale state, start fresh
shipyard ship-state list                  # in-flight ships (title, url, sha)
shipyard ship-state show <pr>             # full state for one PR
shipyard ship-state discard <pr>          # archive stale state
shipyard cleanup --ship-state --apply     # prune closed-PR + aged state
shipyard run --targets windows --smoke    # fast Windows-only check
shipyard run --resume-from test           # skip configure+build, run tests only
shipyard cloud run build <branch>         # dispatch the GHA build workflow
shipyard rescue <PR>                      # recover a wedged PR by redispatching queued runs
shipyard rescue <PR> --rerun-failed       # v0.67.0+: also re-dispatches FAILED/timed-out runs (not just cancelled), and — with --to omitted — RE-RESOLVES the provider local-first (overflow-aware) instead of forcing github-hosted. This is the lever to recover a saturated/timed-out macOS leg (re-run it on a real local runner). Pass --to <provider> to force.
shipyard rescue <PR> --rerun-failed --to local   # force a re-run onto the local runner
shipyard ship --pr N --base main --adopt-head     # recover "ship state SHA drift" after a force-push (Shipyard #346): adopt the current branch head + re-validate, instead of re-shipping from scratch
shipyard runner watch --kill-hung-workers # host-side prevention daemon for self-hosted runners
shipyard update --check --json            # installed vs latest Shipyard drift report
shipyard update                           # apply latest stable Shipyard

# Target management
shipyard targets                          # list targets with reachability
shipyard targets test windows             # probe a single target

# Config inspection
shipyard config show                      # effective merged config
shipyard config profiles                  # list profiles + active

# Fallback only (if Shipyard is broken or unavailable):
python3 tools/local-ci/local_ci.py run
python3 tools/local-ci/local_ci.py ship
```

### Resuming an interrupted ship (v0.3.0+)

If a ship was interrupted (laptop closed, session ended, OS restart),
run `shipyard ship` again — it auto-resumes from a per-PR state file
at `<state_dir>/ship/<pr>.json` without re-dispatching. Shipyard
refuses to resume if the PR head SHA or merge policy changed since
the state was written; re-run with `--no-resume` to discard and ship
fresh.

`shipyard ship-state list` is the self-describing inventory (PR,
title, URL, tip commit subject, dispatched-run IDs). Come back to
a week-old laptop state and still know what you were shipping.

### Fast test iteration on SSH targets

`--resume-from` now works on SSH and SSH-Windows targets. Shipyard
probes the remote for a marker file proving the previous stage
passed for the exact SHA. If found, earlier stages are skipped:

```bash
# After a full build passes, iterate on test failures only:
shipyard run --targets windows --resume-from test   # ~2 min vs 15 min

# Resume from build (skip setup + configure):
shipyard run --resume-from build
```

### Linux self-hosted routing (opt-in) and Windows x64 authority

`workflow_dispatch` retains the existing five-label
`PULP_LOCAL_LINUX_RUNS_ON_JSON` operator selector and the repository-scoped
Proxmox pool. The protected trusted and PR-safe provider roles are installed
separately, but this provider-only change does not consume their labels from
`build.yml` or merge-group routing. Protected automatic events therefore remain
GitHub-hosted until a later workflow-routing change explicitly opts in.

Do not set a protected automatic selector as part of provider installation.
When a later routing change is reviewed, it must use the role-specific sixth
label and exact restricted group described in `docs/guides/local-ci.md`; the
five-label generic selector is not a trust boundary. A successfully assigned
self-hosted job has no live capacity fallback.

Set the `run_windows=false` dispatch input for a trusted Linux-only Mac Pro
proof during hosted saturation. Its default remains true so ordinary manual
dispatches preserve the authoritative hosted Windows leg; automatic events do
not read this input.

The shared build step uses a literal `--parallel 4`: omitting it makes the
Makefile-based Mac Pro VMs compile serially, while a bare `--parallel` is
unbounded. Four fills each VM's assigned cores and remains a bounded share on
the larger self-hosted macOS machines.

Windows is intentionally different. The required `Windows (x64)` functional
gate stays on real GitHub-hosted `windows-2022`; the separate build-only MSVC
release-path gate tracks `windows-latest`. The current local Windows pool is
Windows ARM64 on QEMU (`qemu-system-aarch64`): it can maybe smoke x64 via
Windows-on-ARM translation, but it is not the authoritative Intel/x64 gate.
Do not wire `PULP_LOCAL_WINDOWS_RUNS_ON_JSON` into the required Build-and-Test
Windows x64 matrix leg; use a separate/dedicated label for any local Windows
ARM64 smoke.

### macOS runner routing (current)

As of 2026-05-20, Namespace macOS routing is disabled for cost control.
The required macOS PR and merge-group gates run through the M1/M3/M5
event-class-v2 Tart JIT pool via `PULP_LOCAL_MACOS_RUNS_ON_JSON`; see the
current-truth section at the top for labels, priorities, and proof. Shipyard's
`mac` target must stay `backend = "local"`; do not flip it to `cloud` unless
the operator is explicitly re-enabling Namespace. Each CI job runs in a
throwaway clone of the `pulp-build-runner` golden and then destroys the VM.
The sibling tartci repo owns the production provider; Pulp's
`tools/ci/tart-runner.sh` and launchd template are compatibility/rollout
surfaces, not evidence of current host participation. Full guide: the
`tart-ci` skill.

Linux and Windows CI legs default to GitHub-hosted runners.

### SSH `ubuntu` / `windows` targets are opt-in, per machine

`.shipyard/config.toml` declares exactly one target: **`mac`**. The SSH
`ubuntu` / `windows` targets are not declared there.

They used to be — with `host` deferred to the gitignored
`.shipyard.local/config.toml`. No machine in the fleet ever had that file
populated (checked 2026-07-08 across studio / m1 / m5 / mini), and Shipyard
preflights every *declared* target, so every `shipyard run` and `shipyard pr`
on every box hit:

```
Target 'ubuntu' (ssh) is unreachable.
  Failure category: configuration
  Last error: target has no host configured
```

...an exit-3 papered over with `--skip-target ubuntu --skip-target windows`.
**That flag pair is no longer needed.** The targets now live where their hosts
live.

To opt one machine into a local SSH lane, uncomment the block in
`.shipyard.local/config.toml` (template:
`.shipyard.local/config.toml.example`) and fill in `host` + `repo_path`. The
gitignored overlay can **declare a target outright**, not merely supply its
host — verified against Shipyard v0.70.0. The per-target validation recipes
(`[validation.default.overrides.windows]` etc.) stay version-controlled in
`.shipyard/config.toml` and apply the moment the target exists.

Check what a machine will actually probe:

```bash
shipyard targets list        # reachability per declared target
shipyard targets test ubuntu # probe one
```

**Do not reach for `[profiles.*]` to solve this.** A profile's `targets` list
does **not** gate preflight in Shipyard v0.70.0 — `shipyard config use local`
writes `profile = "local"` into the tracked config and Shipyard still probes
every declared target. (m5's local overlay defines `local` / `normal` / `full`
profiles for exactly this purpose; they have no effect on preflight.) Only
*declaring* a target matters.

If a PR's macOS check is queued, verify queue age, enabled host supervisors,
lease/VM state, repository-visible registration, and exact assignment before
taking action. A runner census by itself is inconclusive for the JIT pool.

Do not rerun or empty-commit just to "unstick" a queued macOS job. Rebase
only when the branch needs current `main` fixes, and cancel stale
pre-cutover Namespace runs instead of rerunning them.

The required PR lane must not let a single flaky or environment-specific
macOS test wedge unrelated PRs behind the serialized local runner pool. If
current `main` has known macOS-only failures, quarantine the exact test names
with a macOS-only `ctest --exclude-regex` in `build.yml`, keep Linux/Windows
coverage intact, and open/follow a targeted fix. Do not hide failures that
reproduce cross-platform or that are caused by the branch being shipped.

**Real-time-thread teardown hangs need `RUN_SERIAL`, not an exclude or a
capacity-shaped `PROCESSORS` assumption.** A test that opens the real CoreAudio
device (anything constructing a `StandaloneApp` and calling `start()`, or using
`AudioSystem::create_device()` + `start()`) tears it down via
`AudioOutputUnitStop` / `AudioUnitUninitialize`, which **block until the
real-time I/O thread observes the request**. Under a saturated full-suite run
that RT thread is CPU-starved, so teardown hangs to the 120s timeout — a flake
that reproduces **only** in the full run, never in isolation or any concurrent
subset (so don't waste time bisecting subsets). Fix it at the scheduler:
`pulp_add_test_suite(... PROPERTIES RUN_SERIAL TRUE)` makes CTest run the suite
alone at every dynamically granted width. Retain an existing `PROCESSORS 8` as
its scheduling weight, but do not mistake that historical value for isolation
or add it to unrelated tests. Prefer this over an exclude — it keeps the test
enabled everywhere. (Adding a shared `RESOURCE_LOCK` does NOT fix it:
serializing the audio tests among themselves still leaves unrelated tests
starving the RT thread.)

The required `macos` context comes directly from the native macOS matrix child
on pull-request, Shipyard workflow-dispatch, and merge-group runs. It therefore
becomes terminal as soon as the owned macOS leg does, without waiting for the
combined matrix or polling the jobs API. A small event-specific bootstrap owns
the same name only when routing/classification fails closed or classification
proves the native build is unnecessary; inactive bootstrap jobs use an
`-unused` name so they cannot satisfy or collide with the required context.
Advisory Linux/Windows work may continue without delaying queue admission.

**Flaky required-leg wedge + the rerun lock (recovery).** Even when the direct
`macos` job reports its failure promptly, a *flaky* failure on the required leg wedges
auto-merge: the PR sits `mergeStateStatus: BLOCKED`, and `ghapp run rerun <run>
--failed` refuses with **"cannot be rerun; This workflow is already running"**
for as long as an advisory leg (Windows x64 / Coverage) keeps the run
`in_progress`. So a slow advisory leg blocks the re-run of the flaky required
leg. First confirm it's a flake, not the branch: the required `[local]` leg's
log is NOT in the GHA store (`run view --job <id> --log` returns 0 lines), so
read a GitHub-hosted sanitizer leg's log instead — a recurring culprit is
`extract_keyboard_shortcuts does not catastrophically backtrack on large
embedded data` (a ReDoS-guard timing test that overruns under ASan / runner
load). Recovery, once confirmed flaky — **reach for the one-liner first**: arm
`ghapp pr merge <pr> --auto` (no strategy flag), then
**`shipyard rescue <pr> --rerun-failed`**.
`rescue` cancels stuck runs and, with `--rerun-failed`, re-dispatches completed
failed/cancelled runs — "e.g. a flaky required leg" (its own help) — re-resolving
the provider so the rerun lands local-first on the JIT pool; the armed
auto-merge then fires when it goes green. Do NOT hand-crank the cancel+rerun
unless `rescue` is unavailable. `shipyard ship` now also *detects* this exact
wedge (validated green + a red required check that maps to a validated-green
target) and prints the `shipyard rescue … --rerun-failed` line for you in its
hand-back, so on a fresh block you usually just copy it. Manual fallback only if
`rescue` is missing/older: `ghapp run cancel <run>` (advisory legs are
expendable), wait ~20–60s for `status: completed`, then `ghapp run rerun <run>
--failed`; the per-run rerun lock is why the cancel must come first. (Or `ghapp
workflow run build.yml --ref <branch>` for a fresh run whose newer `macos`
context supersedes the stale failure — do one or the other, not both.)

**Prevention: retry transient flakes on the required leg.** `build.yml`'s
non-Windows and Windows `ctest` steps run `--repeat until-pass:2` (mirroring
`sanitizers.yml`), so a single timing-flake retries once and self-heals instead
of reddening the required `macos` gate. This is the right tool for *transient*
flakes; reserve the `--exclude-regex` quarantine (above) for tests that fail
*consistently* on `main`, and the `PROCESSORS` reservation for RT-teardown
starvation hangs. Each retry attempt is still bounded by `--timeout 120`. The
deeper structural fix for the rerun lock — splitting the required macOS leg into
its own workflow so advisory legs can never hold its run open — is a tracked
follow-up.

### Historical: superseded M1 busy-count overflow probe (#2467)

This section records why the former busy-count implementation under-counted.
It is not current routing guidance: the required gate now uses the M1/M3/M5
event-class-v2 JIT pool described at the top of this skill, and live overflow
is the `local-only` sentinel. Do not recover the old `pulp-gate-fast`, M1-only,
or persistent-runner assumptions from this incident note.

`build.yml`'s `resolve-provider` job has an inline-Python "busy probe"
(`_count_busy_local_mac_runners`) that decides whether a PR's macOS leg
runs on the local M1s or overflows to github-hosted `macos-15`. The rule:
`BUSY >= LOCAL_MAC_OVERFLOW_THRESHOLD` (default 2) → overflow.

**The probe must count only macOS Build-and-Test jobs that are RIGHT NOW
`status == "in_progress"` on a local M1** — a job whose `status` is
`in_progress` *and* whose `labels` array contains the local self-hosted
label (`PULP_LOCAL_MAC_RUNNER_LABEL`, default `pulp-gate-fast`). The probe
label must match the required selector's fast runner class; otherwise an idle
rollback-only M1 can suppress overflow for work it cannot serve. Everything
else counts 0:

- A `queued` Build-and-Test run has dispatched nothing — never enumerate
  queued runs at all; the probe lists only `status=in_progress` runs.
- A run that is `in_progress` but whose macOS matrix job is not yet
  registered (matrix not expanded) or is still `queued` is NOT holding an
  M1 → count 0.
- An API blip on a per-run `/jobs` call → count 0 (under-count).

**Why err toward "use local":** an earlier cut enumerated `in_progress`
+ `queued` runs and *pessimistically* counted a not-yet-registered macOS
job as local-busy. During a deep Actions queue (~250 runs) that
over-counts catastrophically — hundreds of undispatched queued runs read
as local-busy, BUSY blows past the threshold, every new macOS leg routes
to github-hosted overflow, and the local M1s sit 100% idle while macOS
work — the CI long-pole — starves on the contended hosted pool. Merges
stalled ~80 minutes on 2026-05-20. Slightly oversubscribing the M1s (a
3rd leg briefly queues behind 2 running ones) is far less harmful, so the
probe under-counts, never over-counts.

The probe uses the default workflow `GITHUB_TOKEN` — `repos/.../actions/
runs/<id>/jobs` exposes per-job `status` + `labels` without an
`Administration: Read` scope. Do NOT switch the probe to the
`actions/runners` endpoint (needs admin scope; the first cut did and fell
back to BUSY=0 every run). Regression coverage:
`tools/scripts/test_resolve_provider_busy_probe.py`, wired into
`workflow-lint.yml`; it extracts the inline probe from `build.yml` and
asserts a 50-run deep queue with 2 real local legs reports BUSY=2 (not
50). Cooperates with `macos_reroute_watcher.py` — the probe makes the
initial dispatch call, the watcher catches near-misses after the fact.

### Release workflows: runner routing

Release workflows should follow the same post-cutover rule: do not add
new Namespace defaults while the repo variables are unset. If a release
workflow exposes a manual runner selector, treat Namespace as an explicit
operator choice, not an automatic fallback.

### Per-PR macOS retargeting: `pulp macos`

The matrix in `build.yml` couples Linux/Windows/macOS into a single
`workflow_run`. Rerunning macOS via that matrix means re-running
Linux/Windows too — wasted compute when they already passed.

`build-macos.yml` is a standalone workflow (introduced in pulp task
#20) that runs JUST the macOS build/test on a chosen runner pool.
Branch protection's required `macos` check accepts the latest same-named check
on the exact PR head. Because a trusted-main `workflow_dispatch` is attached to
the main SHA, `build-macos.yml` uses checks-write-only controller jobs to create
one in-progress check run on the API-pinned PR head before execution and
complete that same run afterward. The controllers never check out or execute
PR code.

It must remain semantically identical to the required macOS gate: resolve an
open internal PR from the workflow definition on protected `main`, check out
trusted control code with credentials disabled, then fetch and verify the PR's
exact head SHA and immutable base SHA without materializing PR-controlled files.
An automated zero-job recovery controller must set `recovery=true` and pass
`expected_head_sha`, `source_run_id`, and `source_run_attempt`; the resolver
rejects the dispatch if the live PR head moved or the exact source is no longer
the queued `pull_request` attempt for `.github/workflows/build.yml` with an
exhaustive zero-job census. Operator dispatches leave recovery false, omit the
source identity, and retain live-head resolution.
Shipyard v0.143.0 provides the matching default-off
`shipyard runner zero-job-recover` controller primitive. It creates and then
re-reads an exact receipt, refuses duplicate/conflicting receipt contexts,
requires the exhaustive current-attempt census to remain unchanged, and never
cancels the source run. GitHub remains workflow scheduler and merge authority;
TartCI remains capacity scheduler.
The first PR checkout occurs only in the clone owned by `nobody`; this keeps
PR-selected Git filters and LFS endpoints out of the trusted environment. The
untrusted build job has only contents-read permission, no Actions/Namespace
cache action, no persistent build/dependency cache path, and no ccache; all
writable state is unique to the run and removed at teardown. All PR-controlled
setup/CMake/build/test commands run as the separate `nobody` uid under `env -i`
from a disposable non-hardlinked clone created by trusted control before its
ownership transfers to `nobody`. Do not pass Actions runtime/cache variables,
GitHub variables, tokens, credentials, or command-file paths across that account
boundary; do not forward proxy URLs because they may contain userinfo
credentials. The empty environment must still receive the isolated source path,
and the wrapper must change into its `nobody`-owned home before dropping
privileges; otherwise it inherits an inaccessible Actions checkout cwd and
fails before configure. The reporter
revalidates the complete open PR identity (base and head repository/ref/SHA)
before posting. Immutable recovery identity must be uploaded before the pending
check exists; the protected source-free `workflow_run` reconciler terminalizes
that exact check if cancellation skips the normal reporter. The local route is
fail-closed even for the current event-class JIT Tart selector: disposal after
a job does not protect the
main-scoped runtime/cache token while runner and PR code share the guest admin
account. Re-enable it only with a separately proven two-account Tart class.
Namespace accepts
only the exact approved `namespace-profile-generouscorp-macos` selector. It also installs
the same pinned and checksum-verified Chrome used by `build.yml` inside that
ephemeral root and excludes
`validation|slow|performance|bench|quality-lab`. A retarget changes only the
provider; it must not turn required CI into a full benchmark lane, compare a
behind PR against newer live `main`, or depend on a warm runner's stale refs.

The macOS lane configures with `-DPULP_LOTTIE=ON` so the opt-in skottie
render path (LottieAnimation → SkiaCanvas) gets real CI coverage — it is
the only lane that exercises it, since `PULP_LOTTIE` defaults OFF. This
cannot break the gate: `core/canvas/CMakeLists.txt` runs a configure-time
try-link and auto-disables Lottie on any Skia bundle that can't link
skottie (needs SkJSON + skresources, bundled from Skia chrome/m151 onward).
It only enlarges the test binaries, never shipped plugins.

```bash
# Move a PR's macOS leg to a different runner, without touching Linux/Windows:
pulp macos retarget --pr <N> --to <namespace|github-hosted>

# See where the latest macOS check landed and its state:
pulp macos status --pr <N>
```

`retarget` cancels any in-flight macOS-bearing workflow_runs for the
PR (from both `build.yml` and `build-macos.yml`), then fires a fresh
`gh workflow run build-macos.yml` with the chosen runner.

**When this is the right tool:**

- Local is deliberately unavailable until the two-account Tart class is proven.
- One critical PR needs to skip the queue → `--to namespace` (billable).
- A PR's macOS leg flaked on local; retry on GH-hosted → `--to github-hosted`.

**What it doesn't do:** retargeting only swaps the macOS dispatch.
Linux/Windows from `build.yml`'s matrix keep running independently.
For full-workflow rerun (e.g. after force-push), the existing
close+reopen or `git push --force-with-lease` paths still apply.

### Opportunistic reroute daemon (task #22)

`tools/scripts/macos_reroute_watcher.py` is a long-running watcher
that automates the "local just freed up; pull a queued cloud job back
to local" pattern. Install on the host that runs the self-hosted Mac
GH Actions runner; the launchd template at
`tools/launchd/pulp-macos-reroute-watcher.plist.template` documents
the setup steps.

The watcher is not an active reroute authority while local retarget is
fail-closed. Preserve its state, but do not enable its local handoff until the
two-account Tart prerequisite is proven.

**Capacity is VM-slot-aware (#3299).** "Free capacity" is no longer a
single runner's busy/idle — it's `free_macos_slots(hosts)`, the sum of
free slots across configured hosts. Each host is either **bare-metal**
(one slot, gated by the `ps` Runner.Worker probe — `local_is_busy()`,
local-only, no admin token) or runs **ephemeral Tart VMs** (`cap` slots
— macOS caps 2 running macOS VMs/host, Appendix D — minus the running
macOS VM count from `tart list`, counted locally or over SSH). The
default (`--hosts-config` absent) is a single local bare-metal slot,
i.e. **exactly the pre-#3299 behavior**, so the watcher is safe to run
before the Tart-VM cutover and grows into it. Supply a hosts JSON
(`{"hosts": [{name, mode, cap, ssh}, ...]}`) to make it multi-host /
VM-slot aware. A host whose probe fails is skipped (its capacity is
unknown, not zero); the tick is skipped only if **every** host probe
fails — never act on unknown capacity.

Flap-guard: a PR rerouted in the last 5 min is suppressed (avoids
thrashing). One reroute per tick; the next tick reassesses.

Cooperates with two siblings: the overflow probe in `build.yml` (makes
the initial dispatch decision; the watcher catches near-misses after
the fact), and `tools/ci/tart-runner.sh --loop`, whose capacity+queue
gate uses the **same** VM-slot rule (boot a VM only when queued BAT work
exists AND `running_macos_vms < cap`) so the two never double-book a
host.

### Path-scoped validation profile: `parser`

`.shipyard/config.toml` defines a `[validation.parser]` lane (pulp
#1916) for PRs that only touch runtime-import parser code — the
standalone `tools/import-design` tool, the `tools/import-validation`
scripts, the `packages/pulp-import-ir` package, parser fixtures, the
parser test files, and the `core/view/.../design_import*` family.

The lane configures with `PULP_BUILD_EXAMPLES=OFF` and runs ctest with
`--label-include parser-import`, so plugin validators (auval /
pluginval / clap-validator, registered under `examples/pulp-*/`) and
the broader format-adapter smoke surface stay out of the loop. The
motivating failure was pulp #1910, where pluginval-PulpGain-VST3
segfaulted on a Figma Make parser PR that had no business touching the
VST3 adapter.

```bash
# Auto-select against origin/main and run the matching lane:
shipyard run --pipeline "$(python3 tools/scripts/validation_profile_select.py)"

# Inspect what the classifier decided + which paths drove it:
python3 tools/scripts/validation_profile_select.py --json

# Force the broad lane (useful when the parser scope is technically
# unchanged but you suspect cross-subsystem fallout):
shipyard run --pipeline default
```

The selector returns `parser` only when every changed path lives
inside the scope; any unrelated edit forces `default`. The bias is
toward broad validation.

`shipyard pr` does not yet auto-route to the parser lane — for a final
merge gate, let it default through `[validation.default]`. The
parser lane is for fast iteration before that final gate.

### Iterating on a single-platform failure

When CI goes red on exactly one of Pulp's platforms — e.g. only the
Windows Coverage leg of the #566 matrix, only the macOS AddressSanitizer
leg, only Linux Namespace — **do not default to pushing a fix and
waiting for the full matrix**. That re-validates `mac`, `ubuntu`, and
`windows` on every iteration when only one of them actually failed,
burning ~25 minutes of wall time and GitHub Actions runner minutes on
legs that were already green.

Use `shipyard run` with target selection against Pulp's real lanes:

```bash
# Iterate on the Windows lane only
shipyard run --skip-target mac --skip-target ubuntu --json

# Inclusive form (equivalent)
shipyard run --targets windows --json

# Chain with --resume-from when the build is already green and you're
# only iterating on test failures on that platform:
shipyard run --targets windows --resume-from test --json
```

This validates locally via Pulp's configured backend for the target
(`local` on mac, `ssh` on ubuntu, `ssh-windows` on windows) — see
`shipyard targets list` for the live mapping. Real result in ~5–10
min per target, zero GitHub Actions spend, no re-validation of lanes
you didn't touch. Once the local lane goes green, push + let CI
confirm across the full matrix; only then run `shipyard pr` for the
final merge gate.

**When this loop doesn't fit — keep using the full path:**

- **Final pre-merge gate.** `shipyard pr` is still the only thing that
  produces a merge-eligible evidence record across all three lanes.
  Local iteration gets you to green; `shipyard pr` lands it.
- **Failure specific to a GitHub-hosted lane.** The build matrix on
  main has a `macOS (ARM64) [github-hosted]` leg and `[namespace]`
  Linux/Windows legs. If a failure is specific to the github-hosted
  macOS environment, `shipyard run --targets mac` hits the local mac
  backend which is close but not identical. `shipyard cloud run build
  <branch>` dispatches to the same Namespace runners CI uses without
  re-running everything — the middle ground when you need the exact
  cloud environment.
- **Cross-target behavior you're actually testing.** If the bug only
  manifests when two targets interact (rare — e.g. shared FetchContent
  cache corruption), the single-target loop hides it. Full matrix
  only in that case.

**When `shipyard run` fails for reasons that don't match your change:**

Pulp's `ssh` (ubuntu) and `ssh-windows` (`win` alias) backends
accumulate per-run state — `.shipyard-stage-build-*`,
`.shipyard-stage-configure-*`, a stale worktree branch checkout from
an interrupted earlier run. If `shipyard run --targets windows` errors
with messages that look unrelated to the code you changed (CMake
complaining about files you didn't touch, configure steps timing out
on line one, paths pointing at an earlier branch), the host state is
suspect — your code change probably isn't wrong. Diagnose before
iterating:

On Linux (ssh ubuntu), the checkout is at `~/pulp-validate`; diagnose
with standard POSIX commands:

```bash
ssh ubuntu
cd ~/pulp-validate
git log -1 --oneline && git status --short       # expected SHA? clean worktree?
ls -la .shipyard-stage-* 2>/dev/null             # leftover stage dirs?
rm -rf .shipyard-stage-*                         # safe — shipyard re-stages from scratch
```

On Windows (ssh win), the checkout is at `C:\Users\danielraffel\pulp-validate`.
OpenSSH on Windows runs commands through `cmd.exe` by default, so use
cmd-native syntax — do NOT paste Windows-style backslash paths into a
`bash` block (backslashes get interpreted as escapes):

```bat
:: Via ssh from your Mac; each command is a separate ssh call so cmd.exe parses cleanly
ssh win "cd /d C:\Users\danielraffel\pulp-validate && git log -1 --oneline"
ssh win "cd /d C:\Users\danielraffel\pulp-validate && git status --short"
ssh win "cd /d C:\Users\danielraffel\pulp-validate && dir /b .shipyard-stage-*"
```

PowerShell is reliable for the removal step (the `for /d` cmd idiom is
fragile when shipped through ssh argv quoting):

```bash
ssh win 'powershell -NoProfile -Command "cd C:\Users\danielraffel\pulp-validate; Get-ChildItem -Directory -Filter .shipyard-stage-* | Remove-Item -Recurse -Force"'
```

On a genuinely stale host (validate worktree stuck on a several-weeks-old
commit with 20+ `.shipyard-stage-*` artefacts), combine `git fetch origin &&
git reset --hard origin/main` on the validate checkout with the stage
directory cleanup above. Re-run `shipyard run --targets <host>` after
cleanup.

### Incremental bundles (automatic)

SSH validation now sends only the git delta between the remote HEAD
and the target SHA. Typical cycles drop from ~443 MB to a few KB.
No configuration needed — falls back to full bundle automatically.

To install Shipyard locally for the first time:

```bash
./tools/install-shipyard.sh           # download + verify pinned binary
./tools/install-shipyard.sh --status  # show installed vs pinned version
export PATH="$HOME/.local/bin:$PATH"  # add ~/.local/bin to PATH (one-time)
```

The public Pulp installer intentionally does not install Shipyard or GitHub
CLI (`gh`). Ordinary Pulp users can create, build, run, and upgrade projects
without either tool. Treat them as source-checkout contributor dependencies
for PR/CI work; `gh` is required only for GitHub-facing maintenance commands
and the explicit `pr.workflow=github` bypass.

After install, every Pulp checkout that has `~/.local/bin` on PATH gets
the same pinned Shipyard version automatically. The pin lives in
`tools/shipyard.toml` and is bumped via PR after each Shipyard release
that passes Pulp's CI matrix. Use `shipyard pin bump --to vX.Y.Z`
instead of hand-editing `tools/shipyard.toml`; the helper owns the pin
edit and worktree-safety checks.

The two tools cover the same target matrix (mac local + GitHub-hosted
Linux/Windows; legacy SSH targets only when explicitly requested) and accept
the same `--base` flag for develop branches. Shipyard adds evidence-gated
merge that checks per-platform proof for the exact merge-candidate SHA, which
is stricter than `local_ci.py`'s `job.passed` check.

## Phase 1 failure diagnostics (>= v0.58.0)

Shipyard v0.58.0 (Shipyard PR #304, 2026-05-18) replaces the lossy
`Validation failed. PR #<N> not merged.` emit with a structured failure
block carrying the failing job URL, the failing step name, and a parsed
test-framework footer (CTest by default; `failure_parser` config knob
allows opting into catch2 / pytest / go / auto in a future phase). On a
real failure you now see:

```
✗ Validation failed
  Target: mac (cloud=namespace)
  Job:    macOS (ARM64) [github-hosted]
  URL:    https://github.com/<org>/<repo>/actions/runs/<R>/job/<J>
  Step:   "Test (non-Windows)" — exit 8
  Tests:
    1236 - FontResolver: animation respects LRU cache cap (Failed)
    ...
```

Same data lands in the JSON event under `diagnostics: {...}` so
machine-readers (auto-resolution routines, agent-status dashboards)
can act on it without parsing the human text. Source design:
`https://github.com/danielraffel/Shipyard/issues/303` + the codex-
vetted comment thread there.

**Self-hosted blind spot (important).** The footer parser reads the
**GitHub step log** — which is EMPTY for a self-hosted macOS leg
(`gh run view --log` yields only "Process completed with exit code 8";
check-run annotations are empty too). So for a self-hosted failure the
parsed `Tests:` block is blank and you can't tell which test failed from
Shipyard/`gh` alone. Three ways to recover the failing test:
1. **Job summary + artifact (build.yml #3392):** the macOS leg now writes
   an "❌ ctest failures" block (failed test names + the `FAILED:` /
   `with expansion:` assertion lines) to the run's **summary page**, and
   uploads `Testing/Temporary/` as a `ctest-logs-<key>` artifact — visible
   with no host access.
2. **Runner-local ctest logs (on the host):** read
   `<workFolder>/<repo>/<repo>/build-macos/Testing/Temporary/LastTestsFailed.log`
   + `LastTest.log` (workFolder from `<runner>/.runner`, NOT `_work`). Full
   recipe in the **`tart-ci` skill → "Diagnosing a red macOS leg"**.
3. Shipyard-side fix tracked at danielraffel/Shipyard#344 (teach the footer
   parser to fall back to the runner-local ctest logs when the GH log is
   empty). Related new requests: #345 (rerun should re-resolve provider),
   #346 (reconcile ship-state SHA drift).

## Phase 2 watch diagnostics (>= v0.59.0)

Shipyard v0.59.0 (Shipyard PR #310, 2026-05-19) extends `shipyard
watch --pr N --follow` to surface the same structured failure block
on every terminal-failure transition observed during the poll. The
watch loop caches diagnostics by `(target, run_id)` so at most one
log fetch per transition fires for the lifetime of one watch
invocation. Reuses Phase 1's 256 KB log-tail cap. Both human and
JSON modes carry the diagnostics. Lets you chain
`shipyard pr && shipyard watch --pr <N>` and stop babysitting the
GitHub UI on slow CI runs.

## PR test selection

Pulp's base-owned schema-v3 `[targets.mac.changed_surface_selection]`
declaration records a mandatory kernel, complete literal tests and their
reviewed CMake producer targets for narrowly reviewed families, medium-risk
extended neighbors, and known full-required surfaces. Current bounded families
include the Forge/DSP CLI projections, the isolated ChildProcess test source,
and Forge Rack's `generate.py` plus its registered safety/endings contracts.
The Rack family is deliberately exact: `patch.py`, provenance/preflight tools,
and neighboring delivery skills still select full validation. Documentation
under `docs/guides/**`, `docs/reference/**`, `docs/examples/**`, and
`docs/validation/**` runs the mandatory kernel and may omit the mobile compile
gate; generated authority under `docs/status/**` remains fail-closed. Its protected-base execution
template cannot activate itself: Shipyard reads
`changed_surface_execution.mode` from the independent machine-global config,
where missing or `off` preserves the ordinary full test stage byte-for-byte.
The initial canary uses `shadow_compare`; it builds only the selected producer
targets and executes the literal test selection, then runs the ordinary full
build and CTest commands, returns the full path's status, and writes an
append-only selected-vs-full timing/failure-coverage receipt. Do not
set `authoritative` until those receipts satisfy the reviewed graduation gate.
Because both builds share one locked warm tree, the post-selected full-build
timer is an incremental remainder. Receipts name it that way and separately
record the estimated total full-build duration as selected build plus remainder;
never compare selected time against the remainder alone.
The execution receipt binds the policy, selection, validation, workflow,
literal-test, and literal-build-target digests for session-independent aggregation. A
`missed_full_failure` or `selected_only_failure` is explicit non-graduation
evidence; only a compared selected/full status match is marked
`graduation_eligible`. More precisely, both suites must pass; two red statuses
are `failure_overlap_unproven` until exact failure identity is reviewed.
Receipt publication fsyncs the file and its containing directory.
Unknown paths and
changes to build/toolchain, public ABI, security, provenance, selector policy,
or test topology select full. `changed-surface-policy-selftest` verifies those
dispositions with negative mutations and, on the declared macOS
Debug/examples-on target, checks every authoritative CTest registration rather
than collapsing duplicate display names. The canonical composite binds name,
anchored executable and arguments, working directory, and every CTest property;
the inventory is an order-independent multiset with a pinned count and digest.
Literal selection expands all composites that share a requested name. Missing
commands, duplicate properties or composites, and digest drift fail closed to
the full suite. The selector self-test itself is in the mandatory kernel. Add
broader mappings only after shadow
receipts show they contain the relevant full-suite failures.

The Build-and-Test macOS job uses the independent
`ios_compile_skip_safe_paths` allowlist to skip its separate two-SDK iOS
compile step. Accept `ios_compile_required=false` only for `pull_request` or
`merge_group` diffs whose every path matches that allowlist. A bounded macOS
test family does not inherit this authority; review mobile impact explicitly
before adding a path. Any missing/malformed value, empty or mixed diff, unknown path,
mobile/Apple path, public header, CMake/CI/policy/test-topology change, or policy
read failure runs the gate. Main, manual, nightly, release, and audit execution
never accepts this skip and retains its existing event policy. Condition the
expensive step, never the required workflow/job, so the stable required context
still reports.

Tests that open a real host audio device belong to the explicit
`hardware;validation` tier, not the ordinary PR lane. On macOS, a virtual or
unavailable default CoreAudio route can block inside
`AudioComponentInstanceNew` before Pulp receives a recoverable open failure.
Keep these tests registered and runnable for hardware acceptance, main,
release, or audit work; exclude them from default PR CTest selection rather
than deleting them or weakening their assertions.

When a change deliberately adds or removes CTest registrations, refresh the
inventory contract in the same commit: update
`.shipyard/changed-surface-inventory.json`, the matching `full_test_count` and
count comment in `.shipyard/config.toml`, the pinned-count assertions in
`tools/scripts/test_changed_surface_policy.py`, and the current inventory
counts in `docs/guides/local-ci.md`. Derive the digest from the configured
build's canonical inventory, then run
`python3 tools/scripts/test_changed_surface_policy.py --build-dir build`.
Otherwise the full suite can finish almost entirely green and fail only at the
inventory self-test, forcing a needless second admission cycle.

Merge the current target branch before deriving that inventory. A configured
tree from a stale PR head can be internally consistent and still omit tests
that landed on `main`; refreshing the pinned count and digest from it merely
replaces one stale contract with another. Reconfigure after the merge, derive
the inventory from that exact tree, and keep the JSON, Shipyard count, policy
assertions, and local-CI guide in the same commit.

Catch2 `TEST_CASE` additions, removals, and renames are CTest topology changes
too: discovery materializes each case as a registration even when no CMake
manifest changed. A 2026-08-28 sequence added four cases and removed one after
the last inventory refresh, leaving main's contract three registrations stale
until the next unrelated full proof exposed it. Treat changes to discovered test
sources exactly like explicit `add_test` changes for this refresh requirement.

If two independent exact-head full proofs report the same inventory counts and
digest while the candidate diff adds, removes, or renames no CTest registration,
treat that agreement as current-main inventory drift rather than warm-build
contamination. Derive the canonical manifest from either configured build,
refresh all four mirrors above together, and rerun the inventory self-test. Do
not spend another unchanged full-suite admission: a 2026-09-01 pair of proofs
repeated the same 191-registration delta before this distinction was recorded.

The ordinary and changed-surface build-and-test stages share
`tools/ci/build_dir_lock.py` for canonical build-directory serialization. The
lock is persistent by design (removing it can split lock identity under queued
waiters), but it lives in owner-only per-user host state rather than beside the
build directory, so exact-source verification never sees a lock artifact as a
checkout mutation. Canonical path aliases share a lock; same-named build dirs in
different worktrees use different full-digest identities. The absolute
`PULP_BUILD_DIR_LOCK_ROOT` override is for trusted tests only.

`build.yml` excludes CTest labels matching `slow` on both `pull_request`
events and `workflow_dispatch` because Shipyard PR validation dispatches
the Build-and-Test workflow manually. Preserve that split: slow configure
smokes like `cmake-ios-auv3-configure` can legitimately take many minutes
on the single self-hosted Mac and should not block PR validation after the
classify gate has already decided a native build is needed.

Pulp's `tools/cmake/PulpCatch.cmake` accepts a first-class multi-value
`catch_discover_tests(... LABELS "a;b;c")` argument, keeping the list separate from
generic CTest property pairs. Catch2 v3.7.1 otherwise flattens that list in the
generated `set_tests_properties` call, leaving only the first label and treating
the rest as property names/values. Keep the Pulp wrapper and
`PulpCatchAddTests.cmake` label finalization together; verify changes with the
`cmake-catch-multilabel-properties` test and `ctest --show-only=json-v1`.

## Recovery + maintenance toolkit (>= v0.56.2)

Three operational commands cover the prevention → recovery → maintenance
lifecycle for self-hosted-runner CI. The authoritative reference lives in
Shipyard's own `skills/ci/SKILL.md`; the commands surface from
`shipyard --help` once the pin is bumped.

### Recover: `shipyard rescue <PR>`

When a PR's matrix is stuck behind a queue jam on the self-hosted local
runner (the typical 5+ hour backlog scenario), cancel that PR's queued
runs and redispatch them to GitHub-hosted runners:

```bash
shipyard rescue 1920                  # cancel + redispatch queued runs to github-hosted
shipyard rescue 1920 --rerun-failed   # also re-arm completed/cancelled runs (watchdog-cancelled case)
shipyard rescue 1920 --dry-run        # preview without acting
shipyard rescue --all-stuck           # repo-wide
shipyard rescue 1920 --to <provider>  # default: github-hosted
```

Replaces the older 5-step gh-api cancel + cloud-handoff recipe. Use it
when a self-hosted runner is healthy but its queue is hours deep —
`gh api .../actions/runs?status=queued` will show the depth across the
repo and `ps aux | grep Runner.Worker` confirms the runner itself is
not the bottleneck.

### Prevent: `shipyard runner watch --kill-hung-workers`

Host-side daemon that runs continuously on each self-hosted host.
Cancels stale queued runs AND auto-kills hung `Runner.Worker`
processes via the same recovery sequence as `runner kill --pid <pid>
--yes`: snapshot → SIGTERM → grace → SIGKILL → reap children →
quarantine partial builds → verify `Runner.Listener` → optional wait
for GitHub status flip.

```bash
shipyard runner watch --kill-hung-workers          # implies --fix
shipyard runner watch --kill-hung-workers --json   # structured stream
```

Pair with launchd / systemd so the watchdog survives reboots. JSON
contract: `runner.watch` envelopes with `event=auto_kill_worker`,
`phase ∈ {attempt, killed, failed, no-pid-found}`.

`shipyard runner fleet-status --repo Generous-Corp/pulp --json` also audits
registered runner labels, Tart disk-floor/ccache admission health, and expected
metal hosts declared under `[runner.fleet.expected_host.<name>]` in
`.shipyard/config.toml`. MacPro requires two online ephemeral Linux runners;
Mac Mini requires one online native Intel JIT runner even before its first
registration; the planned MacBook Air is recorded with `active = false` until
commissioned. Expected hosts match stable label subsets, never disposable runner
names. Treat `expected_host_unavailable` as unfinished/offline fleet capacity,
not an intentional absence.

**Do not "fix" the absence of m3/m5/m1 from `expected_host` — it is deliberate,
and declaring them makes the view wrong.** The gap looks obvious and actionable
(the three Macs that serve the required `macos` gate are not declared, so a dead
one raises nothing), which is why it keeps getting re-proposed. Three things to
check before spending a cycle on it, all readable in a minute:

- **The gate hosts have no host-identifying label.** Matching is by label subset,
  and every gate runner on all three registers the same base `self-hosted, macOS,
  ARM64, pulp-build, pulp-build-vm` labels plus its selected event class; labels
  that vary
  (`pulp-build-studio`, `pulp-build-vm-secondary`) are role labels, and m1's and
  m5's are label-identical. There is no `pulp-host-m3` analogue to
  `pulp-host-macpro` / `pulp-host-macmini`. So three per-host entries match one
  pool three times and all report online while a single machine serves — a real
  partial degradation reads as three green rows. Confirm with the live label sets
  before assuming otherwise:
  `ghapp api "repos/Generous-Corp/pulp/actions/runners?per_page=100"`.
- **Pool-wide `min_online` fails the other way.** The gate pool is ephemeral JIT,
  so a healthy idle host has zero runners registered and would alarm on every
  quiet period — the alarm gets muted within a week and the class returns.
- **Nothing consumes `fleet-status`.** `grep -rl fleet-status .github/workflows
  tools/` finds nothing; it is a manual-inspection view, so a declaration pages
  no one on its own.

The per-host question is already answered by the same report's `hosts[]` array —
keyed by class (`m1`, `m5`, `studio`), read from tartci state over SSH rather than
from labels, carrying `routable`, `free`/`cap`, supervisor heartbeat age, and disk
/ ccache admission problems. Read that, not `expected_hosts[]`, when asking
whether a specific Mac is serving. The genuinely open gap is narrower and nobody
has built it: detecting a *partially* degraded pool needs capacity measured
against demand, because a busy pool and a pool at a third of capacity are
indistinguishable from host presence. Rationale in
`docs/guides/local-ci.md`, "Why the macOS gate hosts are not declared as expected
hosts".

### Prevent: build-dir ODR guard (interrupted-build sentinel)

The self-hosted macОS lane uses `clean: false` (warm `build-<key>` dir for
fast incremental builds). A build that is **cancelled/interrupted** mid-compile
leaves partial object files; the next incremental build then mixes object
layouts → heap corruption / **SEGFAULTs in unrelated tests** (HttpStream,
model_store, Theme dtor) while a clean github-hosted build passes. This bit
every open PR on 2026-06-07 after heavy branch churn + many cancelled runs.

`build.yml`'s macОS Configure step writes a `.pulp-build-incomplete` sentinel
into `$PULP_BUILD_DIR` after configure; the Build step removes it on success.
If a new job's Configure finds the sentinel still present, the previous build
did not finish cleanly, so it `rm -rf`s the dir for a pristine rebuild (ccache
keeps the recompile fast; Skia at `external/skia-build` is untouched). This is
the durable complement to `--kill-hung-workers`: the watchdog kills the hung
worker, the sentinel ensures the *next* build doesn't inherit its corruption.
For an already-corrupted dir (no sentinel yet), clean it once with the personal
`pulp-runner-ops` skill / `ssh macstudio … rm -rf … build-macos`.

Pulp's local macOS runner runs through `actions-runner` (PIDs surfaced
via `ps aux | grep Runner.Listener`); the daemon co-exists with the
existing service.

**Shipyard's local lanes carry the same guard, via
`tools/ci/build-dir-sentinel.sh`.** For a long time they did not, which mattered
because Shipyard's lane is the one that actually gets killed: a `timeout_secs`
expiry is a SIGKILL, so no trap or exit handler runs, and 4 of 5 logged mac-lane
runs on 2026-07-26/27 died that way with no compiler error in the log. Each
timeout then seeded the next run's undefined-symbol failure, and the loop
sustained itself. `.shipyard/config.toml`'s POSIX `configure` stages run
`build-dir-sentinel.sh guard build '<configure command>'`; their `build` stages
run `… clear build`. `guard` also tells a **failed** stage from a **killed** one:
a configure that exits non-zero on its own wrote no object files, so it clears
the marker and keeps the warm tree rather than forcing a needless cold rebuild.
Only a signal exit (128+signum) stays armed.

Two details differ from `build.yml` deliberately:

* The sentinel is armed **before** configure, not after, so a run killed
  *during* configure is caught too — a half-written `CMakeCache.txt` is its own
  kind of broken.
* Landing it required the timeout fix first. Arming a sentinel on a cap the
  build cannot finish under produces an infinite wipe → cold rebuild → timeout →
  wipe loop, ~1h of a shared machine per cycle. `[targets.mac] timeout_secs` is
  14400 for this reason; check it before changing either number.

`tools/scripts/test_build_dir_sentinel.py` asserts both directions (a stale
marker wipes, a clean run does not) plus the arm/clear pairing across stages —
a lane that arms without clearing wipes its build dir on every run.

### Prevent: name the configure blockers before cmake does

`tools/scripts/checkout_preflight.py` (advisory, run by `setup.sh`; also
`--root <dir>` to audit another checkout) reports, in seconds, the failures a
tree is already destined for. Run it first when a configure dies — it answers
the two questions the CMake error does not.

**Which Skia is this build actually using, and why.** `FindSkia.cmake` reads
`$SKIA_DIR` **before** the checkout's own `external/skia-build`, and that
variable is pinned in a shell rc on the multi-worktree hosts. So a worktree can
hold a perfectly good cache and still build against a different, broken one. On
2026-08-16 every worktree on M5 — including ones carrying m151 and m152 — was
resolving to a stale m150 tree whose Dawn slice was compiled at macOS 15.0
against a 13.4 floor. `Pulp macOS archive-floor mismatch` names the archive, never
the override, so "set `SKIA_DIR` per worktree" and "fetch a good cache locally"
both look reasonable and **neither can take effect**. The preflight prints the
winning path, says when it overrides the local one, and gives a fix with `--dest`
pointing at the cache actually in use.

**Is this checkout trustworthy to read a pinned value from.** It flags an
unfinished merge and the distance behind `origin/main`. The primary checkout on
M5 sits **6,365 commits behind with 4 conflicted paths**, and it is the most
natural place to look. Reading `tools/deps/manifest.json` and `tools/shipyard.toml`
from it produced two confidently-wrong answers hours apart in one session — once
nearly overriding a peer's correct diagnosis. **Read pinned values with
`git show origin/main:<path>`**, not from a working tree.

It also catches an uninitialised `planning` submodule, which the source-contracts
gate otherwise reports as a contract violation rather than a provisioning gap.

### Prevent: ccache false-hit guard (#3504 follow-up)

The build-dir sentinel above does **not** cover the *ccache*, and the
self-hosted macОS runners share one cache (`CCACHE_DIR=…/cache/ccache`,
configured in each runner's `~/actions-runner-*/.env`). That `.env` sets
`CCACHE_DEPEND=true` with the default `compiler_check=mtime` — depend mode
skips the preprocessor and trusts the compiler's dependency manifest, and
`mtime` keys the compiler weakly; on a shared cache a stale/false-hit object
gets served and corrupts unrelated TUs **even on a clean build dir**. Observed
2026-06-07: the same change-unrelated tests failed on *every* PR's `macos`
gate — including a pure function `resolve_checkpoint_url()` returning `""` —
while the identical tests passed on clean local Debug **and** Release builds on
the same machine. A one-time `ccache -C` does **not** fix it: concurrent builds
repopulate the cache within minutes, so the bad entries come right back.

The fix is a *config* override, not a clear. `build.yml`'s `build` job `env:`
forces the safe ccache path (job env overrides the runner-service `.env` for
those steps): `CCACHE_COMPILERCHECK=content` (key the compiler by content, which
also changes the cache-key namespace so the contaminated mtime-keyed entries are
never hit — self-cleaning), `CCACHE_NODEPEND=true` (disable depend mode, the
actual culprit), and `CCACHE_SLOPPINESS=time_macros` (pulp uses no PCH, so
`pch_defines` was inert). **Gotcha:** ccache rejects `CCACHE_DEPEND=false` with
`invalid boolean environment variable value "false" (did you mean
CCACHE_NODEPEND=true?)` — the env spelling to *disable* a ccache boolean is the
negated `CCACHE_NO<X>=true` form, not `CCACHE_<X>=false`. Direct mode is left
**on** (the fast path) on purpose: it hashes the source + include manifest and
is correct once depend mode is off and the compiler is content-keyed, so there's
no need to force full preprocessor mode (`CCACHE_NODIRECT=true`) and pay its
speed cost. A `Ccache effective config (proof of override)` step prints
`ccache --show-config` before Configure so the run log proves the override
reached the step. Durable host-side hardening (not required once the env
override is in place): give each runner its own `CCACHE_DIR`, or set
`compiler_check=content` in the runner `.env`.

### Keep current: `shipyard update`

```bash
shipyard update --check --json   # report installed vs available (safe in CI / cron)
shipyard update                  # apply latest stable
shipyard update --to v0.56.2     # pin / rollback to a specific version
shipyard update --dry-run        # plan only
```

Replaces any documented `curl install.sh | sh` recipes — the bootstrap
form is only needed when a host has no Shipyard at all. Future
upgrades on a host that already has Shipyard installed go through
`shipyard update`.

The repo-side pin lives in `tools/shipyard.toml`; bump it with
`shipyard pin bump --to vX.Y.Z` (this triggers the ci skill-sync gate,
which is why this section exists). The pin and a developer's local
install can drift — `pulp doctor` surfaces that, and `shipyard
--version` is the local source of truth.

## Nightly full build (`nightly-full-build.yml`)

`.github/workflows/nightly-full-build.yml` is a scheduled coverage net
for a gap in per-PR CI: **`build.yml` does NOT `make all`.** It builds a
curated target set, and most `test/*.cpp` executables are only compiled
on demand by `ctest`. So a refactor that breaks a test file (a stale
include, a moved helper, an undeclared identifier) passes its own PR CI
and rots on `main` undetected until the next full build. Two such
breakages already slipped through this way — `test_mcp_server.cpp` and
`test_canvas_widget_shadow.cpp` (issue #2462, the systemic fix).

What it does:

- Triggers on a nightly `schedule:` cron (`13 9 * * *`, off-peak UTC)
  plus `workflow_dispatch:` for manual runs.
- Runs on GitHub-hosted `macos-15` — **deliberately not** the
  self-hosted M1 runners: a nightly must not compete with PR CI for the
  M1's 2 runners, and overnight latency is irrelevant for a sweep.
- Configures the **full tree** (`examples ON`, tests ON — no
  `-DPULP_BUILD_EXAMPLES=OFF`) with `-G Ninja`, and builds **everything**
  with `cmake --build … -- -k 0` (Ninja keep-going — `cmake` itself has
  no `--keep-going` flag), so one broken target does not mask the rest.
  The full build log is uploaded as an artifact.
- **The run is gated on the BUILD step only.** ctest still runs but is
  *informational*: the nightly runs on GitHub-hosted `macos-15`, which
  lacks the M1's GPU, installed fonts, and registered AU components, so
  ~15 golden / GPU / `auval` / platform-harness tests fail there though
  they pass on the M1 in PR CI. Gating on ctest would make the nightly
  red every night; instead ctest results land in the run's step summary
  for eyeballing.
- On a **build** failure, opens or updates a single de-duplicated
  tracking issue and auto-closes it on the next green run — the same
  watchdog pattern as `auto-release-watchdog.yml` and
  `release-cadence-check.yml` (`permissions: issues: write`).

If you see the "Nightly full build is broken" tracker, a refactor broke
a test target PR CI never compiles. Download the `nightly-full-build-logs`
artifact for the full failure list.

## Webhook endpoint watchdog (`webhook-endpoint-watchdog.yml`)

Shipyard's daemons receive GitHub events over Tailscale Funnel on the
self-hosted Macs. A dead receiver is **indistinguishable from a quiet one**
unless somebody reads delivery history, which is why pulp's macbook webhook
sat returning `502` for **41 days** unnoticed: the Tailscale node had
re-registered as `…-pro-1` while the hook still pointed at `…-pro`, whose node
had gone offline. Every event-driven behaviour on that host was silently dead
the whole time.

This watchdog closes that hole on the same find-or-create / reopen / close
tracker pattern as the two above. It runs on a 30-minute schedule **in GitHub
Actions, deliberately not on the hosts** — a host-local check cannot report
that its own host is unreachable, so GitHub is the only vantage point that can
observe "I cannot reach this endpoint."

It flags an active hook when every sampled delivery failed, when no success
falls inside the grace window, or when there are no deliveries at all. It does
**not** flag a hook that fails only *some* event types: that endpoint is alive
and the cause is a payload-decode bug in the receiver — a different fix. Today
`workflow_job` and `check_suite` return `200` while `workflow_run` and
`check_run` return `400` across both repos, which is exactly that separate bug.

Reading webhooks is admin-scoped, so it prefers `RELEASE_BOT_TOKEN` and falls
back to `GITHUB_TOKEN`; an unreadable API opens the tracker saying so rather
than reporting healthy.

**Do not hand-edit a hook URL to fix a stale endpoint.** Shipyard's registrar
owns hook lifecycle (`src/registrar.rs`) and patches or re-creates hooks for the
repos the daemon is started with, recording them in
`daemon/registrations.json`. The durable repair is:

```sh
shipyard daemon refresh --repo <owner>/<repo> --repo <owner>/<other-repo>
```

Two things that bite:

- **Registration needs a verified tunnel.** `refresh` restarts the tunnel, so
  status immediately afterwards reports `tunnel=inactive · repos=—`. That is
  normal; give it a minute. Re-running `refresh` to "fix" it restarts the tunnel
  again and resets the clock — wait instead of retrying.
- **The registrar only manages hooks it created.** A hook it does not track (an
  older registration, or one from a renamed node) is left behind as an orphan
  pointing at a dead host — the 502 source, and the thing this watchdog reports.
  After a repair, list `repos/<repo>/hooks` and delete any URL that is not a
  daemon's current tunnel URL.

Check what a daemon actually serves with `shipyard daemon status` (it prints the
live tunnel URL plus `repos=…`). A daemon registered for a *different* repo
answers the request and ignores the events, which looks healthy from the outside.

### Linting workflow files locally

`actionlint` is **slow on Pulp's large workflows** — it shells out to
`shellcheck` for every `run:` block, and `build.yml` has many big ones,
so a local `actionlint .github/workflows/build.yml` can spin for many
minutes (observed at 338% CPU / 8+ min with no result). Do not wait it
out:

- Run `actionlint -shellcheck=` — the empty value disables the
  `shellcheck` integration. It still validates workflow YAML, action
  refs, and `${{ }}` expression syntax; it just skips the slow
  per-`run`-block shell linting.
- Or skip local `actionlint` entirely: `yamllint -d relaxed` locally is
  enough, and the CI **`Workflow lint`** job runs `actionlint`
  authoritatively on a fast Linux runner.

Never leave a runaway `actionlint` process behind.

## Stale run reaper (`stale-run-reaper.yml`)

`.github/workflows/stale-run-reaper.yml` is a scheduled janitor that
cancels stuck GitHub Actions runs. GitHub has no automatic cleanup of
runs that wedge, and that gap clogged Pulp's CI queue badly:

- **Hung runs** — Coverage runs sat `in_progress` for 2-7 hours when a
  healthy Coverage run takes ~1h. A hung job squats on a runner slot
  until GitHub's 6h default job timeout finally reaps it.
- **Orphaned queued runs** — runs sat `queued` for **5+ days**, waiting
  on a runner label or branch that no longer exists. A queued job that
  never starts never hits any timeout, so it waits effectively forever.

Both modes occupy the limited macOS-runner concurrency slots, and
everything queued behind them stalls.

What it does:

- Triggers every 30 minutes (`schedule:` cron `*/30 * * * *`) plus
  `workflow_dispatch:` with `in_progress_max_minutes` /
  `queued_max_minutes` inputs to override the thresholds for manual runs.
- Runs on `ubuntu-latest` — it only calls the GitHub API, no build.
  `permissions: actions: write` (required to cancel runs) + `contents: read`.
- Pages through `actions/runs?status=in_progress` and `?status=queued`
  via `gh api --paginate` and cancels anything past the threshold via
  the `runs/<id>/cancel` API.
- **Age basis:** `in_progress` runs are aged from `run_started_at`
  (execution start), **not** `created_at`. `created_at` also counts
  queue time, so during a deep backlog a healthy run that queued for
  hours then started recently would look "hung" and be wrongly
  cancelled. `queued` runs never started, so `created_at` is their age.
- **Default thresholds: `in_progress` > 240 min (4h of *execution*),
  `queued` > 480 min (8h).** The slowest legit run — the cold nightly
  full build — executes ~2h, so the 4h cutoff is a 2x margin; nothing
  healthy is reaped.
- Never cancels its own run (skips `${{ github.run_id }}`); a failed
  cancel on one run never aborts the rest of the sweep; a `concurrency`
  group prevents two reaper runs from overlapping.
- Writes a step summary: runs scanned plus a table of every run
  cancelled (id, workflow, branch, age). If the reaper keeps reaping the
  same workflow, that workflow has a hang bug worth investigating — the
  summary history makes that visible.

If a stuck PR run vanishes unexpectedly, check the reaper's recent runs:
it cancels by age regardless of why a run wedged, so a job that
genuinely *executes* longer than 4h would be cancelled too. Bump the
threshold via `workflow_dispatch` if a one-off legitimately needs longer.

### Gotcha: a job with no `timeout-minutes` can hang the whole workflow

The reaper is the *backstop*, not the fix. The root cause of repeated
Coverage hangs was that `coverage.yml`'s `coverage` matrix job had **no
`timeout-minutes`** and one matrix leg (`macos`) routes to a self-hosted
runner pool via `PULP_COVERAGE_MACOS_RUNS_ON_JSON`. When that pool is
saturated the leg sits `queued` forever — a `queued` job never reaches
its steps, so step-level `continue-on-error` cannot help. The downstream
`coverage-diff-gate` has `needs: coverage` + `if: always()`, so it
cannot reach a terminal conclusion until *every* matrix leg ends; the
required `Diff coverage required` check then sat `queued` and the whole
Coverage run stayed `in_progress` for 4h+ until the reaper killed it.

Fix pattern (applies to any GH-Actions job, not just coverage):

- **`timeout-minutes` does NOT bound a QUEUED job — only a running one.**
  This is the trap (codex P1 on pulp#2521). `timeout-minutes` starts
  counting *after* a runner is assigned; a matrix leg routed to a
  saturated self-hosted pool that never gets a runner sits `queued`
  forever and the timeout never fires. Always set `timeout-minutes` on
  every job anyway — it is the correct backstop for the *running*-hang
  mode — but do not assume it covers the queued-hang mode.
- **For the queued-hang mode, add an explicit queued-job watchdog.**
  `coverage.yml`'s `coverage-queue-watchdog` job is the reference: it
  runs on `ubuntu-latest` (never self-hosted, so it itself can never be
  the stuck thing), starts after `classify` says native coverage is
  required (still no `needs:` on the matrix), and polls this run's own
  jobs via `gh api
  repos/{repo}/actions/runs/{run_id}/jobs`. If a `Coverage report (...)`
  leg has been `status==queued` past a grace window it cancels the whole
  run via `POST runs/{run_id}/cancel` (`permissions: actions: write`).
  There is **no public API to cancel a single matrix leg** — cancelling
  the whole run is the only option, and it is what the required gate
  needs anyway. Age queued legs from the **run's `created_at`** (`gh api
  repos/{repo}/actions/runs/{run_id}`): the jobs API exposes no per-job
  `queued_at`, and `created_at` is when every leg — including the queued
  one — was created. The watchdog must exclude its own job name from the
  scan or it will reap itself.
  - **Do not exit before native coverage legs exist.** The watchdog can
    start before `matrix-config` has materialized the `coverage` matrix.
    In that window the jobs API returns zero matching `Coverage report
    (..., Clang)` jobs; that means "not created yet", not "all coverage
    legs left queued". Track whether at least one required native leg has
    ever been observed, and only use `queued_legs == 0` as an early-exit
    condition after that observation. This exact bug left a later macOS
    coverage leg queued while newer main Coverage runs piled up behind
    the workflow concurrency group, keeping Codecov's main record stale.
  - **Scope the watchdog's job-name match to the REQUIRED legs only.**
    `coverage-queue-watchdog` matches a name that starts with `Coverage
    report (` AND ends with `, Clang)` — i.e. only the native `(macOS,
    Clang)` / `(Linux, Clang)` / `(Windows, Clang)` legs the required
    `Diff coverage required` gate depends on. A bare `Coverage report (`
    prefix match also catches `Coverage report (Android, Kotlin)`, the
    **advisory** Android lane. Reaping cancels the *whole run* (there is
    no single-leg cancel API), so matching an advisory leg would cancel
    the required gate too — even when native coverage would have
    succeeded. Match on a suffix that only the required legs carry
    (`, Clang)` here; the Android lane ends `, Kotlin)`).
  - **Tune the grace window from measured queue-wait data, not a
    guess.** `coverage-queue-watchdog`'s `QUEUED_GRACE_MINUTES` is
    **150** (was 25). Measured coverage-leg queue waits: median ~30 min,
    p90 ~52 min, max ~83 min under deep-queue load — and 89% of legs
    waited >= 25 min then ran and SUCCEEDED. A 25-min grace therefore
    false-cancelled the large majority of healthy coverage runs. 150 min
    sits comfortably above the ~83-min observed legitimate max, so the
    watchdog fires only on a leg genuinely orphaned by a dead/saturated
    pool. The poll window (`POLL_SECONDS` 60 × `MAX_POLLS` 160 = 160
    min) must exceed the grace, and the job's `timeout-minutes` (165)
    must exceed the poll window. A long grace is cheap: the watchdog
    still exits EARLY after it has observed a required native coverage
    leg and none remain queued, so in the common case it never runs the
    full window.
- **Pin a pure gate/aggregation job to a GitHub-hosted runner**
  (`ubuntu-latest` / `ubuntu-24.04`). A job that only downloads
  artifacts and runs a check must never queue behind a saturated
  self-hosted pool — if it does, `if: always()` cannot save it because
  the job still needs a runner to start.
- A `needs:` + `if: always()` job only reaches a conclusion once every
  upstream job is terminal; an upstream job stuck `queued` therefore
  hangs the gate transitively until the watchdog (or the slower
  repo-wide `stale-run-reaper.yml`) cancels the run.

Do not flip a `concurrency` block's `cancel-in-progress` to `true` to
"fix" a pile-up if `false` was a deliberate choice (e.g. coverage.yml
keeps `false` for Codecov's `after_n_builds` upload completeness,
pulp#1884). Per-job `timeout-minutes` plus the queued-job watchdog are
the correct fix; they bound runs regardless of `cancel-in-progress`.

### Gotcha: `coverage.yml` is gated on `classify` — touch the gate carefully

`coverage.yml` mirrors `build.yml`'s `classify` job (runs
`tools/scripts/classify_changes.py --mode=diff`, outputs
`native_build_required`). Skip-safe PRs (docs / planning `*.md` only —
classifier fails *closed*, any uncertainty → `true`) skip the
`coverage` matrix (210 min/leg); docs-only PRs also skip
`android-kotlin-coverage`. The Android Kotlin lane is additionally
guarded with `github.event_name != 'pull_request'`, so it never allocates
a runner on PRs. No coverage runner is allocated on a docs PR.

`coverage-diff-gate` is an **advisory PR signal**, named `Diff coverage
advisory`, and is not in protected main's required-check set. It may report red
without blocking queue admission, but it must still fail honestly when its
inputs or threshold fail.
It has `needs: [classify, coverage]` + `if: always() && github.event_name
== 'pull_request'`, and its first step ("Evaluate coverage gate
preconditions", id `gate_preconditions`) implements **exactly three
terminal cases**:

1. `classify.result != success` → `exit 1` (fail RED — the
   `native_build_required` output is untrusted; fail the advisory signal
   closed rather than guessing).
2. `native_build_required != 'true'` → `exit 0` (pass GREEN — the
   classifier approved a skip-safe PR; no coverage is owed).
3. `native_build_required == 'true'` AND `coverage.result != success`
   → `exit 1` (fail RED — the matrix was supposed to run and didn't).

Only when case 3's preconditions are met (native required + coverage
succeeded) does the step fall through; it sets a step output
`native_required` and every real diff-cover step below carries
`if: steps.gate_preconditions.outputs.native_required == 'true'`.

Critical: even though coverage is advisory, the OLD "all coverage XML missing →
pass GREEN" fallback is **unsafe** and was removed. For
`native_required == 'true'`, a missing/empty merged Cobertura XML now
hard-fails (`exit 1`) in both the merge step (`merge_cobertura` exit-2
all-missing sentinel → `exit 1`, not `exit 0`) and the `Run diff-cover`
step. Only the classifier-approved skip-safe path (case 2) gets an
exit-0 green with no coverage report. If you ever loosen this, you
re-open a false-green 75% diff-coverage signal.

### Gotcha: per-PR coverage is macOS-only — Linux/Windows/Android live in the nightly lane

`coverage.yml`'s `coverage` matrix is **event-conditional** (built by the
`matrix-config` job). Pulp's team actively tests on macOS, so per-PR
coverage measures the macOS development surface only:

- `pull_request` → macOS leg only.
- `push` (to main) + `workflow_dispatch` → full `{linux, macos, windows}`
  matrix, so the canonical main-branch head and manual runs keep a
  complete per-OS Codecov picture.

Linux / Windows / Android coverage on PRs is provided by the **nightly
`cross-platform-check.yml` lane**, which runs the full non-macOS build +
test every night and files (auto-closes) one tracking issue per broken
platform. Do NOT "restore" the per-PR 3-OS matrix thinking it regressed —
macOS-only on PRs is deliberate.

Consequences when touching `coverage.yml` or `coverage-diff-gate`:

- On PRs only the `coverage-cobertura-macos-<sha>` artifact exists. The
  Linux/Windows `Download Cobertura XML` steps fail and are tolerated by
  `continue-on-error: true`; `merge_cobertura.py` then writes a
  macOS-only XML. diff-cover gates the macOS-reachable surface — that is
  correct, not a bug.
- Platform-specific files (`**/platform/linux/**`,
  `**/platform/windows/**`, `android/**`, `*_linux.*`, `*_win*.*`) are
  ABSENT from the macOS XML, so diff-cover does not gate them per-PR.
  The `Note platform-specific paths…` step in `coverage-diff-gate`
  classifies the PR's changed files and appends a visibility note to the
  coverage PR comment listing them. It is a **note, not a block** — do
  not make it fail the gate (that would punish macOS-first velocity for
  code the nightly lane already validates).
- `Diff coverage required` keeps its exact name and pass/fail semantics
  for macOS-reachable code — it is still the REQUIRED branch-protection
  check.
- **`android-kotlin-coverage` must stay off PRs.** It is a separate
  Gradle/JaCoCo job, not part of the `matrix-config` native coverage
  matrix, so macOS-only PR coverage requires its own event guard:
  `github.event_name != 'pull_request' && native_build_required == 'true'`.
  A classify-only guard accidentally queues Android coverage on every
  code PR even though Linux/Windows/Android PR coverage moved to the
  nightly lane.
- **The `matrix-config` job MUST encode `runs_on_json` with `jq`'s
  `tojson`, never `tostring`.** The downstream `coverage` job consumes
  it via `runs-on: ${{ fromJSON(matrix.runs_on_json) }}`, so the field
  must hold valid JSON. `tostring` on a JSON *string* scalar (the
  default fallback, e.g. `"macos-latest"`) strips the quotes and emits a
  bare `macos-latest`, which `fromJSON` rejects — matrix expansion then
  breaks on every leg that resolved to a single scalar label. `tojson`
  always emits valid JSON: a string stays quoted, an array stays an
  array, an object stays an object.
- **@pulp/react Codecov uploads are split by event.** PRs that touch
  `packages/pulp-react/**` still build, test, and upload through the
  path-filtered `pulp-react-build.yml` workflow. Push-to-main and manual
  Codecov uploads for `@pulp/react` live in `coverage.yml` as
  `pulp-react-coverage`. Do not move the main upload back to
  `pulp-react-build.yml`: if the native Coverage run for a SHA is
  superseded before any OS legs upload, an independent React upload would
  advance Codecov's `main` branch record to a mixed React + stale-native
  snapshot.

## Prerequisites Check

Before running any CI command, verify the required tooling AND provider config exists:

```bash
# Required
test -f tools/local-ci/local_ci.py || echo "ERROR: local CI not found — is this a recent checkout?"
command -v shipyard >/dev/null || echo "ERROR: shipyard not installed (run ./tools/install-shipyard.sh)"
command -v gh >/dev/null || echo "WARNING: gh CLI not installed; GitHub-facing fallback/triage commands will be unavailable"

# Preferred (shared machine-global local CI config)
test -f "$HOME/Library/Application Support/Pulp/local-ci/config.json" || echo "WARNING: no shared local CI config — copy tools/local-ci/config.example.json there"

# Fallback (worktree-local legacy config)
test -f tools/local-ci/config.json || echo "WARNING: no worktree fallback config.json"

# Verify GitHub Actions runner routing. The local event-class JIT pool handles
# required macOS work; GHA-hosted handles Linux+Windows.
gh variable list -R Generous-Corp/pulp | grep -q '^PULP_DEFAULT_RUNNER_PROVIDER[[:space:]]*github-hosted' || echo "WARNING: PULP_DEFAULT_RUNNER_PROVIDER should be github-hosted"
gh variable list -R Generous-Corp/pulp | grep -q '^PULP_LOCAL_MACOS_RUNS_ON_JSON' || echo "WARNING: PULP_LOCAL_MACOS_RUNS_ON_JSON is missing; macOS build will use hosted macos-15"
gh variable list -R Generous-Corp/pulp | grep -q '^PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON[[:space:]]*local-only' || echo "WARNING: required macOS overflow is not the reviewed local-only sentinel"
gh variable list -R Generous-Corp/pulp | grep -q '^PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON' && echo "WARNING: Namespace macOS routing should be unset outside an explicit operator test" || true
```

If `local_ci.py` doesn't exist, the user likely has an older checkout. Tell them to pull latest main.

## Visual Harness Container

`ci/visual-harness.Dockerfile` and `.github/workflows/visual-harness.yml`
provide the deterministic visual-harness smoke environment. The Docker image
downloads the pinned Skia `chrome/m153` Linux release asset (from the
`danielraffel/skia-builder` fork — adds iOS/visionOS/mac-x86_64 slices the
upstream `olilarkin/skia-builder` omits), verifies its SHA-256, installs the
bundled Pulp fonts into fontconfig, and installs `skia-python==144.0.post2`
for the B.0 SkPicture byte-identity smoke. The skia-python pin intentionally
trails the C++ surface because the Python bindings ship one milestone behind
on PyPI; the C++ raster harness is the source of truth for goldens. The
workflow runs that Linux container and also runs the same pytest smoke on
macOS arm64 so the future canonical raster lane has a platform signal.

**Pin-drift guards (manifest is source of truth).** The Skia/V8 pin data is
hand-mirrored into several files; two mirrors are *tooling-consumed*, so a
hand-sync typo is a silent behavioural bug rather than a doc lag. When bumping
`tools/deps/manifest.json`, keep these in lockstep — all are enforced in CI
(`workflow-lint.yml`) and pre-push (`gates.sh`):
- `ci/visual-harness.Dockerfile` ← `tools/harness/visual/check_skia_pin.py`
- `external/skia-build/VERSION.md` digest table (documentation only — a
  missing fetcher-written asset stamp always forces verified re-download) and `DEPENDENCIES.md`
  Skia/Dawn/V8 version cells ← `tools/scripts/check_manifest_mirrors.py`
- `tools/harness/visual/pins.py` ← `test_skia_determinism.py`

For m153+, the pin checks are necessary but not sufficient: run
`tools/scripts/verify_skia_m153_capabilities.py --platform <matching-native-desktop-platform>
--skia-dir <generation>` against the exact stamped manifest generation so
`SkLogHandler` and Graphite's executor field are proven through the archive's
exported symbols. After the pin lands, each active Mac build host
must populate its own immutable SHA-addressed generation through the fetcher
and prove the second fetch is a no-download hit; do not rsync one runner's
checkout cache to another.

The `macOS local smoke` job resolves `runs-on` from
`PULP_LOCAL_MACOS_RUNS_ON_JSON` first and falls back to hosted `macos-15` only
when the local selector variable is absent. On the persistent local runner,
this job deliberately uses the installed `python3.12` and a worktree-local venv
instead of `actions/setup-python`, because that action defaults to GitHub's
hosted `/Users/runner` toolcache path and can fail before tests start.

Use it when a fresh worktree has only `external/skia-build` headers/metadata
and no platform static libraries:

```bash
tools/harness/visual/docker-build.sh
docker run --rm -v "$PWD:/workspace" pulp-visual-harness
```

The wrapper defaults to the pinned Skia `linux-x64` lane (`linux/amd64`) and
keeps a reusable local buildx cache under
`~/.cache/pulp/visual-harness/buildx`. The Dockerfile also uses BuildKit cache
mounts for apt packages, the Skia release zip, and pip wheels, so repeated
runs on the same Mac/Ubuntu SSH host do not re-download the expensive inputs
unless the lock or digest changes. Override with `PULP_VISUAL_IMAGE`,
`PULP_VISUAL_DOCKER_PLATFORM`, or `PULP_VISUAL_DOCKER_CACHE` if a host needs a
separate cache namespace.

GitHub-hosted Ubuntu must create a `docker-container` Buildx builder before
calling the wrapper; the default `docker` driver on that image rejects
`type=local` cache export unless containerd image storage is enabled.

For macOS visual/layout jobs, do not use the combined `actions/cache` action
for `ccache` or FetchContent on the local self-hosted runner. Its home
directory persists between jobs, so GitHub cloud-cache saves can spend
20+ minutes uploading multi-GB compiler caches that the runner already has
locally. Use `actions/cache/restore` for GitHub-hosted fallback runners and
`actions/cache/save` only on non-PR `main` runs (`push` where the workflow has
a push trigger, or `workflow_dispatch` on `main` for manual cache seeding).
PRs should restore existing remote caches at most, not publish PR-scoped
ccache blobs.

The container is a reproducible smoke/developer environment. It does not
replace the future canonical arm64-darwin raster-golden gate.

## Language Correction

**IMPORTANT**: When a user says "push to main", "merge to main", or "land this", ALWAYS correct them:

> "I won't push directly to main — that bypasses review. Instead, I'll create a PR, run CI on it, and merge it if everything passes. This keeps main safe."

Then proceed with the `ship` workflow below.

## Runner Priority (hard rule)

**GitHub-hosted is the default runner provider** for Linux and Windows.
macOS uses the self-hosted fast-gate runner class declared in
`tools/scripts/runner_topology.json`. Namespace is not a
default PR-validation backend after the 2026-05-20 cost cutover. The
required branch-protection check on `main` is the literal `macos` context
published by the native matrix child (or the event-specific bootstrap when the
native child is absent). That name MUST NOT be renamed.

Routing variables (verify before debugging "stuck" macOS PRs):
- `PULP_DEFAULT_RUNNER_PROVIDER = github-hosted` (Linux/Windows default)
- `PULP_LOCAL_MACOS_RUNS_ON_JSON` must match the
  `tools/scripts/runner_topology.json` base-selector contract. For PR and merge
  events, `build.yml` replaces its legacy `pulp-gate-fast` discriminator with
  the applicable event-class label before assignment.
- `PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON = local-only` (no Namespace overflow)
- `PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON` should be unset unless the operator is deliberately testing Namespace

`shipyard pr` is the authoritative ship path. Do NOT push empty commits to
retrigger a slow macOS check. If macOS is queued >45 min, inspect the
off-fleet queue-age watchdog plus host supervisor/lease state and exact
repository assignment; do not treat a runner census as a verdict.

## Pre-push rebase hygiene

The macOS runner pool has changed several times. Before pushing a branch
whose CI touches a macOS leg, rebase onto current `main` so it picks up
the latest workflow fixes and runner labels. Skia-sensitive tests should
still be gated on `PULP_HAS_SKIA`; different runner images may or may not
have the bundled Skia archive available.

Before pushing ANY branch whose CI touches a macOS leg
(`Build and Test`, `Sanitizer Tests`, `Coverage`, `Visual Harness`,
`Release-path PR gate`, `macos-15`, the direct `macos` context, etc.):

```bash
git fetch origin main
git merge-base --is-ancestor origin/main HEAD \
  || echo "AT RISK — your branch is behind main; rebase before push"
```

If you're behind, prefer rebase over cherry-pick — main moves fast
during cutovers, and a rebase picks up all relevant invariant fixes
(not just one):

```bash
git fetch origin main
git rebase origin/main
PULP_SKIP_PREPUSH=1 git push --force-with-lease
```

If a rebase conflicts and you can't resolve quickly, cherry-pick the
specific test gate(s) you need from main:

```bash
git fetch origin main
git checkout origin/main -- test/<the-file>.cpp
git commit -m "test(...): pull cross-environment gate from main"
PULP_SKIP_PREPUSH=1 git push
```

### Don't retrigger via empty commit

`.github/workflows/build.yml` runs with `concurrency.cancel-in-progress:
true`. An empty `git commit --allow-empty && git push` cancels whatever
work the previous SHA was doing — including macOS legs that were 80%
through — and slots the new SHA to the BACK of the Namespace concurrency
queue. The correct re-run pattern when CI hit transient breakage is:

```bash
gh api -X POST repos/Generous-Corp/pulp/actions/runs/<RUN_ID>/rerun
# or to rerun only failed jobs:
gh api -X POST repos/Generous-Corp/pulp/actions/runs/<RUN_ID>/rerun-failed-jobs
```

That keeps your SHA + queue position, only re-fires the failed legs.

### Display-name vs runner-name gotcha

For PR, Shipyard manual-dispatch, and merge-group native work, the macOS matrix
job itself has the literal display name `macos`; branch protection gates on
that direct context. Bootstrap jobs use the same name only when the native job
is absent and an `-unused` name otherwise. Do not rename either ownership path
while debugging runner routing.

### Verifying your branch isn't burning macOS runner time

Each failed macOS leg consumes one of the scarce self-hosted runners and
queues every other PR behind your run. Before broadcasting "my CI is stuck",
inspect the exact job state and queue age, then the eligible hosts'
supervisor/lease/VM state and repository-visible assignment. An absent JIT
runner can mean healthy idle, while an organization-visible idle runner can be
phantom capacity.

If your branch's macOS leg is the only thing failing, rebase. If
multiple branches are failing on the same test, file an issue — that's
a real bug in main, not your branch.

### Cancel stuck previous-SHA runs to free the queue

When you rebase + force-push, the prior SHA's matrix runs are
cancelled by `concurrency.cancel-in-progress: true` automatically. But
if you HAD also kicked off rerun-failed-jobs on the previous SHA,
those rerun attempts can still consume runner minutes. Cancel them
explicitly:

```bash
gh api -X POST repos/Generous-Corp/pulp/actions/runs/<RUN_ID>/cancel
```

The default chain (`.github/workflows/build.yml` `resolve-provider` job):

```yaml
REQUESTED_PROVIDER:
  ${{ inputs.runner_provider             # explicit workflow_dispatch input
   || vars.PULP_DEFAULT_RUNNER_PROVIDER  # repo-level override
   || 'github-hosted' }}                 # hardcoded fallback
```

Priority order:
1. **macOS local GitHub runner** — `build.yml` reads
   `PULP_LOCAL_MACOS_RUNS_ON_JSON` into
   `EXPLICIT_MACOS_RUNNER_SELECTOR_JSON`. Do not copy a selector from this
   prose: use the exact value contracted in
   `tools/scripts/runner_topology.json` and verify it against the live repo
   variable with `runner_topology_check.py --mode=report`.
2. **GitHub-hosted Linux/Windows** — advisory; failures should be filed as platform issues and should not block a macOS-focused merge.
3. **Legacy SSH targets** — only when the user explicitly asks. Do not use `ssh ubuntu` or `ssh win` by default.

The `resolve_runs_on.py` optional-namespace mode must still honor explicit
selectors before checking `REQUESTED_PROVIDER`. Otherwise the local macOS repo
variable is ignored when the provider is `github-hosted`, and the required
`macos` gate falls back to hosted `macos-15`.

Build and coverage checkouts keep `lfs: false` even on macOS. The repo has
LFS attributes for historical Skia binary paths, but no current CI input is a
tracked LFS object; enabling checkout LFS on the reused self-hosted workspace
causes `git lfs install --local` to fail because Pulp already owns the
`pre-push` hook.

**Self-hosted macOS build dirs must stay isolated.** The local runner keeps
`build-*` directories between workflows. The ordinary `build.yml` matrix uses
`build-${{ matrix.key }}`, and `sanitizers.yml` uses `build-asan`,
`build-tsan`, `build-ubsan`, and `build-rtsan`. Do not collapse these back to
plain `build/`: a stale sanitizer `CMakeCache.txt` can leak flags such as
`-fsanitize=address` into the required macOS build and make unrelated
JavaScriptCore/host tests abort under ASan. `tools/scripts/test_workflow_build_dirs.py`
is wired into workflow-lint to keep this invariant machine-checked. CLI
delegation to helper binaries must resolve from the active build directory
(`build-${{ matrix.key }}` or the running CLI's sibling build tree), not a
hard-coded `build/` path; otherwise Linux catches missing helpers while warm
self-hosted macOS workspaces can mask the bug.

**PR validation excludes slow CTest labels.** The `build.yml` matrix always
excludes `validation` tests; on `pull_request` and `workflow_dispatch`
events it also excludes CTest tests labelled `slow`. Keep that conditional:
Shipyard PR validation arrives via `workflow_dispatch`, and slow configure
smokes can monopolize the self-hosted macOS runner. `tools/scripts/
test_workflow_build_dirs.py` asserts the label-exclude contract alongside
the build-directory invariant.

**Capability-history checks need the protected base in shallow checkouts.** A
depth-1 `pull_request` checkout contains GitHub's synthetic merge commit but not
its base parent. Before CTest, `build.yml` force-fetches the event-pinned
`pull_request.base.sha` into `refs/remotes/origin/main`; fetching moving current
main would compare against a different contract when main advances. Shipyard's
`workflow_dispatch` payload has no base SHA, so it retains the explicit main
fetch. `test_workflow_build_dirs.py` pins the workflow wiring and proves the
missing-base negative control plus the event-pinned repair with local shallow
repositories.

**GPU provenance checks need connected event ancestry, not detached objects.**
The GPU handoff and historical receipts intentionally validate old
`revision:path` identities and ancestor/last-owner relationships. Fetching each
SHA with `--depth=1` makes the objects readable but leaves them disconnected,
so it cannot satisfy those checks. The native `build.yml` job runs
`hydrate_gpu_provenance_commits.py`: it caps the checked-in Pulp revision set,
unshallows only the exact `GITHUB_REF` event history when required, and then
fails closed unless every named revision is a commit and an ancestor of HEAD.
The cap is a cardinality guard, not permission to truncate evidence. Keep its
accepted and rejected boundaries covered by
`test_hydrate_gpu_provenance_commits.py`, and raise the explicit bound with
headroom when legitimate closed handoff identities approach it; dropping or
coalescing latest-owner revisions would falsify provenance. The current bound
is 128 and the checked-in handoff plus independent probe receipt uses 65.
Do not replace that step with a fixed deepen count, a full all-refs clone, or
detached per-SHA fetches.

**macOS builds with the Ninja generator.** `build.yml`'s Configure step
passes `-G Ninja` on macOS only (Linux/Windows keep their default
generator). Ninja schedules parallelism better and is faster on the
warm/incremental builds the self-hosted M1 mostly does. Because the
self-hosted runners reuse `build-*` dirs (`clean: false`) and CMake
refuses to reconfigure a dir created with a *different* generator, the
Configure step recreates `$PULP_BUILD_DIR` when its cached
`CMAKE_GENERATOR` is not `Ninja` — so the first Ninja run on each runner
pays a one-time fresh-configure (the shared ccache stays warm). If you
add a new macOS build path, configure it with Ninja too, or it will
hit a generator-mismatch error against the warm dir.

### Overrides when you need them

- **Dispatch a build manually**: `runner_provider` defaults to
  `github-hosted` (Namespace is drained — `default: namespace` was the
  stale value that made every plain `workflow_dispatch` fail fast in
  `resolve-provider`). A plain dispatch routes Linux/Windows to
  GitHub-hosted runners; no `-f runner_provider` is needed:
  ```bash
  gh workflow run build.yml --repo Generous-Corp/pulp --ref <branch>
  ```
  Passing `-f runner_provider=namespace` will fail until the
  `PULP_NAMESPACE_BUILD_*_RUNS_ON_JSON` repo variables are restored.
- **Pin macOS to a local runner selector**: set
  `PULP_LOCAL_MACOS_RUNS_ON_JSON` at the repo level, or pass
  `macos_runner_selector_json` on a manual dispatch. Use the contracted label
  set in `tools/scripts/runner_topology.json`; do not maintain a second copy
  here.
- **Do not use Namespace overrides**: any remaining Namespace variable or mode is stale configuration and should be removed rather than worked around.

## Commands

### Legacy `local_ci.py ship [branch]`

Historical fallback only. The normal workflow for "ship this" or "push to
main" is `shipyard pr`, which owns PR creation, Shipyard tracking state,
validation, and merge-on-green.

Use this only when debugging the legacy local CI controller itself. It does
not provide the same Shipyard state discipline as `shipyard pr`.

**Module layout (post-2026-05-17 R2-1 split):**
`tools/local-ci/local_ci.py` is the orchestrator; reusable seams have
been moved into sibling modules so newer code can import them without
pulling in the entire 11k-line file.

The authoritative extraction map is
`tools/local-ci/MODULE_MAP.md`. When touching local-CI extraction
boundaries, keep that map and `tools/local-ci/test_local_ci_contracts.py`
in sync so future code-motion PRs preserve the queue/evidence, target
preflight, source-prep, cleanup, and artifact-publishing contracts.

- `state_paths.py` — owns `state_dir()`, `queue_path()`, `results_dir()`,
  `logs_dir()`, `ensure_state_dirs()`, and the lock-path helpers.
- `normalize.py` — owns priority/validation/desktop normalization
  helpers (`normalize_priority`, `priority_value`,
  `normalize_validation_mode`, `normalize_desktop_*`, `default_desktop_*`,
  `parse_config_bool`, `infer_desktop_adapter`, `normalize_desktop_config`)
  plus the `PRIORITY_VALUES` constant.
- `git_helpers.py` — owns the git + time helpers used by the queue and
  evidence subsystems (`now_iso`, `current_branch`, `current_sha`,
  `git_root_for`, `resolve_git_ref_sha`, `short_sha`) plus the shared
  `ROOT` constant.
- `io_utils.py` — owns the I/O + locking utilities (`tail_lines`,
  `trim_line`, `atomic_write_text`, `image_change_summary`, `file_lock`)
  plus the `LockBusyError` exception. `image_change_summary` falls back
  to a SHA-256 file comparison when Pillow is missing so the test suite
  keeps running on stripped images.
- `footprint.py` — owns disk-footprint accounting helpers
  (`format_size_bytes`, `path_size_bytes`, `local_ci_state_footprint`,
  `describe_path_for_cleanup`). Used by `pulp ci-local status` and the
  cleanup paths to report how much disk the local CI state is using.
- `provenance.py` — owns provenance dict helpers (`normalize_provenance`,
  `provenance_summary`, `normalize_result`) carried on every job + result
  record. Pure functions, no I/O.
- `job_queue.py` — owns the queue persistence layer (`normalize_job`,
  `load_queue_unlocked`, `save_queue_unlocked`). Named `job_queue` (not
  `queue`) to avoid collision with the stdlib `queue` module. The
  lock-acquiring `load_queue` stays in `local_ci.py` because it pulls
  in the running-job reconcile state machine.
- `targets.py` — owns target enable/parse/resolve helpers
  (`enabled_targets`, `parse_targets_arg`, `resolve_targets`). Pure.
- `github_workflows.py` — owns the GitHub Actions workflow dispatch
  cluster: `GITHUB_ACTIONS_DEFAULTS`, `BUILTIN_GITHUB_WORKFLOWS`,
  `REPO_VARIABLE_FALLBACKS` constants + 11 resolver functions
  (`github_actions_settings_for_display`, `resolve_github_actions_settings`,
  `normalize_runs_on_json`, `resolve_workflow_runner_selector_json`,
  `resolve_workflow_dispatch_field_values`, `repo_variable_name_for_workflow_field`,
  `resolve_default_provider_for_workflow`, `resolve_workflow_field_value_and_source`,
  `resolve_workflow_dispatch_defaults`, `summarize_workflow_provider_defaults`,
  `resolve_cli_dispatch_field_values`). Pure resolution — the actual
  subprocess `gh-api` dispatch still lives in `local_ci.py`.
- `evidence_index.py` — owns the local-CI evidence index: result-to-evidence
  normalization, latest passing target records, evidence index persistence,
  branch/SHA grouping, and evidence summaries. Queue mutation, runner state,
  result creation, and target execution stay out of this module.
- `desktop_doctor.py` — owns desktop automation capability derivation,
  writable-artifact checks, WebDriver status probing, and doctor-check
  assembly. Keep CLI output formatting, desktop action execution, artifact
  persistence, and launch-adapter orchestration outside this module.
- `desktop_actions.py` — owns pure desktop action helper policy:
  action artifact path layout, interaction detection/summaries, coordinate
  parsing, view-tree click selection, inspector summaries, content-size
  mapping, screen-point mapping, default labels, and view-tree counts. Keep
  target execution, artifact persistence, report rollups, and OS-specific
  launch/probe helpers out of this module.
- `desktop_cli.py` — owns desktop automation CLI line fragments for status,
  config, action success, recent-run, proof, publish, and cleanup output. Keep
  target execution, artifact persistence, report rollups, proof selection, and
  desktop action policy out of this module.

All original symbols are re-exported from `local_ci.py`, so any old
`mod.state_dir()` / `mod.normalize_priority()` / `mod.current_sha()` /
`mod.file_lock(...)` / `mod.BUILTIN_GITHUB_WORKFLOWS` /
`mod.collect_evidence_groups(...)` test patch keeps
working — but new code should import directly from the sibling module
to avoid the god-module dependency.

```bash
# Legacy fallback only
python3 tools/local-ci/local_ci.py ship [branch]

# Ship to a develop branch (for multi-piece features)
python3 tools/local-ci/local_ci.py ship [branch] --base develop/package-manager
```

**Develop branch workflow:** When working on complex features that use a `develop/*` integration branch, PRs target the develop branch instead of main. The develop branch itself merges to main at phase boundaries. Use `--base` to specify the target.

### `run [branch]` — Just validate, no PR

Run local CI on the current branch without creating a PR or merging.

```bash
python3 tools/local-ci/local_ci.py run [branch]
python3 tools/local-ci/local_ci.py run [branch] --smoke
```

If you pass a branch name explicitly, `run [branch]` resolves that branch tip to an exact SHA before queueing. It must not silently reuse the launching checkout's `HEAD`.

Queueing now performs a submission preflight before the job is recorded:
- prints the queued worktree root, current cwd, config path/source, and per-target host intent
- rejects accidental wrong-root launches by default
- rejects obviously unreachable SSH targets by default when they have no fallback path

Override flags exist for deliberate exceptions:

```bash
python3 tools/local-ci/local_ci.py run [branch] --allow-root-mismatch
python3 tools/local-ci/local_ci.py run [branch] --allow-unreachable-targets
```

For SSH targets, `run` uploads the exact queued SHA as a git bundle before validation, so Ubuntu and Windows do not need that branch tip to be visible on the host ahead of time.
Use `--smoke` for a fast clean install/export preflight when you want early signal before paying for the full test matrix. Smoke runs are explicitly labeled as `validation=smoke`.
If you queue a newer SHA for the same branch, targets, and validation mode, older pending work is superseded automatically.

Useful queue controls:

```bash
python3 tools/local-ci/local_ci.py run [branch] --targets mac
python3 tools/local-ci/local_ci.py run [branch] --smoke --targets windows
python3 tools/local-ci/local_ci.py enqueue [branch] --priority low
python3 tools/local-ci/local_ci.py bump <job-id> high
python3 tools/local-ci/local_ci.py logs <job-id> --target windows
python3 tools/local-ci/local_ci.py evidence [branch]
```

If the task needs GUI/session proof instead of pure build/test validation, use the desktop automation surface on the same controller:

```bash
python3 tools/local-ci/local_ci.py desktop install mac
python3 tools/local-ci/local_ci.py desktop doctor windows --json
python3 tools/local-ci/local_ci.py desktop smoke mac --bundle-id com.apple.TextEdit --label textedit-smoke
python3 tools/local-ci/local_ci.py desktop inspect mac --command '/path/to/pulp-ui-preview' --pulp-app-automation
python3 tools/local-ci/local_ci.py desktop click mac --command '/path/to/pulp-ui-preview' --click-view-id bypass-toggle --capture-ui-snapshot --pulp-app-automation
python3 tools/local-ci/local_ci.py desktop inspect windows --command 'notepad.exe' --label notepad-inspect
python3 tools/local-ci/local_ci.py desktop click windows --command 'notepad.exe' --click 885,18 --capture-before --label notepad-maximize
python3 tools/local-ci/local_ci.py desktop inspect mac --command './build-desktop-automation/examples/ui-preview/pulp-ui-preview' --source-mode exact-sha --sha <commit-sha> --prepare-command 'cmake -S . -B build-desktop-automation && cmake --build build-desktop-automation --target pulp-ui-preview' --pulp-app-automation
python3 tools/local-ci/local_ci.py desktop config set target.mac.webview_driver true
python3 tools/local-ci/local_ci.py desktop config set target.mac.webdriver_url http://127.0.0.1:4444
python3 tools/local-ci/local_ci.py desktop config set target.mac.debug_attach true
python3 tools/local-ci/local_ci.py desktop recent mac
python3 tools/local-ci/local_ci.py desktop proof windows --action inspect --source-mode exact-sha --sha <commit-sha>
python3 tools/local-ci/local_ci.py desktop publish mac --limit 5 --label mac-gallery
```

Desktop adapter truth:
- `macos-local`: local logged-in session, supports `--bundle-id` and Pulp-owned direct-app automation.
- `linux-xvfb`: `xvfb-run` GUI lane, currently `--command` + `--pulp-app-automation` for richer interaction.
- `windows-session-agent`: Scheduled Task + target-side PowerShell agent in a logged-in desktop session; supports generic `window-capture` smoke/click/inspect for normal desktop apps and reserves `--pulp-app-automation` for ViewInspector/UI-selector lanes.
- Optional WebView/debug tiers are opt-in config, not implied adapter behavior. Use `desktop status` / `desktop doctor` to confirm `optional_capabilities` before assuming `webview_dom`, `debug_attach`, `video_capture`, or `frame_stats`.

Exact-SHA desktop source guidance:
- Use `--source-mode exact-sha` when the GUI proof must match one specific commit instead of the mutable live checkout.
- Pair it with `--prepare-command` when the binary or assets must be built inside the prepared root before launch.
- Exact-SHA desktop runs persist additive provenance in `manifest.json` under `source.*` and attach `artifacts.prepare_log` when a fresh prepare step runs.
- Treat exact-SHA desktop mode as a `--command` workflow unless the adapter explicitly documents stronger support.
- Use `desktop proof` instead of ad-hoc bundle inspection when you need the latest successful GUI proof for one target/action/SHA.

### `check <PR#|URL|latest>` — Validate an existing PR

Run CI on an existing PR by number, GitHub URL, or "latest".

1. If `latest` → use `gh pr list --limit 1 --json number` to get the most recent PR
2. If URL → extract PR number from the URL
3. Fetch the PR's head branch: `gh pr view <number> --json headRefName`
4. Checkout that branch locally
5. Run local CI: `python3 tools/local-ci/local_ci.py run <branch>` or `python3 tools/local-ci/local_ci.py check <PR#> --smoke` for a fast preflight
6. Post results as a PR comment via `gh pr comment`

### `list` — Show open PRs

Show open PRs with summaries so the user can pick one to check or merge.

```bash
gh pr list --json number,title,author,headRefName,createdAt,labels --template '{{range .}}#{{.number}} {{.title}} ({{.headRefName}}) by {{.author.login}} {{timeago .createdAt}}{{"\n"}}{{end}}'
```

### `status` — Queue, live target state, and VM status

```bash
python3 tools/local-ci/local_ci.py status
```

While a job is still running, `status` can show live target state for the active job, for example `mac=pass, ubuntu=pass, windows=running`. Quiet phases should now surface heartbeat and idle/liveness hints instead of looking dead by default.

### `logs [job]` — Tail a saved target log

```bash
python3 tools/local-ci/local_ci.py logs <job-id> --target windows
```

Use this when a target looks slow or stuck. The logs come from the machine-global CI state directory, so you can inspect a running job without manual SSH.

When you need to reproduce an intermittent failure locally before spending another full CI run, use:

```bash
tools/scripts/repeat-until-fail.sh 100 -- ctest --test-dir build -R "<test name>" --output-on-failure
```

### `evidence [branch]` — Show last-good exact-SHA target evidence

```bash
python3 tools/local-ci/local_ci.py evidence
python3 tools/local-ci/local_ci.py evidence feature/my-branch --limit 3
```

Use this when a branch has been validated through narrow reruns and you need to know what is already proven without rerunning targets that already passed on the same SHA.

## Active CI Incident Loop

Do not treat a running CI job as something to check later. When a local CI job is active, one agent should own the monitoring loop and one agent or process should work the likely fix path locally as soon as a failure becomes actionable.

Required behavior while a job is active:
- Poll `python3 tools/local-ci/local_ci.py status` proactively.
- Tail `python3 tools/local-ci/local_ci.py logs <job-id> --target <name>` as soon as a target fails or looks stuck.
- Send user updates without waiting to be asked when a target changes state, a failure appears, or a rerun is queued.
- If one target has already failed, stop treating the rest of that job as the only source of truth. Start local repro or code inspection immediately when the failure is actionable.
- Once a failure is actionable, parallel work is required unless it would contend with the same host or invalidate the active run. Keep one CI owner watching the hosts and one local loop reproducing or patching the likely issue.
- Do not sit idle waiting for unrelated targets to finish if the first failing target already tells you what to investigate.
- Never rerun a target that already passed on the exact same SHA unless the prior result is untrustworthy or the environment changed.
- If only part of the matrix is stale, rerun only that subset of targets.
- Once the failure surface is isolated, prefer the minimum sufficient proof instead of a symmetric rerun. If macOS and Ubuntu already passed on the current head and only Windows changed or failed, rerun Windows only.
- A direct exact-SHA validation on one target is acceptable merge evidence for that target. Do not invalidate earlier same-SHA passes on other targets just because they came from a different run.
- Use `validation=smoke` before full CI when the risk is install/export/build structure rather than runtime test behavior.
- Treat `all targets on one SHA` as a goal, not a reason to blindly rerun already-green same-SHA targets.
- On persistent local/self-hosted targets, prefer prepared same-SHA reruns for narrow follow-up validation and make `prepared=clean` vs `prepared=reused` visible in status/logs.
- Prefer the shared machine-global CI config (`state_dir()/config.json`; on macOS `~/Library/Application Support/Pulp/local-ci/config.json`) so every worktree sees the same host map by default.
- Treat worktree-local `tools/local-ci/config.json` as a fallback or temporary override only. Hostnames and `repo_path` values can drift between worktrees.
- Pay attention to the submission preflight. If it says the cwd git root and queued worktree root differ, stop and fix that unless the mismatch is intentional.
- If preflight reports shared-state vs worktree-local config drift for the selected targets, treat that as a real warning, not cosmetic noise.
- For Windows SSH validation, prefer the configured target whose non-interactive PowerShell context resolves `git`, `cmake`, and `ctest`. Do not encode developer-specific host aliases in shared instructions; keep the selection criteria generic and environment-driven.
- If a dead runner left behind a stale Windows validator, let the queue reclaim that specific remote validator before starting fresh work; treat that cleanup as part of the truthful narrow-rerun path, not as ad hoc manual SSH.
- If a stale same-host job is still compiling an obsolete SHA or path, stop that stale process tree before spending more time diagnosing contention on the current run.

Minimum incident response once a failure is visible:
1. Capture the failing job id, target, SHA, validation mode, and first failing test/build step.
2. Decide whether the failure is infrastructure, environment drift, or a likely code/test issue.
3. If actionable, begin the fix or repro loop immediately instead of waiting for the whole matrix to finish.
4. Queue the narrowest truthful rerun needed after the fix.
5. Close the loop with a short status update that says what failed, what is being tried now, and what still remains.

### Gotcha: stop a leftover CI-watch Monitor by the Monitor's task id, not the agent's

When an agent spawns a background Monitor (or a poll loop) to watch a PR's check-runs, that Monitor is a **separate task** from the agent. After the agent's work finishes, the Monitor keeps running — re-emitting "agent completed" events and burning shared REST quota polling `commits/<sha>/check-runs` — until its until-condition is met or it is explicitly stopped.

To stop a leftover Monitor: `TaskStop` **the Monitor's own task id** (the still-running poll-loop task — visible in the task list as a `local_bash` task). Do NOT `TaskStop` the agent's id: once the agent has finished it is already `completed`, so `TaskStop <agent-id>` fails with `not running (status: completed)` and the Monitor keeps going. The agent id and the Monitor id are different — target the Monitor.

### `cloud run [branch]` — Trigger GitHub Actions

Trigger cloud CI only when cloud CI is actually needed, for example
workflow-semantics changes, release validation, or a neutral-host confirmation
that local CI cannot provide. Prefer the built-in `pulp ci-local cloud ...`
surface instead of raw `gh workflow run`:

```bash
pulp ci-local cloud workflows
pulp ci-local cloud defaults
pulp ci-local cloud history
pulp ci-local cloud compare build
pulp ci-local cloud recommend build
pulp ci-local cloud run build <branch>
pulp ci-local cloud run validate <branch>
pulp ci-local cloud run docs-check <branch> --provider namespace
```

### `cloud status` — Check GitHub Actions

```bash
pulp ci-local cloud status
pulp ci-local cloud status latest --refresh
```

`cloud defaults` is the companion visibility command when you need to see the
current effective workflow/provider defaults and where Namespace selectors are
coming from before dispatching a run.

`cloud history`, `cloud compare`, and `cloud recommend` are the next visibility
layer when you need saved timing/provider evidence from earlier runs. Any cost
number shown there is `estimated; verify provider pricing`.

Use raw `gh workflow run` / `gh run view` only as a fallback when debugging the
GitHub side of the operator surface itself.

Provider truth rules:
- If a Namespace cloud dispatch fails before any matrix leg starts, inspect the GitHub run annotations and `resolve-provider` job result before blaming repo code.
- Treat provider CLI health, GitHub dispatch health, and provider billing/spend gates as separate failure surfaces.
- If cloud dispatch is blocked by billing or provider control-plane issues, cut over to the narrowest truthful local/SSH proof instead of retrying the same blocked dispatch loop.

## Configuration

Config is machine-global by default at `state_dir()/config.json` (on macOS `~/Library/Application Support/Pulp/local-ci/config.json`).
`tools/local-ci/config.json` is the gitignored fallback, and `PULP_LOCAL_CI_CONFIG` overrides both.
Template at `tools/local-ci/config.example.json`.

Key fields:
- `targets.mac.enabled` — run local Mac validation (default: true)
- `targets.ubuntu` — SSH target for Linux validation
- `targets.windows` — SSH target for Windows validation
- `targets.<name>.host` — primary SSH host alias
- `targets.<name>.fallback_host` — optional secondary SSH host alias if the primary is unreachable
- `targets.<name>.utm_fallback` — optional UTM VM to boot only if SSH hosts are unreachable
- `targets.windows.cmake_generator` / `targets.windows.cmake_platform` — optional Windows CMake generator settings; if `cmake_platform` is omitted the runner infers `ARM64` vs `x64` from the remote host
- `targets.windows.cmake_generator_instance` — optional explicit Visual Studio instance path; if omitted the runner prefers a full VS install over `BuildTools` when both exist
- `defaults.priority` — default queue priority for `run` and `enqueue`
- `defaults.ship_priority` — default queue priority for `ship`
- `defaults.check_priority` — default queue priority for `check`
- `github_actions.repository` — optional `owner/repo` override for cloud commands
- `github_actions.defaults.workflow` — default workflow key for `cloud run`
- `github_actions.defaults.provider` — default cloud runner provider
- `github_actions.defaults.wait_poll_secs` — cloud wait polling interval
- `github_actions.defaults.match_timeout_secs` — dispatch-to-run match timeout

Keep hostnames and VM names local. Shared repo docs and skills should describe how to choose a target, not which personal alias to use.

## Documentation

Full setup guide: `docs/guides/local-ci.md`
SSH key setup for Windows/Linux VMs: `docs/guides/local-ci.md` § "Set up SSH keys"

**Docs-site CI workflows:**

- `.github/workflows/docs-deploy.yml` — builds + deploys the Pulp docs
  site to GitHub Pages. As of #577 PR 4 this is **MkDocs Material only**:
  the legacy `tools/build-docs.py` generator and the
  `use_legacy_generator` workflow_dispatch fallback have both been
  deleted. Rollback, if ever needed, is via `git revert` of the
  deletion commit, not a runtime toggle. Pagefind is gone (Material
  ships its own built-in search). The workflow also invokes
  `tools/build-api-docs.sh`, which pulls the current SDK version from
  `CMakeLists.txt` and injects it into Doxygen's `PROJECT_NUMBER`.
- `.github/workflows/docs-material.yml` — parallel MkDocs Material build
  (PR-only preview) added under #577 PR 1 and extended in PR 2 to build
  Doxygen + merge `api/` into the artifact and run the
  `tools/mkdocs_hooks.py` pre-build drift checks (`docs_generate.py
  check` + `check-docs-consistency.py`) plus the URL-flatten hook. Runs
  on `pull_request` and `push` when the same docs paths change.
  **No deploy** — uploads `build/site-material/` as a 14-day artifact
  for visual review. Kept as a PR-lane preview after PR 3 so reviewers
  see rendered output before it hits production.

## Required-check ruleset (issue #462)

The branch-protection ruleset for `main` is checked into the repo at
`.github/rulesets/main-protection.json` so drift between the GitHub
ruleset UI and repo intent is visible in PR review. Pattern inspired
by [Astral's ruleset-as-code approach](https://gist.github.com/woodruffw/643a6cf70ad72d404ce6f9f333181cf8).

**Fast lane — required (blocks merge):**

- `macos` — macOS build+test leg of `.github/workflows/build.yml`
- `Enforce version & skill sync` — `.github/workflows/version-skill-check.yml`
- `Build + prove + (owner-gated) deploy`
- `Vellum freeze` — `.github/workflows/vellum-freeze-check.yml`
- `Vellum trusted freeze` — status posted by `.github/workflows/vellum-trusted-gate.yml`

`vellum-routing-contract` is a separate evidence-producing check. It executes
the closed repository-qualified Pulp/Vellum router suite on relevant PRs and on
every push to `main`. Once `.github/vellum-ownership.json` contains an accepted
exact-route expansion, the push-main run uploads the tiny, digest-bound
`pulp-vellum-routing-contract-execution` artifact consumed by Vellum's release
verifier. A green PR run is not a substitute: release evidence requires the
exact Pulp merge commit, a `push` event on `main`, and the matching run receipt.

**`linux` and `windows` are NOT required** — they are advisory GitHub-hosted
lanes. Verify the live list rather than trusting any doc, and read the right
surface:

```sh
ghapp api repos/Generous-Corp/pulp/branches/main/protection \
  --jq '.required_status_checks.contexts'
```

Enforcement lives in **classic branch protection**, not in a ruleset: the only
branch ruleset (`main-merge-queue`) carries a `merge_queue` rule and no
`required_status_checks` rule, so `gh api …/rulesets` reads as "nothing is
required" and has already caused a required check to be written off as
advisory. GitHub honours both surfaces; `ruleset-drift-check.yml` unions them.

The three platform names are intentionally declared as **stable aliases**
so the merge contract survives runner-provider swaps (github-hosted ↔
namespace). The concrete context strings in `build.yml` today resolve
to e.g. `macOS (ARM64) [namespace]`, which is not stable; landing the
alias layer is part of #462.

**Slow lane — advisory (does NOT block merge):**

- `AddressSanitizer (macOS ARM64)`
- `ThreadSanitizer (macOS ARM64)`
- `UndefinedBehaviorSanitizer (macOS ARM64)`
- `RealtimeSanitizer (Linux x86_64, Clang 18)`

These run via `.github/workflows/sanitizers.yml` on `workflow_dispatch`
only and are tracked in the checked-in JSON under `advisory_status_checks`
for visibility/drift, never inside `rules[].required_status_checks`.

**Drift enforcement:** `.github/workflows/ruleset-drift-check.yml` runs
on PR (when `.github/rulesets/**` changes) and weekly on cron. It reads **both**
live surfaces — the named ruleset and classic branch protection — and diffs
their union against the checked-in JSON. PR runs post/update a single comment;
the cron job fails loudly on drift so it shows up as a red check on `main`.

A missing ruleset is **not** drift by itself: the contract may live entirely in
classic branch protection, which is the case here. An unreadable surface (403 —
the token lacks `administration: read`) **is** drift, deliberately, so a scope
problem can never be mistaken for "nothing is required". Reading only
`/rulesets` is what kept this check red on cron from 2026-07-14 onward while the
real contract was intact.

**Making a change to required checks:** always edit the JSON first, open
a PR, and let the drift-check workflow confirm the plan. Then mirror the
change in the GitHub ruleset UI (or reapply via `gh api PUT`). Never edit
the live ruleset in isolation — the next scheduled drift run will fail.

### Required checks live in TWO places — check both before calling one advisory

`main`'s merge contract is enforced by **classic branch protection**, not only
by rulesets. Reading `gh api repos/{owner}/{repo}/rulesets` alone is how you
reach the wrong conclusion that a red check is harmless: the only branch ruleset
is `main-merge-queue`, which carries a `merge_queue` rule and **no**
`required_status_checks` rule at all. The actual required contexts come from:

```sh
ghapp api repos/Generous-Corp/pulp/branches/main/protection \
  --jq '.required_status_checks.contexts'
```

which today returns `macos`, `Enforce version & skill sync`, `Build + prove +
(owner-gated) deploy`, `Vellum trusted freeze`, and `Vellum freeze`. Never
describe a check as non-blocking, or propose deleting it, on ruleset evidence
alone.

Two consequences worth knowing. `.github/rulesets/main-protection.json`
declares a ruleset named `main-protection` that **does not exist live**, so the
scheduled `ruleset-drift-check.yml` has been failing — the checked-in intent is
currently aspiration, and the file's two required contexts are a subset of the
five that classic protection really enforces. And a PR whose only red checks are
Vellum ones is genuinely **not mergeable**, even though nothing in the ruleset
says so; arming auto-merge on it is safe but will simply wait.

### The three red Vellum rows are one failure, not three broken checks

`Vellum freeze`, `Trusted base executor`, and `Vellum trusted freeze` appearing
red together is the normal shape of a *single* freeze-check failure:

| Row | Source | Required? |
|---|---|---|
| `Vellum freeze` | job in `vellum-freeze-check.yml`, on `pull_request` | **yes** |
| `Trusted base executor` | job in `vellum-trusted-gate.yml`, on `pull_request_target` — re-runs the same check from the trusted base | no |
| `Vellum trusted freeze` | the commit status `Trusted base executor` posts, *and* the `merge_group` job name | **yes** |

So `Vellum trusted freeze` legitimately shows up twice in `gh pr checks` — once
as the posted status, once as the `merge_group` job skipped on a `pull_request`
event. Fix the underlying freeze-check failure and all three clear together.

The usual cause is a change to a `framework-authoritative-transferred` slice
with no matching change event under `.github/vellum-change-events/`. The
diagnostic names each uncovered slice, the changed paths behind it, and the
`disposition` values available; reproduce it locally rather than reading CI logs:

```sh
python3 tools/scripts/vellum_freeze_check.py \
  --source-head HEAD --output /tmp/vellum-outbox.json
```

`--base` defaults to the fork point (`git merge-base origin/main HEAD`), which
is the comparison CI performs. Passing `--base origin/main` instead diffs
against the base-branch *tip*, so every change event that landed on `main`
after you forked reads as a deletion — the checker then fails where the gate
passes. Override `--base` only to reproduce a specific CI comparison.

This boundary is deliberate infrastructure, not ceremony — it keeps Pulp changes
to extracted components ingestible by `Generous-Corp/vellum`. Do not "green it
up" by weakening the check or by writing a `pulp-only` disposition you cannot
justify; the rationale field is the artifact that makes a later ingest decision
possible. `docs/contracts/vellum-extraction-freeze.md` is the contract.

**If `Vellum freeze` passes but `Trusted base executor` fails, first check
whether the PR is merely behind `main`.** The two jobs run the same check, so
disagreement can be a base-resolution artifact rather than a content problem.
The trusted gate evaluates a **locally constructed merge result** for this
reason: `base.sha` is the
base-branch *tip*, so diffing it against the raw branch head reports every file
that landed on `main` after you forked as *deleted by your PR* — and an outbox
event someone else added then trips `Vellum outbox events are append-only;
modify/delete/rename is forbidden` against an author who deleted nothing. A
The workflow fetches and verifies the exact API-resolved PR head, then trusted
base code constructs a two-parent candidate from that head and the checked-out
protected-main commit. It never consumes GitHub's `refs/pull/N/merge`, because
that synthetic ref can retain an older first parent while a PR is `BEHIND`.
A genuinely conflicted PR fails local merge construction and is reported as
such, rather than as a freeze violation.

**Do not merge `main` into the branch to clear it.** That was the workaround
before the gate evaluated the merge result; it no longer fixes anything, and
each merge rewrites the head, cancels in-flight validation and starts another
full lane — against a `main` that moves hourly it never converges. If the two
jobs still disagree, re-run the trusted gate rather than moving the branch.

The `pull_request_target` + `statuses: write` shape on the trusted gate is
intentional and safe as written: it checks out literal protected `main` with
`persist-credentials: false`, proves that commit is still the live PR base
before minting secrets, executes **only** trusted-checkout scripts, and adds the
PR head as a worktree that is read as data and never executed. Preserve all
four properties when editing that workflow — running anything out of
`$proposed_tree` would hand a fork PR the Vellum reader credentials.

CodeQL treats a checkout ref carried through a preceding step output as
untrusted in a privileged workflow, even when that step resolved the live
`base.sha` through the API. Keep the privileged checkout ref a literal
`refs/heads/main`, restrict the trigger and resolved PR target to this
repository's `main`, accept manual dispatch only when the workflow itself was
loaded from `refs/heads/main`, and compare the checked-out commit to the
resolved live base SHA before minting any secret-bearing token. A mismatch is a
fail-closed main-advanced race and should be re-run; never restore a PR-derived
checkout ref to avoid that retry.

### `merge_group` belongs ONLY on workflows that produce a required context

A queue entry re-runs every workflow that declares `merge_group:`. The live
build window is configurable, so any
workflow on `merge_group` whose contexts are **not** required sets the drain
rate while being unable to affect the merge decision. Nine workflows once fired
per entry when only five could gate; two of the extras (`Validate examples
(macOS)`, `GPU audio proof (macOS, real WebGPU)`) claimed a self-hosted macOS
runner each, competing with the required `macos` gate for the same three-Mac
pool. Symptom: 87 queued runs against 9 in progress, studio runners idle, head
entry's runs all stuck `queued`. Adding Macs cannot fix that.

The invariant, in both directions:

- **On `merge_group` ⇔ one of the workflow's job names is a required context.**
  Adding it elsewhere throttles the queue for no gating value.
- **Never remove `merge_group` from a workflow whose context IS required** — a
  required check that never reports on a merge group leaves entries unresolvable
  and wedges the queue permanently. That is strictly worse than slow.

Check before editing a trigger, and trust neither the comments nor this list:

```sh
ghapp api repos/Generous-Corp/pulp/branches/main/protection \
  --jq '.required_status_checks.contexts[]'
```

Three workflows carried comments asserting they ran a "required" check on merge
groups when none of their job names was required — that drift is what made the
queue slow, so re-verify rather than believing the comment.

Dropping `merge_group` costs no PR-time signal: those workflows keep
`pull_request` and still run on every PR. They just stop re-running against a
merged result they cannot gate.

### Diagnose a stalled queue from its active build window

`tools/scripts/merge_stall_watchdog.py` treats the merge queue as its own
failure surface. It reads `maximumEntriesToBuild`, tracks activity only across
the entries GitHub can currently build, and reports the head's exact synthetic
merge SHA and unresolved required contexts. Activity from entries outside that
window must not suppress an alarm; activity from a cumulative follower inside
it is real progress. If live collection is degraded, preserve prior state and
fail closed instead of clearing an existing incident from an incomplete sweep.

Use the head-specific blocker list before cancelling anything. A queued
required context is evidence of capacity pressure, not a disposable run; only
cancel exact-current advisory work after proving it cannot contribute to a
required context or another agent's live PR.

### Install consumer smoke (`install-consumer-smoke.yml`)

Pulp #2087 piggyback. Catches the class of bug where in-tree builds
work but installed-SDK consumers break at configure time. Runs on
macos-15 + ubuntu-24.04: builds Pulp, `cmake --install`s it to a temp
prefix, then configures a minimal downstream `find_package(Pulp)` +
`pulp_add_plugin(...)` project against that prefix. Failures here
match what a real downstream (e.g. Spectr) would hit.

Defense-in-depth guard: greps the installed CMake config files for
`${CMAKE_SOURCE_DIR}/tools/cmake/...` or `${CMAKE_SOURCE_DIR}/core/...` —
those patterns inside files that ship in the SDK tarball resolve to
the *consumer's* source tree at find_package time, never Pulp's.
Inside a function body in `tools/cmake/PulpUtils.cmake`, use
`CMAKE_CURRENT_FUNCTION_LIST_DIR`; at top level of a config file,
use `CMAKE_CURRENT_LIST_DIR`. The two existing helpers paths
(`_pulp_add_standalone` for fontconfig, top-level fallbacks for
`_PULP_FORMAT_SOURCE_DIR` etc.) demonstrate the pattern.

### Downstream validation manifest (P0.4)

`tools/validation/downstream/consumer-validation.json` records the
external consumer checklist for refactors that can affect installed SDK,
embed ABI, ProjectIR, or generated UI bundle behavior. It is not a
per-PR external-build gate; it is the canonical inventory of which
downstream repos must be run for a given API/schema/ABI surface and what
evidence each run must produce.

`tools/scripts/verify_downstream_validation_manifest.py` is the fast
schema/checklist guard. `version-skill-check.yml` runs
`tools/scripts/test_downstream_validation_manifest.py` with the other
gate-script fixture tests so the manifest cannot drift to a stale SDK
recipe, drop a roadmap P0.4 consumer, or conflate ProjectIR importer
coverage with Pulp DesignIR coverage. Use `--check-local` only on a
developer machine when you want advisory checkout presence and current
HEAD reporting.

## Versioning & Skill-Sync gates (Layer 3)

`pulp pr` orchestrates the full shipping flow. CI enforces the fast invariant gates on every PR to `main`:

- `.github/workflows/version-skill-check.yml` — runs `tools/scripts/version_bump_check.py`, `tools/scripts/skill_sync_check.py`, `tools/scripts/skill_path_map_lint.py`, `tools/scripts/compat_sync_check.py`, `tools/scripts/compat_aggregate.py check`, `tools/scripts/node_abi_gate.py`, and `tools/scripts/hotspot_size_guard.py` in `--mode=report`. Failure blocks merge. No bypass except the commit trailers documented in `docs/guides/versioning.md` and `docs/guides/compat-sync.md`; the node ABI gate is fixed by preserving existing virtual declarations or appending new virtuals at the tail, and the hotspot-size guard is fixed by shrinking the tracked file, moving code behind a split, or intentionally raising the baseline in `tools/scripts/hotspot_size_guard.json` with the reason in the PR.
- `.shipyard/config.toml` → `[validation.gates]` pipeline — same scripts via `shipyard run --pipeline gates`. Runs with `PULP_ENFORCE_PREPUSH=1` so warnings become errors.

Locally:

- `.githooks/pre-push` (install via `tools/scripts/install-githooks.sh`) runs the same fast scripts, including the compat-sync, compat-aggregate, node ABI, and hotspot-size gates, enforcing by default. `PULP_DISABLE_PREPUSH_GATES=1` demotes the fast gates to advisory; `PULP_SKIP_PREPUSH=1` is the single-push emergency bypass.
- Pre-push gate subprocesses run through `.githooks/lib/gate-output.sh`: their stdout/stderr is captured to a regular temporary file, then replayed best-effort. Keep user-visible fast gates and the slow diff-cover script behind `run_gate_captured`; agent/tool callers can supply nonblocking pipes, and an uncaptured Python `print()` may otherwise raise `BlockingIOError` and turn a healthy gate into a false failure. The wrapper must snapshot and return the real child status before replay, while replay failure never changes that status. `tools/scripts/test_prepush_gate_output.py` pins success, exact nonzero propagation, replay, cleanup, and the slow-lane callsite.
- `tools/scripts/docs_noise_lint.py` (pre-push only, report mode) — scans changed/added lines for transient breadcrumbs across the markdown default scope (docs/reference + skills) **and source comments + test tags** under `core/examples/tools/test/apple/inspect/ship`. Source is diff-scoped only (never `--all`), so the historical backlog never blocks; it is comment-aware (only `//`, `/* */`, `#` comment text + string-literal Catch2 `[tag]`s, never code). Escape a legitimate line with an inline `docs-noise-lint: skip <reason>` comment. Full guidance on *writing* durable comments lives in the `code-comments` skill.
- `tools/scripts/gates.sh` — on-demand runner for JUST the cheap gates (skill-sync + skill-path-map lint + version-bump + compat-sync + compat-aggregate + node-ABI + hotspot-size + deps-audit + codecov-config). Runs in ~1 second, exits non-zero on any hard failure with a one-liner pointing at the right surgical bypass. The codecov-config gate is a *global invariant* (not diff-scoped): it runs the `test_codecov_config.py` / `test_codecov_components.py` contract tests so a new `core/<sub>/` subsystem can't land without a matching `codecov.yml` flag+component (graph/scene drifted onto main exactly this way), no platform subtree gets double-counted, and `codecov.yml`'s `ignore:` stays mirrored to `coverage_config.json`'s `diff_cover_excludes`. Needs PyYAML locally; skips cleanly if absent (the CI `codecov-config-validation` job in `coverage.yml` is the authoritative gate). Use it before `git push` when you've made changes that might touch mapped paths but you don't want to wait for the pre-push hook OR the 20-minute CI roundtrip. Independent of the git hook (no install step needed). Named to align with Shipyard's planned `shipyard gates` subcommand (see `planning/2026-05-19-shipyard-preflight-upstream-proposal.md`); avoids collision with Shipyard's existing `preflight` namespace (SSH backend reachability probes).

**Bypass-priority cheat sheet** — reach for the surgical knob first; the nuclear one masks fast checks too:

| Symptom                                  | Surgical bypass                              | Nuclear bypass (avoid)        |
|------------------------------------------|----------------------------------------------|-------------------------------|
| `diff-cover` is the only failing gate    | `PULP_DISABLE_PREPUSH_DIFF_COVER=1 git push` | `PULP_SKIP_PREPUSH=1 git push` |
| skill-sync / version-bump / compat-sync / compat-aggregate | fix the gate, OR add the documented trailer (`Skill-Update: skip …`, `Version-Bump: skip …`, `Compat-Update: skip …`) on the tip commit; compat-aggregate has no trailer bypass, regenerate `compat.json` or `compat/` with `tools/scripts/compat_aggregate.py` | `PULP_SKIP_PREPUSH=1 git push` |
| Rebase race after force-push (gates already ran cleanly on the pre-rebase tip) | `PULP_SKIP_PREPUSH=1 git push --force-with-lease` (the legitimate use of the nuclear bypass — gates already passed on the same content) | — |
| All gates advisory, don't fail my push   | `PULP_DISABLE_PREPUSH_GATES=1 git push`      | `PULP_SKIP_PREPUSH=1 git push` |

The 2026-05-18 Pulp #2374 lesson: `PULP_SKIP_PREPUSH=1` on a NEW commit (not a rebase) skipped skill-sync, the missed SKILL.md update caught the PR in CI ~20 minutes later, and burned a CI roundtrip. Running `tools/scripts/gates.sh` before `git push` would have surfaced it in ~200ms.

**Gotcha:** changing anything under `.github/workflows/**`, `tools/shipyard.toml`, `.shipyard/**`, `.githooks/**`, `tools/install-shipyard.sh`, or `tools/scripts/install-githooks.sh` triggers the skill-sync gate for the `ci` skill — keep this file in sync when those paths move. The map lives at `tools/scripts/skill_path_map.json`.

**Compat-sync:** `tools/scripts/compat_sync_check.py` mirrors the skill-sync / version-bump shape for the populated `compat.json` matrix at the repo root. The bypass trailer is `Compat-Update: skip prefix=<section|*> reason="..."` (multiple lines allowed). Path map: `tools/scripts/compat_path_map.json`. A compat-json requirement is satisfied when `compat.json` changed in the same diff or the mapped section is already populated; empty sections are scaffolds and should fail until real matrix entries are added. `tools/scripts/compat_aggregate.py check` separately keeps the aggregate byte-identical to the split `compat/` parts. See `docs/guides/compat-sync.md` for the full design.

**CLI ↔ MCP parity (pulp #1997):** `tools/scripts/check_cli_mcp_parity.py` is the fourth invariant gate, added by pulp #1997. It enforces that every top-level CLI command added to `tools/cli/pulp_cli.cpp` either gets a matching `pulp_<command>` tool in `tools/mcp/pulp_mcp.cpp` OR an entry in `tools/scripts/cli_mcp_parity_baseline.json` with a one-line reason. Whole-tree check (no diff base needed) — runs as the `CLI ↔ MCP parity check` step in `version-skill-check.yml` in `--mode=report` (hard fail) and as a hint in `hooks/scripts/cli-plugin-sync.sh`. There is no commit-trailer bypass — the baseline file is itself the bypass mechanism. To intentionally defer MCP exposure for a new CLI command, add an entry to `cli_mcp_parity_baseline.json` in the same PR. The full guidance lives in the `cli-maintenance` skill ("Decide: does this need an MCP tool?").

**Hotspot-size guard (P0.1 refactor roadmap):** `tools/scripts/hotspot_size_guard.py` hard-fails when a tracked monolith exceeds the frozen LOC ceiling in `tools/scripts/hotspot_size_guard.json`. It also warns, without blocking, when a newly added `core/**`, `tools/**`, or `inspect/**` file is already over the configured warning threshold. Lower a ceiling in the same PR that shrinks a hotspot; only raise one when the PR explains why the growth is intentional and still reviewable.

After rebasing a branch that touches tracked hotspots, re-run the guard against
the rebased tree and set each edited ceiling to the exact current LOC. Adjacent
core/view work can legitimately change the post-rebase line count by a few
lines; do not leave a stale pre-rebase ceiling or add headroom.

`tools/cli/kit_commands.cpp` is frozen at its pre-split 3,927-line baseline.
When extracting kit-command modules, follow `tools/cli/KIT_COMMANDS_MODULE_MAP.md`
and lower that ceiling to the new exact LOC in the same PR that moves code out.
`tools/cli/cli_common.cpp` follows the same ratchet rule: when shared helper
logic moves into a focused translation unit such as `cli_delegate.cpp`, lower
the `cli_common.cpp` ceiling in `hotspot_size_guard.json` in that same PR so the
extraction cannot quietly regrow.

**Auto-release:** `.github/workflows/auto-release.yml` fires on push to `main`. It diffs the two version-bearing files (`CMakeLists.txt` project version, `.claude-plugin/plugin.json` version) against the previous push range and creates the corresponding `v<x.y.z>` or `plugin-v<x.y.z>` tag. The existing tag-triggered release workflows (`release-cli.yml`, `sign-and-release.yml`) then build and publish. `Release: skip reason="..."` on the merging commit suppresses the tag.

**fix/feat-needs-bump (issue #1009):** the version-skill-check workflow ALSO
runs `version_bump_check.py --require-bump-for-fix-feat` on `pull_request`
events. If the PR title or any live commit-derived signal matches
`^(fix|feat)(\([^)]*\))?!?:\s` (Conventional Commits user-facing prefix), the
diff range MUST contain either a commit subject `chore: bump versions` OR a
top-level `Version-Bump: skip reason="..."` trailer (with non-empty reason —
bare `skip` is rejected). Explicit reverts cancel their target signals;
reverting a revert restores them.
Checking commit subjects matters under GitHub's
`COMMIT_OR_PR_TITLE` squash policy: a one-commit PR can have a plain-language
PR title while its conventional commit subject becomes the landed squash
subject. Auto-release.yml has a matching backstop step (`Stranded fix/feat
detector`) that emits a `::warning::` annotation and opens a
`release-stuck`-labelled tracking issue when the merge slips through. The
canonical `main` ruleset requires `Enforce version & skill sync`; if a tracker
still fires, compare the PR title with the subject GitHub actually landed
before blaming branch protection. Bypass the check on a one-off basis with
`Version-Bump: skip reason="..."` on any commit in the range; this is
intentionally a different trailer from `Release: skip` so a "don't tag this
release" decision doesn't silently imply "this fix doesn't need a bump."
For PRs targeting `main`, the check's **Expected release tags** run summary
reports the SDK/plugin tags the PR queue should produce after merge, using the
PR head, fetched tags, and GitHub's one-commit-subject/multi-commit-PR-title
squash policy plus sticky `Release: skip` state. Treat it as a prediction:
`auto-release.yml` creates the
actual signed tags after merge. The
post-merge stranded detector classifies the whole pushed range, maps signals to
SDK/plugin surfaces, and checks each surface independently. A created tag or a
sticky `Release: skip` covers only its own surface and only signals at or before
that skipped bump; a later fix remains uncovered. Recovery preserves each
surface's own fix/feat bump level and any explicit numeric `Version-Bump`
override. Its recovery command starts from fetched `origin/main`, analyzes the
recorded historical range, and computes the next version from current `main`
via `--apply-version-base HEAD` plus
`--recover-stranded-release`; recorded `--recover-levels` preserve the
boundary-filtered level, while `--recover-surfaces` prevents an already covered
sibling surface from being bumped again.

**Exact bump-marker format:** for `fix:` / `feat:` PR titles or commit
subjects, the accepted bump-marker prefixes are exactly
`chore: bump versions` (canonical) and
`chore(versions): bump` (legacy). A manually authored subject such as
`chore: bump SDK to v0.78.4` does not satisfy the gate, even if the
version files and changelog are correctly edited. Let `shipyard pr` create
the bump commit when possible; if you need to repair it manually, use the
canonical subject `chore: bump versions`.

**Release-workflow VST3 pin:** `sign-and-release.yml` must clone the same Steinberg tag pinned everywhere else in the repo: `v3.8.0_build_66`. The shorthand `v3.8.0` does not exist upstream and will make tag-time macOS release jobs fail before configure/build even start.

**Release-workflow ctest must skip the `validation` label (#720):** the `Test` step in `sign-and-release.yml` MUST pass `-LE validation` to ctest. Without it, the suite includes the `auval-Pulp*` tests that copy a fresh `.component` to `~/Library/Audio/Plug-Ins/Components/` and immediately call `auval`. Hosted GitHub macOS runners' `AudioComponentRegistrar` does not pick up the new bundle reliably, so auval returns `Cannot get Component's Name strings / Error -50`, the Test step exits non-zero, and the entire sign / notarize / publish pipeline silently fails. This was the failure mode of the 30+ consecutive sign-and-release runs preceding v0.41.0. The validation gates are owned by `validate.yml` on PR; do not duplicate them into the release workflow. `tools/scripts/test_release_workflow_test_step.py` (wired into `workflow-lint.yml`) is the regression test that prevents reintroduction.

**Tag safety:** the auto-release workflow is idempotent-strict — if a tag already exists pointing at a different SHA, it fails loudly rather than overwriting. See `docs/guides/versioning.md` for the manual recovery recipe.

**Shared-source priming is retry-wrapped (#1375):** every `ensure_shared_git_source` call in `setup.sh` runs through `ensure_shared_git_source_with_retry` (3 attempts, 5s/10s/20s backoff, scrubs the partial cache target between attempts). Motivated by v0.74.0 + v0.74.1 release-cli runs both dying on `windows-arm64` mid-`Priming shared Yoga source cache` with exit 127 — a transient command-not-found on a Windows shell wrapper. The retry happens at the WRAPPER level, not inside `ensure_shared_git_source`, because that function uses a `set -e` subshell which a 127 tears down before any inner retry can engage. Override attempts via `PULP_PRIMING_RETRY_ATTEMPTS=N`.

**Release health is ONE reconciler that repairs, not watchdogs that report.**
`.github/workflows/release-reconcile.yml` (+ `tools/scripts/release_reconcile.py`)
runs every 30 min: for each recent SDK tag it compares desired state (a published
release) against actual, and RE-DISPATCHES `release-cli.yml` for anything stuck
(max 3 attempts), so a transient failure no longer needs a human backfill. Rules
that matter: a tag with a LIVE run is left alone at ANY age (slow is not broken); a
tag already superseded by a newer published release is not rebuilt (don't burn a
2h matrix on a release nobody installs); it NEVER cancels or deletes anything; and
it keeps exactly ONE incident issue, updated in place. It replaced four report-only
watchdogs (`release-guard`, `release-health`, `release-cli-watchdog`,
`release-draft-stuck-check`) that filed 413 issues in two weeks and fixed nothing —
their grace windows (15/30/45/60 min) were all shorter than the real pipeline, so
they alarmed on healthy releases. If you add a watchdog, it MUST know whether the
thing it is judging is still running, and MUST be able to find its own previous
issue, or it becomes a firehose.

**Linux release-cli requires libfontconfig1-dev (#1970):** chrome/m144 Skia exposes fontconfig symbols that the previous release kept private. Without `libfontconfig1-dev` in the apt-install step, the Linux link fails on `undefined reference to FcInitLoadConfigAndFonts` et al. Both `release-cli.yml` and `build.yml` Linux deps steps install it. When bumping `tools/deps/manifest.json` Skia pin, run `nm -D` over the new `libskia.a` and grep for unfamiliar prefixes (`Fc`, `Hb`, `FT_`, etc.) — any new symbol class means a matching system package needs to be added.

**Safe backfill of a stuck release-cli tag (#1962):** raw `gh workflow run release-cli.yml --ref vX.Y.Z` re-runs the BROKEN workflow file from the tag's source — useless when the breakage is in the workflow or the scripts it calls. Run the workflow from `main`, pass the old tag as `version`, and leave `source_ref` blank; checkout then uses the tag's source tree and the overlay step copies the current `main` release-pipeline helper files over the in-tree copies. `source_ref` is only for the unusual case where you intentionally want to build another ref under the requested version label. Leave `make_latest` false for old-tag backfills so `/releases/latest` does not move backward; set it true only when backfilling the current newest tag after the automatic tag run failed. To backfill a tag whose source predates a fetch-script fix on main:

```
gh workflow run release-cli.yml --ref main \
    -f version=v0.97.0
```

The workflow file comes from main (fixed), the source tree comes from the tag (correct content), and the overlay step picks up post-tag script fixes automatically. `release-reconcile.yml` will also do this for you automatically within 30 minutes, and closes its incident once the assets land. This was the fix for the four-day stall on v0.95.0..v0.97.0 caused by a skia-builder chrome/m144 zip layout drift (`Release/<arch>/libskia.a` instead of `Release/libskia.a`). The fetch script flattens the arch subdir; regression coverage lives in `tools/scripts/test_fetch_skia_for_release.py`, with workflow-condition coverage in `tools/scripts/test_release_workflow_test_step.py`.

**Linux ARM64 is covered nightly, not per-PR.** pulp ships a `linux-arm64`
artifact, but every Linux lane that gates is x64 — the per-PR check is
`Linux (x64)` by name and by resolver default (`--github-hosted-label
ubuntu-latest`). ARM64 was previously covered only *incidentally*, by
`PULP_LOCAL_LINUX_RUNS_ON_JSON` routing that lane to self-hosted ARM64 VMs
while still reporting as `Linux (x64)`. That lane was retired (2 slots, 4.6h
median waits, runs left queued for already-merged PRs), so the nightly gained
an explicit `linux-arm64` job on GitHub's free `ubuntu-24.04-arm` runners with
its own tracking issue and artifact name. If you re-point a Linux lane at
self-hosted runners, do NOT rely on it for ARM64 coverage under an x64 name —
name the lane for what it builds.

**Nightly cross-platform check (`.github/workflows/cross-platform-check.yml`):** Pulp's team develops and tests on macOS; Linux, Windows, and Android are advisory "tell us if it breaks" signal, and per-PR CI has been slimmed so those legs no longer run on every PR. This scheduled workflow is the backstop. It runs nightly (`cron: '17 7 * * *'` — odd minute, off-peak; also `workflow_dispatch` for manual bisect) and builds + tests **Linux** (`ubuntu-latest`), **Windows** (`windows-latest`), and **Android** (NDK build on `ubuntu-latest`) as three independent jobs with `fail-fast: false` so one platform breaking never masks the others — catching ALL cross-platform breakage in one pass is the point. GitHub-hosted runners only: it must never consume the scarce self-hosted macOS capacity. A final `tracking-issues` job (`needs:` all three, `if: always()`) maintains **one tracking issue PER platform**, keyed by the EXACT titles `Cross-platform Linux check is broken` / `Cross-platform Windows check is broken` / `Cross-platform Android check is broken`. It reuses `auto-release-watchdog.yml`'s find-or-create / edit / reopen / close gh-api pattern: a failed platform job opens (or reopens + edits) its issue; a passing one closes its open issue. De-dup is by `gh issue list --search "in:title <title>" --state all` matching the exact title — never a fresh issue per night. Created issues carry `bug`, `ci`, `cross-platform`, and `platform:linux`/`platform:windows`/`platform:android` labels, and the body includes the run URL, tip SHA, per-job results, artifact name, and the commit range since the last green run (derived from the Actions API) so a regression can be bisected within a night's batch. Distinct from `nightly-full-build.yml`, which does the full macOS `make all` to catch test targets PR CI never compiles; this workflow is the *non-macOS* coverage PR CI no longer provides. If you slim or restore a per-PR advisory platform leg, keep this nightly in sync — it is the only thing keeping cross-platform debt visible.

**Gotcha — `shell: cmd` step exit code is the LAST command's errorlevel.** Under `shell: cmd` GitHub Actions uses `cmd.exe` semantics: the step exit code is the errorlevel of the *last* program run, not the first failing one. The Windows ctest step writes to `test-windows.log` for artifact upload, then `type`s it into the run log — if `type` (always errorlevel 0) ran last, a real `ctest` failure was masked and the job went green, so the nightly tracking-issue logic never fired for genuine Windows breakage (codex P1 on pulp#2536). Fix: capture `set CTEST_RC=%ERRORLEVEL%` on the line *immediately* after `ctest` (before `type` overwrites `%ERRORLEVEL%`), then `exit /b %CTEST_RC%` as the final command. Same trap applies to any multi-command `shell: cmd` block where a non-final command is the one that can fail — capture-and-`exit /b`, or make the fallible command last. Note `build.yml`'s Windows test step is *not* affected: it runs `ctest` as the last command, so its errorlevel propagates naturally.

**`RELEASE_BOT_TOKEN` is required for the auto-release chain to fire.** Without it, auto-release silently degrades — tags get created via `GITHUB_TOKEN` but GitHub doesn't trigger workflows on `GITHUB_TOKEN`-pushed tags, so `release-cli.yml` and `sign-and-release.yml` never run and no GitHub Release appears. Run `pulp doctor` to check; if missing, follow the "One-time setup" section in `docs/guides/versioning.md`. `pulp pr` will also print a heads-up before pushing the PR if the secret isn't present.

**Tarball smoke matrix exercises `pulp-mcp` too.** The CLI
tarball now ships three user-facing binaries (`pulp`, `pulp-cpp`,
`pulp-mcp`). `release-cli.yml`'s `smoke-cli` job invokes
`pulp-mcp --version` (not `pulp-mcp help` — pulp-mcp is a JSON-RPC
stdin server and `--version` is the only short-circuit that exits
cleanly without consuming stdin). When adding a new user-facing
tarball binary, follow the same pattern: pick a flag that exits 0
without touching stdin, add it to the smoke matrix's `artCmd` /
`smoke_cmd` table on BOTH the Unix and Windows steps, and confirm the
binary is stripped on Unix. Smoke-gating a real protocol exchange
would make CI flakier than it needs to be.

## Coverage workflow (`#566` Phase 1)

`.github/workflows/coverage.yml`'s major jobs include:

- `resolve-runners` — shared-helper resolver (`tools/scripts/resolve_runs_on.py`) that picks per-OS runs-on labels in priority order: workflow_dispatch input → `PULP_COVERAGE_<OS>_RUNS_ON_JSON` repo variable → hard-coded default (`ubuntu-latest` / `macos-latest` / `windows-latest`). Coverage deliberately does not read `PULP_NAMESPACE_BUILD_*`; use dedicated ephemeral coverage labels such as `pulp-coverage-vm-macos`, never the warm macOS gate labels or shared build-pilot labels. Change runner for one OS by setting the repo variable — no workflow edit required.
- `coverage` — event-conditional matrix over {macos} on PRs and {linux, macos, windows} on push-to-main / workflow_dispatch. Every leg builds with Clang source-based coverage, runs the native test suite, uploads HTML + summary + Cobertura artifacts, and sends one explicit, semantically verified per-OS report set through `upload-codecov-report` with flag `os-linux`, `os-macos`, or `os-windows`. Python tooling is a separate `python-tools` upload from the Linux leg; the macOS native set includes Swift LCOV when that lane is in scope. A native suite failure cannot upload a partial native report, and a Python-suite failure cannot upload a partial Python report, but either verified surface can upload independently of the other. Successful Codecov transport emits an exact SHA-and-attempt receipt artifact; the main watchdog requires current Linux, macOS, and Python-tools receipts. Subsystem / platform / surface slicing comes from `codecov.yml`'s `component_management` path globs. Has `fail-fast: false` on the matrix — a flake on any one OS never cancels the others, while the in-repo diff gate remains authoritative.
- `android-kotlin-coverage` — Gradle/JaCoCo coverage for `android/app/src/main/kotlin/**`, uploaded to Codecov from the canonical Coverage workflow on push-to-main / workflow_dispatch so main snapshots keep Android JVM coverage fresh without spending a PR runner.
- `pulp-react-coverage` — Vitest/Cobertura coverage for `packages/pulp-react/**` on push-to-main and workflow_dispatch. PR upload remains in `pulp-react-build.yml`; main upload is centralized here so side coverage cannot advance Codecov when native Coverage for the same SHA was cancelled before upload.
- `coverage-diff-gate` — downloads all three OS Cobertura artifacts (`coverage-cobertura-${sha}` for Linux, `coverage-cobertura-macos-${sha}`, `coverage-cobertura-windows-${sha}`), merges them with `tools/scripts/merge_cobertura.py` (taking `max(hits)` per `(filename, line)`), then runs `diff-cover --fail-under=75` against `origin/<base>` on the merged XML. Hard-fails the PR when the global diff-coverage floor is missed. The job still renders and upserts the diff-coverage PR comment via `tools/scripts/coverage_diff_comment.py` even on failure, and it also runs the per-tier gate (`tools/scripts/coverage_tier_check.py`) in advisory mode against the same merged XML.

Gotchas:

- **Fork PRs**: the comment-upsert step has an `if:` guard skipping forks because `GITHUB_TOKEN` is read-only on fork heads; otherwise the comment step would hard-fail with 403 after the gate result is already known.
- **The comment renderer is unit-tested.** When touching `tools/scripts/coverage_diff_comment.py`, run `python3 tools/scripts/test_coverage_diff_comment.py` locally — the workflow also runs it as a pre-flight fixture check so a regression fails fast.
- **The Python tools lane is still Linux-only, but no longer scripts-only.** Today it measures `tools/scripts/**`, `tools/deps/**`, and `tools/local-ci/**`, uses coverage.py's subprocess patching so spawned scripts count, and uploads only on Linux. Python elsewhere (for example top-level `tools/*.py`, `tools/packages/**`, repo-root scripts) is still out of scope until a follow-up expands the surface again.
- **Install Python coverage tooling in a venv, not with `pip install --user`.** Namespace/self-hosted Linux runners can enforce PEP 668 (`externally-managed-environment`), which breaks the coverage lane before `tools/scripts/run_python_coverage.py` even starts. The workflow now creates `build-coverage/python-venv` and runs the Python coverage script through that interpreter; keep future edits on that pattern.
- **The Apple Swift lane is source-only on the macOS leg.** `tools/scripts/run_swift_coverage.py` stages SwiftPM's Codecov JSON for `apple/Sources/PulpSwift/**`; `apple/Tests/**` and generated `apple/.build/**` paths are ignored in `codecov.yml` so the Apple component reflects package sources rather than the test harness. iOS-only files that compile out of the macOS SwiftPM build remain out of scope on this first pass.
- **Adding a new core subsystem** means adding or adjusting the `component_management` path entry in `codecov.yml` and documenting it in `docs/guides/coverage.md`. The upload step itself should still stay at one per-OS flag per upload — Codecov rejected the older "20 flags per upload" shape.
- **Per-OS coverage (Phase 1 PR 4)**: each matrix leg tags its Codecov upload with an OS flag so `host AND os-windows` answers "what fraction of `core/host` is exercised when tests run on Windows?" — a different question from `host AND windows` (which is "what fraction of `core/host/**/windows/` shim files are covered at all"). Cross-OS unions of the same file happen at the Codecov flag layer, NOT via `llvm-profdata merge` (not architecture-portable — see planning decision doc §7).
- **Windows coverage uses Clang, not MSVC.** `tools/cmake/PulpInstrumentation.cmake` rejects MSVC because `/fsanitize-coverage` and llvm-cov emit incompatible profile shapes. The Windows matrix leg adds `C:\Program Files\LLVM\bin` to PATH and builds with clang++; the `windows-msvc-release-gate` job in `build.yml` keeps the MSVC release-path green separately.
- **diff-cover consumes a merged XML, not the per-OS ones.** It's a single-XML tool — running it against three XMLs would produce three PR comments for the same metric with slightly different numbers, more noise than signal. The merge happens once in the job (`merge_cobertura.py`, max-hits-per-line union) so the gate sees a cross-platform view while diff-cover still emits one comment. Earlier the gate read only the Linux artifact and silently skipped Apple-only / Windows-only files (pulp#635). Local `scripts/run_coverage.sh` still produces a single per-host XML; a local diff-cover invocation against that has the original silent-skip and is best treated as a sanity check, not the authoritative gate.
- **Global vs per-tier enforcement**: `diff-cover --fail-under=75` is already required. The per-tier gate is still `continue-on-error: true` while the tier definitions soak; don't silently flip that to required without updating `docs/guides/coverage.md` and the issue trail.
- **Don't `|| ...` the `merge_cobertura` step in `coverage-diff-gate`.** The script uses a DEDICATED exit code (2 = `EXIT_ALL_INPUTS_MISSING`) for the intentionally-tolerated "every input XML missing or empty" case; the diff-cover step then renders the no-XML fallback. Any other non-zero exit (1 = real error: parse failure, script bug, IO error) MUST fail the gate — otherwise a corrupted artifact silently bypasses the required 75% diff-coverage check. Codex P1 reviews on both PR #654 (original `|| echo` shape masked everything) and PR #660 (collapsing rc==1 into the tolerated case let `xml.etree.ParseError` slip through) drove the current shape. The workflow branches on the exact code with `if rc -eq 0` / `elif rc -eq 2` / `else fail`. The script's `EXIT_ALL_INPUTS_MISSING` constant + the workflow's literal `2` are paired — change them in lockstep, and add fixture-tests in `test_merge_cobertura.py` if you alter the contract.
- **Local mirror of the diff-cover gate.** `tools/scripts/local_diff_cover.sh` runs the same `diff-cover --fail-under=$THRESHOLD` flow CI runs, so coverage-only failures don't cost a 20-min CI roundtrip. The threshold + filters are read from `tools/scripts/coverage_config.json` — both the workflow's diff-cover step and the local script consume that file, so editing the JSON in one place keeps CI + local + the pre-push hook in lockstep. Bypass with `PULP_SKIP_DIFF_COVER=1` for workflow-only or doc-only PRs. The Claude Code `/coverage-diff` slash command and `pulp coverage diff` CLI subcommand are thin wrappers over the same script. The pre-push hook runs this check enforcing-by-default; `PULP_DISABLE_PREPUSH_DIFF_COVER=1` demotes it to advisory for an intentional one-push escape hatch. For focused PRs, pass build targets and set `PULP_DIFF_COVER_CTEST_REGEX` to run only the relevant CTest subset while still enforcing the shared 75% floor. Test coverage in `tools/scripts/test_local_diff_cover.py` includes anti-drift gates that fail if a future edit hardcodes `--fail-under=NN` back into `coverage.yml` or drops the targeted CTest selector.
- **Non-native exact diffs skip the local native build automatically.** Before
  the hook prints its build banner or `local_diff_cover.sh` checks disk space,
  dependencies, or locks `build-cov`, the script proves the merge-base diff is
  limited to added/modified files with explicitly known non-native suffixes.
  Native sources and headers, generated native templates, unknown or
  extensionless paths, renames, copies, deletions, binary changes, and any Git
  inspection error retain the full coverage gate. Keep this classifier
  fail-closed: it removes work only when LLVM coverage cannot attribute a line,
  and it does not replace the separate Python coverage lane.
- Local diff-cover configures with `PULP_ENABLE_GPU=OFF` and
  `PULP_BUILD_EXAMPLES=OFF`; it needs the test targets and coverage
  instrumentation, not Skia-dependent example apps.
- **`diff_cover_excludes` pattern + flag-shape contract** (PR #1005, learned the hard way). diff-cover's `--exclude` is `nargs='+'` with default action — repeated `--exclude=foo --exclude=bar` keeps only the LAST entry. AND its matching is fnmatch against (a) the file's basename and (b) its absolute path; a literal relative path like `tools/cli/cmd_loop.cpp` matches NEITHER and is a silent no-op. So entries in `coverage_config.json` MUST be a basename (`cmd_loop.cpp`) or a glob (`**/cmd_loop.cpp`), and both `local_diff_cover.sh` and `coverage.yml` MUST splat them under a SINGLE `--exclude val1 val2 ...` flag (NOT a per-entry `--exclude=PATH` loop). The previous shape was silent-broken since #919; a new exclude (scanner_clap.cpp) on PR #1005 surfaced the latent bug because it was a 2-entry config that suddenly mattered. Don't introduce a 3-entry config without re-checking that the splatted form still works.
- **llvm-cov mis-attribution: inline header virtuals + `break;` inside nested `if`** (PR #2120 case study). llvm-cov-export's Cobertura sometimes reports lines as uncovered when a passing test demonstrably executes them. Two known shapes:
    1. **Inline virtual function bodies in headers** (e.g. `virtual bool accepts_text_input() const { return false; }`) get 0% attribution when the test calls them through a base pointer. Move the body to the matching `.cpp` file (keep the declaration in the header) and coverage attributes correctly.
    2. **`break;` inside a nested `if` inside a loop.** `for(...) { if (match) { if (suppress) break; handle(); } }` may report the `break` line as uncovered even when the suppression branch is observably hit. Flatten to `for(...) { if (!match) continue; if (suppress) break; handle(); }` — same semantics, instrumented cleanly.
  Before refactoring code to satisfy diff-cover, **open `build-coverage/coverage/index.html`** and confirm whether the lines are genuinely unexercised or whether llvm-cov is misattributing. Adding tests that don't actually reach the lines won't help if the attribution itself is broken. Do NOT expand `diff_cover_excludes` to paper over instrumentation quirks — that mechanism is for thin dispatchers exercised end-to-end via shell-out tests, not for "the tooling is confused." Full write-up: `docs/guides/coverage.md` § "llvm-cov mis-attribution gotchas".
- **`merge_cobertura.py` normalises Windows backslash paths and applies `COVERAGE_IGNORE_REGEX` itself.** Two sneaky bugs found together on PR #660 by walking the actual merged XML: (1) the Windows cobertura emits filenames with backslash separators (`core\\format\\src\\clap_adapter.cpp`), Linux/macOS use forward slashes — without normalisation the merge stores them as TWO files and diff-cover matches the backslash variant against the git diff (which uses forward slashes), finding 0 hits and silently reporting 0% on cross-platform code that was actually exercised on Linux. (2) The Windows leg was leaking ~250 `test\*` entries into the cobertura because run_coverage.sh's `COVERAGE_IGNORE_REGEX` matches `/test/` only — backslash paths slipped past. The merge now normalises slashes AND mirrors the same exclude regex (`tools/scripts/merge_cobertura.py::_IGNORE_RE`) so the gate's view is consistent regardless of which OS produced an artifact. Keep the regex in lockstep with `scripts/run_coverage.sh::COVERAGE_IGNORE_REGEX`.
- **Install PyYAML before any step that imports it.** `tools/scripts/test_coverage_tier_check.py` calls `ctc.load_targets()` which imports `yaml`, so the `Install PyYAML` step in `coverage.yml` must run BEFORE both the fixture-tests step and the per-tier gate step. Issue #900 caught the original ordering where the install ran after the test, so runners without preinstalled PyYAML hard-failed the required coverage job. If you add another script under `tools/scripts/` that imports `yaml` and gets wired into a workflow, make sure the PyYAML install step precedes every step that runs it.
- **Every first-party source must classify into exactly one tier (#1056).** `ci/coverage-targets.yaml` tier globs are silent no-ops if a new source path falls outside every tier — it inherits the looser global 75% floor instead of its intended tier. The `TierCoverageCompleteness` cases in `tools/scripts/test_coverage_tier_check.py` lock this in (every tier matches at least one file; every first-party source under `core/`, `tools/`, `apple/`, `android/`, `inspect/` lands in exactly one tier). Non-instrumented surfaces (`apple/**.swift`, `android/**.kt`, `apple/Package.swift`) classify under `infrastructure` for audit-completeness; the `is_instrumented_source` filter in `coverage_tier_check.py` keeps them out of the score so they don't bias the per-tier number. New native user-facing render surfaces, such as `core/scene/**`, belong in the `user-facing` tier alongside `core/render/**` rather than falling through to the global diff-cover fallback.
- **Realtime graph runtime code belongs in `audio-critical`.** `core/graph/**`
  contains graph planning/queue primitives consumed by DSP/host execution; keep
  it on the same 80% tier as `core/audio/**`, `core/host/**`, and
  `core/midi/**`, not the looser infrastructure tier.
- **Don't `cancel-in-progress: true` the coverage workflow (#1884).** `coverage.yml` deliberately sets `concurrency.cancel-in-progress: false`. Codecov's `after_n_builds: 4` (pulp#1883) waits for all per-OS uploads before posting; if a force-push cancels an in-flight run mid-upload, some legs upload and others don't, Codecov gets stuck waiting for the missing leg, and the PR merges with no coverage signal. A 2026-05-12 audit found this pattern on 21/30 most-recent merged PRs (~70% of merges shipping without the `Diff coverage required` check). The fix costs some compute on stale commits but guarantees every push ends with a real check conclusion. If you ever need to flip cancellation back on for this workflow, you MUST also change the Codecov side (drop `after_n_builds` or accept partial reports) or you re-open the same silent-skip.

## IWYU advisory gate (`#594` Phase 2)

`.github/workflows/iwyu.yml` runs include-what-you-use on the Linux Clang lane to catch transitive-include bugs before they reach the cross-platform matrix. Three incidents on 2026-04-21 (#540 `<memory>`, Slice 4 `<atomic>`, #593 `<algorithm>`) triggered this gate.

- **Advisory until 2026-05-05** — `continue-on-error: true`. PR annotations appear inline on the diff; merges are not blocked.
- **Linux Clang only** — macOS libc++ hides the bug class (false negatives); MSVC is not Clang. Don't attempt to extend it to those runners.
- **Scope** — PR events analyze only files changed vs `origin/<base>`; push-to-main events run a full repo scan and upload the raw IWYU output as an artifact so we can track FP trends.
- **The parser is unit-tested.** When touching `tools/scripts/iwyu_annotate.py`, run `python3 tools/scripts/test_iwyu_annotate.py` locally — the workflow runs it as a pre-flight fixture check so regressions fail fast without burning the build.
- **Mappings** — `.iwyu-mappings.imp` at the repo root maps CHOC amalgamated headers and libstdc++ detail paths to the canonical public include. Prefer fixing the code (adding the missing include) over adding a new mapping.
- **Flip to blocking** (Phase 3 of #594) requires FP rate < 5% across a one-week window. On the flip PR, edit `continue-on-error` to `false`, update `docs/guides/iwyu.md`'s "Advisory until" line, and reference the flip in the PR body. Do not close #594 until the blocking gate has held for a week.

See [docs/guides/iwyu.md](../../../docs/guides/iwyu.md) for the full contributor-facing write-up.

## PEP 668 + Namespace runners

Namespace's runner image is PEP-668-strict: `pip install --user <pkg>` fails with `error: externally-managed-environment` unless you also pass `--break-system-packages`. The github-hosted ubuntu-latest image tolerates `--user` without the flag, so this regression only surfaces after a workflow's matrix routes its Linux leg through Namespace via a `PULP_*_RUNS_ON_JSON` repo variable.

When you add a new `pip install --user` step to a workflow that may run on Namespace, ALWAYS include `--break-system-packages`. Same applies to any virtualenv-less Python helper installed inline at workflow time. If the workflow uses a hard-coded `runs-on: ubuntu-24.04` (e.g. `coverage-diff-gate`), the flag isn't required because GH-hosted runners aren't PEP-668-strict — but adding the flag is harmless and future-proofs against a later Namespace migration.

Symptom (on Namespace) when you forget:

```
error: externally-managed-environment
× This environment is externally managed
╰─> To install Python packages system-wide, try apt install python3-xyz, ...
```

Followed by cascade-skipped downstream steps (default `if: success()`) and a "coverage.python.xml is missing" hard-fail in the validation step. Coverage Linux ran into this when it migrated to Namespace via `PULP_COVERAGE_LINUX_RUNS_ON_JSON` (PR #676 → #677).

## Homebrew on Namespace macOS runners (PR #2399)

Namespace macOS runners (`nscloud-macos-tahoe-arm64-*`) come up with
Homebrew configured to disable automatic updates AND with a stale
package index. Any first-call `brew install <pkg>` on a fresh runner
exits non-zero with:

```
You have disabled automatic updates and have not updated today.
Do not report this issue until you've run `brew update` and tried
again.
```

Fix: always run `brew update --quiet` before the first `brew install`
on macOS legs. The step is gated `if: runner.os == 'macOS'` so it
no-ops on Linux/Windows. Local self-hosted Macs already keep brew
warm between runs, so the update is a quick no-op there too —
unconditional execution is simpler than per-runner-environment
detection. See `.github/workflows/build.yml` for the canonical
placement (immediately before `Install ccache (macOS)`).

Cache: the `Namespace cache (brew + ccache + Pulp FetchContent)`
step uses `namespacelabs/nscloud-cache-action@v1` with `cache: brew`
plus ccache and FetchContent paths. It runs only on Namespace /
nscloud labels (`contains(matrix.runs_on_json, 'namespace') ||
contains(matrix.runs_on_json, 'nscloud')`); self-hosted Macs keep
their caches on local disk and github-hosted runners use
`actions/cache@v4` via the existing `Restore ccache (GitHub-hosted)`
step (#420). The brew cache only restores the bottle download cache —
it does NOT restore the brew config that would tell the runner
"updates are recent," so `brew update --quiet` is still required
even when the cache hits.

Incident: 2026-05-19 — PRs #2367, #2374, #2378, #2388 all wedged on
the macOS `Install ccache (macOS)` step within minutes of each other
because Namespace's runner image had drifted past the freshness
window the brew preamble enforces. Adding `brew update --quiet`
once unblocks the whole queue.

## SignalGraph Phase 0 learnings (PR #153)

Gotchas surfaced while landing the four-phase SignalGraph follow-up:

- **AudioUnitSDK 1.4 uses `std::expected` (C++23).** Targets that `#include`
  AUSDK headers need `set_target_properties(<target> PROPERTIES CXX_STANDARD 23)`
  at the per-target level. `target_compile_features(<target> PUBLIC cxx_std_23)`
  alone is **not** enough when `CMAKE_CXX_STANDARD=20` is set at the repo
  root — CMake 3.24's policy makes CXX_STANDARD authoritative over feature
  requirements. Apply to both the `ausdk` target and every consumer
  (`pulp-format`, per-plugin `${target}_AU`). Symptom: GH-hosted mac fails
  with "no template named 'unexpected'"; local Xcode mac builds fine
  because Apple's libc++ is ahead. Linux/Windows are unaffected because
  they don't touch AUSDK.

- **`std::atomic<std::shared_ptr<T>>` needs C++20 libc++ which our
  toolchain doesn't ship.** The workaround is the deprecated
  `std::atomic_load_explicit(&shared_ptr_var, order)` /
  `std::atomic_store_explicit(&shared_ptr_var, value, order)`
  free-function overloads. These still work everywhere we build and
  preserve acquire/release semantics. Revisit when libc++ catches up.

- **Catch2 `REQUIRE` inside a `std::thread` body terminates the process.**
  The REQUIRE throws and std::thread's dtor calls std::terminate when
  unwinding across the thread boundary. For concurrency tests, use an
  `std::atomic<int>` failure counter from the worker and assert on the
  main thread after join.

- **GH-hosted macOS vs local mac for upstream SDK issues.** When an
  upstream SDK (AUSDK, VST3, …) breaks only on `macos-latest` while the
  exact same code builds on a developer's Xcode, that's an Apple clang
  version mismatch. Options: (a) pin the SDK to a known-good commit,
  (b) set CXX_STANDARD per target, (c) `gh pr merge --admin` if
  Linux+Windows Namespace are green and local mac validated. Don't
  chase GH-hosted mac issues on the PR branch — fix upstream or admin-merge.

- **FetchContent threejs clones hang on some macs.** The threejs git
  clone inside CMake's FetchContent step has hung indefinitely several
  times during fresh configures. Mitigations: reuse an existing
  configured build dir; `rm -rf build/_deps/threejs-*` then build only
  the targets that don't need it (e.g., `pulp-host`, `pulp-test-host`);
  or set `-DPULP_ENABLE_GPU=OFF` to bypass the threejs fetch entirely.

- **Fresh worktree cmake configure is expensive (~15+ min)** because every
  FetchContent dep re-populates. Reuse strategy: `git checkout -B
  feature/<new-phase> origin/main` on an already-configured worktree to
  inherit the populated `_deps/`. Saves ~70% on per-phase bootstrap.

- **Skill-sync + version-bump CI gates run on every push.** After each
  push that touches `tools/cli/`, `core/host/`, `.agents/skills/`,
  you'll likely need to (a) append a new bullet to `hosting/SKILL.md`
  or `cli-maintenance/SKILL.md`, and (b) run
  `python3 tools/scripts/version_bump_check.py --mode=apply` to update
  `CMakeLists.txt` + `CHANGELOG.md`. The gate reports "SDK X.Y.Z ✓
  bumped" when satisfied.
- **Android/Kotlin coverage is a separate Gradle lane, not the native coverage matrix.**
  The dedicated `android-kotlin-coverage` job provisions Java + the
  Android SDK/NDK, runs `:app:testDebugUnitTest` plus
  `:app:jacocoDebugUnitTestReport`, uploads the JaCoCo artifacts, and
  sends the XML to Codecov on main/manual coverage runs. Keep it separate
  from the Clang-based `coverage.yml` matrix, and keep it skipped on PRs:
  Android coverage is a Gradle/SDK lane, not a native profraw lane.

- **Release-time workflows must declare `permissions: contents: write`.**
  Both `release-cli.yml` and `sign-and-release.yml` write to the
  GitHub Releases API (create or patch the release, upload artifacts);
  `release-cli.yml` also fetches generated-notes content. Without an explicit job-level
  permissions block they inherit a read-only `GITHUB_TOKEN` on
  `push: tags` events and the `Create GitHub Release` step fails with
  `Resource not accessible by integration` — silent release failure
  that lost ~30 sign-and-release runs across v0.20.x → v0.41.0. See
  `ship` SKILL.md § "`sign-and-release.yml` must declare …" for the
  full gotcha; pulp #720 + #724 for the history. When adding a new
  release-time workflow, add the same block.

### Shipyard-drift detection — pre-push hook logs push origin (pulp #1406)

`.githooks/pre-push` writes every push to `.git/.shipyard-drift-log`
(tab-separated: timestamp, branch, sha, origin) so we can audit when
PRs went up via `shipyard pr` (the canonical full-validation path)
versus a direct `git push` (which silently bypasses skill-sync,
version-bump, diff-coverage, and SSH-host validation, turning CI
into the discovery channel).

**Origin signals** (any one marks the push as supervised):
- `SHIPYARD_PR_RUNNING=1` — set by shipyard's wrapper when it
  invokes git push internally. Upstream feature request open at
  the shipyard CLI repo to make this canonical.
- `PULP_VIA_SHIPYARD=1` — user-set fallback marker for supervised
  direct pushes (e.g. inside a `shipyard ship` retry, or when
  using `git push` deliberately under shipyard tooling that
  doesn't expose the env var yet).

**Behavior**:
- Push proceeds either way (escape hatches need to keep working).
- When neither var is set, hook prints a loud warning with the
  recovery checklist (rate-limit / shipyard-bug / SSH-down).
- The drift log is append-only and gitignored.

**When to suppress the warning** (acceptable temporary fallback):
1. GraphQL rate limit exhausted — verify with
   `gh api rate_limit --jq .resources.graphql.remaining` and
   note the reset time.
2. Shipyard tool itself fails — file an issue at the shipyard CLI
   repo, link it in the PR description.
3. SSH host unreachable — prefer `shipyard pr --skip-target NAME`
   (deliberate skip) over direct push.

In all three cases, set `PULP_VIA_SHIPYARD=1` on the direct-push
command to record the push as supervised AND suppress the warning.

After the obstacle clears, resume `shipyard pr` on the next PR.

### GitHub connectivity triage — prove the failing layer before declaring an outage

A timeout from one client is not evidence that "GitHub is down." GitHub exposes
independent API, web, Git, authentication, and Actions paths; one can fail while
the others remain healthy. Before parking publication or telling another agent
to wait, run this bounded escalation in the same time window. Define this
portable whole-process timeout first; unlike an SSH connect timeout, it also
bounds stalls after a connection succeeds:

```bash
bounded() {
  python3 - "$@" <<'PY'
import subprocess
import sys
import os
import signal

seconds = float(sys.argv[1])
process = subprocess.Popen(sys.argv[2:], start_new_session=True)
try:
    raise SystemExit(process.wait(timeout=seconds))
except subprocess.TimeoutExpired:
    print(f"timed out after {seconds:g}s: {' '.join(sys.argv[2:])}", file=sys.stderr)
    os.killpg(process.pid, signal.SIGTERM)
    try:
        process.wait(timeout=2)
    except subprocess.TimeoutExpired:
        os.killpg(process.pid, signal.SIGKILL)
        process.wait()
    raise SystemExit(124)
PY
}
```

1. Check GitHub's authoritative status and unresolved incidents:

   ```bash
   curl --max-time 15 -fsS https://www.githubstatus.com/api/v2/status.json
   curl --max-time 15 -fsS https://www.githubstatus.com/api/v2/incidents/unresolved.json
   ```

   A healthy status page does not prove every route works, but it forbids calling
   a client-local timeout a global outage.
2. Prefer the Shipyard App identity for GitHub operations:

   ```bash
   bounded 20 ghapp api repos/Generous-Corp/pulp --jq .default_branch
   bounded 20 ghapp api repos/Generous-Corp/pulp/pulls/PR --jq '{state,head:.head.sha,merged}'
   ```

   Try REST through `ghapp api` when GraphQL-backed `ghapp pr view` or `gh pr
   view` times out. A personal `gh` token failure does not invalidate the App
   token.

   On a host where Go-based `ghapp` or Shipyard hangs during DNS/TLS while
   `curl` succeeds, compare the bounded default call with Go's pure resolver:

   ```bash
   bounded 20 env GODEBUG=netdns=go ghapp api repos/Generous-Corp/pulp --jq .default_branch
   ```

   If only the pure-Go resolver succeeds, report a host cgo resolver failure,
   not a GitHub outage. It is safe to wrap that host's `ghapp` and Shipyard
   launchers with `GODEBUG="${GODEBUG:-netdns=go}"`; preserve an explicit caller
   override and keep the bounded probes. This exact failure has occurred on the
   M3 coordinator.
3. Probe the unauthenticated public REST or web path to separate GitHub reachability
   from local authentication:

   ```bash
   curl --max-time 20 -fsS https://api.github.com/repos/Generous-Corp/pulp/pulls/PR
   ```

   Record only fields actually returned; a partial response or HTML page is not
   proof of check state or merge completion.
4. Probe Git independently from API traffic. Try normal SSH first, then HTTPS,
   and, when port 22 is the failing layer, GitHub's supported SSH-over-443 route:

   ```bash
   bounded 20 git ls-remote git@github.com:Generous-Corp/pulp.git refs/heads/main
   bounded 20 git ls-remote https://github.com/Generous-Corp/pulp.git refs/heads/main
   GIT_SSH_COMMAND='ssh -p 443 -o HostName=ssh.github.com -o ConnectTimeout=10' \
     bounded 20 git ls-remote git@github.com:Generous-Corp/pulp.git refs/heads/main
   ```

   Verify the `ssh.github.com:443` host key through normal OpenSSH trust handling;
   never disable host-key verification. Fetch/`ls-remote` success with a hanging
   push usually points to a local pre-push gate or write/auth path, not GitHub
   read availability.

   A cancelled pre-push must not leave its diff-coverage build under PID 1.
   The gate supervisor isolates the build group, but remains in the hook's
   process group; external process managers may escalate TERM to KILL in about
   one second. Its signal path therefore forwards immediately and bounds grace
   below that escalation window before killing the isolated group. Preserve
   `test_signal_cleanup_beats_external_hook_group_escalation`: a longer grace
   can kill the supervisor first and strand Bash/CMake/compiler descendants on
   `/Volumes/Workshop`, causing load and removable-volume prompt churn after
   the push was supposedly cancelled.

   If the configured SSH agent itself stalls, retry the bounded 443 probe with
   the host's explicit approved key and `IdentitiesOnly=yes`, for example:

   ```bash
   GIT_SSH_COMMAND='ssh -p 443 -o HostName=ssh.github.com -o ConnectTimeout=10 -o IdentitiesOnly=yes -i /Users/danielraffel/.ssh/id_rsa' \
     bounded 20 git ls-remote git@github.com:Generous-Corp/pulp.git refs/heads/main
   ```

   This separates an agent integration failure from GitHub SSH availability;
   never copy a host-specific key path into another machine's configuration.
5. Use Shipyard's durable state before inventing a manual publication path:

   ```bash
   shipyard ship-state list
   shipyard ship
   shipyard rescue PR --rerun-failed
   ```

   Plain `shipyard ship` automatically resumes its durable state. Use
   `--resume-from <stage>` only when intentionally selecting a known stage; the
   pinned Shipyard CLI has no `--resume` flag.

   `shipyard pr` remains the normal create/validate/merge path, and `ghapp pr
   merge PR --auto` is the server-side merge-on-green backstop. Pulp's merge
   queue rejects strategy flags such as `--merge` and `--squash`. Do not
   open a duplicate PR merely because a local watcher or CLI process died.
6. Inspect local processes when commands "hang." Stale `git fetch`, `git push`,
   `ssh git-upload-pack`, or `git-receive-pack` children from earlier retries can
   consume sockets indefinitely. Terminate only the exact processes this session
   started; never sweep unrelated agents' Git or build processes.

Use explicit per-probe timeouts and back off between retries. Report the boundary
precisely: API read unavailable, Git read unavailable, branch not pushed, PR not
created, checks unknown, or merge unverified. Declare a global GitHub outage only
when the status service reports one; otherwise say which tested routes failed.
Do not mark a whole program blocked while another independent publication or
validation route is still available.

### GraphQL exhaustion fallback

GitHub's GraphQL quota is independent from the REST `core` quota and is easier
to burn during broad PR sweeps because `gh pr list/view/merge --json ...`
queries large nested PR/check payloads. When GraphQL is exhausted, do not idle
and do not keep retrying GraphQL-backed commands in a loop.

Check quota explicitly:

```bash
gh api rate_limit --jq '.resources | {core, graphql}'
```

Fallback rules while `graphql.remaining == 0`:

- Use REST for status polling:
  `gh api repos/OWNER/REPO/pulls/PR`,
  `gh api repos/OWNER/REPO/commits/SHA/check-runs?per_page=100`, and
  `gh api repos/OWNER/REPO/actions/jobs/JOB_ID/logs`.
- Treat `gh pr view --json`, `gh pr list --json`, and `gh pr merge` as
  unavailable unless proven otherwise; those paths commonly fail before REST
  quota is close to exhausted.
- If a PR is verified green via REST (required checks green, no actionable
  failures in the checks being honored for that lane), merge via REST:

```bash
head_sha=$(gh api repos/OWNER/REPO/pulls/PR --jq '.head.sha')
gh api repos/OWNER/REPO/pulls/PR/merge \
  -X PUT \
  -f sha="$head_sha" \
  -f merge_method=squash \
  -f commit_title='subject (#PR)'
```

If REST merge returns `405 Base branch was modified`, refresh the PR's REST
state and check runs, recompute `head_sha`, and retry once after the base
settles only if the refreshed head SHA and green status are still the values
you intend to merge. If checks have re-queued or the head SHA changed,
re-evaluate before merging. This fallback is for GitHub API transport
exhaustion only; it does not relax the requirement to fix real CI, coverage,
sanitizer, or review failures.

## Self-hosted runner ops

Pulp's required `macos` branch-protection check on `main` routes
through the local self-hosted fast-gate class declared in
`tools/scripts/runner_topology.json` (via the
`PULP_LOCAL_MACOS_RUNS_ON_JSON` repo variable, consumed by
`.github/workflows/build.yml` → `resolve-provider`). When that runner
wedges, every PR's `macos` check sits queued indefinitely and all PRs
land in `mergeable_state=blocked`.

Shipyard v0.55.0+ ships a complete operational toolkit for this
class of problem — **prevent → recover → keep current**. Pulp pins
Shipyard ≥ 0.56.2 in `tools/shipyard.toml` so recovery, update, and
`shipyard wait pr` all have REST fallback paths when GraphQL is rate-limited
or unavailable. The authoritative reference lives in Shipyard's
`skills/ci/SKILL.md`; this section is the Pulp-side quick reference +
Pulp-specific gotchas.

### Recover — `shipyard rescue <PR>` (v0.53.0+)

```bash
shipyard rescue <PR>                # cancel queued runs + redispatch
                                    # to github-hosted (default)
shipyard rescue <PR> --rerun-failed # also re-arm completed/cancelled
                                    # runs (watchdog-cancellation case)
shipyard rescue <PR> --dry-run      # preview without acting
shipyard rescue --all-stuck         # repo-wide sweep
shipyard rescue <PR> --to github-hosted   # explicit provider
```

One command replaces the legacy 5-step recipe (`runner-watchdog --fix`
→ `gh run rerun --failed` → `shipyard cloud handoff run --apply`
manual sweep). Safe under load — does not mark required checks as
`failure`. Cross-link: Shipyard `skills/ci/SKILL.md#rescuing-wedged-
runners-shipyard-rescue`.

After a rescue, prefer `shipyard wait pr <PR> --state green` over manual
polling. Shipyard v0.56.2 adds a REST fallback for this wait path; use
`--no-fallback` only when a caller must fail instead of polling.

### Prevent — `shipyard runner watch --kill-hung-workers` (v0.54.0+)

```bash
# One-time setup on each persistent self-hosted runner host:
shipyard runner watch --kill-hung-workers
# Pair with launchd / systemd for unattended ops.
```

Host-side daemon that auto-cancels stale queued runs AND auto-kills
hung `Runner.Worker` processes (snapshot → SIGTERM → grace → SIGKILL
→ reap children → quarantine partial builds → verify Runner.Listener
→ optionally wait for GitHub status flip). Implies `--fix`. Emits
`runner.watch` JSON envelopes (`event=auto_kill_worker`,
`phase ∈ {attempt, killed, failed, no-pid-found}`) for telemetry.

Cross-link: Shipyard `skills/ci/SKILL.md#preventing-wedges-runner-
watch--kill-hung-workers`.

### Keep current — `shipyard update` (v0.55.0+)

```bash
shipyard update --check --json   # report installed vs available
shipyard update                  # apply latest stable
shipyard update --to v0.56.2     # pin / rollback to Pulp's minimum
shipyard update --dry-run        # plan only
```

Replaces the bootstrap-only `curl … install.sh | sh` workflow. Pulp's
CI / daily cron should run `shipyard update --check --json` to surface
drift; humans run `shipyard update` to apply.

### Pulp-specific gotchas (real wedge patterns)

- **Persistent native runners need an offline-safe bootstrap.** A runner can
  wedge inside `SystemNative_OpenDir` / `open$NOCANCEL` before a workflow step
  starts when its captured `.path` probes Homebrew first or its Rust homes are
  symlinked onto a slow/offline external volume. Fleet policy keeps
  `/usr/bin:/bin:/usr/sbin:/sbin` first, pins Actions runner `2.335.1` with
  auto-update disabled, and places `RUSTUP_HOME` / `CARGO_HOME` under each
  runner's local `_toolcache`. Verify/apply this through the fleet manifest;
  apply refuses an active `Runner.Worker` and never retries or reroutes a job.
- **iOS AUv3 try-compile hangs.** `test/cmake/test_ios_auv3_configure.sh`
  shells `xcodebuild CMAKE_TRY_COMPILE.xcodeproj build` which can
  deadlock on `simctl` / keychain / codesign on the self-hosted host
  (observed 2026-05-13). The `runner watch --kill-hung-workers` daemon
  detects the stall via `Runner.Worker` not making progress for >5 min
  and kills it cleanly.
- **Test binaries open real windows on the dev mac.** Several
  `pulp-test-*` binaries (auval validation, headless-view variants,
  iOS AUv3 try-compile, visual-harness tests) create macOS surfaces
  during CI. Because the runner runs as the human's user account,
  those windows pop on the dev mac's display. Either move the runner
  to a dedicated user account, or accept the brief popups.
- **PRs that touch CI/runner workflows need a manual handoff.** If the
  PR's macOS lane was cancelled by the wedge, even after `rescue` the
  PR may need a fresh push to retrigger the version-skill-sync check
  too.
- **launchd agent exit 75 + "lease denied … rc=2" → check the plist PATH
  for `/usr/sbin` FIRST.** Before suspecting tart, network, or auth. A
  runner agent crash-looping under `KeepAlive` with last-exit **75**
  (`EX_TEMPFAIL`) and `lease denied … rc=2` in its log, with no VM ever
  booting and jobs queueing forever, is this: tartci's `host_profile.py`
  shells bare `sysctl` (which lives at **`/usr/sbin/sysctl`**) to read
  `hw.ncpu` / `hw.memsize`. launchd agents run a minimal PATH, and the
  generated plist's PATH omits `/usr/sbin` — so `sysctl` raises
  `FileNotFoundError`, `host_profile.py` exits 1, the lease governor
  cannot compute a memory budget, and it denies every lease (failing
  CLOSED, which is correct). The lane is silently dead. Diagnose and fix:

  ```bash
  launchctl list | grep tart-runner        # last-exit 75 = this bug
  # inspect EnvironmentVariables:PATH in the plist; if /usr/sbin is absent:
  # append ":/usr/sbin:/sbin", then reload the agent.
  ```

  After the fix the agent goes exit 75 → 0 and logs `lease acquired …
  cores=6 mem_mb=8192`. Agents that already carry `/usr/sbin` are exit 0;
  the failing set is exactly the `/usr/sbin`-missing set.
- **Non-interactive `ssh host 'cmd'` does not source `.zprofile`,** so it
  lacks `/opt/homebrew/bin` and reports Homebrew tools as missing — this
  produces a FALSE "tart is not installed" census result. Use
  `ssh host 'zsh -lc "…"'` for any host census.
- **`TART_HOME` is per-host BY DESIGN — never default it.** Every Pulp VM tool
  now resolves it through `tools/ci/lib/tart-home.sh`: explicit environment,
  then the host profile's `vm_home`, otherwise a loud error. Do not restore a
  host/path table or a guessed `$HOME/VMs`/`/Volumes/.../VMs` fallback. A bare
  `tart list` using an unbound default store is not proof that the host is idle:
  if `tart run` processes or guest setup exist simultaneously, the result is
  **unknown**. Resolve the receipt-bound profile, query its exact store, and
  corroborate process state before any active-work decision. Pulp's topology
  checker can compare supplied profile/receipt/source-manifest fixtures; live
  store/process reconciliation belongs in TartCI.

### Anti-pattern (legacy)

- `planning/scripts/runner-watchdog.sh --fix` — superseded. Use
  `shipyard rescue` (recover, PR-side) or `shipyard runner watch
  --kill-hung-workers` (prevent, host-side).

### Composition with `Version-Bump` gate

`shipyard rescue` does not interact with the `Enforce version & skill
sync` check. If a PR title or live commit-derived signal starts with `fix:` /
`feat:` and the branch lacks either a `chore: bump versions` commit OR a
`Version-Bump: skip reason="..."` trailer in the range, the
version-skill-sync check fails independently. The trailer block must
be CONTIGUOUS (no blank line between `Version-Bump:` and any other
trailer like `Co-Authored-By:`) or git's `interpret-trailers` won't
recognize it. Verify with
`tools/scripts/version_bump_check.py --mode=report --base=origin/main
--require-bump-for-fix-feat --pr-title="..."` which prints
`bypass honored` when the trailer parses correctly.

### Manual machine-side recovery (true last resort)

If `shipyard rescue` doesn't help (e.g. the runner's host OS itself is
unresponsive, not just the Worker), the machine-side recovery is:

1. SSH the runner host (or open Terminal locally if it's the dev mac).
2. `ps -ef | grep '[R]unner.Worker'` — confirm orphan Worker PIDs.
3. `kill <pid>` (gentle), `kill -9` after 30 s grace.
4. Restart via `~/actions-runner/svc.sh restart` or `launchctl
   kickstart -k gui/$(id -u)/actions.runner.<owner>-<repo>.<name>`.
5. After restart: `shipyard runner watch --kill-hung-workers` (one-time
   foreground) verifies the host is healthy before enabling the
   permanent daemon.

Agents should NOT do step 1–4 themselves; ask the human via
`PushNotification`. Agents CAN and SHOULD run `shipyard rescue` for
the PR-side recovery without waiting.

## Cobertura artifact verification (A2 first cut, 2026-05)

The "Cobertura is structurally non-empty" assertion (pulp #605 — a well-formed XML with `lines-valid="0"` gets rejected by Codecov v5 as "Unusable report") used to live as an inline `python3 -c '...'` heredoc in `coverage.yml`, duplicated for the native and Python lanes.

It now lives in `tools/scripts/verify_cobertura_xml.py`. Both lane verifications call:

```bash
python3 tools/scripts/verify_cobertura_xml.py "$xml" \
  --label "<lane>.xml" \
  --hint "<upstream-step-likely-broken>"
```

If you add a third Cobertura artifact (e.g. a future Kotlin lane), reuse the same script — do not paste a new heredoc. Tests in `tools/scripts/test_verify_cobertura_xml.py` cover missing file, empty file, unparseable XML, lines-valid=0 (with and without `--hint`), lines-valid>0, and label propagation. The pattern follows B1's `classify-subject` extraction — script over inline-Python, single source of truth.

## Format validator baseline diff gate

`.github/workflows/format-baseline-diff.yml` runs the format-validator baseline diff (`tools/scripts/format_baseline_diff.py`) whenever a PR touches `core/format/**`, `core/host/src/plugin_slot_*`, `core/host/include/pulp/host/plugin_slot.hpp`, the baseline fixtures, or the scripts themselves.

Behavior:

- Builds `PulpEffect` (AU + VST3 + CLAP) in Release on the self-hosted macOS lane.
- Installs the three bundles into `~/Library/Audio/Plug-Ins/{Components,VST3,CLAP}/`.
- Captures normalized output from `auval`, `pluginval`, `clap-validator`.
- Diffs against committed fixtures in `test/fixtures/format-baseline/`.

Re-capture procedure (when a diff is intentional):

```bash
tools/scripts/format_baseline_capture.sh --build --plugin PulpEffect
```

Commit the updated `test/fixtures/format-baseline/*.txt` files in the same PR. No exception path — intentional behavior changes update the baseline; unintentional regressions get fixed at the source.

### When this gate goes red, read the validator's own output first

The failure that looks like a Pulp regression is usually the validators not
running at all, and the message `All N validator(s) exited non-zero` does not
distinguish the two. Do not start from the workflow log's summary line:

- The job uploads a **`format-baseline-validator-output-*` artifact** on every
  run (`if: always()`) holding each validator's raw output, its exit code, and
  the normalized capture. That is the evidence; start there.
- The log also carries each failing validator's exit code and the first 20
  lines of its output, which is usually enough without downloading anything.
- **A non-zero validator exit is ambiguous by nature.** `auval` and
  `clap-validator` exit non-zero both when they run and report findings and
  when they cannot load the plugin at all. A fast fail (well under a second)
  points at the latter — a real `auval` pass takes seconds.
- **Refresh `AudioComponentRegistrar` after installing and after removing the
  AU bundle on self-hosted runners.** The registrar caches component metadata
  across jobs. Without `killall -9 AudioComponentRegistrar` plus a short wait
  after the copy, `auval` can see the path but fail immediately with `Cannot
  get Component's Name strings` / error `-50`; without the cleanup refresh, a
  later job can retain the removed component's metadata. This is the same
  discipline used by `validate.yml`, the DAW-bench preflight, and
  `pulp doctor --au-cache`.
- Nothing outside `--diag-dir` survives: the capture writes into a temp dir the
  diff script deletes on every return path. A run invoked without `--diag-dir`
  leaves no evidence at all.

Two structural properties worth knowing before trusting a red here: the gate
**passes when 1 of 2 validators fails and fails when 2 of 2 do**, so its verdict
turns on validator availability as much as on plugin behavior; and with no
committed fixtures in `test/fixtures/format-baseline/` it is in bootstrap mode,
where it cannot detect a regression at all. `concurrency.group` is per-ref, so
it does not serialize two different PRs — co-located runners sharing a `$HOME`
can race over the fixed `~/Library/Audio/Plug-Ins/PulpEffect.*` install paths
and each other's `if: always()` cleanup.

Companion-track item U-3 in `planning/2026-05-17-refactor-roadmap-final.md`.

## Source-tree pollution: root-allowlist mode

`tools/scripts/source_tree_pollution_check.py` now has a fourth mode beyond `stage` / `push` / `files`: **`--mode=root-allowlist`**.

The root-allowlist mode reads `git ls-tree --name-only <rev>` and fails if any top-level entry is not in `ALLOWED_ROOT_PATHS` (a frozenset declared at the top of the script — ~51 entries covering hidden config, root docs, root build/config files, and subsystem directories).

Wiring:

- `.githooks/pre-push` invokes `--mode=root-allowlist --rev HEAD` right after the existing `--mode=push` check. Hard-fail; no env-var bypass.
- `.github/workflows/source-tree-pollution-check.yml` runs the same mode in CI. Triggers on `paths: ['**']` (the check is ~5s — no point gating). Catches direct REST / admin merges that skip the pre-push hook.

Adding a new top-level entry requires the same-PR allowlist update — the gate's error message points contributors to the exact line in the script. See the new "Repo-root hygiene" section in `CONTRIBUTING.md` for the contributor-facing explanation.

Companion-track item U-1 in `planning/2026-05-17-refactor-roadmap-final.md`.

## Generic macOS overflow on `workflow_dispatch`

`resolve-provider` in `.github/workflows/build.yml` applies generic macOS
overflow logic on both `pull_request` and `workflow_dispatch` events. The
current selector is `PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON`; the bare
`local-only` sentinel disables overflow, while an unset variable restores the
GitHub-hosted `macos-15` default.

Pre-2026-05-19 behavior gated overflow on `EVENT_NAME == "pull_request"` only, which silently routed `shipyard pr` ship cycles (`workflow_dispatch`-triggered) back to the local self-hosted Mac. That defeated the 2026-05-18 cloud cutover for the path most contributors hit.

Precedence on `workflow_dispatch`:
1. `inputs.macos_runner_selector_json` (operator override) — always wins.
2. Generic overflow when local Mac BUSY ≥ threshold, unless `local-only`.
3. Local default (`PULP_LOCAL_MACOS_RUNS_ON_JSON`).

Manual `workflow_dispatch` with an explicit selector input still overrides; the fix only changes behavior for dispatches that arrive without one (which is the `shipyard pr` shape).

## Change classifier — skip the native build for non-code PRs

`build.yml` has a `classify` job (ubuntu, ~10 s) that runs
`tools/scripts/classify_changes.py` to decide `native_build_required`.
When a PR touches no C++/Swift build input (docs, `*.md`, `.githooks/`,
`.shipyard/`, etc.), the `build` matrix and `windows-msvc-release-gate`
are skipped at the job level — no runner is allocated, no Skia/Dawn
compile — and the `macos`/`linux`/`windows` alias jobs report a fast
green from Ubuntu.

Key facts:

- `classify_changes.py` is **fail-closed**: any uncertainty, git error,
  or empty diff -> `native_build_required=true` (run the build).
  Skipping is the optimization; running is the safe default.
- The skip-safe set is a deliberately small allowlist (`*.md` anywhere,
  `docs/`, `planning/`, `.githooks/`, `.shipyard/`, `.shipyard.local/`,
  a few exact files). The pure-Python
  `tools/scripts/test_prepush_gate_supervisor.py` regression is one exact
  exception because it only exercises the skip-safe `.githooks/` supervisor;
  do not generalize that exception to neighboring scripts. Everything else —
  including `core/**`, all `CMakeLists.txt`, `tools/cmake/**`, `tools/scripts/**`,
  `.github/workflows/**`, and the classifier itself — forces the
  native build.
- **Deny-list exception: `docs/migrations/*.md` forces the build.**
  Those `.md` files are globbed with `CONFIGURE_DEPENDS` into the
  generated `migration_index.cpp` by `tools/cli/CMakeLists.txt`, so
  `FORCE_BUILD_PREFIXES` overrides the `.md`/`docs/` skip-safe rules.
  Any future doc path that feeds codegen must be added there too.
- Diffs are collected with `git diff --no-renames` so a code→docs
  rename can't hide the old code path and wrongly classify skip-safe.
- The required `macos` check is now produced by a dedicated `macos`
  alias job (`if: always()`), NOT by the build matrix leg. The matrix
  leg is named `macOS (ARM64) [<provider>]` uniformly with linux/windows.
- The alias jobs are **fail-closed on a `classify`-job failure**: if
  `needs.classify.result != 'success'` the `macos` gate fails RED
  rather than trusting an unwritten/empty `native_build_required`.
- A release-bot-generated `release/version-bump` PR is the one narrowly
  permitted semantic exception to the path allowlist. Root `CMakeLists.txt`
  and plugin JSON still classify as native first; the workflow may override
  that result only when the protected base's
  `tools/scripts/generated_version_bump_check.py` proves one signed bot commit,
  exact current-main parentage, the fixed branch/title/marker, one associated
  PR, and a candidate tree byte-identical to rerunning the protected base's
  version-at-land writer (including derived projections). A candidate behind
  exactly one prior queue entry qualifies only in the proven #7771 shape: its
  original protected base is the prior group's first parent, the writer and its
  derived generator's complete local executable dependency closure did not
  change, trusted derived generators are executed from that original base, and
  the complete merge-group tree is exactly cumulative base plus the regenerated
  version projection. Any missing script, API/signature error, nested/unknown
  topology, writer drift, extra byte, or ambiguous association retains the full
  native path. The verifier's own PR therefore cannot approve itself.
- To change what counts as skip-safe: edit `SKIP_SAFE_PREFIXES` /
  `SKIP_SAFE_EXACT` / `FORCE_BUILD_PREFIXES` in `classify_changes.py`
  and add a case to `test_classify_changes.py`. Never widen the
  allowlist without a test.

Companion plan: `planning/2026-05-19-ci-optimization-plan.md`.

## macOS runner routing — event-class JIT primary, reviewed hosted overflow

**A routing var describes REALITY; `build.yml`'s `||` default is only the
fallback.** Read the live variable before reasoning about where a leg runs —
the workflow's literal default is what happens when the var is *unset*, not
what happens.

`tools/scripts/runner_topology.json` is the single reviewed lane-to-selector
contract; do not copy its arrays into this skill. Reconcile it against live repo
variables and service history with:

```bash
python3 tools/scripts/runner_topology_check.py --mode=report
```

The contracted required gate is the M1/M3/M5 JIT pool. The JSON records the
legacy base selector; `build.yml` replaces `pulp-gate-fast` with the exact PR or
merge-group event class before assignment. The reviewed overflow contract is
GitHub-hosted `macos-15`; `local-only` remains its explicit disable sentinel,
and paid Namespace variables remain unset. Read both surfaces together for the
effective selector.

**Required/advisory isolation.** The `pulp-build*` and `pulp-preamble*` labels
are reserved for required merge-gate work. Example validation and format
baseline validation resolve `PULP_ADVISORY_MACOS_RUNS_ON_JSON`, falling back to
hosted `macos-15`; the real-GPU web proof resolves
`PULP_ADVISORY_GPU_MACOS_RUNS_ON_JSON` and skips when it is unset. Both paths
pass through `tools/scripts/resolve_advisory_macos_runner.py`, which rejects
required-pool label prefixes and rejects a configured self-hosted selector that
lacks a `pulp-advisory-*` identity. Hosted strings are accepted only from the
resolver's explicit reviewed allowlist; do not broaden it to a numeric pattern.
Do not work around the identity guard by assigning an advisory label to a
required runner. A local advisory lane needs its own tartci supervisor,
governor capacity, and GitHub runner label. Shipyard and tartci are the
placement/control path; do not use Orchard.

MacOS overflow is currently **disabled**, so saturated required work remains on
the fast local JIT pool. Namespace vars are deliberately UNSET for cost control
— do not treat them as an available lane.

**A Linux/Windows runner without a `pulp-host-*` label is invisible to every
lane.** Those lanes pin a machine, and GitHub selects a runner only when it
carries EVERY requested label — so a supervisor registering just
`self-hosted,Linux,ARM64,pulp-build-linux` yields a runner that is online, idle,
and unselectable, with no error anywhere. **Do not diagnose that as runner
saturation**: `runner list` shows free runners, the queue shows waiting jobs, and
both are true at once. The tell is a free runner whose label set is a strict
subset of the lane's. Seen live as 3 free Linux runners against 8 queued Linux
jobs. `tools/ci/tart-runner-linux.sh` and `qemu-runner-windows.sh` now refuse to
register unless a host tag is declared (`--host-tag` / `PULP_RUNNER_HOST_TAG`, in
the LaunchAgent); see the `tart-ci` skill for the tag vocabulary and why
`shipyard runner tag` (`studio`) is not the routing label (`pulp-host-macstudio`).

If `PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON` is unset, `build.yml`'s default
takes over and overflow goes to GitHub-hosted `["macos-15"]`. That default is
the safety net, not the design.

The `resolve-provider` job in `build.yml` decides per-run where each
`Build and Test` macOS leg runs:

- **Local first.** While the local self-hosted Macs have spare capacity
  the macOS leg routes to them (`PULP_LOCAL_MACOS_RUNS_ON_JSON`).
- **Overflow is disabled live.** `PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON`
  is the `local-only` sentinel, so saturation leaves the leg on the fast
  local JIT pool. If an operator deliberately re-enables overflow, that
  variable supplies its selector; unsetting it falls back to free
  GitHub-hosted `macos-15`.
- **Historical capacity-aware implementation (`#3299`).** Before event-class
  V2 and the live `local-only` sentinel, "saturated" was decided by SUPPLY
  first, not just DEMAND. `_count_idle_local_runners()` queries
  `actions/runners` for self-hosted macOS runners that are `online` AND
  `busy == false` carrying `LOCAL_MAC_RUNNER_LABEL`. If **any** idle
  local runner exists, the leg stays local — never overflow while a
  Studio or the M5 sits idle. Only when there is no known idle local
  supply does it fall back to the older demand heuristic
  (`_count_busy_local_mac_runners() >= PULP_LOCAL_MAC_OVERFLOW_THRESHOLD`,
  default 2). The idle probe's failure mode is *unknown* (`-1`), which
  falls through to the demand heuristic — a probe blip never silently
  force-overflows. Before `#3299` the fixed threshold of 2 overflowed a
  3rd leg to the (saturated, ARM-incompatible) hosted pool while the 3rd
  Studio + the M5 were idle — the saturation-timeout failure below.
- **Operator override.** A `workflow_dispatch` `macos_runner_selector_json`
  input always wins.

### Skia provisioning on the macOS leg

`build.yml` runs a **"Fetch prebuilt Skia (macOS)"** step before
Configure. The Skia `.a` libraries are LFS-declared but not committed,
so a fresh checkout has only pointer files — without real Skia,
`FindSkia.cmake` sets `PULP_HAS_SKIA=FALSE` and any examples-ON / GPU
build fails the Configure gate (`examples/design-tool`). The step
unconditionally runs `tools/scripts/fetch_skia_for_release.py
darwin-arm64` (pinned, sha256-verified asset from
`tools/deps/manifest.json`).

The script is **idempotency-stamped**: after a successful unpack it
writes the asset sha256 to `external/skia-build/.skia-asset-sha256`. On
the next run it skips the ~250-500 MiB download only when that stamp
matches the *current* manifest pin. On a self-hosted runner
(`clean: false`) Skia persists between builds, so the common case is a
fast no-op — but a `manifest.json` Skia pin bump changes the expected
sha, the stamp no longer matches, and the asset is re-fetched. Never
guard the fetch on "is `libskia.a` present?" alone: a stale local
library would silently shadow a new pin (pulp #2458 follow-up).

### A JIT lane's IDLE state is indistinguishable from DEAD

**NEVER infer a dead lane from a runner census.** The macOS and Linux VM lanes
are JIT: a runner registers with GitHub **only while serving one job**, then
deregisters. So "zero runners carry `pulp-build-vm`" in `actions/runners` is
**both** the healthy-idle state **and** the dead-lane state. The census cannot
tell them apart.

This has already caused a real misdiagnosis: a census showed 0 runners carrying
`pulp-build-vm` / `pulp-build-vm-release`, that was read as "dead labels", and
three lanes were rerouted onto GitHub-hosted `macos-15` — moving work **off
healthy VMs**. The supervisors were alive and booting VMs the whole time. The
reroute was reverted.

- **A label-satisfiability check is the WRONG instrument for a JIT lane.** It
  would false-alarm every idle night. Satisfiability checks are valid only for
  **persistent** runners — e.g. the bare-metal `pulp-build-studio` Studios.
- **The only signal that distinguishes alive from dead on a JIT lane is QUEUE
  AGE.** If jobs are being served, the lane is alive regardless of the census.
- **Queue DEPTH is not stall.** 40 queued runs with a 5-minute median age is
  healthy churn from many concurrent agents.
- **Do not set a naive age threshold.** Measured healthy baseline (2026-07-16):
  median queue age 5 min, oldest 31 min, 3 runs >30 min under normal busy load.
  A ">30 min = broken" alarm fires on a healthy pool.

To check a JIT lane is alive, look host-side (supervisor running, VMs booting)
rather than at the label:

```bash
ssh <host> 'zsh -lc "launchctl list | grep tart-runner"'   # last-exit 0 = healthy
```

### An online JIT runner can still be phantom capacity

Runner presence is not assignment proof. On 2026-08-30, M1 and M5 minted
Pulp runners with the exact PR-head labels and GitHub reported them online and
idle in the organization runner inventory, while Pulp's repository runner
inventory exposed no usable row and every PR-head `macos` job stayed queued.
The event-class label split was correct; registration authority had not been
split with it.

The governed Tart fleet contract is class-specific on every M1/M3/M5 profile:

- `pulp-build-merge-group` registers through Pulp's repository-scoped group `1`
  and derives merge priority `110`.
- `pulp-build-pr-head` registers through Pulp's repository-scoped group `1` and
  derives PR priority `100`.

Both classes must remain repository-scoped because a `merge_group` ref cannot
satisfy the protected organization group's main-ref workflow restriction.
Never collapse the classes back into one static label set or move either to
group 3. A runner is proven usable only when it appears in Pulp's repository
runner endpoint, becomes busy, and its exact name binds to the queued job.
`online` plus `busy=false` in the organization inventory is not capacity
evidence.

If the repository cannot see an online runner, do not rerun the PR, weaken the
labels, or add hosted overflow. Inspect the loaded profile's
`TARTCI_RUNNER_WORKFLOW_TIER_GROUPS`, registration scope, and TartCI denial
receipt. Deploy the reviewed TartCI profile transactionally at an idle boundary,
then require a real repository-visible assignment receipt. TartCI's keyed denial
fuse prevents repeated VM churn after a 401/403/404 JIT denial; a pre-lease
registration-token capability probe is the follow-up that moves the first auth
failure ahead of VM boot.

### The unifying invariant — no name without a heartbeat

> **A name is trustworthy iff an automated process dereferences it on a
> schedule and alarms on failure.**

A host registry entry, a test asserting a hostname, a runner label, a
`TART_HOME` path — each is a name written where it is consumed and dereferenced
by nothing. Every one of them rots silently and is discovered only during an
outage. When you add a name to CI config, ask what dereferences it on a
schedule; if the answer is "nothing", it is already suspect.

The overflow target is `OVERFLOW_MACOS_RUNS_ON_JSON`, which defaults to
`["macos-15"]`. The repo variable `PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON`
overrides it: set it to a different runs-on selector to change the
target, or to the sentinel `local-only` to disable overflow (macOS
pinned to the local runner). An *empty* variable value does NOT disable
overflow — GitHub treats unset and empty identically — so `local-only`
is the explicit off switch. Routing is decided
**once per run, at dispatch** — a leg already sent to `macos-15` is not
migrated back to local if the Mac frees up; cloud overflow is parallel
capacity, not a queue waiting on the Mac. Namespace is no longer a
routing target (cut for cost, 2026-05-20). The matrix leg name's
`[<provider>]` suffix reflects the real route (`local` / `github-hosted`
/ `operator`).

### Recover a saturated/wedged macOS gate (don't debug it)

The required `macos` check failing at **~28–32 min with no test output**
is almost never a test failure — it is a **saturation timeout**: the leg
overflowed to a contended pool (or queued behind busy local runners) and
never actually ran. Recover; do not go spelunking in logs for a bug that
isn't there. Confirm by opening the failed job — a saturation timeout has
no compile/ctest lines, just a runner-acquisition gap.

The recovery rules, learned the hard way:

1. **The required `macos` check is satisfied ONLY by the `pull_request`
   run's macОС job.** A `workflow_dispatch` run (e.g. an
   `macos_runner_selector_json` operator-override that you route to an
   idle runner) will go green, but its success does **NOT** supersede or
   satisfy the PR's required check. Branch protection keys off the
   `pull_request`-triggered run specifically.
2. **Re-run the PR run, don't dispatch a new one.**
   `gh run rerun <pr-run-id> --failed` re-runs the failed macОС leg
   *within the pull_request run* → satisfies the gate. `gh workflow run`
   creates a `workflow_dispatch` run, which (per #1) does not. Find the
   PR run with `gh run list --branch <branch> --workflow build.yml`.
3. **NEVER `gh run cancel` the auto `pull_request` run.** A cancelled
   run leaves a sticky `macos = CANCELLED/FAILURE` check on the PR that a
   later dispatch run cannot overwrite; you then *must* `gh run rerun`
   the cancelled run to clear it. Let it finish or rerun it — don't
   cancel it.
4. **Check real capacity before assuming "stuck."** For the current M1/M3/M5
   JIT gate, a runner census alone proves neither idle capacity nor outage.
   Inspect queue age, each host's enabled Pulp supervisors and lease/VM state,
   then require repository-visible exact assignment evidence. See "Current
   required-macOS truth" and "An online JIT runner can still be phantom
   capacity" above. Persistent-runner census instructions from the former
   Studio topology are historical and must not be applied to this pool.
5. **Batch stuck PRs into one.** When several PRs are wedged on the gate,
   combining them into a single PR (cherry-pick onto one branch) cuts N
   macОС runs to 1 — landed `#3411` (four PRs) this way on one local run.
6. **The durable fix is `#3299`** (capacity-aware routing above) — it
   stops the overflow-to-saturated-pool-while-idle root cause, so this
   recovery dance should become rare. If you still hit it often, the
   idle probe or the `LOCAL_MAC_RUNNER_LABEL` is likely misconfigured.

## Host-quirks staleness check (host-quirks P4)

`.github/workflows/host-quirks-staleness.yml` — scheduled (monthly) +
`workflow_dispatch`, **preview-only**. Runs `tools/scripts/host_quirks_staleness.py`
(+ the staleness unit test + the catalog parity test) to surface host-quirk
catalog entries due for re-review: Speculative/LessonOnly rows not
re-verified in N months (default 6), and Validated rows with
`affected_versions` (re-check vs the host's current major). It prints a
report and exits 0 — it does **not** open issues (no false-positive spam).
Promoting it to auto-open tracking issues is a future opt-in. Detection
lives in the pure `stale_entries()` fn (unit-tested in
`tools/scripts/test_host_quirks_staleness.py`), so it needs no clock/network.

## Gotcha: sign-and-release fallback must be macOS 15

`sign-and-release.yml` once fell back to GitHub-hosted `macos-14`, whose default
Xcode 15.4 Apple clang lacked C++20 P0960 (parenthesized aggregate init). The
self-hosted PR `macos` lane used a newer clang, so CLI/import translation units
compiled on every PR but failed only in the tagged release path; tags kept
advancing while no Release/binaries published. Fix: route the GitHub-hosted
fallback to `macos-15` and keep selecting the newest installed Xcode 16.x
(`release-cli.yml` and the Build/Test gate already use macOS 15). If the
GitHub-hosted macOS image changes again, verify the fallback runner still has
C++20 parity with the PR lane before changing release routing.

The fallback is last, not first. `sign-and-release.yml` must prefer the isolated
`PULP_RELEASE_MACOS_RUNS_ON_JSON`, then optional Namespace, and only then hosted
`macos-15`. It must not import the Developer ID private key on the shared local
PR pool. A split route can finish all CLI/SDK assets while the signing leg
remains queued with no runner, leaving the release draft unpublished.

## A dispatch publishes, but it mislabels provenance — so it cannot be the fix

`workflow_dispatch` of `release-cli.yml` from `main` builds an older tag's
sources using **main's** workflow file. That is genuinely useful — it is how a
workflow-level fix reaches a tag cut before the fix landed — and it is why a
release-lane fix should be titled `build(release):`/`fix(release):` on `main`
rather than needing a new tag.

**But the resulting release is unverifiable downstream.** GitHub derives build
provenance from the OIDC run context — the ref the workflow file was loaded
from — not from what `actions/checkout` fetches afterwards. So the attestation
records:

```
externalParameters.workflow.ref = refs/heads/main
resolvedDependencies[0]         = git+.../pulp@refs/heads/main
                                  gitCommit = <main's HEAD>
```

and the tag's own commit appears in **no** attestation. Any consumer pinning the
tag then fails, correctly:

```
gh attestation verify <sdk.tar.gz> --source-digest <tag commit>
Error: expected SourceRepositoryDigest to be <tag commit>, got <main HEAD>
```

Do **not** read that as a broken consumer and do not drop `--source-digest` to
clear it. A trusted workflow ref vouching for an arbitrary checkout is the exact
laundering shape that flag exists to reject.

The rule: **a tag is immutable including the workflow file it carries.** When a
tag's workflow is broken, supersede it with a new tag cut from the fixed default
branch; do not repair it by dispatch. `release-cli.yml` now enforces this — a
dispatch whose own ref is not the tag it publishes fails before anything builds,
so dispatch stays available for retrying a flaky tag-push run and nothing else.

If you are diagnosing a release that published but a downstream pin refuses,
check provenance before the bytes:

```bash
ghapp api "repos/OWNER/REPO/attestations/sha256:<asset digest>" \
  --jq '.attestations[].bundle.dsseEnvelope.payload' | base64 -d | grep -o 'refs/[^"]*'
```

## A Windows-only digest failure is CRLF, not corruption

When a release leg fails a **content digest** on Windows while **Linux passes on
the same commit**, stop looking at the file. Git for Windows ships
`core.autocrlf=true`, so the checkout rewrites every LF to CRLF and any pinned
text file hashes to bytes no pin can describe.

This blocked v0.832.0 (and with it five stacked tags), failing both Windows legs
with `pulp-sdk/share/pulp/gpu-recipes.yaml differs from the selected release
catalog digest`. It matters because `release-cli.yml`'s publish job gates on the
**all-platform** `build-cli` matrix — a failed Windows leg blocks publishing even
when darwin is green, so "we only care about macOS" is true for the PR gate and
false for the release.

Confirm it in one step — compare byte counts, not content:

```bash
# a text member of a Windows archive vs its LF blob: size delta == newline count
git cat-file -s "$TAG:docs/status/gpu-recipes.yaml"     # e.g. 5716, 212 LF, 0 CRLF
# the archive's copy will be 5716 + 212 = 5928, 100% CRLF, 0 bare LF
```

The fix is to force upstream LF **before** the checkout — never to relax the
digest (that knowingly ships a Windows SDK whose bytes differ from every other
platform's, against the "embeds these exact catalog bytes" contract):

```yaml
- name: Check out release sources with upstream LF bytes
  shell: bash
  run: |
    git config --global core.autocrlf false
    git config --global core.eol lf
- uses: actions/checkout@v5
```

Ordering is load-bearing: `git config` **after** a checkout cannot undo line
endings already written to the working tree. `ReleaseCliChecksOutLfBytes` in
`tools/scripts/test_release_workflow_test_step.py` asserts both the step's
existence and its position.

Same failure class, already fixed once elsewhere: the Three.js runtime pin in
`tools/cmake/PulpDependencies.cmake` (its comment states the diagnosis). If you
hit a third instance, prefer `* text=auto eol=lf` in `.gitattributes` as the
durable repo-wide guard — but note that only helps **future** tags, since a
checkout reads the tag's own tree.

## "Tag exists but no published release" → the reconciler owns this

`release-reconcile.yml` (every 30 min) is the single owner of "did every recent
tag actually ship?", and the only workflow allowed to act on the answer. It
replaced four report-only watchdogs (`release-guard`, `release-health`,
`release-cli-watchdog`, `release-draft-stuck-check`) that between them filed 413
issues in two weeks without fixing anything, while the real recovery was a human
running `gh workflow run` by hand. Their grace windows (15-60 min) were also
shorter than the real pipeline (70-165+ min), so they alarmed on healthy
releases that were still building.

It reconciles rather than reports: for each recent SDK tag it compares desired
state (a published release) against actual state and drives the difference to
zero by **re-dispatching** the release. It never cancels or deletes anything.

Convergence rules worth knowing before you intervene by hand:

- A published release is done. `release-cli` publishes only after an
  `--exact-required` asset check, so "published" already means complete.
- A tag whose release run is queued or in progress is **left alone, however
  long it has been running**. Slow is not broken.
- A tag younger than `GRACE_MINUTES` is left alone even with no run visible yet.
- A completed terminal failure opens the circuit and is reported rather than
  retried: SDK tags are immutable, so retrying a real validation failure only
  repeats runner load.
- No run, or a run interrupted by cancellation/staleness, is re-dispatched up to
  `MAX_ATTEMPTS`.
- A tag that exhausts `MAX_ATTEMPTS` gets ONE issue, updated in place, labelled
  **`release-guard`** (`tools/scripts/release_reconcile.py`).

So to debug a "release stuck" report: read the newest `release-reconcile.yml`
run and the open `release-guard` issue, not the individual legs. Most of the
time the answer is that it is still building and the reconciler is correctly
doing nothing.

## Build and test strings must take a share, not the machine

A `.shipyard/config.toml` POSIX `build` string runs on the shared self-hosted
Mac, so it must take a *share* of the host — route it through
`tools/ci/governed-build.sh` and carry NO `--parallel`/`-j` (the wrapper injects
a leased/bounded `-j`). "Bounded" is not enough: `--parallel $(getconf
_NPROCESSORS_ONLN)` has a count yet claims every core, so it starves concurrent
builds and the required `macos` gate — the guard rejects that whole-machine shape
on shared-host surfaces (see below). A bare `--parallel` is worse (unbounded
`make -j`) and is rejected everywhere. The CI *workflows* (`.github/workflows/**`)
run on ephemeral GitHub-hosted runners where `-j$(nproc)` is correct and allowed.
The Windows (ssh-windows / PowerShell) overrides use a fixed literal `--parallel 4`
— `$(…)` doesn't parse there, and unbounded MSBuild link parallelism trips
LNK1104 on ARM64.
`tools/scripts/build_parallelism_guard.py` enforces this in the `validation.gates`
setup chain and as a ctest. It rejects two shapes: a **bare** `--parallel`/`-j`
(unbounded) anywhere, and — on the shared-host surfaces agents copy from
(`CLAUDE.md`, `.shipyard/config.toml`, `.agents/skills/**`) — an explicit but
**whole-machine** core-count expansion (`-j$(nproc)` / `-j$(sysctl -n hw.ncpu)` /
`--parallel $(getconf _NPROCESSORS_ONLN)`): it has a count, so it is not
unbounded, but on a shared Mac it claims every core, so concurrent builds starve
each other and the required `macos` gate validating alongside them. The same
expansion stays allowed on `.github/workflows/**` (ephemeral runners, nothing
else on the box) — the rule is a property of the host, not the command.

Every POSIX `build` stage in `.shipyard/config.toml` — `default`, `parser`, AND
`smoke` — runs through `tools/ci/governed-build.sh`, NOT a bare/whole-machine
`cmake --build`. (The `smoke` lane used to run `--parallel $(getconf
_NPROCESSORS_ONLN)` whole-machine on the shared Mac; it is now governed like the
others.) Shipyard's `local` backend executes the config string directly on the
host (bypassing the pulp CLI's lease integration), so the wrapper is what puts a
host-native validation build under a tartci host lease: it sizes `-j` from
`tartci host-profile`, holds a `build`-priority lease for the build's duration
(released via an EXIT trap — it runs the build as a child, never `exec`, so the
trap fires), and falls back to a bounded local `-j` when tartci is absent (build
VM / plain checkout) or the lease is denied (it never fails the build and never
piles onto a saturated host). Keep new POSIX build strings routed through it;
don't add a bare or `$(nproc)`-style `cmake --build … --parallel` back to the
`local`/ssh-linux lanes.

Every POSIX `test` stage uses that wrapper too. The wrapper exports
`CTEST_PARALLEL_LEVEL` from the leased share, so a bare `ctest` command does not
silently serialize the full suite and an explicit `-j$(nproc)` cannot consume
the shared host. Do not add a `-j` or `--parallel` to the Shipyard test string:
the leased environment value is authoritative, while CTest continues to honor
`RUN_SERIAL` and `RESOURCE_LOCK` test properties. Real-CoreAudio suites use
`RUN_SERIAL` for capacity-independent isolation; `PROCESSORS 8` alone is not
isolation once a dedicated builder grants more than eight slots.

`SIGKILL` cannot run the wrapper's release trap. That does not leave capacity
poisoned until a timer or operator acts: tartci's next `leases acquire`
revalidates PID, process-start, and boot identity under the store lock, reaps a
dead/reused owner, and calculates capacity only from the survivors. It does NOT
reap a stale heartbeat whose owner identity still matches — elapsed time is not
authority to steal cores from live work. `tools/ci/test_governed_build.py`
proves both halves against a private out-of-process store when tartci is
installed; its hermetic governor cases remain unconditional.

## macOS Intel (x86_64) CI tiering

Intel portability is verified in four tiers (owned by the `intel-canary` skill;
full design in `docs/guides/intel-support.md`). CI-relevant facts:

- **Tier 0** is a step in `build.yml`'s ARM macOS job, gated on the
  `PULP_INTEL_CANARY` repo variable (set `1` on `Generous-Corp/pulp`; forks
  default off and skip it). It runs the lint + a GPU-off x86_64 compile.
- **Tier 1** is `intel-portability.yml` — a path-triggered **advisory** x86_64
  lane on the STABLE `macos-15` (arm+Rosetta). Do NOT add it to required checks.
- **Tier 2** is `nightly-intel.yml` — job A on the flaky native `macos-15-intel`
  (quarantined here, `timeout-minutes: 120`, infra-vs-product watchdog), job B a
  universal cross-check on `macos-15`. Opens/auto-closes one dedup watchdog
  issue.
- **Tier 3** is the `universal-crosscheck` job in `nightly-intel.yml` (nightly, advisory). It is deliberately NOT on the release path: an advisory gate must never compete with the release for the hosted macOS pool.

Hard rule: **no Intel work ever routes to the self-hosted Studios** (they host
the required `macos` gate) and **Namespace is never used**. All Intel lanes run
on free-for-public-repo GitHub-hosted macOS runners.

## Release page: ONE workflow owns the GitHub Release

`release-cli.yml` is the **sole creator** of the GitHub Release for a `v*` tag:
it sets the title (the **bare tag**, e.g. `v0.645.0` — no "Pulp CLI" prefix),
the body (humanized highlights from `tools/scripts/compose_release_notes.py
--footer`, which appends the `**Full changelog:** CHANGELOG.md § X` +
`**Previous release:** vA.B.C` footer), and uploads the CLI/SDK binaries.
`sign-and-release.yml` must **only** `gh release upload` `appcast.xml` onto that
release — it must NOT create/name/draft it. Both fire on the same `v*` tag, so if
sign-and-release creates/renames/drafts a release it RACES release-cli and
last-writer-wins produces inconsistent titles (`Pulp CLI vX` vs `vX`),
draft/published flips, stray `release-untagged-*` entries, and GitHub's
`Full Changelog: A...B` compare footer instead of the CHANGELOG-§ one. Release
notes link a PR (`#N`) for every entry via `compose_release_notes.py`'s
`pr_for_commit` GitHub commit→PR lookup; only genuine direct-to-main commits show
a short SHA. See `planning/2026-07-10-release-page-hygiene.md`.

## The non-Skia build guard (why it exists)

`non-skia-build-guard.yml` compiles `core/view` with `PULP_ENABLE_GPU=OFF` (⇒ no Skia ⇒ the
Core Graphics path) on a **GitHub-hosted** macOS runner, compile-only.

It exists because **the build is not a required check**, so code that does not compile has
reached `main` twice — and each time it then blocked *every* PR, since Shipyard's pre-push
diff-cover gate runs a build (#6079, #5958). Both breaks **only reproduce without Skia**,
which nothing else in CI builds.

The deeper failure it addresses: when the only build signal is the flaky self-hosted macOS
lane, a *real* failure is indistinguishable from noise — #5958's genuine compile error was
waved through as flake for exactly that reason. This guard is fast (~7 min) and deterministic
so the signal is believable. **Make it a required check** (#6087); that is the actual fix.
## Gotcha: a second job that builds the same target needs the same SETUP

Web lanes build the Pulp UI wasm module in more than one job (`web-plugins.yml`
builds it for the browser fixture *and* for the GPU-audio proof;
`wclap-cloudflare.yml` builds it for the deploy). Each job is a fresh runner with
its own `$GITHUB_ENV` — nothing is inherited. The GPU-audio-proof job configured
`build-webui` **without** `-DPULP_WEBUI_CHOC_INCLUDE` and never ran a "vendor
choc" step, because choc is FetchContent'd and its Linux twin *did*. Result:
CMake hard-failed at configure, ~30 s in, so the job never reached the thing it
exists to prove — and the failure read as "GPU audio proof failed," which points
at the GPU and not at a missing header path.

When you add a job that builds an existing target, diff its steps against the job
that already builds it. If one vendors a dependency or exports an env var, yours
needs it too. Guard the clone (`[ -d "$HOME/x" ] ||`) — self-hosted runners keep
`$HOME` between jobs, so an unguarded `git clone` fails on the second run.

## Gotcha: never commit a symlink into a build directory

A convenience symlink (`examples/web-demos/wclap-build/build` → a local build dir)
got swept up by a broad `git add`. CI then ran `cmake -B` at that path, could not
create `CMakeFiles/pkgRedirects` through a link to a directory that does not exist
on the runner, and **two** lanes — WebCLAP and the Cloudflare deploy — died in
~30 s with a message about the *build directory* being unwritable, which sounds
like a runner-permissions problem and is not one. Check `git status` before an
`add -A` in a tree where you have made local build symlinks.

### Release legs are individually routable (local pool / one machine / GitHub)

`release-cli.yml` resolves each leg's runner from a `platform -> runs-on` map
(`tools/scripts/resolve_release_runners.py`), driven by per-leg repo variables.
Moving a release build is a variable, never a code change:

```bash
tools/scripts/release_routing.sh show
tools/scripts/release_routing.sh local  linux-arm64      # -> local VM pool
tools/scripts/release_routing.sh pin    linux-arm64 m5   # -> that ONE machine
tools/scripts/release_routing.sh github linux-arm64      # -> revert, next tag
```

**Fluidity invariant:** every variable unset == today's GitHub-hosted routing. If the
local pool is down, `github <leg>` is a full revert in one command.

The lightweight resolver jobs for `release-cli.yml` and
`sign-and-release.yml` may use the always-on trusted MacPro Linux/X64 pool
without moving artifact builds or publication there. Their selector priority is
`PULP_RELEASE_CONTROL_LINUX_RUNS_ON_JSON`, then the existing
`PULP_LOCAL_LINUX_RUNS_ON_JSON`, then `ubuntu-latest`. Keep this routing limited
to tag-push or maintainer-dispatch workflows, and keep resolver policy checkouts
pinned to the repository default branch; never expose the persistent pool to
`pull_request` or `merge_group` code through this fallback.

Facts worth keeping (measured):

- The local macOS VM built `darwin-arm64` in **6.4 min after a 0.8 min wait**.
  GitHub-hosted legs took **39-72 min to EXECUTE**, and `darwin-x64` sat **127 min**
  in the hosted queue. The queue, not the compile, was the release's long pole.
- `darwin-x64` needs **no Intel machine** — it is already a cross-compile
  (`-DCMAKE_OSX_ARCHITECTURES=x86_64`), and the release golden has Rosetta, so the
  smoke leg can RUN the x86_64 binary it just built. Verified by booting the golden.
- **The x64 Linux/Windows legs have no local preset.** Both VMs are ARM64 guests, so
  x86_64 there means emulation, and these are shipped artifacts. Their hosted queues
  are ~0 — the pain was never there.
- **Do NOT batch both arches into one job to "reuse the warm VM".** The host ccache is
  already mounted into every ephemeral VM (that is why the build was 6.4 min), so a
  warm cache needs no long-lived VM.
- **Parallelism is bounded by the tartci GOVERNOR, not Apple's limit.**
  `TARTCI_MACOS_HARD_MAX=2` is Apple's guest cap, but the governor reserves cores for
  the required `macos` PR gate (`total_cores 14`, `reserved_gate_cores 8`), leaving 6
  — exactly ONE release VM per host. The darwin legs SERIALIZE on a host, and local
  release builds compete with PR validations for those same non-gate cores.
- **GitHub Actions has NO runner priority.** A pool labelset does not give
  M3-then-M5-then-M1 ordering; the job goes to whichever matching runner registers
  first. Ordering belongs on the tartci supervisor side. `pin` is the deterministic
  lever today.
- **Host-label hygiene matters for pinning.** A supervisor advertising two host labels
  (e.g. both `pulp-host-m5` and `pulp-host-macstudio`) makes `pin` land somewhere you
  did not choose. Check the labels before trusting a pin.

## Git-safe push in the shared-worktree repo (2026-07-15)

This `.git` is shared across 100+ worktrees. In that topology `git push origin <branch>`
has silent failure modes — a *no-op push* (HEAD detached, branch ref lags, git reports
"Everything up-to-date" while pushing nothing) and stale behind/ahead reasoning. A pre-push
hook cannot catch the no-op push (a no-op push never fires it), so the real fix is a wrapper.

**Prefer `tools/scripts/git_safe_push.sh` over a bare `git push`** for PR branches:

```bash
tools/scripts/git_safe_push.sh [<branch>] [-- <extra git push args>]   # e.g. -- --force-with-lease
```

It refuses on detached HEAD, fetches the target first, always pushes `HEAD:<branch>` (what
you built, never a lagging bare ref), and verifies `remote == local HEAD` afterward — failing
loud on a no-op. The pre-push hook (`.githooks/pre-push`) adds two backstop guards for bare
pushes: it refuses a **detached-HEAD** push and an **empty-diff-vs-base** push (the latter
catches a rebase that flattened a branch to zero files). Root cause + the four-fix plan:
`planning/friction/2026-07-15-git-state-in-shared-worktree-hell.md`.

## Coverage-on-main can go red from a time-budget kill (not a code failure) (2026-07-15)

The `Coverage` workflow (`coverage.yml`) is **advisory** — never a merge gate (the
authoritative gate is the separate `Diff coverage required` check). It runs the instrumented
build + full ctest suite under an internal watchdog that SIGTERMs the suite before the job cap
and **drops any partial Cobertura report**. Historically only the `os-windows` leg was
neutralized (job-level `continue-on-error: matrix.os=='windows'`), on the assumption that
macOS/linux always finish under budget. The suite grew (~13.5k ctest cases) and the macOS leg
started crossing the budget too — killed suite → dropped XML → the mandatory `Verify Cobertura
XML exists` step reddened `main`, repeatedly. **A red `Coverage` run on main whose macOS/linux
leg failed at "Verify Cobertura XML exists" / "Upload Cobertura XML" is almost always this
budget kill, not a real build break** — look for `Terminated: 15` / exit `143` in the "Run
coverage suite" step.

Now a budget hit remains advisory only on Windows. Linux and macOS are receipt-authoritative:
their Cobertura verifier runs and fails if the report is absent, so a green workflow can no
longer hide missing native uploads. A shared coverage CTest policy also skips the
slow/soak/configuration proofs already enforced by the primary build matrix; both the canonical
coverage script and local pre-push diff coverage consume it across GitHub-hosted and
SSH/self-hosted M1/M3 callers, and
`--include-slow-tests` opts into the full suite. The suite runs ctest in parallel (`-j`, capped
like `build.yml`) with a per-test `--timeout`. The receipt watchdog remains the cross-run alarm.
Editing
`coverage.yml` requires a `docs/guides/versioning.md` touch (config-doc map) and updating this
skill (skill-sync map).

## Coverage watchdogs require PROOF OF UPLOAD, not just a green run

Coverage watchdogs historically keyed off "a `conclusion==success` coverage.yml
run on main." That proxy is blinded once a budget/timeout hit is made non-fatal: the run
concludes `success` but its C++ Cobertura upload was skipped, so coverage silently stops
while CI stays green (the 2-week silent-degradation class these watchdogs exist to catch).
`coverage-upload-watchdog.yml` is the single authority. It requires exact, non-expired
`codecov-upload-{linux,macos,python-tools}-<sha>-attempt-<n>` artifacts, using each required
job's latest actual rerun attempt so a partial "rerun failed jobs" remains valid without
accepting an older receipt for a lane that was rerun. It scans completed runs regardless of
their overall conclusion because an advisory side job can fail after valid uploads, and it
bases freshness on the oldest required receipt's creation time rather than workflow
`updated_at`. Any Actions API pagination/read failure leaves issue state unchanged instead
of producing a false stale alarm. The shared upload action creates a receipt only after
semantic report verification and successful Codecov transport. Upload-axis flags
(`os-linux`, `os-macos`, `os-windows`, `python-tools`) must keep `carryforward: false` in
`codecov.yml`; otherwise an old OS report can masquerade as current coverage. A persistently
over-budget, build-broken, or transport-broken leg therefore raises the stalled-uploads
issue instead of hiding behind run conclusion.

## MSVC-only breaks pass every blocking gate

Pulp's required gate is `macos`, the pre-push hook is macOS, and most
contributors are on macOS. So a construct Clang accepts and MSVC rejects is
invisible at every point where someone would look, and surfaces later as an
unrelated-looking Windows library failure.

`main` shipped one: `core/host/src/signal_graph.cpp` named `.custom_latency_for`
twice in one designated initializer (a merge re-appended a byte-identical
block). C++20 forbids duplicate and out-of-order designators; Clang did not care,
MSVC failed with `C7560`, and that broke `pulp-host` → `pulp-view` → every
Windows plug-in.

`tools/scripts/designated_initializer_lint.py` now catches the duplicate case.
It runs diff-scoped in `gates.sh` and its self-tests run in
`version-skill-check.yml`. It checks duplicates only — declaration ORDER needs
the struct definition and would guess wrong through macros and templates.

**A second member of the same family: raw `<windows.h>` in a public header.**
It defines `min`/`max` as macros, so any later `std::max(...)` fails to parse
with MSVC `C2589`/`C2059` — reported at the *victim* header, not the culprit.
`widget_bridge.hpp` shipped a raw include and broke consumer plug-in builds
against the installed SDK.

What makes this class nastier than the designator one is that it is **latent**.
`<windows.h>` has an include guard, so whichever header reaches it FIRST decides
whether `NOMINMAX` was set for the whole translation unit. The same raw include
compiles fine for months and then breaks when include order shifts somewhere
else entirely. Two gates miss it for different reasons: `macos` never sees
`<windows.h>`, and Pulp's own Windows lane builds the LIBRARY rather than a
downstream consumer of the installed headers — so only someone building a
plug-in against a `cmake --install`ed SDK hits it.

`tools/scripts/win32_include_lint.py` guards `core/*/include` whole-tree in
`gates.sh`. Always use `pulp/platform/win32_sane.hpp`, which pre-sets `NOMINMAX`
and `WIN32_LEAN_AND_MEAN`. Sources are deliberately out of scope: a `.cpp` that
leaks breaks only itself, immediately; a header exports the hazard.

**`gates.sh` diffs COMMITTED state.** Running it with the change still in the
working tree reports "no mapped paths touched" and passes, then `shipyard pr`
fails skill-sync on the same change seconds later. Commit first, then gate.

The general lesson for this skill: when a change is Windows-affecting, a green
`macos` gate is not evidence it builds. Compile it somewhere with MSVC. The
retained REAPER VM (see the `hosting` skill and the consumer repo's
`WINDOWS_REAPER_QEMU.md`) has Visual Studio Build Tools and answers over SSH,
which is enough for a target build without any GUI.

## A path-filtered check CANNOT be made required (it wedges every PR that misses the paths)

A workflow with `on.pull_request.paths` does not merely skip on an unrelated PR
— it never REPORTS its check at all. That is invisible while the check is
advisory, and it wedges the repo the moment the check is added to branch
protection: GitHub blocks the PR indefinitely waiting for a context that will
never arrive. There is no failure to click into, so it presents as queue lag
rather than as a misconfiguration, which is what makes it expensive to
diagnose.

`gcc-compile-gate.yml` was in exactly this shape while the decision to make the
Linux verdict binding was already taken. Adding the context as-is would have
permanently stalled every docs-only, test-only and tooling-only PR.

**Before adding ANY check to branch protection, confirm it reports on a PR that
touches none of its paths.** Observe it — the failure mode is silent, so
reasoning about it is not enough.

The fix is to move the filter inside the job: trigger on every PR, decide in a
first step whether the diff is relevant, and guard the expensive steps on that
output. Compare against the **merge base**, not the previous commit, or a PR
that edits the covered paths and then pushes an unrelated fixup will skip the
work it needs.

Measuring the cost is worth doing before assuming a required lane is expensive:
this gate's p90 (41 min) sits below the macOS lane's median (47 min), so it
usually finishes inside the required lane's shadow and adds nothing to
time-to-merge.

## `gates.sh` resolves paths from `$ROOT`, not `$REPO_ROOT`

Adding a check to `tools/scripts/gates.sh` and reaching for `$REPO_ROOT` fails
with `unbound variable` under `set -u`, and the failure looks like the new gate
itself is broken. The script sets `ROOT="$(git rev-parse --show-toplevel)"` near
the top and every existing check builds its paths from that (`DEPS_AUDIT="$ROOT/..."`).
Pass the same value through to a script that takes a root argument.

## `unbounded-wait lint` — a test wait must be able to time out

`gates.sh` runs `tools/scripts/unbounded_wait_lint.py` (also a ctest selftest,
`unbounded-wait-lint-selftest`). It fails a PR that introduces a wait a test
cannot escape:

```cpp
while (!started.load(std::memory_order_acquire)) std::this_thread::yield();
started_future.wait();
cv.wait(lock, [&] { return ready; });
```

**Why it is a CI gate and not a style note.** These waits are usually added to
*fix* a flake — a worker that had not been scheduled before a fixed budget
elapsed. They do fix it, and they replace it with something worse: if a real
regression means the worker never reaches that state, none of them ever return.
The suite does not fail, it parks, and the run ends as a job timeout with no
output. A flake at least tells you which assertion failed.

Bound it so the outcome that never arrives becomes a named failed assertion —
helpers are in `test/support/thread_progress.hpp`:

```cpp
CHECK(pulp::test::wait_for_condition([&] { return started.load(); }));
CHECK(fut.wait_for(pulp::test::kProgressDeadline) == std::future_status::ready);
CHECK(cv.wait_for(lock, std::chrono::seconds(10), [&] { return ready; }));
```

Use `CHECK`, not `REQUIRE`: a Catch2 throw at a barrier unwinds past the
worker's `join()`, and destroying a joinable thread terminates the process
instead of reporting the failure.

**It is diff-scoped on purpose.** It flags added/changed lines only. The tree
still carries a large backlog of unbounded waits (`--all` reports them); each
needs its own reproduction before it is touched, so the gate stops the
population growing rather than forcing a blind mass edit. `--all` is a
reporting mode, not a gate, and it over-reports — a worker's own
`while (!stop)` exit loop and waits bounded elsewhere both show up.

Escape a genuinely-bounded wait with `// unbounded-wait: allow <reason>`. "The
OS eventually schedules a runnable thread" is not a reason — it does not cover
the code under test never publishing the value, which is the case that hangs.

## Apple's clang ships no libFuzzer runtime — fuzzing cannot live on the macOS lanes

`libclang_rt.fuzzer_osx.a` is **absent from the Xcode toolchain** (Homebrew LLVM has it). Pulp's
sanitizer lane runs on GitHub-hosted `macos-15` with Apple clang, so **a `-fsanitize=fuzzer`
target would not build there at all** — a libFuzzer-only fuzzing design runs nowhere in this
repo's CI.

The shape that works, and the one `timeline-fuzz.yml` uses:

- **Oracles live outside the fuzzer** and are toolchain-independent, so both lanes share them.
- A **deterministic seeded replay** is the always-on lane — plain Catch2, runs on every platform,
  and a failure is reproducible from a `(seed, index)` pair rather than a corpus artifact.
- The **libFuzzer target is opt-in** behind a CMake option and **Linux-only** in CI, with a
  configure-time probe that fails naming the missing runtime instead of dying at link with an
  unresolved-symbol error that reads like a code bug.

**A fuzz job's wall clock is usually its build, not its fuzzing** — measured here at ~37k
cases/sec, so the whole PR budget is well under a second and the nightly budget can be raised
almost for free. Budget the build, not the iterations, and keep `timeout-minutes` on both jobs
plus `-max_total_time` on libFuzzer.

## A new `core/` module needs three separate registrations, revealed one at a time

Landing a new subsystem under `core/` is not one edit. Four places have to agree,
and `gates.sh` surfaces them **serially** — each run fails on the next one it
reaches, so a fix-and-rerun loop looks like whack-a-mole unless you do all four
up front:

1. `CMakeLists.txt` — the `option()` and the `add_subdirectory()`.
2. `tools/cmake/PulpInstallRules.cmake` — `PULP_SDK_TARGETS` and
   `_pulp_sdk_header_subsystems`, if the module ships in the SDK.
3. `codecov.yml` — an `individual_components` entry. Without one, every file in
   the module matches no component and is invisible to the slicing dashboard
   (`test_codecov_components.py`).
4. `ci/coverage-surfaces.yaml` — the component id in the `native-default`
   component set. A component that exists in `codecov.yml` but has no producer
   expectation and no N/A disposition fails
   `test_coverage_surface_contract.py` with "every configured component must
   have a producer expectation or an N/A disposition".

(3) and (4) are a pair and neither is inferable from the other: `codecov.yml`
says the component *exists*, `coverage-surfaces.yaml` says something is expected
to *measure* it. Fixing only the one the failure named leaves the other to fail
on the next run.

Editing either file trips a different skill-sync gate — `codecov.yml` and
`ci/coverage-surfaces.yaml` map to different skills — so expect the skill-sync
check to fire twice for what is conceptually one change.

## A job's green says nothing about a step that was skipped inside it

Reading a job's conclusion as evidence about a step inside it is wrong whenever
the step is `if:`-guarded, and it is the easiest mistake to make because the
green is real — it is just green about something else.

Concretely: `Release-path build (linux-x64)` passes while never fetching V8,
because the fetch is guarded `if: matrix.platform == 'darwin-arm64'`
(`.github/workflows/release-path-pr-gate.yml`). Reading that pass as "linux's V8
digest matches" produced a confident, wrong conclusion about a supply-chain
drift.

Ask the run about the STEP, not the job:

```sh
ghapp api "repos/Generous-Corp/pulp/actions/runs/$RUN/jobs" \
  --jq '.jobs[] | select(.name|test("<leg>")) | .steps[]
        | select(.name|test("<step>")) | "\(.conclusion)  \(.name)"'
```

It returns `skipped`, `success`, or `failure` per step. Pair it with a leg you
expect to have RUN the step: if the two legs return the same thing, the query is
wrong, not the world.

Related but not the same: `tools/scripts/required_gate_liveness.py` already
asserts every *required context* actually ran for a merged commit. That covers a
gate vanishing entirely; it does not cover a step being conditionally skipped
inside a gate that reports success.

## The fifth registration, and why `gates.sh` cannot warn you about it

The four registrations above are what a new `core/` module needs to *build and
be measured*. Shipping it needs a fifth, and this one is invisible locally:

5. `tools/scripts/release_product_matrix.json` — add the target to
   `pulp_library_stems`, alphabetically. STATIC libraries only; an INTERFACE
   library must NOT be listed.
6. `docs/status/consumption-profiles.json` — regenerate with
   `python3 tools/scripts/consumption_census.py --write`. Missing this fails the
   **required** `macos` gate on `consumption-census-drift` after a ~40-minute
   run, which is the most expensive way to discover any of these.

Six, not four, and they surface one at a time in roughly increasing cost:
`gates.sh` catches the first four in seconds, CI's workflow-lint catches the
fifth in two minutes, and the required macOS gate catches the sixth in forty.

`gates.sh` does not run `test_release_artifact_contents.py`, so a branch goes
green locally and then fails in CI's workflow-lint job with `PULP_SDK_TARGETS
archives and release_product_matrix.json drifted`. Run it yourself:

```sh
python3 tools/scripts/test_release_artifact_contents.py
```

**And treat a second, different drift from that test with suspicion on a built
checkout.** Its interface-library scan used to walk build trees, where
`test/inspector-shipping-scanner` generates a fixture `CMakeLists.txt` declaring
`pulp-format` as an INTERFACE library — so a real STATIC library looked like an
interface one and vanished from the archive set. CI never saw it (clean
checkout); it appeared only on a machine that had built once, which is exactly
the machine verifying the fix. That is fixed now, but the shape is worth
remembering: when a local run disagrees with CI, suspect the local *tree* before
the local *change*.

## A routing variable used LITERALLY in `runs-on` has no fallback, and GitHub will not tell you

Most Pulp workflows pass a `PULP_*_RUNS_ON_JSON` variable into a resolver step
that can pick a different route when the configured one cannot be served. A few
use it directly:

```yaml
runs-on: ${{ fromJSON(vars.PULP_LOCAL_MACOS_RUNS_ON_JSON || '"macos-15"') }}
```

The `||` looks like a safety net. It is not: it fires only when the variable is
**unset**. If the variable is set to a label set nothing can serve, the fallback
never runs and the job is unschedulable — and since an array `runs-on` requires
runners to carry **every** label, adding one dead label to a shared variable
kills every literal consumer of it.

GitHub does not error on an unservable label. It queues. So the lane reads as
slow, and a lane nobody watches can stay dark for weeks: the Visual Harness
macOS jobs lost 48 jobs this way (empty `runner_name`, auto-cancelled) after
`pulp-gate-fast` stopped being minted, while the ubuntu jobs in the same runs
succeeded — a control on the same instrument, which is what proved it was
routing rather than an idle pool.

Two rules:

- A lane routing literally must own its **own** variable. Never borrow another
  lane's, least of all the required gate's: an edit made for the gate then
  silently redirects lanes nobody was thinking about.
- Before adding a label to a shared routing variable, check who consumes it
  literally: `git grep -n "runs-on:.*<VAR>" .github/workflows/`. A resolver
  consumer tolerates a dead label; a literal one does not.

Prefer an unset variable with a GitHub-hosted default for advisory,
low-frequency lanes. Staying off the self-hosted fleet is a stronger guarantee
than a low fleet priority, because the lane cannot contend with the required
gate at all.

## `shipyard pr` can push your branch and then refuse the handoff

Since 2026-08-31 the steward handoff validates `--workstream-id` against a
`^GEN-[1-9][0-9]*$` shape, and its legacy escape hatch cannot fire in this repo.
Two guards combine:

- `shipyard pr` synthesizes the fallback id as `{repo}#{pr}` **preserving case**,
  while the hatch requires an **already-lowercase** slug. This repo is
  `Generous-Corp/pulp`, so the hatch is false by construction.
- Even lowercased, the hatch is refused once an agent route is detected, and
  `CLAUDE_CODE_SESSION_ID` / `CODEX_THREAD_ID` are set in every agent shell.

The failure is deterministic, not flaky, and it happens **after the branch is
pushed** — leaving a real PR with no `shipyard:managed` label and no local
ship-state, which makes it invisible to `shipyard status` and to the queue tick.

**Do not re-run `shipyard pr`** when you hit it; that risks a duplicate PR.
Find the PR that was created and adopt it:

```sh
ghapp pr list --repo Generous-Corp/pulp --head <your-branch> --state all
shipyard ship --pr <n>
```

`[merge_steward] auto_handoff` in `.shipyard/config.toml` is currently **paused**
for this reason, so the auto path no longer runs at all. While it is paused, do
**not** pass `--workstream-id` — an explicit id still opts in, and a fleet where
some PRs are managed and most are not is worse than either state alone.
