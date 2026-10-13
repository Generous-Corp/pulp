# Pinned Build Tools with mise

[mise](https://github.com/jdx/mise) is a command-line tool that installs exact
versions of developer tools (CMake, Ninja, Python, Node.js and others) per
project, without touching the versions installed system-wide. Pulp uses it to
describe the build tools that build Pulp itself, in one reviewed place.

This page covers what Pulp pins today, how to use the pinned tools when you
build Pulp from source, what it means for plugin developers who use the SDK,
and what is planned but not built yet.

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

The pinned tools are **opt in**. They live in an environment-scoped mise
config, `.mise.toolchain.toml`, which mise loads only when `MISE_ENV=toolchain`
is set. If you already activate mise in your shell, entering a Pulp checkout
changes nothing until you opt in.

Nothing in Pulp's build requires mise today: `setup.sh`, `pulp build`, and the
CI, Shipyard and release lanes do not run it. You can keep using your own
CMake, Python and Node.js. Using the pinned set is how you get the same tool
versions the toolchain manifest describes.

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

The `toolchain-test_generate-selftest` ctest checks that the committed files
equal what the manifest generates (`python3 tools/toolchain/generate.py
--check` runs the same comparison), that the lock hashes to the manifest's
`lock_sha256`, that every locked URL comes from the tool's own release
location, and that every tool has a checksummed entry, or a stated reason for
not having one, on every platform. Never edit the generated files by hand.

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

- **A Pulp-built mise binary.** mise 2026.10.8 built from source and published
  with checksums, installed by digest, with self-update disabled, so the mise
  binary itself is pinned.
- **An environment check.** A small script that compares the active `python3`,
  `cmake`, `ninja`, `node`, Rust and `clang-format` against the manifest and
  prints one fix command per mismatch. `pulp build`, `pulp dev`, `pulp loop`,
  `setup.sh` and the generators of committed files would run it first: warn
  only at the start, and later refuse for generators of committed files.
- **An explicit opt-out** for contributors who manage their own tools, printed
  loudly on every run and refused on CI hosts.
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
