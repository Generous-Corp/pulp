# Agent 8 / C5 test decomposition receipt

Branch: `codex/c5-agent8-followup-20261003`
Implementation base: `9b91dbc4baee9b15f3e2669813bca4dd86c001de`; receipt and support fix are included in the final commit.
Base: protected `origin/main`

## Scope and classification

This slice is test-only. It changes test translation-unit membership, a private
WidgetBridge support header, and the Forge seam patch that carries the test
sources. No production source, public header, roadmap, or generated browser
corpus was edited.

The SignalGraph and WidgetBridge files are hand-authored Catch2 tests. Their
splits preserve whole cases and their original names/tags. The Chrome corpus is
also hand-authored: it contains explicit fixture construction, helper
namespaces, process/filesystem fixtures, explanatory comments, and committed
PNG baselines. It has no generator, generated manifest, or one-record-per-case
input. The three `[no-leak]` Home-frame baseline cases now share their private
support header with the retained Chrome seam cases; no case body was rewritten.

| Corpus | Before | After | Identity |
| --- | ---: | ---: | --- |
| SignalGraph | 124 | 99 + 25 | 124/124 name+tag identities preserved |
| WidgetBridge | 216 | 176 + 40 | 216/216 name+tag identities preserved |
| Forge Chrome seam | 191 | 188 + 3 | 191/191 name+tag identities preserved |

The established focused C5 baseline tuple remains `120 / 123 / 53`. The
configure-only CTest inventory before the complete build was 3,105 tests, with
130 label names and 740 label assignments. After `pulp build --all`, the full
inventory was 22,479 tests; the larger count is the complete test graph rather
than a change to this slice's registration.

## Validation

- `~/.local/bin/pulp-worktree-lineage-session --plain` and
  `tools/scripts/worktree_lineage.sh show --path .`: active worktree.
- `pulp build --target pulp-test-host-signal-graph`: passed with no work.
- `pulp build --all`: completed the governed 2,770-target graph; the previously
  missing scan-worker and CLI binaries were produced.
- Focused split execution:
  `ctest --test-dir build -R '^(WidgetBridge|SignalGraph|Forge|Chrome|intrinsic_width)'`
  — **718/718 passed**.
- Full execution after the complete build:
  `ctest --test-dir build --output-on-failure` — **exit 8; 22,479 total,
  22,430 passed, 42 skipped, 7 failed**. The seven failures are unrelated
  environment/tooling lanes. Exact failing CTest names: `gpu-clean-agent-journey-selftest`,
  `governed-build-selftest`, `cmake-ios-auv3-configure`,
  `cmake-ios-hostapp-links`, `cmake-timeline-sdk-consumer`,
  `visual-python-deps-present`, and `cli-gpu-clean-agent-preparer-contract`.
  No split test failed, and the prior NOT_BUILT targets are gone.
- Forge seam structural receipt with a fresh Forge validation worktree:
  `FORGE_SRC=/Users/danielraffel/Code/forge-c5-agent8-validation
  bash forge-seam/test_seam_patch.sh` — passed; `forge-seam/populate.sh`
  copied the new private support header and all test files.
- `git diff --check`: passed.
- `tools/scripts/gates.sh origin/main`: passed; safe to push.

Full-suite green is not claimed: the exact seven failures above remain outside
this test-only decomposition and must be resolved by their owning lanes before
a green full-suite receipt can be issued.
