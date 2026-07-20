# Set up a Mac as a Pulp CI host (Tart VM lane)

Opinionated runbook. Follow it top-to-bottom on a fresh/clean Apple-Silicon Mac and it should **just work**: your Mac will build Pulp in disposable Tart VMs and join the CI runner pool, with Shipyard merging on green. Companion to [`self-hosted-runner.md`](self-hosted-runner.md) (the bare-metal runner) — this is the **VM-isolated** lane. Agents working on this lane should read the `tart-ci` skill (`.agents/skills/tart-ci/SKILL.md`).

## Is this for you? (optional — and why it's recommended)
**You do not need any of this to use, build, test, or contribute to Pulp.** The normal path — `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && ctest`, open a PR, let GitHub Actions validate — works for everyone and needs none of the dependencies below. This page is an **optional, advanced setup** for contributors who want to run CI builds on their own hardware.

- **It adds dependencies + disk.** Tart, a golden VM image (~45 GB) on top of a base (~30 GB) — budget **~100 GB+ free** (more if you keep several dated goldens), plus Xcode. Deliberately opt-in.
- **Why it's worth it (how the maintainer works).** Every CI job runs in a **disposable, identical VM**, so your machine stays responsive, builds never corrupt each other's state, and the toolchain is **pinned/deterministic** (stable font/raster goldens). You validate on *your* fast local hardware instead of queuing on shared runners.
- **On Shipyard + tartci — encouraged, never required.** Shipyard is the merge-on-green orchestrator (`shipyard pr`): one command runs Pulp's gates (skill-sync, version-bump), opens the PR, validates across platforms, and merges when green. tartci is the optional VM provider layer for clean macOS/Linux/Windows build VMs on Apple Silicon. Pulp's current profile lives in `.shipyard/ci-profiles/normal-local-fast.toml`; Shipyard/tartci upstream docs own the generic profile mechanics. **But this is an accelerator, not a gate:** you can always contribute with plain `gh` PRs + GitHub Actions. Use whichever fits you.

## Quick start (one command)
If you already have a golden on another host (or want to bake), `tools/ci/setup-ci-host.sh` automates the whole thing — installs Tart, sets up `~/VMs`, acquires the golden, and installs the runner agent:
```bash
cd ~/Code/pulp
# Copy the golden from an existing host (recommended) and join the pool:
tools/ci/setup-ci-host.sh --class m5 \
  --copy-from 'macstudio:/Volumes/Workshop/VMs/vms/pulp-build-runner:latest'
# Or, if the golden is already present, just wire the host + agent:
tools/ci/setup-ci-host.sh --class m5
```
It's idempotent and prints the one thing it can't automate (the Full Disk Access GUI grant, **only** if you put VMs on an external `/Volumes`). The manual steps below are the reference for what it does.

## What you get
- Every CI job runs in a **throwaway macOS VM** cloned from a versioned **golden image** → the host stays responsive, there's no build-dir churn (the ODR class of failures can't happen), and the toolchain is **deterministic** (pinned Xcode + Skia, so font/raster goldens are stable).
- Your Mac joins the `pulp-build` runner pool **additively** and Shipyard merges PRs on green.

## Opinionated defaults (just do these)
1. **VMs live in `~/VMs` (HOME), never an external `/Volumes` drive.** A launchd agent can't read an external volume without **Full Disk Access** (macOS TCC); HOME avoids that entirely.
2. **Copy the golden from an existing host if you have one** (minutes); otherwise bake it (~1 h, one-time).
3. **Pin Xcode in the golden** — reproducible raster goldens across the fleet.
4. **Ephemeral per-job runners** (one job per pristine VM), driven by the launchd agent.
5. **Host-class label**: register as `self-hosted,macos,arm64,pulp-build,pulp-build-<class>` where `<class>` identifies the machine (`studio`, `m5`, `macbook`, …). The shared `pulp-build` label is the required pool; the class label lets you route/validate one host.

## 0. Prerequisites
```bash
# Apple Silicon, macOS matching the golden (Tahoe for Xcode 26.5).
brew install cirruslabs/cli/tart hudochenkov/sshpass/sshpass
# SSH keypair the golden will trust (or already trusts):
test -f ~/.ssh/id_ed25519 || ssh-keygen -t ed25519 -N "" -f ~/.ssh/id_ed25519
gh auth login -h github.com            # config-file token storage (no keychain dependency); repo admin to mint JIT runner configs
git clone https://github.com/Generous-Corp/pulp.git ~/Code/pulp   # tools/ci/* + the launchd template
echo 'export TART_HOME=$HOME/VMs' >> ~/.zprofile && export TART_HOME=$HOME/VMs
mkdir -p ~/VMs && touch ~/VMs/.metadata_never_index              # keep Spotlight off the VM store
```

## 1. Get the golden image
The golden chain is `macos-build-base → macos-apple-xcode → pulp-build-base → pulp-build-runner`. You only need **`pulp-build-runner:latest`** to run jobs.

**A — copy from an existing host (recommended).** Over Tailscale or a connected drive; `-S` preserves the sparse `disk.img` (else it inflates to its 150 GB apparent size). Copy **stopped** VMs only.
```bash
mkdir -p ~/VMs/vms
rsync -aHS --info=progress2 <other-host>:'<their TART_HOME>/vms/pulp-build-runner:latest' ~/VMs/vms/
tart list   # should show pulp-build-runner:latest
# (optional, for local re-bake capability: also copy the macos-build-base/apple-xcode/pulp-build-base :latest tiers)
```

