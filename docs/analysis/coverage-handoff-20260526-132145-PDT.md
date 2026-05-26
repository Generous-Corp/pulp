# Pulp Codecov Closure Handoff - 2026-05-26 13:21:45 PDT

## Current Goal

Continue the Pulp Codecov closure effort from latest main until all remaining path/component coverage gaps are
above the repo's Codecov requirements, prioritizing areas currently below 75%. Always work from a fresh worktree based
on up-to-date origin/main, fetch/rebase main between PRs, and never touch unrelated dirty checkout changes.

For each PR, pick one coherent area/component, identify uncovered branches with Codecov/local coverage evidence, and
deliver a batch of roughly 36-48 meaningful fixes/assertions/tests. Keep each batch scoped enough to review but large
enough to avoid excessive CI runs. Prefer behavioral regression tests and small correctness guards where uncovered edge
cases expose real bugs; avoid superficial coverage-only assertions.

Before opening each PR:
- run focused tests for the touched area
- run local diff-cover or the repo's expected coverage gate
- run skill/version checks required by Pulp
- confirm the branch is based on latest origin/main

Ship PRs through Shipyard/Pulp's normal PR workflow. After each PR opens, sweep GitHub comments/review threads shortly
after submission, address actionable P0/P1/P2 feedback in the same PR, rerun focused validation, and push follow-up
fixes. Track what landed, what remains below threshold, and choose the next highest-impact area after each PR merges or
is safely handed off.

## Operating Constraints

- Read `CLAUDE.md` before making changes. Current main says Release is the default.
- Do not use routine local SSH for Windows/Ubuntu. Use Shipyard/GitHub runners for those lanes. SSH Windows is only a last-resort debugging aid if GitHub/Shipyard is stuck.
- Use Shipyard/Pulp's normal PR flow. Keep PRs batched at roughly 36-48 meaningful fixes/assertions/tests to avoid unnecessary CI runs.
- Keep worktree hygiene strict. Do not remove dirty, locked, or unmerged worktrees. Never revert unrelated changes.
- Keep tests behavior-focused. Avoid padding assertions just to move coverage.
- Refresh/pull latest `origin/main` between PRs; Codecov branch coverage must reflect the current branch head.

## Latest Main

At handoff, `origin/main` was fetched and pointed at:

```text
9f77dc2bb fix: bundle 8 P2 Codex findings from #2976 (auv3 bundle path, coreaudio wg, vst3 59.94, clap lifecycle, ship, audio cache, midi-ci, screenshot) (#2991)
```

The primary checkout at `/Users/danielraffel/Code/pulp` is bare; run worktree-specific `git status` commands from the worktree paths below.

## Open PR To Monitor

PR: https://github.com/danielraffel/pulp/pull/2983

- Title: `test(design): cover debug helper edge contracts`
- Branch: `feature/coverage-design-debug-contracts-20260526`
- Worktree: `/private/tmp/pulp-coverage-design-debug-20260526`
- Head: `026a9f40b2d11ebb339efbd8aa14253e958b15e7`
- Scope: `tools/design/pulp_design_debug.cpp` coverage via `test/test_design_debug_contracts.cpp`
- Batch size: 46 added assertions, 151 insertions
- Local validation already passed:
  - `cmake --build build --target pulp-test-design-debug-contracts -j$(sysctl -n hw.ncpu)`
  - `./build/test/pulp-test-design-debug-contracts`
  - `git diff --check HEAD~1 HEAD`
  - skill/version/docs coverage gate scripts noted in prior session
- GitHub state at 2026-05-26 13:21 PDT:
  - No comments or reviews visible.
  - macOS and macOS local checks passed.
  - Linux and Windows GitHub-hosted checks were still in progress.
  - AddressSanitizer, ThreadSanitizer, and UndefinedBehaviorSanitizer were queued.
  - IWYU advisory was still in progress.

Next agent action:

```bash
gh pr view 2983 --json number,state,mergeStateStatus,comments,reviews,statusCheckRollup,url,title
gh pr checks 2983
```

Address any actionable P0/P1/P2 review/CI feedback in the same PR. If PR #2983 merges, remove `/private/tmp/pulp-coverage-design-debug-20260526` and delete `feature/coverage-design-debug-contracts-20260526`.

## Current On-Deck Batch

Worktree:

