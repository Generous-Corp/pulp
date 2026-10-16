# Pinned Build Tools with mise

[mise](https://github.com/jdx/mise) is a command-line tool that installs exact
versions of developer tools (CMake, Ninja, Python, Node.js and others) per
project, without touching the versions installed system-wide. Pulp uses it to
describe the build tools that build Pulp itself, in one reviewed place.

This page covers what Pulp pins today, how to use the pinned tools when you
build Pulp from source, what it means for plugin developers who use the SDK,
and what is planned but not built yet.

## Pulp's adoption policy

Pulp treats `tools/toolchain/manifest.toml` as the canonical description of
its build-tool versions. Use the locked mise profile for reproducible local
builds, Windows VM probes, release investigations, and any change that edits
the toolchain manifest or generated lock files:

```bash
MISE_ENV=toolchain mise install --locked
MISE_ENV=toolchain mise exec -- pulp build
```

This is a consistency requirement for toolchain and release evidence: record
the mise version, platform, manifest revision, and lock checksum with the
receipt. A build is not evidence of the pinned toolchain merely because mise
is installed; run the command through `mise exec` (or activate the same
environment) and run `tools/toolchain/check_env.py`.

Ordinary source builds and public CI remain compatible with hosts that do not
have mise yet. The CI images and bootstrap path are being migrated in a
separate, reviewed step; they must not silently download tools or change a
working host's PATH. Until that migration lands, a non-mise build must be
reported as host-toolchain evidence, with the versions from
`tools/toolchain/check_env.py`, rather than as a pinned-toolchain build.

For Windows VM work, keep the image disposable and use the same locked profile
for ARM64 and x64 guests. Do not put credentials, VM-specific paths, or
mutable cache locations in `.mise.toml`; keep those in the VM recipe or host
configuration.

## Why Pulp uses mise

A build machine usually gets its tools from whatever it happens to have:
Homebrew on one Mac, apt on a Linux box, a version manager such as nvm on
another. Two machines that read the same source can then build it with
different CMake, Python or Node.js versions, and a result from one machine does
not reproduce on another. The same kind of drift appears between a developer
laptop and the CI virtual machines.

Pulp's answer is a single source of truth for build-tool versions:

- `tools/toolchain/manifest.toml` names one exact version for every build tool.
  Floating selectors such as `stable`, `latest`, `homebrew` or a bare `3.12`
  are refused by the generator.
- `tools/toolchain/generate.py` turns that manifest into the files mise reads,
  plus the tool fields of the CI virtual-machine image definitions
  (`.shipyard/vm-image.toml`, `.shipyard/vm-image.intel.toml`).
- `mise.toolchain.lock` records, for each tool and each platform, the exact
  download URL and its sha256 checksum. `mise install --locked` installs only
  what the lock names, so a changed or substituted upstream download is
  refused instead of silently installed.

Prebuilt libraries (Skia, Dawn, V8) are not mise tools. They keep their own
pinned, checksummed fetch path in `tools/deps/manifest.json`.

## What the lock pins

| Tool | Version | Installed by | Locked platforms |
|------|---------|--------------|------------------|
| CMake | 4.3.3 | mise (`aqua:Kitware/CMake`) | linux-arm64, linux-x64, macos-arm64, macos-x64, windows-arm64, windows-x64 |
| Ninja | 1.13.2 | mise (`aqua:ninja-build/ninja`) | all six |
| Python | 3.14.4 | mise (`core:python`) | all six |
| Node.js | 26.10.0 | mise (`core:node`) | all six |
| clang-format | 21.1.8 | mise (`conda:clang-format`) | all except windows-arm64 (conda-forge publishes no build for it) |
| Tart | 2.38.0 | mise (`aqua:openai/tart`) | macOS only (Tart runs only on macOS hosts) |

Two tools are pinned in the manifest but are **not** installed by mise:

- **Rust** (`rust = "1.97.1"` in the manifest, with `rustfmt` and `clippy`).
  rustup installs Rust and cargo reads `rust-toolchain.toml`. mise could only
  record a version string for Rust, not a checksum, so Rust is deliberately
  left out of the lock. Note that `experimental/pulp-rs/rust-toolchain.toml`
  still selects `channel = "stable"`; the manifest's 1.97.1 records what the CI
  gate image actually runs, and generating `rust-toolchain.toml` from the
  manifest is planned (see below).
- **Xcode** (`xcode = "26.5"`, build `17F42`). Xcode is installed into the
  macOS CI image by the image provisioning scripts, not by mise.

The manifest also records, per tool, whether a version is already what the CI
gate runs (`observed`) or a version the gate still has to reach (`target`).
That evidence lives in the manifest's `[evidence]` table.

## Building Pulp from source

The pinned tools live in an environment-scoped mise config,
`.mise.toolchain.toml`, which mise loads only when `MISE_ENV=toolchain` is set.
Entering a Pulp checkout never changes an existing shell until you opt in.
For reproducibility-sensitive work, opting in is the project policy; the
remaining host-toolchain path is a migration boundary, not a second set of
Pulp pins. `setup.sh`, `pulp build`, and public CI still accept hosts without
mise until their provisioning is migrated and verified.

### 1. Install mise 2026.10.8

The lock is generated with mise **2026.10.8**, and only that version is
guaranteed to read it the same way: mise releases differ in how they resolve
some download URLs, so an older mise can read the same config differently or
reject rows of the lock.

Install it from the release archive and verify it before use. mise publishes a
`SHASUMS256.txt` for each release, signed with
[minisign](https://jedisct1.github.io/minisign/). Its public key is in the
[mise installation docs](https://mise.jdx.dev/installing-mise.html) and in the
repository's [`minisign.pub`](https://github.com/jdx/mise/blob/main/minisign.pub).
You need `minisign` installed (for example `brew install minisign` or your
Linux package manager). On macOS (on Linux use `linux-x64` or `linux-arm64`, and
`sha256sum -c` in place of `shasum -a 256 -c`):

```bash
mise_version=2026.10.8
mise_platform=macos-arm64   # or macos-x64
base="https://github.com/jdx/mise/releases/download/v${mise_version}"
curl -fL -O "$base/SHASUMS256.txt" -O "$base/SHASUMS256.txt.minisig" \
     -O "$base/mise-v${mise_version}-${mise_platform}.tar.gz"

# 1. The checksum file is signed by the mise release key.
minisign -Vm SHASUMS256.txt -P RWTC3g8W3z4RZK3V3qv7fa1QY4JEWyBtqIHW+85QlJpZc5yG+uNYNBSZ
# 2. The archive matches the signed checksum file.
grep " ./mise-v${mise_version}-${mise_platform}.tar.gz$" SHASUMS256.txt | shasum -a 256 -c
# 3. Only then install it.
tar -xzf "mise-v${mise_version}-${mise_platform}.tar.gz"
mkdir -p ~/.local/bin && install -m 755 mise/bin/mise ~/.local/bin/mise
mise --version
```

Stop if either check fails. As a cross-check, the 2026.10.8 archives have
these sha256 digests:

| Archive | sha256 |
|---------|--------|
| `mise-v2026.10.8-macos-arm64.tar.gz` | `8228699f316dc17931beb86f7a1aac70e4685535430441492ff4e857c72535ad` |
| `mise-v2026.10.8-macos-x64.tar.gz` | `28c924c5703c6344489bc73227c59da2194eaf7fd962c5f3d233b9524b7419e9` |
| `mise-v2026.10.8-linux-arm64.tar.gz` | `88aea77e41c2a3c45a9cbd64df9516ba887017442846213b3de178acbd3c9a46` |
| `mise-v2026.10.8-linux-x64.tar.gz` | `e908ea27810ae358c9a9a7d897ea10197bdc38d39839243e1b36acc31cbef868` |
| `mise-v2026.10.8-windows-x64.zip` | `2548132f9e8b8dc29027933e47a98582557b448dfec791dbecad66e9c760582b` |
| `mise-v2026.10.8-windows-arm64.zip` | `c38a2353088896894df4a2e9c4ea4a9d1b2bee290c0dba9ea5a10e43646d41e1` |

On Windows, compare `(Get-FileHash <archive> -Algorithm SHA256).Hash` with the
table (PowerShell prints it in upper case).

The convenience installer `curl https://mise.run | MISE_VERSION=v2026.10.8 sh`
checks the archive's sha256, against digests embedded in the script when you
ask for the script's own current release and against the release's
`SHASUMS256.txt` otherwise. It does not check the minisign or GPG signature
(the script marks that as a to-do), and the script itself, including those
embedded digests, is served unpinned from `mise.run`. Prefer the steps above.

### 2. Trust the checkout and install the locked tools

From the root of your Pulp checkout:

```bash
mise trust
MISE_ENV=toolchain mise install --locked
```

`--locked` makes mise refuse any tool or platform that the lock does not cover,
rather than resolving a new download. Pulp's `.mise.toml` also sets
`locked = true` and `auto_install = false`, so mise never installs a tool
behind your back.

### 3. Use the tools

Either run single commands through mise:

```bash
MISE_ENV=toolchain mise exec -- cmake --version
```

or export `MISE_ENV=toolchain` and
[activate mise](https://mise.jdx.dev/getting-started.html) in your shell, so
the pinned `cmake`, `ninja`, `python3`, `node` and `clang-format` come first on
your `PATH` inside the checkout.

### Project tasks

`.mise.toml` also defines tasks you can run with `mise run <task>`:

| Task | What it does |
|------|--------------|
| `npm-deps` | Installs the project's npm packages with `npm ci` from the committed lockfiles in `tools/import-design/jsx-runtime` and `tools/scripts` |
| `visual-deps` | Installs the hash-pinned Python packages for the visual-analysis tests |
| `visual-check` | Checks that the configured Python can run the visual-analysis tests |
| `visual-deps-windows`, `visual-check-windows` | The same two tasks using the Windows `python` command |

### How the pins change

A tool version changes only through a pull request that:

1. edits `tools/toolchain/manifest.toml` (the version and its `[evidence]`
   entry), and
2. regenerates the derived files with the pinned mise:

   ```bash
   python3 tools/toolchain/generate.py --lock --mise /path/to/mise-2026.10.8
   ```

   This rewrites `.mise.toolchain.toml`, the generated block in `.mise.toml`,
   the VM image fields, and `mise.toolchain.lock`, and records the lock's
   sha256 in the manifest. It refuses to run with any mise version other than
   the manifest's `[mise].version`, and runs mise with throwaway data, cache and
   config directories so your own mise setup cannot leak into the lock.

`tools/toolchain/update.py` does both steps for one tool and prints a table of
every version and lock checksum that moved, for the pull request body:

```bash
python3 tools/toolchain/update.py cmake --to 4.3.4 --mise /path/to/mise-2026.10.8
python3 tools/toolchain/update.py rust --to 1.98.0      # no lock rows: rustup installs Rust
python3 tools/toolchain/update.py shipyard --to v0.270.0
```

`python3 tools/toolchain/deps.py` groups the three operations under one
command: `check` (the environment report), `resolve` (fetch what a build needs
once, before building: `setup.sh --deps-only` for the pinned dependency sources,
then `npm ci` for any lockfile directory whose `node_modules` differs, then the
report; it never installs a host tool), and `update` (the above, finding the
installed pinned mise itself).

`update` also marks the tool's `[evidence]` row as a `target` (the gate has not been
seen running it yet). It never commits, pushes or installs. If any step fails
it restores every file it touched. Moving mise itself is refused: that takes a
reviewed source build of the new release.

The `toolchain-test_generate-selftest` ctest checks that the committed files
equal what the manifest generates (`python3 tools/toolchain/generate.py
--check` runs the same comparison), that the lock hashes to the manifest's
`lock_sha256`, that every locked URL comes from the tool's own release
location, and that every tool has a checksummed entry, or a stated reason for
not having one, on every platform. Never edit the generated files by hand.

## Checking your environment

`tools/toolchain/check_env.py` compares the tools on your `PATH` with the
manifest and prints one line per difference: the tool, the pinned version, the
version it found, where it found it, and the command that fixes it.

```bash
python3 tools/toolchain/check_env.py
```

```text
toolchain-check: cmake: expected 4.3.3 (tools/toolchain/manifest.toml), found 4.4.3 at /opt/homebrew/bin/cmake; fix: MISE_ENV=toolchain mise install --locked && eval "$(MISE_ENV=toolchain mise activate bash)"
toolchain-check: 1 difference(s) from the manifest; continuing (warn mode)
```

It checks `python3`, `cmake`, `ninja`, `node`, `rustc` (asked from
`experimental/pulp-rs`, where cargo builds), `clang-format`, the `mise` binary
(version, and its sha256 against `[mise.binary]` in the manifest), and that each
npm lockfile directory's `node_modules` is exactly its `package-lock.json`. It
only reads; it never installs anything.

It runs on any Python 3.9 or newer with no packages, because it is what checks
the interpreter. It reads `tools/toolchain/expected.json`, which `generate.py`
writes from the manifest, rather than the TOML manifest itself.

Exit status: `0` when everything matches or the manifest is in warn mode (it
is today), `1` for a difference once the manifest is in enforce mode, and `3`
when it cannot read the expected versions. `--format github` prints GitHub
Actions `::warning::` annotations and `--format json` a machine-readable
report.

`python3 tools/toolchain/fleet_report.py --host m1 --host m3=local --host m5 --host m5s`
runs the same check on each host over ssh, under the non-login PATH lanes and
agents get, and prints one table of the version every host runs against the
pin. It copies the check into a temporary directory, removes it afterwards, and
installs nothing; a host it cannot reach is shown as `unreachable`.

For a host rollout step (a fleet host or a CI image moving to the pinned
toolchain), run the check the way the step judges it:

```bash
MISE_ENV=toolchain mise exec -- python3 tools/toolchain/check_env.py --fleet-path --require-tools
```

`--fleet-path` also looks in the pinned mise's directory and the rustup proxies
(`~/.cargo/bin`), which a non-login ssh or launchd PATH lacks; without it an
installed Rust is reported missing. `--require-tools` exits 1 on any host-tool
difference whatever the manifest's mode, while still only reporting the npm
trees each checkout installs for itself (`pulp deps resolve`).

The pinned mise never asks whether a newer release exists:
`.mise.toml` sets `disable_update_warning = true`, and every version probe the
toolchain scripts run sets `MISE_DISABLE_UPDATE_WARNING=1`, since `mise
--version` is the one command that would otherwise query GitHub. Automatic
self-update is off by default.

A platform the manifest lists as an exception (Linux and Windows still take
CMake from the system package manager) is still reported, labelled as the
declared exception, so the difference stays visible.

`PULP_TOOLCHAIN_UNPINNED=1` is the opt-out for contributors who manage their
own tools: once enforcement is on, it lets the check pass with a banner naming
every difference, on every run. It is refused on a fleet host, one with
`~/.config/pulp/fleet-host` or `PULP_FLEET_HOST=1`.

It also runs on its own, once, before a build:

- every configure of the Pulp checkout runs it (`tools/cmake/PulpToolchainCheck.cmake`),
  which covers `pulp build`, `pulp dev`, `pulp loop` and a raw `cmake -S . -B build`
  alike, and prints its lines as configure status. It does not run when Pulp is a
  subproject of another build, so SDK and plugin projects never see it;
- `setup.sh`, including the `setup.sh --deps-only` that `pulp build` runs on a cold
  checkout, runs it before fetching any dependency;
- every CI build leg (macOS, Linux, Windows) runs it as its own step before
  configuring and shows the differences as warnings on the run (the configure and
  `setup.sh --ci` skip it there, so it is reported once).

In warn mode none of these fails. Once the manifest is in enforce mode a
difference stops the configure and `setup.sh` with the same lines.

## Building plugins with the Pulp SDK

If you build plugins with the installed SDK (`pulp create`, `pulp build` in a
project with a `pulp.toml`), mise is **optional** and nothing in the SDK
workflow reads it. The SDK ships compiled libraries, so you need the
prerequisites listed in [Getting Started](getting-started.md#prerequisites),
not Pulp's own build toolchain. You are free to use mise in your own project to
pin your own tools.

## Opt-in `pulp tool` installs are separate

Tools installed with `pulp tool install` (for example the Audio Quality Lab)
are **not** governed by the toolchain manifest or the mise lock. They keep
their own contract, described in
[Extending Pulp](../reference/extending-pulp.md#every-added-tool-must-be-user-updatable-and-overridable):
each has a registry `pinned_version`, `pulp tool update <id> [--version <v>]`
re-installs it, and you can override its version with
`PULP_TOOL_<ID>_VERSION` or `$PULP_HOME/tool-overrides.json`. They install
under `~/.pulp/tools/`, and mise neither installs anything there nor reads
those overrides.

## Planned

The following is designed but **not built yet**. None of it runs today; this
section is here so the direction is visible, not as instructions.

- **A published mise binary.** The macOS arm64 build the fleet runs is pinned
  by digest today; a reproducible build from source, published with checksums
  for every platform, is still to come.
- **The check before generators of committed files.** The generators that
  write committed files run the environment check first, and refuse outside the
  pinned environment.
- **Rust from the manifest.** Generate `rust-toolchain.toml` from the manifest
  so cargo uses the pinned Rust instead of the current `stable`.
- **CI images built from the lock.** The macOS CI image installs its tools with
  `mise install --locked` instead of unpinned Homebrew packages, and an image is
  only promoted when its recorded tool versions match the manifest.
- **Enforcement on the required CI gate**, after a warn-only period.
- **A `toolchain.mise` setting for SDK projects.** The SDK installer would
  default to setting up mise for a new plugin project, with a fully supported
  way to opt out. SDK projects would never be subject to Pulp's toolchain
  enforcement; their pin stays `sdk_version` in `pulp.toml`.

## See also

- [Getting Started](getting-started.md#building-from-source)
- [Building Pulp](build.md)
- [Licensing & Acknowledgements](../reference/licensing.md)
- [mise documentation](https://mise.jdx.dev/)
