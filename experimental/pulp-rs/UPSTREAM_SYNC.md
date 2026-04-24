# Upstream drift tracking — pulp-rs port

**Purpose:** keep this Rust port in sync with the Pulp C++ CLI as new features land on `main`. Each time a phase completes, we bump `last_synced_sha` to the `origin/main` tip at that moment, so the next phase can diff against it and catch features that need porting.

## Current anchor

```
last_synced_sha = 2a8269c1db2c8137b1f0180f5a9d530ea0594bd1
last_synced_date = 2026-04-23
last_synced_phase = Phase 6 (pr + projects-writes + sdk + orchestrate: build/test/run/clean/status/cache)
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
tools/cli/cli_common.cpp         # partial — pulp_home / read_sdk_version / read_user_config_value
tools/cli/cli_common.hpp         # partial
```

**Deferred watched files** — not in Phase 6 scope; will be added when the
corresponding command is ported:

```
tools/cli/cmd_dev.cpp            # deferred — needs watch_loop port
tools/cli/cmd_create.cpp         # deferred — template tree + registry write + 712 LOC
tools/cli/cmd_docs.cpp           # deferred — 699 LOC YAML walker + mkdocs delegation
tools/cli/cmd_design.cpp         # deferred — design_binding.cpp dependency
```

As more commands are ported, add their C++ sources here.

## Classification matrix (2026-04-24 audit)

Run the post-implementation audit checklist (below) after every phase. The table below is the snapshot as of commit `362b1fdb` on `explore/rust-cli-prototype`.

| C++ entrypoint | Classification | Notes |
|---|---|---|
| `test` | **Ported** | Auto-build + `ctest --output-on-failure` + passthrough |
| `clean` | **Ported** | `build/` deletion |
| `projects` | **Ported** | `list` + `add` + `remove` + Rust-extra `prune` + `--json` |
| `--help` / `-h` | **Ported** | Clap auto-generated |
| `build` | **Ported-partial** | Missing `--watch`, `--test-filter=...`, real `--validate`; simpler configure logic |
| `run` | **Ported-partial** | Missing macOS `.app` fallback |
| `status` | **Ported-partial** | Only short summary; missing git branch/commit, SDK detail, format counts |
| `doctor` | **Ported-partial** | Only `--versions --json`; default doctor + `android` + `ios` + `--fix` + `--ci` + `--dry-run` + `--scan-parents` missing |
| `cache` | **Ported-partial** | `status` + `clean` real; `fetch` stubbed |
| `sdk` | **Ported-partial** | `status` + `clean` real; `install` stubbed |
| `upgrade` | **Ported-partial** | Check/notes ported; install stubbed; positional version missing; default action differs |
| `version` | **Ported-partial** | Show only; missing `bump` + `check` subcommands |
| `pr` | **Ported-partial** | Shipyard delegation real; `--native` fallback missing; version-pin enforcement weakened to advisory |
| `config` | **Ported-partial** | `get/set/list` real; empty-invocation differs; `update.mode` snooze-clear side effect may be missing |
| `create` | **Deferred** | 712 LOC — template tree + CMake-list injection + random VST3 UID |
| `design` | **Deferred** | Depends on `design_binding.cpp` |
| `docs` | **Deferred** | 699 LOC — YAML walker + mkdocs subprocess |
| `dev` | **Deferred** | Depends on `watch_loop` (multi-platform FS-watcher) |
| `validate` | **Stays in C++** | Uses `pulp::view::render_to_file` + `pulp::format` directly |
| `ship` | **Stays in C++** | Uses `pulp::ship::*` APIs directly |
| `audio` | **Stays in C++** | Uses `pulp::tools::audio::*` directly |
| `host` | **Stays in C++** | Uses `pulp::host::{PluginScanner, PluginSlot}` directly |
| `scan` | **Deferred** | Previously missed — depends on plugin-scanner via `pulp::host`; could port the non-host parts but value is low without the scan result |
| `project` (singular) | **Deferred** | Previously missed — per-project SDK pin `bump`/`undo`; worth porting in Phase 6b alongside `projects` |
| `tool` | **Deferred** | Previously missed — lives in `pulp::cli::tools::` namespace with registry lookup |
| `help` (bare) | **Deferred** | Previously missed — top-level `pulp help` prints usage; `pulp-rs help` is disabled |
| `add` / `remove` / `list` / `search` / `update` / `suggest` / `target` / `audit` | **Deferred** | Previously missed — entire package-manager subsystem in `pulp::cli::pkg::` namespace. 8 commands. Big enough to deserve its own phase (6c / 9) |
| `ci-local` / `add-component` | **Already-delegate** | Python-script shims — unchanged |
| `design-debug` / `inspect` / `import-design` / `export-tokens` | **Already-delegate** | Built-binary shims — unchanged |
| `install` (legacy) | **Already-delegate** | Alias for `cache fetch skia` |

**Revised completion:** ~25% feature-complete (5 Ported + 10 Ported-partial at their core paths) against ~30 distinct user-visible commands. Phase 8 (swap) is NOT ready — flipping today would push 12 commands onto the `pulp-cpp` fallthrough path, not 5 as the Phase 7 design implied.

## Deferred list (needs porting in future phases)

Explicitly classified as deferred with scope reasons — NOT swept under the rug:

- `dev` — multi-platform FS-watcher; multi-day
- `create` — 712 LOC template-tree + CMake generator
- `docs` — 699 LOC mkdocs + YAML walker
- `design` — design-tool binary resolution
- `tool` (`pulp::cli::tools::`) — registry lookup + install/uninstall/path/run/doctor subcommands
- `scan` — plugin scanner (uses `pulp::host`; may split ports into Rust/C++ portions)
- `project` (singular) — per-project SDK pin bump/undo
- `help` (top-level) — bare-invocation + `pulp help` usage dump
- Package-manager subsystem (8 commands: `add / remove / list / search / update / suggest / target / audit`)

Phase 6b / 7 / 8 scope decisions should pick subsets of this list based on swap-day cost-benefit. `help` + `project` + `scan` are cheap; the package-manager subsystem is a meaningful chunk; `dev` + `create` + `docs` + `design` + `tool` are expensive.

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
    tools/cli/cli_common.cpp \
    tools/cli/cli_common.hpp
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
