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

## Windows VM recipe

For an ephemeral ARM64 or x64 Windows guest, copy only the locked config and
use a guest-local mise cache. Keep the guest isolated from the persistent
interactive VM, and destroy the overlay after the receipt is copied out. A
successful configure or SDK build is useful evidence, but it is not plugin GUI
or DAW proof; retain that distinction in the status report.
