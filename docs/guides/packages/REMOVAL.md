# Package Manager Removal Guide

This document lists every file, line change, CI workflow, skill, doc reference, and external resource that would need to be removed or reverted to completely excise the package manager feature from the Pulp codebase.

The package manager was designed to be self-contained and removable. No core subsystem depends on it. No example project requires it. No format adapter references it. Removing it is a mechanical process.

Work through each section as a checklist.

---

## 1. Files to Delete

### CLI source files

- [ ] `tools/cli/package_commands.hpp`
- [ ] `tools/cli/package_commands.cpp`
- [ ] `tools/cli/package_registry.hpp`
- [ ] `tools/cli/package_registry.cpp`

### Registry and tooling

- [ ] `tools/packages/registry.json`
- [ ] `tools/packages/registry-schema.json`
- [ ] `tools/packages/packages.lock.schema.json`
- [ ] `tools/packages/validate_registry.py`
- [ ] `tools/packages/freshness_check.py`

### Test stubs (entire directory tree)

- [ ] `tools/packages/test-stubs/cycfi-q/CMakeLists.txt`
- [ ] `tools/packages/test-stubs/cycfi-q/main.cpp`
- [ ] `tools/packages/test-stubs/daisysp/CMakeLists.txt`
- [ ] `tools/packages/test-stubs/daisysp/main.cpp`
- [ ] `tools/packages/test-stubs/dr-libs/CMakeLists.txt`
- [ ] `tools/packages/test-stubs/dr-libs/main.cpp`
- [ ] `tools/packages/test-stubs/fontaudio/CMakeLists.txt`
- [ ] `tools/packages/test-stubs/fontaudio/main.cpp`
- [ ] `tools/packages/test-stubs/libsamplerate/CMakeLists.txt`
- [ ] `tools/packages/test-stubs/libsamplerate/main.cpp`
- [ ] `tools/packages/test-stubs/pffft/CMakeLists.txt`
- [ ] `tools/packages/test-stubs/pffft/main.cpp`
- [ ] `tools/packages/test-stubs/r8brain-free-src/CMakeLists.txt`
- [ ] `tools/packages/test-stubs/r8brain-free-src/main.cpp`
- [ ] `tools/packages/test-stubs/rtneural/CMakeLists.txt`
- [ ] `tools/packages/test-stubs/rtneural/main.cpp`
- [ ] `tools/packages/test-stubs/signalsmith-dsp/CMakeLists.txt`
- [ ] `tools/packages/test-stubs/signalsmith-dsp/main.cpp`
- [ ] `tools/packages/test-stubs/signalsmith-stretch/CMakeLists.txt`
- [ ] `tools/packages/test-stubs/signalsmith-stretch/main.cpp`

Or simply: `rm -rf tools/packages/`

### Documentation guides