```text
/private/tmp/pulp-coverage-python-bindings-20260526
```

Branch:

```text
feature/coverage-mcp-command-contracts-20260526
```

Current base note: this worktree has been rebased onto `9f77dc2bb`, which was `origin/main` after the latest fetch on 2026-05-26.

Important note: this worktree path says Python bindings because that was the first investigated target. The actual useful on-deck work is now `tools/mcp/`. Python bindings looked mostly saturated except NumPy `process_numpy*` paths, and this runner's Python lacked NumPy, so that was not a good high-value batch without adding a dependency.

Files currently changed:

- `tools/mcp/mcp_json.hpp`
- `tools/mcp/mcp_shell.hpp`
- `tools/mcp/mcp_tools.cpp`
- `tools/mcp/pulp_mcp.cpp`
- `test/test_mcp_server.cpp`
- this handoff doc

Behavior fixes currently in the MCP batch:

- `extract_string()` now only accepts a JSON string value immediately after the key's colon. Before this fix, asking for a string from a numeric field could scan forward and return text from the next quoted key/value.
- `handle_validate()` now uses `extract_bool(params_json, "all", false)`. Before this fix, `{"all":false,"json":true}` still appended `--all` because the old code searched for any `"true"` in the argument object.
- MCP shell command construction now quotes project paths and user arguments for build/test/validate/create/docs/screenshot tool paths. This preserves paths/filters/queries with spaces and apostrophes as single shell arguments.

Coverage/tests currently added:

- `MCP JSON helpers preserve raw tokens and reject partial scalars`
- `pulp-mcp flag-only invocations do not enter the JSON-RPC loop`
- `MCP validate only passes --all for the explicit all flag`
- `MCP shell_quote keeps shell arguments atomic`
- `MCP build and test handlers quote project paths and filters`
- `MCP docs_search and create quote user arguments`

