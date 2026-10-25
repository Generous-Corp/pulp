---
name: mise
description: |
  Use Pulp's locked mise toolchain consistently across macOS, Linux, and Windows.
  Apply this when building Pulp, creating a disposable Windows VM, investigating
  toolchain drift, or changing the canonical toolchain manifest. Keep mise
  opt-in at shell entry, fail closed on lock drift, and distinguish pinned
  toolchain evidence from a build made with host-installed tools.
---

# Pulp mise toolchain

Use this skill whenever a task depends on build-tool versions. The canonical
input is `tools/toolchain/manifest.toml`; `.mise.toolchain.toml` and
`mise.toolchain.lock` are generated outputs. Never hand-edit generated files.

## Reproducible build path

From the repository root, verify the checkout and install only the locked tools:

```bash
mise --version
mise trust
MISE_ENV=toolchain mise install --locked
MISE_ENV=toolchain mise exec -- python3 tools/toolchain/check_env.py
MISE_ENV=toolchain mise exec -- pulp build
```

Use `MISE_ENV=toolchain mise exec -- <command>` for one-off commands so the
versioned PATH is explicit in logs. On Windows, use the same commands from
PowerShell (replace `python3` with `python` only when the pinned interpreter's
launcher requires it). Do not use `mise install` without `--locked` for a Pulp
receipt.

A pinned-toolchain claim requires all of the following:

- the mise version named by `[mise].version` in the manifest;
- a successful `mise install --locked` on the guest's platform;
- `tools/toolchain/check_env.py` with no unexpected drift; and
- the manifest source and lock checksums recorded with the build receipt.

If any item is absent, label the result host-toolchain evidence. Do not infer
pinned versions from a generated config that was not actually used.

## Checking a host or VM rollout step

A fleet host, an ssh session or a launchd job has neither the pinned mise's
directory nor the rustup proxies (`~/.cargo/bin`) on its PATH, so a plain
`check_env.py` there reports an installed Rust as missing. Rollout steps run
the one command that judges them:

```bash
MISE_ENV=toolchain mise exec -- python3 tools/toolchain/check_env.py --fleet-path --require-tools
```

`--fleet-path` also searches those directories; `--require-tools` exits 1 on
any host-tool difference whatever the manifest's mode, while only reporting the
npm trees each checkout installs for itself (`pulp deps resolve`).
`tools/toolchain/fleet_report.py` runs the check over ssh on several hosts and
prints one drift table; it installs nothing.

Read the versions a build actually used from its CMakeCache and configure log
(`CMAKE_COMMAND`, `CMAKE_MAKE_PROGRAM`, `Found Python3`), not from the profile.
On macOS, CMake's FindPython prefers a Python.framework install over PATH; the
configure of a Pulp checkout sets `Python3_FIND_FRAMEWORK=LAST` when
`MISE_ENV=toolchain` is set so the pinned interpreter wins. A build directory
configured before opting in keeps its cached interpreter: configure a fresh one.

## Bringing a host up to the pins

Decisions contract rows 24-28 (`.agents/contract.toml`) are the settled rules
below; read them before changing any of it.

**Procedure.** Run `python3 tools/toolchain/host_sync.py [--repo <checkout>]`
by hand or from an interactive or agent session, then read
`~/.config/pulp/toolchain-sync.json`: `exit`, `findings` (what is still
different and its fix), `network_failures` (steps a download broke: just run
it again later) and `report_only` (things it must not change). It is
check_env's fix commands executed: fetch, read the pins from `origin/main`
(never the working tree), `mise trust` of the checkout's own `.mise.toml`,
`mise install --locked` with the pinned mise, `rustup toolchain install` of the
manifest Rust with its components and targets, then `check_env.py --fleet-path`
inside the pinned environment. Exit 0 match, 1 a difference it does not fix,
2 network (retry; never counted as fixed), 3 pins unreadable, 4 another run
holds the lock. `fleet_report.py` (hourly) is the read-only drift alarm.

- Never schedule it as a plain LaunchAgent. A launchd job that opens a
  `/Volumes` path without the tartci launcher's Removable Volumes grant hangs
  there (an m5s orphan hung two hours); tartci will schedule it under its
  consented launcher. Every child it and check_env start is bounded and
  abandoned on timeout; keep it that way (no unbounded `wait`).
- It is additive only. Do not teach it to uninstall, `mise use`, set the rustup
  default, touch shell profiles, Homebrew or Xcode: those are report-only lines
  in the receipt for a human or tartci to act on.
- A mise binary whose sha256 is not the pin is never executed, not even for
  `install`; the binary is tartci's to install. Do not "fix" that by running
  whatever `mise` is on PATH.
- Its verify step needs the ref's `experimental/pulp-rs/rust-toolchain.toml` in
  the copied pins: without it `rustc --version` answers for the default
  toolchain and a host with the pin installed beside a newer `stable` reads as
  drift forever.
- `rustc --version` cannot see a missing clippy or cross std. check_env asks
  `rustup component list` / `target list --toolchain <pin>` (or the toolchain a
  job selected with `RUSTUP_TOOLCHAIN`), and that is the finding that keeps a
  half-installed toolchain from exiting 0.