- [ ] `docs/guides/packages/index.md`
- [ ] `docs/guides/packages/cycfi-q.md`
- [ ] `docs/guides/packages/daisysp.md`
- [ ] `docs/guides/packages/dr-libs.md`
- [ ] `docs/guides/packages/fontaudio.md`
- [ ] `docs/guides/packages/libsamplerate.md`
- [ ] `docs/guides/packages/pffft.md`
- [ ] `docs/guides/packages/r8brain-free-src.md`
- [ ] `docs/guides/packages/rtneural.md`
- [ ] `docs/guides/packages/signalsmith-dsp.md`
- [ ] `docs/guides/packages/signalsmith-stretch.md`
- [ ] `docs/guides/packages/README.md` (this feature's README)
- [ ] `docs/guides/packages/REMOVAL.md` (this file)

Or simply: `rm -rf docs/guides/packages/`

### Agent skill

- [ ] `.agents/skills/packages/SKILL.md`

Or simply: `rm -rf .agents/skills/packages/`

### Planning documents (in the `planning/` submodule)

- [ ] `planning/package-manager-status.md`
- [ ] `planning/ralph-prompt-package-manager.md`
- [ ] `planning/research/package-manager-proposal.md`

### Generated project files (per-project, not in repo)

These are created by `pulp add` in user projects. Not in the repo itself, but listed for completeness:

- [ ] `cmake/pulp-packages.cmake` (generated CMake declarations)
- [ ] `packages.lock.json` (lock file in project root)
- [ ] Any `pulp.toml` `[targets]` section entries added by `pulp target`

---

## 2. Lines to Revert in Shared Files

### `tools/cli/pulp_cli.cpp`

**Includes (lines ~31-32):** Remove these two lines:

```cpp
#include "package_commands.hpp"
#include "package_registry.hpp"
```

**Command dispatch (lines ~4730-4736):** Remove these seven lines from the main dispatch:

```cpp
if (command == "add")      return pulp::cli::pkg::cmd_add(args);
if (command == "remove")   return pulp::cli::pkg::cmd_remove(args);
if (command == "list")     return pulp::cli::pkg::cmd_list(args);
if (command == "search")   return pulp::cli::pkg::cmd_search(args);
if (command == "update")   return pulp::cli::pkg::cmd_update(args);
if (command == "suggest")  return pulp::cli::pkg::cmd_suggest(args);
if (command == "target")   return pulp::cli::pkg::cmd_target(args);
```

**Audit command extensions (lines ~4752-4771):** Remove the package-manager-specific audit flag handling block. This is the section that checks for `--packages`, `--platforms`, and `--licenses` flags and calls `pulp::cli::pkg::audit_*` functions. Leave the existing Python audit fallthrough intact.

```cpp
// Check for package-manager-specific flags
bool pkg_flag = false, plat_flag = false, lic_flag = false;
for (auto& a : args) {
    if (a == "--packages") pkg_flag = true;
    if (a == "--platforms") plat_flag = true;
    if (a == "--licenses") lic_flag = true;
}
if (pkg_flag || plat_flag || lic_flag) {
    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }
    int rc = 0;
    if (pkg_flag) rc |= pulp::cli::pkg::audit_packages(root);
    if (plat_flag) rc |= pulp::cli::pkg::audit_platforms(root);
    if (lic_flag) rc |= pulp::cli::pkg::audit_licenses(root);
    return rc;
}
```

**Doctor checks (lines ~2330-2380):** Remove the "Package health checks" section that checks `packages.lock.json` and `registry.json`, including:
- The "Package lock file" DoctorCheck block (~lines 2331-2358)
- The "Package platform alignment" DoctorCheck block (~lines 2360-2380)

### `tools/cli/CMakeLists.txt`

**Source list (line ~3):** Remove `package_registry.cpp` and `package_commands.cpp` from the source file list:

```
package_registry.cpp package_commands.cpp
```

---

## 3. CLI Docs Entries to Remove

### `docs/reference/cli.md`

Remove these sections entirely (lines ~685-783):

- [ ] `### add` section (lines ~685-698)
- [ ] `### remove` section (lines ~700-710)
- [ ] `### list` section (lines ~712-721)
- [ ] `### search` section (lines ~723-733)
- [ ] `### update` section (lines ~737-744)
- [ ] `### suggest` section (lines ~747-757)
- [ ] `### target` section (lines ~759-770)
- [ ] `### audit (package extensions)` section (lines ~772-783)

### `docs/status/cli-commands.yaml`

Remove these command entries:

- [ ] `add` entry (lines ~388-405) -- the package add command, not to be confused with `add-component`
- [ ] `remove` entry (lines ~407-415)
- [ ] `list` entry (lines ~417-424)
- [ ] `search` entry (lines ~426-437) -- the package search command, not `docs search`
- [ ] `update` entry (lines ~439-446)
- [ ] `suggest` entry (lines ~448-464)
- [ ] `target` entry (lines ~466-478)

---

## 4. CI Workflows to Delete

- [ ] `.github/workflows/freshness-check.yml` -- weekly package freshness check against upstream GitHub repos

---

## 5. Skills to Remove

- [ ] `.agents/skills/packages/SKILL.md` -- the Claude/Codex package discovery and recommendation skill

If this is the only file in the directory, remove the entire `.agents/skills/packages/` directory.

Also check and update:

- [ ] `CLAUDE.md` -- if the skills table was updated to include `packages`, remove that row. (As of this writing, the `packages` skill is not listed in the CLAUDE.md skills table on this branch.)

---

## 6. External Repos to Archive

- [ ] `pulp-packages` GitHub repo (referenced in `package_registry.hpp` as the remote registry URL: `https://raw.githubusercontent.com/danielraffel/pulp-packages/main/registry.json`). Archive or delete this repo if it was created.

---

## 7. Branch Cleanup

- [ ] Delete `develop/package-manager` branch (local and remote)
- [ ] Delete any `feature/pkg-*` branches (e.g., `feature/pkg-prerequisites`, `feature/pkg-registry`, `feature/pkg-cli`, `feature/pkg-community`)
- [ ] Remove any worktrees associated with these branches

---

## 8. Verification After Removal

After completing all the above steps:

1. [ ] `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` -- configure succeeds
2. [ ] `cmake --build build -j$(sysctl -n hw.ncpu)` -- build succeeds with no missing includes or undefined symbols
3. [ ] `ctest --test-dir build --output-on-failure` -- all tests pass
4. [ ] `./build/tools/cli/pulp doctor` -- no package-related checks appear
5. [ ] `./build/tools/cli/pulp help` -- no `add`, `remove`, `list`, `search`, `update`, `suggest`, `target` commands shown
6. [ ] `python3 tools/deps/audit.py --strict` -- no references to package registry files
7. [ ] `grep -r "package_commands\|package_registry\|pulp::cli::pkg" tools/cli/` -- no results

---

## What is NOT Affected by Removal

The following are **not** part of the package manager and should be left intact:

- `ship package` (`pulp ship package`) -- creates `.pkg` installers for plugin distribution. This is part of the shipping subsystem, completely unrelated.
- `core/format/registry.hpp` / `core/format/src/registry.cpp` -- plugin format registry (VST3/AU/CLAP). Unrelated.
- `core/audio/include/pulp/audio/format_registry.hpp` / `core/audio/src/format_registry.cpp` -- audio format codec registry. Unrelated.
- `core/platform/include/pulp/platform/win/registry.hpp` / `core/platform/src/registry.cpp` -- Windows registry access. Unrelated.
- `tools/audio/include/pulp/tools/audio/model_registry.hpp` / `tools/audio/src/model_registry.cpp` -- ML model registry for audio tools. Unrelated.
- `tools/components/registry.yaml` -- UI component registry. Unrelated.
- `apple/Package.swift` -- Swift Package Manager manifest. Unrelated.
- `DEPENDENCIES.md` and `NOTICE.md` -- these may have entries added by `pulp add`, but the files themselves predate the package manager. Only remove entries that were added by the package manager, not the files.
