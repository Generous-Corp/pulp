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

## Runner timing metrics

When asked whether Pulp's local runners are fast, stuck, regressing, or worth
monitoring, query Shipyard's metrics surface before guessing. Pulp does not
mirror these records into `pulp` CLI or `pulp-mcp`; Shipyard is the metrics
store and tartci is an optional VM runtime emitter.

This metrics surface requires a Shipyard build that includes the
`shipyard metrics` subcommand. Pulp's current pinned source-checkout Shipyard in
`tools/shipyard.toml` is `v0.68.0`, which does not include it yet. If a checkout
only has the pinned binary, use a newer Shipyard binary or source checkout for
the optional metrics workflow, or skip metrics and inspect live jobs directly.

Use these commands as the normal agent loop:

```bash
shipyard metrics import github --repo danielraffel/pulp --limit 50 --json
tartci runtime export --repo danielraffel/pulp --since-days 14 \
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
   re-dispatch the checks.
3. **Only THEN consider capacity — and verify, don't assume.** The required
   `macos` gate runs on the **local self-hosted Mac Studios** (`pulp-studio-01/02/03`,
   + the M5 overflow), which are usually idle. Confirm with:
   ```bash
   ghapp api repos/danielraffel/pulp/actions/runners \
     | python3 -c "import sys,json;[print(r['name'],r['status'],'busy='+str(r['busy'])) for r in json.load(sys.stdin)['runners']]"
   ```
   If the Studios show `busy=False`, the pool is NOT saturated — say so. What DOES
   queue independently is the **GitHub-hosted advisory lanes** (Linux, Windows,
   sanitizers, coverage, android) on GitHub's shared pool; those are advisory, not
   the required gate, so a long queue there does not block merge.
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

## GitHub workflow gotchas

- **Every `on: schedule` workflow must carry the fork guard.** When someone forks
  the repo, GitHub copies all workflows and runs the scheduled ones on the fork's
  default branch — then emails the fork owner whenever one *fails*, which our
  monitors reliably do on a fork (they probe this repo's state or use secrets the
  fork lacks). So each entry job (a job with no `needs:` — dependents cascade-skip)
  of a scheduled workflow gets:

  ```yaml
  jobs:
    check:
      if: github.event_name != 'schedule' || github.repository == 'danielraffel/pulp'
  ```

  compose it with an existing condition as
  `if: (github.event_name != 'schedule' || github.repository == 'danielraffel/pulp') && (<existing>)`.
  It only suppresses the **schedule** event on forks — `push` / `pull_request` /
  `workflow_dispatch` are untouched, so PRs to this repo (which run in this repo's
  context) and manual dispatches behave exactly as before. A workflow that
  *should* run on forks' schedules opts out with a top-of-file
  `# fork-guard-exempt: <reason>` comment. This is **enforced**:
  `tools/scripts/scheduled_workflow_fork_guard_check.py` runs in `gates.sh`, the
  pre-push hook, and `workflow-lint.yml`, so a new scheduled workflow missing the
  guard fails the PR. Add the guard when you add the workflow.
- **Codecov "total lines/files dropped" is usually upload starvation, not config drift.** Two distinct guard layers exist and they catch different things: `test_codecov_components.py` / `test_codecov_config.py` (PR-gate) guard the **codecov.yml mapping** (a subsystem matching no component → invisible); `coverage-upload-watchdog.yml` (hourly, main) guards the **outcome** — "main had a *successful* Coverage run in the last N hours." The watchdog exists because the dashboard degrades silently when fresh complete uploads stop arriving, from causes the config gate can't see: coverage runs cancelled by `stale-run-reaper.yml` when a CI dispatch throttle makes them queue past the cutoff (the 2026-06-28 incident — 8 consecutive main Coverage runs cancelled), an `llvm-cov -object`-set regression (a `libpulp-*.a` drops out → a whole subsystem vanishes while `lines-valid` stays > 0, which `verify_cobertura_xml.py`'s `lines-valid > 0` check cannot catch), or Codecov's `after_n_builds` waiting forever on a missing per-OS leg. When triaging a coverage-surface complaint: first check `gh run list --workflow coverage.yml --branch main` for recent **successful** runs (cancelled ≠ uploaded); only then suspect codecov.yml. Note the config gate is **advisory** (not in branch protection), so a fleet-auto-merged PR can land config drift despite a red gate — the watchdog is the main-branch backstop.
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
  (set `CHROME_PATH` or pass `--browser`).
- **`intent-bump-on-merge.yml` is the merge-time half of the version-bump
  intent-trailer model — and it ships DORMANT.** It exists to kill the
  version-bump merge treadmill (PRs editing `CMakeLists` VERSION /
  `plugin.json` / `marketplace.json` re-conflict every time main advances). The
  endgame: a PR declares `Version-Bump: <surface>=<level>` and touches NO
  version files, and this workflow assigns the exact number after merge from
  main's current version via `tools/scripts/apply_intent_bump.py`. **Phase 1
  (current): no-op.** Nothing emits intent trailers yet (Shipyard still file-
  bumps on the PR side; `version-skill-check.yml` still runs WITHOUT
  `--accept-intent-trailers`), so every run finds no trailer and exits clean.
  Two things must be verified before the **phase-2** flip (a separate, reviewed
  change): (1) `RELEASE_BOT_TOKEN` can push a *commit* to protected `main`
  (it already pushes tags from `auto-release.yml`; a commit needs the bot on the
  branch-protection bypass list), and (2) the `Version-Bump:` trailer survives
  squash-merge into main's commit message. The workflow has a recursion guard
  (skips its own `chore: bump versions` commit) and a `concurrency` group so
  near-simultaneous merges bump the version line one at a time. The
  `chore: bump versions` commit it pushes triggers `auto-release.yml` exactly
  like a PR-side bump.
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
- The required `macos` alias in `.github/workflows/build.yml` mirrors the
  macOS matrix leg by polling the Actions jobs API. Keep that poll retry-safe:
  API failures or malformed JSON must log and continue the loop, not trip
  `set -euo pipefail` before the macOS leg has a chance to report.
