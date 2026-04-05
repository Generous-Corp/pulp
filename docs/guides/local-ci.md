# Local CI

Local CI lets you validate branches on your Mac and cross-platform VMs before merging, without spending money on cloud CI minutes.

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
- `build.yml` now accepts `runner_provider` and routes Linux and Windows through
  the selected provider; macOS is omitted from the cloud build by default so it
  can stay local-first
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

If your Windows VM is Windows on ARM, you can either set `cmake_platform` to `"ARM64"` explicitly or leave it blank and let the runner infer `ARM64` vs `x64` from the remote host. If CMake keeps picking the wrong Visual Studio install, set `cmake_generator_instance` to the exact VS path, for example `C:/Program Files/Microsoft Visual Studio/2022/Community`. If you leave `cmake_generator_instance` blank, the runner prefers a full Visual Studio install over `BuildTools` when both are present. The pinned WebGPU dependency already has a Windows `aarch64` prebuilt for this path, so ARM Windows smoke runs can stay on the normal GPU-enabled configuration. This is useful for fast smoke validation on a local UTM VM. Keep an x64 Windows machine for parity runs when you need the authoritative Windows architecture.

### 2. Set up SSH keys

Each VM needs your public key in its `authorized_keys`. Standard procedure:

```bash
ssh-copy-id ubuntu    # or whatever your host alias is
ssh-copy-id win2
```

Test that passwordless login works before proceeding:

```bash
ssh ubuntu exit && echo "ok"
ssh win2 exit && echo "ok"
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
