# CI Validation

Pulp validates branches on macOS (local), Ubuntu (SSH), and Windows (SSH) before merging.

> Setting up a dedicated machine as a persistent CI runner? See
> [self-hosted-runner.md](self-hosted-runner.md) for the walkthrough
> + first-run gotchas (git-lfs hook conflict, Xcode license,
> Apple Clang version skew).

## Primary: Shipyard

[Shipyard](https://github.com/danielraffel/Shipyard) is Pulp's primary CI tool. It delivers exact SHAs via git bundles, runs your build/test commands on each platform, and gates merges on per-SHA evidence.

The required `macos` gate runs the shipyard `mac` target
(`.shipyard/config.toml`, `[validation.default]`). Its `test` step is
`ctest ... --repeat until-pass:2 --label-exclude "validation|slow|performance|bench|quality-lab"`
— it excludes the long `slow` tests, the example plugins' `validation`
format-validators (these gate on the path-filtered `example-validation` lane
instead), and the relative-timing / CPU-budget / benchmark tests
(`performance|bench|quality-lab`), and retries a single flake once so timing-flakes
don't redden the gate. The perf/ratio tests are excluded (2026-07-21, mirrors
build.yml) because they tolerate steady load but flake under the load *variance*
of the Studio's 2 concurrent build VMs (cap=2) — a perf gate can't live on a
cap=2 runner; it belongs in a dedicated cap=1 nightly/perf lane. The full lane model —
what runs where, the label taxonomy, and how to route a new test — is
[docs/guides/test-lanes.md](test-lanes.md).

```bash
./tools/install-shipyard.sh              # install pinned version
./tools/install-shipyard.sh --status     # compare installed vs pinned
shipyard run                              # validate current branch
shipyard pr                               # create, track, validate, and merge on green
shipyard cloud run build <branch>         # dispatch to Namespace
shipyard rescue <PR>                      # recover a wedged PR
shipyard runner watch --kill-hung-workers # prevent self-hosted runner wedges
shipyard update --check --json            # report installed vs latest
```

### Runner timing metrics

Pulp does not store CI timing history in the Pulp CLI or MCP server. When a
checkout uses Shipyard, and optionally tartci for disposable local VMs,
Shipyard owns the timing database and query surface:

- Shipyard can import GitHub Actions job timings and local command evidence.
- tartci can optionally emit per-VM runtime records for macOS, Linux, and
  Windows VM lanes: boot/setup/run/cleanup durations, labels, host, provider,
  golden/cache hints, outcome, and failure class.
- Shipyard imports those tartci records into its local metrics store and exposes
  agent-readable summaries, slowest lanes, trend/drift checks, comparisons, and
  placement advice.

This is mainly for agents watching Pulp CI over time. It gives them enough
history to answer "is this runner behaving normally?", "did boot/build time
regress?", "which lane should I monitor next?", and "is this worth
investigating or just within the usual range?" Humans can use the same commands
for high-level platform comparisons, but no observability service is required.

The `shipyard metrics` commands require a Shipyard build that includes the
metrics subcommand. Pulp's current pinned source-checkout version in
`tools/shipyard.toml` is `v0.68.0`, which does not include that surface yet. Use
a newer Shipyard binary or source checkout for this optional metrics workflow
until the Pulp pin is bumped.

```bash
# Enable VM runtime records on tartci hosts or LaunchAgents.
export TARTCI_RUNTIME_MEASURE=1
export TARTCI_RUNTIME_GH_ENRICH=1

# Inspect tartci's local VM timing records.
tartci runtime recent --repo Generous-Corp/pulp --limit 20 --json
tartci runtime summary --repo Generous-Corp/pulp --json

# Import both GitHub Actions and tartci VM timing into Shipyard's metrics store.
shipyard metrics import github --repo Generous-Corp/pulp --limit 50 --json
tartci runtime export --repo Generous-Corp/pulp --since-days 14 \
  | shipyard metrics import tartci --json

# Agent-friendly queries.
shipyard metrics summary --project pulp --json
shipyard metrics slowest --project pulp --limit 20 --json
shipyard metrics watch --project pulp --since 14d --json
shipyard metrics advise --project pulp --json
```

Use the Shipyard and tartci docs for setup details; this guide only records how
Pulp expects agents and contributors to consume the optional integration.
Without tartci, `shipyard metrics import github` and manual/command metrics
still work for GitHub-hosted or SSH-backed CI lanes.

Pulp intentionally pins Shipyard in `tools/shipyard.toml` even if your daily
global `shipyard` is newer. Use `shipyard pin bump --to vX.Y.Z` for pin
updates instead of hand-editing the file; newer Rust Shipyard releases changed
the macOS asset shape to a signed/notarized `.dmg`, and the bump command keeps
the version and asset metadata in sync.

The public Pulp installer does not install Shipyard or GitHub CLI (`gh`).
That is intentional: ordinary Pulp users do not need either tool to create,
build, run, or upgrade projects. They are source-checkout contributor tools.
`pulp pr` defaults to Shipyard and fails with install/switch guidance if
Shipyard is missing; contributors who prefer their own PR flow can set
`pulp config set pr.workflow github` or `manual`. The `github` workflow uses
`gh` directly and requires it to be installed and authenticated. Run
`pulp status` to see the effective workflow and local tool health.

### Optional local VM routing