- `.github/workflows/release-dry-run.yml` (P9-2, #2576) exercises the release
  build → `package_cli.py` → `pulp ship appcast` chain on a synthetic version
  (`0.0.0-dryrun`) WITHOUT publishing — no GitHub release, no signing/notarize,
  no appcast upload; artifacts are throwaway. It's additive (does not touch
  `release-cli.yml` / `sign-and-release.yml`) and runs weekly + on demand, so a
  build/packaging/appcast-generation regression surfaces BEFORE a real tag.
  Keep it credential-free (notarize/sign stay in the real path) so it can run
  on a schedule without secrets.
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
  watchdog trackers daily and closes any whose version is released or superseded
  — the existing watchdogs only auto-close inside a recent window, so historical
  per-version trackers orphaned (334 had accumulated). It only matches the exact
  auto-generated tracker titles and only closes objectively-resolved ones.
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

## Current Build-and-Test routing

As of the 2026-05-20 classify gate, `build.yml` runs a cheap
`classify` job before allocating the native matrix. PRs that touch only
skip-safe docs/planning paths report the native aliases green without
running macOS/Linux/Windows builds; PRs that touch C++/Swift/CMake or
workflow inputs must run the native matrix. A code PR whose native build
is skipped is a CI bug.

`workflow_dispatch` defaults `runner_provider` to `github-hosted`, not
Namespace; do not dispatch with `runner_provider=namespace`. Linux/Windows
use GitHub-hosted runners by default. macOS routes through the self-hosted
`pulp-build` pool (`pulp-m1-01`, `pulp-m1-02`) via
`PULP_LOCAL_MACOS_RUNS_ON_JSON`; repository variables control any overflow.

Do not push empty commits just to churn queued macOS checks. Cancel
superseded SHAs, rebase or push only when a PR needs current `main`, and
wait unless a check has actually failed.

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

The **os-windows** coverage leg is best-effort. The instrumented MSVC build +
~9k instrumented tests + `llvm-cov` over 1000+ objects exceeds the 150-min job
cap on GitHub-hosted `windows-latest` (it is ~1h on Linux/macOS), and the
staleness watchdog keys off a *successful run*, not per-OS Codecov flags — so a
red Windows leg would otherwise keep a healthy full run red forever. **The
subtle trap (verified by canary):** job-level `continue-on-error` does NOT
neutralize a `timeout-minutes` *cancellation* — a cancelled job still makes the
run conclude `cancelled`. It DOES neutralize a normal job *failure*. So the
coverage suite step self-terminates at an **internal budget (135 min) below the
job cap**, turning the would-be cancellation into a normal non-zero exit that
the job-level `continue-on-error: matrix.os=='windows'` then absorbs → the run
concludes `success`. Don't "simplify" this to bare `continue-on-error`; it will
silently stop closing the watchdog. **And the watchdog that enforces the budget
must separate its steps with `;`, NOT `&&`** — if the kill is `&&`-gated behind
a `: > marker` write (which can fail on a Windows `RUNNER_TEMP` backslash path),
a failed marker write short-circuits the chain and the suite is never killed,
so the job hits the 150-min cap and is *cancelled* anyway. The kill is
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
   **Auto-supersede behavior**: when a new SDK tag is created, the workflow
   cancels any in-flight `release-cli.yml` / `sign-and-release.yml` runs for
   strictly-older SemVer tags and deletes their draft releases. This prevents
   the 2026-05-15 v0.101.x saga from recurring (5 bumps each queueing a full
   macos-15 darwin-arm64 build, latest waiting hours behind already-obsolete
   builds). Opt out via `vars.PULP_RELEASE_MODE=land-all` (repo-wide) or
   `Release-Supersede: skip reason="..."` trailer on the merging commit
   (per-release). See #2076 + the `release-draft-stuck-check.yml` newest-SemVer
   skip for the watchdog side of the cleanup.

Never run `gh pr create` + `shipyard ship` separately for a normal ship
cycle. Never invoke the two version/skill scripts by hand — `shipyard pr`
wires them together with the right flags.

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
ghapp pr merge <PR> --auto --merge      # merge commit, NOT --squash
```

GitHub then merges the moment required checks (`macos` + `Enforce version &
skill sync`) go green, regardless of whether `shipyard pr`, cmux, or this
session survive. Use `--merge` (merge commit), never `--squash`: a squash folds
the `chore: bump versions` commit into the PR-title commit, which trips the
auto-release watchdog into a false "merged without bump." It is safe alongside
`shipyard pr` — whichever merges first wins; the other no-ops on already-merged.
(The Shipyard repo has no branch protection, so GitHub auto-merge is
unavailable there; the host-side queue janitor below covers it.)

**Host-side backstop (both repos + orphan reaping):** a launchd queue-tick on
each CI Mac (`tartci` `scripts/shipyard_queue_tick.sh`) periodically drives
in-flight ship-state to completion via shipyard's own fail-closed `auto-merge`
and reaps records whose PR GitHub reports merged/closed — independent of any
session. Reap a stale local pile by hand with `shipyard ship-state list` →
`shipyard ship-state discard <pr>` for each merged/closed PR (never discard an
OPEN one). Full design: pulp
`planning/2026-06-30-ship-queue-resilience-design.md`.

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
period (see danielraffel/pulp#120).

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

```bash
# Primary: Shipyard
shipyard run                              # validate current branch
shipyard pr                               # create, track, validate, and merge on green
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