- `rustup toolchain link 1.97.1 <sysroot>` is refused ("invalid custom
  toolchain name"): an official release name cannot be aliased. CI selects an
  identical `stable` by name instead (`tools/ci/provide_pinned_rust.sh`,
  branch `stable-alias`). That script fails closed on self-hosted legs and on
  pull requests that change `experimental/pulp-rs`; only hosted legs may fall
  back, with a warning, a summary line and a receipt (contract row 29).
- Tests run it as a subprocess against a real local git origin and shell fakes
  for mise/rustup/rustc with HOME in the fixture
  (`tools/toolchain/test_host_sync.py`); never let a test reach the real
  `~/.config/pulp`.

## Cache placement on volume hosts

Agent-only caches (mise data, rustup, cargo, the Skia keyed cache) on m3
(`/Volumes/Workshop`) and m5s (`/Volumes/Atelier`) live under
`/Volumes/<vol>/caches/<name>`, symlinked from the default path. To move one:
copy it there with nothing using it, verify (file count, size, a few digests),
move the default aside and symlink it, prove the tool reads it (`check_env.py
--fleet-path` and `host_sync.py` show no `cache` finding; the tool runs), and
only then delete the old copy.

Do NOT: link a cache into a tmp or `mktemp` directory, under
`/Volumes/<vol>/VMs`, or inside any reclaim root (the Aug-29 mise link into a
`mktemp` dir under the VM cache tree dangled); move a VM-mounted cache (ccache
`cache_root`, `fetchcontent-src`) or `TARTCI_HOME`/logs with a symlink, which
only tartci's profile placement may move (a dangling `--dir` mount takes a lane
down); delete the old copy before the proof. `check_env.py` reports a cache
symlink that dangles, points into a temporary directory, leaves
`/Volumes/<vol>/caches`, or does not answer (a lost volume grant).

## Fresh worktrees and mise trust

mise refuses an untrusted `.mise.toml`, so the mise `python3` shim fails in a
fresh worktree. `.githooks/post-checkout` trusts exactly the new worktree's
own `.mise.toml` when git creates it; `host_sync.py` trusts the checkout it
runs in. Check with `mise settings get locked` (must print `true` inside the
worktree); repair with `mise trust <worktree>/.mise.toml`. Never trust a
parent directory, `$HOME` or `/`, and never add `trusted_config_paths` roots:
they are prefixes, so a root trusts every repository under it, and a
`.mise.toml` can run code.

## The pinned-toolchain golden (CI images)

`.shipyard/vm-image.mise.toml` bakes a candidate image,
`pulp-build-runner-mise-candidate`, on top of the runner golden with
`tools/ci/mise-golden-layer.sh` (run by `tart-provision.sh manifest`). Its
`[mise]` table is generated from the manifest; regenerate it with
`generate.py`, never by hand. The layer refuses a host or guest mise binary
whose sha256 or `--version` differs from the pin, and toolchain files that
differ from the checkout's commit, so bake from a clean `main`. It installs
with `mise install --locked`, turns rustup self-update off and installs the
manifest's Rust by name, then writes a receipt the guest itself reports
(on the host: `~/.cache/pulp-ci/golden-receipts/<vm>-mise.json`).
The candidate changes no lane: promoting it to `:latest` or routing a lane to
it is a separate, reviewed step.

## Platform exceptions in the lock

Some tools are not locked on every platform, by declaration in the manifest's
`unlocked` table: Tart is macOS-only, and conda-forge publishes no
windows-arm64 clang-format, so windows-arm64 keeps its own clang-format. The
drift test refuses any other missing platform row; never "fix" an exception by
hand-editing the lock.

## Scope and boundaries

- Keep `MISE_ENV=toolchain` opt-in at shell entry. It prevents a checkout from
  rewriting a developer's PATH or installing tools unexpectedly.
- Use the locked profile for Pulp builds, cross-platform VM probes, release
  investigations, and toolchain changes. CI and `setup.sh` may still run on
  hosts without mise while their reviewed provisioning migration is incomplete.
- Never put credentials, VM disk paths, mutable cache directories, or secrets in
  `.mise.toml`. Put those in the VM recipe or host configuration.
- Do not use mise to install or redistribute Skia, Dawn, V8, SDKs, or proprietary
  vendor tools. Those have their own checksum and licensing paths.
- Build through `pulp build`/`pulp dev` or the governed CI wrapper; mise controls
  tool versions, not build parallelism or runner admission.

## Updating pins

To change a tool, edit the manifest on a branch and regenerate with the exact
mise binary named by the manifest:

```bash
python3 tools/toolchain/update.py cmake --to 4.3.4 --mise /path/to/mise
python3 tools/toolchain/generate.py --check
```

Run the toolchain self-tests and dependency audit before publishing. A pin
change must update the manifest, generated mise files, lock checksum, and
`DEPENDENCIES.md`/licensing documentation when the dependency inventory changes.
Do not claim a pin change is complete until the generated-file check is clean.

