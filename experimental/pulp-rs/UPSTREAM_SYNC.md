# Upstream drift tracking — pulp-rs port

**Purpose:** keep this Rust port in sync with the Pulp C++ CLI as new features land on `main`. Each time a phase completes, we bump `last_synced_sha` to the `origin/main` tip at that moment, so the next phase can diff against it and catch features that need porting.

## Current anchor

```
last_synced_sha = 7fbd7db1f45974796e3349562e0c86251fffc4fe
last_synced_date = 2026-04-24
last_synced_phase = Phase 6f / pulp#740 — upgrade + project-bump safety-rail port (Slices A+B+C+D shipped via PR #742; Slice E here)
```

## Watched files

These are the C++ sources whose behavior the Rust port mirrors. Any commit on `origin/main` that modifies one of these should be reviewed for "does the Rust port need to absorb this?"

```
tools/cli/version_diag.cpp
tools/cli/version_diag.hpp
tools/cli/projects_registry.cpp
tools/cli/projects_registry.hpp
tools/cli/cmd_doctor.cpp
tools/cli/cmd_projects.cpp       # added in Phase 4, extended in Phase 6 (add/remove/prune)
tools/cli/cmd_version.cpp        # added in Phase 5
tools/cli/cmd_config.cpp         # added in Phase 5
tools/cli/cmd_upgrade.cpp        # added in Phase 5
tools/cli/update_check.cpp       # added in Phase 5
tools/cli/update_check.hpp       # added in Phase 5
tools/cli/update_mode.cpp        # added in Phase 5 (banner snooze/clear helpers)
tools/cli/update_mode.hpp        # added in Phase 5
tools/cli/cmd_pr.cpp             # added in Phase 6 (happy-path shipyard delegation)
tools/cli/cmd_sdk.cpp            # added in Phase 6 (status/clean ported; install stubbed)
tools/cli/cmd_build.cpp          # added in Phase 6 (configure+build; --watch deferred)
tools/cli/cmd_run.cpp            # added in Phase 6 (binary search + exec)
tools/cli/cmd_misc.cpp           # added in Phase 6 (test, clean, status, cache)
tools/cli/cmd_project.cpp        # added in Phase 6b (bump/undo; migration-note render stubbed)
tools/cli/project_bump.cpp       # added in Phase 6b (pure-logic pin finder, rewrite, undo-batch JSON)
tools/cli/project_bump.hpp       # added in Phase 6b (header)
tools/cli/cmd_host.cpp           # added in Phase 6b (scan lives here alongside host; scan-only watched)
tools/cli/pulp_cli.cpp           # added in Phase 6b (usage banner + fuzzy suggester + bare-invocation); Phase 6c adds the inline if-ladder dispatch for add/remove/list/search/update/suggest/target + handle_audit
tools/cli/package_commands.cpp   # added in Phase 6c — 8 command entrypoints + audit_* helpers
tools/cli/package_commands.hpp   # added in Phase 6c
tools/cli/package_registry.hpp   # added in Phase 6c — registry struct + license verdict + on-disk format
tools/cli/cli_common.cpp         # partial — pulp_home / read_sdk_version / read_user_config_value
tools/cli/cli_common.hpp         # partial
tools/cli/cmd_dev.cpp            # added in Phase 6d (build-once + test/run stub; watch loop deferred)
tools/cli/cmd_create.cpp         # added in Phase 6d (CI-mode scaffold: templates + CMake injection; interactive path rejects)
tools/cli/cmd_docs.cpp           # added in Phase 6d (index/search/open/show ported Rust-native; build-site/build-api/check = Spawner delegates)
tools/cli/cmd_design.cpp         # added in Phase 6d (binding resolution + one-shot launch; watch stubbed)
tools/cli/design_binding.cpp     # added in Phase 6d (helper used by cmd_design)
tools/cli/tool_registry.cpp      # added in Phase 6d (list/path/uninstall/run/doctor ported; install stubbed — archive download + extraction deferred)
tools/cli/tool_registry.hpp      # added in Phase 6d
```

As more commands are ported, add their C++ sources here.

## Classification matrix (2026-04-24 audit)

Run the post-implementation audit checklist (below) after every phase. The table below is the snapshot as of commit `362b1fdb` on `explore/rust-cli-prototype`.

