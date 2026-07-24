---
name: tart-ci
description: Stand up a fast, cached, isolated, disposable macOS CI lane on Tart — layered golden VM images, ephemeral per-job GitHub Actions runners, host-mounted caches, and a reusable per-repo vm-image manifest. Use when setting up VM-based CI for Pulp or generalizing it to another repo, building/refreshing golden images, wiring ephemeral runners, or debugging the VM lane.
requires:
  - tart            # brew install cirruslabs/cli/tart
  - sshpass         # brew install hudochenkov/sshpass/sshpass (first-boot key injection only)
  - gh              # authenticated; minting JIT runner configs needs repo admin
---

# Tart golden-VM CI lane

Run every macOS build/validation in a **throwaway VM cloned from a versioned golden image** so the host stays responsive and builds are reproducible. Generalizes to any repo via one `vm-image` manifest. Born from Pulp `planning/2026-06-01-macos-ci-isolation-plan.md`; the reusable macOS provider now lives in the sibling `/Volumes/Workshop/Code/tartci` repo. Pulp's `tools/ci/tart-runner.sh` / `tart-run-job.sh` scripts are the legacy/precursor shape and should stay as compatibility wrappers once the tartci lane graduates.

## Why (the failure modes this fixes)
- **Build-dir churn → ODR heap corruption.** One `build/` reconfigured across branches/build-types mixes object layouts → `malloc: error for object 0x3f800000` (that's `1.0f` freed as a pointer) aborting in e.g. `Theme::~Theme`. Every job in a *pristine* clone makes this impossible.
- **Host-local validation is fragile + invasive.** Validating in the editing checkout inherits churn, pops GUI keychain dialogs, and competes for CPU. VMs are headless and disposable.
- **Spotlight/`fseventsd` storms** from build churn make the Mac unusable. VMs + `.metadata_never_index` keep the host calm.

## Core pieces (all in `tools/ci/`)
| Script | Role |
|---|---|
| `setup-ci-host.sh` | **One-command host onboarding** (opinionated): install tart/sshpass, set `~/VMs` (no FDA), acquire the golden (`--copy-from` or bake), install the launchd runner agent with a `--class` label. Mirrors `docs/guides/mac-ci-host-setup.md`. |
| `tart-provision.sh` | Build/refresh layered golden images; `verify`/`tag`/`resize`/`manifest` helpers. Subcommands: `base` → `apple-xcode` → `pulp` → `runner`. |
| `tart-runner.sh` | **Ephemeral per-job GitHub Actions runner.** Mints a JIT (single-job) runner config, clones the runner golden, boots with host ccache mounted, runs one job, destroys the VM. `--loop` boots a fresh VM only when there's queued work for `--workflow-name` / `PULP_RUNNER_WORKFLOW_NAME` (default `Build and Test`) AND a free VM slot (`running_macos_vms < cap`); `--once` for a pilot; `--cap N` overrides the per-host cap. Registers under a **static** name per (host, slot) — `pulp-<class>-<NN>`, derived from the `pulp-build-<class>` label (override with `--name` / `--name-prefix` / `--slot` or `PULP_RUNNER_NAME[_PREFIX]` / `PULP_RUNNER_SLOT`). `--print-name` echoes the derived name and exits (pure; no gh/tart — what `test_tart_runner.py` asserts). |
| `tart-run-job.sh` | **Direct** ephemeral build (no GitHub runner): clone golden → virtio-fs mount host caches → build+ctest in-guest → discard. Useful for Shipyard `backend` / manual builds. |
| `pulp-worktree.sh` | Per-branch worktrees + shared ccache (host-side dev isolation; complements the VM lane). |
| `.shipyard/vm-image.toml` | **The per-repo reuse unit** (see below). |

The reusable runner path is now the sibling `tartci` repo:
- `tartci serve macos --once|--loop --labels ...` owns ephemeral JIT runners.
- `tartci observe macos --json [--runner <name>]` ties GitHub job, local VM,
  guest process, ctest tail, and runner log together.
- `tartci doctor --reap --json` is the local cleanup/health digest.
- `TARTCI_RUNTIME_MEASURE=1 tartci serve ...` records per-job VM timings; use
  `tartci runtime recent|summary|export --repo Generous-Corp/pulp --json` and
  pipe exports into `shipyard metrics import tartci` for long-term agent
  baselines.
- `shipyard --json runner fleet-status --target macos` is the cross-host pool
  view for macOS VM slots and supervisor freshness.

## The vm-image manifest (the unit of reuse)
A new repo adds one `.shipyard/vm-image.toml` and the same `tart-provision.sh manifest <path>` bakes it — no hand-provisioning. Two strategies:
- `strategy = "bake"` — pre-bake a project image (fast clones for hot repos).
- `strategy = "configure-on-boot"` — clone the bare base + apply the manifest on boot (flexible/new repos).

Fields: `base`, `disk_gb`, `auto_login`, `[toolchain].xcode` (omit → no Xcode tier), `[toolchain].rust` + `.rust_targets` + `.rosetta` (Intel cross-build layer — omit → not installed), `[brew].packages`, `[pip].packages`, `[caches].ccache_max`, `[[mounts]]`. See `.shipyard/vm-image.toml` (Pulp: Xcode+Skia), `.shipyard/vm-image.intel.toml` (Intel cross-build layer), and `tools/ci/examples/vm-image.rust-repo.toml` (a light Rust profile, no Xcode — proves generalization).

## Intel (x86_64) cross-build lane — no native Intel hardware needed
Pulp's `darwin-x64` release leg is native on GitHub's `macos-15-intel` (slow, nightly-gated). For an interactive "does the Intel build still work?" check, an **Apple-Silicon host cross-compiles x86_64 and runs it under Rosetta 2** — no Intel Mac, and nothing installed on the host.

- **Golden:** `pulp-intel-build:latest`, baked from `.shipyard/vm-image.intel.toml`. `base = pulp-build-runner:latest` (inherits Xcode + cmake/ninja + baked arm64 Skia) + the Intel layer (`[toolchain].rust` + `rust_targets=["x86_64-apple-darwin"]` + `rosetta`). Do NOT re-declare Xcode in the Intel manifest — it's inherited (re-declaring triggers a multi-hour Xcode re-provision).
  ```
  tools/ci/tart-provision.sh manifest .shipyard/vm-image.intel.toml
  tools/ci/tart-provision.sh tag pulp-intel-build pulp-intel-build
  ```
- **Use it:** `tools/ci/intel-vm-cross-build.sh [--ref <branch|tag|sha>] [--keep] [--full-build]` clones the golden → runs the cross-build recipe on a ref → asserts exact-thin x86_64 (`lipo -archs`) + a Rosetta `pulp help` run → discards the VM.
- **THE cross-build gotcha (baked into the recipe):** `setup.sh --ci` prefetches the **host-arch (arm64)** Skia and `fetch_skia_for_release.py`'s flatten step **won't clobber** it, so the x86_64 `cmake` fails `FindSkia`. The recipe does `rm -rf external/skia-build/build` BETWEEN setup.sh and the `darwin-x64` fetch. Native Intel runners never hit this (host==target).
- **Replicate to another Mac host:** either copy the baked `pulp-intel-build` image (tart export/clone over the network) OR run the manifest on that host — it needs `pulp-build-runner:latest` present there first (it's the base). Same 2-VM/host kernel cap applies; route Intel builds off the required-gate runners.

## Verified base/runtime specifics (cirruslabs + Tart, 2026-06-01)
- Base image `ghcr.io/cirruslabs/macos-tahoe-base:latest` = macOS 26 "Tahoe" (matches Xcode 26.5 / build 17F42). Default creds `admin`/`admin`.
- The vanilla base **already** enables auto-login (kcpassword + `loginwindow autoLoginUser`) and Remote Login (sshd) — *verify*, don't recreate. Auto-login is REQUIRED for WindowServer/Metal.
- `tart set <vm> --disk-size N` only grows, only on a **stopped** VM; the bundled tart-guest-agent grows APFS on next boot.
- Prefer rsync'ing a **host-installed Xcode** into the golden over an in-guest `xcodes install` (interactive 2FA + multi-hour re-download). Remote rsync to `/Applications` needs `--rsync-path="sudo /opt/homebrew/bin/rsync"`.

## Caching strategy (the smart split)
- **Immutable, expensive-to-build deps → baked into the golden (CoW-shared, ~free per clone).** Skia + Dawn are prebuilt static libs (`libskia.a`, `libdawn_combined.a`, ~385 MB) baked at `~/pulp-skia-build`; `SKIA_DIR` points there. They are *never* recompiled.
- **Mutable, growing caches → host-mounted via virtio-fs.** ccache (warm across clones — measured cold→warm 0.6%→88%) and FetchContent sources. Keep `CCACHE_TEMPDIR` **in-guest** (cross-fs rename onto virtio-fs breaks ccache); `CCACHE_BASEDIR` normalizes paths; guest `admin` is uid 501 == host primary user so the shared ccache is writable both ways.

### ccache correctness on a shared, many-worktree cache — the #3504 combo
A host that runs 100+ worktrees off one `.git` against one shared ccache is **cold by construction** unless paths are normalized AND depend mode is off. Two failure modes to keep straight:
- **Cold cache, a perf loss:** no `base_dir` + `hash_dir=true` → every worktree keys on its own absolute path, so nothing hits. Fix: `base_dir=<common worktree parent>` + `hash_dir=false` so a hit compiled in one workspace serves another.
- **Corruption, a correctness scar — this is #3504:** depend mode via `CCACHE_DEPEND`/`depend_mode=true` with the default mtime compiler keying on a *shared* cache serves a stale/false-hit object that corrupts unrelated TUs — a pure function returns `""` and change-unrelated tests fail while clean local Debug+Release pass. Fix: **depend mode OFF** plus `compiler_check=content`. Direct mode stays on: fast and correct once depend is off.

These live in the versioned scripts so hosts converge, not just in one host's live config: `tools/ci/bootstrap-macos-host.sh` `tune_ccache()` writes them into the shared `$CCACHE_DIR/ccache.conf` (base_dir derived from the CI work root, never a hardcoded home), and `tools/ci/pulp-worktree.sh` `cache_env()` emits `CCACHE_NODEPEND=true` + `CCACHE_COMPILERCHECK=content` for the shipyard/worktree lane. `.github/workflows/build.yml` forces the same combo via job env for the GH-runner lane. Never turn depend mode back on for speed.

## GPU works in the guest (no hybrid lane needed)
Apple Virtualization provides Metal in the guest **even with `--no-graphics`**. Verified: `pulp-screenshot --backend skia` renders a real Skia/Metal PNG, `nm pulp-ui-preview | grep MacGpuWindowHost` = present. So the full mac lane (build + GPU + tests) runs in-VM.

## Dependency updates → golden re-bake (don't get stuck on old deps)
Skia/Dawn are pinned in `tools/deps/manifest.json` (release-asset URL + sha256 per platform; `tools/scripts/fetch_skia_for_release.py` consumes it). Single source of truth = the manifest pin.
- When deps bump (new `danielraffel/skia-builder` release, e.g. `chrome/m149` → newer), the Dependency Update Workflow bumps the manifest pin.
- **Then re-bake the golden** so its baked Skia matches: re-run the `pulp`/`runner` tiers (they should fetch per the *current* pin, not a stale copy), tag a new `:<date>`, refresh `:latest`. Ephemeral jobs clone `:latest` → get the new Skia. Keep the last 1–2 dated goldens per tier; prune older.
- Tie golden re-bakes to dependency-bump PRs and toolchain bumps (Xcode). Pinning Xcode + Skia in-image keeps font/raster goldens reproducible.

## Concurrency & hosts
- **macOS caps 2 concurrent running VMs PER HOST** (kernel quota; booting a 3rd throws "number of VMs exceeds the system limit"). For ≥3 concurrent, **distribute runners across multiple Macs** (e.g. Mac Studio + MacBook Pro M5 → 2+2 = 4) — each runs `tart-runner.sh`; new hosts inherit the host-class label (`*-studio`, `*-m1`, `*-m5`) and cap=2. A dedicated Studio *can* raise the cap via the kernel-quota override (plan Appendix D; SIP off + dev kernel — last resort).
- A persistent operator VM (e.g. `pulp-vm`) on a host consumes 1 of its 2 slots.
- **Capacity-aware local queue draining is implemented and VM-slot-aware.** The current tartci/Shipyard path shares one rule: a host has free macOS capacity when `running_macos_vms < cap` (cap = 2/host), and only macOS/Darwin guests consume the `macos` VM slot. Linux Tart and Windows QEMU lanes use their own labels, supervisors, and caps; they do not reduce macOS free slots, though CPU/RAM can still need route weights or reservations.
- **Local-first policy:** Pulp's automatic macOS overflow is disabled with `PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON=local-only`. Do not point full-local saturation at GitHub-hosted `macos-15`; let jobs queue for the next local Mac slot. Hosted macOS is an explicit operator fallback for a local fleet outage/unhealthy fleet or a workflow that intentionally wants hosted coverage. Rollback for the old behavior: `gh variable set -R Generous-Corp/pulp PULP_OVERFLOW_BUILD_MACOS_RUNS_ON_JSON --body '["macos-15"]'`.
- **Production required macOS route is VM-first (2026-06-10):** `PULP_LOCAL_MACOS_RUNS_ON_JSON=["self-hosted","macOS","ARM64","pulp-build","pulp-build-vm"]` and `PULP_LOCAL_MAC_RUNNER_LABEL=pulp-build-vm`. The VM supervisors advertise both `pulp-build` and `pulp-build-vm`; bare-metal `pulp-build` runners stay online but are excluded from the default route by the extra `pulp-build-vm` label. Full rollback: restore `PULP_LOCAL_MACOS_RUNS_ON_JSON` to `["self-hosted","pulp-build"]`, restore `PULP_LOCAL_MAC_RUNNER_LABEL=pulp-build`, and unload the VM LaunchAgents if the VM pool itself is unhealthy.

## Linux + Windows pool runners (join the Actions pool like macOS)

Each platform serves the GitHub Actions pool via its own ephemeral per-job
runner supervisor — the analog of `tart-runner.sh` for macOS:

| Supervisor | VM | Golden | Labels (pilot) | LaunchAgent |
|---|---|---|---|---|
| `tools/ci/tart-runner.sh` | Tart macOS | `pulp-build-runner` | `…,pulp-build` | `com.danielraffel.pulp.tart-runner` |
| `tools/ci/tart-runner.sh --workflow-name Coverage` | Tart macOS | `pulp-build-runner` | `…,pulp-coverage-vm-macos` | `com.danielraffel.pulp.tart-runner-coverage-macos` |
| `tools/ci/tart-runner-linux.sh` | Tart Linux | `pulp-linux-build` | `…,Linux,ARM64,pulp-build-linux` | `com.danielraffel.pulp.tart-runner-linux` |
| `tools/ci/qemu-runner-windows.sh` | QEMU Windows | `pulp-windows-build-*.qcow2` | `…,Windows,ARM64,pulp-build-windows` | `com.danielraffel.pulp.qemu-runner-windows` |

All three: mint a JIT (single-job) runner config → boot a throwaway clone
(Tart CoW for Linux, qcow2 overlay on a dynamic SSH port for Windows) → run the
baked `~/actions-runner` agent once → discard. The goldens carry the
`actions-runner-{linux-arm64,win-arm64}` agent (Windows install-if-missing if a
golden predates the bake). `--loop` only boots when there's queued
`Build and Test` work. The macOS coverage lane is the exception: it runs the
same Tart supervisor with `--workflow-name Coverage` and a dedicated
`pulp-coverage-vm-macos` label. Keep `--queue-match-labels` enabled for this
lane so hosted Coverage jobs do not boot a local VM that cannot claim them.

Coverage/sanitizer/release lanes must never reuse the warm macOS gate labels or
the shared `pulp-build-vm` build-pilot label. Coverage routing belongs in
`PULP_COVERAGE_MACOS_RUNS_ON_JSON` or a one-off `macos_runner_selector_json`
dispatch, with a dedicated ephemeral label such as `pulp-coverage-vm-macos`.

**Per-platform opt-in/out** is the Shipyard macOS GUI's "Serve CI builds from
this Mac" switch: each lane is a `CIServingLane` toggled by `launchctl
load/unload` of its LaunchAgent (the labels above). Install a lane on a host by
sed-templating its `tools/launchd/*.plist.template` into `~/Library/LaunchAgents`
(replace `$PULP_REPO`, `$HOME`, and `$TART_HOME`/`$TARTCI_GOLDENS` — launchd
doesn't expand shell vars). Pulp CI routes to these via `build.yml`'s opt-in
`PULP_LOCAL_{LINUX,WINDOWS}_RUNS_ON_JSON` repo vars (default off → github-hosted).

**Hard-won Windows-runner gotchas:**
- The multi-KB JIT blob must NEVER ride a command line — through the
  ssh→cmd.exe→powershell chain it blows cmd's 8191-char limit ("The command line
  is too long"), whether passed as a `run.cmd --jitconfig` arg OR embedded in a
  `powershell -EncodedCommand`. Stream it into a file via **ssh stdin**, then run
  `Runner.Listener.exe run --jitconfig (Get-Content jit.cfg)`.
- A golden may cache the Actions runner binaries, but it must not reuse a stale
  registration. Scrub `C:\actions-runner\.runner`, `.credentials`,
  `.credentials_rsaparams`, `.env`, `.path`, and `jit.cfg` before every JIT run;
  otherwise the guest can connect to GitHub and then fail with "runner
  registration has been deleted" before claiming the queued job.
- `git reset --hard` (+ `core.autocrlf false`) for checkout — the golden's tree
  carries autocrlf churn that aborts a plain `git checkout`.

## Shipping FROM a VM-only runner host
A VM-only host (no host-side cmake/Xcode/Skia — builds only ever run *inside*
the VMs) can serve the pool fine, but `shipyard pr` initiated **on** it needs
care: the default `[targets.mac]` is `backend = "local"` (build on the host) and
fails there ("cmake/git-lfs not found"). Don't reach for `backend = "ssh"` to a
build box either — `auval` (AU validation) needs a real login/audio session to
register the component, so it fails "didn't find the component" over a headless
`ssh host cmd` (compile + the rest of ctest pass; only the auval tests fail).
The lane where auval works is one *with* a session: the self-hosted pool's
**auto-login VMs** (`auto_login = true` in the manifest — this is why) or a
cloud macOS runner. So on a VM-only host set, in gitignored `.shipyard.local`:
```toml
[targets.mac]
backend  = "cloud"
workflow = "build"   # dispatch build.yml; resolve-provider routes mac local-first
platform = "macos-arm64"
```
`shipyard pr` then dispatches `build.yml`; its `resolve-provider` sends the mac
leg to the self-hosted pool's auto-login VM (auval green), with cloud overflow.
Flip a lane mid-flight with `shipyard cloud retarget --target mac --provider <p>`.
(SSH build hosts that you *do* want to drive headless need brew on the non-login
PATH — `eval "$(brew shellenv)"` in `~/.zshenv`, not just `~/.zprofile` — or
`ssh host cmd` won't find cmake.)

## Diagnosing a red macOS leg (read the runner's LOCAL logs — `gh` is opaque)

On a self-hosted macOS runner, `gh run view --log` / `--job` returns **nothing
useful** for the build/test step — only "Process completed with exit code N"
(exit 8 = ctest had test failures), and check-run annotations are empty too. You
**cannot** tell which test failed from GitHub's API. But the runner persists
everything locally, so **if you're on the runner host** (true for Pulp's Mac
Studio — the deps here are the self-hosted Tart/Shipyard pool), read it directly:

1. **Find the work dir** from the runner config — it is NOT `~/actions-runner-*/_work`:
   ```bash
   # workFolder is custom on Pulp's runners:
   grep workFolder ~/actions-runner-pulp-studio-01/.runner
   #   → "/Volumes/Workshop/ci/pulp/work/pulp-studio-01"
   ```
2. **Read ctest's own result logs** in the persisted build dir (`clean:false` on
   self-hosted keeps `build-<key>` warm across runs; `build-macos` is the macOS
   leg). These are the authoritative source for *which* test failed and *why*:
   ```bash
   WS=/Volumes/Workshop/ci/pulp/work/pulp-studio-01/pulp/pulp   # <workFolder>/<repo>/<repo>
   cat "$WS/build-macos/Testing/Temporary/LastTestsFailed.log"  # failed test names (+ ctest index)
   grep -aA25 '<test name>' "$WS/build-macos/Testing/Temporary/LastTest.log"  # the failing REQUIRE + expansion
   ```
   `LastTestsFailed.log` lines look like `7675:pulp-import-design reports help…`;
   `LastTest.log` has the full Catch2 output (`file:line: FAILED:` + `with
   expansion:`). Check all three runners (`pulp-studio-01/02/03`) — the leg runs
   on whichever was free; the freshest `LastTestsFailed.log` mtime is the run.
3. The **step command/env** (not its output) lives in
   `~/actions-runner-<name>/_diag/Worker_*.log` (most-recent file = most-recent
   job; match by the commit SHA, which the checkout echoes). Read step *output*
   from the ctest logs in (2), not from the Worker log.

A "red macOS leg with exit code 8" on a change that can't plausibly affect macOS
is usually a **pre-existing failure on `main`** that the (flaky/overflowing) gate
let slip — e.g. a stale exact-match CLI assertion. Read the ctest log, fix the
real failing test, don't chase your own diff. (Found 2026-06-03: #3386's
`--emit swiftui` broke two `test_import_design_tool.cpp` exact-match asserts on
main, reddening every PR's macOS gate.)

## Rollout: pilot → graduate
1. **Additive pilot (safe):** run `tartci serve macos --once` with a **non-required** label (`pulp-build-vm`). Trigger a real job without touching required routing: `gh workflow run build.yml -f macos_runner_selector_json='["self-hosted","pulp-build-vm"]'`. Confirm green.
2. **Required-label prevalidation (safe):** run a one-shot VM with `pulp-build` **plus a unique proof label**, then dispatch `Build and Test` with `macos_runner_selector_json` requiring both labels. This proves a VM can satisfy the required label while bare-metal `pulp-build` remains online. Verified 2026-06-10: run `27250564395`, runner `tartci-phase6-pulp-build-proof-r2-20260610`, `macOS (ARM64) [operator]` success, `macos` alias success, VM/JIT runner cleaned up. Cancel unrelated Linux/Windows legs after `macos` is green.
3. **Graduated production default route (active 2026-06-10):** persistent VM supervisors now advertise `self-hosted,macOS,ARM64,pulp-build,pulp-build-vm`, and Pulp's default required macOS selector requires that full set. Real `Build and Test` jobs have drained on both the controller VM runner and the secondary-host VM runner:
   - run `27251134234`: default dispatch, no selector override, `pulp-vm-01`, `macOS (ARM64) [local]` success, `macos` alias success; hosted leftovers canceled after `macos` went green.
   - run `27251378268`: real PR, secondary-host `pulp-vm-m5-pilot-01`, `macOS (ARM64) [local]` success, `macos` alias success.
   - run `27251442228`: real PR, controller `pulp-vm-01`, `macOS (ARM64) [local]` success, `macos` alias success.
4. **Rollback path:** keep bare-metal fallback online. To route back to bare-metal:
   ```bash
   gh variable set -R Generous-Corp/pulp PULP_LOCAL_MACOS_RUNS_ON_JSON --body '["self-hosted","pulp-build"]'
   gh variable set -R Generous-Corp/pulp PULP_LOCAL_MAC_RUNNER_LABEL --body 'pulp-build'
   launchctl bootout "gui/$(id -u)/com.danielraffel.pulp.tart-runner"
   ssh <secondary-host> 'launchctl bootout "gui/$(id -u)/com.danielraffel.pulp.tart-runner-macos-pilot"'
   ```

## Gotchas (hard-won)
- **NEVER run signing/keychain tests on a non-VM host.** `check_notarization`/codesign tests call `security`/`codesign`/`notarytool`, which on an interactive host pop GUI keychain dialogs and can disrupt the default keychain. Run them **only in the disposable VM**. Never click "Reset To Defaults" on a keychain prompt on a real Mac; never wipe a host keychain.
- **Gatekeeper disabled in the CI base:** the cirruslabs base ships `spctl --master-disable`, so `spctl --assess` returns 0 for *any* path (even nonexistent). `check_notarization` was hardened with an `fs::exists` short-circuit so it's correct in both environments (see the `ship` skill).
- **Clean the build dir on build-type flips.** Shipyard `backend=local` reconfiguring Debug over a Release `build/` reproduces the ODR churn → false test failures. `rm -rf build` first, or (better) validate in the VM, not the editing checkout.
- **An unset `TART_HOME` is a hard error, by design — do not "fix" it with a default.** Every VM tool resolves the store through `tools/ci/lib/tart-home.sh`: `TART_HOME` from the env, else `vm_home` from `tartci host-profile --json`, else it dies naming the fix. It refuses to guess because a wrong store is **silent**: `tart list` against a path with no VMs is an empty list, not an error. That is how the stray-VM reaper spent its life defaulting to `$HOME/VMs` on a host whose store was elsewhere — inspecting an empty universe, reaping nothing, and exiting 0 reporting success. If a launchd agent dies with the TART_HOME error, the plist is missing its `<key>TART_HOME</key>` (launchd does not read your shell profile), or its `$TART_HOME` placeholder was never substituted. Tests: `tools/scripts/test_tart_home_resolution.py`.
- **Disk: sparse + CoW.** Each `disk.img` is a sparse 150 GB file (~45 GB real); `du`/Finder show apparent size (N×150 GB) but `df` shows the truth (CoW clones share blocks — e.g. 13 VMs ≈ 313 GB real). Don't panic at apparent size; prune redundant bare working VMs (tags retain shared blocks).
- **First key injection needs `sshpass`** (password auth once); afterward everything uses the injected `id_ed25519`. `tart ip` can take ~10–120 s after boot — poll, don't fixed-sleep.
- **A missing `--dir` mount target reads as a fake "no IP".** `tart-runner.sh` boots each VM with `--dir="ccache:$PULP_CI_CACHE/ccache"` (default `$HOME/.cache/pulp-ci/ccache`). If that host dir doesn't exist — the common case on a **fresh CI host** — `tart run` exits *immediately* with `VZErrorDomain Code=2 "directory sharing device configuration is invalid"`, so the VM never boots and the runner times out 120 s later reporting **"no IP"** — pointing you at networking when the real cause is a missing directory. `setup-ci-host.sh` now pre-creates it and `tart-runner.sh` both `mkdir -p`s it and prints the `tart run` boot log on failure. If you ever see "no IP", read the boot log it now emits before suspecting vmnet/DHCP. (Diagnosed on the secondary M-series host bring-up, 2026-06-01: a no-`--dir` boot got an IP in 0 s while a `--dir`-to-missing-path boot died instantly — the mount, not the network.)
- **Use shipyard (its own higher-quota auth) over raw `gh`** for GitHub ops to avoid rate limits; `gh`'s token lives in the login keychain (or `~/.config/gh/hosts.yml` with config storage).
- **Persistent runner via launchd: Full Disk Access + absolute paths.** Run the supervisor as a LaunchAgent (`tools/launchd/pulp-tart-runner.plist.template`) so it survives reboot. Two traps: (1) launchd does NOT expand `$HOME`/`$PULP_REPO` in plist values — the install `sed` must write absolute paths (a literal `$HOME` log path → exit 78); (2) a LaunchAgent can't read a `/Volumes` external VM store without **Full Disk Access** (exit 126 "Operation not permitted") — grant it in System Settings → Privacy & Security → Full Disk Access. The interactive shell has this access; the agent does not.
- **Every per-job step in a `set -euo pipefail` supervisor must clean up on its own failure.** The runner supervisors (`tools/ci/qemu-runner-windows.sh`, `tart-runner-linux.sh`) boot a VM/overlay, then run several SSH/QEMU steps. Any *unguarded* command that can fail (a dropped SSH, a PowerShell error streaming the JIT blob to a file) will abort the whole script under `set -e` **before** the trailing `kill "$qpid"; rm -rf "$jobdir"` cleanup — leaking a live QEMU process + overlay dir that a launchd `--loop` runner then trips over on KeepAlive restart. Mirror the surrounding steps: append `|| { note …; kill "$qpid" 2>/dev/null||true; rm -rf "$jobdir"; return 1; }` to each fallible per-job command, including pipelines (the JIT-config stdin→file upload was the one that slipped through). Caught by Codex review on tartci#10, fixed in both the tartci port and this original.
- **`die` (which `exit`s) must never be reachable from a per-iteration path — `|| true` cannot catch an `exit`.** All three supervisors (`tart-runner.sh`, `tart-runner-linux.sh`, `qemu-runner-windows.sh`) define `die(){ …; exit 1; }` for fatal preconditions. `exit` inside a *function* terminates the whole script, so a `die` reached from `run_one` (JIT mint failure, "no IP after 120s") killed the entire supervisor even though the loop called it as `run_one "$i" || true` — `||` guards a non-zero *return*, not an `exit`. One transient blip (a `gh` token hiccup, one slow VM boot) therefore took the runner down until launchd KeepAlive noticed, and repeated fast deaths hit launchd's **respawn throttle**, turning a seconds-long outage into a long one. The rule: `die` only for preconditions checked **before** the loop starts (missing `tart`/`gh`/golden, bad args); every failure inside `run_one` uses `soft_fail` (same red line, `return 1`) so the loop counts the failure, backs off (`backoff_secs`: POLL, 2×, 4×… capped at `PULP_VM_BACKOFF_MAX`, default 300s), and keeps serving. Related trap: because the loop invokes `run_one` in a `||`/`if` context, `set -e` is suppressed for its **whole body** — so `tart clone` / `qemu-img create` need *explicit* `|| { soft_fail …; return 1; }`, or a failure silently falls through instead of aborting. `test_tart_runner.py` pins this behaviorally by driving the real loop against stub `gh`/`tart` binaries and asserting it reaches iteration 2, plus a structural guard that no `die` reappears in any `run_one`.

- **A `--loop` supervisor must tear down its in-flight VM on SIGTERM, not only after each job.** Per-job cleanup runs when the agent exits normally, but `launchctl unload` (the Shipyard GUI "Serve CI builds from this Mac" toggle OFF) sends **SIGTERM** mid-job or mid-wait — and a warm JIT runner can sit with a VM up for hours waiting for a job. Without a trap, stopping reclaims RAM (launchd kills the `tart run` child) but orphans a **stopped CoW clone** on disk and leaves the runner **registered-but-offline** on GitHub. Track the live VM at script scope (`CURRENT_VM`/`CURRENT_RPID`) and `trap 'discard_current_vm; trap - EXIT; exit 143' INT TERM` + `trap discard_current_vm EXIT`, clearing the vars in each normal teardown path so the EXIT trap no-ops. **The teardown must be FAST** — launchd SIGKILLs the supervisor shortly after SIGTERM, so a graceful `tart stop` + `sleep` can be cut short and leave a *stopped* clone behind (RAM freed, but `tart delete` never runs — observed live on the M5). Hard-`kill -9` the `tart run` host PID (ends the VM at once), then `tart delete` immediately — no `sleep`. `tart-runner.sh` does this; mirror it in the Linux/Windows pool runners when they grow a GUI toggle.

- **Runner names are STATIC per (host, slot) — reclaim before reuse.** The supervisor registers as `pulp-<class>-<NN>` (e.g. `pulp-m5-01`), not the old `ephr-<pid>-<counter>` churn, so the same physical Mac is recognizable in the Settings → Actions → Runners list and matches the bare-metal `pulp-studio-01` convention. A static name is only reusable if you clear the prior identity first: a SIGKILL'd supervisor / errored job / crashed clone can leave a stale GitHub registration (shows **Offline**) *and/or* a stopped Tart clone of the same name — and `generate-jitconfig` rejects a duplicate name while `tart clone` rejects an existing VM. `reclaim_runner_name` (runs before every mint) deletes both, best-effort — the JIT-lane equivalent of bare-metal `config.sh --replace`. The class comes from the `pulp-build-<class>` label `setup-ci-host.sh --class <name>` writes into the plist, so no plist edit is needed for the name. **Two supervisors on one host** (to use the 2-VM cap) must run with **distinct `--slot`** (→ `-01`/`-02`) or their static names collide. Seeing a lone **Offline** `pulp-<class>-NN` row alongside an Idle one is normal — it's a leftover the next reclaim clears.

- **Coverage VM lane: three things the build lane didn't need.** (1) **The label-matched queue scan must cover `in_progress` runs, not just `queued`.** A Coverage (or Release CLI) run flips to `in_progress` the moment its GitHub-hosted resolver/classify job starts — *before* the self-hosted macOS leg is even queued — so a queued-only run scan sees `q=0` forever and the VM never boots. `tart-runner.sh queued_work` and the tartci macOS provider both iterate `for st in queued in_progress`. (2) **Run the coverage agent from `$HOME` like the build runner** (`~/.local/bin/tartci serve macos --loop`, `TART_HOME=$HOME/VMs`), NOT a repo checkout on `/Volumes` — the FDA/`Operation not permitted` trap above bites the coverage agent too, and the `$HOME` layout sidesteps it entirely instead of needing Full Disk Access. (3) **Cap the coverage supervisor at 1** (`TARTCI_MACOS_VM_CAP=1`): label isolation (`pulp-coverage-vm-macos`, never `pulp-build`/`pulp-build-vm`) keeps the *jobs* off the gate, but coverage VMs still share host slots, so the cap is what stops an advisory coverage run from occupying every slot and stalling the required `macos` gate. Routing var: `PULP_COVERAGE_MACOS_RUNS_ON_JSON`.

- **Cap=1 is necessary but NOT sufficient — a long secondary VM still throttles the gate. Use the priority-aware idle gate.** The coverage lane above was **backed out 2026-06-16**: with shared `TART_HOME` and cap=1 it booted whenever the host was idle, then *held* one of the two macOS slots for ~1h, so a required-gate burst (which wants both slots) ran at half throughput and ultimately wedged the gate (launchctl exit 126). cap=1 stops "occupy *every* slot" but not "hold the slot the gate needs." The fix is the tartci provider's opt-in **idle gate**: set `TARTCI_YIELD_TO_WORKFLOW_NAME` (e.g. `Build and Test`) + `TARTCI_YIELD_TO_LABELS` (the gate pool) so the secondary lane boots only when the gate has **no** queued/in-progress work (`priority_demand()` in `providers/tart-macos/runner.sh`; preview with `serve macos --print-priority-demand`; behaviorally tested in `tartci/scripts/test_idle_gate.py`). **Keep the secondary lane on the SAME `TART_HOME` as the gate** so `running_macos_vms` stays a true host-wide 2-guest semaphore — a *separate* store hides the secondary VM from the gate's count and lets total guests hit 3 → the 3rd `tart run` fails on Apple's host-wide cap (and duplicates the ~150GB golden). The same idle gate is how to re-enable coverage safely.

- **A lease DENIAL is a capacity report — never answer it with a host-derived bound.** `tools/ci/governed-build.sh` wraps Shipyard's `local` mac backend (the one path that does NOT go through the `pulp` CLI, so the CLI's lease integration never sees it). It sizes a lease from `tartci host-profile`'s `PULP_BUILD_JOBS`; when that is refused, the only safe responses are a **smaller lease sized from `tartci leases status --json` → `capacity.non_gate_available_cores`**, or leaseless at a conservative floor (`PULP_GOVERNED_BUILD_MIN_JOBS`, default 2). It originally fell back to the **tier-0** bound, `min(cores, RAM_budget/1.5 GiB)` — which reads conservative but is not: on a big-RAM host the memory axis never binds and tier-0 degrades to the **full core count**, so a refusal was answered by running *wider* than the request that had just been denied. Observed on the 28-core/96 GB Studio while it served the required `macos` gate: profile asked 12, store reported **6** free (limit 12, a Tart VM holding 6), denial → **leaseless `-j28`**, host load 50 → 68 in ~90 s. Two shell traps make this easy to re-break under `set -euo pipefail`: a bare `[ a -lt b ] && x=…` whose test is *false* returns non-zero and **kills the script**, and `v="$(f)"` propagates `f`'s failure the same way — so any capacity probe must `return 0` and the comparisons must be full `if` blocks. The **binding limit is the non-gate pool, not `available_cores`** (12 was denied while `available_cores` was 20 and non-gate was 6). Behavior is pinned by `tools/ci/test_governed_build.py` (stub `tartci` on PATH; no compile, no lease store) — registered as the `governed-build-selftest` ctest, because for a long while nothing under `tools/ci/test_*.py` ran anywhere, which is how the unbounded fallback shipped in the first place.

- **Sanitizer VM lane — the first idle-gate consumer; localize TSan only.** `tools/launchd/pulp-tart-runner-sanitizer-macos.plist.template` (label `pulp-sanitizer-vm-macos`, workflow `Sanitizer Tests`, cap=1, shared `$HOME/VMs`, idle-gate env) serves the advisory sanitizer matrix. Pilot is **TSan only**: it is the longest leg (scoped `-j1` serial, ~45 min on `macos-14`), the highest-value for the threaded audio model, and single-core-bound so it gains most from a local M-series host. ASan stays on `macos-15` and UBSan stays on `macos-26` — the four run in parallel on GitHub but serialize (~4×) on one cap=1 lane, slower than hosted except during a backlog; full parallel local sanitizers need a 3rd host. `sanitizers.yml` carries `--deny-labels pulp-build,pulp-build-vm` on the 3 macOS sanitizers so one can never land on the gate pool. Flip `PULP_SANITIZER_TSAN_RUNS_ON_JSON` only after a `workflow_dispatch` proof on the lane, one sanitizer at a time behind a measured gate-latency + matrix-wall-clock go/no-go.

## Store & hygiene
**`TART_HOME` is declared by the host, never by the repo** — hosts with an external build SSD keep the store on a `/Volumes` mount, hosts on internal storage keep it under `$HOME`, and both are correct. Exclude it from Spotlight (`.metadata_never_index`). Tag goldens `:<date>` + roll `:latest`. Ephemeral job VMs are deleted after use; confirm cleanup (`tart delete` fails silently on a *running* VM — stop → delete → verify). Reclaim with `tart-provision.sh list` + prune.