Moving mise itself (`[mise]` version, `source_commit`, `[mise.binary]`) is a
source build of the release tag, not an `update.py` run. Before claiming "no
relock", run `generate.py --lock --mise <new binary>` in a throwaway clone:
it deletes and rewrites the lock, so a fresh lock mtime plus a clean
`git status` proves the new mise reproduces it byte for byte. `--check` alone
never runs mise and cannot show that. On the fleet, the tartci profile's
`[mise]` table refuses the host install when its binary is missing, so the new
binary goes onto a host before that host's table changes, and the previous
binary stays installed as the rollback. A source build at a new path is a new
program to LuLu on every host; flipping the pin needs no network (the tools are
already installed), so pre-seed rather than asking for firewall rules.

A mise source build is not bit-reproducible, so never rebuild on each host to
"verify" the pin. Two builds of 2026.10.17 with identical flags on the same Mac
differed in about 100 KB: the aqua registry that mise's `build.rs` embeds
(serialized from structs that hold hash maps, such as `replacements`) comes
out in a different order each build. The `[mise.binary]` digest names one
build. Hosts get that exact file, copied from the build host and checked by
sha256; a differing rebuild is expected and is not a replacement.

The canonical copy of the 2026.10.17 macos-arm64 pin is
`m3:/Volumes/Workshop/ci/mise-pins/2026.10.17/macos-arm64/mise`, next to its
`SHA256SUMS` and a `README.txt`; m1's `~/.local/opt/mise-2026.10.17/bin/mise`
is a mirror with the same digest. A new pin gets its own
`/Volumes/Workshop/ci/mise-pins/<version>/<platform>/` directory. Install it on
a host by digest, never by rebuilding:

```bash
d=$HOME/.local/opt/mise-2026.10.17; mkdir -p "$d/bin" "$d/lib"
scp m3:/Volumes/Workshop/ci/mise-pins/2026.10.17/macos-arm64/mise "$d/bin/mise.partial"
(cd "$d/bin" && echo "284869e5b5f57211377f026309910bea91ef65b3a71b934295fa8bd712436dfc  mise.partial" | shasum -a 256 -c) \
  && chmod 755 "$d/bin/mise.partial" && mv "$d/bin/mise.partial" "$d/bin/mise"
cp "$HOME/.local/opt/mise-2026.10.11/lib/mise-self-update-instructions.toml" "$d/lib/"  # or 2026.10.8's
MISE_DISABLE_UPDATE_WARNING=1 "$d/bin/mise" --version   # 2026.10.17 macos-arm64
```

Leave the previous version's directory installed: it is the rollback.

A relock can differ from the committed lock with no mise change at all: conda
resolves clang-format's runtime dependencies (for example `libcxx`) to the
newest build the channel publishes. Before blaming the new mise for a lock
diff, relock the same manifest with the currently pinned binary in a second
copy. If both write identical bytes, the diff is upstream drift: keep the
committed lock in the mise move, prove the new binary installs from it with a
cold `mise install --locked` in throwaway mise directories, and move the drift
in its own change.

Since mise 2026.10.13 a project's `[settings]` apply only once the checkout is
trusted. In an untrusted checkout `locked`, `auto_install` and
`disable_update_warning` from `.mise.toml` read as mise's defaults (`false`,
`true`, `false`), so a script that runs mise in a checkout must set
`MISE_TRUSTED_CONFIG_PATHS` (as `generate.py --lock`, the golden layer and the
candidate CI lane do) or pass the setting as an environment variable. Lanes
also export `MISE_LOCKED=1` and `MISE_AUTO_INSTALL=0`, which hold whatever
mise's trust rules do; keep them when adding a lane that runs mise. A fleet
checkout is trusted one file at a time (the post-checkout hook, host_sync, or
`mise trust <checkout>/.mise.toml`), never through `trusted_config_paths`
roots, and a rollout step checks that `mise settings get locked` prints `true`
inside a checkout. Since
2026.10.14 a global `~/.config/mise/config.override.toml` (or
`config.toolchain.override.toml`) outranks the project's tools and is exempt
from `--locked`; when a host reports a tool drift that the lock cannot
explain, look for one. `mise config explain` lists the keys a config file
sets and flags unknown ones.

New mise releases surface in the weekly "Dependency drift: weekly audit"
issue (`deps-drift-check.yml`): `tools/deps/manifest.json` tracks mise as a
`git-tag` upstream, and a test refuses it unless its version and tag match
`[mise].version`. A newer release there starts the evidence path above; it
never moves a host by itself.

jdx/mise also tags its sub-crates (`mise-sigstore-v0.1.0` beside
`v2026.10.12`), so the mise row declares `upstream.tag_pattern`. The audit
ignores tags outside it, and when the releases API names one, it falls back to
tag order and marks the row "tag-order guess". If mise changes its release tag
shape, update the pattern with the pin; the toolchain self-test requires the
pinned tag to match it.

## Windows VM recipe

For an ephemeral ARM64 or x64 Windows guest, copy only the locked config and
use a guest-local mise cache. Keep the guest isolated from the persistent
interactive VM, and destroy the overlay after the receipt is copied out. A
successful configure or SDK build is useful evidence, but it is not plugin GUI
or DAW proof; retain that distinction in the status report.