`build.yml`'s `resolve-provider` feeds the Linux leg selector from (in
precedence): the `linux_runner_selector_json` workflow_dispatch input →
`PULP_LOCAL_LINUX_RUNS_ON_JSON` repo var → `''` (github-hosted, the default).
Set the repo var (e.g. `["self-hosted","Linux","ARM64","pulp-build-linux"]`) to
route that leg to the self-hosted Linux pool, served by
`tools/ci/tart-runner-linux.sh` (see the `tart-ci` skill) and toggled in the
Shipyard macOS GUI. The explicit selector has NO capacity fallback, so only set
it when the pool reliably serves that lane — else legs route to a label with no
online runner and queue.

Windows is intentionally different. The required `Windows (x64)` gate must stay
on real GitHub-hosted `windows-latest` unless a local x64 lane is explicitly
proven and promoted. The current local Windows pool is Windows ARM64 on QEMU
(`qemu-system-aarch64`): it can maybe smoke x64 via Windows-on-ARM translation,
but it is not the authoritative Intel/x64 gate. Do not wire
`PULP_LOCAL_WINDOWS_RUNS_ON_JSON` into the required Build-and-Test Windows x64
matrix leg; use a separate/dedicated label for any local Windows ARM64 smoke.

### macOS runner routing (current)

As of 2026-05-20, Namespace macOS routing is disabled for cost control.
The required macOS PR gate runs through the self-hosted `pulp-build`
runners (`pulp-m1-01`, `pulp-m1-02`) via
`PULP_LOCAL_MACOS_RUNS_ON_JSON`. Shipyard's `mac` target must stay
`backend = "local"`; do not flip it to `cloud` unless the operator is
explicitly re-enabling Namespace.

