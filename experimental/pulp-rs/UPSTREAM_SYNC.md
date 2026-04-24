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
