# Pulp Coverage Continuation Plan - 2026-05-26 14:23:36 PDT

## Active Goal

Continue the Pulp Codecov closure effort from latest main until all remaining path/component coverage gaps are above the repo's Codecov requirements, prioritizing areas currently below 75%. Always work from a fresh worktree based on up-to-date origin/main, fetch/rebase main between PRs, and never touch unrelated dirty checkout changes.

For each PR, pick one coherent area/component, identify uncovered branches with Codecov/local coverage evidence, and deliver a batch of roughly 36-48 meaningful fixes/assertions/tests. Keep each batch scoped enough to review but large enough to avoid excessive CI runs. Prefer behavioral regression tests and small correctness guards where uncovered edge cases expose real bugs; avoid superficial coverage-only assertions.

Before opening each PR:
- run focused tests for the touched area
- run local diff-cover or the repo's expected coverage gate
- run skill/version checks required by Pulp
- confirm the branch is based on latest origin/main

Ship PRs through Shipyard/Pulp's normal PR workflow. After each PR opens, sweep GitHub comments/review threads shortly after submission, address actionable P0/P1/P2 feedback in the same PR, rerun focused validation, and push follow-up fixes. Track what landed, what remains below threshold, and choose the next highest-impact area after each PR merges or is safely handed off.

## Operating Rules

- Read `CLAUDE.md` before editing. Release builds are the default; when reporting a Release build, verify both `CMAKE_BUILD_TYPE=Release` and target flags include `-O3 -DNDEBUG`.
- Use fresh worktrees based on current `origin/main`; fetch/rebase between PRs because main has been moving quickly.
- Batch work into roughly 36-48 meaningful tests/fixes/assertions per PR to reduce CI churn.
- Keep unit tests behavior-driven and useful. Avoid coverage-only tests that pass by mocking away the real contract.
- Do not use routine local SSH for Windows/Ubuntu. Use Shipyard and GitHub/runner orchestration for cross-platform validation. Only use SSH as an optional debugging fallback if it unblocks a specific issue.
- Do not remove unrelated dirty worktrees or locked agent worktrees. Clean merged coverage worktrees after their PRs land.
- GitHub API may be rate-limited for `gh` commands; Shipyard state and direct PR pages may be needed until rate reset.

## Recently Landed Coverage PRs

- PR #2983, `test(design): cover debug helper edge contracts`, merged at `27b1b5ed52ad`.
- PR #2995, `test(mcp): cover command contract edges`, merged at `c18cd2747c7ce8c72d2d5923a25b4bd3bb65a63d`.
- Main at handoff: `origin/main` = `bbcdea0fa docs: regenerate changelog for v0.255.2 [skip ci]`.

## Cleaned Worktrees

These merged coverage worktrees were already removed:

- `/private/tmp/pulp-coverage-design-debug-20260526`
- `/private/tmp/pulp-coverage-python-bindings-20260526`

Deleted merged local branch:

- `feature/coverage-design-debug-contracts-20260526`

## Active Coverage Worktree

- Worktree: `/private/tmp/pulp-coverage-cli-command-contracts-20260526`
- Branch: `feature/coverage-cli-command-contracts-20260526`
- Base: `origin/main` at `bbcdea0fa`
- Current scope: `tools/cli/cmd_version.cpp` and `test/test_cli_shellout.cpp`
- Codecov evidence: latest Codecov main report was stale relative to `origin/main` during this pass. The `tools/cli` path was at 35.51% coverage with `cmd_version.cpp` at 2.44% coverage.

### Current Batch Contents

The batch targets the CLI version command contract with about 48 meaningful assertions/checks:

- Added a guard in `pulp version check` so unknown options fail with exit code 2 instead of being silently ignored.
- Added shell-out tests for:
  - `pulp version` outside a project reporting SDK info while `version check` fails clearly.
  - a complete matching project fixture passing `version check`.
  - mismatched project/plugin/changelog/templates/plugin metadata reporting all drift surfaces.
  - unknown `version check` options failing before checks run.
  - `version bump patch` updating project version and changelog guidance.
  - `version bump --plugin patch` updating only the plugin version.
  - missing and invalid bump components returning errors.

### Validation Status