**B — bake from scratch** (needs Xcode 26.5 on the host, or `xcodes`). From `~/Code/pulp`:
```bash
PULP_HOST_XCODE_APP=/Applications/Xcode.app bash tools/ci/tart-provision.sh verify   # preflight
bash tools/ci/tart-provision.sh base        && bash tools/ci/tart-provision.sh tag macos-build-base  macos-build-base
PULP_HOST_XCODE_APP=/Applications/Xcode.app bash tools/ci/tart-provision.sh apple-xcode && bash tools/ci/tart-provision.sh tag macos-apple-xcode macos-apple-xcode
bash tools/ci/tart-provision.sh pulp        && bash tools/ci/tart-provision.sh tag pulp-build-base    pulp-build-base
PULP_HOST_SHIPYARD=~/.local/bin/shipyard bash tools/ci/tart-provision.sh runner && bash tools/ci/tart-provision.sh tag pulp-build-runner pulp-build-runner
```

## 2. Validate before joining the pool (the test)
Prove your host builds Pulp green in a VM on a **host-only** label, without touching the required lane:
```bash
cd ~/Code/pulp
# terminal 1 — one ephemeral runner on a host-only label:
bash tools/ci/tart-runner.sh --once --labels self-hosted,macos,arm64,pulp-build-<class>
# terminal 2 — route a real build to it and watch:
gh workflow run build.yml --ref main -f macos_runner_selector_json='["self-hosted","pulp-build-<class>"]'
gh run watch "$(gh run list --workflow=build.yml --limit 1 --json databaseId --jq '.[0].databaseId')"
```
**Pass = the macOS job runs on your VM and is green** (note the time; warm ccache is ~8–9 min). If the font-raster golden passes, your baked toolchain matches the fleet — the whole point.

## 3. Join the pool (persistent launchd agent)
```bash
cd ~/Code/pulp
sed -e "s|\$PULP_REPO|$PWD|g" -e "s|\$HOME|$HOME|g" \
  tools/launchd/pulp-tart-runner.plist.template > ~/Library/LaunchAgents/com.danielraffel.pulp.tart-runner.plist
# Edit the rendered plist:
#   EnvironmentVariables → TART_HOME = /Users/<you>/VMs
#   ProgramArguments --labels → self-hosted,macos,arm64,pulp-build,pulp-build-<class>
launchctl load ~/Library/LaunchAgents/com.danielraffel.pulp.tart-runner.plist
launchctl list | grep pulp.tart-runner
tail -F ~/Library/Logs/pulp/tart-runner.log
```
Because `TART_HOME` is in HOME, **no Full Disk Access is needed**. Your Mac now keeps one fresh ephemeral runner ready in the `pulp-build` pool.

## 4. Ship with Shipyard
Normal flow is unchanged — `shipyard pr` runs the gates, opens the PR, validates, and merges on green. The required macOS check routes to the `pulp-build` pool, which now includes your VM runner. Concurrency is **2 VMs per host** (macOS kernel cap), so scale by adding hosts (e.g. 2 Macs → 4 concurrent), not by piling VMs on one.

**Multi-host capacity (when you have ≥2 hosts).** Update Shipyard on each host (`tools/install-shipyard.sh`) to the version with the runner **audit / capacity / reroute-watch** commands, then declare this host in Shipyard's `[host_class.<class>]` config (e.g. `cap = 2`) so reroute-watch knows its free-VM-slot capacity and can drain still-queued cloud macOS jobs to local as slots free up. The `<class>` matches the `pulp-build-<class>` runner label from step 3. (If `shipyard update` returns a GitHub 403, it's a transient API/auth hiccup on that host's fetch — retry after `gh auth` settles; it's per-machine and doesn't affect other hosts.)

## Gotchas (read once)
- **launchd does not expand `$HOME`/`$PULP_REPO`** in plist values — the `sed` above writes absolute paths (a literal `$HOME` log path makes the agent exit 78).
- **Full Disk Access** is only needed if you ignore default #1 and put VMs on `/Volumes` (then grant it to `/bin/bash`). Stick to `~/VMs`.
- **`gh` token via `~/.config/gh/hosts.yml`** (config storage) keeps the runner off the login keychain; **never reset/wipe the host keychain**.
- **`tart delete` silently fails on a *running* VM** — stop → delete → verify.
- **"no IP" on a fresh host is usually a missing ccache mount dir, not networking.** The runner boots each VM with `--dir="ccache:$PULP_CI_CACHE/ccache"` (default `~/.cache/pulp-ci/ccache`); if that dir is absent, `tart run` exits at once (`VZErrorDomain Code=2 "directory sharing device configuration is invalid"`) and you only see a 120 s-later **"no IP"**. `setup-ci-host.sh` pre-creates it and `tart-runner.sh` now `mkdir -p`s it and prints the `tart run` boot log on failure — read that log before suspecting vmnet/DHCP.
- **Copy sparse disk images with `rsync -S`**, and only when stopped.
- **Re-bake on a toolchain/Skia bump**: Skia is pinned in `tools/deps/manifest.json`; the golden's baked Skia is stamped, so a pin bump on a stale golden re-fetches (never stuck on stale). Re-bake the `pulp`/`runner` tiers and re-tag `:<date>` weekly or on a bump.
- **Reusing an old runner machine?** Deregister its stale runners first (on that machine: `cd ~/actions-runner-<name> && ./svc.sh stop && ./svc.sh uninstall && ./config.sh remove`), then onboard fresh per this guide. A drifted bare-metal runner that fails font goldens is the reason to move it to the VM lane.
