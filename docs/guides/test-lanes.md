# Test lanes — what runs where, and why

Pulp runs its test suite in a few distinct **lanes**. Knowing which lane a test
lands in — and how to route a new test — is the difference between a fast,
trustworthy required gate and one that flakes on unrelated work. This is the
single source of truth for that model.

## The lanes

| Lane | Trigger | Gates the PR? | Builds examples? | What it runs |
|------|---------|---------------|------------------|--------------|
| **Required core gate** (`macos`) | every PR | **yes** (blocking) | Actions: no; Shipyard: yes until promotion | all core tests **except** `validation` and `slow` labels; `--repeat until-pass:2` |
| **Example-validation** (`example-validation`) | PRs touching `examples/**`, state/format headers, core CMake, or shared dependency infrastructure | advisory pending promotion (see status below) | yes — Linux + macOS | Linux compiles every example artifact; hosted macOS runs auval + built-in CLAP dlopen checks; pluginval/clap-validator require an operator-dispatched advisory image |
| **API contracts** (`api-contracts`) | every PR + every merge group | advisory pending promotion (see below) | no | the Doxygen strict pass over the catalogued public headers, ~3 s of work |
| **Nightly full build** | schedule (nightly) | no — **informational** | yes | everything, including `validation` + `slow`; results eyeballed, build failures file an issue |
| **cross-platform-check** | per PR (Linux/Windows) | advisory | no | core tests, excludes `validation` + `slow` |

The required gate is **serialized on self-hosted macOS runners** and takes
~30 min. Keeping it lean is why the two label groups below are excluded from it.

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
  ~25-30 min iOS try-compile). **Excluded from the required gate**; run nightly.
- **no special label** — a normal unit/integration test. Runs on the **required
  gate**. This is where the vast majority of tests belong.

The required gate excludes both groups with one CTest filter,
`--label-exclude "validation|slow"` — the same filter `build.yml`'s PR ctest and
`cross-platform-check.yml` already use. It is set in
[`.shipyard/config.toml`](../../.shipyard/config.toml) (`[validation.default]`,
`test =`).

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

## Adding a test — where will it land?

- **A core unit/integration test** → add it with no special label. It runs on the
  required gate. Keep it fast (< a few seconds) and non-flaky.
- **A new example plugin** → its `clap-dlopen`/`auval`/`pluginval` validators
  should carry `LABELS "validation;<format>"` (match the existing examples). That
  automatically keeps them off the required gate and onto the example-validation
  lane. Give `pluginval` a `TIMEOUT` comfortably above its real runtime (e.g.
  `120` — SuperConvolver runs ~25-30 s; 30 s was too tight and flaked).
- **A genuinely long test** (minutes) → `LABELS "slow"`, and make sure something
  (nightly, or a dedicated lane) actually runs it — do **not** rely on the
  informational nightly alone if it must be enforced.

## The trap to avoid

Labeling a test `slow` or `validation` **removes it from the required gate**. If
nothing else runs it as a *gate*, you have silently disabled it — the nightly
runs it but does **not** fail on it. Before moving a test off the required gate,
make sure it is enforced somewhere. During the staged rollout,
`example-validation` reports example-validator failures but remains advisory;
promotion to a required context is what turns that signal into enforcement.
Use a dedicated gating lane for anything that must block before then. "It runs
nightly" is a backstop, not enforcement.
