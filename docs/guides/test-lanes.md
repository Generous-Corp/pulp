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
