# Local CI

Local CI lets you validate branches on your Mac and cross-platform VMs before merging, without spending money on cloud CI minutes.

## TL;DR

- `pulp ci-local run` queues the current `HEAD` in a machine-global queue shared by every worktree on that Mac.
- `pulp ci-local run --smoke` queues a fast clean install/export preflight instead of a full test run.
- The queue serializes jobs, not targets. One CI job runs at a time, but its requested targets (`mac`, `ubuntu`, `windows`) run in parallel inside that job.
- Mac runs locally. Ubuntu and Windows run over SSH against repos you already cloned on those machines.
- Remote targets validate the exact queued git SHA, not "whatever the branch points to later". The runner uploads that SHA as a git bundle before validation, so full-matrix checks do not depend on the host already seeing your latest branch tip.
- `pulp ci-local status` shows the active runner, pending jobs, SSH/VM reachability, and live per-target state for the running job. `pulp ci-local bump <job-id> high` moves a pending job forward.
- While a job is running, `pulp ci-local status` also shows live per-target state such as `mac=pass, ubuntu=pass, windows=running`.
- If you queue a newer SHA for the same branch, targets, and validation mode, older pending work is superseded automatically instead of sitting behind it forever.
- `pulp ci-local logs <job-id> --target windows` tails the saved per-target log from the machine-global CI state dir, so you do not need ad hoc SSH just to see whether a target is building or testing.
- `pulp ci-local evidence [branch]` shows the last-good exact-SHA target evidence already recorded for a branch, so you can keep earlier same-SHA passes instead of rerunning them blindly.
- Persistent local CI hosts now keep a prepared root per `target + validation` so a narrow same-SHA rerun can reuse earlier work instead of rematerializing from scratch.
- If a runner is interrupted, the queued job keeps its last-known per-target state so you can see what already passed before deciding whether to rerun everything or just the remaining target.
- Jobs submitted through `pulp ci-local` are globally queued, and validation itself now takes a per-host lock on macOS/Linux plus a Windows host mutex, so old `validate-build.sh` runs wait instead of colliding.
- SSH targets receive a per-job git bundle before validation. That keeps exact-SHA validation working even when the host validates from a stale local mirror instead of GitHub directly.
- Windows SSH jobs execute from short detached worktrees under `C:\pulp-ci`, and stale worktree metadata is pruned automatically before reruns.
- If a stale runner leaves behind an old Windows validator, the next drain pass now targets that specific remote validator PID for cleanup before starting new work, and `status` keeps the cleanup result visible.
- Reuse is a persistent-host feature for local macOS and SSH-backed/self-hosted hosts. Ephemeral cloud runners should keep the default clean path unless a later policy explicitly opts them in.
- Truly raw ad hoc `ssh`, `cmake`, or custom background processes still bypass coordination until they are stopped or migrated.

## Why local instead of cloud

Pulp has GitHub Actions workflows for CI, but running them on every branch costs money. Local CI is free and faster for iterative development — you get results in minutes from machines you already own or have running locally. Cloud CI remains available for release branches and public PRs where you want the full matrix on neutral hardware.

## How it works

When you run `pulp ci-local`, it:

1. Queues a job in a machine-global queue shared by every worktree on that Mac
2. Runs only one queue drain owner at a time, so separate agents do not stampede the same Mac and VMs
3. Validates locally on Mac via `./validate-build.sh --ref <sha>`
4. For each SSH target in `config.json`: uploads a per-job git bundle, injects that exact SHA into the configured repo on the host, then validates it there
5. If an SSH target is unreachable, it tries to start the corresponding UTM VM, waits for it to boot, then retries the SSH connection
6. Drains queued work on login or wake if you install the launchd agent

Mac validation always runs. SSH targets are skipped if disabled in config.

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

Edit `config.json` and fill in your SSH hostnames and repo paths. The `host` field is the primary SSH target. `fallback_host`, if present, is tried next. The `utm_fallback` block is optional and is only used if SSH targets are unreachable.

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
```

`pulp ci-local run` is the most common command. It enqueues the current `HEAD`, joins the machine-global queue, and waits until that exact job finishes.

Use `--smoke` when you want a quicker preflight before a full matrix run. Smoke mode still validates a clean detached worktree and installed SDK export path, but it disables tests, examples, and GPU in that clean build and skips `ctest`. Queue summaries and PR comments label these jobs as `validation=smoke` so they are not mistaken for full validation.

When a rerun is narrow and stays on the exact same SHA, local CI can now reuse the prepared root for that `target + validation` on persistent hosts. Status output calls this out as `prepared=reused` or `prepared=clean` so reused proof is never mistaken for a fresh cold path.

While a job is still running, `pulp ci-local status` reports live per-target state for the active job when available, for example:

```text
Runner: pid=12345 active=[abcd1234ef56] feature/my-branch

Running (1):
  [abcd1234ef56] feature/my-branch @ 0123456789ab priority=normal targets=mac,ubuntu,windows
    live targets: mac=pass, ubuntu=pass, windows=running
    windows: phase=test, output=2026-04-01T01:34:18+00:00, log=windows.log
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

That means this works even for a local-only commit:

```bash
pulp ci-local run --targets mac,ubuntu,windows
```

`pulp ci-local ship` still pushes first because it opens and validates a PR, but ordinary local validation no longer depends on the remote host already having your branch tip.

## Running Mac-only

If you don't have VMs set up, disable the SSH targets in `config.json`:

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

`config.json` is gitignored. Your host topology stays local.