Core Pulp development can also use local, disposable VMs through
[tartci](https://github.com/danielraffel/tartci). This is optional: a normal
contributor can open a PR and let GitHub Actions run on hosted runners. The
value of the local VM setup is faster feedback on trusted Apple Silicon
hardware while keeping every job clean-per-run.

The current Pulp routing policy is intentionally kept in parseable TOML at
[`.shipyard/ci-profiles/normal-local-fast.toml`](../../.shipyard/ci-profiles/normal-local-fast.toml)
instead of copied into this guide. It names the PR, release, coverage,
scheduled, and issue-on-failure policies and maps stable target IDs to concrete
GitHub `runs-on` selectors. Shipyard owns orchestration and profile selection;
tartci owns the local VM providers, goldens, host caches, and per-host status.
Use the upstream docs for details:

- [Shipyard profiles](https://github.com/danielraffel/Shipyard/blob/main/docs/profiles.md)
  explain how profiles and fallback resolution work.
- [tartci](https://github.com/danielraffel/tartci) explains the Tart/QEMU VM
  lanes, `tartci status --json`, and `tartci profile explain|plan`.
- [mac-ci-host-setup.md](mac-ci-host-setup.md) is the Pulp-specific host setup
  guide for joining the macOS VM pool.

When `pulp build`, `pulp dev`, or `pulp loop` run on a tartci-governed host,
the CLI asks tartci for a host-core lease and caps CMake parallelism to the
leased job count. Lease-backed builds also run through a POSIX process-group
watchdog by default. The watchdog terminates a build that stays over its CPU
budget long enough to threaten the shared host; set
`PULP_TARTCI_WATCHDOG=monitor` to log over-budget samples without killing,
or `PULP_TARTCI_WATCHDOG=0` to disable the wrapper. Operators can tune
`PULP_TARTCI_WATCHDOG_INTERVAL_SECS`, `PULP_TARTCI_WATCHDOG_SAMPLES`,
`PULP_TARTCI_WATCHDOG_TERM_GRACE_SECS`, `PULP_TARTCI_WATCHDOG_CPU_PER_JOB`,
and `PULP_TARTCI_WATCHDOG_PYTHON` per host.

### Shipping a PR: `shipyard pr`

`shipyard pr` is the single "ship this" orchestrator. Agents and humans should
route every normal ship cycle through it rather than pairing `gh pr create`
with `shipyard ship` manually. It:

1. Runs `tools/scripts/skill_sync_check.py` (hard-fails on missing SKILL.md updates).
2. Runs `tools/scripts/version_bump_check.py --mode=apply` to bump SDK / Claude plugin / marketplace versions consistently.
3. Commits the bump (if any) as `chore: bump <surfaces>`.
4. Pushes the branch, creates the PR, and records Shipyard tracking state.
5. Runs cross-platform validate + merge on green.
6. The auto-release workflow tags and publishes binaries on merge.

```bash
shipyard pr                              # primary ship path
shipyard pr --base develop/package-manager # ship to a develop branch
shipyard pr --title "..."                # override PR title
shipyard pr --dry-run                    # print the plan without executing
```

`pulp pr` is a compatibility wrapper that delegates to `shipyard pr` by
default; it is valid, but guidance should name `shipyard pr` directly so
humans and agents understand where PR tracking state lives. Its `github` and
`manual` workflows are explicit local opt-outs and do not create Shipyard
tracking state.

Direct `gh pr create` is an emergency/manual bypass only. If it is used, call
out that the PR may not appear in Shipyard-managed state until it is reconciled
or re-shipped through Shipyard.

### Shipyard v0.3.0 workflow surface

Shipyard v0.3.0 adds stateful ship resume, SSH `--resume-from` staging, and
incremental git bundles on top of the basic `run` / `ship` / `cloud run`
surface above.

```bash
# Resume an interrupted ship
shipyard ship --resume                    # pick up where the last session left off
shipyard ship --no-resume                 # discard stale state and ship fresh

# Inspect in-flight ship state
shipyard ship-state list                  # self-describing inventory: PR, title, URL, tip SHA, dispatched run IDs
shipyard ship-state show <pr>             # full state for one PR
shipyard ship-state discard <pr>          # archive stale state

# Prune old ship state + evidence
shipyard cleanup --ship-state             # dry run — show what would be pruned
shipyard cleanup --ship-state --apply     # prune closed-PR state + aged records

# Fast test iteration on any target
shipyard run --resume-from build          # skip configure+setup, start at the build stage
shipyard run --resume-from test           # skip configure+build, run tests only

# The `windows` / `ubuntu` SSH targets are opt-in per machine and are NOT
# declared in .shipyard/config.toml — the commands below only work once you
# uncomment the matching block in .shipyard.local/config.toml (see
# .shipyard.local/config.toml.example). `shipyard targets list` shows what
# this machine actually has.
shipyard run --targets windows --smoke    # fast Windows-only preflight
shipyard run --targets windows --resume-from test   # ~2 min rerun vs ~15 min full

# Target and config inspection
shipyard targets                          # list configured targets with reachability
shipyard targets test windows             # probe a single target
shipyard config show                      # effective merged config
shipyard config profiles                  # list profiles plus the active one
```

Ship state lives at `<state_dir>/ship/<pr>.json`. Shipyard auto-resumes the
next time you run `shipyard ship` on the same PR — it refuses to resume if
the PR's head SHA or merge policy changed since the state was written, so a
rebase or force-push deliberately forces a fresh ship.

`--resume-from` works on both local and SSH targets. On SSH targets,
Shipyard probes the remote for a marker file proving the previous stage
passed for the exact SHA, and skips earlier stages when it finds one.

**Incremental bundles** — SSH validation now sends only the git delta
between the remote HEAD and the target SHA. Typical cycles drop from
~443 MB to a few KB. No configuration needed — Shipyard falls back to a
full bundle automatically when the delta would be larger than the full
pack.

## Keeping fleet Macs on the Shipyard pin (optional)

`tools/shipyard.toml` pins the Shipyard version every checkout uses, and
`tools/install-shipyard.sh` installs exactly that pin. On a machine that ships
PRs every day the pin moves underneath you, and a machine that quietly falls
behind — or, worse, drifts *ahead* after a stray `shipyard update` — runs a
Shipyard that was never validated against Pulp's CI matrix and that disagrees
with the `SHIPYARD_VERSION` every workflow declares.

`tools/scripts/shipyard_autoupdate.py` converges one machine onto the pin.
Nothing about it is required: a public cloner runs `install-shipyard.sh` once
and never thinks about this again. It exists for the local Macs.

```bash
# What would happen, without touching anything:
python3 tools/scripts/shipyard_autoupdate.py --check --json

# Converge now (no-op and silent if already at the pin):
python3 tools/scripts/shipyard_autoupdate.py

# Run it hourly, in the background, per machine:
tools/scripts/install_shipyard_autoupdate.sh
tools/scripts/install_shipyard_autoupdate.sh --status
tools/scripts/install_shipyard_autoupdate.sh --uninstall
```

**Kill switch.** Auto-update is on once installed, and off everywhere it is
not installed. To stop it without uninstalling:

```bash
echo off > ~/.config/pulp/shipyard-autoupdate    # `on` resumes
```

`PULP_SHIPYARD_AUTOUPDATE=0` does the same for a shell or a one-off run, and
overrides the file. The **file** is the one that matters for the background
agent: a launchd agent inherits no shell environment, so an env-only kill
switch could not reach the thing it is meant to kill.

What it guarantees, and why each one is there:

| Behaviour | Why |
|---|---|
| Converges to the pin, **never to `latest`** | The pin is the source of truth; a bare `shipyard update` tracks `latest` and strands the machine ahead of the pin (7 minors ahead on 2026-07-16). |
| Handles **both** directions | `shipyard update` refuses to go backwards — it reports `update_available: false` and exits 0 — so coming back from ahead of the pin goes through `install-shipyard.sh`. |
| Reads the pin from **`origin/main`** | A dev checkout is usually parked on a feature branch, which may carry an experimental pin. `PULP_SHIPYARD_AUTOUPDATE_PIN_REF=worktree` overrides. |
| **Never updates mid-job** | Swapping the binary under an in-flight ship could corrupt a run. It defers while a Pulp `Runner.Worker` or a validating `shipyard` subcommand is alive. The always-on `shipyard daemon` does not count as busy. |
| **Fails closed** | Any probe that cannot answer (`ps` fails, version unreadable, host offline) means "do not update". The working binary is left in place and the machine converges on a later tick — which is also how an intermittently-offline laptop is meant to behave. |
| **Verifies the outcome** | Exit 0 is not proof. The installed version is re-read and must equal the pin, so a declined update or a swallowed checksum failure reports as a failure instead of a false success. |
| **One installer at a time** | A hand-run converger and a background tick both writing `~/.local/bin/shipyard` is exactly the half-installed binary to avoid; the install step is held under a machine-wide lock. |
| **Silent when nothing changed** | The steady state prints nothing. Every decision is still published to `~/.local/state/pulp/shipyard_autoupdate.json`. |

## Host resource governance

Pulp's local Macs are shared: CI validation builds run alongside agent and
developer builds on the same host. Two of them melted in July 2026 — one
CPU-bound, one memory-bound/OOM — from unbounded builds oversubscribing the
machine. A per-host build-resource **governor** now bounds every build path.
It is tiered:

- **Tier 0 — always, zero config.** The `pulp` CLI bounds build parallelism to
  `min(cores, RAM_budget / 1.5 GiB)` on every build it emits (`pulp
  build/dev/loop`, the local-SDK build). No lease store required; override the
  RAM axis with `PULP_BUILD_MEM_BUDGET_MB`. `tools/scripts/build_parallelism_guard.py`
  rejects a bare `--parallel`/`-j` (unbounded) anywhere in the repo, and — on the
  shared-host surfaces agents copy from (`CLAUDE.md`, `.shipyard/config.toml`,
  `.agents/skills/**`) — also rejects an *explicit but whole-machine* count
  (`-j$(nproc)` / `-j$(sysctl -n hw.ncpu)` / `--parallel $(getconf
  _NPROCESSORS_ONLN)`): it has a count, so it is not unbounded, but on a shared
  Mac it claims every core, so N concurrent builds request N × cores and starve
  each other. The rule is a property of the host, not the command — so the guard
  fires only where a static scan can *prove* the surface is shared. It does NOT
  scan `.github/workflows/**`, and **not** because a workflow leg never shares a
  box: a workflow's `runs-on` is resolved dynamically (often
  `${{ fromJSON(matrix.runs_on_json) }}` or a repo var) and can point at the
  shared self-hosted Studios — Pulp's own macOS matrix leg resolves to
  `PULP_LOCAL_MACOS_RUNS_ON_JSON`, the Studios that host the required `macos`
  gate. A file scan cannot resolve that, so in a workflow the bound is the
  **author's** responsibility: route a self-hosted macOS leg through
  `tools/ci/governed-build.sh` (as `build.yml`'s intel-canary compile,
  `examples-validation.yml`, `web-plugins.yml`'s `gpu-audio-macos` job, and
  `format-baseline-diff.yml` now do). The steer everywhere is `pulp build` /
  `tools/ci/governed-build.sh`, which take their `-j` from the governor.
- **Tier 1 — tartci per-host lease governor.** On a host running a tartci lease
  store, builds and VM runners acquire a weighted core+memory lease before
  starting; admission is `min(core-budget, memory-budget)`, so a build that
  would exhaust RAM is refused even when CPU is free. Each host derives a role
  budget from `tartci host-profile`:
  - **dedicated-builder** — a machine whose job is CI builds (largest core +
    memory budget).
  - **dev-overflow** — a shared dev machine that also takes overflow CI, running
    its VM lane at non-gate priority so it never starves the required `macos`
    gate.
  - **light** — a low-resource/travel host with a small budget.
- **Tier 2 — Orchard fleet VM placement (shadow phase).** Fleet-level placement,
  wired but placing nothing yet. See the tartci runbook's Orchard section.

The **mac local lane** is the one that historically escaped the CLI: Shipyard's
`local` backend runs the `.shipyard/config.toml` build string directly on the
host and does not pass through the `pulp` CLI. Every build stage in that config
— `default`, `parser`, **and** `smoke` — is therefore wrapped by
`tools/ci/governed-build.sh`, which acquires a tartci build lease sized from the
host profile, exports the granted `-j`, runs the build as a child process, and
releases the lease on exit. When tartci is absent (a build VM or a plain
checkout) or the lease is denied (host saturated), it falls back to the Tier-0
bound — it never fails the build and never piles onto a saturated host. (The
`smoke` lane previously used a raw `--parallel $(getconf _NPROCESSORS_ONLN)` and
so ran whole-machine on the shared Mac while the required gate validated
alongside it; it now takes a governed share like the other lanes.) The
version-controlled `overrides.windows` recipes keep a fixed `--parallel 4`
instead: they run under PowerShell with no wrapper-path or `$(…)` assumptions,
and unbounded MSBuild link parallelism trips LNK1104 on ARM64.

`pulp status` reports the active tier with a `Build governance: Tier N (…)`
line. Host-side setup and the deeper lease/role/memory-axis mechanics live in
the [tartci](https://github.com/danielraffel/tartci) repo (`scripts/leases.py`,
`scripts/host_profile.py`, `tartci host-profile` / `tartci leases`).

## Validation Profiles

Shipyard validates from a profile (`shipyard run --pipeline <name>`).
Pulp's `.shipyard/config.toml` defines three:

| Profile | When to use | What it runs |
|---------|-------------|--------------|
| `default` | Most PRs. The lane every cross-platform target gates on. | Full `setup → configure → build → test`. Examples ON. Excludes the `slow` ctest label. |
| `parser` | PRs that only touch runtime-import parser code. | Same stages with `PULP_BUILD_EXAMPLES=OFF`; tests filter to `--label-include parser-import`. Skips plugin validators (auval / pluginval / clap-validator) and the broader format-adapter smoke surface. |
| `smoke` | Quick downstream-scaffold check after dependency or install-layout edits. | Configure + governed build only (both `cmake --build` steps go through `tools/ci/governed-build.sh`); runs the SDK-smoke export against a downstream scaffold. |
| `gates` | Version-bump / skill-sync gate scripts. | `tools/scripts/skill_sync_check.py` + `tools/scripts/version_bump_check.py` in report mode. |

`shipyard config profiles` lists what is installed locally and which one is active.

### Auto-selecting the parser profile

`tools/scripts/validation_profile_select.py` classifies the current diff
and prints `parser` or `default`:

```bash
# Default: diff HEAD against origin/main
shipyard run --pipeline "$(python3 tools/scripts/validation_profile_select.py)"

# Explicit diff base
shipyard run --pipeline "$(python3 tools/scripts/validation_profile_select.py --base origin/develop)"

# Operate on a literal file list (e.g. piped from gh pr diff)
gh pr diff <PR> --name-only \
  | python3 tools/scripts/validation_profile_select.py --paths-from -

# JSON envelope (profile + matched + unmatched)
python3 tools/scripts/validation_profile_select.py --json
```

The script returns `parser` only when every changed path falls inside
the explicit parser-only scope (the standalone `tools/import-design`
tool, `tools/import-validation` scripts, the `packages/pulp-import-ir`
package, `test/fixtures/imports/**`, the parser test files in `test/`,
the `core/view/.../design_import*` family, and the import-runtime JS).
Any path outside that set forces `default` — the safety bias is toward
broad validation.

To opt out for an individual run, pass `--pipeline default` explicitly.

## Cache-warming runs on `main`

`build.yml` triggers on `push: branches: [main]` in addition to
`pull_request` / `merge_group` / `workflow_dispatch`. That run gates nothing —
it exists solely to **publish the GitHub-hosted Linux/Windows ccache and
FetchContent caches** that PR runs restore from.

It is needed because of how GitHub's cloud cache is scoped: a cache entry
written by a PR run is visible only to that PR's own ref, so PR runs can never
warm each other. Only a non-PR run on the default branch writes an entry every
subsequent PR can read. Without the `push` trigger the `Save …` steps are
unreachable and the matching `Restore …` steps are a permanent miss.

Each trigger runs a deliberately different slice of the matrix:

| | PR run | `merge_group` run | `push: main` cache run |
|---|---|---|---|
| macOS matrix leg | yes | yes | **no** — omitted by `resolve-provider` |
| Linux matrix leg | yes | yes | yes (publishes the cache) |
| Windows matrix leg | **no** — see below | yes | yes (publishes the cache) |
| `windows-{msvc-release,midi2,ble}-gate` | **no** — see below | yes | no |
| `macos` / `linux` / `windows` alias jobs | yes | yes | no |
| Writes to GitHub's cloud cache | no | no | Linux + Windows only |

The macOS leg is dropped because macOS builds on the **self-hosted** Macs that
serve the one required check in this repo, and those machines keep ccache and
FetchContent on local disk between jobs. Scheduling a macOS leg on a push would
put the required gate's runners under load to save a cache that is never
uploaded — strictly a cost. For the same reason the two `Save …` steps are
scoped `runner.environment == 'github-hosted' && runner.os != 'macOS'`, which
is narrower than the restore side on purpose.

Push runs are also exempt from `cancel-in-progress`: they share the
`refs/heads/main` concurrency group, so cancelling a superseded one would kill
its cache-save step exactly when main is busiest. PR runs still cancel.

The `classify` job diffs an **event-dependent base**
(`tools/scripts/resolve_classify_base.py`): a PR diffs
`github.event.pull_request.base.sha`, a push diffs `github.event.before`. On a
push, `origin/main` resolves to HEAD itself and the diff is always empty — so a
docs-only merge is indistinguishable from a core merge, and the run never
skips. A docs-only merge to main now correctly skips the whole matrix.

## Windows is gated by the merge queue, not by the PR head

Windows is advisory and runs entirely on GitHub-hosted runners, and a single
run carries **four** Windows jobs: the `Windows (x64)` matrix leg plus the
`windows-msvc-release-gate`, `windows-midi2-gate`, and `windows-ble-gate`
compile gates. The repository draws all of those from one fixed pool of
concurrent GitHub-hosted jobs, shared with every other workflow.

That pool is the scarce resource, and Windows is by far its largest consumer.
With a handful of PRs open at once, advisory Windows work fills nearly every
slot and the **required** hosted check — `Build + prove + (owner-gated)
deploy`, on `ubuntu-latest` — cannot get a runner. The merge queue then holds
its entry in `AWAITING_CHECKS` until the ruleset's check-response timeout
expires, evicts it, and nothing lands at all. macOS is never implicated: it
runs on the self-hosted Macs, which sit outside the hosted pool.

So Windows runs where it actually gates:

- **`merge_group`** — the serial queue validation, which builds PR ∪ main.
  This is strictly better coverage than a PR-head build, and being serial it
  costs at most one run's worth of Windows jobs at a time.
- **`push: main`** — publishes the Windows ccache.
- **`workflow_dispatch`** — explicit reruns when you want Windows early.

A PR head keeps macOS (self-hosted) and Linux (self-hosted Linux VMs) for fast
signal; neither competes for the hosted pool. The advisory `windows` alias job
short-circuits to green on `pull_request` — without that it would fail closed
looking for a matrix leg that deliberately did not run.

The trade is later Windows feedback: a Windows-only break is caught when the PR
reaches the queue rather than on the PR head, costing one eviction and a
requeue. That is cheaper than the failure it replaces, where *no* PR could
merge at all. Dispatch `build.yml` manually against the branch if you want
Windows before enqueueing.

`tools/scripts/test_windows_runner_policy.py` locks this in: it executes
`resolve-provider`'s matrix resolver for each event and asserts the Windows leg
is absent on `pull_request` and present on `merge_group` / `workflow_dispatch`,
that all three compile gates skip `pull_request`, and — as a negative control —
that macOS and Linux still run on the PR head.

## Routing contract (checked)

Every `*_RUNS_ON_JSON` repo variable is a **lane**: it names the labels a class
of jobs is dispatched to. The intended lane→label mapping lives in
**`tools/scripts/runner_topology.json`**, and
`tools/scripts/runner_topology_check.py` reconciles it against the live repo
variables and the live registered runners.

**The contract is the source of truth for lane→label.** Label values quoted
inline elsewhere in this guide are illustrative and can lag; the contract plus
its checker are authoritative, because they are the only pair that is verified.

### The failure this prevents

GitHub does not validate `runs-on`. **A job that asks for a label no runner
carries is not an error — it is queued, forever.** There is no warning, no
annotation, no failed check. The only symptom is jobs piling up while the pool
looks saturated, which is indistinguishable from "we're just busy".

That makes a mis-pointed routing variable *silent*. A relief valve routed into a
black hole is worse than no relief valve: it reports healthy and relieves
nothing, and the queue it was supposed to drain grows behind it.

The same class of bug already bit the busy probe in `build.yml`: reading
`actions/runners` needs `Administration: Read`, which the default
`GITHUB_TOKEN` lacks, so the probe 403s and falls back to `BUSY=0` — silently
disabling overflow. Nothing about either failure is visible without asking.

### What the checker asserts

| Check | Failure it catches |
|-------|--------------------|
| `drift` | A variable was edited without updating the contract (or vice versa). The variable is a reviewed artifact, not a blind edit. |
| `black-hole` | The lane's labels are satisfiable by no runner. |
| `degraded` | The only matching runners are offline — the host may just be asleep. A warning, not an error: a different failure from a label nobody owns. |
| `undeclared` | A live routing variable with no lane in the contract. |
| `hosted-unknown` | A `runs-on` value that is not self-hosted and not a known GitHub image — i.e. a typo, which queues forever. |
| `must-unset` | A paid Namespace overflow variable is set (cost guard). |

Label matching is **subset containment**: GitHub dispatches to a runner only if
it carries *every* label in the array. A lane requesting
`[self-hosted, macOS, ARM64, pulp-build, pulp-build-studio]` is not served by a
runner carrying only `pulp-preamble`, however much the labels overlap.

### Three runner states, not two

`online` / `offline` is not the whole story. Tart runners register **JIT and
ephemeral** (`tools/ci/tart-runner.sh`, `tart-runner-linux.sh`): they exist only
while a job runs and vanish when idle. For those lanes an empty registry proves
nothing — the provisioner may simply have nothing to do.

So ephemeral lanes are judged on **service history** instead: has any job been
dispatched to this exact label set inside the lookback window? A label set with
no runner *and* no recent service has nothing provisioning it, and that is a
black hole. This distinction is load-bearing — without it the release lanes,
which are idle between releases, would be flagged as broken every sweep.

Service history is gathered from the workflows that **consume the lane**, found
by scanning `.github/workflows` for the variable. A repo-wide "last N runs"
sweep is *not* a time window: on a busy repo the newest 100 runs were measured
covering well under an hour, so any lane used less often than that — every
release lane — would be condemned on every sweep. Scoping to the consuming
workflow makes 20 runs reach back months for a handful of API calls. The scan is
also **lazy**: a lane with a live runner costs zero API calls.

**Honest limits.** This check proves a lane *can* be served; it does not prove
jobs *are* being served well. It will not catch a runner that is online but
wedged, a lane that is slow rather than dead, a capacity shortfall (labels
resolve, queue still grows), or a black hole in a `runs-on` hard-coded in a
workflow rather than driven by a variable. An ephemeral lane whose consuming
workflow has not run inside the lookback window yields no evidence and is
reported as a black hole — a false positive that is deliberately biased loud, on
the grounds that a silent relief valve is what caused this in the first place.

### Where it runs, and why

- **`runner-topology-check.yml`** — hourly cron on `ubuntu-latest`, opening and
  auto-closing a tracking issue. The invariant is about *live fleet state*, so
  it can break with **no commit at all**: a runner is decommissioned, a host
  renamed, a variable edited in the web UI. A PR gate would never see any of
  that. It runs GitHub-hosted deliberately — a check that queues behind the
  saturated pool it is auditing is no check.
- **`runner-topology-selftest`** (ctest) — the diff-shaped half: contract
  well-formedness and the reconciliation logic. No network, so it runs on every
  PR for free and never adds an API call to the required macOS gate.

The checker exits `2` when live state cannot be read, distinct from pass (`0`)
and violation (`1`), so a missing token scope fails loudly instead of reporting
a false green.

### Changing a lane

Edit the variable **and** its lane in `runner_topology.json` in the same change —
the drift check exists to make that atomic. Then:

```bash
# Reconcile against the live fleet (uses ghapp locally — the App token bucket).
python3 tools/scripts/runner_topology_check.py --mode=report

# Advisory (never fails), useful while iterating.
python3 tools/scripts/runner_topology_check.py --mode=hint
```

## macOS overflow routing (Plan B)

> **Namespace is OFF (cost).** We build macOS on **local Macs + GitHub-hosted**
> only. `PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON` is kept **UNSET**, so the
> Namespace overflow described here never fires — it's a documented break-glass
> option, not the active path. The required gate is the **bare-metal Mac
> Studios** on m3 (`PULP_LOCAL_MACOS_RUNS_ON_JSON`); the overflow tier is the
> **local JIT VM pool** on m1 + m5 (`PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON`).
> Do **not** repurpose the Namespace var to point at self-hosted runners (see
> CLAUDE.md "Runner priority").

**Read the live variable, not this page's defaults.** A routing var describes
reality; `build.yml`'s `||` fallback is only what happens when the var is unset.
The two disagree: `build.yml` defaults macOS overflow to GitHub-hosted
`["macos-15"]`, while the live variable sends it to the local VM pool. Confirm
before reasoning about a route:

```bash
gh variable list -R Generous-Corp/pulp | grep RUNS_ON_JSON
```

### Live routing state (verified 2026-07-16)

| Variable | Value | Lane |
|---|---|---|
| `PULP_LOCAL_MACOS_RUNS_ON_JSON` | `["self-hosted","macOS","ARM64","pulp-build","pulp-build-studio"]` | bare-metal Studios (m3) — required gate |
| `PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON` | `["self-hosted","macOS","ARM64","pulp-build","pulp-build-vm"]` | JIT VMs (m1 + m5) |
| `PULP_RELEASE_MACOS_RUNS_ON_JSON` | `["self-hosted","macOS","ARM64","pulp-build-vm-release"]` | JIT VMs (m1 + m5) |
| `PULP_INTEL_RELEASE_MACOS_RUNS_ON_JSON` | `["self-hosted","macOS","ARM64","pulp-build-vm-release"]` | JIT VMs |
| `PULP_COVERAGE_MACOS_RUNS_ON_JSON` | `"macos-15"` | GitHub-hosted |
| `PULP_LOCAL_MAC_OVERFLOW_THRESHOLD` | `3` | busy count that triggers overflow |
| `PULP_LOCAL_LINUX_PRIMARY_RUNS_ON_JSON` | `[…,"pulp-build-linux","pulp-host-macstudio"]` | capacity 1 |
| `PULP_LOCAL_LINUX_OVERFLOW_RUNS_ON_JSON` | `[…,"pulp-build-linux","pulp-host-m5"]` | capacity 1 |

When the local self-hosted Mac runner is saturated, `build.yml`'s
`resolve-provider` job *could* route new macOS legs to a Namespace cloud runner
instead of queueing on local — but only if the Namespace var is set, which it is
not. Snap-back is automatic — once local clears, fresh dispatches return to
local. Source of truth:
`planning/2026-05-13-namespace-overflow-implementation.md` (Plan B,
reviewed by `/codex` 2026-05-13).

**Precedence** (highest first, resolved per dispatch):

1. **Operator override** — `gh workflow run build.yml --field macos_runner_selector_json='"<label>"'`. Always wins.
2. **Overflow** — `BUSY >= PULP_LOCAL_MAC_OVERFLOW_THRESHOLD` (default `2`) AND `PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON` is set AND the trigger is `pull_request`. Routes to `namespace-profile-generouscorp-macos`.
3. **Local default** — `PULP_LOCAL_MACOS_RUNS_ON_JSON`. For the current label set, read the lane in `tools/scripts/runner_topology.json` — that file is checked against the live fleet, so it cannot drift the way a value quoted here can.

**Tuning knobs** (repo variables):

| Variable | Default | Purpose |
|----------|---------|---------|
| `PULP_LOCAL_MAC_OVERFLOW_THRESHOLD` | `2` | BUSY count that triggers overflow. Raise when Plan A's 2nd local runner lands. |
| `PULP_LOCAL_MAC_RUNNER_LABEL` | `sanitizer` | Label the busy probe filters runners by. Must match `PULP_LOCAL_MACOS_RUNS_ON_JSON`'s target label. |
| `PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON` | (unset) | Namespace selector JSON, e.g. `"namespace-profile-generouscorp-macos"`. When empty, overflow is disabled — everything stays local. |

**Disabling overflow** (revert to local-only):

```bash
gh variable delete PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON --repo Generous-Corp/pulp
```

The next PR's `resolve-provider` falls through to local since condition #2 fails.

**Inspecting routing decisions:** `resolve-provider`'s stderr prints a one-line summary, e.g. `resolve-provider: macOS route = overflow (BUSY=2 >= 2); selector = "namespace-profile-generouscorp-macos"`. Find it via the GitHub Actions UI under the `resolve-provider` job's log, or:

```bash
gh run view <run-id> --log --repo Generous-Corp/pulp | grep "macOS route"
```

**Manual overflow / rescue** is still available via `shipyard rescue <PR>` and remains useful for in-flight PRs that queued before the overflow logic kicked in. With Plan B in place, manual rescue should be needed much less frequently.

### `pulp overflow` — operator surface

`tools/cli/cmd_overflow.cpp` wraps the three repo variables behind a discoverable CLI:

```bash
# Show current routing state (local target, overflow target, threshold,
# plus self-hosted runner registration if visible to the default token):
pulp overflow status

# Turn overflow on (defaults to free GH-hosted "macos-15"):
pulp overflow enable
pulp overflow enable --to '"namespace-profile-generouscorp-macos"'   # paid Namespace

# Turn overflow off — every macOS leg goes to the local target.
# In-flight cloud jobs continue to completion; only new dispatches change.
pulp overflow disable

# Read / set the BUSY threshold (default 2; set to 1 for single-runner setups):
pulp overflow threshold
pulp overflow threshold 1
```

`pulp overflow disable` does not cancel in-flight cloud runs — it's a
config-change only. To force a currently-routed-to-cloud PR back to local,
use `pulp macos retarget --pr N --to local` (see "Per-PR macOS retargeting"
below).

## Per-PR macOS retargeting (`pulp macos`)

For the case where automatic overflow picked the "wrong" pool — e.g. you want to push a specific PR to Namespace for paid-fast turnaround, or pull a queued GH-hosted job back to the local Mac because local just freed up — use the **`build-macos.yml`** workflow + the **`pulp macos`** CLI:

```bash
# Switch PR's macOS leg to the local self-hosted Mac, freeing the GH-hosted slot:
pulp macos retarget --pr 1910 --to local

# Pay to skip the queue (Namespace billable, fast parallel):
pulp macos retarget --pr 1910 --to namespace

# Force GH-hosted macos-15 (free, slower):
pulp macos retarget --pr 1910 --to github-hosted

# See where the current macOS check is routed:
pulp macos status --pr 1910
```

`pulp macos retarget` cancels any in-flight macOS-bearing workflow_run for the PR and fires a fresh `build-macos.yml` dispatch on the chosen runner. Branch protection's required `macos` check is satisfied by whichever workflow most recently produced that check name, so retargeting supersedes the previous macOS leg **without re-running Linux/Windows**.

`build-macos.yml` is independent of `build.yml`'s matrix — they share check names but not workflow_runs. The matrix workflow continues running Linux/Windows as usual; only the macOS leg is replaced.

Workflow inputs (visible in `gh workflow run build-macos.yml --help`):

| Input | Default | Effect |
|-------|---------|--------|
| `runner` | `local` | Routes to `PULP_LOCAL_MACOS_RUNS_ON_JSON` |
| `runner=namespace` | — | Routes to `PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON` |
| `runner=github-hosted` | — | Routes to `"macos-15"` (free GH-hosted) |
| `target_ref` | (workflow's `ref`) | Branch / SHA to build |

### Opportunistic reroute daemon

`tools/scripts/macos_reroute_watcher.py` is a long-running watcher (intended as a launchd agent on the self-hosted Mac) that automates the "when local frees up, claw back queued GH-hosted jobs" pattern. It polls every 30 seconds:

1. Is the local Mac runner idle? (process-based detection via `ps`; no admin token needed.)
2. Is there a queued Build-and-Test workflow_run whose macOS job has `macos-15` (or `nscloud-*` / `namespace-profile-*`) labels — i.e., dispatched to cloud but not yet picked up?

When both conditions hold, the watcher invokes `pulp macos retarget --pr N --to local` to cancel the cloud dispatch and rerun on local with warm caches. A 5-minute flap-guard prevents repeatedly bouncing the same PR.

**Install (one-time per host):**

```bash
# Copy the template into LaunchAgents, substituting your Pulp checkout path:
sed "s|\$PULP_REPO|$PWD|g" \
  tools/launchd/pulp-macos-reroute-watcher.plist.template \
  > ~/Library/LaunchAgents/com.danielraffel.pulp.macos-reroute-watcher.plist

launchctl load ~/Library/LaunchAgents/com.danielraffel.pulp.macos-reroute-watcher.plist

# Logs:
tail -F ~/Library/Logs/pulp/macos-reroute-watcher.log
```

**Run by hand for testing:**

```bash
python3 tools/scripts/macos_reroute_watcher.py --interval 30 --log-level DEBUG
```

**Stop:**

```bash
launchctl unload ~/Library/LaunchAgents/com.danielraffel.pulp.macos-reroute-watcher.plist
```

The watcher is **safe to run alongside the overflow probe in `build.yml`** — they cooperate. The probe decides *where to dispatch initially*; the watcher *opportunistically reroutes* dispatches that landed on cloud while local was busy, once local frees up before the cloud runner has picked up the job. If the cloud runner has already started, the watcher takes no action.

### Self-hosted runner operations: prevent, recover, maintain

Shipyard v0.55.0+ is the minimum pin for the full self-hosted-runner
operational toolkit, and Pulp pins v0.56.2+ so rescue, update, and
`shipyard wait pr` have REST fallback paths when GraphQL is rate-limited. The
commands are discoverable from `shipyard --help` and replace the legacy
`planning/scripts/runner-watchdog.sh` + manual reinstall workflow.

```bash
# Recover one PR whose required macOS check is wedged or stale
shipyard rescue <PR>                      # cancel queued runs + redispatch to github-hosted
shipyard rescue <PR> --rerun-failed       # also re-arm cancelled/failed runs
shipyard rescue <PR> --dry-run            # preview without acting
shipyard rescue --all-stuck               # repo-wide stuck-run sweep
shipyard rescue <PR> --to github-hosted   # explicit destination provider

# Prevent future wedges on a self-hosted runner host
shipyard runner watch --kill-hung-workers # implies --fix; pair with launchd/systemd

# Keep the installed Shipyard CLI current
shipyard update --check --json            # report installed vs available
shipyard update                           # apply latest stable
shipyard update --to v0.56.2              # pin or roll back to Pulp's minimum
shipyard update --dry-run                 # plan only

# Wait after handoff/rescue without depending solely on GraphQL
shipyard wait pr <PR> --state green       # REST fallback as of v0.56.2
```

### Off-fleet queue-age watchdog (`runner-health-check.yml`)

`.github/workflows/runner-health-check.yml` sweeps every 30 minutes and opens a
tracking issue when a lane has stopped serving work. It runs on `ubuntu-latest`
**on purpose**: a guard that lives on the fleet dies with the fleet, so the one
outage it exists to report would be the outage that silences it. It is the
symptom-level backstop under the recovery tooling above — `shipyard rescue` and
`runner watch` fix a wedge you already know about; this tells you a wedge
exists.

**Why queue age, and not a runner-label check.** The macOS lanes are
JIT/ephemeral: a runner registers with GitHub only while it serves a job. So
"zero runners carry label `pulp-studio-01`" is *both* the healthy-idle state and
the dead-lane state, and nothing on GitHub's side can tell them apart. A
label-satisfiability probe therefore false-alarms every idle night and gets
muted within a week. Queue age is the observable that separates alive from dead,
and it is cause-agnostic — it catches causes nobody has enumerated yet, not just
the one that happened last time.

**Why it stays quiet on a busy afternoon.** A deep queue on a healthy pool is
normal: the measured baseline on this repo under normal load is a median queue
age of 5 min, an oldest of 31 min, and 3 runs past 30 min. A naive "queued > 30
min" rule alarms on that. So an alarm requires **two independent conditions**:

1. **Age** — the job has waited past `alarm_minutes` (default **45**, roughly
   1.5x the observed healthy maximum).
2. **Liveness** — its lane shows no sign of life: nothing with comparable labels
   is `in_progress`, and nothing with comparable labels has *started* since the
   job queued.

The liveness condition carries the false-alarm load, which is what lets the age
threshold stay tight enough to detect a dead lane within 45–75 minutes. A
saturated pool keeps its runners visibly busy, so it stays quiet at any queue
depth; one runner grinding on a 90-minute job is alive, not dead; an idle fleet
has nothing queued and so says nothing. Only "work piling up with nothing
serving it" alarms. Findings between 30 and 45 minutes appear in the run summary
only, never on the issue.

The issue is edited in place each sweep and closes automatically on recovery —
the same open/update/auto-close contract as the release watchdogs (see
[release-watchdog.md](release-watchdog.md)). The report names the labels the
stalled jobs asked for, so a human sees *which* lane is sick.

Thresholds and analysis live in `tools/scripts/queue_age_watchdog.py`, tested by
`tools/scripts/test_queue_age_watchdog.py` — which pins the measured baseline
above as a must-stay-quiet regression case, so a future threshold edit that
would re-introduce afternoon false alarms fails at PR time.

```bash
# Tune or dry-run a sweep by hand
gh workflow run runner-health-check.yml -f dry_run=true
gh workflow run runner-health-check.yml -f alarm_minutes=60

# Replay a recorded snapshot offline (no API calls, verdict pinned to capture time)
python3 tools/scripts/queue_age_watchdog.py --snapshot snapshot.json
```

### Diagnosing a VM lane: idle looks exactly like dead

The macOS and Linux VM lanes are **JIT** — a runner registers with GitHub only
while serving one job, then deregisters. A runner census therefore cannot tell a
healthy idle lane from a dead one: "zero runners carry `pulp-build-vm`" is both
states at once. Do **not** conclude a lane is dead from `actions/runners`, and do
not build a label-satisfiability alarm on a JIT label — it would fire every idle
night. Satisfiability is a valid check only for the **persistent** bare-metal
Studios.

The signal that separates alive from dead on a JIT lane is **queue age** (not
queue depth — 40 queued runs with a 5-minute median is healthy churn from many
concurrent agents). Baseline measured on a healthy busy pool (2026-07-16):
median queue age 5 min, oldest 31 min, 3 runs over 30 min. A naive
">30 min = broken" threshold alarms on that healthy pool; calibrate above it.

To check a lane host-side:

```bash
# Non-interactive ssh does NOT source .zprofile, so it lacks /opt/homebrew/bin
# and will falsely report "tart is not installed". Always use a login shell:
ssh <host> 'zsh -lc "launchctl list | grep -E \"tart-runner|qemu-runner\""'
```

Last-exit `0` means the supervisor is healthy and the lane is alive regardless of
what the runner census says.

### Runner agent crash-loops with exit 75 (the `/usr/sbin` PATH trap)

**Symptom:** a runner LaunchAgent shows last-exit **75** (`EX_TEMPFAIL`) and
crash-loops under `KeepAlive`; its log says `lease denied … rc=2`; no VM ever
boots; jobs queue on that lane forever.

**Cause:** tartci's `host_profile.py` shells bare `sysctl` — which lives at
**`/usr/sbin/sysctl`** — to read `hw.ncpu` / `hw.memsize`. macOS launchd agents
run with a minimal PATH, and the generated plist's PATH omits `/usr/sbin`:

```
/Users/<u>/.config/tartci/ghapp-shim:/opt/homebrew/bin:/usr/local/bin:/Users/<u>/.local/bin:/usr/bin:/bin
```

`sysctl` raises `FileNotFoundError` → `host_profile.py` exits 1 → the tartci
lease governor cannot compute a memory budget → it denies every lease (failing
**closed**, which is correct) → no VM ever starts.

**Diagnose this FIRST** — before suspecting tart, the network, or auth:

```bash
launchctl list | grep -E 'tart-runner|qemu-runner'   # last-exit 75 = this bug
# then inspect EnvironmentVariables:PATH in the agent's plist for /usr/sbin
```

**Fix:** append `:/usr/sbin:/sbin` to the plist's PATH and reload the agent. Exit
goes 75 → 0, the log turns to `lease acquired … cores=6 mem_mb=8192`, a VM boots,
and the queue drains. The failing set is exactly the `/usr/sbin`-missing set:
agents that already carry it are exit 0.

### `TART_HOME` is per-host by design

VM homes differ intentionally per machine:

| Host | `TART_HOME` | Storage |
|---|---|---|
| m3 (Studios) | `/Volumes/Workshop/VMs` | external SSD |
| m1, m5 | `~/VMs` | internal SSD |

**Always set `TART_HOME` explicitly when invoking `tools/ci/*.sh` on a VM host.**
Those scripts carry contradictory hardcoded defaults — most assume
`/Volumes/Workshop/VMs`, while `reap-stray-vms.sh` and `setup-ci-host.sh` assume
`$HOME/VMs`. The worst failure mode is silent: on m3, `reap-stray-vms.sh`
defaults to `$HOME/VMs`, which is empty on that host, so the stray-VM reaper
inspects the wrong universe, reaps nothing, and exits 0 reporting success — a
permanent no-op that looks like a pass.

The principle: a default is an undeclared name wearing a trench coat. On a VM
host an unset `TART_HOME` should be a loud hard error naming the fix, never a
guess. The repo holds RULES; the host holds VALUES — per-host truth belongs in
the tartci host profile, not a repo constant.

These three traps share one shape, covered in the `ci` skill under "The unifying
invariant — no name without a heartbeat": a name is trustworthy only if an
automated process dereferences it on a schedule and alarms on failure.

### Off-fleet merge-stall watchdog (`merge-stall-check.yml`)

`.github/workflows/merge-stall-check.yml` sweeps every 30 minutes and opens a
tracking issue when PRs are **merge-ready but not merging**. It runs on
`ubuntu-latest` for the same reason as the queue-age watchdog: the wedge it
catches lives in whatever presses the merge button (Shipyard's per-host
queue-tick), so an on-fleet guard would die with the thing it watches.

**The gap it closes — the opposite shape from the queue-age watchdog.** The
queue-age guard alarms on a *dead runner lane*: jobs sitting **queued** because
runners died. This one alarms on the inverse: every required check **green**,
nothing queued, and still nothing merging — the signature of an auto-merger
silently held in reap-only mode. No job-level signal sees "everything is green
and nobody is merging"; the only observable is a population of merge-ready PRs
that stays merge-ready and unmerged. (Motivating incident: the repo went ~4
hours with 34 PRs open and nothing merging while every check was green.)

**The alarm predicate.** A PR trips only when ALL hold:

1. **Required checks green** — every check in the repo's REQUIRED set. That set
   is read from branch protection at runtime, not hardcoded; if the token cannot
   read protection rules it falls back to the documented `main` set (`macos`,
   `Enforce version & skill sync`).
2. **`mergeStateStatus` in `{CLEAN, BEHIND}`** — GitHub's own merge verdict.
   `DIRTY` (conflicts), `BLOCKED` (a required check red/missing/review pending),
   and `UNSTABLE` (a non-required check still moving) are excluded — those wait
   on something real, not on the merger.
3. **Auto-merge enabled** — the signal that a machine, not a human, owns pressing
   merge. A green PR without it is waiting on a person and must not alarm.
4. **Merge-ready longer than the threshold** (default **45 min**), measured from
   the completion time of the last required check to go green — a real duration,
   independent of the sweep cadence.

**Why two consecutive sweeps.** A single snapshot can misread — a per-PR REST
poll of merge state gets rate-limited and returns *false* CLEAN/BEHIND readings
under load, which is exactly how the incident state looked wrong. Collection
therefore uses **one GraphQL call** for every open PR's `mergeStateStatus`
(`tools/scripts/merge_stall_watchdog.py`), and on top of that a PR must satisfy
the full predicate on **two consecutive sweeps** before it is issue-worthy: the
first qualifying sweep records it as *pending* (run-summary only), the second
promotes it to *alarm*. A normal in-flight PR that merges within a tick never
reaches the second observation, so it never trips. The cross-sweep memory is the
set of stuck PR numbers, persisted as a workflow artifact — crash-safe, held by
GitHub independently of this repo or any host.

The issue is edited in place each sweep and closes automatically once no PR is
stuck merge-ready — the same open/update/auto-close contract as the release
watchdogs (see [release-watchdog.md](release-watchdog.md)).

Analysis and the predicate live in `tools/scripts/merge_stall_watchdog.py`,
tested by `tools/scripts/test_merge_stall_watchdog.py` — which pins the
must-stay-quiet cases (young PR, DIRTY, BLOCKED, no auto-merge, single-sweep
blip) as regressions so a future edit that would make the guard cry wolf fails
at PR time. The script also carries an inert, clearly-marked stub for a **second
condition to add once a GitHub merge queue is enabled** ("queue depth > 0 AND no
`merge_group` check started in 30 min" — a wedged *queue*, distinct from a wedged
auto-merger); it stays off until the queue is live.

```bash
# Dry-run a sweep by hand (log findings, do not touch the issue)
gh workflow run merge-stall-check.yml -f dry_run=true
gh workflow run merge-stall-check.yml -f threshold_minutes=60

# Replay a recorded snapshot offline (no API calls, verdict pinned to capture time)
python3 tools/scripts/merge_stall_watchdog.py --snapshot snapshot.json --prev-state state.json
```

### GraphQL quota fallback for PR sweeps

The `gh pr ... --json` and `gh pr merge` paths can consume or require GitHub's
GraphQL quota. That quota is separate from the REST `core` quota and can hit
zero while REST still has thousands of calls available.

When a broad PR sweep hits GraphQL exhaustion, switch the sweep to REST instead
of waiting:

```bash
gh api rate_limit --jq '.resources | {core, graphql}'
gh api repos/OWNER/REPO/pulls/PR
gh api repos/OWNER/REPO/commits/SHA/check-runs?per_page=100
gh api repos/OWNER/REPO/actions/jobs/JOB_ID/logs
```

For a PR already verified green through REST, merge through the REST endpoint:

```bash
head_sha=$(gh api repos/OWNER/REPO/pulls/PR --jq '.head.sha')
gh api repos/OWNER/REPO/pulls/PR/merge \
  -X PUT \
  -f sha="$head_sha" \
  -f merge_method=squash \
  -f commit_title='subject (#PR)'
```

If the merge endpoint returns `405 Base branch was modified`, refresh the PR
state and check runs through REST, recompute `head_sha`, then retry once only
if the refreshed head SHA and green status are still the values you intend to
merge. This is a transport fallback, not a validation bypass: do not merge
around real CI, coverage, sanitizer, or review failures.

Use `shipyard rescue` when a PR is otherwise ready but blocked by queued,
cancelled, or failed runner contexts caused by a self-hosted-runner wedge. It is
the PR-side recovery path and avoids the old failure mode where cancelling
queued runs left required checks stuck as `failure`.

Use `shipyard runner watch --kill-hung-workers` on the runner host itself. It
auto-cancels stale queued runs and kills hung `Runner.Worker` processes through
Shipyard's safe recovery sequence: snapshot, SIGTERM, grace period, SIGKILL,
child reaping, partial-build quarantine, Listener verification, and optional
wait for GitHub status to flip. Its JSON output uses `runner.watch` envelopes
with `event=auto_kill_worker` and `phase` values of `attempt`, `killed`,
`failed`, or `no-pid-found`.

Use `shipyard update` instead of the old ad hoc `curl install.sh | sh` path
once a machine already has Shipyard installed. Pulp still records the canonical
repo pin in `tools/shipyard.toml`; `shipyard update --check --json` is the
machine-local drift check, while `shipyard pin bump --to vX.Y.Z` is the repo
pin-change workflow.

## Required Merge Process (All Agents)

Every change to `main` must go through this workflow — no exceptions:

1. **Branch** — work on `feature/*` or `fix/*`, never directly on main
2. **Ship** — run `shipyard pr` to create and track the PR, validate on macOS + Ubuntu + Windows, and merge on green
3. **GitHub Actions** — PR also triggers build+test CI on all 3 platforms (redundant safety net)

The `ci` skill (`.agents/skills/ci/SKILL.md`) captures this as the authoritative trigger list — natural-language phrases like "ship this", "push a PR", "we're done", and "run CI" all route through `shipyard pr`.

## Legacy: pulp ci-local

`tools/local-ci/local_ci.py` is the previous CI controller. It remains
available as a fallback while the Shipyard path finishes replacing it, but it
is scheduled for removal.

## TL;DR

- `pulp ci-local run` queues the current `HEAD` in a machine-global queue shared by every worktree on that Mac.
- `pulp ci-local run <branch>` queues that branch tip's exact commit SHA, not the launching checkout's `HEAD`.
- `pulp ci-local run --smoke` queues a fast clean install/export preflight instead of a full test run.
- The queue serializes jobs, not targets. One CI job runs at a time, but its requested targets (`mac`, `ubuntu`, `windows`) run in parallel inside that job.
- Mac runs locally. Ubuntu and Windows run over SSH against repos you already cloned on those machines.
- Remote targets validate the exact queued git SHA, not "whatever the branch points to later". The runner uploads that SHA as a git bundle before validation, so full-matrix checks do not depend on the host already seeing your latest branch tip.
- `pulp ci-local status` shows the active runner, pending jobs, SSH/VM reachability, and live per-target state for the running job. `pulp ci-local bump <job-id> high` moves a pending job forward.
- queueing now prints the submission root, current cwd, config path/source, and per-target host preflight before a job is recorded
- queueing fails fast if you launched from the wrong git root or selected an SSH target that is currently unreachable with no fallback, unless you explicitly override that safety check
- While a job is running, `pulp ci-local status` also shows live per-target state such as `mac=pass, ubuntu=pass, windows=running`.
- Quiet long-running targets now emit runner heartbeats, so `status` can show `heartbeat=...`, `idle=...`, and `liveness=quiet|stuck` even when the underlying toolchain has not printed a new line recently.
- If you queue a newer SHA for the same branch, targets, and validation mode, older pending work is superseded automatically instead of sitting behind it forever.
- `pulp ci-local logs <job-id> --target windows` tails the saved per-target log from the machine-global CI state dir, so you do not need ad hoc SSH just to see whether a target is building or testing.
- `pulp ci-local evidence [branch]` shows the last-good exact-SHA target evidence already recorded for a branch, so you can keep earlier same-SHA passes instead of rerunning them blindly.
- `pulp ci-local cleanup` shows reclaimable local-CI disk usage without deleting anything; `--apply` is blocked while jobs are running.
- `pulp ci-local cloud workflows` lists the GitHub Actions workflows that the local CI control plane knows how to dispatch, plus which runner providers each one supports.
- `pulp ci-local cloud run <workflow> [branch]` dispatches a GitHub Actions workflow deliberately when workflow semantics or neutral-host confirmation matter more than the local queue.
- `pulp ci-local cloud status` shows the latest tracked GitHub Actions dispatches that this machine has launched; `pulp ci-local status` includes the same recent cloud summary alongside local queue state.
- Persistent local CI hosts now keep a prepared root per `target + validation` so a narrow same-SHA rerun can reuse earlier work instead of rematerializing from scratch.
- If a runner is interrupted, the queued job keeps its last-known per-target state so you can see what already passed before deciding whether to rerun everything or just the remaining target.
- Jobs submitted through `pulp ci-local` are globally queued, and validation itself now takes a per-host lock on macOS/Linux plus a Windows host mutex, so old `validate-build.sh` runs wait instead of colliding.
- SSH targets receive a per-job git bundle before validation. That keeps exact-SHA validation working even when the host validates from a stale local mirror instead of GitHub directly.
- Windows SSH jobs execute from short detached worktrees under `C:\pulp-ci`, and stale worktree metadata is pruned automatically before reruns.
- If a stale runner leaves behind an old Windows validator, the next drain pass now targets that specific remote validator PID for cleanup before starting new work, and `status` keeps the cleanup result visible.
- For Windows SSH validation, choose the configured target whose non-interactive PowerShell context resolves `git`, `cmake`, and `ctest`. Keep those host aliases local to your environment; shared repo docs should describe the selection rule, not your personal machine names.
- Reuse is a persistent-host feature for local macOS and SSH-backed/self-hosted hosts. Ephemeral cloud runners should keep the default clean path unless a later policy explicitly opts them in.
- Truly raw ad hoc `ssh`, `cmake`, or custom background processes still bypass coordination until they are stopped or migrated.

## Why local instead of cloud

Pulp has GitHub Actions workflows for CI, but running them on every branch costs money. Local CI is free and faster for iterative development — you get results in minutes from machines you already own or have running locally. Cloud CI remains available for release branches, public PRs, and the narrow cases where you need workflow-level or neutral-host confirmation.

Cloud orchestration is now available through the same control plane:

- `pulp ci-local cloud workflows`
- `pulp ci-local cloud run <workflow> [branch]`
- `pulp ci-local cloud status [dispatch-id|latest]`

That cloud surface is intentionally separate from the local queue. `run`,
`check`, `ship`, `enqueue`, and `drain` still operate on the exact-SHA
local/SSH queue. `cloud run` dispatches GitHub Actions explicitly and tracks the
result beside local CI state instead of pretending a hosted workflow is just
another local target.

Namespace is now wired into the deliberate cloud companion path for both
`docs-check.yml` and `build.yml`. The normal day-to-day default remains
local-first: macOS runs locally, while deliberate cloud dispatches can route
Linux/Windows through Namespace and keep macOS local unless you opt into a
one-off cloud macOS selector.

## How it works

When you run `pulp ci-local`, it:

1. Queues a job in a machine-global queue shared by every worktree on that Mac
2. Prints the exact queue intent first: submission root, cwd, config path/source, and remote-host preflight
3. Runs only one queue drain owner at a time, so separate agents do not stampede the same Mac and VMs
4. Validates locally on Mac via `./validate-build.sh --ref <sha>`
5. For each SSH target in `config.json`: uploads a per-job git bundle, injects that exact SHA into the configured repo on the host, then validates it there
6. If an SSH target is unreachable, it tries to start the corresponding UTM VM, waits for it to boot, then retries the SSH connection
7. Drains queued work on login or wake if you install the launchd agent

Mac validation always runs. SSH targets are skipped if disabled in config.

## GitHub Actions companion

Use the `cloud` subcommands when you want GitHub Actions as the orchestrator,
not when you want another exact-SHA local queue job:

```bash
pulp ci-local cloud workflows
pulp ci-local cloud defaults
pulp ci-local cloud history
pulp ci-local cloud compare build
pulp ci-local cloud recommend build
pulp ci-local cloud run build feature/my-branch
pulp ci-local cloud run build feature/my-branch --provider namespace
pulp ci-local cloud run build feature/my-branch --provider namespace --macos-runner-selector-json '"namespace-profile-big-apple"'
pulp ci-local cloud run build feature/my-branch --provider namespace --macos-runner-selector-json '"nscloud-macos-tahoe-arm64-6x14"'
pulp ci-local cloud run docs-check feature/my-branch --provider namespace --wait
pulp ci-local cloud run docs-check feature/my-branch --provider namespace --runner-selector-json '"namespace-profile-big-apple"'
pulp ci-local cloud namespace doctor
pulp ci-local cloud namespace setup
pulp ci-local cloud status
pulp ci-local cloud status latest --refresh
```

Important constraints in the current phase:

- `cloud run` dispatches by branch name, not by a detached exact SHA
- cloud dispatch records are persisted under the same machine-global CI state
  directory as local results, but they do not enter `queue.json`
- local `status` remains fast and local-first; it shows the latest tracked cloud
  summaries without hitting GitHub unless you explicitly run `cloud status --refresh`
- `cloud defaults` shows the effective workflow/provider defaults plus where the
  current selector values came from (local config versus repo-variable fallback)
- `cloud history` shows recent tracked cloud runs with saved timing plus any
  configured estimated cost line items
- `cloud compare <workflow>` rolls up observed provider medians for a workflow
  from tracked run history
- `cloud recommend <workflow>` suggests a provider from recorded cloud history;
  it is intentionally conservative and uses observed medians instead of
  hardcoded guesses
- `cloud status` now reports Namespace runtime/machine-shape truth when the run
  was launched on Namespace and `nsc` can see the matching instances
- tracked cloud runs now persist queue-delay and elapsed-duration timing so the
  later comparison view can answer "how long did GitHub-hosted vs Namespace
  take?" from saved run history instead of rough notes
- estimated cost output is opt-in via local config; every estimate is labeled
  `estimated; verify provider pricing`
- if the provider CLI does not expose billing totals, Pulp keeps reporting
  runtime and machine shape instead of inventing invoice truth
- if a Namespace dispatch dies in `resolve-provider` before any matrix leg starts,
  inspect the GitHub run annotations first; provider billing or control-plane
  failures are a different problem from repo or workflow breakage
- `build.yml` now accepts `runner_provider` and routes Linux and Windows through
  the selected provider; macOS is omitted from the cloud build by default so it
  can stay local-first
- **Default provider for PR checks** is controlled by the GitHub repo variable
  `PULP_DEFAULT_RUNNER_PROVIDER`. Set it to `namespace` to route all PR checks
  through Namespace runners (faster, parallel). Set to `github-hosted` to use
  GitHub-hosted runners (free tier, queued). The `workflow_dispatch` input
  overrides this for manual runs. To change the default:
  ```bash
  # Switch to Namespace (recommended for faster CI)
  gh variable set PULP_DEFAULT_RUNNER_PROVIDER --body "namespace"
  
  # Switch back to GitHub-hosted
  gh variable set PULP_DEFAULT_RUNNER_PROVIDER --body "github-hosted"
  ```
- `build` also accepts one-off leg overrides:
  `--linux-runner-selector-json`, `--windows-runner-selector-json`, and
  `--macos-runner-selector-json`; that means you can keep the normal
  Linux/Windows Namespace + macOS local default and still do an explicit
  one-off macOS Namespace build without changing saved config
- those one-off selector overrides can be either:
  a Namespace profile label such as `"namespace-profile-generouscorp-macos"`,
  or a direct Namespace machine label such as
  `"nscloud-macos-tahoe-arm64-6x14"`
- `docs-check` accepts an explicit `--runner-selector-json` override, for example
  `"namespace-profile-default"` or `["self-hosted","linux"]`
- if no explicit selector is passed, `docs-check` falls back to
  `github_actions.workflows.docs-check.providers.<provider>.runner_selector_json`
  in local config when present, then to the repo variable
  `PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON` for the Namespace provider
- `build` can take Linux/Windows Namespace selectors from
  `github_actions.workflows.build.providers.namespace.linux_runner_selector_json`
  and `.windows_runner_selector_json` in local config, and the workflow also
  supports repo-variable fallbacks
  `PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON` and
  `PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON`
- macOS Namespace is an explicit validation path, not part of the default cloud
  build: if you want to test macOS on Namespace, provide
  `--macos-runner-selector-json`, or set
  `github_actions.workflows.build.providers.namespace.macos_runner_selector_json`
  in local config, or `PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON`
- make sure that selector points at a real macOS-capable Namespace profile:
  GitHub job names alone do not guarantee the underlying OS, and a Linux-backed
  profile can still satisfy the `runs-on` label while executing the leg on
  Linux instead of macOS
- if you want macOS to stay local-first by default, leave the macOS selector
  unset in shared config and repo variables, and pass
  `--macos-runner-selector-json` only for one-off validation runs
- for the Namespace path, install the `nsc` CLI and run `nsc login` first
  before trying to route work there; that is the recommended operator setup path
  for this pilot
- SSH/VM target topology and Namespace provider setup stay separate:
  `targets.*` still configures local/SSH validation hosts, while Namespace
  provider routing lives under the GitHub Actions workflow/provider config and
  the `cloud namespace` helper commands

## Fast-CI vs full-CI (`build.yml`)

The `Build and Test` workflow has two test trajectories without forking the
YAML:

- **Fast-CI** runs on `pull_request` events. The ctest invocation
  excludes BOTH the `validation` and `slow` CTest labels, dropping the
  longest-running tests so PR cycle time stays tight. Examples that
  carry `LABELS slow` today (defined in `test/CMakeLists.txt`):
  - `cmake-ios-auv3-configure` — fresh-cache ~3 min iOS-leg configure
  - `cmake-pulp-add-binary-data-encoder` / `cmake-pulp-install-layout`
  - `pulp-test-hot-reload`, `pulp-test-scripted-ui`, `pulp-test-scan-cache`,
    `pulp-test-scan-blacklist` (filesystem-mtime sleep loops)
  - `pulp-test-sync`, `pulp-test-sync-race-hammer`,
    `pulp-test-events-timer-helpers` (race + timer hammers; also covered
    under sanitizer.yml's TSan lane)

- **Full-CI** runs on `push` to `main`, the nightly schedule, and
  `workflow_dispatch`. Only the `validation` label is excluded — every
  `slow`-labelled test runs before code lands on the release lane.

Both paths satisfy the branch-protection-required `macos` /
advisory `linux` / advisory `windows` alias gates because the alias
jobs read each matrix leg's outcome via the GitHub API.

### Tagging a new test as slow

Add `LABELS slow` either to a single test's `set_tests_properties`, or
to a Catch2 binary's `catch_discover_tests(... PROPERTIES LABELS slow)`
so every discovered test inherits the label:

```cmake
add_test(NAME my-expensive-cmake-smoke COMMAND ...)
set_tests_properties(my-expensive-cmake-smoke PROPERTIES
    LABELS "smoke;slow"
    TIMEOUT 600)

add_executable(pulp-test-my-suite test_my_suite.cpp)
target_link_libraries(pulp-test-my-suite PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-my-suite PROPERTIES LABELS slow)
```

Rule of thumb for `slow`: a test consistently >5 sec on at least one
platform, OR a sleep-bounded smoke (file-mtime, hammer race, message-loop
bound) whose value lies in soak coverage rather than per-PR feedback.
Anything covered by sanitizers.yml or another scheduled lane is a
strong candidate.

### Demoting a fast test to slow (or vice versa)

`ctest --test-dir build -L slow -N` lists every test currently tagged
`slow`. To move a test in or out of the fast-CI surface, add or remove
the `slow` label in `test/CMakeLists.txt` (or the appropriate subdir
CMakeLists) and reconfigure. There's no separate registry to keep in
sync.

## Switching a job's runner without a code change

Every runner-selection decision in `.github/workflows/*.yml` is driven by
`tools/scripts/resolve_runs_on.py`. That means the runner for any job
below can be flipped between **GitHub-hosted**, **Namespace**, and **local
self-hosted** by setting a repository variable — no workflow edit, no
full-matrix re-run, no PR. This is the fluidity we want: move a job
mid-incident without touching code.

### Precedence (identical for every variable below)

For each target the resolver checks, in this order:

1. A `workflow_dispatch` input (if present on the workflow) — one-off override.
2. The target's repository variable (the `PULP_*_RUNS_ON_JSON` values below).
3. For the build matrix only: `PULP_DEFAULT_RUNNER_PROVIDER` + the
   provider's selector var (`PULP_NAMESPACE_*` or `PULP_LOCAL_*`).
4. A hard-coded default label (e.g. `macos-14`, `ubuntu-24.04`, `macos-15`).

**When every variable below is unset, the workflows resolve to exactly the
hard-coded defaults they had before this mechanism was lifted into a
shared resolver. Nothing changes by default.** Setting one variable moves
one job. Nothing more.

Coverage is stricter than the build matrix. It reads explicit
`workflow_dispatch` inputs and `PULP_COVERAGE_*_RUNS_ON_JSON`, not
`PULP_NAMESPACE_BUILD_*`. If coverage moves local, use a dedicated ephemeral
label such as `pulp-coverage-vm-macos`; do not point coverage at `pulp-build`,
`pulp-build-vm`, or the warm macOS gate pool.

### Global default (`build.yml` matrix only)

| Variable | Effect | Example |
|---|---|---|
| `PULP_DEFAULT_RUNNER_PROVIDER` | Default provider for Linux and Windows legs of `build.yml`. One of `github-hosted` \| `namespace` \| `local`. Falls back to `github-hosted` when unset. | `gh variable set PULP_DEFAULT_RUNNER_PROVIDER --body "namespace"` |

### `build.yml` — Linux / Windows / macOS legs

| Variable | Provider | Example |
|---|---|---|
| `PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON` | Namespace | `gh variable set PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON --body '["namespace-profile-generouscorp"]'` |
| `PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON` | Namespace | `gh variable set PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON --body '["namespace-profile-generouscorp-windows"]'` |
| `PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON` | Namespace (optional) | `gh variable set PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON --body '"namespace-profile-generouscorp-macos"'` |
| `PULP_LOCAL_MACOS_RUNS_ON_JSON` | Local macOS ARM64 primary pool (bare-metal Studios; see the live table under "macOS overflow routing") | `gh variable set PULP_LOCAL_MACOS_RUNS_ON_JSON --body '["self-hosted","macOS","ARM64","pulp-build","pulp-build-studio"]'` |
| `PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON` | Local macOS ARM64 JIT VM overflow pool. Unset → `build.yml` falls back to GitHub-hosted `["macos-15"]`; `local-only` disables overflow. | `gh variable set PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON --body '["self-hosted","macOS","ARM64","pulp-build","pulp-build-vm"]'` |
| `PULP_LOCAL_LINUX_RUNS_ON_JSON` | Local Linux ARM64 VM pool | `gh variable set PULP_LOCAL_LINUX_RUNS_ON_JSON --body '["self-hosted","Linux","ARM64","pulp-build-linux"]'` |
| `PULP_LOCAL_WINDOWS_RUNS_ON_JSON` | Local Windows ARM64 QEMU pool | `gh variable set PULP_LOCAL_WINDOWS_RUNS_ON_JSON --body '["self-hosted","Windows","ARM64","pulp-build-windows"]'` |

`.shipyard/ci-profiles/normal-local-fast.toml` is the repo-local, read-only
policy mirror. Its PR-only `github.windows-x64-runtime` target records the
stable `windows-2022` functional lane, while the shared
`github.windows-x64` target records the `windows-latest` coverage/scheduled
lane. The current Shipyard profile planner does not apply these selectors to
a dispatch; `build.yml` remains authoritative. Inspect the profile when
reviewing policy, then verify the workflow input and repository-variable
precedence before changing live routing.

Do not put ordered fallback chains directly into GitHub Actions. GitHub receives
one `runs-on` selector per job; Shipyard/tartci must resolve "Mac Studio, then
M5/blackbook, then GitHub" before dispatch or variable application.

Windows local QEMU is Windows ARM64. An x64 MSVC/Prism smoke can be useful, but
it is not a replacement for the GitHub-hosted Intel/x64 functional gate. The
required `build.yml` functional matrix is pinned to `windows-2022` so its CRT
and Visual Studio generation do not move underneath the complete runtime
suite. The standalone MSVC release-path, MIDI 2, and BLE compile gates remain
on `windows-latest`; release builds and the nightly Intel safety net also keep
tracking the newest hosted image.
`tools/scripts/test_windows_runner_policy.py` enforces this split across the
actual build, release, coverage, and nightly workflows plus the release runner
resolver and Shipyard mirror. It runs in `workflow-lint`, including when the
profile or the policy test itself changes, so these surfaces cannot drift while
an isolated mirror test remains green.

### Nightly GitHub Intel validation

`.github/workflows/cross-platform-check.yml` is the scheduled Linux/Windows
Intel safety net for this profile. It runs GitHub-hosted `ubuntu-latest` and
`windows-latest`, files or updates one deduped issue per broken platform, and
auto-closes the tracker when the platform recovers. Do not add a duplicate
nightly Intel workflow unless this one is deliberately retired.

### `sanitizers.yml` — per-sanitizer target selection

Each sanitizer job resolves independently. Setting one variable moves
exactly that sanitizer; the others stay on their defaults.

| Variable | Default label when unset | Example (dedicated sanitizer VM label) |
|---|---|---|
| `PULP_SANITIZER_ASAN_RUNS_ON_JSON` | `macos-14` | `gh variable set PULP_SANITIZER_ASAN_RUNS_ON_JSON --body '["self-hosted","macOS","ARM64","pulp-sanitizer-vm-macos"]'` |
| `PULP_SANITIZER_TSAN_RUNS_ON_JSON` | `macos-14` | `gh variable set PULP_SANITIZER_TSAN_RUNS_ON_JSON --body '["self-hosted","macOS","ARM64","pulp-sanitizer-vm-macos"]'` |
| `PULP_SANITIZER_UBSAN_RUNS_ON_JSON` | `macos-14` | `gh variable set PULP_SANITIZER_UBSAN_RUNS_ON_JSON --body '["self-hosted","macOS","ARM64","pulp-sanitizer-vm-macos"]'` |
| `PULP_SANITIZER_RTSAN_RUNS_ON_JSON` | `ubuntu-24.04` | `gh variable set PULP_SANITIZER_RTSAN_RUNS_ON_JSON --body '["self-hosted","linux","x64","sanitizer"]'` |

The three macOS sanitizers (ASan/TSan/UBSan) carry a `--deny-labels
pulp-build,pulp-build-vm` guard in `sanitizers.yml`'s resolver, so a
sanitizer can **never** be misrouted onto the required-gate pool (the
resolver hard-fails). They no longer read `PULP_NAMESPACE_BUILD_MACOS_*`
either; the per-sanitizer variable is the single switch.

**Capacity finding: localize at most one sanitizer, and only TSan.**
macOS allows only **two running macOS guests per host** (Apple's limit),
and both belong to the required `macos` build gate. A local sanitizer VM
is a *third* guest, so localizing is gated on the **tartci idle-gate**:
the `pulp-sanitizer-vm-macos` lane shares `TART_HOME` with the gate (a real
host-wide 2-guest semaphore) and yields its slot whenever the gate has
queued/in-progress work — it can never starve the required check the way
the coverage lane did. Pick **TSan**: it is the longest sanitizer (scoped
`-j1` serial, ~45 min on the 3 vCPU `macos-14`) and the highest value for a
real-time audio framework, and being single-core-bound it gains most from a
local M-series runner. The other three stay on `macos-15`: the four run in
**parallel** on GitHub but would **serialize** (~4×) on one cap=1 local lane,
which is slower than hosted except during a hosted backlog. Full parallel
local sanitizers would need a third macOS host. Roll out one sanitizer at a
time, each behind a measured go/no-go (gate queue latency + matrix
wall-clock).

### One-off overrides via `workflow_dispatch`

`sanitizers.yml` accepts one `*_runner_selector_json` input per
sanitizer. They win over the corresponding repo variable for a single
manual run:

```bash
gh workflow run sanitizers.yml \
  -f tsan_runner_selector_json='["self-hosted","macOS","ARM64","pulp-sanitizer-vm-macos"]'
```

Prove TSan green via this dispatch (with the `pulp-sanitizer-vm-macos`
LaunchAgent loaded and the tartci idle-gate present) **before** setting
`PULP_SANITIZER_TSAN_RUNS_ON_JSON`. The lane template is
`tools/launchd/pulp-tart-runner-sanitizer-macos.plist.template` (ships
parked — see its header for the load/quiet-window preconditions).

`coverage.yml` accepts `linux_runner_selector_json`,
`macos_runner_selector_json`, and `windows_runner_selector_json` inputs. The
macOS Tart coverage proof path is:

```bash
gh workflow run coverage.yml \
  -f macos_runner_selector_json='["self-hosted","macOS","ARM64","pulp-coverage-vm-macos"]'
```

Only set `PULP_COVERAGE_MACOS_RUNS_ON_JSON` to that selector after the proof
run uploads the `os-macos` Codecov flag. The coverage LaunchAgent uses
`--queue-match-labels` so existing hosted Coverage jobs do not accidentally
boot a local coverage VM.

`build.yml` has the equivalent `linux_runner_selector_json`,
`windows_runner_selector_json`, and `macos_runner_selector_json` inputs.
These are the same inputs already documented above; they are listed
here for completeness alongside the repo-variable knobs.

### Reverting

Unset the variable and the job falls back to the hard-coded default
immediately on the next run:

```bash
gh variable delete PULP_SANITIZER_TSAN_RUNS_ON_JSON
```

No code change is needed to revert, either.

### Registering a self-hosted Mac runner (appendix)

Flipping a job to `"self-hosted"` labels assumes those labels are
advertised by a running GitHub Actions runner somewhere. Register one
on the Mac you want the job to run on:

```bash
# 1. From repo Settings -> Actions -> Runners, click "New self-hosted runner"
#    to get a short-lived registration token. Then on the Mac:
mkdir -p ~/actions-runner && cd ~/actions-runner
curl -o actions-runner.tar.gz -L https://github.com/actions/runner/releases/latest/download/actions-runner-osx-arm64.tar.gz
tar xzf actions-runner.tar.gz

# 2. Register with labels that match the JSON you set in the repo var.
#    Example for the TSan / sanitizer lane:
./config.sh --url https://github.com/Generous-Corp/pulp \
            --token <REGISTRATION_TOKEN> \
            --name "$(hostname)-sanitizer" \
            --labels "self-hosted,macos,arm64,sanitizer" \
            --work _work

# 3. Install as a launchd service so it runs at login and survives reboots.
./svc.sh install
./svc.sh start
./svc.sh status
```

> **Operational note.** Self-hosted runners execute arbitrary code from
> any branch that can trigger the workflow. Use them on dedicated
> hardware / VMs you control, not shared personal machines. Apple
> Silicon hosts should prefer `arm64` labels so jobs don't try to
> match Intel-only labels.
>
> Agents do NOT register runners. Treat these commands as a human ops
> task documented here for completeness.

### Creating a Namespace macOS runner profile

Today, `nsc` can verify login/workspace state and inspect the instances created
by GitHub Actions, but it does not create or edit GitHub Actions runner
profiles from this workflow. Creating a new runner profile is currently a
Namespace dashboard step.

Use this path in Namespace:

- `GitHub Actions -> Profiles -> New Profile`

Recommended fields for the first macOS validation profile:

- Name in the UI: `generouscorp-macos`
- OS & Architecture: `macOS on Apple Silicon`
- Resources: `6 vCPU, 14 GB RAM`
- Base image: a recent Xcode/macOS image appropriate for your build
- Cache toggles: leave enabled unless you have a reason to turn them off

Important selector detail:

- the Namespace UI shows the profile name without the GitHub runner prefix
- the selector you pass to Pulp/GitHub Actions is the prefixed form
- example: UI profile `generouscorp-macos` becomes selector
  `"namespace-profile-generouscorp-macos"`
- for one-off experiments you can skip profile creation entirely and pass a
  direct machine label instead, for example:
  `"nscloud-macos-tahoe-arm64-6x14"`

After creating the profile, validate it with a one-off run:

```bash
pulp ci-local cloud run build feature/my-branch \
  --provider namespace \
  --macos-runner-selector-json '"namespace-profile-generouscorp-macos"'
```

Or use a direct machine label for an ad hoc run:

```bash
pulp ci-local cloud run build feature/my-branch \
  --provider namespace \
  --macos-runner-selector-json '"nscloud-macos-tahoe-arm64-6x14"'
```

Then confirm the backing instance shape with:

```bash
nsc instance history --all -o json --max_entries 10
```

For a real macOS runner, the matching entry should report:

- `user_label.nsc.runner-profile-tag = "namespace-profile-generouscorp-macos"`
- `shape.os = "macos"`
- `shape.machine_arch = "arm64"`

If it instead shows `linux/amd64`, the profile label is valid but the backing
runner is not a real macOS machine yet.

## Prerequisites

- [UTM](https://docs.getutm.app) — free VM manager for macOS (Apple Silicon and Intel)
- SSH key access to your VMs (password auth is not supported)
- The Pulp repo cloned on each VM at the path specified in `config.json`

UTM is the simplest option, but any SSH-reachable host works: Proxmox, a cloud VM (Azure/AWS/GCP), or a physical machine on your network. Cloud VMs cost money to run but are otherwise fully supported.

## Setup

### 1. Create your config

```bash
cp tools/local-ci/config.example.json tools/local-ci/config.json
```

Local CI now prefers a machine-global config at `~/Library/Application Support/Pulp/local-ci/config.json` on macOS (or the platform-equivalent `state_dir()/config.json`) so every worktree on the same machine sees the same host topology. `tools/local-ci/config.json` remains the fallback if no shared config exists, and `PULP_LOCAL_CI_CONFIG` still overrides both when you need an explicit one-off config.

Create the initial file from the example, then copy it to the shared state location if you want all worktrees to reuse it:

```bash
mkdir -p ~/Library/Application\\ Support/Pulp/local-ci
cp tools/local-ci/config.example.json ~/Library/Application\\ Support/Pulp/local-ci/config.json
```

Edit the chosen `config.json` and fill in your SSH hostnames and repo paths. The `host` field is the primary SSH target. `fallback_host`, if present, is tried next. The `utm_fallback` block is optional and is only used if SSH targets are unreachable.

Keep those aliases environment-local. Shared skills and docs should not hardcode your personal hostnames or VM names; they should explain how to choose the right target and where that target is configured.

The optional `github_actions.workflows.docs-check.providers.namespace.runner_selector_json`
value lets you set the default Namespace `runs-on` selector that `cloud run docs-check`
should dispatch when you do not pass `--runner-selector-json` explicitly.

### 1b. Optional estimated billing config

If you want per-run and billing-period cost estimates in `cloud status`,
`cloud history`, and `cloud compare`, fill in the `telemetry.billing` block in
your local config.

These numbers are estimates only. Verify provider pricing.

Example:

```json
{
  "telemetry": {
    "billing": {
      "enable_provider_reported_totals": false,
      "currency": "USD",
      "billing_period_start_day": 1,
      "github_hosted_job_os_rates_per_minute": {
        "linux": 0.008,
        "windows": 0.016,
        "macos": 0.08
      },
      "namespace_profile_tag_rates_per_hour": {
        "namespace-profile-generouscorp": 0.50,
        "namespace-profile-generouscorp-macos": 1.20
      }
    }
  }
}
```

Notes:

- GitHub-hosted estimates use per-job OS rates when Pulp can infer the runner OS
- Namespace estimates prefer a profile-tag hourly rate and fall back to a
  machine-shape rule if you configured one
- if no matching rate exists, the CLI prints `cost: unavailable (...)`
- `enable_provider_reported_totals` is off by default; turn it on only if you
  want Pulp to ask GitHub for repo-wide billing totals when that API is
  available
- provider-reported GitHub totals are shown separately from tracked-run
  estimates because they are repo-wide current-period figures, not per-run truth
- GitHub can still return `unavailable` here if the account/API path does not
  support the newer billing endpoints

### 1a. Recommended Namespace setup

If you want to use the Namespace runner-provider path, the easiest setup today is:

```bash
brew install namespace-so/tap/nsc   # or use the install method from Namespace docs
nsc login
```

That is the recommended operator path for this pilot. Pulp can dispatch the
GitHub workflow without shelling out to `nsc`, but keeping `nsc` installed makes
it much easier to verify your Namespace workspace, inspect the account, and
later support thin `pulp ci-local cloud namespace ...` helper commands without
re-implementing Namespace setup logic inside Pulp.

Once `nsc` is installed, Pulp's thin helper commands can verify the state for
you:

```bash
pulp ci-local cloud namespace doctor
pulp ci-local cloud namespace setup
```

`doctor` checks that `nsc` exists, verifies login state, and prints the current
workspace identity. `setup` stays deliberately thin: it runs `nsc login` when
needed and then re-renders the same status.

```json
{
  "targets": {
    "mac": {
      "type": "local",
      "enabled": true
    },
    "ubuntu": {
      "type": "ssh",
      "host": "ubuntu",
      "repo_path": "/home/yourname/Code/pulp-validate",
      "utm_fallback": {
        "vm_name": "Ubuntu 24.04",
        "boot_wait_secs": 30,
        "ssh_retry_secs": 60
      }
    },
    "windows": {
      "type": "ssh",
      "host": "win",
      "repo_path": "C:\\Users\\yourname\\pulp-validate",
      "cmake_generator": "Visual Studio 17 2022",
      "cmake_platform": "x64",
      "cmake_generator_instance": "",
      "fallback_host": "win2",
      "utm_fallback": {
        "vm_name": "Windows 11",
        "boot_wait_secs": 60,
        "ssh_retry_secs": 120
      }
    }
  }
}
```

SSH host aliases come from `~/.ssh/config`. Set them up there rather than putting raw IPs in this file. This makes it easy to prefer a fast local VM as the primary target and keep a slower hardware-backed machine as the fallback when you only need it for edge cases.

Before trusting a Windows SSH target for CI, verify that its non-interactive PowerShell context resolves `git`, `cmake`, and `ctest`. An interactive shell that works is not sufficient proof for the SSH service context the runner actually uses.

If your Windows VM is Windows on ARM, you can either set `cmake_platform` to `"ARM64"` explicitly or leave it blank and let the runner infer `ARM64` vs `x64` from the remote host. If CMake keeps picking the wrong Visual Studio install, set `cmake_generator_instance` to the exact VS path, for example `C:/Program Files/Microsoft Visual Studio/2022/Community`. If you leave `cmake_generator_instance` blank, the runner prefers a full Visual Studio install over `BuildTools` when both are present. The pinned WebGPU dependency already has a Windows `aarch64` prebuilt for this path, so ARM Windows smoke runs can stay on the normal GPU-enabled configuration. This is useful for fast smoke validation on a local UTM VM. Keep an x64 Windows machine for parity runs when you need the authoritative Windows architecture.

### 2. Set up SSH keys

Each VM needs your public key in its `authorized_keys`. The Linux path is
straightforward; Windows requires extra steps because OpenSSH on Windows uses a
separate file with strict ACLs for admin users.

#### Find your public key (on your Mac)

If your private key is `~/.ssh/id_ed25519`, your public key is:

```bash
cat ~/.ssh/id_ed25519.pub
```

Copy the output — you'll paste it on each VM. If you're running the VM in UTM
or another hypervisor and can't copy/paste between host and guest, install the
guest tools for your hypervisor first (e.g. SPICE guest tools for UTM/QEMU,
VMware Tools, VirtualBox Guest Additions).

#### Linux (Ubuntu)

If `ssh-copy-id` is available and you can already reach the VM by password:

```bash
ssh-copy-id ubuntu    # or whatever your host alias is
```

If you're setting up from scratch on a fresh VM, SSH into it (or open its
console) and run:

**1. Note the VM's IP address:**

```bash
ip addr show
```

Look for the `inet` line under your active adapter (usually `enp0s1` or `eth0`).

**2. Install and enable the SSH server** (if not already running):

```bash
sudo apt update && sudo apt install -y openssh-server
sudo systemctl enable --now ssh
```

**3. Add your public key:**

```bash
mkdir -p ~/.ssh && chmod 700 ~/.ssh
echo "ssh-ed25519 AAAA...your-key-here..." >> ~/.ssh/authorized_keys
chmod 600 ~/.ssh/authorized_keys
```

**4. (Optional) Disable password auth** for tighter security:

```bash
sudo sed -i 's/^#\?PasswordAuthentication.*/PasswordAuthentication no/' /etc/ssh/sshd_config
sudo systemctl restart ssh
```

#### Windows

On the Windows VM, open PowerShell **as Administrator** and run:

**1. Note the VM's IP address** (you'll need it for SSH config later):

```powershell
ipconfig
```

Look for the `IPv4 Address` line under your active adapter.

**2. Create the admin authorized_keys file and add your public key:**

```powershell
New-Item -Force -ItemType File -Path "C:\ProgramData\ssh\administrators_authorized_keys"
Add-Content -Path "C:\ProgramData\ssh\administrators_authorized_keys" -Value "ssh-ed25519 AAAA...your-key-here..."
```

**3. Fix the ACL** (OpenSSH ignores the file if permissions are wrong):

```powershell
icacls "C:\ProgramData\ssh\administrators_authorized_keys" /inheritance:r /grant "SYSTEM:(F)" /grant "Administrators:(F)"
```

**4. Make sure sshd is running and set to auto-start:**

```powershell
Set-Service -Name sshd -StartupType Automatic
Start-Service sshd
```

> **Why `administrators_authorized_keys`?** Windows OpenSSH uses
> `C:\ProgramData\ssh\administrators_authorized_keys` for users in the
> Administrators group, not `~/.ssh/authorized_keys`. The ACL step is required —
> without it, sshd silently skips the file and falls back to password auth.

#### Set up SSH config on your Mac

Add entries to `~/.ssh/config` so you can type `ssh win` instead of remembering
IPs and usernames:

```
Host win
  HostName 192.168.64.5
  User your-username
  IdentityFile ~/.ssh/id_ed25519
  IdentitiesOnly yes
  ConnectTimeout 5

Host ubuntu
  HostName 192.168.64.4
  User your-username
  IdentityFile ~/.ssh/id_ed25519
  IdentitiesOnly yes
  ConnectTimeout 5
```

Replace the `HostName` values with the actual IPs from `ipconfig` (Windows) or
`ip addr` (Linux). The host aliases here (`win`, `ubuntu`) are what you'll use
in `hosts.local.json` for CI targets.

#### Test passwordless login

```bash
ssh ubuntu exit && echo "ok"
ssh win exit && echo "ok"
```

### 3. Clone the repo on each VM

The runner does a `git fetch` + checkout on the target, so the repo must already exist at the configured `repo_path`.

```bash
# On each VM:
git clone https://github.com/your-org/pulp.git ~/Code/pulp-validate
```

### 4. (Optional) Install the launchd drain agent

To automatically drain the queue on login and every 30 minutes:

```bash
cp tools/local-ci/dev.pulp.local-ci.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/dev.pulp.local-ci.plist
```

Edit the plist first if your repo is at a different path. To remove:

```bash
launchctl unload ~/Library/LaunchAgents/dev.pulp.local-ci.plist
rm ~/Library/LaunchAgents/dev.pulp.local-ci.plist
```

## Usage

```bash
# Enqueue the current HEAD and wait for completion
pulp ci-local run

# Queue even if your current cwd belongs to a different git root than the script checkout
pulp ci-local run --allow-root-mismatch

# Fast preflight: clean configure/build/install + installed-SDK smoke, no tests
pulp ci-local run --smoke

# Fast PR preflight with a comment that is clearly labeled as smoke-only
pulp ci-local check 56 --smoke

# Run Mac-only while iterating locally
pulp ci-local run --targets mac

# Queue background work with explicit priority
pulp ci-local enqueue --priority low

# Bump a pending job to the front of the queue
pulp ci-local bump <job-id> high

# Drain pending jobs if no other runner already owns the queue
pulp ci-local drain

# Show queue, active runner, recent results, live target state, and VM status
pulp ci-local status

# Tail a running or completed target log
pulp ci-local logs <job-id> --target windows

# Show accumulated exact-SHA target evidence for a branch
pulp ci-local evidence feature/my-branch --limit 3

# Show local-CI disk usage and reclaimable artifacts without deleting anything
pulp ci-local cleanup
pulp ci-local cleanup --dry-run

# Delete stale bundles/logs/results once no local CI job is running
pulp ci-local cleanup --apply

# Include prepared build/install caches too; later reruns will rebuild them
pulp ci-local cleanup --apply --include-prepared
```

`pulp ci-local run` is the most common command. It enqueues the current `HEAD`, joins the machine-global queue, and waits until that exact job finishes.

### Develop branch workflow

For complex, multi-piece features that use a `develop/*` integration branch, PRs target the develop branch instead of `main`. The `ship` command supports this via `--base`:

```bash
# Ship a feature to the develop branch (not main)
pulp ci-local ship feature/pkg-registry --base develop/package-manager

# The develop branch itself ships to main at phase boundaries
pulp ci-local ship develop/package-manager
```

GitHub Actions CI triggers on PRs to both `main` and `develop/**` branches, so CI runs automatically regardless of the target.

If you pass a branch name explicitly, for example `pulp ci-local run feature/my-branch`, local CI resolves and records that branch tip's exact SHA immediately. This prevents a stale launching checkout from accidentally queuing its own `HEAD` while you intended to validate a different branch.

Before queueing, local CI now also records:
- the worktree root that is actually being queued
- the current cwd and its git root, if any
- the config path and whether it came from `PULP_LOCAL_CI_CONFIG`, shared state, or the worktree fallback
- the selected SSH host/transport intent for each remote target

If the current cwd belongs to a different git root than the `local_ci.py` checkout you are invoking, queueing fails fast by default. Pass `--allow-root-mismatch` only when that mismatch is intentional.

If a selected SSH target is down and no fallback host or UTM fallback is configured, queueing now fails fast instead of burning time on a doomed job. Pass `--allow-unreachable-targets` only when you deliberately want to queue past that preflight.

Use `--smoke` when you want a quicker preflight before a full matrix run. Smoke mode still validates a clean detached worktree and installed SDK export path, but it disables tests, examples, and GPU in that clean build and skips `ctest`. Queue summaries and PR comments label these jobs as `validation=smoke` so they are not mistaken for full validation.

When a rerun is narrow and stays on the exact same SHA, local CI can now reuse the prepared root for that `target + validation` on persistent hosts. Status output calls this out as `prepared=reused` or `prepared=clean` so reused proof is never mistaken for a fresh cold path.

While a job is still running, `pulp ci-local status` reports live per-target state for the active job when available, for example:

```text
Runner: pid=12345 active=[abcd1234ef56] feature/my-branch

Running (1):
  [abcd1234ef56] feature/my-branch @ 0123456789ab priority=normal targets=mac,ubuntu,windows
    submission: root=/Users/me/Code/pulp-worktree config=/Users/me/Library/Application Support/Pulp/local-ci/config.json (shared-state)
    live targets: mac=pass, ubuntu=pass, windows=running
    windows: phase=test, output=2026-04-01T01:34:18+00:00, heartbeat=2026-04-01T01:34:33+00:00, idle=15s, liveness=quiet, log=windows.log
      37/1263 Test: OSC 4-byte alignment
```

If a run is interrupted after some targets have finished, the job is requeued but keeps its last known target state:

```text
Pending (1):
  [abcd1234ef56] feature/my-branch @ 0123456789ab priority=normal targets=mac,ubuntu,windows
    last known targets: mac=pass, ubuntu=pass, windows=running
```

Results are written to the machine-global state directory:

- macOS: `~/Library/Application Support/Pulp/local-ci/results/`
- Linux: `${XDG_STATE_HOME:-~/.local/state}/pulp/local-ci/results/`

A non-zero exit means at least one target failed.

If a newer SHA is queued for the same branch, targets, and validation mode, older
pending work is marked `superseded` and written to the results directory with a
reference to the replacement job. If a runner dies and reconciliation finds a newer
replacement already queued for that same scope, the stale running job is also
superseded instead of being requeued.

## Cleanup And Disk Usage

`pulp ci-local status` now includes a local footprint summary so retained CI
state stops being invisible drift:

- bundles
- prepared build/install caches
- logs
- results
- tracked cloud-run records

Use `pulp ci-local cleanup` to inspect what can be reclaimed. The command is a
dry run by default, and `--dry-run` is available explicitly when you want that
spelled out in scripts or notes.

What is cleaned automatically after job completion:

- completed-job git bundles once no pending/running job still needs them
- orphaned logs outside retained queue history
- orphaned result files outside retained queue history

What is not cleaned automatically in this first pass:

- prepared build/install state under `prepared/<target>/<mode>`

Prepared state is an intentional reuse cache. If you include it in manual
cleanup, later reruns will rebuild it from scratch.

Examples:

```bash
# Inspect reclaimable space
pulp ci-local cleanup

# Show the same dry-run plan explicitly
pulp ci-local cleanup --dry-run

# Delete stale bundles/logs/results
pulp ci-local cleanup --apply

# Also delete prepared caches
pulp ci-local cleanup --apply --include-prepared
```

Safety rules:

- `cleanup --apply` is blocked while local CI jobs are running
- prepared cleanup is destructive to cached build/install state
- logs/results tied to jobs still present in queue history are retained

If you need immediate manual cleanup outside the CLI, make sure no
`pulp ci-local` job is active first.


## Desktop automation

`pulp ci-local desktop ...` adds a GUI/session automation layer under the same local CI control plane. Use it when an agent needs to launch an app, inspect it, click on it, capture screenshots, or publish a local evidence gallery without logging into the target machine manually.

Current desktop commands:

```bash
# Prepare one target and record its contract/receipt
pulp ci-local desktop install mac
pulp ci-local desktop install ubuntu
pulp ci-local desktop install windows

# Health and capability reporting
pulp ci-local desktop doctor mac
pulp ci-local desktop status
pulp ci-local desktop recent mac --limit 3
pulp ci-local desktop proof windows --action inspect --source-mode exact-sha --sha <commit-sha>

# Configure artifact/publish settings
pulp ci-local desktop config show
pulp ci-local desktop config set artifact_root ~/Library/Application\\ Support/Pulp/desktop-automation/runs
pulp ci-local desktop config set publish_mode none

# Run GUI actions
pulp ci-local desktop smoke mac --bundle-id com.apple.TextEdit --label textedit-smoke
pulp ci-local desktop inspect mac --command '/path/to/pulp-ui-preview' --label ui-preview-inspect --pulp-app-automation
pulp ci-local desktop click mac --command '/path/to/pulp-ui-preview' --click-view-id bypass-toggle --capture-ui-snapshot --pulp-app-automation
pulp ci-local desktop inspect windows --command 'notepad.exe' --label notepad-inspect
pulp ci-local desktop click windows --command 'notepad.exe' --click 885,18 --label notepad-maximize

# Run against an exact prepared SHA instead of the live checkout
pulp ci-local desktop inspect mac \
  --command './build-desktop-automation/examples/ui-preview/pulp-ui-preview' \
  --source-mode exact-sha \
  --sha <commit-sha> \
  --prepare-command 'cmake -S . -B build-desktop-automation && cmake --build build-desktop-automation --target pulp-ui-preview' \
  --pulp-app-automation

# Publish or prune local bundles
pulp ci-local desktop publish mac --limit 5 --label mac-gallery
pulp ci-local desktop cleanup mac --older-than-days 14 --keep-last 10
```

Ubuntu prerequisite:

```bash
sudo apt-get update
sudo apt-get install -y git-lfs xvfb xauth xdotool imagemagick wmctrl x11-utils
git lfs install
```

Supported Ubuntu/Linux setup tiers:

- baseline deterministic backend: `xvfb` + `xauth`
- source/bootstrap prerequisite: `git-lfs`
- richer interaction/capture lane: `xdotool`, `imagemagick`, `x11-utils`, and `wmctrl`

`xvfb-run` is the supported deterministic backend for Ubuntu/Linux desktop automation. A visible `:0` display socket alone is not enough for SSH-driven automation because X11 authorization is often unavailable inside the remote shell. For repeatable CI and agent-driven runs, use the package set above and keep `xvfb-run` as the documented default.

`desktop doctor ubuntu` is an aggregate report. If multiple prerequisites are missing, it reports the full missing set plus remediation commands in one run instead of stopping at the first failure.

`desktop doctor ubuntu` checks the non-interactive SSH environment, not your interactive shell. `setup.sh` now prepends the common `~/.local/bin` path automatically before dependency checks, but if `git-lfs` still fails over SSH after that, add the real install location to the non-interactive login-shell PATH or install `git-lfs` system-wide so `git lfs version` succeeds without extra shell setup.

Exact-SHA source prep on Ubuntu/Linux uses the same non-interactive SSH environment. The controller now treats bundle-based checkout and LFS materialization as separate steps:

- prepend `~/.local/bin` before any `git-lfs`-dependent command
- fetch and checkout with `GIT_LFS_SKIP_SMUDGE=1`
- attach the clone URL as `origin`
- then let `setup.sh --deps-only --ci` / `git lfs pull` materialize the SDK blobs

That split matters on fresh VMs because a bundle checkout alone does not carry an `origin` remote, and LFS smudge/pull fails if it cannot resolve the repository URL.

Fresh Ubuntu proof checklist:

1. Start from a fresh source root, `PULP_HOME`, and `PULP_PROJECTS_DIR`
2. Run `./setup.sh --deps-only --ci`
3. Build `pulp-cli`
4. Run `pulp create <ProjectName> --manufacturer "<Name>" --no-interactive`
5. Run `pulp build` inside the generated project
6. Verify actual emitted artifacts, not just configure/test success

Current expected native outputs from that proof are:

- Linux: VST3 target output under `build/VST3`, `CLAP`, `LV2`, and the standalone binary
- macOS: `build/VST3/<Name>.vst3`, `build/AU/<Name>.component`, `build/CLAP/<Name>.clap`, and the standalone `.app` bundle
- Windows: `build/VST3/Debug/<Name>.dll` for the VST3 target, `build/CLAP/Debug/<Name>.clap`, and the standalone `.exe`

If web formats are required, make them explicit in the generated project format list; the default native create proof does not imply web artifact output.

Windows first-time setup checklist:

1. Install and enable OpenSSH Server.
2. Keep a normal desktop user logged in to the VM. The Windows session-agent runs inside that logged-in session; SSH by itself is not a GUI session.
3. Make sure `winget` is available. `desktop install windows` uses it to provision required remote tooling such as Git when the VM is still fresh.
4. Run `pulp ci-local desktop install windows` once. This bootstraps the scheduled task, installs required remote tooling when possible, and writes the target-side PowerShell agent under `%LOCALAPPDATA%\\Pulp\\desktop-automation-agent`.
5. Run `pulp ci-local desktop doctor windows` and make sure SSH, the scheduled-task contract, and the required `git` check are green before attempting live proofs.
6. For source builds on the Windows VM itself, use `powershell -ExecutionPolicy Bypass -File .\setup.ps1`. The wrapper imports the Visual Studio environment and uses a short temporary drive alias so first-time bootstrap does not fail on long nested dependency paths.

The short-path rule is not theoretical. Windows source builds can fail from
long nested checkout roots and then pass once the same source tree is mapped
through a temporary drive alias before the first configure/build. Treat
`setup.ps1` or an equivalent short-path wrapper as the supported bootstrap path
for Windows source builds.

Remote tooling policy on Windows:

- required: `git`
  - used by the exact-SHA bundle-sync and prepare flows
  - `desktop install windows` will provision it via `winget` when possible
- optional: `gh`
  - useful for remote GitHub workflows on the target
  - not required for smoke/inspect/click proofs
- optional: `gh auth`
  - advisory only; authenticate it only if you intentionally want GitHub CLI workflows on the Windows target

Remote repo bootstrap policy on Windows:

- first-time `desktop install windows` should not require GitHub credentials on the target VM
- the controller prefers a locally uploaded git bundle to materialize `pulp-validate`
- `origin` is still attached when available so later fetches remain truthful
- `gh` and stored Git credentials are optional unless you intentionally want GitHub workflows on the Windows machine itself

Useful host-side verification commands:

```powershell
Get-Service sshd
Set-Service -Name sshd -StartupType Automatic
Start-Service sshd
Get-NetFirewallRule -Name *ssh*
where.exe winget
where.exe git
where.exe gh
```

Useful first-time remote installs if you want to pre-provision them manually:

```powershell
winget install --id Git.Git -e --source winget --accept-package-agreements --accept-source-agreements --disable-interactivity
winget install --id GitHub.cli -e --source winget --accept-package-agreements --accept-source-agreements --disable-interactivity
```

Supported Windows v1 interaction tiers:

- generic window-capture lane:
  - `--command` only
  - works for normal desktop apps such as `notepad.exe`
  - supports window screenshot capture and coordinate clicks
- Pulp-owned app automation lane:
  - add `--pulp-app-automation`
  - enables `ViewInspector` snapshots and view-target selectors such as `--click-view-id`

Artifact bundles are written outside the repo by default:

- macOS: `~/Library/Application Support/Pulp/desktop-automation/runs/`
- Linux: `${XDG_STATE_HOME:-~/.local/state}/pulp/desktop-automation/runs/`
- Windows: `%LOCALAPPDATA%\\Pulp\\desktop-automation\\runs\\`

Each bundle stores:

- `manifest.json`
- `stdout.log` / `stderr.log`
- `prepare.log` when exact-SHA mode runs a fresh prepare step
- `ui-tree.json` when a UI snapshot is available
- `screenshots/window.png`
- `screenshots/before.png` / `screenshots/diff.png` when an interaction captures before/after evidence

The artifact root also maintains rolling summaries for agents and status tooling:

- `latest-run.json` — newest observed run summary
- `latest-proof.json` — newest successful proof summary
- `runs.jsonl` — raw summary stream for recent desktop automation runs
- target-scoped copies under `<artifact-root>/<target>/...`
- `_published/latest-report.json` — newest staged local HTML/JSON gallery summary
- `_published/reports.jsonl` — raw summary stream for local published galleries

`manifest.json` now includes additive source provenance when desktop actions run through the controller:

- `source.mode` (`live` or `exact-sha`)
- `source.branch`
- `source.sha`
- `source.prepare_command`
- `source.prepare_timeout_secs`
- `source.prepared_root`
- `source.launch_cwd`

Desktop reporting surfaces are intentionally split:

- `desktop recent` = raw run history, including failed attempts
- `desktop proof` = successful proof summaries grouped by `target + action + source.mode + source.sha`
- `desktop status` = target config plus `latest_run`, `latest_proof`, and the newest local publish summary (`latest_publish`)

Use `desktop proof` when you need to answer questions like:

- “What live-host proof do we already have for Ubuntu on this SHA?”
- “Did Windows ever pass this exact-SHA inspect lane?”
- “What is the newest successful proof, even if the newest run failed?”

### Exact-SHA desktop source mode

`desktop smoke`, `desktop click`, and `desktop inspect` all share a controller-owned source mode:

- `--source-mode live|exact-sha`
- `--branch`
- `--sha`
- `--prepare-command`
- `--prepare-timeout`

Behavior:

- `live` launches from the target's normal working copy behavior.
- `exact-sha` prepares a per-target source root for the requested SHA, launches from that prepared root, and records the prepared-root provenance in the run manifest.
- On Windows, `--prepare-command` executes inside a generated `.cmd` script under `cmd.exe`. Use double quotes for paths, generator names, and arguments. POSIX-style single-quoted tokens are treated as literal text and are rejected by the controller before the remote prepare step starts.
- When `desktop_automation.targets.<target>.optional.webview_driver=true`, `desktop doctor` probes the configured `webdriver_url` through the WebDriver `/status` endpoint and reports whether the driver is actually reachable and ready, not just whether the URL exists in config.

Preparation/cache semantics:

- Prepared roots are keyed by `target + sha + prepare_command`.
- A repeated identical request may reuse the prepared root instead of rebuilding it.
- `prepare_command` only runs when a fresh prepared root is created.

Launch behavior:

- Desktop actions switch their launch `cwd` to the prepared root in exact-SHA mode.
- Repo-local executable paths in the first command token are rewritten into the prepared root automatically.
- The current exact-SHA workflow is a `--command` lane. Do not assume `--bundle-id` participates in exact-SHA source preparation.

`pulp-ui-preview` is currently Apple-desktop-only, so the Linux and Windows
source-build examples below use the cross-platform PulpGain standalone target.

Examples:

```bash
# macOS local exact-SHA inspect
pulp ci-local desktop inspect mac \
  --command './build-desktop-automation/examples/ui-preview/pulp-ui-preview' \
  --source-mode exact-sha \
  --sha <commit-sha> \
  --prepare-command 'cmake -S . -B build-desktop-automation && cmake --build build-desktop-automation --target pulp-ui-preview' \
  --pulp-app-automation

# Ubuntu xvfb exact-SHA smoke against a Linux-supported standalone
pulp ci-local desktop smoke ubuntu \
  --command './build-desktop-automation/examples/pulp-gain/PulpGain' \
  --source-mode exact-sha \
  --sha <commit-sha> \
  --prepare-command 'cmake -S . -B build-desktop-automation && cmake --build build-desktop-automation --target PulpGain_Standalone'

# Windows session-agent exact-SHA smoke
pulp ci-local desktop smoke windows \
  --command '.\\build-desktop-automation\\examples\\pulp-gain\\Debug\\PulpGain.exe' \
  --source-mode exact-sha \
  --sha <commit-sha> \
  --prepare-command 'cmake -S . -B build-desktop-automation -G \"Visual Studio 17 2022\"; cmake --build build-desktop-automation --target PulpGain_Standalone --config Debug'

# Windows generic live inspect
pulp ci-local desktop inspect windows \
  --command 'notepad.exe' \
  --label notepad-inspect

# Windows generic live click with before/after evidence
pulp ci-local desktop click windows \
  --command 'notepad.exe' \
  --click 885,18 \
  --label notepad-maximize

# Query the newest successful Windows proof for one SHA
pulp ci-local desktop proof windows \
  --action smoke \
  --source-mode exact-sha \
  --sha <commit-sha>
```

### Desktop adapter truth

- `macos-local`
  - runs directly on the local logged-in macOS session
  - supports bundle launch via `--bundle-id`
  - supports Pulp-owned app automation (`--pulp-app-automation`) for direct launch commands, including `ViewInspector` snapshots and view-target clicks
- `linux-xvfb`
  - runs GUI smoke/inspect/click through `xvfb-run`
  - currently supports `--command` only
  - currently requires `--pulp-app-automation` for the click/inspect lane
- `windows-session-agent`
  - bootstraps a scheduled task plus target-side PowerShell agent in the logged-in Windows desktop session
  - requires a real logged-in desktop user; SSH alone is not enough
  - currently supports `--command` only
  - supports generic `window-capture` smoke/inspect/click for normal desktop apps
  - supports coordinate clicks and before/after screenshot diffs without `--pulp-app-automation`
  - supports `ViewInspector` snapshots and view-target selectors only with `--pulp-app-automation`
  - uses the scheduled task plus target-side agent as the honest v1 Windows interaction lane; external UI automation tools are optional future adapters, not the core controller

### Proof lookup

`desktop proof` is the first-class proof query surface for desktop automation:

- filters:
  - `target`
  - `--action`
  - `--source-mode live|exact-sha|legacy`
  - `--sha`
  - `--branch`
- groups successful proofs by `target/action/source.mode/source.sha`
- ignores failed runs when computing proof summaries

Example:

```bash
pulp ci-local desktop proof ubuntu --action click --source-mode exact-sha --sha <commit-sha>
```

`desktop status` now reports both:

- `latest_run`: the newest run, even if it failed
- `latest_proof`: the newest successful proof summary for that target
- `latest_publish`: the newest local HTML/JSON gallery summary staged under `_published/`

### Desktop config keys

`tools/local-ci/config.json` accepts a `desktop_automation` block:

```json
{
  "desktop_automation": {
    "artifact_root": "",
    "publish_mode": "none",
    "publish_branch": "dev-artifacts",
    "retention_days": 14,
    "targets": {
      "mac": {
        "adapter": "macos-local",
        "bootstrap": "launchagent",
        "capability_tier": "v2",
        "optional": {
          "webview_driver": false,
          "webdriver_url": "",
          "debug_attach": false,
          "debugger_command": "lldb",
          "video_capture": false,
          "frame_stats": false
        }
      },
      "ubuntu": {
        "adapter": "linux-xvfb",
        "bootstrap": "xvfb-run",
        "capability_tier": "v2",
        "optional": {
          "webview_driver": false,
          "webdriver_url": "",
          "debug_attach": false,
          "debugger_command": "lldb",
          "video_capture": false,
          "frame_stats": false
        }
      },
      "windows": {
        "adapter": "windows-session-agent",
        "bootstrap": "scheduled-task",
        "capability_tier": "v2",
        "task_name": null,
        "remote_root": null,
        "optional": {
          "webview_driver": false,
          "webdriver_url": "",
          "debug_attach": false,
          "debugger_command": "",
          "video_capture": false,
          "frame_stats": false
        }
      }
    }
  }
}
```

For Windows:

- `task_name` is optional. If omitted, local CI uses `PulpDesktopAutomationAgent-<target>`.
- `remote_root` is optional. If omitted, the agent is installed under `%LOCALAPPDATA%\Pulp\desktop-automation-agent`.
- `optional.webview_driver` enables the future WebView/WebDriver capability vocabulary for that target. Pair it with `optional.webdriver_url` only when the app under test actually exposes a localhost WebDriver endpoint in debug/test mode.
- `optional.debug_attach`, `optional.video_capture`, and `optional.frame_stats` are opt-in groundwork flags. They make the target advertise and doctor those optional tiers; they do not magically make the adapter support them unless the required tooling is also present.

Convenience updates through the CLI:

```bash
pulp ci-local desktop config set target.mac.webview_driver true
pulp ci-local desktop config set target.mac.webdriver_url http://127.0.0.1:4444
pulp ci-local desktop config set target.mac.debug_attach true
pulp ci-local desktop config set target.mac.debugger_command lldb
pulp ci-local desktop config set target.mac.video_capture true
pulp ci-local desktop config set target.mac.frame_stats true
```

Recommended host-side remediation when `desktop doctor windows` reports `SSH service reset during handshake`:

```powershell
Get-Service sshd
Set-Service -Name sshd -StartupType Automatic
Start-Service sshd
Get-NetFirewallRule -Name *ssh*
```

Treat that failure as a Windows host-side OpenSSH issue, not a desktop-agent contract failure.

### Desktop publication

`pulp ci-local desktop publish` always stages a local HTML/JSON gallery from recent bundles. In the default `publish_mode=none` path, that is the whole feature. When `publish_mode=branch`, the same report is also mirrored to the configured publish branch under `desktop-automation/latest/` and `desktop-automation/reports/<report-id>/`.

- `index.html`
- `index.json`
- copied screenshots and diffs
- source manifest/log references
- `_published/latest-report.json` and `_published/reports.jsonl` rollups for the newest/known local galleries

Use `desktop config set publish_mode ...` only when you intentionally want publication behavior. The default should stay `none` for normal development.

Branch publication notes:

- `publish_mode=branch` pushes the latest local report to `publish_branch`
- the branch stores `desktop-automation/latest/` plus immutable `desktop-automation/reports/<report-id>/` snapshots
- when the repo remote is GitHub, the publish report includes clickable branch/tree/blob URLs for the mirrored artifacts

## Evidence Tracking

`pulp ci-local evidence` summarizes the last-good recorded results by exact SHA, target, and validation mode. This is the operator-facing answer to:

- what already passed on this branch?
- which exact SHA has Windows full proof?
- do we really need to rerun macOS again?

The compact evidence section in `pulp ci-local status` uses the same data so the current branch’s known-good results stay visible during active work.

## Working A Failure

Do not wait for a whole matrix to finish before reacting. The fastest loop is:

1. start a run
2. watch `pulp ci-local status`
3. tail `pulp ci-local logs <job-id> --target <name>` on the first failing or suspicious target
4. begin the narrowest local repro or code inspection immediately
5. rerun only the truthful scope needed after the fix

In practice, that means:

- one process owns CI monitoring and host state
- one process or agent works the likely fix locally as soon as a failure becomes actionable
- user updates should be sent when a target changes state or the first actionable failure appears, not only when asked
- a target that already failed is enough to start debugging; do not burn time waiting for unrelated targets to finish unless their result changes the fix
- once a failure is actionable, start the fix track in parallel unless it would contend with the same host or invalidate the active run
- do not rerun a target that already passed on the exact same SHA unless that prior result is untrustworthy or the environment changed
- if only one or two targets are stale, rerun only those targets instead of the whole matrix
- once the failure surface is isolated, prefer the minimum sufficient proof instead of a symmetric rerun
- a direct exact-SHA validate on one target counts as valid evidence for that target; keep earlier same-SHA passes for the other targets unless something actually invalidated them
- on persistent hosts, narrow same-SHA reruns should prefer prepared-state reuse instead of paying again for clean worktree/setup/build work
- use `--smoke` first when the risk is install/export/build structure rather than runtime test behavior
- `all targets on one SHA` is a goal, not a reason to blindly rerun already-green same-SHA targets
- if a broader in-flight job is no longer informative, cut over to the narrower rerun instead of letting the queue drift

## Priorities

Jobs are ordered by priority first, then FIFO within the same priority.

- `low` — background validation
- `normal` — default interactive work
- `high` — shipping, PR checks, or work you want to run first

You can set the initial priority with `--priority` and change a pending job later with:

```bash
pulp ci-local bump <job-id> high
```

`pulp ci-local status` prints the job ids you can bump.

## Exact SHAs On Remote Targets

Remote targets validate the queued SHA, not the latest branch tip. That keeps queued jobs truthful, and the runner now uploads that exact SHA to SSH targets as a git bundle before validation.

If you queued work with an explicit branch name, the runner first resolves that branch name to a commit SHA and then treats the run exactly like any other exact-SHA validation.

That means this works even for a local-only commit:

```bash
pulp ci-local run --targets mac,ubuntu,windows
```

`pulp ci-local ship` still pushes first because it opens and validates a PR, but ordinary local validation no longer depends on the remote host already having your branch tip.

## Running Mac-only

If you don't have VMs set up, disable the SSH targets in your active CI config:

```json
"ubuntu": {
  "type": "ssh",
  "enabled": false,
  ...
}
```

Mac validation still runs. You get single-platform coverage, which is better than nothing for catching build breaks before pushing.

You can also keep the SSH targets enabled and request Mac-only while iterating:

```bash
pulp ci-local run --targets mac
```

## For contributors

You don't need the same VM setup as the original developer. Options:

- **Mac-only**: Disable all SSH targets. Fast, free, covers the primary development platform.
- **UTM VMs**: Free. Requires ~40 GB of disk for both VMs. UTM images can be created from ISO or from the UTM gallery.
- **Cloud VMs**: Works with any SSH-accessible host. Costs money while running — stop them when not in use.
- **Physical machines**: A spare Linux box or Windows machine on your network works fine.

Local CI config is intentionally gitignored. Keep your host topology local, and prefer the machine-global config path so every worktree uses the same host map by default.

## Troubleshooting

### `JSONDecodeError` on Shipyard queue file

Shipyard's local job queue lives at `~/Library/Application Support/shipyard/queue/queue.json` on macOS (`~/AppData/Local/shipyard/queue/queue.json` on Windows, `${XDG_STATE_HOME:-~/.local/state}/shipyard/queue/queue.json` on Linux). On rare crashes Shipyard can truncate this file to zero bytes, which then breaks every subsequent invocation with a `JSONDecodeError`.

Recovery (run once):

```bash
echo '{"jobs": []}' > ~/Library/Application\ Support/shipyard/queue/queue.json
```

Re-running `tools/install-shipyard.sh` also performs this reset automatically. Tracked as #528.

## The Shipyard macOS lane builds Debug — on purpose

`.shipyard/config.toml` configures the macOS validation lane with
`-DCMAKE_BUILD_TYPE=Debug`. This contradicts CLAUDE.md ("Release is the default")
and looks like config drift. **It is deliberate, and flipping it to Release would
remove the only lane in CI that can see a whole class of undefined behaviour.**

On 2026-07-12 it caught a real ODR violation (#6081). `snap_to_zero()` is an inline
function *template* defined in a header, its body gated by a build-time macro, and a
test TU redefined that macro before including the header. Both translation units then
emitted the **same mangled symbol with different bodies**:

| build | what happens | result |
|---|---|---|
| `-O3` | each TU inlines its own copy, so each behaves per its own macro | the A/B test appears to work — **Release is green, the bug is invisible by construction** |
| `-O0` | nothing inlines; both TUs emit a weak symbol, the linker keeps exactly **one**, and both call it | the "disabled" reference silently ran the enabled code — **Debug is red** |

The red test was the *mild* outcome. The linker's choice is arbitrary: had it kept
the other definition, the assertions would have **passed while exercising a no-op** —
a null test, asserting nothing, green forever.

**The fix shape** is not "delete the redefine". It is: give the variant its **own
binary**, compiled consistently end to end, linking no default-built TU (see
`test/denormal_null_refgen.cpp`). The class is now guarded by
`tools/scripts/test_odr_macro_gated_headers.py`.

### A perf gate failing there is a mis-calibrated gate, not a reason to flip the lane

Debug builds are much slower, and CLAUDE.md is right that Debug is the wrong default
for most work. The answer is not "Debug everywhere" — it is **keep one `-O0` lane,
and calibrate perf gates for the build they actually run in.**

`test/test_yoga_layout_bench.cpp` is the worked example. Its timing threshold is
0.25 x a 60fps frame (4166.7us), sized at ~11x an M-series **Release** baseline
(~380us) to tolerate a loaded CI box. But in the Debug lane the same 484-node pass
takes ~4420us — about **11.6x slower**, which eats the entire safety margin. The gate
sat permanently at the edge (4421.8us vs 4166.7us, ~6% over) and load merely tipped
it. It was never "flaky because the box was busy"; it was a Release-calibrated gate
running unoptimized, where it measured **the absence of the optimizer**, not the cost
of layout.

The timing assertion is now `#ifdef NDEBUG`-gated — the GitHub macOS lane configures
Release, so it still runs with real coverage and the right calibration. The
**structural** assertions (`allocs_per_pass > 0`, frees-match-allocs) still run in
every build; they catch real regressions and do not care about the optimizer.

> A false red is worse than no gate: it trains everyone to wave away red as
> "probably the box" — which is exactly how a real bug gets dismissed.