**Tart VM ephemeral runners** are graduating into the `pulp-build` pool:
each CI job runs in a throwaway clone of the `pulp-build-runner` golden,
then the VM is destroyed (no build-dir churn / state drift). Driven by
`tools/ci/tart-runner.sh`, persisted via
`tools/launchd/pulp-tart-runner.plist.template` (needs Full Disk Access
for the `/Volumes` VM store; launchd doesn't expand `$HOME`). Add VM
runners additively while the bare-metal runners remain the safety net;
never leave the required label with zero online runners. Full guide:
the `tart-ci` skill. NOTE: shipyard `backend = "local"` validates in the
editing checkout, which false-fails via Debug-over-Release build-dir
churn + host-keychain prompts — Phase 2 points that validation at the
VM lane instead.

Linux and Windows CI legs default to GitHub-hosted runners. The legacy
Shipyard SSH targets (`ubuntu`, `windows`) are optional local validation
hosts and may be unreachable; use `--skip-target ubuntu --skip-target
windows` when shipping a macOS/local-only tranche and the PR has already
run the appropriate focused local tests.

If a PR's macOS check is queued, first verify whether it is actually
waiting on the self-hosted runner pool before taking action:

```bash
gh api repos/danielraffel/pulp/actions/runners --jq \
  '.runners[] | select(.labels[].name == "pulp-build") |
   {name,status,busy,labels:[.labels[].name]}'
```

Do not rerun or empty-commit just to "unstick" a queued macOS job. Rebase
only when the branch needs current `main` fixes, and cancel stale
pre-cutover Namespace runs instead of rerunning them.

The required PR lane must not let a single flaky or environment-specific
macOS test wedge unrelated PRs behind the serialized local runner pool. If
current `main` has known macOS-only failures, quarantine the exact test names
with a macOS-only `ctest --exclude-regex` in `build.yml`, keep Linux/Windows
coverage intact, and open/follow a targeted fix. Do not hide failures that
reproduce cross-platform or that are caused by the branch being shipped.

**Real-time-thread teardown hangs need a `PROCESSORS` reservation, not an
exclude.** A test that opens the real CoreAudio device (anything constructing a
`StandaloneApp` or calling `AudioSystem::create_device()` + `start()`) tears it
down via `AudioOutputUnitStop` / `AudioUnitUninitialize`, which **block until
the real-time I/O thread observes the request**. Under `ctest -j8` full-suite
load that RT thread is CPU-starved, so teardown hangs to the 120s timeout — a
flake that reproduces **only** in the full run on a saturated runner, never in
isolation or any concurrent subset (so don't waste time bisecting subsets). Fix
it at the scheduler: `pulp_add_test_suite(... PROPERTIES PROCESSORS 8)` (== the
CI `-j`) makes ctest run the suite alone so the RT thread gets the machine.
Prefer this over an exclude — it keeps the test enabled everywhere. (Adding a
shared `RESOURCE_LOCK` does NOT fix it: serializing the audio tests among
themselves still leaves the other ~8 `-j8` tests starving the RT thread.)

The required `macos` alias should not `need` the whole cross-platform build
matrix. It should depend only on cheap setup/classification jobs and poll the
macOS matrix leg by name, then exit as soon as that leg reports. Otherwise
advisory Linux/Windows jobs can keep a green macOS leg from satisfying branch
protection.

**Flaky required-leg wedge + the rerun lock (recovery).** Even when the `macos`
alias reports its failure promptly, a *flaky* failure on the required leg wedges
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
`gh pr merge <pr> --squash --auto`, then **`shipyard rescue <pr> --rerun-failed`**.
`rescue` cancels stuck runs and, with `--rerun-failed`, re-dispatches completed
failed/cancelled runs — "e.g. a flaky required leg" (its own help) — re-resolving
the provider so the rerun lands local-first on the idle Studios; the armed
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

### Gotcha: the macOS overflow busy-probe must count only *running* M1 legs (#2467)

`build.yml`'s `resolve-provider` job has an inline-Python "busy probe"
(`_count_busy_local_mac_runners`) that decides whether a PR's macOS leg
runs on the local M1s or overflows to github-hosted `macos-15`. The rule:
`BUSY >= LOCAL_MAC_OVERFLOW_THRESHOLD` (default 2) → overflow.

**The probe must count only macOS Build-and-Test jobs that are RIGHT NOW
`status == "in_progress"` on a local M1** — a job whose `status` is
`in_progress` *and* whose `labels` array contains the local self-hosted
label (`PULP_LOCAL_MAC_RUNNER_LABEL`, default `sanitizer`). Everything
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
Branch protection's required `macos` check accepts the latest
same-named check from either workflow, so `build-macos.yml`'s `macos`
job supersedes the matrix's `macos` job when fresher.

The macOS lane configures with `-DPULP_LOTTIE=ON` so the opt-in skottie
render path (LottieAnimation → SkiaCanvas) gets real CI coverage — it is
the only lane that exercises it, since `PULP_LOTTIE` defaults OFF. This
cannot break the gate: `core/canvas/CMakeLists.txt` runs a configure-time
try-link and auto-disables Lottie on any Skia bundle that can't link
skottie (needs SkJSON + skresources, bundled from Skia chrome/m151 onward).
It only enlarges the test binaries, never shipped plugins.

```bash
# Move a PR's macOS leg to a different runner, without touching Linux/Windows:
pulp macos retarget --pr <N> --to <local|namespace|github-hosted>

# See where the latest macOS check landed and its state:
pulp macos status --pr <N>
```

`retarget` cancels any in-flight macOS-bearing workflow_runs for the
PR (from both `build.yml` and `build-macos.yml`), then fires a fresh
`gh workflow run build-macos.yml` with the chosen runner.

**When this is the right tool:**

- Local Mac just freed up and a PR is sitting queued on GH-hosted →
  `pulp macos retarget --pr N --to local` claws it back to local.
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

Polling cadence: 30s. When there is **free macOS capacity** AND a BAT
run's macOS job has cloud labels (`macos-15` or `nscloud-*` /
`namespace-profile-*`) AND hasn't started yet, the watcher invokes
`pulp macos retarget --pr N --to local`.

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

`build.yml` excludes CTest labels matching `slow` on both `pull_request`
events and `workflow_dispatch` because Shipyard PR validation dispatches
the Build-and-Test workflow manually. Preserve that split: slow configure
smokes like `cmake-ios-auv3-configure` can legitimately take many minutes
on the single self-hosted Mac and should not block PR validation after the
classify gate has already decided a native build is needed.

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
`coverage` matrix (150 min/leg); docs-only PRs also skip
`android-kotlin-coverage`. The Android Kotlin lane is additionally
guarded with `github.event_name != 'pull_request'`, so it never allocates
a runner on PRs. No coverage runner is allocated on a docs PR.

`coverage-diff-gate` is a **REQUIRED branch-protection check** — break it
and docs PRs get blocked OR coverage silently passes when it shouldn't.
It has `needs: [classify, coverage]` + `if: always() && github.event_name
== 'pull_request'`, and its first step ("Evaluate coverage gate
preconditions", id `gate_preconditions`) implements **exactly three
terminal cases**:

1. `classify.result != success` → `exit 1` (fail RED — the
   `native_build_required` output is untrusted; fail the required gate
   closed rather than guessing).
2. `native_build_required != 'true'` → `exit 0` (pass GREEN — the
   classifier approved a skip-safe PR; no coverage is owed).
3. `native_build_required == 'true'` AND `coverage.result != success`
   → `exit 1` (fail RED — the matrix was supposed to run and didn't).

Only when case 3's preconditions are met (native required + coverage
succeeded) does the step fall through; it sets a step output
`native_required` and every real diff-cover step below carries
`if: steps.gate_preconditions.outputs.native_required == 'true'`.

Critical: once coverage is gated, the OLD "all coverage XML missing →
pass GREEN" fallback is **unsafe** and was removed. For
`native_required == 'true'`, a missing/empty merged Cobertura XML now
hard-fails (`exit 1`) in both the merge step (`merge_cobertura` exit-2
all-missing sentinel → `exit 1`, not `exit 0`) and the `Run diff-cover`
step. Only the classifier-approved skip-safe path (case 2) gets an
exit-0 green with no coverage report. If you ever loosen this, you
re-open a silent bypass of the required 75% diff-coverage floor.

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

# Verify GitHub Actions runner routing. Namespace handles macOS PR work
# (2026-05-18 re-commissioning); GHA-hosted handles Linux+Windows.
gh variable list -R danielraffel/pulp | grep -q '^PULP_DEFAULT_RUNNER_PROVIDER[[:space:]]*github-hosted' || echo "WARNING: PULP_DEFAULT_RUNNER_PROVIDER should be github-hosted"
gh variable list -R danielraffel/pulp | grep -q '^PULP_LOCAL_MACOS_RUNS_ON_JSON' || echo "WARNING: PULP_LOCAL_MACOS_RUNS_ON_JSON is missing; macOS build will use hosted macos-15"
gh variable list -R danielraffel/pulp | grep -q '^PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON' || echo "WARNING: PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON is missing; macOS overflow will not reach Namespace"
gh variable list -R danielraffel/pulp | grep -q '^PULP_LOCAL_MAC_OVERFLOW_THRESHOLD[[:space:]]*0' || echo "INFO: PULP_LOCAL_MAC_OVERFLOW_THRESHOLD is non-zero; macOS leg will prefer the local self-hosted Mac before overflowing to Namespace"
```

If `local_ci.py` doesn't exist, the user likely has an older checkout. Tell them to pull latest main.

## Visual Harness Container

`ci/visual-harness.Dockerfile` and `.github/workflows/visual-harness.yml`
provide the deterministic visual-harness smoke environment. The Docker image
downloads the pinned Skia `chrome/m151` Linux release asset (from the
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
- `external/skia-build/VERSION.md` digest table (a *fetch cache-skip oracle* —
  `fetch_skia_for_release.py` trusts it to skip downloads) and `DEPENDENCIES.md`
  Skia/Dawn/V8 version cells ← `tools/scripts/check_manifest_mirrors.py`
- `tools/harness/visual/pins.py` ← `test_skia_determinism.py`

The `macOS local smoke` job resolves `runs-on` from
`macOS local smoke` job resolves `runs-on` from
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
macOS uses the self-hosted `pulp-build` runners. Namespace is not a
default PR-validation backend after the 2026-05-20 cost cutover. The
required branch-protection check on `main` is the `macos` alias job —
that name MUST NOT be renamed.

Routing variables (verify before debugging "stuck" macOS PRs):
- `PULP_DEFAULT_RUNNER_PROVIDER = github-hosted` (Linux/Windows default)
- `PULP_LOCAL_MACOS_RUNS_ON_JSON = ["self-hosted","pulp-build"]` (local mac selector)
- `PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON = local-only` (no Namespace overflow)
- `PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON` should be unset unless the operator is deliberately testing Namespace

`shipyard pr` is the authoritative ship path. Do NOT push empty commits to
retrigger a slow macOS check. If macOS is queued >45 min, check
`gh api repos/danielraffel/pulp/actions/runners` first.

## Pre-push rebase hygiene

The macOS runner pool has changed several times. Before pushing a branch
whose CI touches a macOS leg, rebase onto current `main` so it picks up
the latest workflow fixes and runner labels. Skia-sensitive tests should
still be gated on `PULP_HAS_SKIA`; different runner images may or may not
have the bundled Skia archive available.

Before pushing ANY branch whose CI touches a macOS leg
(`Build and Test`, `Sanitizer Tests`, `Coverage`, `Visual Harness`,
`Release-path PR gate`, `macos-15`, the `macos` alias, etc.):

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
gh api -X POST repos/danielraffel/pulp/actions/runs/<RUN_ID>/rerun
# or to rerun only failed jobs:
gh api -X POST repos/danielraffel/pulp/actions/runs/<RUN_ID>/rerun-failed-jobs
```

That keeps your SHA + queue position, only re-fires the failed legs.

### Display-name vs runner-name gotcha

The macOS matrix job display name includes a provider suffix, but branch
protection gates on the stable `macos` alias job. Do NOT rename the alias
while debugging runner routing.

### Verifying your branch isn't burning macOS runner time

Each failed macOS leg consumes one of the scarce self-hosted runners and
queues every other PR behind your run. Before broadcasting "my CI is
stuck", confirm:

```bash
# Are the pulp-build runners online and busy?
gh api repos/danielraffel/pulp/actions/runners --jq \
  '.runners[] | select(.labels[].name == "pulp-build")
   | "\(.name) status=\(.status) busy=\(.busy)"'
```

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
gh api -X POST repos/danielraffel/pulp/actions/runs/<RUN_ID>/cancel
```

The default chain (`.github/workflows/build.yml` `resolve-provider` job):

```yaml
REQUESTED_PROVIDER:
  ${{ inputs.runner_provider             # explicit workflow_dispatch input
   || vars.PULP_DEFAULT_RUNNER_PROVIDER  # repo-level override
   || 'github-hosted' }}                 # hardcoded fallback
```

Priority order:
1. **macOS local GitHub runner** — `build.yml` reads `PULP_LOCAL_MACOS_RUNS_ON_JSON` into `EXPLICIT_MACOS_RUNNER_SELECTOR_JSON`; with the usual value `["self-hosted","sanitizer"]`, the macOS build uses Daniels-MacBook-Pro.
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
  gh workflow run build.yml --repo danielraffel/pulp --ref <branch>
  ```
  Passing `-f runner_provider=namespace` will fail until the
  `PULP_NAMESPACE_BUILD_*_RUNS_ON_JSON` repo variables are restored.
- **Pin macOS to a local runner selector**: set `PULP_LOCAL_MACOS_RUNS_ON_JSON` at the repo level, or pass `macos_runner_selector_json` on a manual dispatch. Keep the selector compatible with the runner labels (`self-hosted`, `sanitizer`).
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
- `linux` — Linux (x64) build+test leg of `.github/workflows/build.yml`
- `windows` — Windows (x64) build+test leg of `.github/workflows/build.yml`
- `Enforce version & skill sync` — `.github/workflows/version-skill-check.yml`

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
on PR (when `.github/rulesets/**` changes) and weekly on cron. It fetches
the live ruleset via `gh api /repos/{owner}/{repo}/rulesets` and diffs
against the checked-in JSON. PR runs post/update a single comment; the
cron job fails loudly on drift so it shows up as a red check on `main`.

**Making a change to required checks:** always edit the JSON first, open
a PR, and let the drift-check workflow confirm the plan. Then mirror the
change in the GitHub ruleset UI (or reapply via `gh api PUT`). Never edit
the live ruleset in isolation — the next scheduled drift run will fail.

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

- `.github/workflows/version-skill-check.yml` — runs `tools/scripts/version_bump_check.py`, `tools/scripts/skill_sync_check.py`, `tools/scripts/compat_sync_check.py`, `tools/scripts/compat_aggregate.py check`, `tools/scripts/node_abi_gate.py`, and `tools/scripts/hotspot_size_guard.py` in `--mode=report`. Failure blocks merge. No bypass except the commit trailers documented in `docs/guides/versioning.md` and `docs/guides/compat-sync.md`; the node ABI gate is fixed by preserving existing virtual declarations or appending new virtuals at the tail, and the hotspot-size guard is fixed by shrinking the tracked file, moving code behind a split, or intentionally raising the baseline in `tools/scripts/hotspot_size_guard.json` with the reason in the PR.
- `.shipyard/config.toml` → `[validation.gates]` pipeline — same scripts via `shipyard run --pipeline gates`. Runs with `PULP_ENFORCE_PREPUSH=1` so warnings become errors.

Locally:

- `.githooks/pre-push` (install via `tools/scripts/install-githooks.sh`) runs the same fast scripts, including the compat-sync, compat-aggregate, node ABI, and hotspot-size gates, enforcing by default. `PULP_DISABLE_PREPUSH_GATES=1` demotes the fast gates to advisory; `PULP_SKIP_PREPUSH=1` is the single-push emergency bypass.
- `tools/scripts/docs_noise_lint.py` (pre-push only, report mode) — scans changed/added lines for transient breadcrumbs across the markdown default scope (docs/reference + skills) **and source comments + test tags** under `core/examples/tools/test/apple/inspect/ship`. Source is diff-scoped only (never `--all`), so the historical backlog never blocks; it is comment-aware (only `//`, `/* */`, `#` comment text + string-literal Catch2 `[tag]`s, never code). Escape a legitimate line with an inline `docs-noise-lint: skip <reason>` comment. Full guidance on *writing* durable comments lives in the `code-comments` skill.
- `tools/scripts/gates.sh` — on-demand runner for JUST the cheap gates (skill-sync + version-bump + compat-sync + compat-aggregate + node-ABI + hotspot-size + deps-audit + codecov-config). Runs in ~1 second, exits non-zero on any hard failure with a one-liner pointing at the right surgical bypass. The codecov-config gate is a *global invariant* (not diff-scoped): it runs the `test_codecov_config.py` / `test_codecov_components.py` contract tests so a new `core/<sub>/` subsystem can't land without a matching `codecov.yml` flag+component (graph/scene drifted onto main exactly this way), no platform subtree gets double-counted, and `codecov.yml`'s `ignore:` stays mirrored to `coverage_config.json`'s `diff_cover_excludes`. Needs PyYAML locally; skips cleanly if absent (the CI `codecov-config-validation` job in `coverage.yml` is the authoritative gate). Use it before `git push` when you've made changes that might touch mapped paths but you don't want to wait for the pre-push hook OR the 20-minute CI roundtrip. Independent of the git hook (no install step needed). Named to align with Shipyard's planned `shipyard gates` subcommand (see `planning/2026-05-19-shipyard-preflight-upstream-proposal.md`); avoids collision with Shipyard's existing `preflight` namespace (SSH backend reachability probes).

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

`tools/cli/kit_commands.cpp` is frozen at its pre-split 3,927-line baseline.
When extracting kit-command modules, follow `tools/cli/KIT_COMMANDS_MODULE_MAP.md`
and lower that ceiling to the new exact LOC in the same PR that moves code out.
`tools/cli/cli_common.cpp` follows the same ratchet rule: when shared helper
logic moves into a focused translation unit such as `cli_delegate.cpp`, lower
the `cli_common.cpp` ceiling in `hotspot_size_guard.json` in that same PR so the
extraction cannot quietly regrow.

**Auto-release:** `.github/workflows/auto-release.yml` fires on push to `main`. It diffs the two version-bearing files (`CMakeLists.txt` project version, `.claude-plugin/plugin.json` version) against the previous push range and creates the corresponding `v<x.y.z>` or `plugin-v<x.y.z>` tag. The existing tag-triggered release workflows (`release-cli.yml`, `sign-and-release.yml`) then build and publish. `Release: skip reason="..."` on the merging commit suppresses the tag.

**fix/feat-needs-bump (issue #1009):** the version-skill-check workflow ALSO runs `version_bump_check.py --require-bump-for-fix-feat` on `pull_request` events. If the PR title matches `^(fix|feat)(\([^)]*\))?!?:\s` (Conventional Commits user-facing prefix), the diff range MUST contain either a commit subject `chore: bump versions` OR a top-level `Version-Bump: skip reason="..."` trailer (with non-empty reason — bare `skip` is rejected). This is the structural fix for the 2026-04-30 incident (PR #1008) where a `fix(view):` merged via `gh pr merge` after a force-push race with `shipyard pr` and stranded the change on main. Auto-release.yml has a matching backstop step (`Stranded fix/feat detector`) that emits a `::warning::` annotation and opens a `release-stuck`-labelled tracking issue when the merge slips through to push. Branch protection on `main` requiring the `Enforce version & skill sync` check would close the loop entirely — see `docs/guides/release-watchdog.md` for the recommended setup. Bypass the check on a one-off basis with `Version-Bump: skip reason="..."` on any commit in the range; this is intentionally a different trailer from `Release: skip` so a "don't tag this release" decision doesn't silently imply "this fix doesn't need a bump."

**Exact bump-marker format:** for `fix:` / `feat:` PR titles, the accepted
commit subject prefixes are exactly `chore: bump versions` (canonical) and
`chore(versions): bump` (legacy). A manually authored subject such as
`chore: bump SDK to v0.78.4` does not satisfy the gate, even if the
version files and changelog are correctly edited. Let `shipyard pr` create
the bump commit when possible; if you need to repair it manually, use the
canonical subject `chore: bump versions`.

**Release-workflow VST3 pin:** `sign-and-release.yml` must clone the same Steinberg tag pinned everywhere else in the repo: `v3.7.12_build_20`. The shorthand `v3.7.12` does not exist upstream and will make tag-time macOS release jobs fail before configure/build even start.

**Release-workflow ctest must skip the `validation` label (#720):** the `Test` step in `sign-and-release.yml` MUST pass `-LE validation` to ctest. Without it, the suite includes the `auval-Pulp*` tests that copy a fresh `.component` to `~/Library/Audio/Plug-Ins/Components/` and immediately call `auval`. Hosted GitHub macOS runners' `AudioComponentRegistrar` does not pick up the new bundle reliably, so auval returns `Cannot get Component's Name strings / Error -50`, the Test step exits non-zero, and the entire sign / notarize / publish pipeline silently fails. This was the failure mode of the 30+ consecutive sign-and-release runs preceding v0.41.0. The validation gates are owned by `validate.yml` on PR; do not duplicate them into the release workflow. `tools/scripts/test_release_workflow_test_step.py` (wired into `workflow-lint.yml`) is the regression test that prevents reintroduction.

**Tag safety:** the auto-release workflow is idempotent-strict — if a tag already exists pointing at a different SHA, it fails loudly rather than overwriting. See `docs/guides/versioning.md` for the manual recovery recipe.

**Shared-source priming is retry-wrapped (#1375):** every `ensure_shared_git_source` call in `setup.sh` runs through `ensure_shared_git_source_with_retry` (3 attempts, 5s/10s/20s backoff, scrubs the partial cache target between attempts). Motivated by v0.74.0 + v0.74.1 release-cli runs both dying on `windows-arm64` mid-`Priming shared Yoga source cache` with exit 127 — a transient command-not-found on a Windows shell wrapper. The retry happens at the WRAPPER level, not inside `ensure_shared_git_source`, because that function uses a `set -e` subshell which a 127 tears down before any inner retry can engage. Override attempts via `PULP_PRIMING_RETRY_ATTEMPTS=N`.

**Per-tag release-cli watchdog (#1375):** `.github/workflows/release-cli-watchdog.yml` triggers on `workflow_run` for `release-cli.yml`. It resolves the tag from `head_branch`, queries the GitHub release for `pulp-sdk-*` and `pulp-{darwin,linux,windows}-*` assets via `gh release view`, and opens a per-tag tracker on three failure shapes: `run_failure`, `success_with_missing_assets` (the v0.74.0 pattern — release exists with only plugin .pkg files from `sign-and-release.yml`), `no_release` (the v0.74.1 pattern — `release-cli`'s `release` job never ran because `build-cli`/`smoke-cli` failed). The tracker's body suggests `gh workflow run release-cli.yml --ref vX.Y.Z -f version=vX.Y.Z` to backfill, and auto-closes when the SDK assets land. This is documented as Layer 2b in `docs/guides/release-watchdog.md`.

**Linux release-cli requires libfontconfig1-dev (#1970):** chrome/m144 Skia exposes fontconfig symbols that the previous release kept private. Without `libfontconfig1-dev` in the apt-install step, the Linux link fails on `undefined reference to FcInitLoadConfigAndFonts` et al. Both `release-cli.yml` and `build.yml` Linux deps steps install it. When bumping `tools/deps/manifest.json` Skia pin, run `nm -D` over the new `libskia.a` and grep for unfamiliar prefixes (`Fc`, `Hb`, `FT_`, etc.) — any new symbol class means a matching system package needs to be added.

**Safe backfill of a stuck release-cli tag (#1962):** raw `gh workflow run release-cli.yml --ref vX.Y.Z` re-runs the BROKEN workflow file from the tag's source — useless when the breakage is in the workflow or the scripts it calls. Run the workflow from `main`, pass the old tag as `version`, and leave `source_ref` blank; checkout then uses the tag's source tree and the overlay step copies the current `main` release-pipeline helper files over the in-tree copies. `source_ref` is only for the unusual case where you intentionally want to build another ref under the requested version label. Leave `make_latest` false for old-tag backfills so `/releases/latest` does not move backward; set it true only when backfilling the current newest tag after the automatic tag run failed. To backfill a tag whose source predates a fetch-script fix on main:

```
gh workflow run release-cli.yml --ref main \
    -f version=v0.97.0
```

The workflow file comes from main (fixed), the source tree comes from the tag (correct content), and the overlay step picks up post-tag script fixes automatically. `release-guard.yml` and `release-cli-watchdog.yml` will auto-close their trackers when the SDK assets land. This was the fix for the four-day stall on v0.95.0..v0.97.0 caused by a skia-builder chrome/m144 zip layout drift (`Release/<arch>/libskia.a` instead of `Release/libskia.a`). The fetch script flattens the arch subdir; regression coverage lives in `tools/scripts/test_fetch_skia_for_release.py`, with workflow-condition coverage in `tools/scripts/test_release_workflow_test_step.py`.

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
- `coverage` — event-conditional matrix over {macos} on PRs and {linux, macos, windows} on push-to-main / workflow_dispatch. Every leg builds with Clang source-based coverage, runs the native test suite, uploads HTML + summary + Cobertura artifacts, and pushes to Codecov with exactly one per-OS flag (`os-linux`, `os-macos`, `os-windows`). The Linux leg also installs `coverage.py >= 7.10`, runs `tools/scripts/run_python_coverage.py` for `tools/scripts/**`, `tools/deps/**`, and `tools/local-ci/**`, uploads the Python HTML + summary + Cobertura artifacts, and includes `build-coverage/python/coverage.python.xml` in the same Codecov upload. The macOS leg additionally runs `tools/scripts/run_swift_coverage.py`, stages `build-coverage/apple/coverage.apple.lcov`, uploads the Apple summary artifact, and includes that LCOV in the same Codecov upload when present. Subsystem / platform / surface slicing comes from `codecov.yml`'s `component_management` path globs, not from a multi-flag CSV on the upload step. Has `fail-fast: false` on the matrix — a flake on any one OS never cancels the others and never blocks a merge.
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
  GitHub Releases API (create the release, upload artifacts, fetch
  generate-release-notes content). Without an explicit job-level
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
through the local self-hosted `sanitizer` runner (via the
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
# One-time setup on a self-hosted runner host (Daniels-MacBook-Pro):
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

### Anti-pattern (legacy)

- `planning/scripts/runner-watchdog.sh --fix` — superseded. Use
  `shipyard rescue` (recover, PR-side) or `shipyard runner watch
  --kill-hung-workers` (prevent, host-side).

### Composition with `Version-Bump` gate

`shipyard rescue` does not interact with the `Enforce version & skill
sync` check. If a PR title starts with `fix:` / `feat:` and the branch
lacks either a `chore: bump versions` commit OR a
`Version-Bump: skip reason="..."` trailer on the tip commit, the
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

Companion-track item U-3 in `planning/2026-05-17-refactor-roadmap-final.md`.

## Source-tree pollution: root-allowlist mode

`tools/scripts/source_tree_pollution_check.py` now has a fourth mode beyond `stage` / `push` / `files`: **`--mode=root-allowlist`**.

The root-allowlist mode reads `git ls-tree --name-only <rev>` and fails if any top-level entry is not in `ALLOWED_ROOT_PATHS` (a frozenset declared at the top of the script — ~51 entries covering hidden config, root docs, root build/config files, and subsystem directories).

Wiring:

- `.githooks/pre-push` invokes `--mode=root-allowlist --rev HEAD` right after the existing `--mode=push` check. Hard-fail; no env-var bypass.
- `.github/workflows/source-tree-pollution-check.yml` runs the same mode in CI. Triggers on `paths: ['**']` (the check is ~5s — no point gating). Catches direct REST / admin merges that skip the pre-push hook.

Adding a new top-level entry requires the same-PR allowlist update — the gate's error message points contributors to the exact line in the script. See the new "Repo-root hygiene" section in `CONTRIBUTING.md` for the contributor-facing explanation.

Companion-track item U-1 in `planning/2026-05-17-refactor-roadmap-final.md`.

## Namespace macOS overflow on `workflow_dispatch`

`resolve-provider` in `.github/workflows/build.yml` applies the Namespace macOS overflow logic on both `pull_request` AND `workflow_dispatch` events (since 2026-05-19, closes #2314).

Pre-2026-05-19 behavior gated overflow on `EVENT_NAME == "pull_request"` only, which silently routed `shipyard pr` ship cycles (`workflow_dispatch`-triggered) back to the local self-hosted Mac. That defeated the 2026-05-18 cloud cutover for the path most contributors hit.

Precedence on `workflow_dispatch`:
1. `inputs.macos_runner_selector_json` (operator override) — always wins.
2. Namespace overflow when local Mac BUSY ≥ threshold.
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
  a few exact files). Everything else — including `core/**`, all
  `CMakeLists.txt`, `tools/cmake/**`, `tools/scripts/**`,
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
- To change what counts as skip-safe: edit `SKIP_SAFE_PREFIXES` /
  `SKIP_SAFE_EXACT` / `FORCE_BUILD_PREFIXES` in `classify_changes.py`
  and add a case to `test_classify_changes.py`. Never widen the
  allowlist without a test.

Companion plan: `planning/2026-05-19-ci-optimization-plan.md`.

## macOS runner routing — local primary, GitHub-hosted overflow

The `resolve-provider` job in `build.yml` decides per-run where each
`Build and Test` macOS leg runs:

- **Local first.** While the local self-hosted Mac has spare capacity
  the macOS leg routes to it (`PULP_LOCAL_MACOS_RUNS_ON_JSON`).
- **Overflow to free GitHub-hosted `macos-15`.** When the local Mac is
  saturated the leg routes to `macos-15` instead. The repo is public, so
  GitHub-hosted macOS is **free**; overflow costs nothing, it just runs
  slower than the M1 Max.
- **Capacity-aware (`#3299`).** "Saturated" is decided by real SUPPLY
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
4. **Check real idle capacity before assuming "stuck."**
   `gh api repos/<owner>/<repo>/actions/runners --jq '[.runners[] |
   select(.status=="online" and .busy==false) | .name]'` lists idle
   self-hosted runners (`pulp-build-studio` Studios + the `pulp-build-m5`
   blackbook M5 are the usual idle ones; `pulp-m1-*` are often offline).
   If a leg failed on saturation while these sit idle, the
   `gh run rerun` will now claim them (capacity-aware routing, #3299).
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

## Release health escalation (`release-health.yml`)

Beyond the auto-release/cadence/release-cli watchdogs, `release-health.yml`
(every 2h) is the SYMPTOM-LEVEL catch-all: it fails RED + keeps ONE rolling
`release-health`-labelled issue when the newest N tags (default 2) have no
non-draft release with assets — i.e. published releases aren't keeping pace with
tags, regardless of which leg broke. To debug a "release stuck" report: check
this workflow's latest run + the open `release-health` issue, then the failing
`sign-and-release.yml`/`release-cli.yml` legs for the newest tag. Discord push is
wired but OFF unless the `RELEASE_ALERT_DISCORD_WEBHOOK` secret is set.

## Release is built by TWO workflows → publish via the coordinator (immutable releases)

A Pulp release is assembled from two parallel tag-triggered workflows: `Release
CLI` (release-cli.yml → CLI binaries) and `Sign and Release` (sign-and-release.yml
→ macOS sign/notarize + appcast.xml). GitHub now makes a PUBLISHED release
IMMUTABLE, so the leg that published first locked the other out ("Cannot upload
asset to an immutable release") and releases shipped incomplete. Both legs now
create the release as a DRAFT on a tag push; `release-publish.yml` (the "Release
publish coordinator", workflow_run on both) flips it to published EXACTLY ONCE,
when BOTH legs succeed for the same SHA. Consequences to know: a release stays a
draft until both legs are green (incomplete releases never publish), and
release-health.yml treats a stuck draft as unhealthy. When debugging "tag exists
but no published release", check both legs' runs AND the coordinator. A manual
release-cli workflow_dispatch backfill still publishes directly (draft only on
`push`).
