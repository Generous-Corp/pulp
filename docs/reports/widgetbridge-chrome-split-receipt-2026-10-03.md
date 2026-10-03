# WidgetBridge / Forge Chrome test split receipt

This receipt records the acceptance evidence for Agent 8 branch
`codex/widgetbridge-chrome-split-20261003`, commit
`a571a6a9456b507deca2549e67afeff6d0a22b3d`.

## Inventory and identity

| Corpus | Before | After | Result |
| --- | ---: | ---: | --- |
| `test/test_widget_bridge.cpp` Catch2 cases | 216 | 216 (`176 + 40`) | Exact case-name and tag multisets preserved |
| Forge Chrome seam Catch2 cases | 191 | 191 (`188 + 3`) | Exact case-name and tag multisets preserved |

The WidgetBridge CTest group discovers 176 cases from the retained source and
40 from `test_widget_bridge_gpu_tail.cpp`. The baseline configure-only CTest
receipt listed 1,794 registrations overall; because the group executable was
not built yet, only the two static WidgetBridge checks appeared as
`_NOT_BUILT`. After the focused build, CTest listed 1,954 registrations overall
and discovered all 216 WidgetBridge cases.

The three moved Chrome cases are the byte-exact `[no-leak]` Home-frame guards
for FX, Instrument, and MIDI. The Chrome file is hand-authored: it contains
explanatory prose, named fixture/helper functions, explicit Catch2 cases, and
committed PNG baselines under `forge-seam/test/baselines/chrome-home/`. The
Forge README documents deliberate baseline refresh through
`FORGE_NO_LEAK_UPDATE=1`; this is not generated corpus output.

The Pulp checkout has no active root CTest registration for the Forge Chrome
seam. Its target and the second translation unit are carried by
`forge-seam/patches/0001-chrome-copy-from-the-shell.patch` and the seam's
private test directory.

## Validation

- CMake Release configure with examples disabled: passed.
- `pulp-test-group-view-widgets` target build: passed.
- WidgetBridge `[bridge]` execution: 636 cases, 5,411 assertions passed.
- `forge-seam/test_seam_patch.sh`: passed; applying the patch to
  `/tmp/forge-cur` was skipped because that checkout is unavailable.
- `git diff --check`: passed.
- The pushed branch contains only test sources, private test support, and the
  WidgetBridge/Forge test manifest/patch changes. No production, SignalGraph,
  or roadmap files are included.

The local diff-coverage pre-push job was skipped by the host governor because
only two build jobs were available; CI remains responsible for that required
check.