| C++ entrypoint | Classification | Notes |
|---|---|---|
| `test` | **Ported** | Auto-build + `ctest --output-on-failure` + passthrough |
| `clean` | **Ported** | `build/` deletion |
| `projects` | **Ported** | `list` + `add` + `remove` + Rust-extra `prune` + `--json` |
| `help` (bare) | **Ported** | Phase 6b — top-level `pulp help` prints usage; bare-invocation (`pulp-rs` with no args) also prints the banner + exits 0 |
| `--help` / `-h` | **Ported** | Clap auto-generated |
| `project` (singular) | **Ported** | Phase 6b base + pulp#740 Slices A/B/C/D (PR #742). All flags now ported: positional, `--to`, `--all`, `--dry-run`, `--force-dirty`, `--allow-downgrade`, **`--allow-cli-skew`** (Slice A, refuse newer-than-CLI), **`--allow-redundant`** (Slice B, fail-open `origin/main` probe), `--verify-builds`. Standalone-mode bump (Slice C): `pulp.toml sdk_version` is source of truth, `find_package(Pulp X.Y.Z)` mirrored, `project(VERSION)` preserved as product version. Source-checkout refusal (Slice A) blocks running inside the Pulp framework tree. Undo records explicit `UndoEdit` list (Slice D) with backward-compat read of legacy single-edit batches. Migration-note rendering still stubbed with pointer to C++ binary (`cmd_project.cpp` links `migration_runtime.cpp`). **Slice C2 deferred:** managed-cache `sdk_path` rewrite needs the standalone-SDK resolution helpers from `cli_common.cpp` to land first. |
| `build` | **Ported-partial** | Missing `--watch`, `--test-filter=...`, real `--validate`; simpler configure logic |
| `run` | **Ported** | Phase 6e — macOS `.app` bundle fallback added; matches `cmd_run.cpp` line-for-line. |
| `status` | **Ported** | Phase 6e — `GitProbe` trait drives `git -C <root>` branch + commit, standalone SDK detail (version, path-hint readiness, local + download caches, checkout hint), source-tree walk (`.cpp`/`.mm` + `.hpp`/`.h` counts, `test/*.cpp` count, `examples/*` dir count), plugin-format availability (VST3/AU/CLAP/AAX) with `PULP_AAX_SDK_DIR` + host-OS gate. |
| `doctor` | **Ported-partial** | Only `--versions --json`; default doctor + `android` + `ios` + `--fix` + `--ci` + `--dry-run` + `--scan-parents` missing. Phase 7 wrapper will exec pulp-cpp for these. |
| `cache` | **Ported-partial** | `status` + `clean` real; `fetch` stubbed — Phase 7 wrapper delegates `fetch` to pulp-cpp. |
| `sdk` | **Ported-partial** | `status` + `clean` real; `install` stubbed — Phase 7 wrapper delegates to pulp-cpp (curl + tar + `setup.sh --local`). |
| `upgrade` | **Ported-partial** | Check/notes ported; install / positional version / default-action diff — Phase 7 wrapper covers. |
| `version` | **Ported** | Phase 6e — `bump <major|minor|patch> [--plugin]` rewrites `CMakeLists.txt` / `pulp_add_plugin(...)` cell + inserts CHANGELOG heading; `check [--with-bump-check]` runs the full 6-step walk (SDK↔CLI, AU Info.plist guard, CHANGELOG heading match, plugin.json semver shape, plugin.json↔marketplace.json drift, marketplace.json `plugins[0].version`) and shells to `version_bump_check.py --mode=report` when requested. |
| `pr` | **Ported-partial** | Shipyard delegation real; `--native` fallback missing; Phase 7 wrapper delegates `pr --native` to pulp-cpp. |
| `config` | **Ported** | Phase 6e — bare / `help` / `--help` / `-h` print usage + exit 0; `update.mode` changes clear `$PULP_HOME/update-snooze` best-effort (Slice 5 #550 parity). |
| `scan` | **Ported-partial** | Phase 6b — file-enumeration stub: walks `~/Library/Audio/Plug-Ins/{CLAP,VST3,Components}` + LV2 dirs, prints `[FMT] N plugin(s)` in the same shape as `cmd_host.cpp cmd_scan`. Plug-in metadata (vendor, version, unique-ID) omitted — the C++ path reads them via `pulp::host::PluginScanner::scan()`, which requires dlopen+factory inspection. Rust stub uses file basename as the "name" column |
| Fuzzy "Did you mean…?" | **Ported** | Phase 6b — Levenshtein suggester in `src/help.rs`; matches C++ threshold of `dist <= 3`. Emits `Unknown command: <typed>\nDid you mean: pulp-rs <closest>?` for close matches, else falls back to `Run `pulp-rs help` for usage` |
| `list` | **Ported** | Phase 6c — 3-column table + `--json` array. Lock-file enrichment pulls license + category from registry. |
| `remove` | **Ported** | Phase 6c — lock edit + CMake regen + `DEPENDENCIES.md` / `NOTICE.md` strip. Matches C++ exit-1 on unknown package. |
| `update` | **Ported** | Phase 6c — dry-run diff by default; `--apply` writes lock + regenerates `cmake/pulp-packages.cmake`. |
| `suggest` | **Ported** | Phase 6c — `--description` / `--analyze` / `--alternative` / `--format json`. `--analyze` parses `#include` lines and matches tags. |
| `target` | **Ported** | Phase 6c — `list` / `add` / `remove` with platform-arch whitelist parity + pulp.toml splice writer. |
| `add` | **Ported-partial** | Phase 6c — license gate (`Allowed` / `ReviewRequired` / `Rejected` + restricted-vs-rejected tier), platform guard, CMake gen, metadata MD updates. Missing: the C++ path's `--lane <module>` flag (not in scope for 6c). |
| `search` | **Ported-partial** | Phase 6c — local-registry fuzzy search + JSON. `--refresh` recognised but **no-op** (the C++ path shells out to `curl`; Rust stub prints "no packages found" when refresh is requested on an empty registry). |
| `audit` | **Ported-partial** | Phase 6c — `--packages` / `--platforms` / `--licenses` run Rust-native checks over lock + registry + targets. No-flag invocation delegates to `tools/audit.py` via `Spawner`. Exit codes OR'd matching C++ `handle_audit`. |
| `create` | **Ported-partial** | Phase 6d — full `--ci` path: arg parsing, name derivation, random VST3 UID, template-file expansion, format-gated entry-file skip, standalone `main.cpp`, `pulp.toml` emission, `examples/CMakeLists.txt` injection, Android tree copy. Skipped: doctor pre-flight, `ensure_sdk()`, post-scaffold build+test, registry add. Interactive mode rejects with pointer to C++ binary. |
| `design` | **Ported-partial** | Phase 6d — flag parse (incl. positional `.js/.mjs/.cjs`), binding resolution (simplified vs C++: cache-root disagreement probe omitted), configure+build+one-shot exec via `Spawner`. `--watch` stubbed. |
| `docs` | **Ported-partial** | Phase 6d — `index`, `search` (literal + fuzzy fallback), `open`, `show support/command/cmake/style` ported Rust-native over the hand-rolled YAML walker (matches C++ byte-for-byte). `check`, `build-site`, `build-api` delegate via `Spawner` (mirrors C++ delegation to `mkdocs` / `bash`). |
| `dev` | **Ported-partial** | Phase 6d — full flag parse (`--test`, `--test-filter=`, `--validate`, `--run`, `--design`, `--target`, `-- tail`). Configure+build pass via `orchestrate::build_with`, then optional `ctest`, then one-shot launch of `--run TARGET`. **Watch loop stubbed** (would need `notify` crate + debounced rebuild; deferred to a future slice). |
| `validate` | **Stays in C++** | Uses `pulp::view::render_to_file` + `pulp::format` directly |
| `ship` | **Stays in C++** | Uses `pulp::ship::*` APIs directly |
| `audio` | **Stays in C++** | Uses `pulp::tools::audio::*` directly |
| `host` | **Stays in C++** | Uses `pulp::host::{PluginScanner, PluginSlot}` directly |
| `tool` | **Ported-partial** | Phase 6d — `list`, `path`, `uninstall`, `run`, `doctor` ported Rust-native over new `src/tool_registry.rs` reader (serde-driven; preserves BTreeMap ordering). `install` stubbed — archive download + extraction (tar/zip/xz + xattr cleanup + chmod) would add ~500 LOC of deps; users fall back to C++ `pulp tool install` until that lands. |
| `ci-local` / `add-component` | **Already-delegate** | Python-script shims — unchanged |
| `design-debug` / `inspect` / `import-design` / `export-tokens` | **Already-delegate** | Built-binary shims — unchanged |
| `install` (legacy) | **Already-delegate** | Alias for `cache fetch skia` |

**Revised completion (post-Phase-6e):** ~92% feature-complete (15 Ported + 15 Ported-partial at their core paths) against ~30 distinct user-visible commands, plus two cross-cutting UX parity fixes (bare invocation + fuzzy suggester). Phase 6e closed the cheap-fill gaps: `version` (bump + check), `status` (git probe + SDK detail + format counts), `run` (macOS `.app` fallback), `config` (bare-invocation help + snooze clear). Phase 7 introduces a `pulp-cpp` fallthrough wrapper so remaining Ported-partial branches (`cache fetch`, `sdk install`, `upgrade install`, `pr --native`, most of `doctor`, watch loops in `dev`/`design`, `tool install`, `create` wizard, deep `scan` metadata) route to the C++ binary transparently — no user-visible regressions on the Phase 8 swap day.

## Deferred list (needs porting in future phases)

Explicitly classified as deferred with scope reasons — NOT swept under the rug. Phase 6d cleared the last four function-surface items (`dev`, `create`, `docs`, `design`) and the orphan `tool` namespace. What remains is subsystem-scoped polish, not command-level gaps.

Remaining polish items (all tracked inside their classification-matrix rows above):

- `scan` deep metadata — needs `pulp::host::PluginScanner` dlopen path.
- `dev` / `design` watch loop — needs `notify` crate integration + debounced rebuild.
- `tool install` — needs archive download (ureq, already present) + extraction (tar/zip/xz crates) + platform chmod / xattr fixups.
- `create` interactive wizard — keeps the C++ binary as the prompt driver.
- `docs build-site/build-api/check` — delegates intentionally stay as `Spawner` calls to `mkdocs` / `bash`.

Phase 8 (the actual swap) can proceed because every top-level C++ command now has a Rust-side entrypoint. The stubbed branches all exit with a clear "use the C++ binary for this" message, so users land on a documented affordance rather than an "unknown subcommand" error.

## How to check for drift

From the explore branch worktree:

```bash
# list commits on main that touched watched files since last sync
cd /path/to/pulp-main-checkout
git fetch origin main
git log --oneline <last_synced_sha>..origin/main -- \
    tools/cli/version_diag.cpp \
    tools/cli/version_diag.hpp \
    tools/cli/projects_registry.cpp \
    tools/cli/projects_registry.hpp \
    tools/cli/cmd_doctor.cpp \
    tools/cli/cmd_projects.cpp \
    tools/cli/cmd_version.cpp \
    tools/cli/cmd_config.cpp \
    tools/cli/cmd_upgrade.cpp \
    tools/cli/update_check.cpp \
    tools/cli/update_check.hpp \
    tools/cli/update_mode.cpp \
    tools/cli/update_mode.hpp \
    tools/cli/cmd_pr.cpp \
    tools/cli/cmd_sdk.cpp \
    tools/cli/cmd_build.cpp \
    tools/cli/cmd_run.cpp \
    tools/cli/cmd_misc.cpp \
    tools/cli/cmd_project.cpp \
    tools/cli/project_bump.cpp \
    tools/cli/project_bump.hpp \
    tools/cli/cmd_host.cpp \
    tools/cli/pulp_cli.cpp \
    tools/cli/package_commands.cpp \
    tools/cli/package_commands.hpp \
    tools/cli/package_registry.hpp \
    tools/cli/cli_common.cpp \
    tools/cli/cli_common.hpp \
    tools/cli/cmd_dev.cpp \
    tools/cli/cmd_create.cpp \
    tools/cli/cmd_docs.cpp \
    tools/cli/cmd_design.cpp \
    tools/cli/design_binding.cpp \
    tools/cli/tool_registry.cpp \
    tools/cli/tool_registry.hpp
```

If the output is non-empty, inspect each commit:
- **Behavior change** (new field in doctor JSON, new findings rule, new projects format) → port into Rust + update fixtures.
- **Refactor / naming** → probably ignore; Rust port doesn't need to track C++ internals.
- **Bug fix** → port if the same bug exists in Rust; often we'll have avoided it via different semantics.

After porting everything that needs porting, bump `last_synced_sha` at the top of this file to the new `origin/main` tip.

## Post-implementation audit checklist

**Run this after EVERY phase.** Manual "which commands did we port" audits are error-prone — on 2026-04-24 I missed 10 commands (the entire package-manager subsystem + scan + project + tool + help) because I only enumerated `cmd_*.cpp` files without checking `pulp_cli.cpp`'s inline `if (command == ...)` ladder. The checklist below is the fix.

### A. Rebuild the C++ inventory from `pulp_cli.cpp`

1. Open `tools/cli/pulp_cli.cpp`. Enumerate, in order:
   - Every `commands[]` entry (main dispatch table)
   - Every `script_commands[]` entry (Python-script delegates)
   - Every `binary_commands[]` entry (built-binary delegates)
   - Every inline `if (command == "...")` ladder entry (package-manager + tool + audit + aliases)
   - Every legacy alias (`add-component`, `install`, etc.)
   - Help tokens (`help`, `--help`, `-h`)
2. Record the total count.

### B. Rebuild the Rust inventory from `src/main.rs`

3. Open `experimental/pulp-rs/src/main.rs`.
4. Enumerate every `enum Command` variant.
5. Record global clap behavior:
   - No-arg invocation (exit code + output)
   - `help` subcommand handling
   - `--help` / `-h` handling
   - Unknown-command message + exit code

### C. Diff command-by-command

6. For every overlapping top-level command, open the C++ implementation file (`cmd_<name>.cpp` or the package-manager/tool namespace) AND the Rust equivalent in `src/cmd/<name>.rs`.
7. Compare:
   - Subcommand names
   - Accepted flags (short + long, required + optional)
   - Positional arguments
   - Default behavior when invoked with no subcommand
   - Explicitly-stubbed / rejected branches
   - Meaningful side effects: cache writes, mode changes, marker files, env-var clears

### D. Classify every missing item honestly

8. For each C++ command not present in Rust, ask in order:
   - Is it explicitly listed in this file's "Deferred" section? → **Deferred** (OK)
   - Is it policy-bound to C++ (linked to `pulp::host/view/ship/tool-audio`)? → **Stays in C++** (OK)
   - Is it already a delegate/wrapper in C++ (e.g. `inspect` delegates to the inspect binary)? → **Already-delegate** (OK)
   - Is it a legacy alias for another command? → **Alias** (OK)
   - Otherwise → **Missing (undocumented gap)** (NOT OK — either port it or add to the deferred list with a scope reason in this file)

### E. Check cross-cutting UX parity

9. Verify:
   - Bare invocation: `pulp` vs `pulp-rs` with no args — exit code + output shape should match
   - `help` subcommand + `--help` + `-h`
   - Unknown-command message (C++ has fuzzy "Did you mean..." logic)
   - Alias behavior
   - Dispatch-order collisions (e.g. a package-manager command shadowed by a `commands[]` entry)

### F. Refresh proof artifacts

10. Add/update parity fixtures for every newly ported subcommand path.
11. Refresh JSON/human-lane snapshot tests via `cargo insta review`.
12. Update this file:
    - `last_synced_sha`, `last_synced_date`, `last_synced_phase`
    - Watched files list (add every C++ file the phase touched)
    - Deferred list (any new items, with scope rationale)

### G. Publish the refreshed classification matrix

13. Output a table with columns: `C++ entrypoint`, `C++ surface`, `Rust reality`, `classification`, `audit note`.
14. Compare totals against the previous phase's audit — highlight any shift in category counts.
15. Link the table from the migration plan doc (`planning/rust-cli-migration-plan.md`) so the plan state stays honest.

### Tooling

Use RepoPrompt's `context_builder` with a `response_type=question` prompt that selects `pulp_cli.cpp` + all `cmd_*.cpp` + `package_commands.cpp` + `tool_registry.cpp` + the Rust `src/main.rs` + `src/cmd/*.rs`, and ask for the classification. Manual enumeration is error-prone on a surface this wide — let the tool read every entrypoint simultaneously.

Example query:

> Enumerate every dispatch path in `tools/cli/pulp_cli.cpp` (commands[], script_commands[], binary_commands[], inline if-ladder, aliases, help tokens). For each, check `src/main.rs`'s `enum Command` and per-command `src/cmd/<name>.rs` to classify as `Ported / Ported-partial / Deferred / Stays in C++ / Already-delegate / Missing (undocumented gap)`. Produce a table.

## How to regenerate parity fixtures

The `expected.json` files under `tests/fixtures/*/` were captured from a specific C++ binary build. When the C++ CLI's JSON output changes legitimately, re-capture:

```bash
cd /path/to/pulp-main-checkout
cmake --build build --target pulp-cli
./build/tools/cli/pulp doctor --versions --json > /path/to/pulp-rs-proto/experimental/pulp-rs/tests/fixtures/<fixture>/expected.json
```

Then run `cargo test --test parity_test` to confirm the Rust port still matches the new expected.

## Sync log

Each phase that re-syncs against upstream gets an entry below.

| Date | Phase | last_synced_sha | Commits absorbed | Notes |
|---|---|---|---|---|
| 2026-04-24 | Phase 2 initial | be3fe863... | N/A — first port | doctor --versions --json port; 5 fixtures captured |
| 2026-04-24 | Phase 4 | f794a16f... | *(fill in via `git log --oneline be3fe863..f794a16f -- watched-files`)* | projects list port; plan absorb any post-Phase-2 version_diag tweaks |
| 2026-04-23 | Phase 5 | 2a8269c1... | none — `git log f794a16f..2a8269c1 -- tools/cli/version_diag* tools/cli/projects_registry* tools/cli/cmd_doctor.cpp tools/cli/cmd_projects.cpp tools/cli/cli_common.*` is empty, so Phase 2/4 stays current | version + config + upgrade ports; adds cmd_version.cpp, cmd_config.cpp, cmd_upgrade.cpp, update_check.cpp/hpp, update_mode.cpp/hpp to watched files; 8 new fixtures (3 version, 3 config, 3 upgrade templates) |
| 2026-04-23 | Phase 6 | 2a8269c1... | none since Phase 5 — no commits on any watched file between Phase-5 bump and the Phase-6 kickoff | `pr` (happy-path shipyard delegation, `--native` rejected), `projects add/remove/prune` (extends Phase-4 list), `sdk` status+clean (install stubbed), `build` (no `--watch`), `test`, `run`, `clean`, `status` (partial — no SDK info, no git branch), `cache` status+clean (fetch stubbed). Adds `proc::Spawner` / `project::ActiveProject` shared plumbing; adds cmd_pr.cpp, cmd_sdk.cpp, cmd_build.cpp, cmd_run.cpp, cmd_misc.cpp to watched files. 1 parity fixture (sdk/empty), 10 new integration tests (`orchestrate_parity_test.rs`). Deferred: `dev` (watch_loop), `create` (template tree), `docs` (YAML walker), `design` (design_binding), and the 4 C++-linked commands (host, audio, ship, validate) by policy. |
| 2026-04-23 | Phase 6c | c3c5db0c... | `git log cadf06e2..c3c5db0c -- tools/cli/package_commands.cpp tools/cli/package_commands.hpp tools/cli/package_registry.hpp tools/cli/pulp_cli.cpp` is empty — the package-manager surface is unchanged between the Phase-6b anchor and the Phase-6c anchor, so the port mirrors the C++ code at `cadf06e2`. | **Ports:** `list` (3-col table + `--json`), `remove` (inverse of add, with metadata strip), `update` (dry-run + `--apply`), `suggest` (description / analyze / alternative / JSON), `target` (list/add/remove + pulp.toml splice writer), `audit` (`--packages` / `--platforms` / `--licenses` internal; default delegates to `tools/audit.py` via `Spawner`). **Ported-partial:** `add` (full license-gate + platform-guard + CMake gen; C++ `--lane` flag not ported), `search` (local registry fuzzy-search + JSON; `--refresh` recognised but a no-op — the C++ path shells out to `curl`). **New modules:** `src/pkg/registry.rs` (JSON registry + lock file readers, `BTreeMap`-backed deterministic iteration, `find_project_root` + `search`), `src/pkg/license.rs` (SPDX verdict + tier + explanation mirrors), `src/pkg/targets.rs` (`PlatformTarget` parse + pulp.toml splice writer), `src/pkg/metadata.rs` (alphabetical `DEPENDENCIES.md` / `NOTICE.md` insertion), `src/pkg/cmake.rs` (pure `FetchContent` renderer — matches C++ line-by-line), `src/cmd/pkg.rs` (7-command dispatch), `src/cmd/audit.rs` (flag parse + Rust-native + Spawner delegate). **Fixtures:** 4 project fixtures (`empty_project`, `one_package`, `multi_platform`, `license_conflict`), each with `CMakeLists.txt` + `core/` marker + `tools/packages/registry.json` + optional `packages.lock.json` / `pulp.toml`. **Tests added:** 29 integration tests (`pkg_parity_test.rs`) + ~60 unit tests across `src/pkg/*` + `src/cmd/pkg.rs` + `src/cmd/audit.rs`; total suite 359 passing. **Deviations:** `search --refresh` is a no-op (no HTTP fetch); `add --lane` not surfaced; `audit` passthrough assumes `python3` on PATH (matches C++ behavior). Adds `tools/cli/package_commands.cpp`, `package_commands.hpp`, `package_registry.hpp` to watched files. |
| 2026-04-23 | Phase 6b | cadf06e2... | none on already-watched files between Phase 6 and Phase 6b — the 12 commits between `2a8269c1` and `cadf06e2` touched plugin-planning / shipyard pin / CLAP unaligned access / MSVC /utf-8 / stress harness, none of which modify the Rust-mirrored CLI surface. New files added to the watched set: `cmd_project.cpp`, `project_bump.cpp`, `project_bump.hpp`, `cmd_host.cpp` (scan slice only), `pulp_cli.cpp` (banner + fuzzy suggester + bare-invocation). | **Ports:** `help` top-level subcommand + bare-invocation parity (prints usage banner, exit 0); fuzzy "Did you mean…?" suggester with C++ `dist<=3` threshold; `project` singular (`bump` with full flag surface incl. `--all`, `--dry-run`, `--force-dirty`, `--allow-downgrade`, `--verify-builds`, positional + `--to` + `--to=` forms) and `project undo [<timestamp>]` with undo-batch JSON round-trip; `scan` as a file-enumeration stub (walks CLAP/VST3/AU/LV2 system roots, prints `[FMT] N plugin(s)` groups matching the C++ writer shape). **New modules:** `src/help.rs` (shared usage banner + Levenshtein), `src/bump.rs` (pure-logic pin discovery + rewrite + undo-batch serde), `src/cmd/help.rs`, `src/cmd/project.rs`, `src/cmd/scan.rs`. **Fixtures:** 5 project-bump fixtures (fetch-content, pulp_add_project, project-version, dynamic-branch, no-pin); 2 scan fixtures (mixed-formats, empty); 1 help fixture (captured C++ banner). **Tests added:** 7 help-parity, 14 project-parity, 6 scan-parity; total suite 275 passing. **Deviations:** migration-note rendering (`migration_runtime.cpp`) stubbed — Rust port prints a one-line pointer to the C++ binary for per-hop notes. Scan's plug-in-metadata column uses file basename instead of the vendor/version/unique-id the C++ host pulls from dlopen+factory. Bare-invocation exit code is 0 (matches C++ `pulp` argv<2). Unknown-command exits 1 (matches C++); clap's default of 2 is overridden in `clap_exit_code`. |
| 2026-04-24 | pulp#740 (Slices A+B+C+D) | 7fbd7db1... | `git log abe2b07a..7fbd7db1 -- tools/cli/cmd_project.cpp tools/cli/project_bump.cpp tools/cli/project_bump.hpp tools/cli/cli_common.cpp tools/cli/cli_common.hpp` — diff = 7fbd7db1 itself ("feat: harden project SDK bump flow", +1082 −171 lines). | **Ports the upgrade + project-bump safety-rail spec.** **Slice A:** source-checkout refusal (`is_pulp_source_checkout` — CMakeLists + core/ + tools/cli + tools/shipyard.toml all present), `--allow-cli-skew` gate refusing target newer than installed CLI. **Slice B:** `--allow-redundant` flag + fail-open `origin/main` redundant-pin probe (`probe_origin_main_pin`, `git_show`, `parse_toml_string_value` helpers in `cmd::project`). **Slice C:** standalone-mode bump (`is_standalone_project`, `bump_one_standalone`) — pulp.toml `sdk_version` source-of-truth, `find_package(Pulp X.Y.Z)` mirror, `project(VERSION)` preserved. New `bump.rs` helpers: `find_find_package_pulp_version`, `find_toml_pin_site`. Atomic apply with rollback on mirror failure. **Slice D:** `UndoEdit` struct, `UndoEntry.{notes, edits}`, three new `PinKind` variants (`PulpTomlSdkVersion`, `PulpTomlSdkPath`, `CMakeFindPackagePulpVersion`); legacy single-edit `bump-undo-*.json` files still deserialize via `#[serde(default)]`. **Tests:** 30+ new (10 in `bump.rs`, 9 standalone-mode + 7 source-checkout/skew/redundant/edits in `cmd::project`). PR #742, branch `feature/rust-upgrade-bump-port-740`. **Deferred to Slice C2:** conservative `sdk_path` managed-cache rewrite (needs shared standalone-SDK resolution helpers from `cli_common.cpp` to land first). Adds `tools/cli/cli_common.cpp`, `cli_common.hpp` to the watched files list (already implicitly mirrored as needed; codifying explicitly now). |
| 2026-04-23 | Phase 6d | abe2b07a... | `git log c3c5db0c..abe2b07a -- tools/cli/cmd_dev.cpp tools/cli/cmd_create.cpp tools/cli/cmd_docs.cpp tools/cli/cmd_design.cpp tools/cli/design_binding.cpp tools/cli/tool_registry.cpp tools/cli/tool_registry.hpp` is empty — the five Phase-6d surfaces are unchanged between the Phase-6c anchor and the Phase-6d anchor. Port mirrors the C++ code at `c3c5db0c`. | **Ports:** `dev` (full flag parse, configure+build+test+run one-shot, watch loop stubbed); `create` (`--ci` scaffold: name derivation, random VST3 UID, `{{VAR}}` template expansion, format-gated entry files, standalone `main.cpp`, `pulp.toml`, `examples/CMakeLists.txt` injection, Android tree copy — skipped doctor/ensure_sdk/build/registry-add); `docs` (`index`/`search`/`open`/`show {support,command,cmake,style}` Rust-native; `check`/`build-site`/`build-api` = `Spawner` delegates); `design` (binding resolution + one-shot launch; watch stubbed); `tool` (`list`/`path`/`uninstall`/`run`/`doctor` Rust-native; `install` stubbed). **New modules:** `src/cmd/dev.rs` (296 LOC), `src/cmd/create.rs` (975), `src/cmd/docs.rs` (1,104), `src/cmd/design.rs` (392), `src/cmd/tool.rs` (510), `src/tool_registry.rs` (409). **New crate dep:** `rand = "0.8"` with `std`+`std_rng` features (drives `make_vst3_uid`; scope-justified in Cargo.toml). **Fixtures:** `tests/fixtures/docs/mini_docs_tree/` (CMakeLists + core/ + docs/status/docs-index.yaml + 2 sample .md files); `tests/fixtures/tool/minimal_registry/` (CMakeLists + core/ + tools/packages/tool-registry.json). **Tests added:** 16 integration-level (`phase6d_parity_test.rs`: dev help flags, dev-outside-project error, create help/ci-missing/name-missing, docs search/index/open-unknown/show-unknown-topic, design-outside-checkout, tool bare/list/uninstall-missing/install-stub/path-unknown) + ~75 unit tests across the 6 new modules. Total suite: 16 + 319 lib + existing = **all passing**. **Deviations / stubs:** (1) `dev --watch` and `design --watch` do a single build+launch with a notice; FS-watcher requires `notify` crate + ~300 LOC glue, tracked in the Deferred list. (2) `create` rejects non-`--ci` mode; interactive wizard stays on C++. (3) `create` skips doctor pre-flight, SDK fetch, post-scaffold build, and registry add — those call into subsystems not yet ported. (4) `tool install` prints "not ported" and returns rc=1; archive download + extraction requires tar/zip/xz crates + platform chmod/xattr. (5) `design` binding logic skips the cache-root disagreement probe (only matters when `--build-dir` points at an unrelated checkout). Adds `tools/cli/cmd_dev.cpp`, `cmd_create.cpp`, `cmd_docs.cpp`, `cmd_design.cpp`, `design_binding.cpp`, `tool_registry.cpp`, `tool_registry.hpp` to the watched-files list. Deferred list is now empty of command-level gaps — remaining items are sub-command polish (watch loops, install, host-dlopen scan, interactive wizard). |
