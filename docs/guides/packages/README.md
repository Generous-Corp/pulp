# Pulp Package Manager

The Pulp package manager makes it easy to add vetted, license-compatible third-party audio libraries to a Pulp project. It handles discovery, fetching, CMake wiring, license checking, and platform compatibility -- so you can focus on building your plugin.

**Status:** This is an evolving feature on the `develop/package-manager` branch. It is functional but still being refined. See the [Phase Roadmap](#phase-roadmap) at the bottom of this document.

**Isolation guarantee:** The package manager is fully self-contained and removable. No core subsystem depends on it. No example project requires it. No format adapter references it. If you want to remove it entirely, see [REMOVAL.md](REMOVAL.md).

---

## What It Is

Pulp's built-in `core/signal/` subsystem ships 30+ DSP processors (filters, delay, reverb, compressor, oscillators, FFT, and more). The package manager extends a project with capabilities beyond what ships natively -- pitch detection, time-stretching, sample rate conversion, neural network inference, physical modeling, and audio file decoding.

It is not a general-purpose package manager. It is a curated registry of audio-focused libraries that have been evaluated for license compatibility (MIT/BSD/Apache only), platform support, and integration quality with Pulp's CMake build system.

### Design Principles

1. **Pulp is complete without packages.** Every example works without `pulp add`. The getting-started guide never mentions it. `core/signal/` is the primary DSP offering.
2. **Fully optional, never imposed.** Developers who prefer manual CMake wiring can still use `FetchContent_Declare(...)` directly. The package manager does not own `external/` or prevent manual additions.
3. **No API abstraction layer.** The package manager handles plumbing (discovery, fetch, license check, CMake wiring). It does not unify library APIs behind abstract interfaces.
4. **Curation over breadth.** 10-100 tested, license-compatible audio packages is better than thousands of untested general-purpose ones.

---

## Architecture Overview

### Registry

The package registry lives at `tools/packages/registry.json`. It is a JSON file (v2 format) validated by `tools/packages/registry-schema.json`. Each package entry includes:

- **Metadata**: name, version, description, license, category, URL
- **Fetch info**: git repository, tag, method (FetchContent, header-only, or vendored)
- **CMake info**: target names, header-only flag, include directory
- **Platform support**: per-platform architecture lists with optional notes
- **Audio-specific fields**: `rt_safe` flag, `provides` capabilities, `overlaps_with_builtin` mapping, `unique_value` description, `alternatives`, `migration_notes`
- **Verification**: last verified date, build status per platform-arch pair

The registry currently contains 10 packages across 4 categories:

| Category | Packages |
|----------|----------|
| DSP | signalsmith-stretch, signalsmith-dsp, cycfi-q, pffft, daisysp |
| Audio I/O | dr-libs, libsamplerate, r8brain-free-src |
| ML | rtneural |
| UI | fontaudio |

### Lock File

When you `pulp add` a package, the resolved version, git URL, integrity hash, and commit are recorded in `packages.lock.json` at the project root. The lock file schema is defined in `tools/packages/packages.lock.schema.json`.

### CMake Generation

`pulp add` generates `cmake/pulp-packages.cmake` in the project root. This file contains `FetchContent_Declare` / `FetchContent_MakeAvailable` calls for each added package. Your project's `CMakeLists.txt` includes this file, and packages are linked via `target_link_libraries`.

For header-only packages, the generated CMake uses `target_include_directories` after fetching.

### CLI Commands

| Command | What It Does |
|---------|-------------|
| `pulp add <package>` | Add a package: license check, platform check, CMake wiring, metadata updates |
| `pulp remove <package>` | Remove a package: clean lock file, CMake, and metadata |
| `pulp list` | Show installed packages (human-readable or `--json`) |
| `pulp search <query>` | Search registry by name, tags, description, or category |
| `pulp update` | Check for and apply package updates |
| `pulp suggest` | Context-aware package recommendations |
| `pulp target` | Manage project platform targets in `pulp.toml` |
| `pulp audit --packages` | Verify lock file integrity |
| `pulp audit --platforms` | Check package/platform coverage |
| `pulp audit --licenses` | Verify license compatibility |

---

## Quick Start

### Add a package

```bash
pulp add signalsmith-stretch
```

This will:
1. Check the license is compatible with Pulp's MIT license (reject GPL/LGPL/copyleft)
2. Check platform support against your project's declared targets
3. Warn if the package overlaps with Pulp built-in processors
4. Generate or update `cmake/pulp-packages.cmake` with FetchContent declarations
5. Update `packages.lock.json`, `DEPENDENCIES.md`, and `NOTICE.md`
6. Print usage instructions (which target to link and which headers to include)

### Search for packages

```bash
pulp search "pitch detection"
pulp search dsp
pulp search fft --format json
```

### Get recommendations

```bash
pulp suggest --description "I need pitch shifting"
pulp suggest --analyze src/my_processor.cpp
pulp suggest --alternative pffft
```

### List installed packages

```bash
pulp list
pulp list --json
```

### Remove a package

```bash
pulp remove signalsmith-stretch
```

### Check for updates

```bash
pulp update            # dry-run: show available updates
pulp update --apply    # apply updates and regenerate CMake
```

### Manage platform targets

```bash
pulp target list                  # show current targets
pulp target add Windows-arm64     # add a target
pulp target remove Linux-x64     # remove a target
```

Default targets (if none configured): `macOS-arm64`, `Windows-x64`, `Linux-x64`.

---

## What Files It Creates in a Project

When you use the package manager, these files are created or modified in your project:

| File | Created By | Purpose |
|------|-----------|---------|
| `cmake/pulp-packages.cmake` | `pulp add` | FetchContent declarations for all added packages |
| `packages.lock.json` | `pulp add` | Exact resolved versions and integrity hashes |
| `pulp.toml` (targets section) | `pulp target` | Platform targets for compatibility checking |
| `DEPENDENCIES.md` entries | `pulp add` | New rows in the dependency table |
| `NOTICE.md` entries | `pulp add` | Attribution entries for added packages |

All of these are cleaned up by `pulp remove` when the last package is removed.

---

## Integration Points

### CLI Integration

The package manager adds seven top-level commands (`add`, `remove`, `list`, `search`, `update`, `suggest`, `target`) and three audit flags (`--packages`, `--platforms`, `--licenses`) to the `pulp` CLI. These are dispatched from `tools/cli/pulp_cli.cpp` and implemented in `tools/cli/package_commands.cpp` and `tools/cli/package_registry.cpp`.

### Doctor Integration

`pulp doctor` includes two package-related health checks:

- **Package lock file**: checks if `packages.lock.json` exists and is consistent with the registry
- **Package platform alignment**: checks that all installed packages support all project targets

These checks pass silently when no packages are installed.

### Claude Code Plugin / Agent Skill

The `.agents/skills/packages/SKILL.md` skill teaches Claude Code and Codex agents how to use the package manager. It covers:

- Package discovery and search
- Category awareness (what each category provides)
- Overlap detection (checking built-ins before suggesting packages)
- License policy enforcement
- The `pulp add` / `pulp search` / `pulp suggest` commands

### CI Integration

`.github/workflows/freshness-check.yml` runs weekly (Monday 06:00 UTC) to check package versions against upstream GitHub repos. It detects:

- Newer versions available
- Archived repositories
- License changes

A Tier 2 mode (manual trigger) runs full build validation with CMake stub projects for each package across macOS, Ubuntu, and Windows.

---

## How the Remote Registry Works

The package registry supports both local and remote modes:

- **Local registry**: `tools/packages/registry.json` in the Pulp repo. This is the source of truth for the curated package list.
- **Remote registry**: Hosted at `https://raw.githubusercontent.com/danielraffel/pulp-packages/main/registry.json` (the `pulp-packages` public repo). `pulp search` can query this remote registry, with results cached locally with a configurable TTL (default: 24 hours).
- **Cache management**: Remote registry data is cached in the default cache directory. Use `pulp search --refresh` to force a cache refresh.

The remote registry follows the same v2 schema as the local registry. Package submissions to the remote registry go through PR review with automated validation (license, schema, build).

---

## How License Checking Works

Every `pulp add` checks the package's SPDX license identifier against a policy:

| Verdict | Licenses | Behavior |
|---------|----------|----------|
| **Allowed** | MIT, MIT-0, BSD-2-Clause, BSD-3-Clause, Apache-2.0, ISC, zlib, BSL-1.0, Unlicense, CC0-1.0 | Added without prompts |
| **Review required** | MPL-2.0, other unlisted | Warning printed, `--license-override` flag required |
| **Rejected** | GPL-2.0, GPL-3.0, LGPL-2.1, LGPL-3.0, AGPL-3.0, SSPL-1.0 (all variants) | Blocked. Cannot add. |

The `--license-override commercial` flag can bypass review-required licenses for projects with commercial license agreements, but cannot bypass rejected licenses.

License checking is also available standalone via `pulp audit --licenses` and the `tools/packages/validate_registry.py` script.

---

## Isolation Guarantees

The package manager is designed for clean removal. Here is what it touches and what it does not:

### What it touches

- `tools/cli/pulp_cli.cpp` -- seven command dispatch lines, two include lines, doctor check block, audit flag block
- `tools/cli/CMakeLists.txt` -- two source files in the build list
- `tools/cli/package_commands.{hpp,cpp}` -- command implementations (own files)
- `tools/cli/package_registry.{hpp,cpp}` -- registry data structures (own files)
- `tools/packages/` -- entire directory is package-manager-specific
- `docs/guides/packages/` -- entire directory is package-manager-specific
- `docs/reference/cli.md` -- eight CLI reference sections
- `docs/status/cli-commands.yaml` -- seven command entries
- `.agents/skills/packages/` -- agent skill directory
- `.github/workflows/freshness-check.yml` -- CI workflow

### What it does NOT touch

- No `core/` subsystem files
- No `examples/` files
- No format adapters (VST3, AU, CLAP)
- No `external/` vendored dependencies
- No build system root `CMakeLists.txt`
- No test harness files
- No shipping/signing code
- No other CLI commands (build, test, validate, doctor logic beyond the two added checks, ship, etc.)

For a complete removal checklist, see [REMOVAL.md](REMOVAL.md).

---

## Per-Package Guides

Each package has a detailed integration guide in this directory:

| Package | Guide |
|---------|-------|
| Signalsmith Stretch | [signalsmith-stretch.md](signalsmith-stretch.md) |
| Signalsmith DSP | [signalsmith-dsp.md](signalsmith-dsp.md) |
| Q (Cycfi) | [cycfi-q.md](cycfi-q.md) |
| PFFFT | [pffft.md](pffft.md) |
| DaisySP | [daisysp.md](daisysp.md) |
| dr_libs | [dr-libs.md](dr-libs.md) |
| libsamplerate | [libsamplerate.md](libsamplerate.md) |
| r8brain-free-src | [r8brain-free-src.md](r8brain-free-src.md) |
| RTNeural | [rtneural.md](rtneural.md) |
| fontaudio | [fontaudio.md](fontaudio.md) |

See also [index.md](index.md) for a categorized overview.

---

## Phase Roadmap

The package manager is being built in phases. Each phase is self-contained and lands via PR to `develop/package-manager`, then merges to `main` at phase boundaries.

### Phase 1: Integration Guides + Registry

Registry format design, initial 10-package registry, per-package integration guides, JSON schema validation, Tier 1 freshness CI workflow, manual build verification across macOS/Windows/Linux.

**Status:** Complete on `develop/package-manager`.

### Phase 2: CLI + Claude Plugin Integration

`pulp add/remove/list/search/update/suggest/target` commands, license checking, platform compatibility analysis, overlap detection, CMake generation, lock file management, `pulp doctor` checks, `pulp audit` extensions, Claude Code skill, Tier 2 build validation CI.

**Status:** Complete on `develop/package-manager`.

### Phase 3: Community Registry + Web Discovery

`pulp-packages` public repo, community submission workflow, remote registry search with local caching, semver constraint resolution, static site generator for package browsing, quality scores, verification badges.

**Status:** Partially implemented on `develop/package-manager` (remote search, semver, quality scores are in; web UI and community submission workflow are not yet started).

### Phase 4: Extended Ecosystem

LAION CLAP, UV/Python integration, yt-dlp/pydub, and other extended ecosystem packages.

**Status:** Deferred. Revisit after Phases 1-3 prove the concept and there is user demand.

### Phase 5: Future

Dependency resolution (transitive dependencies), version conflict detection, workspace-level package management, package authoring tools.

**Status:** Not planned. These would only be needed if the package count grows significantly.