A Release configure succeeded with:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=ON -DPULP_BUILD_TESTS=ON -DSKIA_DIR=/private/tmp/pulp-coverage-cli-command-contracts-20260526/external/skia-build
```

The worktree had to fetch the pinned Skia release asset because the shared local Skia cache was stale for chrome/m149 and lacked `include/effects/SkGradient.h`:

```bash
python3 tools/scripts/fetch_skia_for_release.py darwin-arm64
```

Focused validation passed before rebasing to latest main:

```bash
cmake --build build --target pulp-cli -j$(sysctl -n hw.ncpu) && (cd build/test && ./pulp-test-cli-shellout "[cli][shellout][version]")
```

Result: 9 test cases passed, 135 assertions passed.

The branch was then rebased from `c18cd2747` to `bbcdea0fa` and the focused validation command was rerun successfully:

```text
All tests passed (135 assertions in 9 test cases)
```

## Required Next Steps For This PR

1. Finish the focused build/test:

   ```bash
   cmake --build build --target pulp-cli -j$(sysctl -n hw.ncpu)
   (cd build/test && ./pulp-test-cli-shellout "[cli][shellout][version]")
   ```

2. Verify Release status:

   ```bash
   grep '^CMAKE_BUILD_TYPE' build/CMakeCache.txt
   grep '^CXX_FLAGS ' build/test/CMakeFiles/pulp-test-cli-shellout.dir/flags.make
   ```

3. Run hygiene and repo gates:

   ```bash
   git diff --check origin/main...HEAD
   python3 tools/scripts/skill_sync_check.py --base origin/main
   python3 tools/scripts/version_bump_check.py --base origin/main --mode=report
   python3 tools/scripts/docs_sync_check.py --base origin/main
   python3 tools/scripts/test_run_coverage.py
   python3 tools/scripts/test_workflow_lint.py
   ```

4. Fetch and rebase before opening the PR:

   ```bash
   git fetch origin main --prune
   git rebase origin/main
   ```

5. Commit as:

   ```text
   test(cli): cover version command contracts
   ```

   Add required gate trailers if the scripts request them. A likely skill trailer is:

   ```text
   Skill-Update: skip skill=cli-maintenance reason="version command tests and option guard only; no CLI maintenance workflow guidance changed"
   ```

6. Open through Shipyard:

   ```bash
   shipyard pr
   ```

   If Shipyard tries routine local SSH Windows/Ubuntu validation, skip those targets and let Shipyard/GitHub runners handle them.

7. Sweep comments/checks shortly after PR creation. Address actionable P0/P1/P2 feedback in the same PR, rerun focused validation, and push.

## Next Coverage Targets After This PR

The user specifically called out these files and folders as desired next passes:

- `bindings/python/bindings.cpp`
- `ship/platform/mac/codesign_mac.mm`
- `apple/Sources/PulpSwift/PulpBridge.cpp`
- `apple/Sources/PulpSwift/PulpMotionProbe.swift`
- `ship/platform/linux/package_linux.cpp`
- `inspect/include/pulp/inspect/inspector_server.hpp`
- `inspect/include/pulp/inspect/inspector_window.hpp`
- `inspect/include/pulp/inspect/inspector_overlay.hpp`
- `tools/design/pulp_design_debug.cpp`
- `tools/audio/src/excerpt_service.cpp`
- `tools/audio/src/model_registry.cpp`
- `tools/import-design/design_import_benchmark.cpp`
- `tools/import-design/import_detect.cpp`
- folders: `inspect/src/`, `tools/mcp/`, `tools/scan-worker/`, `tools/screenshot/`, `tools/design/`, `tools/cli/`

Recommended order after the CLI version batch:

1. Continue `tools/cli/` if Codecov still shows very low files with testable command contracts.
2. Pick one coherent folder/component from the user's list and do another 36-48 assertion batch.
3. Re-query Codecov after each merged PR because the Codecov main branch number has been stale at times and may lag `origin/main`.

## Open Worktrees At Handoff

Coverage worktree to continue or clean after PR merge:

- `/private/tmp/pulp-coverage-cli-command-contracts-20260526`

Known unrelated worktrees present; do not clean unless their owner confirms they are merged/disposable:

- `/private/tmp/pulp-native-cross-compare`
- `/private/tmp/pulp-prefs-tooltip-dragger`
- `/private/tmp/pulp-runtime-utils`
- `/private/tmp/pulp-webgpu-rpath-pr`
- `/Users/danielraffel/Code/pulp-claudemd-release`
- `/Users/danielraffel/Code/pulp-platform-native-window-embedding`
- `/Users/danielraffel/Code/pulp-platform-osc`
- `/Users/danielraffel/Code/pulp-windows-signal-m-pi`
- `/Users/danielraffel/Code/pulp-wt-macos-cross-phase5`
- `/Users/danielraffel/Code/pulp-wysiwyg-p1-p2`
- `/Users/danielraffel/worktrees/cli-modularization`
- locked `.claude/worktrees/agent-*` worktrees
- Shipyard/local-ci prepared worktrees under `~/Library/Application Support/Pulp/local-ci/`
