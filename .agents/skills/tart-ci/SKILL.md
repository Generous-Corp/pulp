---
name: tart-ci
description: Stand up a fast, cached, isolated, disposable macOS CI lane on Tart — layered golden VM images, ephemeral per-job GitHub Actions runners, host-mounted caches, and a reusable per-repo vm-image manifest. Use when setting up VM-based CI for Pulp or generalizing it to another repo, building/refreshing golden images, wiring ephemeral runners, or debugging the VM lane.
requires:
  - tart            # brew install cirruslabs/cli/tart
  - sshpass         # brew install hudochenkov/sshpass/sshpass (first-boot key injection only)
  - gh              # authenticated; minting JIT runner configs needs repo admin
---

# Tart golden-VM CI lane

Run every macOS build/validation in a **throwaway VM cloned from a versioned golden image** so the host stays responsive and builds are reproducible. Generalizes to any repo via one `vm-image` manifest. Born from Pulp `planning/2026-06-01-macos-ci-isolation-plan.md`; the scripts live in `tools/ci/`.

## Why (the failure modes this fixes)
- **Build-dir churn → ODR heap corruption.** One `build/` reconfigured across branches/build-types mixes object layouts → `malloc: error for object 0x3f800000` (that's `1.0f` freed as a pointer) aborting in e.g. `Theme::~Theme`. Every job in a *pristine* clone makes this impossible.
- **Host-local validation is fragile + invasive.** Validating in the editing checkout inherits churn, pops GUI keychain dialogs, and competes for CPU. VMs are headless and disposable.
- **Spotlight/`fseventsd` storms** from build churn make the Mac unusable. VMs + `.metadata_never_index` keep the host calm.

## Core pieces (all in `tools/ci/`)
| Script | Role |
|---|---|
| `setup-ci-host.sh` | **One-command host onboarding** (opinionated): install tart/sshpass, set `~/VMs` (no FDA), acquire the golden (`--copy-from` or bake), install the launchd runner agent with a `--class` label. Mirrors `docs/guides/mac-ci-host-setup.md`. |
| `tart-provision.sh` | Build/refresh layered golden images; `verify`/`tag`/`resize`/`manifest` helpers. Subcommands: `base` → `apple-xcode` → `pulp` → `runner`. |
| `tart-runner.sh` | **Ephemeral per-job GitHub Actions runner.** Mints a JIT (single-job) runner config, clones the runner golden, boots with host ccache mounted, runs one job, destroys the VM. `--loop` keeps a fresh one ready; `--once` for a pilot. |
| `tart-run-job.sh` | **Direct** ephemeral build (no GitHub runner): clone golden → virtio-fs mount host caches → build+ctest in-guest → discard. Useful for Shipyard `backend` / manual builds. |
| `pulp-worktree.sh` | Per-branch worktrees + shared ccache (host-side dev isolation; complements the VM lane). |
| `.shipyard/vm-image.toml` | **The per-repo reuse unit** (see below). |

## The vm-image manifest (the unit of reuse)
A new repo adds one `.shipyard/vm-image.toml` and the same `tart-provision.sh manifest <path>` bakes it — no hand-provisioning. Two strategies:
- `strategy = "bake"` — pre-bake a project image (fast clones for hot repos).
- `strategy = "configure-on-boot"` — clone the bare base + apply the manifest on boot (flexible/new repos).

Fields: `base`, `disk_gb`, `auto_login`, `[toolchain].xcode` (omit → no Xcode tier), `[brew].packages`, `[pip].packages`, `[caches].ccache_max`, `[[mounts]]`. See `.shipyard/vm-image.toml` (Pulp: Xcode+Skia) and `tools/ci/examples/vm-image.rust-repo.toml` (a light Rust profile, no Xcode — proves generalization).

## Verified base/runtime specifics (cirruslabs + Tart, 2026-06-01)
- Base image `ghcr.io/cirruslabs/macos-tahoe-base:latest` = macOS 26 "Tahoe" (matches Xcode 26.5 / build 17F42). Default creds `admin`/`admin`.
- The vanilla base **already** enables auto-login (kcpassword + `loginwindow autoLoginUser`) and Remote Login (sshd) — *verify*, don't recreate. Auto-login is REQUIRED for WindowServer/Metal.
- `tart set <vm> --disk-size N` only grows, only on a **stopped** VM; the bundled tart-guest-agent grows APFS on next boot.
- Prefer rsync'ing a **host-installed Xcode** into the golden over an in-guest `xcodes install` (interactive 2FA + multi-hour re-download). Remote rsync to `/Applications` needs `--rsync-path="sudo /opt/homebrew/bin/rsync"`.

## Caching strategy (the smart split)
- **Immutable, expensive-to-build deps → baked into the golden (CoW-shared, ~free per clone).** Skia + Dawn are prebuilt static libs (`libskia.a`, `libdawn_combined.a`, ~385 MB) baked at `~/pulp-skia-build`; `SKIA_DIR` points there. They are *never* recompiled.
- **Mutable, growing caches → host-mounted via virtio-fs.** ccache (warm across clones — measured cold→warm 0.6%→88%) and FetchContent sources. Keep `CCACHE_TEMPDIR` **in-guest** (cross-fs rename onto virtio-fs breaks ccache); `CCACHE_BASEDIR` normalizes paths; guest `admin` is uid 501 == host primary user so the shared ccache is writable both ways.

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
- Capacity-aware cloud→local queue draining belongs in Shipyard's multi-Mac controller: its "local idle" signal must become "free VM slot" (`running_macos_vms < cap`, summed across hosts) so a freed slot drains a still-queued job instead of leaving it on cloud. (Track the cross-cutting work in `planning/`, not here.)

## Rollout: pilot → graduate
1. **Additive pilot (safe):** run `tart-runner.sh --once` with a **non-required** label (`pulp-build-vm`). Trigger a real job without touching required routing: `gh workflow run build.yml -f macos_runner_selector_json='["self-hosted","pulp-build-vm"]'`. Confirm green.
2. **Graduate:** add the runner to the required `pulp-build` pool (or stage replacing bare-metal), distributed across hosts. Never point a required check at an empty label; preflight that the label has online runners.

## Gotchas (hard-won)
- **NEVER run signing/keychain tests on a non-VM host.** `check_notarization`/codesign tests call `security`/`codesign`/`notarytool`, which on an interactive host pop GUI keychain dialogs and can disrupt the default keychain. Run them **only in the disposable VM**. Never click "Reset To Defaults" on a keychain prompt on a real Mac; never wipe a host keychain.
- **Gatekeeper disabled in the CI base:** the cirruslabs base ships `spctl --master-disable`, so `spctl --assess` returns 0 for *any* path (even nonexistent). `check_notarization` was hardened with an `fs::exists` short-circuit so it's correct in both environments (see the `ship` skill).
- **Clean the build dir on build-type flips.** Shipyard `backend=local` reconfiguring Debug over a Release `build/` reproduces the ODR churn → false test failures. `rm -rf build` first, or (better) validate in the VM, not the editing checkout.
- **Disk: sparse + CoW.** Each `disk.img` is a sparse 150 GB file (~45 GB real); `du`/Finder show apparent size (N×150 GB) but `df` shows the truth (CoW clones share blocks — e.g. 13 VMs ≈ 313 GB real). Don't panic at apparent size; prune redundant bare working VMs (tags retain shared blocks).
- **First key injection needs `sshpass`** (password auth once); afterward everything uses the injected `id_ed25519`. `tart ip` can take ~10–120 s after boot — poll, don't fixed-sleep.
- **Use shipyard (its own higher-quota auth) over raw `gh`** for GitHub ops to avoid rate limits; `gh`'s token lives in the login keychain (or `~/.config/gh/hosts.yml` with config storage).
- **Persistent runner via launchd: Full Disk Access + absolute paths.** Run the supervisor as a LaunchAgent (`tools/launchd/pulp-tart-runner.plist.template`) so it survives reboot. Two traps: (1) launchd does NOT expand `$HOME`/`$PULP_REPO` in plist values — the install `sed` must write absolute paths (a literal `$HOME` log path → exit 78); (2) a LaunchAgent can't read a `/Volumes` external VM store without **Full Disk Access** (exit 126 "Operation not permitted") — grant it in System Settings → Privacy & Security → Full Disk Access. The interactive shell has this access; the agent does not.

## Store & hygiene
`TART_HOME=/Volumes/Workshop/VMs` (Spotlight-excluded via `.metadata_never_index`). Tag goldens `:<date>` + roll `:latest`. Ephemeral job VMs are deleted after use; confirm cleanup (`tart delete` fails silently on a *running* VM — stop → delete → verify). Reclaim with `tart-provision.sh list` + prune.