Focused validation already passed:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_PYTHON=OFF -DPULP_BUILD_TESTS=ON
cmake --build build --target pulp-test-mcp-server -j$(sysctl -n hw.ncpu)
./build/test/pulp-test-mcp-server
```

Result:

```text
All tests passed (400 assertions in 29 test cases)
```

Repo pre-PR gates already passed after the latest fetch/rebase check:

```bash
cmake --build build --target pulp-test-mcp-server -j$(sysctl -n hw.ncpu)
./build/test/pulp-test-mcp-server
git diff --check origin/main...HEAD
python3 tools/scripts/skill_sync_check.py --base origin/main
python3 tools/scripts/version_bump_check.py --base origin/main --mode=report
python3 tools/scripts/docs_sync_check.py --base origin/main
python3 tools/scripts/test_run_coverage.py
python3 tools/scripts/test_workflow_lint.py
```

This is now the requested batch shape for a focused MCP PR: roughly 40 meaningful new assertions across six tests, with three real correctness guards.

Suggested next MCP additions:

- Cover `handle_test()` filter command construction with a fake project CLI, including empty filter and a filter with spaces/special shell characters if the command surface supports it safely.
- Cover `pulp_inspect_evaluate` parameter forwarding with JSON-escaped expression text. Be careful: current code concatenates `inspector_params` without shell quoting in one arm, so inspect the exact behavior before asserting. If it exposes a bug, fix it with a small guard and test.
- Cover `pulp_create` required/optional argument command construction with fake `tools/create-project.py` in a fake project root, if that can be hermetic without shell portability problems.
- Cover `Content-Length:` framing path in `pulp_mcp_main_for_test` by redirecting `std::cin`/`std::cout` with an input stream containing one framed initialize request. Keep stream restoration exception-safe.

Before PR:

```bash
git fetch origin --prune
git rebase origin/main
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_TESTS=ON
cmake --build build --target pulp-test-mcp-server -j$(sysctl -n hw.ncpu)
./build/test/pulp-test-mcp-server
git diff --check origin/main...HEAD
python3 tools/scripts/skill_sync_check.py --base origin/main
python3 tools/scripts/version_bump_check.py --base origin/main --mode=report
python3 tools/scripts/docs_sync_check.py --base origin/main
python3 tools/scripts/test_run_coverage.py
python3 tools/scripts/test_workflow_lint.py
```

Then open with Shipyard/Pulp PR workflow. Do not use local SSH runners for Windows/Ubuntu.

## User-Requested Low-Coverage Areas

Requested files:

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

Requested folders:

- `inspect/src/`
- `tools/mcp/`
- `tools/scan-worker/`
- `tools/screenshot/`
- `tools/design/`
- `tools/cli/`

What is already handled or likely saturated from recent work:

- `tools/design/pulp_design_debug.cpp`: PR #2983 open, awaiting CI.
- `tools/audio/src/excerpt_service.cpp` and `tools/audio/src/model_registry.cpp`: recent coverage PRs already landed.
- `ship/platform/mac/codesign_mac.mm`: recent coverage branch already landed.
- `ship/platform/linux/package_linux.cpp`: recent Linux packaging coverage already landed.
- `apple/Sources/PulpSwift/PulpBridge.cpp`: recent PulpBridge coverage already landed.
- `apple/Sources/PulpSwift/PulpMotionProbe.swift`: existing Swift tests cover manual probe behavior and provenance. Re-check Codecov before adding more.
- `tools/scan-worker/`: recent scan-worker coverage landed.
- `tools/screenshot/`: recent screenshot coverage landed.
- `tools/import-design/import_detect.cpp`: recent import-detect coverage landed.
- `tools/import-design/design_import_benchmark.cpp`: recent benchmark coverage landed.
- `bindings/python/bindings.cpp`: embedded test already covers module initialization, parameter/state, MIDI, and host descriptor/state behavior. NumPy process lambdas remain the obvious gap but require NumPy in the Python environment or a careful dependency decision.

Good next candidates after MCP:

- `inspect/src/` and inspector headers if Codecov still shows below-threshold branch gaps. Use existing `test/test_inspector*.cpp`, `test/test_motion_inspector.cpp`, and `test/test_motion_scrubber.cpp` as anchors.
- `tools/cli/` only if Codecov evidence shows specific low files; the folder has many recent CLI coverage PRs, so avoid broad padding.

## Worktree Cleanup State

Already cleaned this session:

- `/private/tmp/pulp-coverage-signal-special-functions-20260526`
- deleted branch `feature/coverage-signal-special-functions-20260526`

Do not remove these unless merged and clean:

- `/private/tmp/pulp-coverage-design-debug-20260526`: open PR #2983, keep until merged.
- `/private/tmp/pulp-coverage-python-bindings-20260526`: active on-deck MCP batch, keep.

Dirty merged-looking worktrees that were intentionally left alone:

- `/private/tmp/pulp-auv3-ios-validation`: branch merged to main but has local modifications.
- `/private/tmp/pulp-nsd`: branch merged to main but has local modifications/untracked files.
- `/private/tmp/pulp-prefs-tooltip-dragger`: branch merged to main but has local modifications/untracked files.
- `/Users/danielraffel/Code/pulp-windows-signal-m-pi`: branch merged to main but has local modifications.

Other worktrees were either unmerged, unrelated, locked agent worktrees, local-ci prepared trees, or detached comparison trees. Do not clean them without a fresh status/merged check.

Useful cleanup audit commands:

```bash
git worktree list --porcelain
git branch --merged origin/main --format='%(refname:short)'
git -C <worktree> status --short --branch
git merge-base --is-ancestor <branch> origin/main && echo merged || echo not-merged
```

Only remove worktrees that are clean, not locked, and whose branch is merged or clearly obsolete.

## Codecov Notes

The user noticed Codecov's branch summary panel can show a lower "Coverage on branch" value than the trend graph tooltip. Keep verifying that branch uploads are attached to the latest head SHA. When pushing PRs, refresh/update branch details so Codecov is not reporting stale head coverage.

For future PRs, confirm:

- Branch is rebased on latest `origin/main`.
- Codecov check references the latest pushed commit.
- PR description includes focused validation and coverage rationale.

## Immediate Next Steps

1. Sweep PR #2983:
   - `gh pr checks 2983`
   - `gh pr view 2983 --json comments,reviews,statusCheckRollup`
2. Continue the MCP batch in `/private/tmp/pulp-coverage-python-bindings-20260526`.
3. Open the MCP PR via Shipyard/Pulp PR workflow if it is not already open.
4. Sweep comments/reviews shortly after opening and address actionable feedback.
5. If PR #2983 or the MCP PR merges, clean only its clean/merged worktree and branch.
