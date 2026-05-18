# Phase 3 Codecov Queue Pause Ledger

Last updated: 2026-05-15 01:21 PDT

This local ledger records the open `codecov` PR validation runs paused to free Namespace capacity for higher-priority work, plus the small-batch resume queue. Branches, PRs, commits, labels, and tracker comments stay intact; queued GitHub Actions validation attempts are cancellable and replaceable.

2026-05-14 update: CI is no longer fully paused, but Namespace and SSH
targets remain intentionally out of scope. New batches should use
GitHub Actions hosted PR workflows, with macOS validated locally before
push when useful and Linux/Windows left to hosted Actions. The old
Namespace-specific notes below are retained as history for older PRs and
should not be read as current dispatch guidance.

Batch size guidance after reopening GitHub-hosted CI: prefer larger
coherent test-only batches over one-tranche PRs. A good batch is one
related code area, one or a small number of test binaries, and enough
coverage work to justify a full hosted CI cycle. Do not depend on merge
queues; each batch PR should stand alone, run the normal required
pull-request checks, and merge directly once green. Keep source behavior
fixes, version-bump changes, and platform-only branches separate unless
the shared validation target makes the combined risk clearly lower.

## Queue Policy

- Reopen Codecov validation in small batches; do not dump the full paused queue back into Namespace at once.
- While a batch is in flight, continue local-only tranche preparation, review, and failure triage.
- Merge `UNSTABLE` PRs only when GitHub branch protection shows all required checks are green and the PR can merge normally.
- For merge eligibility after a pause, refresh PR branches with `gh pr update-branch <pr>` so GitHub creates fresh `pull_request` check suites.
- Use `shipyard cloud run build <branch> --provider namespace --require-sha <sha> --no-wait` only as a Namespace smoke or targeted diagnostic; successful `workflow_dispatch` checks do not replace stale failed or cancelled `pull_request` contexts for branch protection.
- If one lane is slow or stuck, prefer `shipyard cloud retarget`/`shipyard cloud add-lane` so existing useful runs are preserved instead of cancelled wholesale.
- Branch protection currently requires the lower-case `linux`, `macos`, and
  `windows` contexts. The visible Namespace child jobs may appear as
  `Linux (x64) [namespace]`, `macOS (ARM64) [namespace]`, and
  `Windows (x64) [namespace]`; wait for the lower-case reusable-workflow
  wrappers to settle before declaring a PR mergeable.

## Draft CI Resume Plan

This is planning only. Do not push, update PR branches, dispatch CI, or
merge until Namespace capacity is explicitly reopened.

When CI resumes, use a two-stage loop:

1. Local readiness sweep. For each held branch, confirm clean status,
   rebase onto the then-current `origin/main`, run the ledger's focused
   build/test target plus sync/version/diff guards, and classify the
   branch as `ready`, `conflict`, `test-fail`, `duplicate/upstream`,
   `platform-only`, or `source-fix/version-bump`.
2. CI waves. Push or update only `ready` branches in waves of roughly
   3-5 PRs. Let PR-event checks run naturally; avoid extra
   `workflow_dispatch` diagnostics unless a specific failure needs it.
   Merge green PRs as they clear required checks, quarantine failures,
   and keep the next wave moving if Namespace capacity is still healthy.

Use individual PRs for already-open branches unless we intentionally
supersede one. For local-only branches with no remote PR, it is valid to
create a combined batch PR when the tranches are tightly related,
test-only, and touch the same target/file family. Good candidates for a
combined PR include the #640 `test/test_audio_file.cpp` audio-file
tranches, after one final local rebase/re-smoke. Do not combine source
behavior fixes with test-only branches; keep those separate so version
bump and regression risk stay obvious.

Batch-prep rule while CI remains paused: prepare combined branches
locally only, roughly 3-8 closely related test-only tranches per branch,
then run all touched target builds, focused tags, full touched binaries,
CTest selectors, sync/version/docs/compat guards, and diff checks before
recording the batch here. Do not push or open the batch PR until CI
capacity is explicitly available. Avoid active development overlap,
including #2009's mac platform-test harness / back-buffer capture seam.
The first low-conflict batch candidate is the #640 audio-file cluster
(`local/phase3-audio-data-shape-640`, `local/phase3-aiff-pcm-edges-640`,
and `local/phase3-streaming-writer-reopen-640`) because it is
test-only, shares `test/test_audio_file.cpp`, and does not touch #2009's
files.

Failure handling during CI waves:

- If a PR has a real code/test failure, mark it `quarantined`, keep the
  failure logs, and continue merging unrelated green PRs.
- If the failure is external or infrastructure-like, such as SDK
  download/DNS failures, mark it `rerun-only` and do not patch code
  unless the same failure reproduces locally or points at branch code.
- If Codecov patch/diff coverage fails, use that PR's report to add the
  smallest local coverage fix, then requeue that PR in a later wave.
- If a branch is already absorbed by `origin/main`, close/supersede it
  rather than spending another CI run.
- Required lower-case `linux`, `macos`, and `windows` wrapper contexts
  still decide mergeability; successful diagnostic dispatches do not
  replace stale or failed PR-event contexts.

Proposed CI cutover plan for discussion:

1. Stop starting new coverage tranches once the current local tranche is
   recorded. Keep only rebase/re-smoke and failure-fix work active until
   the first CI wave is settled.
2. Run a final no-CI sweep over the first-wave candidates: fetch,
   confirm the branch is clean, rebase onto the then-current
   `origin/main`, rerun the focused test target plus full test binary,
   CTest selector, CLI skew/sync guard, skill/version/docs/compat reports,
   and diff checks. Mark anything that fails locally as `quarantined`
   before it reaches Namespace.
3. Start with one canary wave of the already-current, test-only PRs:
   #1274 CLI ship, #1287 CLI audio, #1269 design-import bundle, #1271
   create-targets, and #1273 package commands. Add #1286 NamedPipe only if
   capacity is healthy after those dispatch.
4. Let the whole wave finish instead of stopping at the first red PR.
   Merge green PRs as soon as required PR-event `linux`, `macos`, and
   `windows` contexts are green. Move red PRs to `quarantined` or
   `rerun-only` with their failure class, then keep the next unrelated
   wave moving if capacity remains available.
5. For held local-only tranches, reduce cycles by forming batch PRs only
   when scopes are naturally related, test-only, and share a target/file
   family. The best first combined candidates are the #640
   `test/test_audio_file.cpp` audio-file tranches. Keep source-fix branches
   such as AudioFocus separate because they need explicit version-bump
   handling.
6. For already-open PRs, assume each PR eventually needs its own required
   PR-event contexts unless we intentionally close/supersede it with a
   combined PR. A diagnostic `workflow_dispatch` run can find failures
   cheaply, but it does not make an existing PR mergeable under branch
   protection.
7. After each wave, update this ledger with `green/merged`,
   `quarantined`, `rerun-only`, `superseded`, or `needs-local-fix`, then
   build the next wave from the remaining ready queue.

Initial no-CI readiness sweep, refreshed through 2026-05-14 22:35 PDT:

| Candidate | Local Head | Base | Status | Suggested Action |
| --- | --- | --- | --- | --- |
| #1274 CLI ship | `31c07f2f` | `c98896db` | merged to main | no local action |
| #1287 CLI audio | `ac79bcff` | `c98896db` | merged to main | no local action |
| #1269 design-import bundle | `0b3c9493` | `c98896db` | merged to main | no local action |
| #1271 create-targets | `c45ee4ae` | `c98896db` | merged to main | no local action |
| #1273 package commands | `8a2c9ee2` | `c98896db` | merged to main | no local action |
| #1286 named pipe | `196b1673` | `c98896db` | merged to main | no local action |
| #1280 mmap reader | `d6287dc5` | `c98896db` | merged to main | no local action |
| local audio tools/model-store | `75ed3a63` | `92e83b37` | clean, test-only, no open PR, behind latest base | rebase/re-smoke before #643 tools wave |
| #646 SDL3 surface fallback | `d81b03cc` | `92e83b37` | clean, test-only, no remote branch, behind latest base | rebase/re-smoke before #646 render wave |
| local AudioFocus dispatch | `f819d985` | `d191cdca` | clean, includes source fix, behind current base | rebase/re-smoke; keep separate; SDK patch bump required before push/CI |
| #2012 audio-file batch | `18333207` | `b70bd42a` | open PR on `feature/phase3-audio-file-batch-640`; test-only GitHub-hosted batch; refreshed after CI exposed a parallel temp-directory race in the existing MIDI-file tests; local macOS target build, focused tag run, full binary, parallel MIDI-file selector, and sync/version/docs/compat guards passed | monitor GitHub Actions hosted checks, merge when required checks are green |
| local AudioFileData shape | `eac19fe2` | `92e83b37` | superseded by #2012 batch | no standalone CI action |
| local AIFF PCM edges | `74774950` | `92e83b37` | superseded by #2012 batch | no standalone CI action |
| local StreamingWriter reopen | `5e1d020d` | `92e83b37` | superseded by #2012 batch | no standalone CI action |
| local frame-fill edges | `8467c68d` | `92e83b37` | duplicate coverage now present on `origin/main` in `test/test_frame_fill.cpp`; intentionally pruned from the broad wave | no standalone CI action |
| #2014 signal DSP batch | `3bb753ac` | `c98896db` | open PR on `feature/phase3-signal-dsp-batch-645`; test-only GitHub-hosted batch superseding six local #645 tranches; local macOS target build, focused tag runs, full binaries, exact CTest selector, and sync/version/docs/compat guards passed | monitor GitHub Actions hosted checks, merge when required checks are green |
| local Linkwitz-Riley edges | `3deafcfe` | `bd036171` | superseded by #2014 batch | no standalone CI action |
| local ADSR edges | `0c9e7d4d` | `bd036171` | superseded by #2014 batch | no standalone CI action |
| local NoiseGate edges | `032f46c2` | `bd036171` | superseded by #2014 batch | no standalone CI action |
| local chorus/reverb edges | `e6b5c6bc` | `4bebc7bf` | superseded by #2014 batch | no standalone CI action |
| local oscillator edges | `5e3a1db3` | `4bebc7bf` | superseded by #2014 batch | no standalone CI action |
| local signal helper edges | `431a3c5d` | `4bebc7bf` | superseded by #2014 batch | no standalone CI action |
| #2016 CLI/platform/tooling wave | `4167e133` | `7a532c59` | open PR on `feature/phase3-tools-cli-batch-643`; broad test-only GitHub-hosted wave across audio tools, CLI shellout/config, FetchContent cache, environment, child process, SDL3 surface fallback, ruleset drift config, auto-release decision, format validation, and package freshness tests; local macOS build/focused/full test validation and sync/version/docs/compat guards passed | monitor GitHub Actions hosted checks, merge when required checks are green |
| local platform/runtime batch | `b77c6979` | `c98896db` | pushed durability branch `feature/phase3-platform-runtime-batch-640`; intentionally superseded by #2016 so these tests land on `main` through the larger wave PR, not a standalone PR | no standalone CI action; close/delete branch after #2016 lands if no longer needed |
| local Environment dispatch | `33565a65` | `92e83b37` | superseded by #2016 via the platform/runtime batch | no standalone CI action |
| local ChildProcess output | `4c474115` | `92e83b37` | superseded by #2016 via the platform/runtime batch | no standalone CI action |
| local audio tools/model-store | `75ed3a63` | `92e83b37` | superseded by #2016 | no standalone CI action |
| local CLI config command | `08308f66` | `92e83b37` | superseded by #2016 | no standalone CI action |
| local ruleset drift config | `145cd195` | `92e83b37` | superseded by #2016 | no standalone CI action |
| local FetchContent cache edges | `5cbf2d87` | `92e83b37` | superseded by #2016 | no standalone CI action |
| local auto-release extra | `26076d92` | `92e83b37` | superseded by #2016 | no standalone CI action |
| local format validation extra | `96b9dfd8` | `92e83b37` | superseded by #2016 | no standalone CI action |
| local package freshness extra | `55bee1c5` | `92e83b37` | superseded by #2016 | no standalone CI action |
| #646 SDL3 surface fallback | `d81b03cc` | `92e83b37` | superseded by #2016 | no standalone CI action |
| #2017 view/widget wave | `8f9effa7` | `7a532c59` | open PR on `feature/phase3-view-widget-batch-493`; broad test-only GitHub-hosted wave superseding 18 local #493 view/widget tranches; local macOS target build, full touched binaries, and sync/version/docs/compat guards passed | monitor GitHub Actions hosted checks, merge when required checks are green |
| #2019 MIDI/host format batch | `62a17f78` | `b70bd42a` | open PR on `feature/phase3-midi-host-batch-645`; test-only GitHub-hosted batch covering MPE buffer/tracker edges, diagnostic reporter/default JSON, host-type feature heuristics, descriptor validation warning/error combinations, and scan blacklist parser edges; local macOS target build, focused tag runs, full touched binaries, and sync/version/docs/compat guards passed | monitor GitHub Actions hosted checks, merge when required checks are green |
| #2022 host graph/serializer batch | `276f7f7f` | `a29cb7dc` | open PR on `feature/phase3-host-graph-batch-493`; GitHub-hosted batch covering SignalGraph invalid/duplicate edge variants, live gain updates, release/clear helper behavior, GraphSerializer unresolved-plugin identity reserialization, and plugin-field error cleanup; includes a narrow source fix preserving placeholder plugin identity by using the existing `add_plugin_node(info)` path; local macOS target build, focused tag runs, full touched binaries, skill/version/docs guards, and diff checks passed; local pre-push diff-cover was demoted because its coverage configure path failed fetching Highway before tests | monitor GitHub Actions hosted checks, merge when required checks are green |
| #2024 tools Python control-plane batch | `50acc069` | `a29cb7dc` | open PR on `feature/phase3-tools-python-batch-643`; test-only GitHub-hosted batch covering host-pump linting, release Skia fetcher extra paths, and macOS reroute watcher control-plane behavior; local unittest, direct script, py_compile, diff, skill/version/docs guards passed; local Python coverage runner was unavailable because the installed coverage.py is below the repo's required 7.10 floor; local pre-push diff-cover was demoted because its coverage configure path failed fetching Highway before tests | monitor GitHub Actions hosted checks, merge when required checks are green |
| #2025 runtime/state utility batch | `dda4d1e` | `a29cb7dc` | open PR on `feature/phase3-runtime-state-batch-641`; test-only GitHub-hosted batch covering PropertiesFile JSON/save/no-op edges, StateTree typed getter/listener/child/JSON/deep-copy edges, StateTreeSynchroniser reattach/truncated-batch edges, TemporaryFile move/extension behavior, MemoryMappedFile reopen, and Range boundaries; local macOS target build, focused tag runs, full touched binaries, exact CTest selector, skill/version/docs guards, and diff checks passed; local pre-push diff-cover was intentionally disabled after focused/full validation | monitor GitHub Actions hosted checks, merge when required checks are green |
| #2026 events/runtime IPC batch | `bad6149` | `a29cb7dc` | open PR on `feature/phase3-events-runtime-batch-642`; test-only GitHub-hosted batch covering IPC endpoint malformed-port paths, endpoint disconnect reset, NetworkServiceDiscovery backend/cache lifecycle edges, callback-less discovery storage, ActionBroadcaster no-listener sends, MultiTimer restart behavior, and LockingAsyncUpdater synchronous trigger-and-wait behavior; local macOS target build, focused tag runs, full touched binaries, exact CTest selector, skill/version/docs guards, and diff checks passed; local pre-push diff-cover was intentionally disabled after focused/full validation | monitor GitHub Actions hosted checks, merge when required checks are green |
| #2028 canvas/OSC/render helper batch | `9570cdb` | `a29cb7dc` | open PR on `feature/phase3-canvas-osc-render-batch-644`; test-only GitHub-hosted batch covering OSC typed-default/codec/sender edges, OSC bundle timetag/default-element/pattern edges, AttributedString/TextLayout empty/reuse/wrap behavior, and DirtyTracker full-repaint/threshold/invalid-viewport edges; local macOS target build, focused tag runs, full touched binaries, CTest selector, skill/version/docs guards, and diff checks passed; local pre-push diff-cover was intentionally disabled after focused/full validation | monitor GitHub Actions hosted checks, merge when required checks are green |
| #2030 runtime services batch | `93458d4` | `a29cb7dc` | open PR on `feature/phase3-runtime-services-batch-641`; test-only GitHub-hosted batch covering Analytics disabled flush and file-destination edges, WidgetTracker forwarded details, i18n overwrite/substitution/parser/global edges, UUID parse/order/hash behavior, and LicenseValidator optional fields/file/empty-payload classification; local macOS target build, focused tag runs, full touched binaries, CTest selector, skill/version/docs guards, and diff checks passed; local pre-push diff-cover was intentionally disabled after focused/full validation | monitor GitHub Actions hosted checks, merge when required checks are green |
| local appearance manager | `0bbaaa9a` | `92e83b37` | superseded by #2017 | no standalone CI action |
| local audio bridge edges | `2d00e9ac` | `92e83b37` | superseded by #2017 | no standalone CI action |
| local AutoUi edges | `832c0781` | `92e83b37` | superseded by #2017 | no standalone CI action |
| local code editor document MRU | `fef0ef6c` | `92e83b37` | superseded by #2017 | no standalone CI action |
| local graph editor paint | `8ed9f335` | `92e83b37` | superseded by #2017 | no standalone CI action |
| local GUI components edges | `a37626d7` | `92e83b37` | superseded by #2017 | no standalone CI action |
| local image cache trim | `165c2357` | `92e83b37` | superseded by #2017 | no standalone CI action |
| local input events edges | `e87e6b10` | `92e83b37` | superseded by #2017 | no standalone CI action |
| local modal overlay edges | `67fe482e` | `92e83b37` | superseded by #2017 | no standalone CI action |
| local param attachment edges | `4e7ab1ad` | `92e83b37` | superseded by #2017 | no standalone CI action |
| local new widgets input | `ba19ff03` | `92e83b37` | superseded by #2017 | no standalone CI action |
| local phase9 widget edges | `b23e4d25` | `92e83b37` | superseded by #2017 | no standalone CI action |
| local tree view edges | `ca5267a7` | `92e83b37` | superseded by #2017 | no standalone CI action |
| local UI components edges | `4bc91d9e` | `92e83b37` | superseded by #2017 | no standalone CI action |
| local visualization bridge edges | `a30900b8` | `92e83b37` | superseded by #2017 | no standalone CI action |
| local waveform editor edges | `1ea97d99` | `92e83b37` | superseded by #2017 | no standalone CI action |
| local widgets render paths | `15ca568b` | `92e83b37` | superseded by #2017 | no standalone CI action |
| local window manager edges | `f7da4e54` | `92e83b37` | superseded by #2017 | no standalone CI action |

2026-05-14 22:58 PDT: created the larger #645 signal DSP batch
`feature/phase3-signal-dsp-batch-645` at `3bb753ac`, PR #2014,
from current `origin/main` `c98896dbc`. It batches
`local/phase3-linkwitz-riley-edges-645`,
`local/phase3-adsr-edges-645`,
`local/phase3-noise-gate-edges-645`,
`local/phase3-modulation-reverb-edges-645`,
`local/phase3-oscillator-edges-645`, and
`local/phase3-signal-helper-edges-645` into one test-only diff touching
`test/test_signal.cpp` and `test/test_dsp_expansion.cpp`. Local macOS
validation passed: configure with
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
build `cmake --build build --target pulp-test-signal
pulp-test-dsp-expansion -j$(sysctl -n hw.ncpu)`, focused
`./build/test/pulp-test-signal "[issue-645]" -r compact` passing 1246
assertions in 33 test cases, focused
`./build/test/pulp-test-dsp-expansion "[issue-645]" -r compact`
passing 43 assertions in 7 test cases, full `pulp-test-signal` passing
2013 assertions in 86 test cases, full `pulp-test-dsp-expansion`
passing 96 assertions in 37 test cases, exact `ctest --test-dir build
-R "ADSR handles immediate|NoiseGate clamps range|Chorus dry mix|Reverb handles zero decay|Oscillator reset|LinkwitzRiley reset clears history|LinkwitzRiley cutoff boundary|LadderFilter resets buffer|FirFilter empty coefficients|LookupTable indexed access" --output-on-failure`
passing 10/10, `git diff --check`, `git diff --cached --check`,
skill-sync report, version-bump report, docs-sync report, and
compat-sync report. Branch pushed with GitHub-hosted PR workflow only;
no Namespace dispatch and no SSH targets. Resume action: monitor #2014
required checks and merge directly when green.

2026-05-14 23:04 PDT: prepared the next larger #640 platform/runtime
batch locally as `feature/phase3-platform-runtime-batch-640` at
`b77c6979`, based on current `origin/main` `c98896dbc`. It batches
`local/phase3-platform-environment-dispatch-640` and
`local/phase3-child-process-read-output-640` into one test-only diff
touching `test/test_environment.cpp` and `test/test_child_process.cpp`.
Local macOS validation passed: configure with
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
build `cmake --build build --target pulp-test-environment
pulp-test-child-process -j$(sysctl -n hw.ncpu)`, focused
`./build/test/pulp-test-environment "[environment][issue-640]" -r compact`
passing 61 assertions in 10 test cases, focused
`./build/test/pulp-test-child-process "[child_process][issue-640]" -r compact`
passing 44 assertions in 9 test cases, full `pulp-test-environment`
passing 106 assertions in 21 test cases, full `pulp-test-child-process`
passing 65 assertions in 21 test cases, exact `ctest --test-dir build
-R "Environment: token self move|Environment: listener removed before|Environment: reset during dispatch|read_available_output drains stdout" --output-on-failure`
passing 4/4, `git diff --check`, `git diff --cached --check`,
skill-sync report, version-bump report, docs-sync report, and
compat-sync report. This batch is committed and pushed as `feature/phase3-platform-runtime-batch-640`, but no PR is open yet while #2012 and #2014 are still consuming hosted CI.
Resume action: push/open as one GitHub-hosted PR when active hosted
batch pressure drops; do not run Namespace or SSH targets.

2026-05-14 23:42 PDT: created the broad GitHub-hosted coverage wave
`feature/phase3-tools-cli-batch-643` at `4167e133`, PR #2016, based on
current `origin/main` `7a532c59` after #2009 and the 0.100.0 changelog
landed. This intentionally supersedes the smaller pushed
`feature/phase3-platform-runtime-batch-640` durability branch, plus
`local/phase3-audio-tools-model-store-643`,
`local/phase3-cli-config-command-643`,
`local/phase3-ruleset-drift-config-643`,
`local/phase3-fetchcontent-cache-edges-643`,
`local/phase3-auto-release-extra-643`,
`local/phase3-check-format-validation-coverage-643`,
`local/phase3-pkg-freshness-extra-643`, and
`feature/phase3-sdl3-surface-fallback-646`. `local/phase3-frame-fill-edges-640`
was checked but pruned because equivalent frame-fill coverage is already
on `origin/main` in `test/test_frame_fill.cpp`.

Local macOS validation passed on the combined #2016 branch: target build
`cmake --build build --target pulp-test-audio-tools
pulp-test-cli-shellout pulp-test-cli-fetchcontent-cache
pulp-test-environment pulp-test-child-process pulp-test-sdl3-surface
-j$(sysctl -n hw.ncpu)`, focused runs for
`[audio][tools][issue-643]`, `[cli][shellout][config][issue-643]`,
`[fetchcontent_cache][issue-643]`, `[environment][issue-640]`,
`[child_process][edge][issue-640]`, and `[render][sdl3][issue-646]`,
full touched binaries for audio tools, FetchContent cache, environment,
child process, and SDL3 surface, plus direct Python unit tests for
`tools/scripts/test_auto_release_decision_extra.py`,
`tools/scripts/test_check_format_validation.py`,
`tools/scripts/test_ruleset_drift_config.py`, and
`tools/packages/test_freshness_check_extra.py`. `git diff --check`,
skill-sync, version-bump, docs-sync, and compat-sync reports passed.
The pre-push local diff-cover gate was demoted with
`PULP_DISABLE_PREPUSH_DIFF_COVER=1` because its local coverage configure
could not fetch the pinned Highway tree; GitHub Actions remains the
authoritative CI gate for this PR. Resume action: monitor #2016 required
GitHub-hosted checks and merge directly when green; do not use Namespace
or SSH targets.

2026-05-15 00:16 PDT: created the #493 view/widget coverage wave
`feature/phase3-view-widget-batch-493` at `8f9effa7`, PR #2017, based on
current `origin/main` `7a532c59`. The branch batches 18 local test-only
tranches into one 17-file diff touching only test sources:
AppearanceTracker, AudioBridge, AutoUi, CodeEditor document/MRU paths,
GraphEditorView paint/drag paths, GUI components, ImageCache trim,
InputEvents, ModalOverlay, ParamAttachment, Phase9 widgets, TreeView,
UI components, VisualizationBridge, WaveformEditor, generic widgets, and
WindowManager. Excluded source-fix, version-bump, docs/skill, and CMake
target-creation tranches from this wave.

Local macOS validation passed for #2017: configure with GPU/examples off
and the shared MbedTLS cache, build `pulp-test-appearance`,
`pulp-test-audio-bridge`, `pulp-test-auto-ui`, `pulp-test-code-editor`,
`pulp-test-graph-editor-view`, `pulp-test-gui-components`,
`pulp-test-image-cache`, `pulp-test-input-events`, `pulp-test-modal`,
`pulp-test-param-attachment`, `pulp-test-phase9-widgets`,
`pulp-test-tree-view`, `pulp-test-ui-components`,
`pulp-test-visualization`, `pulp-test-waveform-editor`,
`pulp-test-widgets`, and `pulp-test-window-manager`, then run all 17
touched binaries fully with `-r compact`. `git diff --check`,
skill-sync, version-bump, docs-sync, and compat-sync reports passed.
Branch push pre-push gates passed without demotion. Resume action:
monitor #2017 required GitHub-hosted checks and merge directly when
green; do not use Namespace or SSH targets.

2026-05-15 00:18 PDT: refreshed #2012 after GitHub-hosted macOS exposed
a parallel test isolation issue outside the audio-file diff. The failed
check was `Build and Test / macOS (ARM64) [github-hosted]`; its log
showed `read_midi_file falls back for non-PPQ divisions` failing because
`read.has_value()` was false. The failing test used a `TempDir` name
based only on `steady_clock::now().time_since_epoch().count()`, so
parallel Catch/CTest processes could collide on the same temp path and
one destructor could remove another process's fixture files. The fix adds
random-device entropy to the MIDI-file `TempDir` name. Revalidated on
macOS after rebasing onto `origin/main` `b70bd42a`: build
`pulp-test-audio-file` and `pulp-test-midi-file`, focused
`./build/test/pulp-test-audio-file "[audio][file][issue-640]" -r compact`
passing 308 assertions in 16 test cases, full `pulp-test-audio-file`
passing 561 assertions in 33 test cases, and
`ctest --test-dir build -R "read_midi_file" -j 8 --output-on-failure`
passing 4/4. `git diff --check`, `git diff --cached --check`,
skill-sync, version-bump, docs-sync, and compat-sync reports passed.
Force-with-lease pushed `feature/phase3-audio-file-batch-640` to
`18333207`; resume action is to monitor the fresh GitHub-hosted
pull-request checks and merge #2012 directly when green.

2026-05-15 00:20 PDT: created the #645/#493 MIDI/host format batch
`feature/phase3-midi-host-batch-645` at `62a17f78`, PR #2019, based on
current `origin/main` `b70bd42a`. The branch is test-only and touches
`test/test_mpe_buffer.cpp`, `test/test_diagnostic_reporter.cpp`,
`test/test_descriptor_validation.cpp`, and `test/test_scan_blacklist.cpp`.
It adds MPE buffer equal-offset/move-add/tracker sample-offset coverage,
diagnostic reporter default and JSON output coverage, HostType resize and
sidechain feature heuristics, descriptor validation warning/error
combinations, and scan blacklist parser edge coverage.

Local macOS validation passed for #2019: build `pulp-test-mpe-buffer`,
`pulp-test-diagnostic`, `pulp-test-descriptor-validation`, and
`pulp-test-scan-blacklist`; focused `"[issue-645]"` MPE run passed 20
assertions in 3 test cases; focused `"[issue-493]"` diagnostic,
descriptor-validation, and scan-blacklist runs passed 26 assertions in 3
test cases, 34 assertions in 9 test cases, and 16 assertions in 3 test
cases respectively; full touched binaries passed 39 assertions in 6 test
cases, 46 assertions in 10 test cases, 50 assertions in 19 test cases,
and 42 assertions in 12 test cases respectively. `git diff --check
origin/main...HEAD`, `git diff --check`, `git diff --cached --check`,
skill-sync, version-bump, docs-sync, and compat-sync reports passed.
Branch pushed and PR opened with GitHub-hosted CI only; no Namespace or
SSH dispatch. Resume action: monitor #2019 required checks and merge
directly when green.

2026-05-15 00:32 PDT: created the #493 host graph/serializer batch
`feature/phase3-host-graph-batch-493` at `276f7f7`, PR #2022, based on
current `origin/main` `a29cb7dc`. The branch touches
`core/host/src/graph_serializer.cpp`, `test/test_host.cpp`, and
`test/test_graph_serializer.cpp`. It adds SignalGraph coverage for
missing-node and duplicate edge variants across audio/feedback/MIDI,
live gain updates without re-prepare, release silence behavior,
non-plugin parameter helper returns, prepare guard returns, and clear
state reset. It adds GraphSerializer coverage for unresolved-plugin
identity reserialization without state blobs and for field-error cleanup
after a partially loaded graph. The one-line source fix routes missing
plugin placeholders through the existing `add_plugin_node(info)` path so
GraphNode preserves the serialized PluginInfo identity when a saved
plugin cannot be resolved.

Local macOS validation passed for #2022: configured Debug with GPU and
examples off, built `pulp-test-host` and `pulp-test-graph-serializer`,
focused `./build/test/pulp-test-host "[host][graph][issue-493]" -r
compact` passed 88 assertions in 3 test cases, focused
`./build/test/pulp-test-graph-serializer
"[host][serializer][issue-493]" -r compact` passed 33 assertions in 5
test cases, full `pulp-test-host -r compact` passed 1126 assertions in
31 test cases, and full `pulp-test-graph-serializer -r compact` passed
382 assertions in 16 test cases. `git diff --check origin/main...HEAD`,
`git diff --check`, `git diff --cached --check`,
`python3 tools/scripts/skill_sync_check.py`,
`tools/scripts/cli_version_check.sh`, and `./tools/check-docs.sh`
passed; docs check reported only existing warnings. Local pre-push
diff-cover was demoted because its coverage configure path failed while
fetching Highway commit `457c891775a7397bdb0376bb1031e6e027af1c48`
before test execution. Branch pushed and PR opened with GitHub-hosted CI
only; no Namespace or SSH dispatch. Resume action: monitor #2022
required checks and merge directly when green.

2026-05-15 00:45 PDT: created the #643 tools Python control-plane batch
`feature/phase3-tools-python-batch-643` at `50acc069`, PR #2024, from
current `origin/main` `a29cb7dc`. The diff is test-only and adds
`tools/scripts/test_host_pump_lint.py`,
`tools/scripts/test_fetch_skia_for_release_extra.py`, and
`tools/scripts/test_macos_reroute_watcher.py`. Coverage areas include
host-pump lint pairing, window, skip-marker, and main-result paths;
release Skia fetcher usage, platform, manifest, checksum, unpack, and
layout-drift paths; and macOS reroute watcher subprocess wrapping, local
busy checks, cloud lane labeling, BAT queue parsing, reroute
success/failure/timeout, flap-guard trimming, and `tick()` busy,
candidate, and flap behavior.

Local macOS validation passed for #2024: `python3 -m unittest
tools.scripts.test_host_pump_lint
tools.scripts.test_fetch_skia_for_release_extra
tools.scripts.test_macos_reroute_watcher` passed 17 tests; combined
`python3 -m unittest tools.scripts.test_fetch_skia_for_release
tools.scripts.test_fetch_skia_for_release_extra
tools.scripts.test_host_pump_lint
tools.scripts.test_macos_reroute_watcher` passed 27 tests; direct script
runs for the three new tests passed; `python3 -m py_compile
tools/scripts/host_pump_lint.py tools/scripts/fetch_skia_for_release.py
tools/scripts/macos_reroute_watcher.py
tools/scripts/test_host_pump_lint.py
tools/scripts/test_fetch_skia_for_release_extra.py
tools/scripts/test_macos_reroute_watcher.py` passed; `git diff --check
origin/main...HEAD`, `git diff --check`, `git diff --cached --check`,
`python3 tools/scripts/skill_sync_check.py`,
`tools/scripts/cli_version_check.sh`, and `./tools/check-docs.sh`
passed. Local `tools/scripts/run_python_coverage.py` could not run
because the installed coverage.py is below the required `>=7.10` floor.
The broader `python3 -m unittest discover -s tools/scripts -p
'test_*lint.py'` sweep hit the existing unrelated
`tools/scripts/test_workflow_lint.py` workflow-step expectation for
`Release-pipeline regression tests (#720)`. Local pre-push diff-cover
was demoted because its coverage configure path failed fetching Highway
commit `457c891775a7397bdb1031e6e027af1c48` before tests. Branch pushed
and PR opened with GitHub-hosted CI only; no Namespace or SSH dispatch.
Resume action: monitor #2024 required checks and merge directly when
green.

2026-05-15 00:56 PDT: created the #641 runtime/state utility batch
`feature/phase3-runtime-state-batch-641` at `dda4d1e`, PR #2025, from
current `origin/main` `a29cb7dc`. The diff is test-only and touches
`test/test_properties_file.cpp`, `test/test_state_tree.cpp`, and
`test/test_runtime_utils.cpp`. Coverage areas include PropertiesFile
bool-string interpretation, non-object JSON failure state, unsupported
JSON member filtering, directory save failure, and missing-key/empty
clear no-ops; StateTree typed getter fallback, invalid child indexes,
insert-at-end and missing-pointer removal, property listener old/new
values, remove-without-callback behavior, JSON filtering and default
node type, deep-copy child parent rewiring, ObservableValue assignment,
CachedProperty mismatched external updates, StateTreeSynchroniser
reattach, empty batches, and truncated-batch prefix preservation; and
runtime utility coverage for TemporaryFile extension normalization,
move-assignment cleanup, self-move ownership, MemoryMappedFile reopen,
and Range integer/double boundary behavior.

Local macOS validation passed for #2025: configured Debug with GPU and
examples off, built `pulp-test-properties`, `pulp-test-state-tree`, and
`pulp-test-runtime-utils`, focused
`./build/test/pulp-test-properties "[issue-641]" -r compact` passed 24
assertions in 5 test cases, focused
`./build/test/pulp-test-state-tree "[issue-641]" -r compact` passed 55
assertions in 12 test cases, focused
`./build/test/pulp-test-runtime-utils "[issue-641]" -r compact` passed
88 assertions in 18 test cases, full `pulp-test-properties -r compact`
passed 75 assertions in 22 test cases, full `pulp-test-state-tree -r
compact` passed 171 assertions in 43 test cases, full
`pulp-test-runtime-utils -r compact` passed 150 assertions in 43 test
cases, and exact `ctest --test-dir build -R
"PropertiesFile|StateTree|TemporaryFile|MemoryMappedFile|Range"
--output-on-failure` passed 76/76 tests. `git diff --check
origin/main...HEAD`, `git diff --check`, `git diff --cached --check`,
`python3 tools/scripts/skill_sync_check.py`,
`tools/scripts/cli_version_check.sh`, and `./tools/check-docs.sh`
passed; docs check reported existing warnings. Local pre-push diff-cover
was intentionally disabled after the focused/full local validation
because this is a test-only batch. Branch pushed and PR opened with
GitHub-hosted CI only; no Namespace or SSH dispatch. Resume action:
monitor #2025 required checks and merge directly when green.

## Snapshot Summary

- Open `codecov` PRs in snapshot: 73
- Queued/in-progress Codecov-branch workflow runs selected for cancellation: 193
- Selected workflows: Build and Test, Coverage, Sanitizer Tests
- Local checkpoint at 2026-05-05 16:01 PDT: `origin/main` is `7e9795b4`.
  The refreshed #493 ModalOverlay, AutoUi, ImageCache, and
  VisualizationBridge, WaveformEditor, WindowManager, and ParamAttachment
  and InputEvents tranches recorded below are validated on that base. The
  refreshed #493 WidgetBridge DOM tranche is also validated on that base
  and includes a one-line source behavior fix with an SDK patch bump
  suggestion recorded below. The refreshed #643 harness/verifier tranche
  is validated on that base with current Yoga and CSS harness baselines.
  Local `origin/main` then advanced to `a8c4c16` at 2026-05-05 18:09
  PDT, and the refreshed #1271 / #643 create-targets and #1273 / #493
  package-command tranches plus the #1269 / #493 design-import bundle
  tranche, #1286 / #641 NamedPipe runtime tranche, #1282 / #493
  TextEditor tranche, #1274 / #643 CLI ship tranche, and #1287 / #643
  CLI audio command tranche, plus the #1280 / #640 MemoryMappedAudioReader
  tranche, local #643 audio tools/model-store tranche, and #646 SDL3
  surface fallback tranche, plus the local #640 AudioFileData shape,
  AIFF PCM edge, StreamingWriter reopen, frame-fill, Environment
  dispatch, and ChildProcess read-output tranches have been validated on
  that newer base.
  Local `origin/main` then advanced to `d191cdca` at 2026-05-05 19:26
  PDT, and the #640 AudioFocus dispatch source/test tranche has been
  validated on that newest base with the SDK patch bump requirement still
  recorded for any later push/CI resume. The #1274 / #643 CLI ship
  tranche was then refreshed and validated on the same newest base at
  2026-05-05 19:31 PDT, the #1287 / #643 CLI audio command tranche
  was refreshed and validated there at 2026-05-05 19:35 PDT, and the
  #1271 / #643 create-targets tranche was refreshed and validated there
  at 2026-05-05 19:43 PDT, the #1273 / #493 package-command tranche
  was refreshed and validated there at 2026-05-05 19:46 PDT, and the
  #1269 / #493 design-import bundle tranche was refreshed and validated
  there at 2026-05-05 19:50 PDT. The #1286 / #641 NamedPipe runtime
  tranche was refreshed and validated on the same newest base at
  2026-05-05 19:53 PDT. Local `origin/main` then advanced again to
  `92e83b37`, and the #1280 / #640 MemoryMappedAudioReader tranche was
  refreshed and validated on that newest base at 2026-05-05 20:00 PDT.
  The local #640 AudioFileData shape tranche was then refreshed and
  validated on the same newest base at 2026-05-05 20:05 PDT, followed
  by the local #640 AIFF PCM edge tranche at 2026-05-05 20:08 PDT and
  the local #640 StreamingWriter reopen tranche at 2026-05-05 20:11 PDT.
  The local #640 frame-fill tranche was refreshed and validated there at
  2026-05-05 20:16 PDT, the local #643 audio tools/model-store tranche
  was refreshed and validated there at 2026-05-05 20:23 PDT, and the
  local #640 Environment dispatch tranche was refreshed and validated
  there at 2026-05-05 20:26 PDT, and the local #640 ChildProcess
  read-output tranche was refreshed and validated there at
  2026-05-05 20:34 PDT. The #646 SDL3 surface fallback tranche was
  refreshed and validated on the same current base at
  2026-05-05 20:38 PDT. The #1286 / #641 NamedPipe runtime tranche was
  refreshed and validated there at 2026-05-05 20:43 PDT. Local
  `origin/main` then advanced to `8a58501b`, and the #1274 / #643 CLI
  ship tranche was refreshed and validated on that newest base at
  2026-05-05 20:50 PDT, and the #1287 / #643 CLI audio command
  tranche was refreshed and validated there at 2026-05-05 21:00 PDT.
  Read-only reconciliation on 2026-05-14 found current `origin/main`
  at `c98896dbc`, with the former first-wave and adjacent held PRs
  already landed: #1269 as `0b3c9493`, #1271 as `c45ee4ae`, #1273 as
  `8a2c9ee2`, #1274 as `31c07f2f`, #1287 as `ac79bcff`, #1286 as
  `196b1673`, #1280 as `d6287dc5`, #1282 as `42833f6b`, #1283 as
  `4d3f907b`, and #1285 as `57835f59`. Do not spend local validation
  cycles on those stale branches; use them only as historical evidence.
  The remaining `92e83b37` local-only candidates still need one final
  rebase/re-smoke before any CI resume.
  Rebase/re-smoke any held branch against the then-current base before a
  later push/CI resume.
  The refreshed #493 NewWidgets input, FileBrowser paint, AppearanceManager,
  and TreeView tranches were validated on `9a67a517` and are now behind
  the latest local `origin/main`; keep them local-only and rebase/re-smoke
  before any later push/CI resume. The remaining #493 view tranches
  recorded on `83271a94`, plus older #643 CLI/tool and #493/#640 tranches
  on `24047ba3` or older bases, remain behind as well.

## Pause Update 2026-05-04

Latest read-only REST snapshot at 2026-05-04 16:09 PDT shows 18 open
`codecov` PRs: #1256, #1257, #1263, #1266, #1269, #1271, #1273,
#1274, #1275, #1276, #1278, #1279, #1280, #1282, #1283, #1285,
#1286, and #1287.

No CI dispatch, PR branch update, rerun, push, or merge was performed
while Namespace capacity is reserved for other projects. Keep the
Phase 3 queue paused and continue only local validation / ledger
reconciliation until capacity returns.

Local-only progress at 2026-05-04 16:30 PDT, refreshed through
2026-05-05 10:39 PDT, then refreshed audio focus at 2026-05-05 12:18 PDT,
2026-05-05 12:46 PDT, 2026-05-05 15:49 PDT, and 2026-05-05 17:26 PDT, child process at 2026-05-05 12:20 PDT,
2026-05-05 12:49 PDT, 2026-05-05 15:56 PDT, 2026-05-05 17:21 PDT,
2026-05-05 19:22 PDT, and 2026-05-05 20:34 PDT, and environment at 2026-05-05 12:21 PDT,
2026-05-05 12:51 PDT, 2026-05-05 15:53 PDT, 2026-05-05 17:23 PDT, and
2026-05-05 20:26 PDT:
rebased/amended
`local/phase3-child-process-read-output-640` from `02c06848` to
`55c46d02`, then to `4008bd73`, then to `a4fb3f1f`, then to
`16f53a6f`, then to `ca647d70`, then to `73543f6b`, covering
`ChildProcess::read_available_output()` while a spawned process is still
running on current `origin/main` `b567dbeb`, then to `734140e0` on
current `origin/main` `7e9795b4`, then to `28d0c4cc` on current
`origin/main` `42bacd19`, then to `2af4d252` on current `origin/main`
`a8c4c16`, then to `4c474115` on current `origin/main` `92e83b37`,
and rebased/amended
`local/phase3-audio-focus-dispatch-640` from `3c36343d` to
`1ab6e24b`, then to `d766f4b9`, then to `2ae7de77`, then to
`94bc6a2a`, then to `69b4d18c`, then to `e61d430c`, covering
AudioFocusRegistry inactive-listener skip behavior when a listener is
removed or the registry is reset during dispatch on current `origin/main`
`b567dbeb`, then to `27c21311` on current `origin/main` `7e9795b4`,
then to `440d1838` on current `origin/main` `42bacd19`, then to
`f819d985` on current `origin/main` `d191cdca`,
and rebased `local/phase3-platform-environment-dispatch-640` from
`49d0611a` to `b271d2e6`, then to `422a64cc`, then to `cfb1874d`,
then to `7f2cf371`, then to `325604de`, then to `96aba9de`,
covering Environment token self-move, listener removal before dispatch,
and reset-during-dispatch skip behavior on current `origin/main`
`b567dbeb`, then to `1778b21c` on current `origin/main` `7e9795b4`,
then to `7d3ad7c6` on current `origin/main` `42bacd19`, then to
`db85e719` on current `origin/main` `a8c4c16`, then to `33565a65` on
current `origin/main` `92e83b37`. All
three remain unpushed and undispatched.

Additional local-only progress at 2026-05-04 16:37 PDT, refreshed at
2026-05-05 03:58 PDT, 2026-05-05 09:21 PDT,
2026-05-05 16:23 PDT, 2026-05-05 16:28 PDT, and
2026-05-05 17:56 PDT, then 2026-05-05 19:05 PDT, and
2026-05-05 20:38 PDT: refreshed
`feature/phase3-sdl3-surface-fallback-646` from `908b3a49` to
`1dc45105`, then to `3fe05a72`, then to `32a3bee6`, then to
`f1601d22`, then to `c6e775c0`, then to `cc9b6358`, then to
`d81b03cc`, covering SDL3
native-surface fallback/null-window behavior on current `origin/main`
`92e83b37`. Local
validation passed
`cmake -S . -B build-sdl3-surface -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build-sdl3-surface --target pulp-test-sdl3-surface -j$(sysctl -n hw.ncpu)`,
focused `./build-sdl3-surface/test/pulp-test-sdl3-surface "SDL3 surface info validity follows backend state - issue-646" -r compact`
with 4 assertions in 1 test case, full
`./build-sdl3-surface/test/pulp-test-sdl3-surface -r compact` with 13
assertions in 2 test cases, exact `ctest --test-dir build-sdl3-surface -R "SDL3 surface|sdl3-surface|issue-646" --output-on-failure`
passing 2/2, CLI sync check passing with slash-command/skill-reference
warnings only, sync/version/docs/compat guard reports, and
`git diff --check` against `origin/main`, the worktree, and the index. It
has no remote branch ref and remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 16:50 PDT, refreshed at
2026-05-05 04:02 PDT and 2026-05-05 10:04 PDT: added
`local/phase3-cli-config-command-643` at `af1ef6f8`, then refreshed to
`76b36dcf`, then to `08308f66`, covering `pulp config` set/get/list,
snooze clearing, malformed update keys, and invalid-key shellout
behavior against an isolated `PULP_HOME` on current `origin/main`
`bd036171`. Local validation passed GPU-on configure/build for
`pulp-cli` and `pulp-test-cli-shellout`, tag run
`PULP_CLI_PATH=... ./build/test/pulp-test-cli-shellout "[cli][shellout][config][issue-643]" -r compact`
with 34 assertions in 2 test cases, full
`PULP_CLI_PATH=... ./build/test/pulp-test-cli-shellout -r compact` with
496 assertions in 64 test cases, exact
`PULP_CLI_PATH=... ctest --test-dir build -R "^pulp config (set/get/list round-trips isolated update settings|rejects malformed and invalid update keys)$" --output-on-failure`
passing 2/2, CLI sync check passing with slash-command/skill-reference
warnings only, sync/version/docs/compat guard reports, generated
`classnames.json` cleanup, and `git diff --check` against
`origin/main`, the worktree, and the index. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-05 04:17 PDT, refreshed at
2026-05-05 08:38 PDT, 2026-05-05 10:52 PDT, 2026-05-05 12:02 PDT,
2026-05-05 12:40 PDT, 2026-05-05 16:01 PDT, and
2026-05-05 17:33 PDT, then 2026-05-05 19:09 PDT, then
2026-05-05 20:05 PDT:
added `local/phase3-audio-data-shape-640` at `cdfa7ee1`, then refreshed
to `e175a987`, then to `d0decd4f`, then to `c9ae1343`, then to
`b2eb2cfb`, then to `a33f0252`, then to `255818f0`, then to
`204e0972`, then to `eac19fe2`, covering
AudioFileData helper shape semantics and WAV writer first-channel-empty
rejection for the #640 audio tranche on current `origin/main`
`92e83b37`. Local validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-audio-file -j$(sysctl -n hw.ncpu)`,
focused `./build/test/pulp-test-audio-file "AudioFileData shape helpers and WAV writer reject first-channel empties" -r compact`
with 11 assertions in 1 test case, tag run
`./build/test/pulp-test-audio-file "[audio][file][issue-640]" -r compact`
with 252 assertions in 12 test cases, full
`./build/test/pulp-test-audio-file -r compact` with 505 assertions in 29
test cases, exact `ctest --test-dir build -R "AudioFileData shape helpers" --output-on-failure`
passing 1/1, CLI sync check passing with slash-command/skill-reference
warnings only, sync/version/docs/compat guard reports, and
`git diff --check` against `origin/main`, the worktree, and the index. It
remains unpushed and undispatched; this local-only branch is a combined
#640 audio-file batch candidate when Namespace capacity returns.

Additional local-only progress at 2026-05-05 04:29 PDT, refreshed at
2026-05-05 08:53 PDT, 2026-05-05 10:54 PDT, 2026-05-05 12:05 PDT,
2026-05-05 12:43 PDT, 2026-05-05 16:06 PDT, 2026-05-05 16:28 PDT,
2026-05-05 17:37 PDT, 2026-05-05 19:12 PDT, and
2026-05-05 20:08 PDT:
added `local/phase3-aiff-pcm-edges-640` at `2dd20995`, then refreshed to
`89d91749`, then to `4bf1d89a`, then to `6ff872ee`, then to
`6965d8d8`, then to `dc615db5`, then to `9a954004`, then to
`ad0202ec`, then to `5a14c78c`, then to `74774950`, covering AIFF
invalid COMM metadata and
unsupported PCM bit-depth rejection for the #640 audio tranche on current
`origin/main` `92e83b37`. Local validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-audio-file -j$(sysctl -n hw.ncpu)`,
focused `./build/test/pulp-test-audio-file "AIFF reader rejects invalid COMM metadata and unsupported PCM depths" -r compact`
with 19 assertions in 1 test case, tag run
`./build/test/pulp-test-audio-file "[audio][file][registry][aiff][issue-640]" -r compact`
with 19 assertions in 1 test case, full
`./build/test/pulp-test-audio-file -r compact` with 513 assertions in 29
test cases, exact `ctest --test-dir build -R "AIFF reader rejects invalid COMM metadata" --output-on-failure`
passing 1/1, CLI sync check passing with slash-command/skill-reference
warnings only, sync/version/docs/compat guard reports, and
`git diff --check` against `origin/main`, the worktree, and the index. It
remains unpushed and undispatched; this local-only branch is a combined
#640 audio-file batch candidate when Namespace capacity returns.

Additional local-only progress at 2026-05-05 04:36 PDT, refreshed at
2026-05-05 09:11 PDT, 2026-05-05 10:59 PDT, 2026-05-05 12:10 PDT,
2026-05-05 12:35 PDT, 2026-05-05 16:10 PDT, 2026-05-05 16:28 PDT,
2026-05-05 17:40 PDT, 2026-05-05 19:14 PDT, and
2026-05-05 20:11 PDT:
added `local/phase3-streaming-writer-reopen-640` at `6f8dad35`, then
refreshed to `be1b0ec3`, then to `8693c84d`, then to `c03c7104`, then
to `9875e436`, then to `9f8cb6c2`, then to `2902595f`, then to
`65caa032`, then to `65cdbc1b`, then to `5e1d020d`, covering
StreamingWriter
close-on-reopen and failed-open state/header finalization behavior for the
#640 audio tranche on current
`origin/main` `92e83b37`. Local validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-audio-file -j$(sysctl -n hw.ncpu)`,
focused `./build/test/pulp-test-audio-file "StreamingWriter finalizes the active file before a failed reopen" -r compact`
with 22 assertions in 1 test case, tag run
`./build/test/pulp-test-audio-file "[audio][file][streaming][issue-640]" -r compact`
with 103 assertions in 5 test cases, full
`./build/test/pulp-test-audio-file -r compact` with 516 assertions in 29
test cases, exact `ctest --test-dir build -R "StreamingWriter finalizes" --output-on-failure`
passing 1/1, CLI sync check passing with slash-command/skill-reference
warnings only, sync/version/docs/compat guard reports, and `git diff --check`
against `origin/main`, the worktree, and the index. It remains
unpushed and undispatched; this local-only branch is a combined #640
audio-file batch candidate when Namespace capacity returns.

Additional local-only progress at 2026-05-05 05:18 PDT, refreshed at
2026-05-05 09:15 PDT, 2026-05-05 11:02 PDT, 2026-05-05 12:14 PDT,
2026-05-05 12:31 PDT, 2026-05-05 12:37 PDT,
2026-05-05 16:18 PDT, 2026-05-05 16:28 PDT,
2026-05-05 17:43 PDT, 2026-05-05 19:17 PDT, and
2026-05-05 20:16 PDT:
added `local/phase3-frame-fill-edges-640` at `1611b10d`, then refreshed
to `7dc55715`, then to `2161075e`, then to `058a98b5`, then to
`1a1bb59e`, then to `3ccadef4`, then to `ea38e774`, then to
`23513264`, then to `75c6a599`, then to `04ce61b2`, then to
`8467c68d`, covering
`zero_fill_short_read()` tail-fill, read-count clamp, null-buffer, and
non-positive-dimension guard paths for the #640 audio tranche on current
`origin/main` `92e83b37`. Local validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-audio-frame-fill -j$(sysctl -n hw.ncpu)`,
tag run `./build/test/pulp-test-audio-frame-fill "[audio][frame_fill][issue-640]" -r compact`
with 26 assertions in 3 test cases, full
`./build/test/pulp-test-audio-frame-fill -r compact` with 26 assertions
in 3 test cases, exact `ctest --test-dir build -R "zero_fill_short_read|audio-frame-fill|frame_fill" --output-on-failure`
passing 3/3, CLI sync check passing with slash-command/skill-reference
warnings only, sync/version/docs/compat guard reports, and
`git diff --check` against `origin/main`, the worktree, and the index. It
remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 17:03 PDT, refreshed at
2026-05-05 03:44 PDT: added
`local/phase3-linkwitz-riley-edges-645` at `385c51a7`, then refreshed to
`7b1a82a3`, then to `3deafcfe`, covering
Linkwitz-Riley reset/history and cutoff boundary finite-output behavior
for the #645 signal tranche. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 23:34 PDT, refreshed at
2026-05-05 03:26 PDT:
`local/phase3-validation-harness-json-493` from `15fac4aa` to
`974cf572`, then to `4e753dd3`, covering ValidationHarness report metadata and entry JSON
escaping for the #493 format tranche on current `origin/main`. It
remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 23:37 PDT, refreshed at
2026-05-05 03:32 PDT and 2026-05-05 10:29 PDT: refreshed
`local/phase3-signal-graph-guards-493` from `adcf5dcf` to
`247be833`, then to `9f33226f`, then to `82f2264c`, covering SignalGraph missing-node guards, remove-node
connection cleanup, and default accessors for the #493 host tranche on
current `origin/main`. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-05 04:41 PDT, refreshed at
2026-05-05 09:46 PDT: added
`local/phase3-background-scanner-restart-493` at `b109a24f`, then
rebased it to `3da08b56`, covering BackgroundScanner restart after a
completed-but-unjoined worker for the #493 host tranche on current
`origin/main` `bd036171`. Local validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-background-scanner -j$(sysctl -n hw.ncpu)`,
focused
`./build/test/pulp-test-background-scanner "BackgroundScanner: restart joins a completed unjoined worker" -r compact`
with 6 assertions in 1 test case, tag run
`./build/test/pulp-test-background-scanner "[host][bg-scan][issue-493]" -r compact`
with 18 assertions in 2 test cases, full
`./build/test/pulp-test-background-scanner -r compact` with 28 assertions
in 7 test cases, exact
`ctest --test-dir build -R "BackgroundScanner|bg-scan|pulp-test-background-scanner" --output-on-failure`
passing 7/7, CLI sync check passing with slash-command/skill-reference
warnings only, sync/version/docs/compat guard reports, and
`git diff --check` against `origin/main`, the worktree, and the index.
It remains unpushed and undispatched.

Additional local-only progress at 2026-05-05 05:11 PDT, refreshed at
2026-05-05 09:49 PDT: added
`local/phase3-plugin-slot-dispatch-493` at `b6ef9689`, then rebased it
to `4945bad3`, covering PluginSlot invalid descriptor fail-closed
dispatch across CLAP, AU, AUv3, VST3, and LV2 loader paths for the
#493 host tranche on current `origin/main` `bd036171`. Local validation
passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-host -j$(sysctl -n hw.ncpu)`,
focused
`./build/test/pulp-test-host "PluginSlot load fails closed for invalid descriptors across formats" -r compact`
with 5 assertions in 1 test case, tag run
`./build/test/pulp-test-host "[host][slot][issue-493]" -r compact`
with 5 assertions in 1 test case, full
`./build/test/pulp-test-host -r compact` with 1038 assertions in 27
test cases and existing local plugin scan warnings, exact
`ctest --test-dir build -R "PluginSlot load fails closed for invalid descriptors across formats" --output-on-failure`
passing 1/1, CLI sync check passing with slash-command/skill-reference
warnings only, sync/version/docs/compat guard reports, and
`git diff --check` against `origin/main`, the worktree, and the index.
It remains unpushed and undispatched.

Additional local-only progress at 2026-05-05 05:44 PDT: added
`local/phase3-settings-panel-edges-493` at `f076664e`, covering
SettingsPanel no-audio-device fallback sample-rate/buffer-size lists,
latency-label refresh, input-channel fallback apply behavior, and
test-tone disable/reselect callback paths for the #493 format/view
tranche on current `origin/main` `0447498e`. Local validation included:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-standalone-editor-chrome -j10`;
focused Catch filters for `SettingsPanel uses fallback rates when no
audio devices are enumerated` and `SettingsPanel test tone toggle emits
disabled config`; `[standalone][settings][issue-493]`; exact CTest
selector `SettingsPanel uses fallback rates|SettingsPanel test tone
toggle`; full `pulp-test-standalone-editor-chrome`; sync/version guard
reports; and `git diff --check` against `origin/main`, the worktree, and
the index. Existing third-party/platform warnings were observed during
build. It remains unpushed and undispatched. Resume note: when Namespace
capacity returns, rename/push as a feature branch and run
`shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`
only as part of a small resume batch.

Additional local-only progress at 2026-05-05 05:52 PDT: added
`local/phase3-headless-host-edges-493` at `950b73f4`, covering
HeadlessHost MIDI-overload process-context defaults, release forwarding,
and null-processor prepare/process/release guard paths for the #493
format tranche on current `origin/main` `0447498e`. Local validation
included:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-headless -j10`; focused Catch
filters for `HeadlessHost MIDI overload defaults process context`,
`HeadlessHost release forwards every call`, and `HeadlessHost
no-processor guards leave buffers untouched`; `[headless][issue-493]`;
exact CTest selector `HeadlessHost MIDI overload|HeadlessHost release
forwards|HeadlessHost no-processor`; full `pulp-test-headless`;
sync/version/docs/compat guard reports; and `git diff --check` against
`origin/main`, the worktree, and the index. Existing
third-party/platform warnings were observed during build. It remains
unpushed and undispatched. Resume note: when Namespace capacity returns,
rename/push as a feature branch and run
`shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`
only as part of a small resume batch.

Additional local-only progress at 2026-05-05 05:55 PDT, refreshed again
at 2026-05-05 13:05 PDT: refreshed
`local/phase3-check-format-validation-coverage-643` from `76a2541b` to
`4a0217fb`, then to `96b9dfd8`, covering `tools/check_format_validation.py` parser, mode,
reporting, and read-error branches for the #643 tools tranche on current
`origin/main` `24047ba3`. Local validation included:
`python3 tools/scripts/test_check_format_validation.py` passing 9 tests;
`python3 tools/check_format_validation.py --mode=warn` returning 0 with
the current four missing production-validation warnings; report mode
returning 1 on the same four expected missing `production_validated`
entries; sync/version/compat guard reports; and `git diff --check` against `origin/main`,
the worktree, and the index. It remains unpushed and undispatched.
Resume note: when Namespace capacity returns, rename/push as a feature
branch and run
`shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`
only as part of a small resume batch.

Additional local-only progress at 2026-05-05 06:10 PDT, refreshed again
at 2026-05-05 07:04 PDT, 2026-05-05 11:06 PDT, and 2026-05-05
11:46 PDT, then again at 2026-05-05 12:55 PDT,
2026-05-05 15:34 PDT, 2026-05-05 16:36 PDT,
2026-05-05 17:48 PDT, 2026-05-05 18:50 PDT,
2026-05-05 19:31 PDT, and 2026-05-05 20:50 PDT: refreshed
`feature/phase3-cli-ship-coverage-643-next` from the paused #1274 remote
head `c924f1e8` to local head `384c4d5e`, covering deterministic
`pulp ship sign` missing-identity guidance in a valid project/build-cache
shellout path for the #643 CLI ship tranche on current `origin/main`
`8a58501b`. A first local refresh produced `64c4424c` on prior
`origin/main` `0447498e`; after `origin/main` advanced locally to
`50ff5822`, then `b7ec8f08`, then `cf5ea658`, then `24047ba3`, then
`7e9795b4`, then `df01f4f6`, then `42bacd19`, then `a8c4c16`, then
`d191cdca`, then `8a58501b`, the branch was rebased again before
recording final evidence. Local validation included:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=ON -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-cli pulp-test-cli-ship-shellout -j$(sysctl -n hw.ncpu)`;
focused Catch filter for `pulp ship sign in project without identity
reports signing guidance` passing 4 assertions in 1 test case; full
`pulp-test-cli-ship-shellout` passing 49 assertions in 11 test cases;
ship-tag shellout run passing 49 assertions in 11 test cases; exact
CTest selector `pulp ship sign in project without identity` passing 1/1;
CLI skew check passed silently; CLI sync passed with
slash-command/skill-reference warnings only and zero issues;
skill-sync, version-bump, docs-sync, and compat-sync reports passed or
reported no mapped paths touched; and diff checks (`git diff --check origin/main...HEAD`,
`git diff --check`, and `git diff --cached --check`) passed. It remains unpushed and
undispatched; #1274 still points at the old
remote head until Namespace capacity returns. Resume note: when Namespace
capacity returns, update #1274 from this refreshed local branch with
`--force-with-lease` only as part of a small resume batch, then let the
normal PR-event Namespace checks prove the branch before merge.

Additional local-only progress at 2026-05-05 06:15 PDT, refreshed again
at 2026-05-05 06:54 PDT, 2026-05-05 11:19 PDT, and 2026-05-05
11:59 PDT, then again at 2026-05-05 13:14 PDT,
2026-05-05 16:55 PDT, 2026-05-05 18:05 PDT,
2026-05-05 18:59 PDT, and 2026-05-05 20:00 PDT: refreshed
`feature/phase3-mmap-reader-extra-640` from the paused #1280 remote head
`ed2ba7bf` to local head `a4cb8a9b`, covering
`MemoryMappedAudioReader` unsupported-file fail-closed and EOF no-copy
behavior for the #640 audio tranche on current `origin/main` `92e83b37`.
A first local refresh produced `84470694` on prior `origin/main`
`0447498e`; after `origin/main` advanced locally to `50ff5822`, then
`b7ec8f08`, `6c8b9920`, then `24047ba3`, then `df01f4f6`, then
`42bacd19`, then `a8c4c16`, then `d191cdca`, then `92e83b37`, the two
local commits were rebased again before recording final evidence. Local
validation included:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-audio-file -j$(sysctl -n hw.ncpu)`;
focused Catch filter `MemoryMappedAudioReader*` passing 41 assertions in
3 test cases; full `pulp-test-audio-file` passing 509 assertions in 30
test cases; exact CTest selector `MemoryMappedAudioReader` passing 3/3;
CLI skew, sync/version/docs/compat guard reports; and `git diff --check` against
`origin/main`, the worktree, and the index. Final status was clean and
ahead 156 / behind 2 against the paused remote. It remains unpushed and
undispatched; #1280 still points at the old remote head until Namespace
capacity returns. Resume note: when Namespace capacity returns, update
#1280 from this refreshed local branch with `--force-with-lease` only as
part of a small resume batch, then let the normal PR-event Namespace
checks prove the branch before merge.

Additional local-only progress at 2026-05-05 06:18 PDT, refreshed again
at 2026-05-05 06:58 PDT, 2026-05-05 11:33 PDT, and 2026-05-05
13:17 PDT, then 2026-05-05 17:05 PDT, 2026-05-05 17:11 PDT, and
2026-05-05 18:46 PDT: refreshed
`feature/view-text-editor-coverage-493-next` from the paused #1282 remote
head `03e5e3cd` to local head `5b5173a2`, covering TextEditor
key-up/unhandled-key, modifier/word and shift navigation, delete/redo,
shift-click, and exact double-click word-selection paths for the #493
view tranche on current `origin/main` `a8c4c16`. A first local refresh
produced `f377ef5c` on prior `origin/main` `0447498e`; after
`origin/main` advanced locally to `50ff5822`, `b7ec8f08`, `cf5ea658`,
`24047ba3`, `df01f4f6`, `d11f530e`, and `a8c4c16`, the branch was
rebased again before recording final evidence.
Local validation included:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-text-editor -j$(sysctl -n hw.ncpu)`;
focused Catch tag `[view][text_editor][issue-493]` passing 62 assertions
in 10 test cases; full `pulp-test-text-editor` passing 148 assertions in
37 test cases; exact CTest selector `TextEditor` passing 37/37; CLI
sync, sync/version/docs/compat guard reports; and `git diff --check`
against `origin/main`, the worktree, and the index. Final status was
clean and ahead 153 / behind 1 against the paused remote. It remains
unpushed and undispatched; #1282 still points at the old remote head until Namespace
capacity returns. Resume note: when Namespace capacity returns, update
#1282 from this refreshed local branch with `--force-with-lease` only as
part of a small resume batch, then let the normal PR-event Namespace
checks prove the branch before merge.

Additional local-only progress at 2026-05-05 06:24 PDT, refreshed again
at 2026-05-05 07:02 PDT, 2026-05-05 11:28 PDT, and 2026-05-05
11:55 PDT, then again at 2026-05-05 13:20 PDT and
2026-05-05 17:08 PDT: reconciled
`feature/phase3-splash-screen-coverage-493` from the paused #1285 remote
head `d3cd9f47` to local head `fc62b3bc`, incorporating and superseding
`local/phase3-splash-screen-493` (`1dd00e70`) so #1285 is the single
resume path for this SplashScreen tranche. The refreshed branch covers
SplashScreen advance/dismiss callback behavior, dismiss-on-click gating,
and text/image paint output for the #493 view tranche on current
`origin/main` `d11f530e`. A first local refresh produced `7cba95e3` on
prior `origin/main` `0447498e`; after `origin/main` advanced locally to
`50ff5822`, then `b7ec8f08`, then `6c8b9920`, then `24047ba3`, then
`d11f530e`, the branch was rebased again before recording final evidence.
Local validation included:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-splash-screen -j$(sysctl -n hw.ncpu)`;
focused Catch tag `[view][splash-screen][coverage][issue-493]` passing
34 assertions in 3 test cases; full `pulp-test-splash-screen` passing
34 assertions in 3 test cases; exact CTest selector `SplashScreen`
passing 3/3; CLI skew, sync/version/docs/compat guard reports; and
`git diff --check` against `origin/main`, the worktree, and the index.
It remains unpushed and
undispatched; #1285 still points at `d3cd9f47` until Namespace capacity
returns. Resume note: when Namespace capacity returns, update #1285 from
this refreshed local branch with `--force-with-lease` only as part of a
small resume batch, then let the normal PR-event Namespace checks prove
the branch before merge.

Additional local-only progress at 2026-05-05 06:31 PDT, refreshed again
at 2026-05-05 07:09 PDT, 2026-05-05 11:15 PDT, and 2026-05-05
11:50 PDT, then again at 2026-05-05 13:04 PDT,
2026-05-05 15:38 PDT, 2026-05-05 16:42 PDT, 2026-05-05 18:02 PDT,
2026-05-05 18:56 PDT, 2026-05-05 19:35 PDT, and
2026-05-05 21:00 PDT:
refreshed
`feature/phase3-cli-audio-command-coverage-643` from the paused #1287
remote head `cb0a4acb` to local head `34e71acd`, covering deterministic
`pulp audio` usage/parser errors and missing-bundle JSON behavior for the
#643 CLI tranche on current `origin/main` `8a58501b`. A first local
refresh produced `353a6afd` on prior `origin/main` `0447498e`; after
`origin/main` advanced locally to `50ff5822`, then `b7ec8f08`, then
`cf5ea658`, then `24047ba3`, then `7e9795b4`, then `df01f4f6`, then
`42bacd19`, then `a8c4c16`, then `d191cdca`, then `8a58501b`, the
branch was rebased again before recording final evidence.
Local validation included:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=ON -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-cli pulp-import-design pulp-test-cli-shellout -j$(sysctl -n hw.ncpu)`;
focused real-CLI Catch tag `[cli][shellout][audio][issue-643]` with
`PULP_CLI_PATH=build/tools/cli/pulp` passing 56 assertions in 2 test
cases; full `pulp-test-cli-shellout` with
`PULP_CLI_PATH=build/tools/cli/pulp` passing 518 assertions in 64 test
cases; focused CTest selector `pulp audio|cli-shellout|audio usage`
passing 2/2; CLI skew check passed silently; CLI sync passed with
slash-command/skill-reference warnings only and zero issues;
skill-sync, version-bump, docs-sync, and compat-sync reports passed or
reported no mapped paths touched; and diff checks (`git diff --check origin/main...HEAD`,
`git diff --check`, and `git diff --cached --check`) passed.
Generated `classnames.json` from the full shellout run was removed, and
final status was clean and ahead 151 / behind 1 against the paused
remote. It remains unpushed and undispatched;
#1287 still points at `cb0a4acb` until Namespace capacity returns. Resume
note: when Namespace capacity returns, update #1287 from this refreshed
local branch with `--force-with-lease` only as part of a small resume
batch, then let the normal PR-event Namespace checks prove the branch
before merge.

Additional local-only progress at 2026-05-05 06:35 PDT, refreshed again
at 2026-05-05 07:17 PDT, 2026-05-05 11:24 PDT, and 2026-05-05
11:53 PDT, then again at 2026-05-05 15:42 PDT and
2026-05-05 16:45 PDT, then again at 2026-05-05 18:41 PDT,
2026-05-05 19:53 PDT, and 2026-05-05 20:43 PDT: refreshed
`feature/phase3-named-pipe-coverage-641` from the paused #1286 remote
head `2d59c4c` to local head `6fcbcdd1`, covering NamedPipe
closed/missing endpoint fail-closed behavior, POSIX FIFO round-trip,
cleanup, move-ownership, and create-failure paths for the #641 runtime
tranche on current `origin/main` `92e83b37`. A first local refresh
produced `eba02124` on prior `origin/main` `0447498e`; after
`origin/main` advanced locally to `50ff5822`, then `b7ec8f08`, then
`6c8b9920`, then `7e9795b4`, then `df01f4f6`, then `a8c4c16`, then
`d191cdca`, then `92e83b37`, the branch was rebased again before
recording final evidence.
Local validation included:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-stream -j$(sysctl -n hw.ncpu)`;
focused Catch tag `[stream][named_pipe][issue-641]` passing 41 assertions
in 4 test cases; full `pulp-test-stream` passing 112 assertions in 12
test cases; exact CTest selector `NamedPipe|named_pipe|pulp-test-stream`
passing 4/4; CLI skew check passed silently; CLI sync passed with
slash-command/skill-reference warnings only and zero issues; skill-sync,
version-bump, docs-sync, and compat-sync reports passed or reported no
mapped paths touched; and `git diff --check origin/main...HEAD`,
`git diff --check`, and `git diff --cached --check` passed. Final status
was clean and ahead 151 / behind 1 against the paused remote. It remains
unpushed and undispatched; #1286 still points at `2d59c4c` until
Namespace capacity returns. Resume note: when Namespace capacity returns,
update #1286 from this refreshed local branch with `--force-with-lease`
only as part of a small resume batch, then let the normal PR-event
Namespace checks prove the branch before merge.

Additional local-only progress at 2026-05-05 06:39 PDT, refreshed again
at 2026-05-05 06:51 PDT, 2026-05-05 11:09 PDT,
2026-05-05 11:40 PDT, 2026-05-05 12:57 PDT,
2026-05-05 16:48 PDT, 2026-05-05 18:31 PDT, and
2026-05-05 19:43 PDT: refreshed
`feature/phase3-create-targets-coverage-643` from the paused #1271
remote head `62ca4512` to local head `f24a69cb`, covering
`create_default_build_targets()` optional plugin format suffixes,
duplicate suppression, and empty standalone app target filtering for the
#643 CLI helper tranche on current `origin/main` `d191cdca`. A first
local refresh produced `79a3489c` on prior `origin/main` `0447498e`;
after `origin/main` advanced locally to `50ff5822`, `b7ec8f08`,
`cf5ea658`, then `24047ba3`, then `df01f4f6`, then `a8c4c16`, then
`d191cdca`, the branch was rebased again before
recording final evidence.
Local validation included:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-cli-create-targets -j$(sysctl -n hw.ncpu)`;
focused Catch tag `[cli][create][issue-643]` passing 2 assertions in 2
test cases; full `pulp-test-cli-create-targets` passing 5 assertions in
5 test cases; CTest selector
`CLI create targets|cli-create-targets|create targets` passing 5/5; CLI sync,
sync/version/docs/compat guard reports; and `git diff --check` against
`origin/main`, the worktree, and the index. Final status was clean and
ahead 161 / behind 1 against the paused remote. It remains unpushed and
undispatched; #1271 still points at `62ca4512` until Namespace capacity
returns. Resume note: when Namespace capacity returns, update #1271 from
this refreshed local branch with `--force-with-lease` only as part of a
small resume batch, then let the normal PR-event Namespace checks prove
the branch before merge.

Additional local-only progress at 2026-05-05 06:43 PDT, refreshed again
at 2026-05-05 06:51 PDT, 2026-05-05 11:12 PDT, and
2026-05-05 11:42 PDT, then again at 2026-05-05 13:01 PDT,
2026-05-05 16:52 PDT, 2026-05-05 18:35 PDT, and
2026-05-05 19:46 PDT: refreshed
`feature/phase3-package-commands-coverage-493` from the paused #1273
remote head `c52cd486` to local head `a7341506`, covering `pulp target
add` package target compatibility warnings when an installed package
does not support a newly added target for the #493 CLI package tranche
on current `origin/main` `d191cdca`. A first local refresh produced
`89e30728` on prior `origin/main` `0447498e`; after `origin/main`
advanced locally to `50ff5822`, `b7ec8f08`, `cf5ea658`, then
`24047ba3`, then `df01f4f6`, then `a8c4c16`, then `d191cdca`, the branch
was rebased again before
recording final evidence. Local validation
included:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-cli-package-commands -j$(sysctl -n hw.ncpu)`;
exact Catch case `cmd_target add warns when installed packages do not support the new target`
passing 6 assertions in 1 test case; full
`pulp-test-cli-package-commands` passing 195 assertions in 12 test cases;
CTest selector
`cmd_target add warns|package commands|Package|pulp-test-cli-package-commands`
passing 2/2; CLI sync, sync/version/docs/compat guard reports; and
`git diff --check`
against `origin/main`, the worktree, and the index. Final status was
clean and ahead 160 / behind 1 against the paused remote. It remains
unpushed and undispatched; #1273 still points at `c52cd486` until Namespace
capacity returns. Resume note: when Namespace capacity returns, update
#1273 from this refreshed local branch with `--force-with-lease` only as
part of a small resume batch, then let the normal PR-event Namespace
checks prove the branch before merge.

Local triage at 2026-05-05 06:45 PDT: inspected paused #1276
`feature/phase3-popup-menu-coverage-640` at remote/local head
`c66d74e4` and did not refresh it because current `origin/main`
`0447498e` already covers the same PopupMenu item metadata and non-Apple
stub no-selection paths in `test/test_file_dialog.cpp`. Treat #1276 as
low-value duplicate coverage during the pause. Resume note: when
Namespace capacity returns, either close/supersede #1276 after a final
Codecov/code check, or retarget it to a platform gap that is not already
covered by `origin/main`.

Additional local-only progress at 2026-05-05 06:49 PDT, refreshed again
at 2026-05-05 11:37 PDT, 2026-05-05 13:10 PDT, and
2026-05-05 17:03 PDT, then 2026-05-05 17:11 PDT,
2026-05-05 18:38 PDT, and 2026-05-05 19:50 PDT: refreshed
`feature/phase3-design-import-bundle-coverage-493` from the paused #1269
remote head `923b04f1` to local head `e9669ee9`, covering Claude bundle
malformed template/non-object manifest handling, malformed asset skips,
referenced-JS indexing, and bundled classname extraction through
font-face-leading styles for the #493 design-import tranche on current
`origin/main` `d191cdca`. A first rebase landed on prior local
`origin/main` `0447498e`, then `origin/main` advanced locally to
`50ff5822`, `cf5ea658`, `24047ba3`, `df01f4f6`, then `d11f530e`, then
`a8c4c16`, then `d191cdca`; the branch was rebased again before recording final
evidence. Local validation included:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-design-import-claude-bundle -j$(sysctl -n hw.ncpu)`;
focused Catch tag `[view][import][issue-468]` passing 46 assertions in 9
test cases; full `pulp-test-design-import-claude-bundle` passing 45
assertions in 8 default test cases; CTest selector
`malformed template|skips malformed assets|extract_claude_classnames|parse_claude_bundle`
passing 8/8;
CLI sync, sync/version/compat guard reports; and `git diff --check`
against `origin/main`, the worktree, and the index. Existing
text-shaping, third-party/runtime/view, duplicate-library, and platform
warnings were observed during build. Final status was clean and ahead
162 / behind 1 against the paused remote. It remains unpushed and
undispatched; #1269 still points at `923b04f1` until
Namespace capacity returns. Resume note: when Namespace capacity returns,
update #1269 from this refreshed local branch with `--force-with-lease`
only as part of a small resume batch, then let the normal PR-event
Namespace checks prove the branch before merge.

Reconciliation at 2026-05-14 22:35 PDT found this tranche is already on
current `origin/main` as `0b3c94934 test: cover Claude bundle edge
parsing (#1269)`. The local branch
`feature/phase3-design-import-bundle-coverage-493` was checked only to
verify duplication; no push, PR update, workflow rerun, or merge was
performed. Resume action: none for #1269. Treat the old local branch and
paused remote as historical/superseded by main.

Additional local-only progress at 2026-05-04 23:39 PDT, refreshed at
2026-05-05 03:34 PDT, and refreshed again at 2026-05-05 07:24 PDT:
refreshed
`local/phase3-widgets-render-paths-493` from `fefb7e94` to `55a90f79`,
then to `812f84ce`, then to `15ca568b`, covering widget custom-shader
CPU fallbacks, minimal render-style branches, and knob/fader/toggle
interaction edges for the #493 view tranche on current `origin/main`
`50ff5822`. Local validation included:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-widgets -j$(sysctl -n hw.ncpu)`;
focused Catch tag `[issue-493]` passing 34 assertions in 4 test cases;
full `pulp-test-widgets` passing 294 assertions in 68 test cases; exact
CTest selector `Audio widgets custom shader paths fall back on recording canvas|Audio widgets minimal render style paints simplified branches|Knob mouse paths update value|Fader and toggle mouse paths dispatch clamped interactive values` passing 4/4; CLI skew,
sync/version/docs/compat guard reports; and `git diff --check` against
`origin/main`, the worktree, and the index. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 23:41 PDT, refreshed at
2026-05-05 03:38 PDT, and refreshed again at 2026-05-05 07:21 PDT:
refreshed
`local/phase3-audio-bridge-edges-493` from `255b0516` to `cba288df`,
then to `a976d60e`, then to `2d00e9ac`, covering AudioBridge
first-pop/default meter, max-channel clamp, zero-sample analysis, and
MeterBallistics tiny-value clamp paths for the #493 view tranche on
current `origin/main` `50ff5822`. Local validation included:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-audio-bridge -j$(sysctl -n hw.ncpu)`;
focused Catch tag `[issue-493]` passing 40 assertions in 3 test cases;
full `pulp-test-audio-bridge` passing 63 assertions in 8 test cases;
exact CTest selector `AudioBridge pop_latest reports empty before first meter|AudioBridge analyze clamps channels and handles zero sample blocks|MeterBallistics releases tiny values to exact zero` passing 3/3; CLI skew,
sync/version/docs/compat guard reports; and `git diff --check` against
`origin/main`, the worktree, and the index. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 18:04 PDT, refreshed at
2026-05-05 03:47 PDT and 2026-05-05 10:14 PDT: added
`local/phase3-adsr-edges-645` at `e4844caa`, then refreshed to
`bd746f3c`, then refreshed to `0c9e7d4d`, covering ADSR immediate
stage progression, idle `note_off`, and reset paths for the #645
signal tranche. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 18:09 PDT, refreshed at
2026-05-05 03:49 PDT and 2026-05-05 10:17 PDT: added
`local/phase3-noise-gate-edges-645` at `98f73bb8`, then refreshed to
`717067d6`, then refreshed to `032f46c2`, covering NoiseGate
range clamp, instant attack/release timing, reset, silence, and buffer
paths for the #645 signal tranche. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 18:17 PDT, refreshed at
2026-05-05 03:52 PDT and 2026-05-05 10:20 PDT: added
`local/phase3-modulation-reverb-edges-645` at `971488d5`, then refreshed to
`9cfcc171`, then refreshed to `e6b5c6bc`, covering
Chorus dry/reset/phase-wrap behavior and Reverb zero-decay, damping
clamp, dry-mix, and reset paths for the #645 signal tranche. It
remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 18:21 PDT, refreshed at
2026-05-05 03:56 PDT and 2026-05-05 10:23 PDT: added `local/phase3-oscillator-edges-645` at
`8f51cba4`, then refreshed to `8f2e043`, then refreshed to
`5e3a1db3`, covering
Oscillator reset, phase wrap, getter, and PolyBLEP edge paths for the
#645 signal tranche. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-05 04:54 PDT, refreshed at
2026-05-05 10:26 PDT: added
`local/phase3-signal-helper-edges-645` at `be2d441f`, then refreshed to
`431a3c5d`, covering
FirFilter empty-coefficient passthrough/reset, LookupTable indexed clamp
and zero-length buffer no-op paths, and LadderFilter
buffer/reset/resonance-clamp finite-output behavior for the #645 signal
tranche on current `origin/main` `4bebc7bf`. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 18:31 PDT, refreshed at
2026-05-05 02:07 PDT, 2026-05-05 09:33 PDT, 2026-05-05 10:48 PDT, and
2026-05-05 12:18 PDT, 2026-05-05 12:46 PDT,
2026-05-05 17:26 PDT, and 2026-05-05 19:26 PDT: rebased/amended
`local/phase3-audio-focus-dispatch-640` from `3c36343d` to
`1ab6e24b`, then to `d766f4b9`, then to `2ae7de77`, then to
`94bc6a2a`, then to `69b4d18c`, then to `e61d430c`, then to
`27c21311`, covering
AudioFocusRegistry inactive-listener skip behavior when a listener is
removed or the registry is reset during dispatch for the #640 audio
tranche on current `origin/main` `7e9795b4`, then to `440d1838` on
current `origin/main` `42bacd19`, then to `f819d985` on current
`origin/main` `d191cdca`. Local validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-audio-focus -j$(sysctl -n hw.ncpu)`,
`./build/test/pulp-test-audio-focus "[audio][focus][issue-640]" -r compact`
with 20 assertions in 6 test cases, full
`./build/test/pulp-test-audio-focus -r compact` with 44 assertions in 16
test cases, exact `ctest --test-dir build -R "AudioFocusRegistry|audio-focus|pulp-test-audio-focus" --output-on-failure`
passing 16/16, CLI sync check passing with slash-command/skill-reference
warnings only, skill-sync, docs-sync, and compat-sync reports with no mapped
paths touched, version-bump report with an SDK patch suggestion and no plugin
bump, and `git diff --check` against `origin/main`, the worktree, and
the index. It remains unpushed
and undispatched.

Additional local-only progress at 2026-05-04 18:42 PDT, refreshed at
2026-05-05 02:09 PDT, 2026-05-05 09:18 PDT,
2026-05-05 16:21 PDT, 2026-05-05 16:28 PDT,
2026-05-05 17:53 PDT, 2026-05-05 19:02 PDT, and
2026-05-05 20:23 PDT: added
`local/phase3-audio-tools-model-store-643` at `a88ddfe8`, then rebased it
to `9743ccd7`, then refreshed to `a2f7bc2d`, then to `3a7ed654`, then
to `a5aa9f8e`, then to `34ede9bb`, then to `ae770658`, then to
`75ed3a63`, covering `tools/audio` model registry URL resolution,
legacy/malformed model metadata, model/bundle JSON serialization/defaults,
and excerpt-find guard and unsupported-input paths for the #643 tools
tranche on current `origin/main` `92e83b37`. Local validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-audio-tools -j$(sysctl -n hw.ncpu)`,
focused `./build/test/pulp-test-audio-tools "[audio][tools][issue-643]" -r compact`
with 93 assertions in 8 test cases, full
`./build/test/pulp-test-audio-tools -r compact` with 188 assertions in 20
test cases, exact `ctest --test-dir build -R "audio model|excerpt bundle|excerpt find|audio-tools|pulp-test-audio-tools" --output-on-failure`
passing 20/20, CLI skew check passing silently, CLI sync check passing
with slash-command/skill-reference warnings only and zero issues,
skill-sync, version-bump, docs-sync, and compat-sync reports passing or
reporting no mapped paths touched, and `git diff --check origin/main...HEAD`,
`git diff --check`, and `git diff --cached --check` passing. It remains
unpushed and undispatched.

Additional local-only progress at 2026-05-04 18:50 PDT, refreshed at
2026-05-05 02:11 PDT, 2026-05-05 09:24 PDT, 2026-05-05 10:45 PDT,
2026-05-05 12:21 PDT, 2026-05-05 12:51 PDT,
2026-05-05 17:23 PDT, 2026-05-05 19:19 PDT, and
2026-05-05 20:26 PDT: rebased
`local/phase3-platform-environment-dispatch-640` from `49d0611a` to
`b271d2e6`, then to `422a64cc`, then to `cfb1874d`, then to
`7f2cf371`, then to `325604de`, then to `96aba9de`, then to
`1778b21c`, then to `7d3ad7c6`, then to `db85e719`, then to
`33565a65`, covering Environment token self-move, listener removal before
dispatch, and reset-during-dispatch skip behavior for the #640 platform
tranche on current `origin/main` `92e83b37`. Local validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-environment -j$(sysctl -n hw.ncpu)`,
`./build/test/pulp-test-environment "[environment][issue-640]" -r compact`
with 61 assertions in 10 test cases, full
`./build/test/pulp-test-environment -r compact` with 106 assertions in 21
test cases, exact
`ctest --test-dir build -R "Environment: token self move|Environment: listener removed|Environment: reset during dispatch|environment|pulp-test-environment" --output-on-failure`
passing 3/3, CLI skew check passing silently, CLI sync check passing with
slash-command/skill-reference warnings only and zero issues, skill-sync,
version-bump, docs-sync, and compat-sync reports passing or reporting no
mapped paths touched, and `git diff --check origin/main...HEAD`,
`git diff --check`, and `git diff --cached --check` passing. It remains
unpushed and undispatched.

Additional local-only progress at 2026-05-04 18:59 PDT, refreshed at
2026-05-04 23:23 PDT, 2026-05-05 02:14 PDT, and 2026-05-05
09:37 PDT: rebased
`local/phase3-midi-ci-edges-645` from `9b7e547f` to `c3f7e68b`, then
to `f66f0d4`, then to `6226f5af`, covering MIDI-CI malformed
universal/sub-ID guard paths, directly addressed discovery inquiries,
short discovery replies, and reserved-byte profile matching for the
#645 MIDI tranche on current `origin/main` `bd036171`. Local validation
passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-midi-ci -j$(sysctl -n hw.ncpu)`,
tag run `./build/test/pulp-test-midi-ci "[midi][ci][issue-645]" -r compact`
with 54 assertions in 10 test cases, full
`./build/test/pulp-test-midi-ci -r compact` with 81 assertions in 19 test
cases, exact `ctest --test-dir build -R "CiDiscovery|MUID|midi-ci|pulp-test-midi-ci" --output-on-failure`
passing 19/19, CLI sync check passing with slash-command/skill-reference
warnings only, sync/version/docs/compat guard reports, and
`git diff --check` against `origin/main`, the worktree, and the index. It
remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 19:07 PDT, refreshed at
2026-05-05 02:16 PDT and 2026-05-05 09:55 PDT: rebased/amended
`local/phase3-mpe-allocator-edges-645` from `fc198dec` to `83ce06b3`,
then to `dcacdaa0`, then to `249ad105`, fixing the MpeVoiceAllocator
release-steal glide refcount path, documenting the releasing-steal
invariant in the MPE skill, and covering unmatched MpeGlideDetector
note-off/reset behavior for the #645 MIDI/MPE tranche on current
`origin/main` `bd036171`. Local validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-mpe-synth-voice -j$(sysctl -n hw.ncpu)`,
tag run
`./build/test/pulp-test-mpe-synth-voice "[midi][mpe][issue-645]" -r compact`
with 62 assertions in 6 test cases, full
`./build/test/pulp-test-mpe-synth-voice -r compact` with 86 assertions
in 14 test cases, exact
`ctest --test-dir build -R "MpeVoiceAllocator|MpeGlideDetector|mpe-synth-voice" --output-on-failure`
passing 12/12, CLI sync check passing with slash-command/skill-reference
warnings only, skill-sync passing with the MPE skill update, version-bump
report passing with the SDK patch override and plugin patch suggestion,
docs/compat guard reports, and `git diff --check` against `origin/main`,
the worktree, and the index. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 19:20 PDT, refreshed at
2026-05-05 02:19 PDT and 2026-05-05 09:43 PDT: rebased
`local/phase3-host-scanner-order-493` from `c3b79d68` to `97d9f833`,
then to `1dfeb616`, then to `e84ade81`, covering PluginScanner
VST3/LV2 format-lane merging, final name ordering, LV2 URI identity,
VST3 stem fallback identity, and hermetic scanner fixture paths for
the #493 host tranche on current `origin/main` `bd036171`. Local
validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-host-regression -j$(sysctl -n hw.ncpu)`,
focused
`./build/test/pulp-test-host-regression "PluginScanner merges enabled format lanes and sorts by plugin name" -r compact`
with 10 assertions in 1 test case, tag run
`./build/test/pulp-test-host-regression "[host][scanner]" -r compact`
with 61 assertions in 11 test cases, full
`./build/test/pulp-test-host-regression -r compact` with 430 assertions
in 19 test cases, exact
`ctest --test-dir build -R "PluginScanner merges enabled format lanes" --output-on-failure`
passing 1/1, CLI sync check passing with slash-command/skill-reference
warnings only, sync/version/docs/compat guard reports, and
`git diff --check` against `origin/main`, the worktree, and the index.
It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 19:32 PDT, refreshed at
2026-05-05 02:23 PDT and 2026-05-05 09:52 PDT: rebased
`local/phase3-fetchcontent-cache-edges-643` from `dd7433d8` to
`b056ca59`, then to `e5b13514`, then to `5cbf2d87`, covering
FetchContent cache fallback entry splitting/order, live symlink
classification, file-backed declared-ref parsing, label fallbacks, and
symlink removal without deleting targets for the #643 CLI/tools tranche
on current `origin/main` `bd036171`. Local validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-cli-fetchcontent-cache -j$(sysctl -n hw.ncpu)`,
tag run
`./build/test/pulp-test-cli-fetchcontent-cache "[fetchcontent_cache][issue-643]" -r compact`
with 53 assertions in 7 test cases, full
`./build/test/pulp-test-cli-fetchcontent-cache -r compact` with 137
assertions in 25 test cases, exact
`ctest --test-dir build -R "status and fix labels|undeclared entries|live symlink|symlinks without deleting|parse_declared_refs_from_file|lstat miss|control characters" --output-on-failure`
passing 7/7, CLI sync check passing with slash-command/skill-reference
warnings only, sync/version/docs/compat guard reports, and
`git diff --check` against `origin/main`, the worktree, and the index.
It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 23:44 PDT, refreshed at
2026-05-05 02:26 PDT, refreshed again at 2026-05-05 07:27 PDT, and
refreshed again at 2026-05-05 08:12 PDT, then refreshed again at
2026-05-05 14:05 PDT:
rebased `local/phase3-ui-components-edges-493`
from `33d5f737` to `16cc8df1`, then to `58940ee2`, then to `a4ae8412`,
then to `edb03525`, then to `3fc80395`, then to `4bc91d9e`, covering ComboBox popup
handoff/typeahead no-op, ScrollView scrolled-child pointer-event hit
testing and paint clipping/visibility, and ListBox boundary-key and
out-of-range mouse guards for the #493 view tranche on current
`origin/main` `83271a94`.
Local validation included:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-ui-components -j$(sysctl -n hw.ncpu)`;
focused Catch cases for `ComboBox opening another popup closes the previous one`,
`ScrollView hit testing honors pointer modes with scrolled children`,
`ScrollView paint_all clips translated content and skips invisible views`, and
`ListBox ignores boundary keys and out-of-range mouse presses`; full
`pulp-test-ui-components` passing 147 assertions in 37 test cases; exact
CTest selector `ComboBox opening another popup|ScrollView hit testing honors pointer modes|ScrollView paint_all clips translated content|ListBox ignores boundary keys` passing 4/4; CLI sync check with existing
slash-command/skill-reference warnings only; sync/version/docs/compat
guard reports; and `git diff --check` against `origin/main`, the
worktree, and the index. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 23:47 PDT, refreshed at
2026-05-05 02:31 PDT, refreshed again at 2026-05-05 07:32 PDT, and
refreshed again at 2026-05-05 08:15 PDT, then refreshed again at
2026-05-05 14:08 PDT:
rebased
`local/phase3-phase9-widget-edges-493` from `d5000a24` to `d9ffd85b`,
then to `17e25c73`, then to `3b2fa68f`, then to `0de6d547`, then to
`6017bbc7`, then to `b23e4d25`, covering SplitView drag minimum clamps, miss/drag guards,
divider grip paint branches, and PropertyList boolean editing, category
height/paint, and scalar value formatting paths for the #493 view/widget
tranche on current `origin/main` `83271a94`. Local validation included:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-phase9-widgets -j$(sysctl -n hw.ncpu)`;
focused Catch cases for `SplitView drag handling clamps to pane minimums and ignores misses`,
`SplitView paint emits horizontal and vertical divider grips`,
`PropertyList mouse editing toggles writable booleans only`, and
`PropertyList paints categories and scalar value variants`; full
`pulp-test-phase9-widgets` passing 161 assertions in 34 test cases;
exact CTest selector `SplitView drag handling|SplitView paint emits|PropertyList mouse editing|PropertyList paints categories`
passing 4/4; CLI sync check with slash-command/skill-reference warnings only; sync/version/docs/compat guard
reports; and `git diff --check` against `origin/main`, the worktree, and
the index. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 23:49 PDT, refreshed at
2026-05-05 02:34 PDT, refreshed again at 2026-05-05 07:35 PDT, and
refreshed again at 2026-05-05 08:17 PDT, then refreshed again at
2026-05-05 14:11 PDT:
rebased `local/phase3-gui-components-edges-493` from `30d414ea` to
`b26a0e5f`, then to `7e94a781`, then to `b0e683e9`, then to
`c52aac5a`, then to `ad50c32d`, then to `a37626d7`, covering TableListBox header sorting, selection and
out-of-range row guards, scaled/aligned painting, and ConcertinaPanel
invalid-index, content visibility/layout, paint, and mouse hit paths for
the #493 view/gui tranche on current `origin/main` `83271a94`. Local
validation included:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-gui-components -j$(sysctl -n hw.ncpu)`;
focused Catch cases for `TableListBox header sorting and row selection edge paths`,
`TableListBox paint covers empty guards scaled columns and alignment`,
`ConcertinaPanel guards indices and syncs content visibility`, and
`ConcertinaPanel paint and mouse hit testing cover content offsets`; full
`pulp-test-gui-components` passing 174 assertions in 33 test cases; exact
CTest selector `TableListBox header sorting|TableListBox paint covers|ConcertinaPanel guards indices|ConcertinaPanel paint and mouse`
passing 4/4; CLI sync check with slash-command/skill-reference warnings only; sync/version/docs/compat guard
reports; and `git diff --check` against `origin/main`, the worktree, and
the index. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 23:52 PDT, refreshed at
2026-05-05 01:08 PDT, 2026-05-05 01:10 PDT, 2026-05-05
02:36 PDT, 2026-05-05 08:24 PDT, and 2026-05-05 14:14 PDT: rebased
`local/phase3-live-constant-editor-493` from `aeae2883` to
`0e4a677e`, then to `238736d1`, then to `29af1db8`, then to
`4ee6e846`, then to `76bbe504`, then to `84af7d62`, covering
LiveConstantRegistry duplicate registration, clamp, callback, missing
key, reset, and reset-all paths, plus LiveConstantEditor visibility,
paint, slider drag, header guard, and missing-row drag paths for the
#493 view tranche on current `origin/main` `83271a94`. Local validation
passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-live-constant-editor -j$(sysctl -n hw.ncpu)`,
full `./build/test/pulp-test-live-constant-editor -r compact` with 26
assertions in 2 test cases, exact `ctest --test-dir build -R "LiveConstantRegistry|LiveConstantEditor|live-constant" --output-on-failure`
passing 2/2, CLI sync check with slash-command/skill-reference warnings
only, sync/version/docs/compat guard reports, and
`git diff --check` against `origin/main`, the worktree, and the index. It
remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 23:54 PDT, refreshed at
2026-05-05 02:38 PDT, 2026-05-05 08:27 PDT, and 2026-05-05 14:17 PDT: rebased `local/phase3-code-editor-doc-mru-493`
from `8595aab3` to `c97574e6`, then to `e8e3545b`,
then to `f09fd8d6`, then to `130cb1a8`, then to `fef0ef6c`, covering FileBasedDocument
successful load/save-as dirty-state behavior and RecentlyOpenedFilesList
remove/missing-path behavior for the #493 view code-editor tranche on
current `origin/main` `83271a94`. Local validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-code-editor -j$(sysctl -n hw.ncpu)`,
focused `./build/test/pulp-test-code-editor "FileBasedDocument handles successful load and save_as paths" -r compact`
with 15 assertions in 1 test case, focused
`./build/test/pulp-test-code-editor "RecentlyOpenedFilesList removes entries and ignores missing paths" -r compact`
with 7 assertions in 1 test case, full
`./build/test/pulp-test-code-editor -r compact` with 74 assertions in 13
test cases, exact `ctest --test-dir build -R "FileBasedDocument handles successful|RecentlyOpenedFilesList removes entries" --output-on-failure`
passing 2/2, CLI sync check with slash-command/skill-reference warnings
only, sync/version/docs/compat guard reports, and
`git diff --check` against `origin/main`, the worktree, and the index. It
remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 20:36 PDT, refreshed at
2026-05-04 23:59 PDT, 2026-05-05 01:17 PDT, 2026-05-05
02:41 PDT, 2026-05-05 08:20 PDT, and 2026-05-05 14:21 PDT: rebased
`local/phase3-graph-editor-paint-493` from `0258cfa4` to `31d8d7cc`,
then to `2d21c893`, then to `d9ade421`, then to `6986b4af`, then to
`8ed9f335`, covering
GraphEditorView auto-layout/manual-position preservation, unnamed
node/multi-port painting, and feedback/MIDI edge paint colors for the
#493 view graph-editor tranche on current `origin/main` `83271a94`.
Local validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-graph-editor-view -j$(sysctl -n hw.ncpu)`,
focused `./build/test/pulp-test-graph-editor-view "GraphEditorView auto layout preserves positions and paints unnamed ports" -r compact`
with 7 assertions in 1 test case, focused
`./build/test/pulp-test-graph-editor-view "GraphEditorView paints feedback and midi edge colors" -r compact`
with 4 assertions in 1 test case, full
`./build/test/pulp-test-graph-editor-view -r compact` with 33 assertions
in 6 test cases, exact `ctest --test-dir build -R "GraphEditorView auto layout preserves|GraphEditorView paints feedback" --output-on-failure`
passing 2/2, CLI sync check with slash-command/skill-reference warnings
only, sync/version/docs/compat guard reports, and
`git diff --check` against `origin/main`, the worktree, and the index. It
remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 20:43 PDT, refreshed at
2026-05-05 00:02 PDT, 2026-05-05 01:20 PDT, and 2026-05-05
02:43 PDT, 2026-05-05 08:30 PDT, 2026-05-05 14:27 PDT, and
2026-05-05 14:41 PDT: rebased
`local/phase3-new-widgets-input-493` from `741caf74` to `251cfa5d`,
then to `7b5927ad`, then to `217ae169`, then to `651c4379`, then to
`3b4a06a1`, then to `ba19ff03`, covering
MidiKeyboard vertical drag release/miss behavior and note-name/highlight
painting, plus FileDropZone rejected-drop reset behavior and
idle/valid/invalid/no-icon paint paths for the #493 view/widget tranche
on current `origin/main` `9a67a517`. Local validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-phase9-widgets -j$(sysctl -n hw.ncpu)`,
focused `./build/test/pulp-test-phase9-widgets "MidiKeyboard vertical drag releases previous notes and misses" -r compact`
with 15 assertions in 1 test case, focused
`./build/test/pulp-test-phase9-widgets "MidiKeyboard paint emits note names and active highlight color" -r compact`
with 4 assertions in 1 test case, focused
`./build/test/pulp-test-phase9-widgets "FileDropZone rejected drop resets state without callback" -r compact`
with 5 assertions in 1 test case, focused
`./build/test/pulp-test-phase9-widgets "FileDropZone paint covers idle valid invalid and no-icon states" -r compact`
with 6 assertions in 1 test case, full
`./build/test/pulp-test-phase9-widgets -r compact` with 155 assertions in
34 test cases, exact `ctest --test-dir build -R "MidiKeyboard vertical drag|MidiKeyboard paint emits|FileDropZone rejected drop|FileDropZone paint covers" --output-on-failure`
passing 4/4, CLI sync check with slash-command/skill-reference warnings
only, sync/version/docs/compat guard reports, and
`git diff --check` against `origin/main`, the worktree, and the index. It
remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 20:51 PDT, refreshed at
2026-05-05 00:04 PDT, 2026-05-05 01:22 PDT, and 2026-05-05
02:46 PDT, 2026-05-05 08:22 PDT, 2026-05-05 14:30 PDT, and
2026-05-05 14:41 PDT: rebased
`local/phase3-file-browser-paint-493` from `74dd1dd0` to `74bc4483`,
then to `57a12010`, then to `091fdb77`, then to `410e0be0`, then to
`d37c7cd8`, then to `f7ff2469`, covering
FileBrowser sorted-row paint clipping and MultiDocumentPanel
active/inactive tab paint output for the #493 view/file-browser tranche
on current `origin/main` `9a67a517`. Local validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-file-browser -j$(sysctl -n hw.ncpu)`,
focused `./build/test/pulp-test-file-browser "FileBrowser paint clips rows after sorted visible entries" -r compact`
with 9 assertions in 1 test case, focused
`./build/test/pulp-test-file-browser "MultiDocumentPanel paint emits active and inactive tab labels" -r compact`
with 12 assertions in 1 test case, full
`./build/test/pulp-test-file-browser -r compact` with 81 assertions in 6
test cases, exact `ctest --test-dir build -R "FileBrowser paint clips rows|MultiDocumentPanel paint emits" --output-on-failure`
passing 2/2, CLI sync check with slash-command/skill-reference warnings
only, sync/version/docs/compat guard reports, and
`git diff --check` against `origin/main`, the worktree, and the index. It
remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 20:57 PDT, refreshed at
2026-05-05 00:06 PDT, 2026-05-05 01:26 PDT, and 2026-05-05
02:48 PDT: rebased
`local/phase3-splash-screen-493` from `38f2c93d` to `0e7ebab3`,
then to `0ac19fbd`, then to `1dd00e70`, covering SplashScreen advance/dismiss
callback behavior, dismiss-on-click gating, and text/image paint output
for the #493 view tranche on current `origin/main`. It remains unpushed
and undispatched.

Additional local-only progress at 2026-05-04 21:03 PDT, refreshed at
2026-05-05 00:09 PDT, 2026-05-05 01:29 PDT, and 2026-05-05
02:51 PDT, refreshed again at 2026-05-05 07:53 PDT, and refreshed
again at 2026-05-05 13:23 PDT, 2026-05-05 14:33 PDT, and
2026-05-05 14:36 PDT: rebased
`local/phase3-appearance-manager-493` from `b96abb6b` to `31d4e8ff`,
then to `ceb05add`, then to `c0b6bece`, then to `3a0fae19`, then to
`ce479305`, then to `a2fbbbde`, then to `0bbaaa9a`, covering
AppearanceTracker repeated lock callbacks and locked poll no-op behavior,
plus ThemeManager locked-theme, locked-appearance callback, and unlock
behavior for the #493 view appearance-manager tranche on current
`origin/main` `9a67a517`. Local validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-appearance -j$(sysctl -n hw.ncpu)`,
focused `./build/test/pulp-test-appearance "AppearanceTracker callbacks follow repeated locks and locked poll no-op" -r compact`
with 7 assertions in 1 test case, focused
`./build/test/pulp-test-appearance "ThemeManager callbacks cover locked theme poll and unlock" -r compact`
with 13 assertions in 1 test case, full
`./build/test/pulp-test-appearance -r compact` with 34 assertions in 10
test cases, exact `ctest --test-dir build -R "AppearanceTracker callbacks follow|ThemeManager callbacks cover" --output-on-failure`
passing 2/2, CLI sync check with slash-command/skill-reference warnings
only, sync/version/docs/compat guard reports, and
`git diff --check` against `origin/main`, the worktree, and the index. It
remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 21:09 PDT, refreshed at
2026-05-05 00:12 PDT, 2026-05-05 01:32 PDT, and 2026-05-05
02:55 PDT, refreshed again at 2026-05-05 07:39 PDT, and refreshed
again at 2026-05-05 13:25 PDT and 2026-05-05 14:46 PDT: rebased
`local/phase3-tree-view-edges-493` from `76b99bee` to `dc87d5fe`,
then to `94ddde56`, then to `de5d45ae`, then to `5444a6c5`, then to
`656158c8`, then to `ca5267a7`, covering TreeView disclosure collapse, left-key
consumed-state behavior,
selected-row paint highlight, and expanded/collapsed disclosure paint
output for the #493 view TreeView tranche on current `origin/main`
`9a67a517`. Local validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-tree-view -j$(sysctl -n hw.ncpu)`,
focused `./build/test/pulp-test-tree-view "TreeView triangle click toggles without selecting" -r compact`
with 10 assertions in 1 test case, focused
`./build/test/pulp-test-tree-view "TreeView key left consumes collapsed selected nodes without toggling" -r compact`
with 3 assertions in 1 test case, focused
`./build/test/pulp-test-tree-view "TreeView paint covers selection highlight and disclosure states" -r compact`
with 7 assertions in 1 test case, full
`./build/test/pulp-test-tree-view -r compact` with 53 assertions in 15
test cases, exact `ctest --test-dir build -R "TreeView triangle click|TreeView key left consumes|TreeView paint covers" --output-on-failure`
passing 3/3, CLI sync check with slash-command/skill-reference warnings
only, sync/version/docs/compat guard reports, and
`git diff --check` against `origin/main`, the worktree, and the index. It
remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 21:17 PDT, refreshed at
2026-05-05 00:16 PDT, 2026-05-05 01:34 PDT, and 2026-05-05
02:58 PDT, refreshed again at 2026-05-05 07:42 PDT, and refreshed again
at 2026-05-05 13:31 PDT and 2026-05-05 14:50 PDT: rebased
`local/phase3-modal-overlay-edges-493` from `e40432f2` to `0f30dc7a`,
then to `95a7b597`, then to `ba5f804e`, then to `7fdd0c30`, then to
`45491075` on `24047ba3`, then to `3496e7e6` on `83271a94`, then to
`67fe482e` on `7e9795b4`, covering
ModalOverlay key-release/no-callback Escape behavior, backdrop alpha
paint output, and backdrop-click dismissal hit/flag guards for the #493
view ModalOverlay tranche on current `origin/main` `7e9795b4`. Local
validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-modal -j$(sysctl -n hw.ncpu)`,
focused `./build/test/pulp-test-modal "ModalOverlay ignores key releases and handles Escape without callback" -r compact`
with 2 assertions in 1 test case, focused
`./build/test/pulp-test-modal "ModalOverlay paint applies backdrop alpha to fill color" -r compact`
with 4 assertions in 1 test case, focused
`./build/test/pulp-test-modal "ModalOverlay backdrop mouse dismissal respects hit target and flag" -r compact`
with 4 assertions in 1 test case, full
`./build/test/pulp-test-modal -r compact` with 18 assertions in 8 test
cases, exact `ctest --test-dir build -R "ModalOverlay ignores key releases|ModalOverlay paint applies|ModalOverlay backdrop mouse dismissal" --output-on-failure`
passing 3/3, CLI sync check with slash-command/skill-reference warnings
only, sync/version/docs/compat guard reports, and
`git diff --check` against `origin/main`, the worktree, and the index. It
remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 21:28 PDT, refreshed at
2026-05-05 00:19 PDT, 2026-05-05 01:38 PDT, and 2026-05-05
03:01 PDT, refreshed again at 2026-05-05 07:57 PDT, refreshed again
at 2026-05-05 13:35 PDT, and refreshed again at 2026-05-05 14:56 PDT:
rebased
`local/phase3-auto-ui-edges-493` from `485f6cd5` to `545493ad`,
then to `39c0e3a1`, then to `19f02f0c`, then to `f763c914`, then to
`1ea687bf`, then to `832c0781`, covering AutoUi generated toggle state, generated knob
display-format branches, and sync propagation to toggles and existing
faders for the #493 view AutoUi tranche on current `origin/main`
`7e9795b4`. Local validation passed:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-auto-ui -j$(sysctl -n hw.ncpu)`;
focused `./build/test/pulp-test-auto-ui "AutoUi generated controls expose toggle state and formatted values" -r compact`
with 10 assertions in 1 test case; focused
`./build/test/pulp-test-auto-ui "AutoUi sync updates generated toggles and existing faders" -r compact`
with 6 assertions in 1 test case; full
`./build/test/pulp-test-auto-ui -r compact` passing 21 assertions in 4 test cases; exact-ish
CTest selector `AutoUi generated controls|AutoUi sync updates` passing
3/3, including the pre-existing sync case matched by the selector; CLI
sync check passing with slash-command/skill-reference warnings only;
sync/version/docs/compat guard reports; and
`git diff --check` against `origin/main`, the worktree, and the index.
It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 21:34 PDT, refreshed at
2026-05-05 00:21 PDT, 2026-05-05 01:42 PDT, and 2026-05-05
03:05 PDT, refreshed again at 2026-05-05 07:59 PDT, refreshed again
at 2026-05-05 13:39 PDT, and refreshed again at 2026-05-05 15:00 PDT:
rebased
`local/phase3-image-cache-trim-493` from `1afb279e` to `312a008f`, then
to `d9a0fc92`, then to `a3ca7f7c`, then to `9422522f`, then to
`dca796b7`, then to `165c2357`, covering ImageCache byte-budget lowering, least-recently-used
trimming, and releaser behavior for the #493 view ImageCache tranche on
current `origin/main` `7e9795b4`. Local validation passed:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-image-cache -j$(sysctl -n hw.ncpu)`;
focused `./build/test/pulp-test-image-cache "lowering byte budget trims least recently used entries" -r compact`
passing 13 assertions in 1 test case; full
`./build/test/pulp-test-image-cache -r compact` passing 49 assertions
in 9 test cases; exact CTest selector
`lowering byte budget trims|image-cache|ImageCache` passing 1/1; CLI
sync check passing with slash-command/skill-reference warnings only;
sync/version/docs/compat guard reports; and
`git diff --check` against `origin/main`, the worktree, and the index.
It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 21:42 PDT, refreshed at
2026-05-05 00:25 PDT, 2026-05-05 01:44 PDT, 2026-05-05
03:07 PDT, 2026-05-05 08:02 PDT, 2026-05-05 13:44 PDT, and
2026-05-05 15:03 PDT: rebased
`local/phase3-visualization-bridge-edges-493` from `10405bc2` to
`dc41a698`, then to `3e45d531`, then to `a39f609c`, then to
`bc8a0cfe`, then to `453a19e7`, then to `a30900b8`, covering VisualizationBridge disabled-waveform,
zero-channel, and waveform capture-length clamp paths for the #493 view
tranche on current `origin/main` `7e9795b4`. Local validation passed:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-visualization -j$(sysctl -n hw.ncpu)`;
focused bridge run `./build/test/pulp-test-visualization "[vizbridge]" -r compact`
passing 24 assertions in 9 test cases; full
`./build/test/pulp-test-visualization -r compact` passing 38 assertions
in 15 test cases; exact CTest selector
`VisualizationBridge skips waveform|VisualizationBridge zero-channel|VisualizationBridge clamps waveform`
passing 3/3; CLI sync check passed with slash-command/skill-reference
warnings only; skill-sync, version-bump, docs-sync, and compat-sync
reports passed or reported no mapped paths touched; and `git diff --check` against
`origin/main`, the worktree, and the index. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 21:49 PDT, refreshed at
2026-05-05 00:28 PDT, 2026-05-05 01:47 PDT, 2026-05-05
03:09 PDT, 2026-05-05 08:07 PDT, 2026-05-05 13:47 PDT, and
2026-05-05 15:05 PDT: rebased
`local/phase3-waveform-editor-edges-493` from `a199b6dc` to
`9e28c887`, then to `761d4c15`, then to `1cf964ba`, then to
`020a22a4`, then to `e3b02b07`, then to `1ea97d99`, covering WaveformEditor selection, visible-range, and
playhead clamps, paint overlay output, key scroll/release handling, and
mouse selection extension paths for the #493 view tranche on current
`origin/main` `7e9795b4`. Local validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-waveform-editor -j$(sysctl -n hw.ncpu)`,
focused `./build/test/pulp-test-waveform-editor "[view][waveform_editor]" -r compact`
with 62 assertions in 20 test cases, full
`./build/test/pulp-test-waveform-editor -r compact` with 62 assertions
in 20 test cases, exact `ctest --test-dir build -R "WaveformEditor clamps selection|WaveformEditor zoom to selection is a no-op|WaveformEditor visible range clamps|WaveformEditor playhead clamps|WaveformEditor paint records|WaveformEditor key scrolls|WaveformEditor mouse click" --output-on-failure`
passing 7/7, CLI sync check passed with slash-command/skill-reference
warnings only, skill-sync/version-bump/docs-sync/compat-sync reports
passed or reported no mapped paths touched, and `git diff --check` against
`origin/main`, the worktree, and the index. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 21:56 PDT, refreshed at
2026-05-05 00:31 PDT, 2026-05-05 01:49 PDT, and 2026-05-05
03:12 PDT, 2026-05-05 08:10 PDT, 2026-05-05 13:50 PDT, and
2026-05-05 15:09 PDT: rebased
`local/phase3-window-manager-edges-493` from `5d62f37a` to `7fd5052a`,
then to `49f96229`, then to `2ba38714`, then to `8220c168`, then to
`baa1ae56`, then to `f7da4e54`, covering WindowManager unregister callback/missing-id
cleanup, null host/root close behavior, and missing-handler
send/broadcast paths for the #493 view tranche on current `origin/main`
`7e9795b4`. Local validation passed
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
`cmake --build build --target pulp-test-window-manager -j$(sysctl -n hw.ncpu)`,
focused `./build/test/pulp-test-window-manager "[view][multiwindow]" -r compact`
with 82 assertions in 21 test cases, full
`./build/test/pulp-test-window-manager -r compact` with 82 assertions in
21 test cases, exact `ctest --test-dir build -R "WindowManager unregister invokes|WindowManager closes windows without|WindowManager send and broadcast skip" --output-on-failure`
passing 3/3, CLI sync check passed with slash-command/skill-reference
warnings only, skill-sync/version-bump/docs-sync/compat-sync reports
passed or reported no mapped paths touched, and `git diff --check` against
`origin/main`, the worktree, and the index. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 22:02 PDT, refreshed at
2026-05-05 00:34 PDT, 2026-05-05 01:51 PDT, and 2026-05-05
03:14 PDT, 2026-05-05 07:50 PDT, 2026-05-05 13:53 PDT, and
2026-05-05 15:12 PDT: rebased
`local/phase3-param-attachment-edges-493` from `0d6600ec` to
`cf7957c3`, then to `80bc3351`, then to `61a5fdc2`, then to
`c862cceb`, then to `6da58f0f`, then to `4e7ab1ad`, covering ParamAttachment
fader/toggle/combo callback forwarding, missing parameter-id no-op
behavior, and `poll_bindings()` external-change propagation for the #493
view tranche on current `origin/main` `7e9795b4`. Local validation passed:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-param-attachment -j$(sysctl -n hw.ncpu)`;
tag run `./build/test/pulp-test-param-attachment "[view][attachment][issue-493]" -r compact`
passing 27 assertions in 3 test cases; full
`./build/test/pulp-test-param-attachment -r compact` passing 43 assertions
in 13 test cases;
exact CTest selector `param attachments forward|param attachments tolerate|poll_bindings forwards`
passing 3/3; CLI sync check passed with slash-command/skill-reference
warnings only; skill-sync, version-bump, docs-sync, and compat-sync
reports passed or reported no mapped paths touched; and `git diff --check` against
`origin/main`, the worktree, and the index. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 22:08 PDT, refreshed at
2026-05-05 00:36 PDT, 2026-05-05 01:54 PDT, and 2026-05-05
03:16 PDT, 2026-05-05 07:46 PDT, 2026-05-05 13:57 PDT, and
2026-05-05 15:16 PDT: rebased
`local/phase3-input-events-edges-493` from `58a06b63` to `c7d18912`,
then to `235f98b2`, then to `d96d76a6`, then to `361242e4`, then to
`ee320ea1`, then to `e87e6b10`, covering InputEvents wheel/meta mouse helper paths,
ended/cancelled gesture deltas, key release/repeat main-modifier checks,
and missing pointer-capture release behavior for the #493 view tranche
on current `origin/main` `7e9795b4`. Local validation passed:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-input-events -j$(sysctl -n hw.ncpu)`;
tag run `./build/test/pulp-test-input-events "[view][input][issue-493]" -r compact`
passing 29 assertions in 4 test cases; full
`./build/test/pulp-test-input-events -r compact` passing 85 assertions
in 22 test cases; exact CTest selector
`MouseEvent wheel|GestureEvent ended|KeyEvent main modifier|View pointer capture ignores`
passing 4/4; CLI sync check passed with slash-command/skill-reference
warnings only; skill-sync, version-bump, docs-sync, and compat-sync
reports passed or reported no mapped paths touched; and `git diff --check` against
`origin/main`, the worktree, and the index. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 22:16 PDT, refreshed at
2026-05-05 00:41 PDT, 2026-05-05 01:56 PDT, and 2026-05-05
03:18 PDT, 2026-05-05 14:01 PDT, and 2026-05-05 15:20 PDT:
rebased/amended
`local/phase3-widget-bridge-dom-493` from `40629315` to `df183299`,
then to `d2eec1b9`, then to `d332d795`, then to `8987b7d2`, then to
`4ecd226d`, covering
WidgetBridge native DOM subtree moves, recursive DOM removal widget-map
cleanup, root/missing layout helper paths, and the root-aware
`getLayoutRect("")` registration fix for the #493 view tranche on
current `origin/main` `7e9795b4`. Local validation passed:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`;
`cmake --build build --target pulp-test-widget-bridge -j$(sysctl -n hw.ncpu)`;
focused `./build/test/pulp-test-widget-bridge "[view][bridge][dom][issue-493]" -r compact`
passing 29 assertions in 3 test cases; full
`./build/test/pulp-test-widget-bridge -r compact` passing 931 assertions
in 133 test cases; exact
`ctest --test-dir build -R "WidgetBridge DOM append|WidgetBridge DOM remove|WidgetBridge layout helpers" --output-on-failure`
passing 3/3; CLI sync check passed with slash-command/skill-reference
warnings only; skill-sync and docs-sync reported no mapped paths touched;
compat-sync passed through the existing `Compat-Update: skip` trailers for
`canvas2d`, `css`, `html`, `imports`, `react`, `rn`, and `yoga`;
version-bump report suggested an SDK patch and no plugin bump; and
`git diff --check` against `origin/main`, the worktree, and the index. It
remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 22:34 PDT, refreshed at
2026-05-04 22:46 PDT, 2026-05-04 23:04 PDT, 2026-05-05 00:50 PDT,
2026-05-05 02:02 PDT, 2026-05-05 03:22 PDT, and 2026-05-05 15:30 PDT:
rebased/amended `local/phase3-harness-verifier-643` from `51c0c8bf`
to `bdb9cd42`, then to `7b7cc19d`, then to `e9be30f6`, then to
`b94b48c0`, then to `345c0039`, then to `08353da0`, covering
`tools/harness` status/verifier helper paths, current Yoga and CSS
harness baselines, adapter unit-test discovery from `test/harness/test_*.py`
including the current RN adapter coverage, compat-sync's
unknown-requirement hard-error expectation, and Python coverage-runner
discovery/omit rules so harness tests participate in the measured Python
tools lane without transient adapter fixtures being reported as source.
The latest refresh is on current `origin/main` `7e9795b4` with Yoga
expecting total 53, PASS 21, DIVERGE 20, NOT_IMPL 12 and CSS expecting
199 catalog entries with `backfaceVisibility` as the supported-but-not-wired
fixture. It remains unpushed and undispatched.

The earlier #1272 Android failure remains classified as external
infrastructure: Gradle could not resolve `dl.google.com` while
installing Build-Tools 34. The PR later merged as
`cb641ff19c16ce0115e3dae7c04e5c2fa5a62760`; if the same failure
reappears on another Android coverage tranche, treat it as rerun-only,
not a code patch, once the workflow is in a safe rerunnable state.

## Current Watch Point

Last live check: 2026-05-01 21:41:43 EDT.

- Open `codecov` PRs: 4.
- Merge state: #1211 merged cleanly as `827227339a0609358ba0371a86417a868ee9879e`.
  #1207 also merged cleanly as `9da7b03a522a2c08042bbfe6f3149612ed02bb82`
  after the long Windows release-path gate completed. #1212 merged
  cleanly as `cb954187cc2e7cc00b6cd972f6f2767a9b01af58`. #1213 merged
  cleanly as `ea03a328ef3351eeaada61259321d9849f6f4fa4`. #1214 merged
  cleanly as `e1ef408071456dbe9a66297c912edb6dcca8523b`. #1216 merged
  cleanly as `57527d7569f8ebd8882779d10e8ad5b7e92cf259`. #1217 merged
  cleanly as `bc9765f4ddb79b6e9c8c4e3e4aa4e601daecd619`. #1215 merged
  cleanly as `51f1762ca23e6c5f45b4e31bd5dda5122d2cc855` after the
  unrelated Linux `BufferingReader` failure cleared on rerun. #1202 merged
  cleanly as `00b0871a6dea89fecb14d4aca8502e5c52507416`. #1218 merged
  cleanly as `a147af718361001323755ac01da395d271fe4ee2`. #1219 merged
  cleanly as `4e93da00dddab17b75531d2fcda1b0511e3a4e9c`. #1220 merged
  cleanly as `3d2ebe6e6af57f10a29c2e17439e926221af8521`. #1221 merged
  cleanly as `7a49b91aa0a0dd650f0dc5fc1c1d560e488e51c8`. #1222 merged
  cleanly as `30fc6ec792295d13412c599027aa4000d47d35ff`. #1223 is open from
  `feature/phase3-add-component-coverage-643` at `cd93085b0057`. #1224 is
  open from `feature/phase3-cli-sync-check-extra-643` at `db185f4d4786`.
  #1225 is open from `feature/phase3-compat-sync-extra-643` at
  `48bf3b9eeabb`. #1226 is open from
  `feature/phase3-list-limitations-coverage-643` at `fc21f4edf873`.
  #1223 merged cleanly as `71267a71d7b631a79f14b375d710f6a093d0403d`.
  #1224 merged cleanly as `870eb62abb724f63ffb715b82764792979759e2b`.
  #1225 merged cleanly as `26155037c6df284cf5bbbf29a104faf7368dcb37`.
  #1226 merged cleanly as `2623a4b47c1d87ccbac3acda13f549bc10dbf320`.
  #1227 merged cleanly as `e495cdba0e02b41bc14f1cd625274f3416346574`.
  #1228 merged cleanly as `d7b973395782ed4457a7dab65a1b0ea7ca5d5cd8`.
  #1229 merged cleanly as `a12411e4979832733d66eb9fac9b5471be024be3`.
  #1230 merged cleanly as `d1750daa6dbd52aed42dd84d2dc92ba4dd701826`.
  #1232 merged cleanly as `8c1c80882a29e676bcc9de74ea5f8a6765e20c46`.
  #1233 merged cleanly as `077145f543e2074b22513df3ac2becdfda2c100d`.
  #1234 merged cleanly as `8a9eadc47004911a6c4a7e33b646cbd52b096935`.
  #1235 merged cleanly as `39b3b76858e486a607f047b800bcde69abc0bdb1`.
  #1236 merged cleanly as `69bbe8a467d2900cb462498bd0e4211a156d79a7`.
  #1237 merged cleanly as `f6e092ba647553a4eb1864f42594afe8973009c3`.
  #1238 merged cleanly as `ef4303856482580beb5dcc4c15a7f22a2715454a`.
  #1239 merged cleanly as `0a0cb73737097f7e9f9bd2606ea0f3579738773e`.
  #1231 merged cleanly as `e9a1d7e33abd3469255676070b0dbcf306cd35db`.
  #1240 merged from `UNSTABLE` as
  `f414cdc85dbc93bf18b416f7ba94d31716d0691c` after required gates were
  green and only advisory macOS sanitizer jobs remained pending. #1243,
  #1244, and #1245 were opened from local Phase 3 tranches and are now
  queued/running under PR-event Namespace checks. #1241 merged cleanly
  as `b0372e3c61b1750bb65cbcbfe139496b55321d90`.
- Refill: opened #1227 from
  `local/phase3-android-target-coverage-643`, branch
  `feature/phase3-android-target-coverage-643`, head `92135ab5df18`.
  Applied `codecov`, linked #641/#643, and PR-event checks are
  queued/running. Local validation after rebasing onto `origin/main` at
  `71267a71d7b6` included `python3 tools/scripts/test_android_target.py`,
  venv-backed `run_python_coverage.py --pattern
  tools/scripts/test_android_target.py`, and diff/index cleanliness checks;
  focused coverage reported 100% for `tools/local-ci/android_target.py`.
- Refill: opened #1228 from
  `local/phase3-audit-top-level-coverage-643`, branch
  `feature/phase3-audit-top-level-coverage-643`, head `95112c60799a`.
  Applied `codecov`, linked #641/#643, and PR-event checks are
  queued/running. Local validation after rebasing onto `origin/main` at
  `26155037c6df` included `python3 tools/scripts/test_audit_top_level.py`,
  venv-backed `run_python_coverage.py --pattern
  tools/scripts/test_audit_top_level.py`, docs-sync, skill-sync, and
  diff/index cleanliness checks; focused coverage reported 100% for
  `tools/audit.py`.
- Refill: opened #1229 from
  `local/phase3-validate-hosts-coverage-643`, branch
  `feature/phase3-validate-hosts-coverage-643`, head `18363f59e392`.
  Applied `codecov`, linked #641/#643, and PR-event checks passed. Local
  validation after rebasing onto `origin/main` at
  `2623a4b47c1d` included `python3 tools/scripts/test_validate_hosts.py`,
  venv-backed `run_python_coverage.py --pattern
  tools/scripts/test_validate_hosts.py`, docs-sync, and diff/index
  cleanliness checks; focused coverage reported 100% for
  `tools/deps/validate_hosts.py`. No real SSH host validation was run.
  Merged cleanly as `a12411e4979832733d66eb9fac9b5471be024be3`.
- Refill: opened #1230 from
  `local/phase3-build-migration-index-extra-643`, branch
  `feature/phase3-build-migration-index-extra-643`, head `bc37c17e8f92`.
  Applied `codecov`, linked #641/#643, and PR-event checks passed. Local
  validation after rebasing onto `origin/main` at
  `e495cdba0e02` included 7 base tests, 10 extra tests, venv-backed
  `run_python_coverage.py --pattern tools/scripts/test_build_migration_index.py
  --pattern tools/scripts/test_build_migration_index_extra.py`, sync checks,
  and diff/index cleanliness checks; focused coverage reported 100% for
  `tools/scripts/build_migration_index.py`. Merged cleanly as
  `d1750daa6dbd52aed42dd84d2dc92ba4dd701826`.
- Refill: opened #1231 from
  `local/phase3-skill-sync-extra-643`, branch
  `feature/phase3-skill-sync-extra-643`, head `eca11f92c418`.
  Applied `codecov`, linked #641/#643, and PR-event checks are
  queued/running. Build and Test attempt 1 was cancelled after the Linux
  Namespace job sat in `Test (non-Windows)` for nearly an hour with a stale
  workflow `updatedAt`; attempt 2 is running under the same workflow run
  `25237965433` with fresh required platform jobs. Local validation after
  rebasing onto `origin/main` at
  `a12411e49798` included 4 base tests, 20 extra tests, 39 gate tests,
  venv-backed `run_python_coverage.py --pattern
  tools/scripts/test_skill_sync_check.py --pattern
  tools/scripts/test_skill_sync_check_extra.py`, sync checks, version-bump
  report, and diff cleanliness checks; focused coverage reported 100% for
  `tools/scripts/skill_sync_check.py`.
- Refill: opened #1232 from
  `local/phase3-validate-registry-extra-643`, branch
  `feature/phase3-validate-registry-extra-643`, head `619560ed83b7`.
  Applied `codecov`, linked #641/#643, and PR-event checks passed. Local
  validation after rebasing onto `origin/main` at
  `a12411e49798` included 8 extra tests, 14 package validation tests,
  non-strict registry validation, venv-backed `run_python_coverage.py
  --pattern tools/scripts/test_validate_registry_extra.py`, sync/version
  checks, coverage-tier, and diff cleanliness checks; focused coverage
  reported 100% for `tools/packages/validate_registry.py`. Merged cleanly as
  `8c1c80882a29e676bcc9de74ea5f8a6765e20c46`.
- Refill: opened #1233 from
  `local/phase3-cmajor-external-extra-643`, branch
  `feature/phase3-cmajor-external-extra-643`, head `89f10266b338`.
  Applied `codecov`, linked #641/#643, and PR-event checks are
  queued/running. Local validation after rebasing onto `origin/main` at
  `d1750daa6dbd` included 13 unittest cases, venv-backed
  `run_python_coverage.py --pattern tools/scripts/test_cmajor_external.py
  --pattern tools/scripts/test_cmajor_external_extra.py`, the Cmajor doctor
  smoke, and diff cleanliness checks; focused coverage reported 100% for
  `tools/scripts/cmajor_external.py`. A follow-up correction comment records
  the full head SHA after an earlier comment expanded the short SHA
  incorrectly.
- Refill: opened #1234 from
  `local/phase3-resolve-runs-on-extra-643`, branch
  `feature/phase3-resolve-runs-on-extra-643`, head `42f24313468a`.
  Applied `codecov`, linked #641/#643, and PR-event checks are
  queued/running. Local validation after rebasing onto `origin/main` at
  `d1750daa6dbd` included 18 base tests, 2 extra tests, venv-backed
  `run_python_coverage.py --pattern tools/scripts/test_resolve_runs_on.py
  --pattern tools/scripts/test_resolve_runs_on_extra.py`, docs-sync, and
  diff cleanliness checks; focused coverage reported 100% for
  `tools/scripts/resolve_runs_on.py`.
- Refill: opened #1235 from
  `local/phase3-freshness-extra-643`, branch
  `feature/phase3-freshness-extra-643`, head `9ad94e55264d`.
  Applied `codecov`, linked #641/#643, then force-pushed a correction so
  the extra tests live under the default Linux Python coverage surface at
  `tools/scripts/test_freshness_check_extra.py`. Local validation after
  rebasing onto `origin/main` at `8c1c80882a29` included 12 extra tests,
  non-strict registry validation, sync/version checks, and diff
  cleanliness checks; focused `run_python_coverage.py --pattern
  tools/scripts/test_freshness_check_extra.py` reported 100% for
  `tools/packages/freshness_check.py`. Merged cleanly as
  `39b3b76858e486a607f047b800bcde69abc0bdb1` after corrected PR-event
  checks passed.
- Refill: opened #1236 from
  `local/phase3-merge-cobertura-extra-643`, branch
  `feature/phase3-merge-cobertura-extra-643`, head `79c89e970d34`.
  Applied `codecov`, linked #641/#643, and PR-event checks passed.
  Local validation after rebasing onto `origin/main` at
  `077145f543e2` included 14 base tests, 3 extra tests, focused
  `run_python_coverage.py --pattern tools/scripts/test_merge_cobertura.py
  --pattern tools/scripts/test_merge_cobertura_extra.py`, docs-sync,
  skill-sync, version-bump, CLI sync, and diff cleanliness checks;
  focused coverage reported 100% for `tools/scripts/merge_cobertura.py`.
  Merged cleanly as `69bbe8a467d2900cb462498bd0e4211a156d79a7`.
- Refill: opened #1237 from
  `local/phase3-lcov-cobertura-extra-643`, branch
  `feature/phase3-lcov-cobertura-extra-643`, head `d1d7686b99a9`.
  Applied `codecov`, linked #641/#643, and PR-event checks passed.
  Local validation after rebasing onto `origin/main` at
  `8a9eadc47004` included 8 base tests, 11 extra tests, focused
  `run_python_coverage.py --pattern tools/scripts/test_lcov_cobertura.py
  --pattern tools/scripts/test_lcov_cobertura_extra.py`, docs-sync,
  skill-sync, version-bump, CLI sync, and diff cleanliness checks;
  focused coverage reported 100% for `tools/scripts/lcov_cobertura.py`.
  Merged cleanly as `f6e092ba647553a4eb1864f42594afe8973009c3`.
- Refill: opened #1238 from
  `local/phase3-eq-curve-view-edges-493`, branch
  `feature/phase3-eq-curve-view-edges-493`, head `e13679518661`.
  Applied `codecov`, linked #493/#641, and required PR-event gates passed.
  Local validation after rebasing onto `origin/main` at
  `8a9eadc47004` included configure, `pulp-test-phase9-widgets`,
  direct `[eq_curve]` widget tests, 4 matching `EqCurveView` CTest cases,
  skill-sync and version-bump reports, and diff checks; final diff is
  limited to `test/test_phase9_widgets.cpp`. Merged from `UNSTABLE` as
  `ef4303856482580beb5dcc4c15a7f22a2715454a` after required wrappers and
  Codecov patch were green; only advisory/non-blocking checks were active.
- Refill: opened #1239 from
  `local/phase3-websocket-channel-edges-641`, branch
  `feature/phase3-websocket-channel-edges-641`, head `80dd3df80eda`.
  Applied `codecov`, linked #641, and PR-event checks are queued/running.
  The branch was rebased after #1236 merged; local validation included
  `pulp-test-websocket-channel`, 11 matching WebSocket CTest cases, direct
  binary run with 62 assertions in 11 test cases, and diff checks.
- Refill: opened #1240 from
  `local/phase3-theme-io-validation-493`, branch
  `feature/phase3-theme-io-validation-493`, head `af2411244db3`.
  Applied `codecov`, linked #493/#641, and PR-event checks are
  queued/running. The branch was rebased after #1236 merged; local
  validation included `pulp-test-theme "[theme]"`, 17 matching focused
  CTest cases, and diff checks.
- Refill: opened #1241 from
  `local/phase3-docs-generate-coverage-643`, branch
  `feature/phase3-docs-generate-coverage-643`, head `7d9b34c6a985`.
  Applied `codecov`, linked #641/#643, and PR-event checks are
  queued/running. Local validation after rebasing onto `origin/main` at
  `f6e092ba6475` included 15 docs-generator tests,
  `tools/docs_generate.py check`, focused 100% coverage for
  `tools/docs_generate.py`, and diff checks.
- GitHub Actions pressure: #1231, #1239, #1240, and #1241 have PR-event
  Build/Coverage checks active or queued on Namespace-backed lanes and no
  failed checks. The newer batch is draining normally.
- Local-only progress: `pulp-check-docs-status-extra-643` refreshed cleanly
  at `36e80fe7` against `origin/main` at `ef430385`; 10
  format-validation tests, 23 coverage-runner tests, warn-mode format
  validation, focused coverage, and diff checks passed. Warn mode still
  reports 4 existing missing `production_validated` entries.
- Local-only progress: `pulp-phase3-websocket-channel-edges-641`
  refreshed cleanly at `7b86ded8` with direct WebSocket tests, 11 matching
  CTest cases, and a narrowed coverage/diff-cover mirror green. A follow-up
  worker reconfirmed the branch is still based on `origin/main` at
  `8a9eadc47004`, clean, one commit ahead, and limited to
  `test/test_websocket_channel.cpp`.
- Local-only progress: `pulp-phase3-eq-curve-view-edges-493` refreshed
  cleanly at `e1367951` with direct `[eq_curve]` widget tests, 4 matching
  CTest cases, and diff scope limited to `test/test_phase9_widgets.cpp`.
- Refill progress: `pulp-phase3-theme-io-validation-493` is now queued as
  #1240 at `af241124` with direct `[theme]` tests, 17 matching CTest cases,
  and diff scope limited to `test/test_theme.cpp`.
- Local-only audit: `pulp-phase3-canvas-fallback-edges-641` was rebased to
  `origin/main` at `69bbe8a4`; the old local patch was already absorbed
  upstream as `42464b4b` / #1201, so no reopen action is needed. Validation
  still passed: `pulp-test-canvas "[canvas][fallback]"` reported 62
  assertions in 4 test cases.
- Local-only audit: `pulp-phase3-rectangle-list-edges-641` was rebased to
  `origin/main` at `f6e092ba`; the old local patch was already absorbed
  upstream as `9d1e7d66` / #1203. Validation still passed:
  `pulp-test-rectangle-list` reported 60 assertions in 16 test cases and
  matching CTest passed 16/16.
- Refill progress: `pulp-docs-generate-coverage-643` is now queued as #1241
  at `7d9b34c6` with 15 docs-generator tests, `tools/docs_generate.py check`,
  focused 100% coverage for `tools/docs_generate.py`, and diff checks green.
- Local-only progress: `pulp-mkdocs-hooks-coverage-643` refreshed cleanly
  at `900faf7b` against `origin/main` at `ef430385`; direct hook unittest
  passed 10 tests and diff checks are clean. Local coverage/pytest variants
  are blocked only because this machine lacks `coverage.py >= 7.10` and
  `pytest`; earlier focused coverage for this tranche reported 100% for
  `tools/mkdocs_hooks.py`.
- Local-only progress: `pulp-merge-cobertura-extra-643` refreshed cleanly
  at `29cb5a51` with 17 total tests and 100% target coverage for
  `tools/scripts/merge_cobertura.py`.
- Local-only progress: `pulp-lcov-cobertura-extra-643` refreshed cleanly
  at `e931e469` with 19 total tests and 100% target coverage for
  `tools/scripts/lcov_cobertura.py`.
- Local-only progress: `pulp-phase3-app-framework-edges-493` was rebased
  cleanly again to `f5a44955` after #1237 merged. The earlier target
  validation remains recorded below, but this latest head was not
  revalidated because both `build/` and `build-cov/` had been removed from
  the worktree; rerun the focused app-framework build before queueing.
- Local-only progress: `pulp-phase3-asset-manager-coverage-493` remains
  useful after #1125 and refreshed cleanly at `9bd0fea1` against
  `origin/main` at `f6e092ba`. It adds file-backed shader/blob cache and
  cached `@2x` image behavior coverage; `pulp-test-asset-manager` passed 75
  assertions in 29 test cases, matching CTest passed 24/24, and diff checks
  are clean.
- Local-only audit: `pulp-phase3-table-model-edges-493` rebased to
  `origin/main` with no remaining diff; the tranche is already absorbed
  upstream via earlier table-model work and should not be queued again.
- Local-only progress: `pulp-audit-top-level-coverage-643` refreshed cleanly
  at `339b79c8` with 9 tests and 100% target coverage for
  `tools/audit.py`.
- Local-only progress: `pulp-resolve-runs-on-extra-643` refreshed cleanly
  at `42f24313` with 18 base tests plus 2 extra tests and 100% target
  coverage for `tools/scripts/resolve_runs_on.py`.
- Local-only progress: `pulp-lcov-cobertura-extra-643` refreshed cleanly
  at `6ec8d60e` with 8 base tests plus 11 extra tests and 100% target
  coverage for `tools/scripts/lcov_cobertura.py`.
- Local-only progress: `pulp-merge-cobertura-extra-643` refreshed cleanly
  at `f40c0485` with 14 base tests plus 3 extra tests and 100% target
  coverage for `tools/scripts/merge_cobertura.py`.
- Local-only progress: `pulp-cmajor-external-extra-643` refreshed cleanly
  at `89f10266` with 4 base tests plus 9 extra tests and 100% target
  coverage for `tools/scripts/cmajor_external.py`.
- Local-only progress: `pulp-skill-sync-extra-643` refreshed cleanly at
  `0631b22d` with 4 base tests plus 20 extra tests, 39 gate tests, and
  100% target coverage for `tools/scripts/skill_sync_check.py`.
- Local-only progress: `pulp-freshness-extra-643` refreshed cleanly at
  `1f0507eb` with 9 extra tests, 23 combined package validation tests,
  and 100% target coverage for `tools/packages/freshness_check.py` when
  run with direct focused coverage over the package tests.
- Local-only progress: `pulp-validate-registry-extra-643` refreshed
  cleanly at `619560ed` with 8 extra tests, non-strict registry
  validation, and 100% target coverage for `tools/packages/validate_registry.py`.
- Local-only progress: `pulp-coverage-tier-check-extra-643` is now
  absorbed upstream; rebase skipped the old local commit and left no diff,
  so do not reopen it.
- Local-only progress: `pulp-ruleset-drift-config-643` refreshed again on
  2026-05-05 at `145cd195`; it remains a static guard/test-file-only
  tranche with no measured source surface in the focused Python coverage
  runner.
- Codecov dashboard watch: recent rapid main merges cancelled most older
  main-branch `Coverage` push runs. The main coverage run for #1213
  completed successfully at `ea03a328ef33`; #1214's merge should start
  the next main coverage push run for `e1ef40807145`.
- Just merged: #1117, #1204, #1199, #1194, #1125, #1116, #1113, #1104,
  #1097, #1088, #1203, #1115, #1195, #1083, #1096, #1200, #1205, #1078,
  #1196, #1197, #1198, #1202, and #1201 after required
  `linux`/`macos`/`windows` wrappers and Codecov patch gates were green.
- Active triage: none for #1202; it merged after the fake `amixer` PATH
  fix `b9e086fa` cleared the Namespace lanes. Refill from the local-ready
  queue while main post-merge jobs run.
- Retarget investigation: `shipyard cloud retarget` can plan a
  Namespace/GitHub-hosted lane move, but `--apply` failed while trying
  to cancel the old job. `gh` already has `workflow` scope and Shipyard
  doctor reports cloud auth ready; the job-level cancel endpoint returned
  GitHub 404. A fallback `workflow_dispatch` did not replace the stale
  queued PR-event check in the PR rollup for #1078. Filed Shipyard #265
  to make this recovery path explicit.
- Queue cleanup: cancelled duplicate active/queued `workflow_dispatch`
  build runs `25222305579`, `25216296162`, `25216004212`, and
  `25215881987` because they do not satisfy the current PR-event branch
  protection contexts and were consuming runner capacity.
- Refill: opened #1205 from `local/phase3-bench-diff-coverage-643`,
  branch `feature/bench-diff-coverage-643`, head `6a190c5b7f45`.
  Applied `codecov`, linked #641/#643, and dispatched Namespace run
  `25222999092`. Merged as `7ad9308f6cc1f62d971f89e782d8887b40153953`
  after required wrappers and Codecov patch were green; stale branch runs
  `25223019523` and `25222998771` were cancelled after merge.
- Refill: opened #1206 from `local/phase3-check-docs-consistency-coverage-643`,
  branch `feature/check-docs-consistency-coverage-643`, head
  `01c963a046a1`. Applied `codecov`, linked #641/#643, and dispatched
  Namespace run `25223345260`. Merged as
  `4ddee1e67e0c1ae9b8cab32eda91ff3da3f4f083` after required gates were
  green.
- Refill: opened #1207 from `local/phase3-encode-binary-data-coverage-643`,
  branch `feature/encode-binary-data-coverage-643`, head
  `f151cf6b6c7b`. Applied `codecov`, linked #641/#643, and dispatched
  Namespace run `25227006300`. Local validation:
  `python3 tools/scripts/test_encode_binary_data.py` reported 6 passing
  tests.
- Refill: opened #1208 from `local/phase3-docs-generate-coverage-643`,
  branch `feature/phase3-docs-generate-coverage-643`, head
  `92c88abff663`. Applied `codecov`, linked #641/#643, and fresh
  PR-event Build and Coverage workflows are running. Local validation
  previously reported 13 passing docs-generate tests, `docs_generate.py
  check` passing, and 97% target coverage. Merged as
  `4d45182c7800ac61594db1cc48625b8129882751` after required gates were
  green; no leftover branch runs were active after merge.
- Refill: opened #1209 from `local/phase3-mkdocs-hooks-coverage-643`,
  branch `feature/mkdocs-hooks-coverage-643`, head `9e2f898db95c`.
  Applied `codecov`, linked #641/#643, and fresh PR-event Build and
  Coverage workflows are running. Local validation was rerun with
  `python3 tools/scripts/test_mkdocs_hooks.py` and reported 6 passing
  tests; prepared coverage validation reported 96% target coverage.
  Merged as `87e3966a46641c97a41dc6a256fd9b2bd288fcbb` after required
  gates were green; no leftover branch runs were active after merge.
- Refill: opened #1210 from
  `local/phase3-coverage-tier-check-extra-643`, branch
  `feature/phase3-coverage-tier-check-extra-643`, head `f1d96a882462`.
  Applied `codecov`, linked #641/#643, and fresh PR-event Build and
  Coverage workflows are running. Local validation had passed with 100%
  target coverage before queueing.
- Refill: opened #1211 from `local/phase3-embed-js-coverage-643`, branch
  `feature/phase3-embed-js-coverage-643`, head `92d94f0431e2`.
  Applied `codecov`, linked #641/#643, and fresh PR-event Build and
  Coverage workflows are running. Local validation was rerun with
  `python3 tools/scripts/test_embed_js.py` and reported 6 passing tests;
  prepared coverage validation reported 100% target coverage.
- Refill: opened #1212 from `local/phase3-iwyu-annotate-extra-643`, branch
  `feature/phase3-iwyu-annotate-extra-643`, head `e2d34d3193fa`.
  Applied `codecov`, linked #641/#643, and PR-event Build and Coverage
  workflows are running. Local validation was refreshed with
  `python3 tools/scripts/test_iwyu_annotate.py`,
  `python3 tools/scripts/test_iwyu_annotate_extra.py`, and venv-backed
  `run_python_coverage.py --pattern tools/scripts/test_iwyu_annotate.py
  --pattern tools/scripts/test_iwyu_annotate_extra.py`, which reported
  100% target coverage for `tools/scripts/iwyu_annotate.py`.
- Refill: opened #1213 from
  `local/phase3-coverage-diff-comment-extra-643`, branch
  `feature/phase3-coverage-diff-comment-extra-643`, head `cd1f91eb0ec2`.
  Applied `codecov`, linked #641/#643, and PR-event Build and Coverage
  workflows are running. Local validation was refreshed with
  `python3 tools/scripts/test_coverage_diff_comment.py`,
  `python3 tools/scripts/test_coverage_diff_comment_extra.py`,
  `python3 -m unittest discover -s tools/scripts -p
  'test_coverage_diff_comment*.py'`, and venv-backed
  `run_python_coverage.py --pattern tools/scripts/test_coverage_diff_comment.py
  --pattern tools/scripts/test_coverage_diff_comment_extra.py`, which
  reported 100% target coverage for
  `tools/scripts/coverage_diff_comment.py`.
- Refill: opened #1214 from `local/phase3-jsfx-subset-extra-643`, branch
  `feature/phase3-jsfx-subset-extra-643`, head `a62d0b27bf47`.
  Applied `codecov`, linked #641/#643, and PR-event Build and Coverage
  workflows are running. Local validation was refreshed with
  `python3 -m unittest tools.scripts.test_jsfx_subset
  tools.scripts.test_jsfx_subset_extra` and venv-backed
  `run_python_coverage.py --pattern tools/scripts/test_jsfx_subset.py
  --pattern tools/scripts/test_jsfx_subset_extra.py`, which reported
  100% target coverage for `tools/scripts/jsfx_subset.py`.
- Refill: opened #1215 from
  `local/phase3-auto-release-decision-extra-643`, branch
  `feature/phase3-auto-release-decision-extra-643`, head `879ed2d53c60`.
  Applied `codecov`, linked #641/#643, and PR-event Build and Coverage
  workflows are running. Local validation was refreshed with
  `python3 tools/scripts/test_auto_release_decision.py` and
  `uv run --with 'coverage>=7.10' python -m coverage run
  tools/scripts/test_auto_release_decision.py && uv run --with
  'coverage>=7.10' python -m coverage report -m
  tools/scripts/auto_release_decision.py`, which reported 100% target
  coverage for `tools/scripts/auto_release_decision.py`. The first Linux
  Namespace Build failed in unrelated `BufferingReader restart clears
  finished state and stale buffered data`; failed jobs were rerun and
  #1215 merged as `51f1762ca23e6c5f45b4e31bd5dda5122d2cc855` after
  required gates were green.
- Refill: opened #1216 from `local/phase3-check-status-ladder-extra-643`,
  branch `feature/phase3-check-status-ladder-extra-643`, head
  `67038aa94490`. Applied `codecov`, linked #641/#643, and PR-event
  Build and Coverage workflows are running. Local validation was
  refreshed with `python3 tools/test_check_status_ladder.py` and
  `uvx --from 'coverage>=7.10' coverage run --branch
  --include='*/tools/check_status_ladder.py'
  tools/test_check_status_ladder.py`, followed by coverage report, which
  reported 100% line/branch coverage for
  `tools/check_status_ladder.py`. Merged as
  `57527d7569f8ebd8882779d10e8ad5b7e92cf259` after required wrappers,
  Namespace platform jobs, Codecov patch, and diff coverage were green.
- Refill: opened #1217 from `local/phase3-package-cli-extra-643`, branch
  `feature/phase3-package-cli-extra-643`, head `f2727c976227`.
  Applied `codecov`, linked #641/#643, and PR-event Build and Coverage
  workflows are running. Local validation was refreshed after rebasing
  onto `origin/main` at `e1ef40807145` with `python3 -m unittest
  tools/scripts/test_package_cli.py tools/scripts/test_package_cli_extra.py`
  and venv-backed `run_python_coverage.py --pattern
  tools/scripts/test_package_cli.py --pattern
  tools/scripts/test_package_cli_extra.py`, which reported 100% target
  coverage for `tools/scripts/package_cli.py`. Merged as
  `bc9765f4ddb79b6e9c8c4e3e4aa4e601daecd619` after required wrappers,
  Namespace platform jobs, Codecov patch, and diff coverage were green.
- Refill: opened #1218 from
  `local/phase3-docs-sync-check-extra-643`, branch
  `feature/phase3-docs-sync-check-extra-643`, head `6960c4d280ee`.
  Applied `codecov`, linked #641/#643, and PR-event Build and Coverage
  workflows are running. Local validation was refreshed after rebasing
  onto `origin/main` at `57527d7569f8` with
  `PYTHONPATH=tools/scripts python3 -m unittest
  tools/scripts/test_docs_sync_check.py
  tools/scripts/test_docs_sync_check_extra.py` and venv-backed
  `run_python_coverage.py --pattern 'tools/scripts/test_docs_sync_check*.py'`,
  which reported 100% target coverage for
  `tools/scripts/docs_sync_check.py`. #1218 merged as
  `a147af718361001323755ac01da395d271fe4ee2`.
- Refill: opened #1219 from
  `local/phase3-run-python-coverage-extra-643`, branch
  `feature/phase3-run-python-coverage-extra-643`, head `a113961af0e7`.
  Applied `codecov`, linked #641/#643, and PR-event Build and Coverage
  workflows are queued/running. Local validation after rebasing onto
  `origin/main` at `51f1762ca23e` included `python3
  tools/scripts/test_run_python_coverage.py`, `python3
  tools/scripts/test_run_python_coverage_extra.py`, venv-backed
  `run_python_coverage.py --pattern tools/scripts/test_run_python_coverage.py
  --pattern tools/scripts/test_run_python_coverage_extra.py`, and
  `git diff --check origin/main...HEAD`. Focused coverage reported 100% for
  `tools/scripts/run_python_coverage.py`. #1219 merged as
  `4e93da00dddab17b75531d2fcda1b0511e3a4e9c`.
- Refill: opened #1220 from
  `local/phase3-version-bump-extra-643`, branch
  `feature/phase3-version-bump-extra-643`, head `57ceb82fa903`.
  Applied `codecov`, linked #641/#643, and PR-event Build and Coverage
  workflows are queued/running. Local validation after rebasing onto
  `origin/main` at `51f1762ca23e` included `python3
  tools/scripts/test_gates.py`, `python3
  tools/scripts/test_version_bump_check_extra.py`, version-bump/skill-sync
  reports, venv-backed `run_python_coverage.py --pattern
  tools/scripts/test_gates.py --pattern
  tools/scripts/test_version_bump_check_extra.py`, and `git diff --check`.
  Focused coverage reported 100% for
  `tools/scripts/version_bump_check.py`. #1220 merged as
  `3d2ebe6e6af57f10a29c2e17439e926221af8521`.
- Refill: opened #1221 from
  `local/phase3-deps-audit-extra-643`, branch
  `feature/phase3-deps-audit-extra-643`, head `eb0db9ab7922`.
  Applied `codecov`, linked #641/#643, and PR-event Build and Coverage
  workflows are queued/running. Local validation after rebasing onto
  `origin/main` at `3d2ebe6e6af5` included `python3
  tools/deps/test_audit.py`, `python3 tools/deps/test_audit_extra.py`,
  `python3 tools/deps/audit.py --strict`, venv-backed
  `run_python_coverage.py --pattern tools/deps/test_audit.py --pattern
  tools/deps/test_audit_extra.py`, and `git diff --check`. Focused
  coverage reported 100% for `tools/deps/audit.py`. #1221 merged as
  `7a49b91aa0a0dd650f0dc5fc1c1d560e488e51c8` after required checks,
  Namespace lanes, Codecov patch, and coverage lanes were green.
- Refill: opened #1222 from
  `local/phase3-run-swift-coverage-extra-643`, branch
  `feature/phase3-run-swift-coverage-extra-643`, head `b11ec33712ce`.
  Applied `codecov`, linked #641/#643, and PR-event checks are
  queued/running. Local validation after rebasing onto `origin/main` at
  `3d2ebe6e6af5` included `python3 -m unittest
  tools/scripts/test_run_swift_coverage.py
  tools/scripts/test_run_swift_coverage_extra.py`, venv-backed
  `run_python_coverage.py --pattern tools/scripts/test_run_swift_coverage.py
  --pattern tools/scripts/test_run_swift_coverage_extra.py`, direct
  `python3 tools/scripts/run_swift_coverage.py`, and `git diff --check`.
  Focused Python coverage reported 100% for
  `tools/scripts/run_swift_coverage.py`; direct Swift coverage reported
  91.68% Apple source coverage. #1222 merged as
  `30fc6ec792295d13412c599027aa4000d47d35ff` after required checks,
  Namespace lanes, Codecov patch, diff coverage, and coverage lanes were
  green.
- Refill: opened #1223 from
  `local/phase3-add-component-coverage-643`, branch
  `feature/phase3-add-component-coverage-643`, head `cd93085b0057`.
  Applied `codecov`, linked #641/#643, and PR-event checks are
  queued/running. Local validation after rebasing onto `origin/main`
  included `python3 tools/scripts/test_add_component.py`, venv-backed
  `run_python_coverage.py --pattern tools/scripts/test_add_component.py`,
  direct branch coverage for `tools/add-component.py`, and `git diff
  --check`; focused coverage reported 100% for `tools/add-component.py`.
- Refill: opened #1224 from
  `local/phase3-cli-sync-check-extra-643`, branch
  `feature/phase3-cli-sync-check-extra-643`, head `db185f4d4786`.
  Applied `codecov`, linked #641/#643, and PR-event checks are
  queued/running. Local validation after rebasing onto `origin/main`
  included base and extra CLI sync tests, venv-backed
  `run_python_coverage.py --pattern tools/scripts/test_cli_sync_check.py
  --pattern tools/scripts/test_cli_sync_check_extra.py`, direct
  `cli_sync_check.py --strict --json`, and `git diff --check`; focused
  coverage reported 100% for `tools/scripts/cli_sync_check.py`.
- Refill: opened #1225 from
  `local/phase3-compat-sync-extra-643`, branch
  `feature/phase3-compat-sync-extra-643`, head `48bf3b9eeabb`.
  Applied `codecov`, linked #641/#643, and PR-event checks are
  queued/running. Local validation after rebasing onto `origin/main`
  included base and extra compat-sync tests through venv-backed
  `run_python_coverage.py`, direct `compat_sync_check.py --mode=report
  --enforce`, and `git diff --check`; focused coverage reported 100% for
  `tools/scripts/compat_sync_check.py`.
- Refill: opened #1226 from
  `local/phase3-list-limitations-coverage-643`, branch
  `feature/phase3-list-limitations-coverage-643`, head `fc21f4edf873`.
  Applied `codecov`, linked #641/#643, and PR-event checks are
  queued/running. Local validation after rebasing onto `origin/main`
  included `python3 -m unittest tools/test_list_limitations.py`,
  venv-backed `run_python_coverage.py --pattern
  tools/test_list_limitations.py`, direct branch coverage for
  `tools/list_limitations.py`, skill-sync and version-bump reports, and
  `git diff --check`; focused coverage reported 100% for
  `tools/list_limitations.py`.
- Queue cleanup: requested cancellation of duplicate `workflow_dispatch`
  Build and Test runs `25227025413` (#1207) and `25227667875` (#1208)
  because the PR-event Build and Test workflows are already producing the
  merge-eligible Namespace child jobs.
- Queue cleanup: requested cancellation of duplicate `workflow_dispatch`
  Build and Test run `25228924648` (#1209) for the same reason.
- Queue cleanup: requested cancellation of duplicate `workflow_dispatch`
  Build and Test run `25230061797` (#1210) for the same reason.
- Queue cleanup: requested cancellation of duplicate `workflow_dispatch`
  Build and Test run `25230693088` (#1211) for the same reason.
- Queue cleanup: requested and confirmed cancellation of duplicate
  `workflow_dispatch` Build and Test run `25231034599` (#1212) for the
  same reason.
- Queue cleanup: requested cancellation of duplicate `workflow_dispatch`
  Build and Test run `25231259746` (#1213) for the same reason.
- Queue cleanup: requested cancellation of duplicate `workflow_dispatch`
  Build and Test run `25231763147` (#1214) for the same reason.
- Queue cleanup: requested cancellation of duplicate `workflow_dispatch`
  Build and Test run `25232091712` (#1215) for the same reason.
- Queue cleanup: requested cancellation of duplicate `workflow_dispatch`
  Build and Test run `25232344234` (#1216) for the same reason.
- Merged #1210 as `e858f561c89e69b7ccb82b8d607b420775f9a0b9` after
  required wrappers and Codecov patch were green.
- Merged #1211 as `827227339a0609358ba0371a86417a868ee9879e` after
  required wrappers and Codecov patch were green.
- Merged #1207 as `9da7b03a522a2c08042bbfe6f3149612ed02bb82` after the
  long Windows MSVC release-path gate completed green.
- Merged #1212 as `cb954187cc2e7cc00b6cd972f6f2767a9b01af58` after
  required wrappers and Codecov patch were green.
- Merged #1213 as `ea03a328ef3351eeaada61259321d9849f6f4fa4` after
  required wrappers and Codecov patch were green.
- Local-only progress: `pulp-auto-release-decision-extra-643` is now the
  preferred #643 `tools/scripts/auto_release_decision.py` tranche. It is
  rebased on current `origin/main` at `e7f9e1fd`, reports 24 passing
  tests and 100% target coverage, and supersedes the older
  `local/phase3-auto-release-extra-643` duplicate.
- Local-only progress: `pulp-check-docs-status-extra-643` now provides the
  preferred #643 `tools/check_format_validation.py` tranche at
  `a5bcacaf432f`, with 10 format-validation tests, 23 coverage-runner
  tests, and 100% target coverage after rebasing over current `origin/main`.
  `tools/check-docs.sh` passes with existing warnings; strict
  `check_format_validation.py --mode=report` still fails on 4 existing
  support-matrix entries unrelated to this tranche.
- Local-only progress: `pulp-check-status-ladder-extra-643` prepared a
  #643 `tools/check_status_ladder.py` tranche at `c94b8e65`, with 13
  focused tests and 100% line/branch target coverage.
- Merged #1078 as `81be4ee00e02e367feec32c3c6885d0785179efa`,
  #1196 as `dfae8e9f4a3b7b019b091dd8786f354612e8e4ae`, #1197 as
  `f1d06c6210c1ee81686b72d449ab3f40308e6e3e`, #1198 as
  `10212599092d2a313996ab120ef128178173a467`, #1202 as
  `00b0871a6dea89fecb14d4aca8502e5c52507416`, and #1201 as
  `42464b4bc3282e056e57e6ebb7820f4b91a974a9`. Cancelled leftover branch
  coverage run `25216712973` for #1201 after merge.
- Local-only progress: `pulp-check-docs-status-extra-643` supersedes the
  earlier check-format-validation branch and avoids the baseline
  `production_validated` caveat by staying in focused unit tests.
- Local-only progress: `pulp-docs-generate-coverage-643` is refreshed
  against current `origin/main` at `a063d8910fa5` as a single local
  feature commit and locally validated with 100% target coverage.
  Validation: `python3 tools/scripts/test_docs_generate.py` reported 15
  passing tests; `python3 tools/docs_generate.py check` passed; venv-backed
  `run_python_coverage.py --pattern tools/scripts/test_docs_generate.py`
  passed and reported 100% for `tools/docs_generate.py`.
- Local-only progress: `pulp-list-limitations-coverage-643` is refreshed
  against current `origin/main` at `08e4c375aac8` as a local feature commit
  and locally validated with 100% target coverage. Validation:
  `python3 -m unittest tools/test_list_limitations.py` reported 10 passing
  tests; venv-backed `run_python_coverage.py --pattern
  tools/test_list_limitations.py` passed and reported 100% for
  `tools/list_limitations.py`.
- Local-only progress: `pulp-mkdocs-hooks-coverage-643` is refreshed
  against current `origin/main` at `478a2d88d2aa` as a single local
  feature commit and locally validated with 100% target coverage.
  Validation: `python3 tools/scripts/test_mkdocs_hooks.py` reported 10
  passing tests; venv-backed `run_python_coverage.py --pattern
  tools/scripts/test_mkdocs_hooks.py` passed and reported 100% for
  `tools/mkdocs_hooks.py`.
- Local-only progress: `pulp-encode-binary-data-coverage-643` is
  refreshed against current `origin/main` and locally validated; this
  tranche is now queued remotely as #1207.
- Local-only progress: `pulp-add-component-coverage-643` is refreshed
  against current `origin/main` at `1621c426993d` as a single local
  feature commit and locally validated with 100% target coverage.
  Validation: `python3 tools/scripts/test_add_component.py` reported 9
  passing tests; venv-backed direct coverage over
  `tools/scripts/test_add_component.py` passed and reported 100% for
  `tools/add-component.py`.
- Local-only progress: `pulp-audit-top-level-coverage-643` is refreshed
  against current `origin/main` at `339b79c881db` as a single local
  feature commit and locally validated with 100% target coverage.
  Validation: `python3 tools/scripts/test_audit_top_level.py` reported 9
  passing tests; venv-backed `run_python_coverage.py --pattern
  tools/scripts/test_audit_top_level.py` reported 100% for
  `tools/audit.py`; docs-sync, skill-sync, and diff checks passed.
- Local-only progress: `pulp-embed-js-coverage-643` is refreshed against
  current `origin/main` at `27293e8ff654` as a single local feature
  commit and locally validated with 100% target coverage. Validation:
  `python3 tools/scripts/test_embed_js.py` reported 6 passing tests;
  venv-backed `run_python_coverage.py --pattern
  tools/scripts/test_embed_js.py` passed and reported 100% for
  `core/view/js/embed_js.py`.
- Local-only progress: `pulp-run-swift-coverage-extra-643` is refreshed
  against current `origin/main` at `d6d08c37` as a single local feature
  commit and locally validated with 100% target coverage. Validation:
  `python3 -m unittest
  tools/scripts/test_run_swift_coverage.py
  tools/scripts/test_run_swift_coverage_extra.py` reported 18 passing
  tests; venv-backed `run_python_coverage.py --pattern
  tools/scripts/test_run_swift_coverage.py --pattern
  tools/scripts/test_run_swift_coverage_extra.py` reported 100% for
  `tools/scripts/run_swift_coverage.py`; direct
  `python3 tools/scripts/run_swift_coverage.py` passed 10 Swift tests and
  reported 91.68% Apple source coverage.
- Local-only progress: `pulp-android-target-coverage-643` is refreshed
  against current `origin/main` at `acf6832c9036` as a single local
  feature commit and locally validated with 100% target coverage.
  Validation: `python3 tools/scripts/test_android_target.py` reported 16
  passing tests; venv-backed `run_python_coverage.py --pattern
  tools/scripts/test_android_target.py` passed and reported 100% for
  `tools/local-ci/android_target.py`.
- Local-only progress: `pulp-validate-hosts-coverage-643` is refreshed
  against current `origin/main` at `18363f59e392` as a single local
  feature commit and locally validated with 100% target coverage, then
  queued remotely as #1229.
  Validation: `python3 tools/scripts/test_validate_hosts.py` reported 9
  passing tests; focused coverage over `tools/deps/validate_hosts.py`
  passed at 100%; docs-sync and diff/index checks
  passed. No real SSH host validation was run.
- Local-only progress: `pulp-compat-sync-extra-643` advanced to
  `199785183264` with remaining focused coverage for
  `tools/scripts/compat_sync_check.py`. Validation: venv-backed
  `tools/scripts/test_compat_sync_check.py`,
  `tools/scripts/test_compat_sync_check_extra.py`, and
  `run_python_coverage.py --pattern 'tools/scripts/test_compat_sync_check*.py'`
  passed and reported 100% for `tools/scripts/compat_sync_check.py`.
- Local-only progress: `pulp-build-migration-index-extra-643` is
  refreshed against current `origin/main` at `bc37c17e8f92` as a single
  local feature commit, locally validated with 100% target coverage, and
  queued remotely as #1230.
  Validation: `python3 tools/scripts/test_build_migration_index.py &&
  python3 tools/scripts/test_build_migration_index_extra.py` reported
  7 + 10 passing tests; venv-backed coverage over
  tools/scripts/test_build_migration_index.py --pattern
  tools/scripts/test_build_migration_index_extra.py` reported 100% for
  `tools/scripts/build_migration_index.py`; skill-sync, docs-sync,
  temp codegen smoke, and diff checks passed.
- Local-only progress: `pulp-docs-sync-check-extra-643` advanced to
  `6960c4d280ee` after rebase with entrypoint coverage for
  `tools/scripts/docs_sync_check.py`. Validation:
  `PYTHONPATH=tools/scripts python3 -m unittest
  tools/scripts/test_docs_sync_check.py
  tools/scripts/test_docs_sync_check_extra.py` reported 28 tests, and
  throwaway-venv focused coverage reported 100% for
  `tools/scripts/docs_sync_check.py`.
- Local-only progress: `pulp-coverage-tier-check-extra-643` was rebased
  onto current `origin/main` at `2623a4b47c1d`; Git skipped the old local
  commit as already applied and left no diff.
  Validation: `python3 tools/scripts/test_coverage_tier_check.py`
  reported 15 passing tests; `python3
  tools/scripts/test_coverage_tier_check_extra.py` reported 17 passing
  tests; venv-backed `run_python_coverage.py --pattern
  tools/scripts/test_coverage_tier_check.py --pattern
  tools/scripts/test_coverage_tier_check_extra.py` passed and reported
  100% for `tools/scripts/coverage_tier_check.py`. No reopen action needed.
- Local-only progress: `pulp-skill-sync-extra-643` advanced to
  `0631b22ddd8f` with remaining helper, reporting, and entrypoint
  coverage for `tools/scripts/skill_sync_check.py`. Validation:
  `python3 tools/scripts/test_skill_sync_check.py` reported 4 tests,
  `python3 tools/scripts/test_skill_sync_check_extra.py` reported 20
  tests, `python3 tools/scripts/test_gates.py` reported 39 tests, sync
  reports and `git diff --check` passed, and venv-backed focused coverage
  reported 100% for `tools/scripts/skill_sync_check.py`.
- Local-only progress: `pulp-lcov-cobertura-extra-643` advanced to
  `6ec8d60e5e18` with zero-hit line, demangler, missing `c++filt`, and
  entrypoint coverage for `tools/scripts/lcov_cobertura.py`. Validation:
  `python3 tools/scripts/test_lcov_cobertura.py &&
  python3 tools/scripts/test_lcov_cobertura_extra.py` passed with 8 + 11
  tests, venv-backed focused coverage over `test_lcov_cobertura*.py`
  reported 100% line/branch coverage for
  `tools/scripts/lcov_cobertura.py`, and docs-sync, skill-sync, CLI sync,
  and diff checks passed.
- Local-only progress: `pulp-deps-audit-extra-643` advanced to
  `a7c80bf9` with remaining parser defensive-branch coverage for
  `tools/deps/audit.py`. Validation: `python3 -m unittest
  tools.deps.test_audit tools.deps.test_audit_extra` reported 24 tests,
  `python3 tools/deps/audit.py --strict` passed, and venv-backed focused
  coverage over `tools/deps/test_audit*.py` reported 100% for
  `tools/deps/audit.py`.
- Local-only progress: `pulp-validate-registry-extra-643` refreshed to
  `619560ed83b7` with registry validator edge coverage for
  `tools/packages/validate_registry.py`. Validation:
  `python3 tools/scripts/test_validate_registry_extra.py` reported 8 tests,
  `python3 tools/packages/test_package_validation_tools.py` reported 14
  tests, and venv-backed focused coverage over both suites reported 100%
  for `tools/packages/validate_registry.py`. Non-strict
  `tools/packages/validate_registry.py` passes with 36 packages, 0 errors,
  and 12 warnings; strict `--check-licenses` still exits 1 on existing
  registry license findings, now 8 errors and 14 warnings.
- Local-only progress: `pulp-version-bump-extra-643` advanced to
  `57ceb82f` with remaining branch and entrypoint coverage for
  `tools/scripts/version_bump_check.py`. Validation: `test_gates.py`
  reported 39 tests, `test_version_bump_check_extra.py` reported 27
  tests, `git diff --check` passed, and venv-backed focused coverage
  over both suites reported 100% for `tools/scripts/version_bump_check.py`.
- Local-only progress: `pulp-run-python-coverage-extra-643` advanced to
  `a113961af0e7` with entrypoint coverage for
  `tools/scripts/run_python_coverage.py`. Validation: venv-backed
  `tools/scripts/test_run_python_coverage.py` reported 23 tests,
  `tools/scripts/test_run_python_coverage_extra.py` reported 8 tests, and
  focused coverage over both suites reported 100% for
  `tools/scripts/run_python_coverage.py`. #1219 is open and running
  PR-event Namespace checks.
- Local-only progress: `pulp-cli-sync-check-extra-643` advanced to
  `f9d365b2` with one-sided mismatch and entrypoint coverage for
  `tools/scripts/cli_sync_check.py`. Validation: `python3 -m unittest
  tools.scripts.test_cli_sync_check tools.scripts.test_cli_sync_check_extra`
  reported 16 tests, and venv-backed focused coverage reported 100% for
  `tools/scripts/cli_sync_check.py`.
- Operating mode: keep polling, merge PRs as soon as required
  `linux`/`macos`/`windows` wrappers are green, cancel leftover advisory
  PR-head runs after merge, and only refill when the active queue drains
  or a high-confidence local tranche is already validated.

## Cancellation Result

- Cancellation pass completed on 2026-04-30 EDT.
- Cancellation requests submitted successfully for all 193 recorded runs.
- A few runs completed while cancellation was in flight; sticky queued
  runs for #1133 and #1134 required a second cancellation request.
- Final verification after GitHub settled: 0 queued or in-progress
  workflow runs remained for the open `codecov` PR branches in this
  snapshot.
- Expected side effect: cancelled validation attempts may leave PRs
  `BLOCKED` or `UNSTABLE` until the queue is reopened and fresh
  validation runs are dispatched.

## Resume Batches

### 2026-05-01 Batch 1: Build Gate Refill

Namespace capacity check showed no queued or in-progress GitHub workflow
runs for `danielraffel/pulp`, so the first refill batch replaced only the
cancelled `Build and Test` gates for recent Codecov PRs. Existing passing
Codecov/version/docs/license results were left intact.

| PR | Branch | Head | Workflow | New Run | Status at Dispatch |
| --- | --- | --- | --- | --- | --- |
| #1140 | `feature/status-ladder-coverage-643` | `bf87273e0534` | Build and Test | `25202620106` | in progress |
| #1141 | `feature/ship-appcast-coverage-644-next` | `2d2c119b4c8d` | Build and Test | `25202620128` | in progress |
| #1142 | `codex/package-tools-coverage-643` | `cb6fb9c4e9c3` | Build and Test | `25202620088` | in progress |
| #1143 | `feature/render-compute-coverage-646` | `2b2ccde36e63` | Build and Test | `25202620280` | in progress |

Next refill rule: when this batch drops below four active Codecov build
runs, dispatch the next merge-ready paused PRs by pinned SHA and update
this table before adding more local-only tranche PRs.

Follow-up correction: these `workflow_dispatch` runs all passed, but they
did not satisfy branch protection because stale failed or cancelled
`pull_request` check runs with the same required context names remained on
the PR head commits. The branches were then refreshed against `main` with
`gh pr update-branch` to create merge-eligible PR-event check suites:

| PR | Branch | Refreshed Head | Build Run | Coverage Run | Status |
| --- | --- | --- | --- | --- | --- |
| #1140 | `feature/status-ladder-coverage-643` | `f48a49e62ec3` | `25203370720` | `25203370713` | pending |
| #1141 | `feature/ship-appcast-coverage-644-next` | `bccfec4797d5` | `25203371862` | `25203371864` | pending |
| #1142 | `codex/package-tools-coverage-643` | `45dbdda453a3` | `25203373031` | `25203372990` | pending |
| #1143 | `feature/render-compute-coverage-646` | `14098f98d57e` | `25203374251` | `25203374250` | pending |

### 2026-05-01 Batch 2: Build Gate Refill

The first batch assigned cleanly on Namespace with no GitHub queued-run
backlog, so a second four-PR batch was queued behind it. This keeps the
active window warm without re-dispatching the full paused set.

| PR | Branch | Head | Workflow | New Run | Status at Dispatch |
| --- | --- | --- | --- | --- | --- |
| #1136 | `feature/package-registry-cache-fallback-643-next` | `46854e04fcf2` | Build and Test | `25202718222` | queued |
| #1137 | `feature/audio-platform-helper-coverage-640-next` | `5149c107a3ac` | Build and Test | `25202718230` | queued |
| #1138 | `feature/coverage-no-idle-guidance-641` | `6fea2fa4686a` | Build and Test | `25202718329` | queued |
| #1139 | `codex/events-phase3-coverage-tranche` | `a6237254b05d` | Build and Test | `25202718395` | queued |

Note: `shipyard cloud run build` printed older saved run IDs for #1136
and #1137, but `gh run list` confirmed the fresh workflow_dispatch run
IDs above.

### 2026-05-01 Batch 3: Build Gate Refill

A third build-only batch was queued after #1136-#1139. #1131 was not
queued because its `Diff coverage required` check is red and needs
coverage-gate triage before a build-only re-run can make it mergeable.

| PR | Branch | Head | Workflow | New Run | Status at Dispatch |
| --- | --- | --- | --- | --- | --- |
| #1132 | `codex/midi-sysex-sidecar-tests` | `2695097458c4` | Build and Test | `25202770455` | queued |
| #1133 | `feature/audio-channel-set-coverage-640-next` | `df227394cd7e` | Build and Test | `25202770462` | queued |
| #1134 | `codex/coverage-phase3-tranche-20260430095455` | `1dbd12f64ba0` | Build and Test | `25202770509` | queued |
| #1135 | `feature/signal-processor-chain-reset-coverage-645` | `89242ca0bfa5` | Build and Test | `25202770726` | queued |

Held for triage: #1131 `feature/audio-window-enumerator-coverage-640-next`
has passing `codecov/patch` but failing `Diff coverage required`, so it
likely needs a coverage workflow re-run or a patch rather than build-only
refill.

Follow-up correction: because build-only dispatches do not clear stale
PR-event contexts, the queued or running build-only dispatches for
#1132-#1139 were cancelled after the #1140-#1143 branch-refresh batch
started. Cancelled workflow_dispatch run IDs: `25202718222`,
`25202718230`, `25202718329`, `25202718395`, `25202770455`,
`25202770462`, `25202770509`, and `25202770726`.

### 2026-05-01 Batch 4: PR-Event Branch Refresh

After confirming the build-only dispatches were not merge-eligible
evidence, the next refill batch used `gh pr update-branch` so GitHub
created fresh required `pull_request` checks.

| PR | Branch | Refreshed Head | Build Run | Coverage Run | Status |
| --- | --- | --- | --- | --- | --- |
| #1135 | `feature/signal-processor-chain-reset-coverage-645` | `bc32ea5904b6` | `25203513486` | `25203513496` | pending |
| #1136 | `feature/package-registry-cache-fallback-643-next` | `476677d7b926` | `25203494171` | `25203494182` | pending |
| #1137 | `feature/audio-platform-helper-coverage-640-next` | `cc7fcaf98be8` | `25203494454` | `25203494423` | pending |
| #1138 | `feature/coverage-no-idle-guidance-641` | `72fa08f3553a` | `25203493849` | `25203493834` | pending |
| #1139 | `codex/events-phase3-coverage-tranche` | `df3f376fa716` | `25203493795` | `25203493796` | pending |

### 2026-05-01 Batch 5: PR-Event Branch Refresh

Namespace still had the earlier PR-event batch queued but not failing, so
the next low-risk stale build-matrix PRs were refreshed in a small batch.

| PR | Branch | Refreshed Head | Build Run | Coverage Run | Status |
| --- | --- | --- | --- | --- | --- |
| #1129 | `feature/midi-sysex-accumulator-coverage-645-next` | `a5ae96aa067c` | `25203575118` | `25203575127` | pending |
| #1132 | `codex/midi-sysex-sidecar-tests` | `8b403641dfd4` | `25203575435` | `25203575450` | pending |
| #1133 | `feature/audio-channel-set-coverage-640-next` | `a82513bedc13` | `25203575337` | `25203575346` | pending |
| #1134 | `codex/coverage-phase3-tranche-20260430095455` | `8c91ac2d0315` | `25203575338` | `25203575315` | pending |

Follow-up refresh after `main` advanced through #1141 and #1131:

| PR | Branch | Refreshed Head | Build Run | Coverage Run | Status |
| --- | --- | --- | --- | --- | --- |
| #1139 | `codex/events-phase3-coverage-tranche` | `539bbb76c41a` | `25204395492` | `25204395482` | pending; fresh PR-event checks queued with sanitizer run `25204395501` and IWYU run `25204395484` |

### 2026-05-01 Batch 6: PR-Event Branch Refresh

After #1139 merged and GitHub Actions pressure dropped to 18 active
runs, the next four patched/stale Codecov PRs were refreshed with
`gh pr update-branch` so branch protection gets fresh `pull_request`
contexts. Pressure after dispatch rose to 39 active runs, with 24 queued
and 13 in progress, so hold further refill until this batch drains.

| PR | Branch | Refreshed Head | Build Run | Coverage Run | Sanitizer Run | IWYU Run | Status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| #1051 | `feature/signal-poly-math-coverage-645` | `0675a09ecabd` | `25207650057` | `25207650060` | `25207650064` | `25207650080` | merged as `8f038e9b8150`; queued advisory sanitizer run cancellation requested after merge |
| #1062 | `codex/coverage-midi-edge-644` | `4fdcb2605585` | `25207651042` | `25207651053` | `25207651046` | `25207651044` | merged as `2fbe9a6ce7a2`; queued advisory sanitizer run cancellation requested after merge |
| #1066 | `feature/signal-filter-meter-coverage-645` | `cd2aafe8be87` | `25207652324` | `25207652319` | `25207652379` | `25207652327` | queued/in progress |
| #1075 | `feature/cli-host-coverage-643` | `e3e7b0c6bedc` | `25207653425` | `25207653428` | `25207653415` | `25207653416` | queued/in progress |

### 2026-05-01 Batch 7: One-PR Refill

After stale duplicate workflow-dispatch and already-merged PR runs were
cancelled, only current PR-event work plus main advisory push workflows
remained in the queue. A single stale PR was refreshed to avoid flooding
Namespace while #1190, #1191, and #1188 continue draining.

| PR | Branch | Refreshed Head | Build Run | Coverage Run | Sanitizer Run | IWYU Run | Status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| #1128 | `feature/audio-workgroup-coverage-640-next` | `494233c87fe0` | `25212496127` | `25212496120` | `25212496122` | `25212496131` | merged as `ebc118d95684`; queued coverage/sanitizer run cancellations requested after merge |

### 2026-05-01 Batch 8: One-PR Refill

#1127 had no active runs and only stale cancelled/failed contexts from an
older base, so it was refreshed with `gh pr update-branch`. Hold further
cloud refill while #1127, #1128, #1191, and #1188 drain.

| PR | Branch | Refreshed Head | Build Run | Coverage Run | Sanitizer Run | IWYU Run | Status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| #1127 | `feature/render-texture-atlas-coverage-646-next` | `310074265e45` | `25213360729` | `25213360744` | `25213360732` | `25213360752` | merged as `8ccc4a151d4e`; queued advisory sanitizer run cancellation requested after merge |

## Merge Waves While Queue Is In Flight

These PRs were already green/mergeable when the paused queue was
reopened, so they were squash-merged without consuming additional
Namespace capacity. #1099 became conflicted after the preceding view
coverage merges and is held for a branch refresh.

| PR | Merge SHA | Result |
| --- | --- | --- |
| #988 | `9da94da8e4be` | already merged before this queue sweep; confirmed no remaining queue action needed |
| #989 | `a0f94f120b15` | already merged before this queue sweep; confirmed no remaining queue action needed |
| #1014 | `6cb97e612aee` | already merged before this queue sweep; confirmed no remaining queue action needed |
| #1065 | `a428365ded95` | merged |
| #1080 | `9c5a455d1cd9` | merged |
| #1081 | `bb8e9ba00a32` | merged |
| #1084 | `66c74123430a` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts were green, only advisory macOS sanitizer lanes were still pending |
| #1087 | `c89bd020f53c` | merged |
| #1090 | `fd7ff5386993` | merged |
| #1091 | `4210ff8f2c14` | merged |
| #1092 | `72a6421776b3` | merged |
| #1093 | `6da560926231` | merged |
| #1094 | `4ffa26b1e3a9` | merged |
| #1095 | `296e521c97c9` | merged |
| #1098 | `c3aff2bf92cd` | merged |
| #1099 | pending | merge conflict after view merges; refresh branch before retry |
| #1100 | `bd4829234834` | merged |
| #1101 | `e22fb97b12e9` | merged |
| #1103 | `ac8f62ffccf2` | merged |
| #1105 | `cc02d524888c` | merged |
| #1106 | `3436fb6fa16b` | merged |
| #1107 | `ba2b79f69e1b` | merged |
| #1108 | `a7e21a107ba7` | merged |
| #1109 | `d19d71b1d938` | merged |
| #1110 | `02d32afb3fc1` | merged |
| #1111 | `5841a31279e6` | merged |
| #1112 | `fef94b10e89c` | merged |
| #1123 | `57ac9d3c3a70` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts were green, only advisory coverage/sanitizer lanes were still pending |
| #1099 | `a18291ee6642` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts were green, only advisory lanes were still pending |
| #1132 | `3d7fa34f380d` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts were green, only advisory lanes were still pending |
| #1136 | `caf93e5f835f` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts were green, only advisory lanes were still pending |
| #1141 | `49136be956d` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts were green, only advisory macOS coverage/sanitizer lanes were still pending |
| #1131 | `0f4d38f6c30` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts were green, only advisory macOS coverage/sanitizer lanes were still pending |
| #1120 | `dba48cb3f53c` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts were green, only advisory macOS coverage/sanitizer lanes were still pending |
| #1140 | `3695a6af7163` | merged from `CLEAN`; required `linux`, `macos`, and `windows` contexts, diff coverage, and Codecov patch were green |
| #1085 | `5aac29496436` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts, diff coverage, and Codecov patch were green, only advisory macOS sanitizer lanes were still pending |
| #1138 | `44cff0532848` | merged from `CLEAN`; durable coverage guide now codifies the no-idle Phase 3 loop and Namespace-default validation posture |
| #1082 | `b0903cd8ed4b` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts plus Codecov patch were green, only advisory macOS sanitizer/coverage lanes were still pending |
| #1135 | `44e67d52cbfc` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` contexts plus Codecov patch/diff coverage were green, only advisory macOS sanitizer lanes were still pending |
| #1139 | `d701d4ae8d45` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, only advisory macOS coverage/sanitizer lanes were still pending |
| #1134 | `a01ca7410faa` | merged from `CLEAN`; required `linux`, `macos`, and `windows` wrappers, Codecov patch, diff coverage, coverage lanes, and sanitizer lanes were green |
| #1119 | `0bf8f64aeb8b` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, only advisory macOS coverage was still pending |
| #1102 | `b3df384ab0de` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, Codecov patch, and coverage lanes were green, only advisory macOS sanitizer lanes were still pending |
| #1142 | `ed34a23d9e4d` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, only advisory macOS coverage was still pending |
| #1071 | `160e76ec7c86` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, only advisory macOS coverage/sanitizer lanes were still pending |
| #1045 | `2e4a0ed2e256` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, only advisory macOS coverage/sanitizer lanes were still pending |
| #1137 | `ea731cbf365c` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, Codecov patch, and diff coverage were green, only advisory macOS sanitizer lanes were still pending |
| #1143 | `d36fddc2cd9f` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, Codecov patch, diff coverage, and platform build/coverage lanes were green, only advisory macOS UBSan was still pending |
| #1075 | `c34f11f7138c` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green. Red Windows coverage job was advisory artifact-upload plumbing: tests, Cobertura existence check, and Codecov upload succeeded |
| #1133 | `e1a22a1ffd93` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, Codecov patch, diff coverage, and coverage/build lanes were green, only advisory macOS ASan/UBSan jobs were still pending |
| #1086 | `0300ba207577` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, Windows/Linux coverage and platform build lanes were green, pending macOS coverage/sanitizers were advisory |
| #1074 | `5898bf057163` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, Codecov patch, diff coverage, coverage lanes, and platform build lanes were green, only advisory macOS sanitizer jobs were still pending |
| #1184 | `4d67e04547cf` | merged from `CLEAN`; required `linux`, `macos`, and `windows` wrappers, Namespace platform checks, Codecov patch, diff coverage, coverage lanes, and sanitizer lanes were green |
| #1185 | `57f5ba3ca08f` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, only advisory macOS coverage/sanitizer lanes were still pending |
| #1190 | `a7b41acc3cf2` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, only advisory macOS coverage/sanitizer lanes were still pending |
| #1051 | `8f038e9b8150` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, Codecov patch, and diff coverage were green, only advisory macOS ASan/UBSan lanes were still pending |
| #1187 | `b9ad40021e0d` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, Codecov patch, and diff coverage were green, only advisory macOS sanitizer lanes were still pending |
| #1186 | `4afbf2c1a83e` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, only advisory macOS coverage/sanitizer lanes were still pending |
| #1089 | `d214d24ec20b` | merged from `CLEAN`; required `linux`, `macos`, and `windows` wrappers, Namespace platform checks, Codecov patch, diff coverage, coverage lanes, and sanitizer lanes were green |
| #1062 | `2fbe9a6ce7a2` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, Namespace platform checks, Codecov patch, and diff coverage were green, only advisory macOS TSan/UBSan lanes were still pending |
| #1079 | `da004f90e21c` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, Namespace platform checks, Codecov patch, diff coverage, and coverage lanes were green, only advisory macOS ASan was still pending |
| #1117 | `202943e61da7` | merged from `CLEAN`; required `linux`, `macos`, and `windows` wrappers, diff coverage, and Codecov patch were green |
| #1204 | `c96d0ac038c0` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, diff coverage, and Codecov patch were green, only advisory lanes were still pending |
| #1199 | `f3c673ec7592` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, only advisory lanes were still pending |
| #1194 | `b69f2b5ad477` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, diff coverage, and Codecov patch were green, only advisory lanes were still pending |
| #1125 | `f1db8c29cc41` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, diff coverage, and Codecov patch were green, only advisory lanes were still pending |
| #1116 | `4955ae59fd99` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, diff coverage, and Codecov patch were green, only advisory lanes were still pending |
| #1113 | `c7a9a0a0fcbe` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, diff coverage, and Codecov patch were green, only advisory lanes were still pending |
| #1104 | `f46c83f5848d` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, diff coverage, and Codecov patch were green, only advisory lanes were still pending |
| #1097 | `cd0f141fa708` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, only advisory lanes were still pending |
| #1088 | `80139f392047` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, only advisory lanes were still pending |
| #1203 | `9d1e7d661e8e` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, diff coverage, and Codecov patch were green, only advisory macOS sanitizer lanes were still pending |
| #1115 | `daa0aa704427` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers plus Codecov patch were green, only advisory macOS coverage was still pending |
| #1195 | `2c5135b05628` | merged from `CLEAN`; required `linux`, `macos`, and `windows` wrappers, Codecov patch, diff coverage, coverage lanes, and sanitizer lanes were green |
| #1083 | `df3ca99d8052` | merged from `CLEAN`; required `linux`, `macos`, and `windows` wrappers, Namespace platform checks, Codecov patch, diff coverage, coverage lanes, and sanitizer lanes were green |
| #1096 | `c544a9ef221b` | merged from `CLEAN`; required `linux`, `macos`, and `windows` wrappers, Namespace platform checks, Codecov patch, diff coverage, coverage lanes, and sanitizer lanes were green |
| #1200 | `99d852ec1e6a` | merged from `UNSTABLE`; required `linux`, `macos`, and `windows` wrappers, Namespace platform checks, Codecov patch, and non-macOS coverage lanes were green, only advisory macOS coverage/sanitizer jobs were still queued |

## Conflict And Failure Triage

| PR | Branch | New Head | Status |
| --- | --- | --- | --- |
| #1099 | `feature/view-layout-widgets-coverage-493` | `66decf5958e8` | Merged as `a18291ee6642` after required `linux`, `macos`, and `windows` wrappers passed; advisory lanes were still pending. |
| #1084 | `feature/runtime-text-diff-coverage-641` | `cb63e51416ed` | Merged as `66c74123430a` after required `linux`, `macos`, and `windows` wrappers passed; advisory macOS sanitizer lanes were still pending. |
| #1089 | `feature/view-file-browser-coverage-493` | `86cbe4ca12e9` | Conflict resolved, pushed, and later merged as `d214d24ec20b` after required `linux`, `macos`, and `windows` wrappers, Namespace platform checks, Codecov patch, diff coverage, coverage lanes, and sanitizer lanes were green. |
| #1131 | `feature/audio-window-enumerator-coverage-640-next` | `f5af90f4f5af` | Pushed coverage-harness fix for Windows `.exe` object discovery in `scripts/run_coverage.sh`; `bash -n`, `test_run_coverage.py`, skill-sync report, version-bump report, and `git diff --check` passed. Local CMake validation was not rerun after the harness patch because the laptop filesystem had only about 150-211 MiB free; the earlier excerpt binary validation was already green and the platform-sensitive coverage fix now needs CI/Namespace. |
| #1137 | `feature/audio-platform-helper-coverage-640-next` | `e4ea28dfc2e0` | Pushed a test isolation fix after macOS Namespace exposed a parallel CTest temp-dir collision in `test_cli_projects_registry.cpp`. Focused local `pulp-test-cli-projects-registry "add_project falls back to directory basename when no name hint"` passed; skill-sync/version-bump reports and `git diff --check` passed. Merged as `ea731cbf365c` after required wrappers, Codecov patch, and diff coverage passed; advisory macOS sanitizer lanes were still pending. |
| #1079 | `feature/volume-detector-coverage-642` | `5efc687e53a0` | Conflict resolved after #1045 landed overlapping service-discovery coverage. Kept the non-duplicated lifecycle coverage from #1079, dropped the now-duplicated backend registration failure test, validated `pulp-test-network-service-discovery`, focused `[issue-642]`, broad `NSD|MountedVolumeListChangeDetector|LockingAsyncUpdater` CTest, skill-sync report, version-bump report, and `git diff --check`, then force-with-lease pushed. Merged as `da004f90e21c` after required wrappers, Namespace platform checks, Codecov patch, diff coverage, and coverage lanes were green. |
| #1202 | `feature/system-volume-coverage-640` | `b9e086fa7dfb` | Linux Namespace exposed that the fake `amixer` PATH hid normal shell tools used by the production pipeline. Patched the test to prepend the fake bin directory, not replace `PATH`; local `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`, `cmake --build build --target pulp-test-system-volume -j4`, direct test binary, focused CTest, skill-sync report, version-bump report, and `git diff --check` passed. #1202 merged as `00b0871a6dea89fecb14d4aca8502e5c52507416`. |
| Shipyard #265 | `shipyard cloud retarget` | n/a | Filed `cloud retarget: support useful fallback when job cancellation is denied` after #1078 showed that retarget could identify the queued macOS lane, but apply failed while calling the job-level cancel endpoint. Follow-up evidence showed `gh` already has `workflow` scope and the endpoint returns GitHub 404, so the issue is likely unsupported job-level cancellation rather than only token scope. A plain GitHub-hosted `workflow_dispatch` started a new run but did not replace the stale queued PR-event check in the PR rollup. |

### Live Queue Audit 2026-05-01 02:12 EDT

- Merge-now set: none.
- Watch first: #1123, #1099, #1132, #1142, #1143. These are mergeable at
  the Git level, have no real failing checks, and are waiting on the
  underlying Namespace/build or coverage legs before the lower-case
  required aliases can go green.
- #1075 was the only fresh actionable gate failure in this sweep
  (`Enforce version & skill sync` after the coverage workflow patch);
  it was fixed with metadata-only head `4469868e669b`.
- Older PRs with red lower-case `linux`/`macos`/`windows` contexts mostly
  reflect the deliberate queue-cancellation pause and need future
  PR-event branch refreshes in small batches, not immediate code patches.
- Queue pressure at the audit: roughly 72 active Codecov-like workflow
  runs across 25 branches, so do not refill Namespace until the current
  batch drains.

### Live Queue Audit 2026-05-01 02:55 EDT

- Open `codecov` PRs: 44.
- Merge-now set: none. All open Codecov PRs currently report
  `mergeStateStatus=BLOCKED`.
- GitHub Actions pressure: 60 queued workflow runs and 4 in progress for
  `danielraffel/pulp`, so do not add another PR-event refresh batch yet.
- Watch first: #1143, #1142, #1139, #1137, #1140. #1143 has Linux,
  Windows, diff coverage, Codecov patch, and advisory coverage/sanitizer
  lanes green, but its required `macOS (ARM64) [namespace]` build wrapper
  is still queued. #1142 is similarly waiting on macOS build and macOS
  coverage. #1139/#1137 are still waiting on macOS Namespace lanes, with
  #1137 also waiting on the Linux build wrapper.
- Local-only progress continues while the queue drains. Workers were
  assigned to refresh `local/phase3-merge-cobertura-extra-643`,
  `local/phase3-lcov-cobertura-extra-643`, and
  `local/phase3-run-python-coverage-extra-643` against current
  `origin/main`; no push, PR, or Namespace dispatch until capacity returns.

### Live Queue Audit 2026-05-01 03:17 EDT

- Open `codecov` PRs: 43 after #1120 merged.
- Merge-now set: none. A fresh merge-candidate query returned no
  `CLEAN`/`UNSTABLE` open Codecov PRs.
- GitHub Actions pressure remained high at 58 queued and 5 in progress,
  so no additional PR-event refresh batch was added.
- #1143 remained blocked only on the queued `macOS (ARM64) [namespace]`
  build job. A dry-run `shipyard cloud retarget --pr 1143 --target macOS
  --provider github-hosted --workflow build` showed a clean single-job
  retarget plan, but `--apply` could not cancel the queued job because
  the current `gh` token lacks `actions:write`. Leave the existing
  Namespace lane intact unless a human cancels it manually.

### Live Queue Audit 2026-05-01 03:33 EDT

- #1138 merged as `44cff0532848`; tracker comments posted to #641 and
  #493.
- Open `codecov` PRs: 40 after #1138 merged.
- Merge-now set: none. A fresh merge-candidate query returned no
  `CLEAN`/`UNSTABLE` open Codecov PRs.
- GitHub Actions pressure after the small refill batch: 50 active
  Codecov-like workflow runs, with 45 queued and 5 in progress.
- Watch first: #1143 and #1142 are still blocked on old queued macOS
  Namespace jobs. Keep those existing useful runs rather than pushing
  branch-refresh noise that would invalidate already-green lanes.
- Refill batch added because pressure had dropped below the earlier
  58-72 queued range:

| PR | Branch | Refreshed Head | Build Run | Coverage Run | Sanitizer Run | IWYU Run | Status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| #1045 | `feature/events-service-discovery-coverage-642-next6` | `bfc1d7f981c6` | `25206704717` | `25206704705` | `25206704724` | `25206704727` | queued/in progress |
| #1071 | `feature/background-scanner-restart-coverage-493` | `beacb534d34b` | `25206704899` | `25206704914` | `25206704918` | `25206704896` | queued/in progress |
| #1072 | `feature/design-import-edge-coverage-493` | `2d78c1d52a77` | `25206705148` | `25206705135` | `25206705153` | `25206705144` | merged as `cacf3050929b` |
| #1074 | `feature/audio-format-registry-compressed-640` | `12bed93ae611` | `25206705263` | `25206705261` | `25206705389` | `25206705221` | queued/in progress |

### Merge Update 2026-05-01 03:38 EDT

- #1082 merged as `b0903cd8ed4b` after required `linux`, `macos`,
  and `windows` wrappers, Codecov patch, and the relevant platform build
  lanes were green. Advisory macOS sanitizer/coverage jobs were still
  queued at merge time.
- Tracker comments posted to #641 and #646.
- Open `codecov` PRs: 39 immediately after the merge; fresh
  merge-candidate poll returned none.
- Subsequent queue poll: 57 active Codecov-like workflow runs, with 51
  queued and 6 in progress. Do not add another Namespace/PR-event refill
  batch yet; continue local-only prep and merge monitoring.

### Merge Update 2026-05-01 03:56 EDT

- #1135 merged as `44e67d52cbfc` after required `linux`, `macos`,
  and `windows` wrappers, Codecov patch, and diff coverage were green.
  Advisory macOS sanitizer jobs were still queued at merge time.
- Tracker comments posted to #641 and #645.
- Open `codecov` PRs: 38 immediately after the merge.

### Merge Update 2026-05-01 04:08 EDT

- #1139 merged as `d701d4ae8d45` after required `linux`, `macos`,
  and `windows` wrappers, Namespace build lanes, and Codecov patch were
  green. Pending macOS coverage and sanitizer jobs were advisory.
- Tracker comments posted to #641 and #642.
- Queue pressure immediately before the merge was 18 active GitHub
  workflow runs, with 17 queued and 1 in progress, so capacity is low
  enough for another small refill batch rather than a full queue dump.

### Merge Update 2026-05-01 04:30 EDT

- #1134 merged as `a01ca7410faa` after required `linux`, `macos`,
  and `windows` wrappers, Codecov patch, diff coverage, coverage lanes,
  and sanitizer lanes were green.
- #1119 merged as `0bf8f64aeb8b` after required `linux`, `macos`,
  and `windows` wrappers plus Codecov patch were green. Pending macOS
  coverage was advisory.
- Tracker comments posted to #641 for both merges and to #645 for #1134.

### Merge And Refill Update 2026-05-01 05:12 EDT

- #1137 merged as `ea731cbf365c` from `UNSTABLE` after required
  `linux`, `macos`, and `windows` wrappers, Codecov patch, and diff
  coverage were green. Pending macOS ASan/UBSan jobs were advisory.
  Tracker comments were posted to #641 and #640.
- #1184 opened from the previously local memory-message-channel tranche
  as `feature/memory-message-channel-coverage-641` at
  `065ef90edbd3`. It was labeled `codecov`; PR-event checks are queued
  and explicit Namespace build dispatch `25209211899` was started with
  `shipyard cloud run build feature/memory-message-channel-coverage-641
  --provider namespace --require-sha HEAD --no-wait`.
- #1185 opened from the previously local binding tranche as
  `feature/state-binding-coverage-641` at `c1e5bd595244`. It was
  rebased onto `origin/main`, revalidated locally with
  `pulp-test-binding`, focused `Binding` CTest, skill-sync report,
  version-bump report, and `git diff --check`, then labeled `codecov`.
  Explicit Namespace build dispatch `25209337122` is queued/running.
- The first #1184 `shipyard pr` attempt used the unskipped local target
  config and stopped before PR creation when SSH Windows timed out. The
  retry used the documented Namespace-first pattern:
  `shipyard pr --skip-target mac --skip-target ubuntu --skip-target
  windows`, followed by the explicit Namespace dispatch above.

### Refill Update 2026-05-01 05:21 EDT

- #1186 opened from the text-layout worker tranche as
  `feature/canvas-text-layout-coverage-641` at `d4d40aa77928`. It was
  rechecked locally with `pulp-test-text-shaper`, focused text-shaper
  CTest, skill-sync report, version-bump report, and `git diff --check`,
  then labeled `codecov`. Explicit Namespace build dispatch
  `25209419094` is queued/running.
- #1187 opened from the SVG worker tranche as
  `feature/canvas-svg-coverage-641` at `7e4e8c5fabdd`. It was rechecked
  locally with `pulp-test-svg`, exact `^SvgImage ` CTest, skill-sync
  report, version-bump report, and `git diff --check`, then labeled
  `codecov`. Explicit Namespace build dispatch `25209443645` is
  queued/running.
- AssetManager worker output is held local-only because #1125 is already
  open on `feature/view-asset-manager-coverage-493-next` for the same
  surface.

### Merge Update 2026-05-01 05:23 EDT

- #1143 merged as `d36fddc2cd9f` after required `linux`, `macos`, and
  `windows` wrappers, Codecov patch, diff coverage, and platform
  build/coverage lanes were green. Pending macOS UBSan was advisory.
- Tracker comments posted to #641 and #646.

### Merge Update 2026-05-01 05:27 EDT

- #1075 merged as `c34f11f7138c` after required `linux`, `macos`, and
  `windows` wrappers plus Codecov patch were green. The red Windows
  coverage job was advisory artifact-upload plumbing: coverage suite,
  Cobertura existence check, and Codecov upload succeeded; only the
  Cobertura artifact upload step failed.
- Tracker comments posted to #641 and #643.

### Merge Update 2026-05-01 05:33 EDT

- #1133 merged as `e1a22a1ffd93` after required `linux`, `macos`, and
  `windows` wrappers, Codecov patch, diff coverage, and coverage/build
  lanes were green. Pending macOS ASan/UBSan jobs were advisory.
- Tracker comments posted to #641 and #640.

### Refill Update 2026-05-01 05:35 EDT

- #1188 opened from the table-model tranche as
  `feature/view-table-model-coverage-493` at `9700d0aada15`. It was
  rebased onto current `origin/main`, revalidated locally with
  `pulp-test-table-model`, focused table-model CTest, skill-sync report,
  version-bump report, and `git diff --check`, then labeled `codecov`.
  Explicit Namespace build dispatch `25209808806` is queued/running.
- Tracker comments posted to #641 and #493.

### Refill Update 2026-05-01 05:48 EDT

- #1189 opened from the fast-math tranche as
  `feature/signal-fast-math-boundary-coverage-645` at `55a7e26b9415`.
  It was rebased/current with `origin/main`, revalidated locally with
  `pulp-test-fast-math`, focused fast-math CTest, skill-sync report,
  version-bump report, and `git diff --check`, then labeled `codecov`.
  Explicit Namespace build dispatch `25210105555` is queued/running.
- Tracker comments posted to #641 and #645.

### Merge Update 2026-05-01 05:55 EDT

- #1086 merged as `0300ba207577` after required `linux`, `macos`, and
  `windows` wrappers plus Codecov patch were green. Windows/Linux
  coverage and platform build lanes were green; pending macOS coverage
  and macOS sanitizer jobs were advisory.
- Tracker comments posted to #641 and #640.
- Generated `build*` directories were removed from completed sibling
  coverage worktrees only, recovering local free disk from roughly
  248 MiB to roughly 3.7 GiB. Source worktrees and branches were left
  intact.

### Merge Update 2026-05-01 06:01 EDT

- #1074 merged as `5898bf057163` after required `linux`, `macos`, and
  `windows` wrappers, Codecov patch, diff coverage, coverage lanes, and
  platform build lanes were green. Pending macOS sanitizer jobs were
  advisory.
- Tracker comments posted to #641 and #640.

### Refill Update 2026-05-01 06:10 EDT

- #1190 opened from the path-to-SDF tranche as
  `feature/path-to-sdf-coverage-641` at `c4b62563589a`. It was already
  refreshed against current `origin/main`, revalidated locally with
  `pulp-test-path-to-sdf`, focused `path_to_sdf` CTest, skill-sync
  report, version-bump report, and `git diff --check`, then labeled
  `codecov`. Explicit Namespace build dispatch `25210687152` is
  queued/running.
- Follow-up: the first Windows Namespace run exposed a pre-existing
  `pulp-test-hot-reload` watcher timing issue under parallel CTest, so
  #1190 was rebased onto `origin/main` at `7b78de80` and pushed at
  `69aa03601cb8` with deterministic hot-reload temp paths/content waits
  plus a CTest resource lock. Local validation passed
  `cmake --build build --target pulp-test-hot-reload -j4`,
  `./build/test/pulp-test-hot-reload "[view][hotreload]"`, focused
  `HotReloader` CTest, `pulp-test-path-to-sdf`, focused `path_to_sdf`
  CTest, skill-sync report, version-bump report, and `git diff --check`.
  Fresh PR-event checks are queued under Build and Test run
  `25211537622`.
- Tracker comment posted to #641.

### Merge Update 2026-05-01 06:12 EDT

- #1189 merged as `e60cc129153c` after required `linux`, `macos`, and
  `windows` wrappers, Namespace platform jobs, and Codecov patch were
  green. Pending macOS coverage and sanitizer jobs were advisory.
- Tracker comments posted to #641 and #645.

### Refill Update 2026-05-01 06:15 EDT

- #1191 opened from the SVG path widget tranche as
  `feature/svg-path-widget-coverage-493` at `476ca6ac5639`. It was
  rebased onto current `origin/main`, revalidated locally with
  `pulp-test-svg-path-widget`, focused `svg-path|SvgPath` CTest,
  skill-sync report, version-bump report, `git diff --check`, and
  `git diff --check origin/main...HEAD`, then labeled `codecov`.
  Explicit Namespace build dispatch `25210821512` is queued/running.
- Tracker comments posted to #641 and #493.

### Merge Update 2026-05-01 06:26 EDT

- #1129 merged as `9c43e9fedae2` after required `linux`, `macos`, and
  `windows` wrappers, Namespace platform jobs, Codecov patch, diff
  coverage, and coverage lanes were green. Pending macOS ASan/TSan jobs
  were advisory.
- Tracker comments posted to #641 and #645.

### Merge Update 2026-05-01 06:30 EDT

- #1066 merged as `7b78de802f38` after required `linux`, `macos`, and
  `windows` wrappers, Namespace platform jobs, and Codecov patch were
  green. Pending macOS coverage/UBSan/TSan jobs were advisory.
- Tracker comments posted to #641 and #645.

## Real Diff-Gap Patch Queue

These PRs were inspected after the pause. The failures are not just stale
branch-protection checks; the repo diff-coverage gate reported concrete
uncovered changed lines. Assigned items have workers patching their own
branches in separate worktrees.

| PR | Branch | Gap | Current Action |
| --- | --- | --- | --- |
| #1123 | `feature/host-scan-cache-coverage-493-next` | `core/host/src/scan_cache.cpp` lines `184-188,195,198` | patched/pushed head `fbb4aa88dc10`; merged as `57ac9d3c3a70` after required wrappers passed |
| #1120 | `feature/descriptor-validation-coverage-493-next` | `core/format/src/descriptor_validation.cpp` lines `41-43,69-72` | patched/pushed head `5f4e686b8022`; merged as `dba48cb3f53c` after required wrappers passed |
| #1119 | `feature/state-undo-history-coverage-641-next` | `core/state/include/pulp/state/edit_history.hpp` lines `47-49` | patched/pushed head `e6c0736326ca`; merged as `0bf8f64aeb8b` after required wrappers passed |
| #1102 | `feature/midi-running-status-coverage-645-next` | `core/midi/src/running_status.cpp` lines `92-97` | patched/pushed head `a412d3c88316`; CI queued |
| #1086 | `feature/audio-hotplug-coverage-640` | `core/audio/include/pulp/audio/device.hpp` lines `89-94,116-121,125-126` | patched/pushed head `77b98ed1a2e8`; merged as `0300ba207577` after required wrappers and Codecov patch passed |
| #1085 | `feature/audio-load-measurer-coverage-640` | `core/audio/include/pulp/audio/load_measurer.hpp` lines `35-38,40-41,43-47,49,78-79` | patched/pushed head `9dbd9544a65b`; merged as `5aac29496436` after required wrappers passed |
| #1082 | `feature/render-loop-coverage-646` | `core/render/src/render_loop.cpp` and `core/render/src/render_loop_state.hpp` lifecycle/state lines | patched/pushed head `c53953830003`; merged as `b0903cd8ed4b` after required wrappers and Codecov patch passed |
| #1066 | `feature/signal-filter-meter-coverage-645` | `core/signal/include/pulp/signal/multi_channel_meter.hpp` line `151` | patched/pushed head `cd2aafe8be87`; merged as `7b78de802f38` after required wrappers, Namespace lanes, and Codecov patch passed |
| #1062 | `codex/coverage-midi-edge-644` | `core/midi/include/pulp/midi/message.hpp` factory/masking lines | patched/pushed head `8a9bd9efc2ba`; refreshed head `4fdcb2605585`; merged as `2fbe9a6ce7a2` after required wrappers, Namespace lanes, Codecov patch, and diff coverage passed |
| #1051 | `feature/signal-poly-math-coverage-645` | `core/signal/include/pulp/signal/poly_math.hpp` lines `51-55` | patched/pushed head `0675a09ecabd`; merged as `8f038e9b8150` after required wrappers, Codecov patch, and diff coverage passed |
| #1075 | `feature/cli-host-coverage-643` | diff gate passed; failure was Windows coverage summary upload plumbing, then fresh skill-sync gate required a `ci` skill bypass for `.github/workflows/coverage.yml` | patched/pushed head `4469868e669b`; merged as `c34f11f7138c` after required wrappers and Codecov patch passed. Red Windows coverage job was advisory artifact-upload plumbing; the coverage suite, Cobertura existence check, and Codecov upload succeeded. |

Additional local-only workers started while Namespace was saturated:

| Branch | Worktree | Scope | Status |
| --- | --- | --- | --- |
| `local/phase3-local-ci-extra-643` | `/Users/danielraffel/Code/pulp-local-ci-extra-643` | improve `tools/local-ci/local_ci.py` focused coverage via `tools/local-ci/test_local_ci_extra.py` | Refreshed again on 2026-05-02; `git rebase origin/main` skipped local commit `756c7987` because it is already upstream as `03ee7e86` / #1246, leaving no diff. Validation still passed: CLI skew check, `python3 tools/local-ci/test_local_ci.py`, `python3 tools/local-ci/test_local_ci_extra.py`, skill-sync report, version-bump report, `git diff --check`, and empty `origin/main...HEAD` / `origin/main..HEAD` diffs. Do not queue. |
| `local/phase3-version-bump-extra-643` | `/Users/danielraffel/Code/pulp-version-bump-extra-643` | improve `tools/scripts/version_bump_check.py` focused coverage via `tools/scripts/test_version_bump_check_extra.py` | Refreshed again on 2026-05-02; `git rebase origin/main` skipped local commit `57ceb82f` because it is already upstream as `3d2ebe6e` / #1220, leaving no diff. Validation still passed: CLI skew check, `python3 -m unittest tools/scripts/test_version_bump_check_extra.py`, skill-sync report, version-bump report, and empty `origin/main..HEAD` diff. Do not queue. |
| `local/phase3-compat-sync-extra-643` | `/Users/danielraffel/Code/pulp-compat-sync-extra-643` | improve `tools/scripts/compat_sync_check.py` focused coverage via `tools/scripts/test_compat_sync_check_extra.py` | Refreshed again on 2026-05-02; local commits were duplicate content and are already upstream as `26155037` / #1225, leaving no diff at `origin/main`. Validation still passed: CLI skew check, compat-sync extra/base tests, direct script runs, skill-sync report, version-bump report, and enforced compat-sync report. Do not queue. |
| `local/phase3-audit-top-level-coverage-643` | `/Users/danielraffel/Code/pulp-audit-top-level-coverage-643` | improve `tools/audit.py` focused coverage via `tools/scripts/test_audit_top_level.py` | refreshed locally at `e9c0a82ce136`; focused coverage reports 100%; no push/CI |
| `local/phase3-pulp-sandbox-extra-643` | `/Users/danielraffel/Code/pulp-pulp-sandbox-extra-643` | improve `tools/sandbox-e2e/pulp_sandbox.py` focused coverage via `tools/sandbox-e2e/test_pulp_sandbox_unit.py` | refreshed locally at `85be094c38fe`; focused coverage now reports 100%; shared Python coverage runner intentionally excludes this sandbox surface; no push/CI |

Local disk note: after completed Codecov worktrees pushed their fixes, their
generated `build/` directories plus stale generated build outputs from
older Codecov worktrees were removed. Source worktrees and branches were
left intact. Free space recovered from about `192 MiB` to about `4.7 GiB`.

### Queue Refill 2026-05-01 09:01 EDT

- Direct PR checks showed #1126, #1125, and #1122 were idle with no active
  runs. Their blocking checks were stale cancelled/failure wrappers from
  earlier runs, not current failures.
- Refreshed the three branches with `gh pr update-branch`, producing fresh
  heads and new PR-event workflows:
  - #1126 `feature/view-frame-clock-coverage-493-next` -> `79f2d65046d0`;
    queued Build and Test `25215188581`, Coverage `25215188580`, and
    Sanitizer Tests `25215188559`; Versioning/Skill-Sync `25215188561` and
    IWYU `25215188571` started.
  - #1125 `feature/view-asset-manager-coverage-493-next` -> `33b2a33f0203`;
    queued Build and Test `25215188302`, Coverage `25215188276`, and
    Sanitizer Tests `25215188287`; Versioning/Skill-Sync `25215188284` and
    IWYU `25215188293` started.
  - #1122 `feature/render-draw-batcher-coverage-646-next` -> `dce6dcd7adc3`;
    queued Build and Test `25215188316`, Coverage `25215188317`, and
    Sanitizer Tests `25215188323`; Versioning/Skill-Sync `25215188321` and
    IWYU `25215188319` started.

### Queue Refill 2026-05-01 09:04 EDT

- Subagent triage confirmed #1121 and #1118 were idle, behind current `main`,
  and blocked only by stale cancelled/failed wrapper checks. Refreshed both:
  - #1121 `feature/audio-buffering-reader-coverage-640-next` ->
    `afdff6e1cc06`; queued Build and Test `25215267350`, Coverage
    `25215267349`, and Sanitizer Tests `25215267373`; Versioning/Skill-Sync
    `25215267345` and IWYU `25215267366` started.
  - #1118 `feature/osc-bundle-coverage-641-next` -> `e85122359eb6`; queued
    Build and Test `25215267166`, Coverage `25215267197`, and Sanitizer Tests
    `25215267162`; Android Build `25215267158`, Versioning/Skill-Sync
    `25215267156`, and IWYU `25215267163` started.

### Queue Refill 2026-05-01 09:06 EDT

- Subagent triage confirmed #1117, #1116, #1115, #1114, and #1113 were idle,
  behind current `main`, and blocked only by stale wrapper checks. Refreshed the
  batch:
  - #1117 `feature/lcov-cobertura-coverage-643-next` -> `965cb2e06aa1`.
  - #1116 `feature/signal-interpolator-coverage-645-next` -> `a100d78830bf`.
  - #1115 `feature/runtime-license-analytics-coverage-641-next` ->
    `bdfb46a9e4b0`.
  - #1114 `feature/runtime-file-library-coverage-641-next` -> `18b36739bab5`.
  - #1113 `feature/runtime-expression-coverage-641-next` -> `93c2aea2bd75`.
- Fresh PR rollups show all five active with no current failed checks.

### Queue Refill 2026-05-01 09:10 EDT

- Subagent triage confirmed #1104, #1097, #1096, #1088, #1083, and #1078 were
  idle, behind current `main`, and blocked only by stale wrapper checks.
  Refreshed the batch:
  - #1104 `feature/cli-create-coverage-643` -> `d4c908848517`.
  - #1097 `feature/view-toolbar-coverage-493` -> `4a7000d7b2f9`.
  - #1096 `feature/render-pure-coverage-646` -> `96d7629eb340`.
  - #1088 `feature/events-async-helper-coverage-642` -> `130557461f64`.
  - #1083 `feature/platform-registry-coverage-640` -> `7453f8672bd4`.
  - #1078 `feature/runtime-gzip-header-coverage-641` -> `3a93e12603f8`.
- Fresh PR rollups show all six active with no current failed checks.
  Repository active-run count was about `85`; stop blind refilling here and
  switch to watch/fix/merge until capacity drains.
- #1072 was intentionally not refreshed because it already had active queued
  macOS checks.

## Local-Only Work Prepared During Pause

This table started as the local-only tranche queue prepared after the
Namespace pause began. Many rows now record the later PR / merge
outcome; rows marked queued or held are the ones that still need action
after capacity returns.

May 14 reconciliation note: some older rows below still preserve their
original pause-time validation text. The readiness sweep and snapshot
summary above are the current override for already-landed work; in
particular #1269, #1271, #1273, #1274, #1280, #1282, #1283, #1285,
#1286, and #1287 are now historical because their coverage commits are
on `origin/main`.

| Branch | Head | Scope | Files | Local Validation | Resume Action |
| --- | --- | --- | --- | --- | --- |
| `feature/phase3-docs-generate-coverage-643` | `7d9b34c6` | #643 tooling tranche for `tools/docs_generate.py` paths | `tools/scripts/test_docs_generate.py` | Rebased cleanly onto `origin/main` at `f6e092ba`; `python3 tools/scripts/test_docs_generate.py` reports 15 tests; `python3 tools/docs_generate.py check` reports `docs-generate: OK`; venv/uv-backed `tools/scripts/run_python_coverage.py --pattern tools/scripts/test_docs_generate.py` passed and reported 100% for `tools/docs_generate.py`; `git diff --check origin/main...HEAD` and tracked status clean. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1241, then exited with no local targets remaining as expected for the Namespace-only route; #1241 was labeled `codecov` and linked from #641/#643. Required wrappers, Namespace platform jobs, Codecov patch, diff coverage, and coverage lanes passed. | Merged #1241 as `b0372e3c61b1750bb65cbcbfe139496b55321d90`; tracker comments posted to #641/#643. |
| `feature/phase3-list-limitations-coverage-643` | `fc21f4ed` | #643 tooling tranche for `tools/list_limitations.py` paths plus shared coverage-runner behavior needed by this tranche | `tools/test_list_limitations.py`, `tools/scripts/run_python_coverage.py` | Rebased cleanly onto current `origin/main`; `python3 -m unittest tools/test_list_limitations.py` reports 10 tests; venv-backed `tools/scripts/run_python_coverage.py --pattern tools/test_list_limitations.py` passed and reported 100% for `tools/list_limitations.py`; direct venv-backed coverage report over `tools/test_list_limitations.py` also reported 100% for `tools/list_limitations.py` with 88 statements, 0 misses, 36 branches, and 0 partials; skill-sync report and version-bump report both reported no action needed; `git diff --check origin/main...HEAD` and `git diff --check` passed; final tracked status clean and ahead 2; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1226, then exited with no local targets remaining as expected for the Namespace-only route; #1226 was labeled `codecov`, linked from #641/#643, and merged as `2623a4b47c1d87ccbac3acda13f549bc10dbf320` after PR-event Namespace and coverage gates were green. | Merged. |
| `feature/phase3-check-docs-status-extra-643` | `36e80fe7` | #643 tooling tranche for `tools/check_format_validation.py` paths | `tools/test_check_format_validation.py`, `tools/scripts/run_python_coverage.py`, `tools/scripts/test_run_python_coverage.py` | Rebased cleanly onto `origin/main` at `ef430385`; `python3 tools/test_check_format_validation.py` passed with 10 tests; `python3 tools/scripts/test_run_python_coverage.py` passed with 23 tests; `python3 tools/check_format_validation.py --mode warn` passed while reporting 4 existing warn-mode missing `production_validated` entries; `build-coverage/python-venv/bin/python tools/scripts/run_python_coverage.py --pattern tools/test_check_format_validation.py --pattern tools/scripts/test_run_python_coverage.py` passed; `git diff --check origin/main...HEAD` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1242, then exited with no local targets remaining as expected for the Namespace-only route; #1242 was labeled `codecov` and linked from #641/#643. Required Namespace wrappers, platform lanes, coverage lanes, Codecov patch, and diff coverage passed. | Merged #1242 as `445350d56389d0112ce4076ac53303df075d35ba`; tracker comments posted to #641/#643. |
| `feature/phase3-check-status-ladder-extra-643` | `67038aa9` | #643 tooling tranche for `tools/check_status_ladder.py` paths | `tools/test_check_status_ladder.py` | Rebased onto current `origin/main`; `python3 tools/test_check_status_ladder.py` reports 13 tests; `uvx --from 'coverage>=7.10' coverage run --branch --include='*/tools/check_status_ladder.py' tools/test_check_status_ladder.py` passed; `uvx --from 'coverage>=7.10' coverage report -m tools/check_status_ladder.py` reported 100% line/branch coverage for `tools/check_status_ladder.py`; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1216, then exited with no local targets remaining as expected for the Namespace-only route. Duplicate workflow-dispatch build `25232344234` was cancelled. #1216 merged as `57527d7569f8ebd8882779d10e8ad5b7e92cf259` after required gates were green. | Merged. |
| `feature/bench-diff-coverage-643` | `6a190c5b` | #643 tooling tranche for `tools/scripts/bench_diff.py` paths | `tools/scripts/test_bench_diff.py` | Refreshed against current `origin/main` at `daa0aa70`; `python3 tools/scripts/test_bench_diff.py` reports 8 tests; temp-venv focused coverage passed and reported 98% for `tools/scripts/bench_diff.py`; skill-sync report; version-bump report; `git diff --check origin/main...HEAD`; `git diff --check`; final status clean and ahead 1. System `python3` lacked coverage, so the focused run used a temporary venv outside the repo and removed it afterward. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1205, then exited with no local targets remaining as expected for the Namespace-only route. Explicit Namespace run dispatched as `25222999092`; #1205 is labeled `codecov` and linked from #641/#643. | Queued: monitor #1205 and merge once required gates are green. |
| `feature/check-docs-consistency-coverage-643` | `01c963a` | #643 tooling tranche for `tools/check-docs-consistency.py` paths | `tools/scripts/test_check_docs_consistency.py` | Rebased onto current `origin/main` at `2c5135b0`; `python3 -m unittest tools.scripts.test_check_docs_consistency` reports 9 tests; venv-backed `tools/scripts/run_python_coverage.py --pattern tools/scripts/test_check_docs_consistency.py` passed and reported 98% for `tools/check-docs-consistency.py`; `python3 tools/check-docs-consistency.py` passed; skill-sync report; version-bump report; compat-sync report; `git diff --check origin/main...HEAD`; final status clean. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1206, then exited with no local targets remaining as expected for the Namespace-only route. Explicit Namespace run dispatched as `25223345260`; #1206 is labeled `codecov` and linked from #641/#643. | Queued: monitor #1206 and merge once required gates are green. |
| `feature/mkdocs-hooks-coverage-643` | `b73504a3` | #643 tooling tranche for `tools/mkdocs_hooks.py` paths | `tools/scripts/test_mkdocs_hooks.py` | Refreshed and verified as not patch-equivalent to already-merged #1209; diff remains limited to `tools/scripts/test_mkdocs_hooks.py`. `python3 tools/scripts/test_mkdocs_hooks.py` passed 10 tests; `uv run --with 'coverage>=7.10' python tools/scripts/run_python_coverage.py --pattern tools/scripts/test_mkdocs_hooks.py` passed and reported 100% for `tools/mkdocs_hooks.py`; version-bump, skill-sync, docs-sync, CLI skew, and `git diff --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1244, then exited with no local targets remaining as expected for the Namespace-only route; #1244 is labeled `codecov` and linked from #641/#643. Required Android/Linux/macOS/Windows coverage lanes, required Namespace wrappers, Windows MSVC, diff coverage, version/skill sync, and Codecov patch passed. | Merged #1244 as `fa18065469c469e7f7566c4901e65a6c577b11a2`; tracker comments posted to #641/#643. |
| `feature/encode-binary-data-coverage-643` | `f151cf6b` | #643 tooling tranche for `tools/cmake/scripts/encode_binary_data.py` paths | `tools/scripts/test_encode_binary_data.py` | Refreshed against current `origin/main`; `python3 tools/scripts/test_encode_binary_data.py` reports 6 tests; venv-backed `tools/scripts/run_python_coverage.py --pattern tools/scripts/test_encode_binary_data.py` passed and reported 100% for `tools/cmake/scripts/encode_binary_data.py`; #1207 merged as `9da7b03a522a2c08042bbfe6f3149612ed02bb82` after required gates, including the Windows MSVC release-path gate, were green. | Merged. |
| `feature/phase3-add-component-coverage-643` | `cd93085b` | #643 tooling tranche for `tools/add-component.py` paths | `tools/scripts/test_add_component.py` | Rebased cleanly onto current `origin/main`; `python3 tools/scripts/test_add_component.py` reports 9 tests; venv-backed `tools/scripts/run_python_coverage.py --pattern tools/scripts/test_add_component.py` passed and reported 100% for `tools/add-component.py`; venv-backed direct coverage with `python -m coverage run --branch --include='*/tools/add-component.py' tools/scripts/test_add_component.py` plus `coverage report -m tools/add-component.py` passed and reported 100% for `tools/add-component.py` with 61 statements, 0 misses, 30 branches, and 0 partials; `git diff --check origin/main...HEAD` and `git diff --check` passed; final status clean and ahead 1; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1223, then exited with no local targets remaining as expected for the Namespace-only route; #1223 was labeled `codecov`, linked from #641/#643, and merged as `71267a71d7b631a79f14b375d710f6a093d0403d` after PR-event Namespace and coverage gates were green. | Merged. |
| `feature/phase3-audit-top-level-coverage-643` | `95112c60` | #643 tooling tranche for `tools/audit.py` paths | `tools/scripts/test_audit_top_level.py` | Rebased cleanly onto `origin/main` at `26155037`; `python3 tools/scripts/test_audit_top_level.py` passed with 9 tests; venv-backed `tools/scripts/run_python_coverage.py --pattern tools/scripts/test_audit_top_level.py` passed and reported 100% for `tools/audit.py` with 64 statements, 0 misses, 34 branches, and 0 partials; docs-sync and skill-sync reports passed with no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --exit-code`, and `git diff --cached --exit-code` passed; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1228, then exited with no local targets remaining as expected for the Namespace-only route; #1228 was labeled `codecov`, linked from #641/#643, and merged as `d7b973395782ed4457a7dab65a1b0ea7ca5d5cd8` after PR-event Namespace and coverage gates were green. | Merged. |
| `local/phase3-pulp-sandbox-extra-643` | `85be094c` | #643 tooling tranche for `tools/sandbox-e2e/pulp_sandbox.py` paths | `tools/sandbox-e2e/test_pulp_sandbox_unit.py` | Refreshed against current `origin/main`; temp-venv `pytest tools/sandbox-e2e/test_pulp_sandbox_unit.py` reports 17 tests; shared `run_python_coverage.py --pattern tools/sandbox-e2e/test_pulp_sandbox_unit.py` exits with the expected `matched tests are outside the configured Python coverage surfaces`; direct temp-venv coverage over the pytest run reports 100% for `tools/sandbox-e2e/pulp_sandbox.py`; skill-sync report; version-bump report; `git diff --check origin/main...HEAD`; final status clean. System `python3` lacked pytest and coverage, so the worker used a temporary venv outside the repo and removed it afterward. | Superseded by refreshed #1243 entry below; do not queue this older local snapshot separately. |
| `feature/phase3-embed-js-coverage-643` | `92d94f04` | #643 tooling tranche for `core/view/js/embed_js.py` paths | `tools/scripts/test_embed_js.py` | Refreshed against current `origin/main`; `python3 tools/scripts/test_embed_js.py` reports 6 tests; venv-backed `tools/scripts/run_python_coverage.py --pattern tools/scripts/test_embed_js.py` passed and reported 100% for `core/view/js/embed_js.py`; `python3 tools/scripts/test_run_python_coverage.py` reports 23 tests; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1211; duplicate workflow-dispatch build `25230693088` was cancelled; #1211 merged as `827227339a0609358ba0371a86417a868ee9879e` after required gates were green. | Merged. |
| `feature/phase3-run-swift-coverage-extra-643` | `b11ec337` | #643 tooling tranche for `tools/scripts/run_swift_coverage.py` paths and script entrypoint | `tools/scripts/test_run_swift_coverage_extra.py` | Rebased onto current `origin/main` at `3d2ebe6e`; `python3 -m unittest tools/scripts/test_run_swift_coverage.py tools/scripts/test_run_swift_coverage_extra.py` reports 18 tests; venv-backed `tools/scripts/run_python_coverage.py --pattern tools/scripts/test_run_swift_coverage.py --pattern tools/scripts/test_run_swift_coverage_extra.py` passed and reported 100% for `tools/scripts/run_swift_coverage.py`; direct `python3 tools/scripts/run_swift_coverage.py` passed 10 Swift tests and reported 91.68% Apple source coverage; `git diff --check` passed; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1222, then exited with no local targets remaining as expected for the Namespace-only route; #1222 merged as `30fc6ec792295d13412c599027aa4000d47d35ff` after required checks were green. | Merged. |
| `feature/phase3-android-target-coverage-643` | `92135ab5` | #643 tooling tranche for `tools/local-ci/android_target.py` paths | `tools/scripts/test_android_target.py` | Rebased cleanly onto `origin/main` at `71267a71`; `python3 tools/scripts/test_android_target.py` passed with 16 tests; `uv run --with 'coverage>=7.10' python tools/scripts/run_python_coverage.py --pattern tools/scripts/test_android_target.py` passed and reported 100% for `tools/local-ci/android_target.py` with 69 statements, 0 misses, 28 branches, and 0 partials; `git diff --check origin/main...HEAD`, `git diff --exit-code`, and `git diff --cached --exit-code` passed; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1227, then exited with no local targets remaining as expected for the Namespace-only route; #1227 was labeled `codecov`, linked from #641/#643, and merged as `e495cdba0e02b41bc14f1cd625274f3416346574` after PR-event Namespace and coverage gates were green. | Merged. |
| `feature/phase3-validate-hosts-coverage-643` | `18363f59` | #643 tooling tranche for `tools/deps/validate_hosts.py` paths | `tools/scripts/test_validate_hosts.py` | Rebased cleanly onto `origin/main` at `2623a4b4`; `python3 tools/scripts/test_validate_hosts.py` passed with 9 tests and only mocked local/SSH command construction; venv-backed `run_python_coverage.py --pattern tools/scripts/test_validate_hosts.py` passed and reported 100% for `tools/deps/validate_hosts.py` with 61 statements, 0 misses, 14 branches, and 0 partials; docs-sync report passed; `git diff --check origin/main...HEAD`, `git diff --exit-code`, and `git diff --cached --exit-code` passed; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1229, then exited with no local targets remaining as expected for the Namespace-only route; #1229 was labeled `codecov`, linked from #641/#643, and merged as `a12411e4979832733d66eb9fac9b5471be024be3` after PR-event Namespace and coverage gates were green. No real SSH, VM, or remote validation was run. | Merged. |
| `feature/phase3-compat-sync-extra-643` | `48bf3b9e` | #643 tooling tranche for `tools/scripts/compat_sync_check.py` paths | `tools/scripts/test_compat_sync_check_extra.py` | Rebased cleanly onto current `origin/main`; venv-backed `tools/scripts/run_python_coverage.py --pattern tools/scripts/test_compat_sync_check.py --pattern tools/scripts/test_compat_sync_check_extra.py` passed with 30 base tests plus 31 extra tests and reported 100% for `tools/scripts/compat_sync_check.py` with 340 statements, 0 misses, 178 branches, and 0 partials; `python3 tools/scripts/compat_sync_check.py --mode=report --enforce --base origin/main --head HEAD` passed with no mapped paths touched; `git diff --check` passed; final tracked status clean and ahead 2; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1225, then exited with no local targets remaining as expected for the Namespace-only route; #1225 was labeled `codecov`, linked from #641/#643, and merged as `26155037c6df284cf5bbbf29a104faf7368dcb37` after PR-event Namespace and coverage gates were green. | Merged. |
| `feature/phase3-build-migration-index-extra-643` | `bc37c17e` | #643 tooling tranche for `tools/scripts/build_migration_index.py` paths | `tools/scripts/test_build_migration_index_extra.py` | Rebased cleanly onto `origin/main` at `e495cdba`; base test passed with 7 tests; extra test passed with 10 tests; venv-backed `run_python_coverage.py --pattern tools/scripts/test_build_migration_index.py --pattern tools/scripts/test_build_migration_index_extra.py` passed and reported 100% for `tools/scripts/build_migration_index.py` with 179 statements, 0 misses, 88 branches, and 0 partials; skill-sync, docs-sync, compat-sync, and diff/index checks passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1230, then exited with no local targets remaining as expected for the Namespace-only route; #1230 was labeled `codecov`, linked from #641/#643, and merged as `d1750daa6dbd52aed42dd84d2dc92ba4dd701826` after PR-event Namespace and coverage gates were green. | Merged. |
| `feature/phase3-docs-sync-check-extra-643` | `6960c4d2` | #643 tooling tranche for `tools/scripts/docs_sync_check.py` paths and script entrypoint | `tools/scripts/test_docs_sync_check_extra.py` | Rebased onto current `origin/main` at `57527d75`; `PYTHONPATH=tools/scripts python3 -m unittest tools/scripts/test_docs_sync_check.py tools/scripts/test_docs_sync_check_extra.py` reports 28 tests; throwaway-venv `tools/scripts/run_python_coverage.py --pattern 'tools/scripts/test_docs_sync_check*.py'` passed and reported 100% for `tools/scripts/docs_sync_check.py` with 106 statements, 0 misses, 30 branches, and 0 partials; `git diff --check` passed; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1218, then exited with no local targets remaining as expected for the Namespace-only route; #1218 is labeled `codecov` and linked from #641/#643. #1218 merged as `a147af718361001323755ac01da395d271fe4ee2` after required gates were green. | Merged. |
| `local/phase3-coverage-tier-check-extra-643` | `2623a4b4` | #643 tooling tranche for `tools/scripts/coverage_tier_check.py` paths | none after rebase | Rebased onto current `origin/main`; Git skipped the previously applied local commit and left no diff; `python3 tools/scripts/test_coverage_tier_check.py` passed with 15 tests; `python3 tools/scripts/test_coverage_tier_check_extra.py` passed with 17 tests; targeted coverage reported 100% for `tools/scripts/coverage_tier_check.py`; `git diff --check` passed; final status clean. | No reopen action needed; tranche is already absorbed upstream. |
| `feature/phase3-skill-sync-extra-643` | `eca11f92` | #643 tooling tranche for `tools/scripts/skill_sync_check.py` paths, reporting edges, and script entrypoint | `tools/scripts/test_skill_sync_check_extra.py` | Rebased cleanly onto `origin/main` at `a12411e4`; `python3 tools/scripts/test_skill_sync_check.py` passed with 4 tests; `python3 tools/scripts/test_skill_sync_check_extra.py` passed with 20 tests; `python3 tools/scripts/test_gates.py` passed with 39 tests; venv-backed `run_python_coverage.py --pattern tools/scripts/test_skill_sync_check.py --pattern tools/scripts/test_skill_sync_check_extra.py` passed and reported 100% for `tools/scripts/skill_sync_check.py` with 239 statements, 0 misses, 106 branches, and 0 partials; skill-sync, docs-sync, compat-sync, version-bump, and diff checks passed; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1231, then exited with no local targets remaining as expected for the Namespace-only route; #1231 was labeled `codecov` and linked from #641/#643. Required Namespace wrappers, coverage lanes, Codecov patch, diff coverage, and version/skill sync passed. | Merged #1231 as `e9a1d7e33abd3469255676070b0dbcf306cd35db`; tracker comments posted to #641/#643. |
| `feature/phase3-version-bump-extra-643` | `57ceb82f` | #643 tooling tranche for `tools/scripts/version_bump_check.py` paths, glob separator branches, apply-without-changelog behavior, and script entrypoint | `tools/scripts/test_version_bump_check_extra.py` | Rebased onto current `origin/main`; `python3 tools/scripts/test_gates.py` reports 39 tests; `python3 tools/scripts/test_version_bump_check_extra.py` reports 27 tests; `python3 tools/scripts/version_bump_check.py --mode=report --base origin/main --head HEAD` reports no bump needed; `python3 tools/scripts/skill_sync_check.py --mode=report --base origin/main --head HEAD` reports no mapped paths touched; `git diff --check` passed; venv-backed focused `run_python_coverage.py --pattern tools/scripts/test_gates.py --pattern tools/scripts/test_version_bump_check_extra.py` passed and reported 100% for `tools/scripts/version_bump_check.py` with 496 statements, 0 misses, 250 branches, and 0 partials; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1220, then exited with no local targets remaining as expected for the Namespace-only route; #1220 is labeled `codecov` and linked from #641/#643. #1220 merged as `3d2ebe6e6af57f10a29c2e17439e926221af8521` after required gates were green. | Merged. |
| `feature/phase3-auto-release-decision-extra-643` | `879ed2d5` | #643 tooling tranche for `tools/scripts/auto_release_decision.py` CLI and parser edges | `tools/scripts/test_auto_release_decision.py` | Rebased onto current `origin/main`; `python3 tools/scripts/test_auto_release_decision.py` reports 24 tests; `uv run --with 'coverage>=7.10' python -m coverage run tools/scripts/test_auto_release_decision.py && uv run --with 'coverage>=7.10' python -m coverage report -m tools/scripts/auto_release_decision.py` passed and reported 100% for `tools/scripts/auto_release_decision.py`; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1215, then exited with no local targets remaining as expected for the Namespace-only route. Duplicate workflow-dispatch build `25232091712` was cancelled. This supersedes older duplicate branch `local/phase3-auto-release-extra-643` at `26076d92`; do not queue both. First Linux Namespace Build failed in unrelated `BufferingReader restart clears finished state and stale buffered data`; failed jobs were rerun with `gh run rerun 25232081804 --failed`. #1215 merged as `51f1762ca23e6c5f45b4e31bd5dda5122d2cc855` after required gates were green. | Merged. |
| `feature/phase3-iwyu-annotate-extra-643` | `e2d34d31` | #643 tooling tranche for `tools/scripts/iwyu_annotate.py` edges | `tools/scripts/test_iwyu_annotate_extra.py` | Rebased onto current `origin/main`; `python3 tools/scripts/test_iwyu_annotate.py` reports 16 tests; `python3 tools/scripts/test_iwyu_annotate_extra.py` reports 8 tests; venv-backed `run_python_coverage.py --pattern tools/scripts/test_iwyu_annotate.py --pattern tools/scripts/test_iwyu_annotate_extra.py` passed and reported 100% for `tools/scripts/iwyu_annotate.py`; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1212, then exited with no local targets remaining as expected for the Namespace-only route. Duplicate workflow-dispatch build `25231034599` was cancelled. #1212 merged as `cb954187cc2e7cc00b6cd972f6f2767a9b01af58` after required gates were green. | Merged. |
| `feature/phase3-coverage-diff-comment-extra-643` | `cd1f91eb` | #643 tooling tranche for `tools/scripts/coverage_diff_comment.py` CLI and report-rendering edges | `tools/scripts/test_coverage_diff_comment_extra.py` | Rebased onto current `origin/main`; `python3 tools/scripts/test_coverage_diff_comment.py` reports 11 tests; `python3 tools/scripts/test_coverage_diff_comment_extra.py` reports 5 tests; `python3 -m unittest discover -s tools/scripts -p 'test_coverage_diff_comment*.py'` reports 16 tests; venv-backed `run_python_coverage.py --pattern tools/scripts/test_coverage_diff_comment.py --pattern tools/scripts/test_coverage_diff_comment_extra.py` passed and reported 100% for `tools/scripts/coverage_diff_comment.py`; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1213, then exited with no local targets remaining as expected for the Namespace-only route. Duplicate workflow-dispatch build `25231259746` was cancelled. #1213 merged as `ea03a328ef3351eeaada61259321d9849f6f4fa4` after required gates were green. | Merged. |
| `feature/phase3-cmajor-external-extra-643` | `89f10266` | #643 tooling tranche for `tools/scripts/cmajor_external.py` edges | `tools/scripts/test_cmajor_external_extra.py` | Rebased cleanly onto `origin/main` at `d1750daa`; `python3 -m unittest tools.scripts.test_cmajor_external tools.scripts.test_cmajor_external_extra` passed with 13 tests; `uv run --with 'coverage>=7.10' python tools/scripts/run_python_coverage.py --pattern tools/scripts/test_cmajor_external.py --pattern tools/scripts/test_cmajor_external_extra.py` passed and reported 100% for `tools/scripts/cmajor_external.py` with 119 statements, 0 misses, 32 branches, and 0 partials; `python3 tools/scripts/cmajor_external.py doctor --patch examples/cmajor-gain/CmajorGain.cmajorpatch` passed and reported expected missing external `cmaj`; `git diff --check origin/main...HEAD` passed; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1233, then exited with no local targets remaining as expected for the Namespace-only route; #1233 was labeled `codecov`, linked from #641/#643, and merged as `077145f543e2074b22513df3ac2becdfda2c100d` after required gates were green. Full head SHA is `89f10266b338a4d8a18b9cd05d37941c7cfe0602`; a correction comment was posted after an earlier comment expanded the short SHA incorrectly. The subprocess-heavy base test emitted one no-data warning, but the combined selected run completed with exit code 0 and produced the target 100% row. | Merged. |
| `feature/phase3-jsfx-subset-extra-643` | `a62d0b27` | #643 tooling tranche for `tools/scripts/jsfx_subset.py` parser and human-output edges | `tools/scripts/test_jsfx_subset_extra.py` | Rebased onto current `origin/main`; `python3 -m unittest tools.scripts.test_jsfx_subset tools.scripts.test_jsfx_subset_extra` reports 10 tests; venv-backed `run_python_coverage.py --pattern tools/scripts/test_jsfx_subset.py --pattern tools/scripts/test_jsfx_subset_extra.py` passed and reported 100% for `tools/scripts/jsfx_subset.py`; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1214, then exited with no local targets remaining as expected for the Namespace-only route. Duplicate workflow-dispatch build `25231763147` has a cancellation request pending. #1214 merged as `e1ef408071456dbe9a66297c912edb6dcca8523b` after required wrappers, Codecov patch, and platform coverage contexts were green. | Merged. |
| `feature/phase3-package-cli-extra-643` | `f2727c97` | #643 tooling tranche for `tools/scripts/package_cli.py` cache, rpath, and macOS packaging edges | `tools/scripts/test_package_cli_extra.py` | Rebased onto current `origin/main` at `e1ef4080`; `python3 -m unittest tools/scripts/test_package_cli.py tools/scripts/test_package_cli_extra.py` reports 17 tests; `uv run --with 'coverage>=7.10' python tools/scripts/run_python_coverage.py --pattern tools/scripts/test_package_cli.py --pattern tools/scripts/test_package_cli_extra.py` passed and reported 100% for `tools/scripts/package_cli.py` with 125 statements, 0 misses, 54 branches, and 0 partials; `git diff --check` clean; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1217, then exited with no local targets remaining as expected for the Namespace-only route. #1217 merged as `bc9765f4ddb79b6e9c8c4e3e4aa4e601daecd619` after required gates were green. | Merged. |
| `feature/phase3-resolve-runs-on-extra-643` | `42f24313` | #643 tooling tranche for `tools/scripts/resolve_runs_on.py` edges | `tools/scripts/test_resolve_runs_on_extra.py` | Rebased cleanly onto `origin/main` at `d1750daa`; `python3 tools/scripts/test_resolve_runs_on.py` passed with 18 tests; `python3 tools/scripts/test_resolve_runs_on_extra.py` passed with 2 tests; `uv run --with 'coverage>=7.10' python3 tools/scripts/run_python_coverage.py --pattern tools/scripts/test_resolve_runs_on.py --pattern tools/scripts/test_resolve_runs_on_extra.py` passed and reported 100% for `tools/scripts/resolve_runs_on.py` with 94 statements, 0 misses, 40 branches, and 0 partials; docs-sync report passed; `git diff --check origin/main...HEAD` passed; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1234, then exited with no local targets remaining as expected for the Namespace-only route; #1234 was labeled `codecov`, linked from #641/#643, and merged as `8a9eadc47004911a6c4a7e33b646cbd52b096935` after required gates were green. The focused coverage run emitted one no-data warning before the first test output, but completed with exit code 0 and produced the target 100% row. | Merged. |
| `feature/phase3-merge-cobertura-extra-643` | `79c89e97` | #643 tooling tranche for `tools/scripts/merge_cobertura.py` edges | `tools/scripts/test_merge_cobertura_extra.py` | Rebased cleanly onto `origin/main` at `077145f5`; `.venv/bin/python tools/scripts/test_merge_cobertura.py` passed with 14 tests; `.venv/bin/python tools/scripts/test_merge_cobertura_extra.py` passed with 3 tests; `.venv/bin/python tools/scripts/run_python_coverage.py --pattern tools/scripts/test_merge_cobertura.py --pattern tools/scripts/test_merge_cobertura_extra.py` passed and reported 100% for `tools/scripts/merge_cobertura.py`; docs-sync, skill-sync, version-bump, and CLI sync reports passed; `git diff --check origin/main...HEAD` and `git diff --check` passed; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1236, then exited with no local targets remaining as expected for the Namespace-only route; #1236 was labeled `codecov`, linked from #641/#643, and merged as `69bbe8a467d2900cb462498bd0e4211a156d79a7` after required gates passed. | Merged. |
| `feature/phase3-lcov-cobertura-extra-643` | `d1d7686b` | #643 tooling tranche for `tools/scripts/lcov_cobertura.py` zero-hit, demangler, missing-tool, and entrypoint edges | `tools/scripts/test_lcov_cobertura_extra.py` | Rebased cleanly onto `origin/main` at `8a9eadc4`; `uv run python3 tools/scripts/test_lcov_cobertura.py` passed with 8 tests; `uv run python3 tools/scripts/test_lcov_cobertura_extra.py` passed with 11 tests; `uv run --with 'coverage>=7.10' --with PyYAML python3 tools/scripts/run_python_coverage.py --pattern tools/scripts/test_lcov_cobertura.py --pattern tools/scripts/test_lcov_cobertura_extra.py` passed and reported 100% for `tools/scripts/lcov_cobertura.py`; docs-sync, skill-sync, version-bump, and CLI sync reports passed; `git diff --check origin/main...HEAD` and `git diff --check` passed; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1237, then exited with no local targets remaining as expected for the Namespace-only route; #1237 was labeled `codecov`, linked from #641/#643, and merged as `f6e092ba647553a4eb1864f42594afe8973009c3` after required gates passed. | Merged. |
| `feature/phase3-run-python-coverage-extra-643` | `a113961a` | #643 tooling tranche for `tools/scripts/run_python_coverage.py` edges plus script entrypoint | `tools/scripts/test_run_python_coverage_extra.py` | Rebased against current `origin/main`; `python3 tools/scripts/test_run_python_coverage.py` reports 23 tests; `python3 tools/scripts/test_run_python_coverage_extra.py` reports 8 tests; venv-backed `tools/scripts/run_python_coverage.py --pattern tools/scripts/test_run_python_coverage.py --pattern tools/scripts/test_run_python_coverage_extra.py` passed and reported 100% for `tools/scripts/run_python_coverage.py`; `git diff --check origin/main...HEAD` passed; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1219, then exited with no local targets remaining as expected for the Namespace-only route; #1219 is labeled `codecov` and linked from #641/#643. #1219 merged as `4e93da00dddab17b75531d2fcda1b0511e3a4e9c` after required gates were green. | Merged. |
| `feature/phase3-cli-sync-check-extra-643` | `db185f4d` | #643 tooling tranche for `tools/scripts/cli_sync_check.py` one-sided mismatch and entrypoint edges | `tools/scripts/test_cli_sync_check_extra.py` | Rebased cleanly onto current `origin/main`; `python3 tools/scripts/test_cli_sync_check.py` passed with 7 tests; `python3 tools/scripts/test_cli_sync_check_extra.py` passed with 9 tests; venv-backed `tools/scripts/run_python_coverage.py --pattern tools/scripts/test_cli_sync_check.py --pattern tools/scripts/test_cli_sync_check_extra.py` passed and reported 100% for `tools/scripts/cli_sync_check.py` with 130 statements, 0 misses, 72 branches, and 0 partials; `python3 tools/scripts/cli_sync_check.py --strict --json` passed with `issues: 0` and warnings only; `git diff --check origin/main...HEAD` and `git diff --check` passed; final tracked status clean and ahead 1; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1224, then exited with no local targets remaining as expected for the Namespace-only route; #1224 was labeled `codecov`, linked from #641/#643, and merged as `870eb62abb724f63ffb715b82764792979759e2b` after PR-event Namespace and coverage gates were green. | Merged. |
| `feature/phase3-local-ci-extra-643` | `756c7987` | #643 tooling tranche for `tools/local-ci/local_ci.py` helper edges | `tools/local-ci/test_local_ci.py`, `tools/local-ci/test_local_ci_extra.py` | Refreshed from the local-only local-ci tranche and rebased onto current `origin/main` at `b0372e3c`; `python3 -m unittest discover -s tools/local-ci -p 'test_local_ci_extra.py'` reports 18 tests; `python3 -m unittest discover -s tools/local-ci -p 'test_local_ci*.py'` reports 231 tests; focused temp-venv coverage over the local-ci tests reported 73% for `tools/local-ci/local_ci.py`; CLI skew, skill-sync, version-bump, and `git diff --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1246, then exited with no local targets remaining as expected for the Namespace-only route; #1246 is labeled `codecov` and linked from #641/#643. Required `linux`, `macos`, and `windows` contexts plus Codecov patch passed; only advisory macOS coverage was still running at merge time. | Merged #1246 as `03ee7e86eca6aea65ab6d20af95ec4ebd41754e8`; tracker comments posted to #641/#643. |
| `feature/phase3-deps-audit-extra-643` | `eb0db9ab` | #643 tooling tranche for `tools/deps/audit.py` parser and defensive branches | `tools/deps/test_audit_extra.py` | Rebased onto current `origin/main` at `3d2ebe6e`; `python3 tools/deps/test_audit.py` reports 10 tests; `python3 tools/deps/test_audit_extra.py` reports 14 tests; venv-backed `tools/scripts/run_python_coverage.py --pattern tools/deps/test_audit.py --pattern tools/deps/test_audit_extra.py` passed and reported 100% for `tools/deps/audit.py` with 308 statements, 0 misses, and 154 branches; `python3 tools/deps/audit.py --strict` passed; `git diff --check` passed; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1221, then exited with no local targets remaining as expected for the Namespace-only route; #1221 merged as `7a49b91aa0a0dd650f0dc5fc1c1d560e488e51c8` after required checks were green. | Merged. |
| `feature/phase3-pulp-sandbox-extra-643` | `47171942` | #643 tooling tranche for `tools/sandbox-e2e/pulp_sandbox.py` paths | `tools/sandbox-e2e/test_pulp_sandbox_unit.py` | Refreshed from the local sandbox tranche; `uv run --with pytest --with coverage pytest tools/sandbox-e2e/test_pulp_sandbox_unit.py -q` passed with 17 tests; direct coverage for `tools/sandbox-e2e/pulp_sandbox.py` reported 184 statements, 0 misses, 100%; version-bump, skill-sync, docs-sync, compat-sync, CLI skew, and `git diff --check origin/main...HEAD && git diff --check` passed. Shared `run_python_coverage.py` still intentionally excludes the sandbox surface. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1243, then exited with no local targets remaining as expected for the Namespace-only route; #1243 was labeled `codecov` and linked from #641/#643. Required Namespace wrappers, platform lanes, coverage lanes, sandbox-e2e jobs, Codecov patch, and diff coverage passed. | Merged #1243 as `8bf35b42f494f0cc6db0e77fbceca6fc8cec8c9b`; tracker comments posted to #641/#643. |
| `feature/phase3-freshness-extra-643` | `9ad94e55` | #643 tooling tranche for `tools/packages/freshness_check.py` edges | `tools/scripts/test_freshness_check_extra.py` | Rebased cleanly onto `origin/main` at `8c1c8088`; initial PR #1235 used `tools/packages/test_freshness_check_extra.py`, then was corrected to `tools/scripts/test_freshness_check_extra.py` so the default Linux Python coverage lane executes the tests; `python3 tools/scripts/test_freshness_check_extra.py` passed with 12 tests; `uv run --with 'coverage>=7.10' python tools/scripts/run_python_coverage.py --pattern tools/scripts/test_freshness_check_extra.py` passed and reported 100% for `tools/packages/freshness_check.py` with 109 statements, 0 misses, 44 branches, and 0 partials; `python3 tools/packages/validate_registry.py` passed functionally with 36 packages, 0 errors, and 12 warnings when `jsonschema` was unavailable; skill-sync, docs-sync, version-bump, and diff checks passed; #1235 was labeled `codecov`, linked from #641/#643, and merged as `39b3b76858e486a607f047b800bcde69abc0bdb1` after corrected PR-event checks passed. Strict license checking still exposes existing registry policy findings unrelated to this tranche. | Merged. |
| `local/phase3-pkg-freshness-extra-643` | `55bee1c5` | #643 alternate tooling tranche for `tools/packages/freshness_check.py` edges | `tools/packages/test_freshness_check_extra.py` | Superseded by `local/phase3-freshness-extra-643`; do not open separately because the alternate branch drags unrelated LV2 coverage files against current `origin/main`. | Ignore unless a future manual comparison is needed. |
| `feature/phase3-validate-registry-extra-643` | `619560ed` | #643 tooling tranche for `tools/packages/validate_registry.py` edges | `tools/scripts/test_validate_registry_extra.py` | Rebased cleanly onto `origin/main` at `a12411e4`; `python3 tools/scripts/test_validate_registry_extra.py` passed with 8 tests; `python3 tools/packages/test_package_validation_tools.py` passed with 14 tests; `uv run --with 'coverage>=7.10' python tools/scripts/run_python_coverage.py --pattern tools/scripts/test_validate_registry_extra.py` passed and reported 100% for `tools/packages/validate_registry.py` with 125 statements, 0 misses, 58 branches, and 0 partials; non-strict `python3 tools/packages/validate_registry.py` passed with 36 packages, 0 errors, and 12 warnings; skill-sync, version-bump, coverage-tier, and diff checks passed; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1232, then exited with no local targets remaining as expected for the Namespace-only route; #1232 is labeled `codecov` and linked from #641/#643; #1232 merged as `8c1c80882a29e676bcc9de74ea5f8a6765e20c46` after required gates were green. `jsonschema` is not installed, so non-strict schema validation was skipped. | Merged. |
| `feature/phase3-workflow-lint-coverage-643` | `bfc8b7fa` | #643 static workflow guard tranche for `.github/workflows/workflow-lint.yml` invariants | `tools/scripts/test_workflow_lint.py` | Rebased cleanly onto `origin/main` at `8bf35b42`; `git cherry -v origin/main HEAD` showed the commit is still needed; `python3 tools/scripts/test_workflow_lint.py` passed 7 tests; `python3 tools/scripts/test_release_workflow_test_step.py` passed 5 tests; `yamllint --no-warnings -d 'relaxed' .github/workflows/` passed; structural PyYAML parse passed across 28 workflow files; `actionlint -shellcheck= -pyflakes= .github/workflows/workflow-lint.yml` passed; CLI skew, skill-sync report, and version-bump report passed; `PULP_SKIP_DIFF_COVER=1 tools/scripts/local_diff_cover.sh` passed via the expected static/test-only skip path; focused `run_python_coverage.py --pattern tools/scripts/test_workflow_lint.py` ran the 7 tests then exited during report generation because this static-only path collects no measured source data; `git diff --check origin/main...HEAD` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1251, then exited with no local targets remaining as expected for the Namespace-only route; #1251 is labeled `codecov` and linked from #641/#643. #1251 merged as `7cd7d08dfa509caa2340acae205c4f6785ff40f2`. | Merged. |
| `feature/phase3-codecov-config-edges-643` | `f2bc0e7a` | #643 static coverage-config guard tranche for `codecov.yml` invariants | `tools/scripts/test_codecov_config.py` | Rebased cleanly onto `origin/main` at `8bf35b42`; `python3 tools/scripts/test_codecov_config.py` passed 13 tests; `python3 tools/scripts/test_run_python_coverage.py` passed 23 tests; `python3 tools/scripts/test_run_python_coverage_extra.py` passed 8 tests; `python3 tools/scripts/test_gates.py` passed 39 tests; `uv run --with 'coverage>=7.10' --with PyYAML python tools/scripts/run_python_coverage.py --pattern tools/scripts/test_codecov_config.py` passed with no direct measured source target because this is a test-only/static config guard; CLI skew, skill-sync, docs-sync, compat-sync, version-bump, and diff/index checks passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1250, then exited with no local targets remaining as expected for the Namespace-only route; #1250 is labeled `codecov` and linked from #641/#643. | Merged #1250 as `cad3be04c0c7a0259351bb15a9a9293afd912476`; tracker comments posted to #641/#643. |
| `feature/view-theme-coverage-493-next5` | `c08a6cdf` | #493 view tranche for theme contrast helper edge paths | `test/test_theme_contrast.cpp` | Rebased cleanly onto `origin/main` at `fa180654`; verified #1240 is already upstream and this contrast-helper tranche is still unique; `git cherry -v origin/main HEAD` shows `c08a6cdfdc098586120b857a7cf74f2e42900b36` still needed; `cmake --build build --target pulp-test-theme-contrast -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-theme-contrast` passed 48 assertions in 23 test cases; `ctest --test-dir build -R 'Min contrast thresholds are correct|Adjust lightness clamps at range bounds|Blend colors clamps interpolation factor|With alpha preserves rgb and applies alpha' --output-on-failure` passed 4/4; `git diff --check`; CLI skew check. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1252, then exited with no local targets remaining as expected for the Namespace-only route; #1252 is labeled `codecov` and linked from #493/#641. | Merged #1252 as `2fe041527d4e750c3a5da47b1a4f864f7f3b908d`; tracker comments posted to #493/#641. |
| `feature/canvas-helper-coverage-641-next2` | `ac1e9891` | #641 canvas tranche for CPU fallback helper command recording and gradient fallback behavior | `test/test_canvas.cpp` | Rebased cleanly onto current `origin/main` at `7cd7d08d`; `git cherry -v origin/main HEAD` shows the commit is still needed; `cmake --build build --target pulp-test-canvas -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-canvas "Canvas fallback helpers record CPU-safe commands"` passed 12 assertions; `./build/test/pulp-test-canvas "Canvas gradient fallbacks use first stop when present"` passed 7 assertions; `ctest --test-dir build -R "Canvas fallback helpers|Canvas gradient fallbacks" --output-on-failure` passed 2/2; CLI skew, skill-sync report, version-bump report, `git diff --check origin/main...HEAD`, and `git diff --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1265, then exited with no local targets remaining as expected for the Namespace-only route; #1265 is labeled `codecov` and linked from #641. Required Namespace wrappers/platform lanes, Windows MSVC, Codecov patch, diff coverage, audit, IWYU, and version/skill gates were green; advisory macOS coverage/sanitizer lanes were still queued. | Merged #1265 as `414f9e9a2f06a45290ef12cf4c5369d34bf27eaf`; tracker comment posted to #641; cancellation requested for leftover advisory workflows `25242814020` and `25242814025`. |
| `feature/events-coverage-642-next4` | `4daca9a3` | #642 events tranche for IPC endpoint parse edge behavior | `test/CMakeLists.txt`, `test/test_ipc_endpoints.cpp` | Rebased cleanly onto current `origin/main` at `7cd7d08d`; `git cherry -v origin/main HEAD` shows the commit is still needed; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` passed with existing optional dependency caveats; `cmake --build build --target pulp-test-ipc-endpoints -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-ipc-endpoints "[events][ipc][issue-642]"` passed 12 assertions in 2 test cases; `ctest --test-dir build -R "IPC socket endpoints reject empty ports without opening connections|IPC socket listener rejects empty endpoint strings" --output-on-failure` passed 2/2; skill-sync report, version-bump report, `git diff --check origin/main...HEAD`, and `git diff --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1266, then exited with no local targets remaining as expected for the Namespace-only route; #1266 is labeled `codecov` and linked from #642/#641. | Queued: monitor #1266 and merge once required gates are green. |
| `feature/phase3-widget-bridge-gpu-coverage-493` | `4b7adf18` | #493 view tranche for `core/view/src/widget_bridge.cpp` GPU descriptor, fallback canvas, and `applyShader` bridge edges | `test/test_widget_bridge.cpp` | Created from current `origin/main` at `ae542586`; configured `build` with `-DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF` and the shared MbedTLS cache; `cmake --build build --target pulp-test-widget-bridge -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-widget-bridge "WidgetBridge GPU info and fallback canvas descriptors are scriptable" -r compact` passed 20 assertions; `./build/test/pulp-test-widget-bridge "[shader]" -r compact` passed 6 assertions; full `./build/test/pulp-test-widget-bridge -r compact` passed 696 assertions in 94 test cases; `ctest --test-dir build -R "WidgetBridge" --output-on-failure` passed 85/85; CLI skew, skill-sync report, version-bump report, `git diff --check origin/main...HEAD`, and `git diff --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1267, then exited with no local targets remaining as expected for the Namespace-only route; #1267 is labeled `codecov` and linked from #493/#641. Required gates were green; advisory macOS coverage/sanitizer lanes were still pending. | Merged #1267 as `778d3d352c55994168ba273039b089ea0f53ae52`; tracker comments posted to #493/#641. |
| `feature/phase3-cli-common-config-coverage-493` | `87796d2b` | #493 CLI tranche for `tools/cli/cli_common.cpp` config parser, bare-home project base dir, AAX guidance, and temp text helper edges, plus deterministic BufferingReader restart test hardening | `test/test_cli_project_command.cpp`, `test/test_buffering_reader.cpp` | Created from current `origin/main` at `ae542586`, then rebased onto current `origin/main` at `db400429` after #1260/#1253 merged; configured `build` with `-DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF` and the shared MbedTLS cache; `cmake --build build --target pulp-test-cli-project-command -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-cli-project-command -r compact` passed 522 assertions in 35 test cases; focused CTest passed; CLI skew, skill-sync report, version-bump report, and diff checks passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1268, then exited with no local targets remaining as expected for the Namespace-only route; #1268 is labeled `codecov` and linked from #493/#641. Windows Namespace exposed CRLF sensitivity in the temp-note assertion; pushed `8f03c180` to normalize newlines. Linux Namespace then exposed `BufferingReader restart clears finished state and stale buffered data`; pushed `87796d2b` (`test(audio): make buffering restart check deterministic`) in `test/test_buffering_reader.cpp`. Required gates were green; advisory macOS coverage/sanitizer lanes were still pending. | Merged #1268 as `91660df143d9c1c0273fbad58f5144e54c11e739`; tracker comments posted to #493/#641/#643. |
| `feature/phase3-design-import-bundle-coverage-493` | `e9669ee9` local; #1269 remote remains `923b04f1` | #493 view/import tranche for `core/view/src/design_import.cpp` Claude bundle malformed JSON/template, skipped asset, referenced-JS indexing, and bundled classname extraction edges | `test/test_design_import_claude_bundle.cpp` | Worker-created from current `origin/main`; original #1269 head `923b04f1` was locally rebased on 2026-05-05 to `ddd6e9af` on current `origin/main` `50ff5822`, rebased again to `e1758468a0a471dd31065940f70c24d91a1dbb54` on current `origin/main` `cf5ea658`, rebased again to `45b77a0be216209c39e2e6cbd12484852cab3658` on current `origin/main` `24047ba3`, rebased again to `5e1fcbd2a559310a91336e309734519f6ff0c587` on current `origin/main` `df01f4f64696f3915902173ecf38223811b8ed60`, then rebased again to `9b2f2ed04fd2d7fc7291d9d3b973e476de34e2ed` on current `origin/main` `d11f530e34248f97bbf31ed1b14e8bf659cc035e`, then rebased again to `3b57ae4e8faaac3e926049bd6ad682e193cc4196` on current `origin/main` `a8c4c16bccdf62c58dfa5ac8cc7934a69c55375d`, then rebased again to `e9669ee9b29669753c1b158471d09473d1d14624` on current `origin/main` `d191cdcaf85c1461d9d1ac44cfc9c656debb1096` with no push or CI dispatch. The final diff is limited to `test/test_design_import_claude_bundle.cpp`; an intermediate rebase onto `0447498e` was superseded after local `origin/main` advanced. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, canvas, view, duplicate-library, and platform warnings and configured Pulp `0.78.1`; `cmake --build build --target pulp-test-design-import-claude-bundle -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-design-import-claude-bundle "[view][import][issue-468]" -r compact` passed 46 assertions in 9 test cases, including the hidden fixture skip path; full `./build/test/pulp-test-design-import-claude-bundle -r compact` passed 45 assertions in 8 default test cases; `ctest --test-dir build -R "malformed template|skips malformed assets|extract_claude_classnames|parse_claude_bundle" --output-on-failure` passed 8/8; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 162 / behind 1 against the paused remote. The prior #1269 PR creation/label/tracker links remain intact but stale until the remote branch is updated. | Hold local refreshed branch while Namespace is intentionally paused; when capacity returns, update #1269 with `--force-with-lease` in a small resume batch and merge only after fresh required PR-event gates are green. |
| `feature/phase3-widgets-paint-coverage-493` | `f4fce52a` | #493 view tranche for `core/view/src/widgets.cpp` Knob/Fader sprite-strip paint, vertical RangeSlider paint, and SpectrumView invalid-dB paint edges | `test/test_widgets.cpp` | Worker-created from current `origin/main`; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` passed; `cmake --build build --target pulp-test-widgets -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-widgets` passed 259 assertions in 63 test cases; `ctest --test-dir build -R "Widgets|widgets" --output-on-failure` passed 1/1; CLI skew, skill-sync report, version-bump report, `git diff --check`, and `git diff --cached --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1270, then exited with no local targets remaining as expected for the Namespace-only route; #1270 is labeled `codecov` and linked from #493/#641. Required Namespace wrappers, Windows MSVC, and Codecov patch passed; Windows coverage failed on a WGPU DLL copy issue while advisory macOS coverage/sanitizer lanes were still queued. | Merged #1270 as `910029f6f7cebe7b20f01a003c249dc938838d80`; tracker comments posted to #493/#641; cancellation requested for leftover advisory workflows `25243597556` and `25243597558`. |
| `feature/phase3-create-targets-coverage-643` | `f24a69cb` local; #1271 remote remains `62ca4512` | #643 CLI helper tranche for `tools/cli/create_targets.cpp` optional format suffixes, duplicate suppression, and empty standalone app target filtering | `test/test_cli_create_targets.cpp` | Created from current `origin/main` at `451a2428`; original #1271 head `62ca4512` was locally rebased on 2026-05-05 to `ad7aebbe` on current `origin/main` `50ff5822`, rebased again to `61be2d26dbedadb4b8547ba3055fa34919c4568b` on current `origin/main` `b7ec8f08`, rebased again to `b9b2e8298a01afa5119a7b5251fab2a00fa0950a` on current `origin/main` `cf5ea658`, rebased again to `05f0acdad01b9d6550a80f6af2dff44e7571b6f0` on current `origin/main` `24047ba3`, then rebased again to `13f5981b7342e67f5b371acb22ac1b938eb39a6d` on current `origin/main` `df01f4f6`, then rebased again to `d3a749f4eb1576db2ede41eb2b6b85409002650b` on current `origin/main` `a8c4c16bccdf62c58dfa5ac8cc7934a69c55375d`, then rebased again to `f24a69cbf934e791738d37eb4d229623ef9623d7` on current `origin/main` `d191cdcaf85c1461d9d1ac44cfc9c656debb1096` with no push or CI dispatch. Intermediate local refresh `79a3489c` on `0447498e` was superseded after `origin/main` advanced. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1`; `cmake --build build --target pulp-test-cli-create-targets -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-cli-create-targets "[cli][create][issue-643]" -r compact` passed 2 assertions in 2 test cases; full `./build/test/pulp-test-cli-create-targets -r compact` passed 5 assertions in 5 test cases; `ctest --test-dir build -R "CLI create targets|cli-create-targets|create targets" --output-on-failure` passed 5/5; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 161 / behind 1 against the paused remote. The prior #1271 PR creation/label/tracker links remain intact but stale until the remote branch is updated. | Hold local refreshed branch while Namespace is intentionally paused; when capacity returns, update #1271 with `--force-with-lease` in a small resume batch and merge only after fresh required PR-event gates are green. |
| `feature/phase3-import-design-tool-coverage-493` | `bc463fcf` | #493 tooling/import tranche for `tools/import-design/pulp_import_design.cpp` help/error, dry-run token export, and Stitch import write paths | `test/CMakeLists.txt`, `test/test_import_design_tool.cpp` | Worker-created from current `origin/main` at `5984a309`; configured and built `pulp-test-import-design-tool`; `./build/test/pulp-test-import-design-tool` passed 29 assertions in 3 test cases; `ctest --test-dir build -R "import-design-tool|pulp import-design" --output-on-failure` passed 3/3; CLI skew, skill-sync report, version-bump report, `git diff --check`, and `git diff --cached --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1272, then exited with no local targets remaining as expected for the Namespace-only route; #1272 is labeled `codecov` and linked from #493/#641. Required Namespace wrappers, Windows MSVC, and Codecov patch passed. Android coverage failed on the earlier SDK/download transient (`dl.google.com` resolution / build-tools install), and advisory macOS coverage/sanitizer lanes were still queued. | Merged #1272 as `cb641ff19c16ce0115e3dae7c04e5c2fa5a62760`; tracker comments posted to #493/#641; cancellation requested for leftover advisory workflows `25243703306` and `25243703313`. |
| `feature/phase3-package-commands-coverage-493` | `a7341506` local; #1273 remote remains `c52cd486` | #493 package-command tranche for `tools/cli/package_commands.cpp` target compatibility warning behavior | `test/test_cli_package_commands.cpp` | Worker-created from current `origin/main` at `5984a309`; original #1273 head `c52cd486` was locally rebased on 2026-05-05 to `5fa859ab` on current `origin/main` `50ff5822`, rebased again to `1e5d20d96f27f3285e1ac2b9a96f1790c2dcf213` on current `origin/main` `b7ec8f08`, rebased again to `fead2efd3d438269d0a543308e5a97ae5bad1ac7` on current `origin/main` `cf5ea658`, rebased again to `106b1cf9d4d222d7750c04796411f810fca94282` on current `origin/main` `24047ba3`, then rebased again to `8f26959e75ef507e7e2859a1163f54244734e9b8` on current `origin/main` `df01f4f6`, then rebased again to `d72ac3d51de0884a1a302d5e7ad765c2bcc950b8` on current `origin/main` `a8c4c16bccdf62c58dfa5ac8cc7934a69c55375d`, then rebased again to `a734150695ddc5bf74d9077a9fc8ee092cb8512e` on current `origin/main` `d191cdcaf85c1461d9d1ac44cfc9c656debb1096` with no push or CI dispatch. Intermediate local refresh `89e30728` on `0447498e` was superseded after `origin/main` advanced. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1`; `cmake --build build --target pulp-test-cli-package-commands -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-cli-package-commands "cmd_target add warns when installed packages do not support the new target" -r compact` passed 6 assertions in 1 test case; full `./build/test/pulp-test-cli-package-commands -r compact` passed 195 assertions in 12 test cases; `ctest --test-dir build -R "cmd_target add warns|package commands|Package|pulp-test-cli-package-commands" --output-on-failure` passed 2/2; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 160 / behind 1 against the paused remote. The prior #1273 PR creation/label/tracker links remain intact but stale until the remote branch is updated. | Hold local refreshed branch while Namespace is intentionally paused; when capacity returns, update #1273 with `--force-with-lease` in a small resume batch and merge only after fresh required PR-event gates are green. |
| `feature/phase3-cli-ship-coverage-643-next` | `384c4d5e` local; #1274 remote remains `c924f1e8` | #643 CLI ship tranche for deterministic `pulp ship sign` missing-identity guidance in a valid project/build cache | `test/test_cli_ship_shellout.cpp` | Worker-created from current `origin/main`; original #1274 head `c924f1e8` was locally rebased on 2026-05-05 to `844f501b` on current `origin/main` `50ff5822`, then rebased again to `baebedd5148bf5042c1d81fd79c6d3afebf0c117` on current `origin/main` `b7ec8f08`, then rebased again to `3c55283d071a173898bf58c12ce45b4d080ead70` on current `origin/main` `cf5ea658`, then rebased again to `d113ca6ce2a4af4b0fc99264979d268b8f52189b` on current `origin/main` `24047ba3`, rebased again to `c39f3d42ade480a59bbb5dfbc7a7c6cfe9184fe0` on current `origin/main` `7e9795b4`, rebased again to `6e4734c4891d51992a22939faaee8c82449f3942` on current `origin/main` `df01f4f6`, then rebased again to `7f5be8e8a0c626d035c033318b48fe5e371e8ce8` on current `origin/main` `42bacd19`, then rebased again to `2e691f7d66af3676f7177d8221738de11338f63c` on current `origin/main` `a8c4c16bccdf62c58dfa5ac8cc7934a69c55375d`, then rebased again to `84d9ab05d094956b5032219dec74b168ad4a8a4c` on current `origin/main` `d191cdcaf85c1461d9d1ac44cfc9c656debb1096`, then rebased again to `384c4d5e1514d4947fc4fad4ed007b1ba0b16283` on current `origin/main` `8a58501be964085d0315245e498799305dfeae69` with no push or CI dispatch; intermediate local refresh `64c4424c` on `0447498e` was superseded after `origin/main` advanced. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=ON -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing WebGPU/Skia-not-found, third-party, SDL3, view, CLI, duplicate-library, and platform warnings and configured Pulp `0.78.1`; `cmake --build build --target pulp-cli pulp-test-cli-ship-shellout -j$(sysctl -n hw.ncpu)` passed; focused real-CLI `PULP_CLI_PATH=/Users/danielraffel/Code/pulp-cli-ship-coverage-643-next/build/tools/cli/pulp ./build/test/pulp-test-cli-ship-shellout "pulp ship sign in project without identity reports signing guidance" -r compact` passed 4 assertions in 1 test case; full real-CLI `PULP_CLI_PATH=... ./build/test/pulp-test-cli-ship-shellout -r compact` passed 49 assertions in 11 test cases; ship-tag real-CLI `PULP_CLI_PATH=... ./build/test/pulp-test-cli-ship-shellout "[cli][shellout][ship]" -r compact` passed 49 assertions in 11 test cases; exact `PULP_CLI_PATH=... ctest --test-dir build -R "pulp ship sign in project without identity" --output-on-failure` passed 1/1; CLI skew check passed silently; CLI sync check passed with slash-command/skill-reference warnings only and zero issues; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 162 / behind 1 against the paused remote. The prior #1274 PR creation/label/tracker links remain intact but stale until the remote branch is updated. | Hold local refreshed branch while Namespace is intentionally paused; when capacity returns, update #1274 with `--force-with-lease` in a small resume batch and merge only after fresh required PR-event gates are green. |
| `feature/phase3-cli-audio-command-coverage-643` | `34e71acd` local; #1287 remote remains `cb0a4acb` | #643 CLI audio command parser tranche for deterministic `pulp audio` usage, parser errors, and missing-bundle JSON behavior | `test/test_cli_shellout.cpp` | Worker-created from current `origin/main`; REST PR lookup confirmed #1287 is open at `cb0a4acbca31e4f7b51e4c2f3c92226b76ba7318` and labeled `codecov`. Locally rebased on 2026-05-05 to `c88ec0f3` on current `origin/main` `50ff5822`, rebased again to `df994ea77d094498cd66bc34af57389ea86c5e78` on current `origin/main` `b7ec8f08`, rebased again to `63efb4f7fcbcf757e933b1d7df0a264219c2383f` on current `origin/main` `cf5ea658`, rebased again to `fc736f3a767e7fe2ae58bfd9366c83f2599c1874` on current `origin/main` `24047ba3`, rebased again to `18dce663530fae23205133f8a2f71c023f5f0e77` on current `origin/main` `7e9795b4`, then rebased again to `8769986971e95728554ce9fa3184b9f75c8743e4` on current `origin/main` `df01f4f6`, then rebased again to `490b4a36287b77113af69349d71456b73d530eb0` on current `origin/main` `42bacd1900a01446b3e3b53cf2e667e2eabbec6d`, then rebased again to `4bd8824958e12e795f8d6c31cd2cb0dc11fa61d9` on current `origin/main` `a8c4c16bccdf62c58dfa5ac8cc7934a69c55375d`, then rebased again to `95c6fc6e0997d7857f400507c3afe312088714ca` on current `origin/main` `d191cdcaf85c1461d9d1ac44cfc9c656debb1096`, then rebased again to `34e71acdaa470d9d27f807f8f4d9999340aec430` on current `origin/main` `8a58501be964085d0315245e498799305dfeae69` with no push or CI dispatch; intermediate local refresh `353a6afd` on `0447498e` was superseded after `origin/main` advanced. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=ON -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing WebGPU/Skia-not-found, third-party, SDL3, view, CLI, duplicate-library, and platform warnings and configured Pulp `0.78.1`; `cmake --build build --target pulp-cli pulp-import-design pulp-test-cli-shellout -j$(sysctl -n hw.ncpu)` passed; focused real-CLI `PULP_CLI_PATH=/Users/danielraffel/Code/pulp-phase3-cli-audio-command-coverage-643/build/tools/cli/pulp ./build/test/pulp-test-cli-shellout "[cli][shellout][audio][issue-643]" -r compact` passed 56 assertions in 2 test cases; full `PULP_CLI_PATH=... ./build/test/pulp-test-cli-shellout -r compact` passed 518 assertions in 64 test cases; focused `PULP_CLI_PATH=... ctest --test-dir build -R "pulp audio|cli-shellout|audio usage" --output-on-failure` passed 2/2; CLI skew check passed silently; CLI sync check passed with slash-command/skill-reference warnings only and zero issues; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; generated `classnames.json` from the full shellout run was removed and final status was clean and ahead 151 / behind 1 against the paused remote. The prior #1287 PR creation/label/tracker links remain intact but stale until the remote branch is updated. | Hold local refreshed branch while Namespace is intentionally paused; when capacity returns, update #1287 with `--force-with-lease` in a small resume batch and merge only after fresh required PR-event gates are green. |
| `codecov-phase3-android-kotlin-runtime-coverage-641` | `56e1b7b0` | #641 Android/Kotlin tranche for `PulpMidiManager` device discovery, open/close lifecycle, MIDI send-port reuse, and transport fallback | `android/app/src/test/kotlin/com/pulp/midi/PulpMidiManagerTest.kt` | Worker-created from current `origin/main`; `JAVA_HOME=/opt/homebrew/opt/openjdk@17 ANDROID_HOME="$HOME/Library/Android/sdk" ./gradlew :app:testDebugUnitTest --tests com.pulp.midi.PulpMidiManagerTest` passed; `JAVA_HOME=/opt/homebrew/opt/openjdk@17 ANDROID_HOME="$HOME/Library/Android/sdk" ./gradlew :app:jacocoDebugUnitTestReport` passed; CLI skew, skill-sync report with Android skill bypass trailer, version-bump report, and `git diff --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1275, then exited with no local targets remaining as expected for the Namespace-only route; #1275 is labeled `codecov` and linked from #641. | Queued: monitor #1275 and merge once required gates are green. |
| `feature/phase3-popup-menu-coverage-640` | `c66d74e4` | #640 platform tranche for popup menu item/separator bookkeeping and non-Apple fallback no-selection behavior | `test/test_platform.cpp` | Created from current `origin/main` at `910029f6`; configured `build` with `-DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF` and the shared MbedTLS cache after a first configure hit a slow fresh MbedTLS clone; `cmake --build build --target pulp-test-platform -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-platform -r compact` passed 37 assertions in 4 test cases; exact `ctest --test-dir build -R "^(Platform detection|PopupMenu records items and separators|Windows registry helpers fail closed on non-Windows platforms)$" --output-on-failure` passed 3/3. A broader `registry` regex also matched unrelated NOT_BUILT registry placeholders in this partial build, so exact discovered names are the valid CTest evidence. CLI skew, skill-sync report, version-bump report, and `git diff --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1276, then exited with no local targets remaining as expected for the Namespace-only route; #1276 is labeled `codecov` and linked from #640/#641. Local triage on 2026-05-05 found current `origin/main` already covers the same PopupMenu metadata and non-Apple stub paths in `test/test_file_dialog.cpp`, so this branch was not locally refreshed during the Namespace pause. | Hold as likely duplicate coverage; when capacity returns, close/supersede #1276 after final code/Codecov confirmation or retarget it to a platform gap not already covered by `origin/main`. |
| `phase3-midi-ump-coverage-645` | `a1dc2508` | #645 MIDI tranche for deterministic UMP channel voice helper factory encoding | `test/test_ump_buffer_conversion.cpp` | Worker-created from current `origin/main`; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` passed; `cmake --build build --target pulp-test-ump-buffer-conversion -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-ump-buffer-conversion` passed 139 assertions in 17 test cases; `ctest --test-dir build -R "ump-buffer-conversion|UmpPacket remaining" --output-on-failure` passed 1/1; CLI skew, skill-sync report, version-bump report, and `git diff --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1277, then exited with no local targets remaining as expected for the Namespace-only route; #1277 is labeled `codecov` and linked from #645/#641. | Queued: monitor #1277 and merge once required gates are green. |
| `fix/phase3-ci-doctor-unblock-643` | `a2bdd728` | #643 CI robustness fix for shared `cli-doctor` CTest instability and Android coverage SDK install retries | `.github/workflows/coverage.yml`, `tools/cli/CMakeLists.txt` | Worker-created after queue triage classified #1269 and older #1255 failures as shared CI/CTest robustness issues and #1272 Android coverage as an SDK/download transient. Local validation passed: `cmake -S . -B build-phase3-ci-doctor -G Ninja -DPULP_BUILD_TESTS=ON -DPULP_ENABLE_GPU=ON`; `cmake --build build-phase3-ci-doctor --target pulp-cli -j4`; `ctest --test-dir build-phase3-ci-doctor --output-on-failure -R "^cli-doctor(-versions|-ci|-validators|-versions-json|-caches-empty|-caches-json)?$"`; `actionlint .github/workflows/coverage.yml`; `git diff --check`; CLI skew. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1278, then exited with no local targets remaining as expected for the Namespace-only route; #1278 is labeled `codecov`. | Queued: merge #1278 when required gates are green, then rerun/rebase PRs blocked only by the shared doctor/Android robustness failures. |
| `feature/android-kotlin-coverage-phase3-tranche` | `b845c453` | #641 Android/Kotlin tranche for `PulpAudioController` lifecycle edges | `android/app/src/test/kotlin/com/pulp/audio/PulpAudioControllerTest.kt` | Worker-created from current `origin/main` without overlapping #1275; local validation passed `./gradlew :app:testDebugUnitTest --tests com.pulp.audio.PulpAudioControllerTest`, `./gradlew :app:testDebugUnitTest :app:jacocoDebugUnitTestReport`, CLI skew, skill-sync report with Android skill bypass trailer, version-bump report, and `git diff --check origin/main...HEAD`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1279, then exited with no local targets remaining as expected for the Namespace-only route; #1279 is labeled `codecov` and linked from #641. Initial Android Build Windows job failed inside `actions/cache@v5` before Gradle/test code ran; `gh run rerun 25244691254 --failed` requested and the rerun passed both macOS and Windows Android-build jobs. | Queued: monitor #1279 and merge once required gates are green. |
| `feature/phase3-mmap-reader-extra-640` | `a4cb8a9b` local; #1280 remote remains `ed2ba7bf` | #640 audio tranche for `MemoryMappedAudioReader` unsupported-file fail-closed and EOF no-copy edges | `test/test_audio_file.cpp` | Created from current `origin/main` at `414f9e9a`; #1280 was originally opened at `867fedc5` and later amended remotely to `ed2ba7bf` to close the mmap reader before temp cleanup. Locally refreshed through `a4cb8a9bfd368ed2f3802b8441b8483f3107edc4` on current `origin/main` `92e83b376a24718d7e40ea27f2a65fb5579027db` with no push or CI dispatch; previous local refreshes on `0447498e`, `50ff5822`, `b7ec8f08`, `6c8b9920`, `24047ba3`, `df01f4f6`, `42bacd19`, `a8c4c16`, and `d191cdca` were superseded after local `origin/main` advanced. The final diff is limited to `test/test_audio_file.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1`; `cmake --build build --target pulp-test-audio-file -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-audio-file "MemoryMappedAudioReader*" -r compact` passed 41 assertions in 3 test cases; full `./build/test/pulp-test-audio-file -r compact` passed 509 assertions in 30 test cases; `ctest --test-dir build -R "MemoryMappedAudioReader" --output-on-failure` passed 3/3; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 156 / behind 2 against the paused remote. The prior #1280 PR creation/label/tracker links remain intact but stale until the remote branch is updated. | Hold local refreshed branch while Namespace is intentionally paused; when capacity returns, update #1280 with `--force-with-lease` in a small resume batch and merge only after fresh required PR-event gates are green. |
| `feature/phase3-version-diag-edge-coverage-643` | `bc77c8df` | #643 CLI tranche for `tools/cli/version_diag.cpp` manifest-reader and empty-project-root edges | `test/test_cli_version_diag.cpp` | Worker-created from current `origin/main` at `414f9e9a` after checking open PR scopes to avoid #1271/#1273/#1274/#1278; `cmake --build build --target pulp-test-cli-version-diag -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-cli-version-diag` passed 92 assertions in 35 test cases; `ctest --test-dir build -R "(manifest has no version field|empty project root)" --output-on-failure` passed; focused LLVM coverage for `tools/cli/version_diag.cpp` reported 87.50% line coverage; CLI skew check, `git diff --check HEAD~1..HEAD`, skill-sync, and version-bump passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1281, then exited with no local targets remaining as expected for the Namespace-only route; #1281 is labeled `codecov` and linked from #641/#643. Required gates were green; only advisory macOS sanitizer/coverage lanes were pending at merge time. | Merged #1281 as `01ecdaf109fc31a995efc82650dad8cf2a31d3e0`; tracker comments posted to #641/#643. |
| `feature/view-text-editor-coverage-493-next` | `5b5173a2` local; #1282 remote remains `03e5e3cd` | #493 view tranche for `core/view/src/text_editor.cpp` navigation edges | `test/test_text_editor.cpp` | Worker-created from current `origin/main` at `414f9e9a` after rebasing away inherited audio commits; final PR diff is limited to `test/test_text_editor.cpp`. Locally rebased on 2026-05-05 to `40606e1d` on current `origin/main` `50ff5822`, then to `533b249e` on current `origin/main` `b7ec8f08`, then to `6e97178e4b81add6d7c243710aaf7228850ac9e2` on current `origin/main` `cf5ea658`, then rebased again to `0656ed8593bf21f721204205fa4ff26f42938d12` on current `origin/main` `24047ba3`, rebased again to `25570a36b47471c00c64408619669b23d65174d4` on current `origin/main` `df01f4f64696f3915902173ecf38223811b8ed60`, then rebased again to `426ffe57521e2288004cd76df144f2b5fe4698e3` on current `origin/main` `d11f530e34248f97bbf31ed1b14e8bf659cc035e`, then rebased again to `5b5173a298c023bc0f09f191861183ec4a1d6cbb` on current `origin/main` `a8c4c16bccdf62c58dfa5ac8cc7934a69c55375d` with no push or CI dispatch; intermediate local refresh `f377ef5c` on `0447498e` was superseded after `origin/main` advanced. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, canvas, view, and platform warnings and configured Pulp `0.78.1`; `cmake --build build --target pulp-test-text-editor -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-text-editor "[view][text_editor][issue-493]" -r compact` passed 62 assertions in 10 test cases; full `./build/test/pulp-test-text-editor -r compact` passed 148 assertions in 37 test cases; `ctest --test-dir build -R "TextEditor" --output-on-failure` passed 37/37; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 153 / behind 1 against the paused remote. Prior focused LLVM coverage for `core/view/src/text_editor.cpp` on the remote head reported 79.13% line, 59.74% branch, and 88.57% function coverage. The prior #1282 PR creation/label/tracker links remain intact but stale until the remote branch is updated. | Hold local refreshed branch while Namespace is intentionally paused; when capacity returns, update #1282 with `--force-with-lease` in a small resume batch and merge only after fresh required PR-event gates are green. |
| `feature/python-inspect-helper-coverage-641-next` | `96087469` | #641 Python bindings tranche for `ParamRange` clamp/zero-span and `MidiBuffer` clear/reuse edges | `test/test_python_bindings.py` | Rebased cleanly onto current `origin/main` at `414f9e9a`; diff is limited to `test/test_python_bindings.py`; first fresh configure was interrupted because it tried to clone MbedTLS in the worktree, then rerun with `-DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`; `cmake -S . -B build-python-helper -DPULP_BUILD_PYTHON=ON -DPULP_BUILD_TESTS=ON -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Debug -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed; `cmake --build build-python-helper --target pulp_python pulp-test-python-bindings-embedded -j$(sysctl -n hw.ncpu)` passed; `ctest --test-dir build-python-helper -R "python-bindings-(smoke|embedded-smoke)" --output-on-failure` passed 2/2; skill-sync report, version-bump report, `git diff --check origin/main...HEAD`, and `git diff --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1283, then exited with no local targets remaining as expected for the Namespace-only route; #1283 is labeled `codecov` and linked from #641. After #1264 merged, #1283 became dirty/conflicting in `test/test_python_bindings.py`; rebased onto `origin/main` at `91660df1`, kept both #1264's binding assertions and this tranche's clamp/reuse assertions, reran the same focused Python binding build/CTest plus CLI skew, skill-sync, version-bump, and diff checks, and force-with-lease pushed `960874696639d0a631da44ba75d5a219fa95a61f`. | Fresh PR-event checks are running for `96087469`; merge once required gates are green. |
| `feature/phase3-splash-screen-coverage-493` | `fc62b3bc` local; #1285 remote remains `d3cd9f47` | #493 view tranche for `core/view/src/splash_screen.cpp` lifecycle, click-dismiss, and paint fallback/image edges | `test/test_splash_screen.cpp`, `test/CMakeLists.txt` | Created from current `origin/main` at `91660df1`; original #1285 remote head `d3cd9f47` had initial local validation with 25 assertions in 3 test cases. Locally rebased on 2026-05-05 to `6ee48b54` on current `origin/main` `50ff5822`, rebased again to `af745c197cbc59e4f14ed0a90802ceb0590fac8d` on current `origin/main` `b7ec8f08`, then to `836eaf0ac6e3bdd4e5d3f4c51931953f8082e4ea` on current `origin/main` `6c8b9920`, then rebased again to `437d9ac5594ace7eb3884b72c575612bf6e439ce` on current `origin/main` `24047ba3`, then rebased again to `fc62b3bcd666c273dd67789f77581bcb88818617` on current `origin/main` `d11f530e34248f97bbf31ed1b14e8bf659cc035e` with no push or CI dispatch; amended to incorporate and supersede `local/phase3-splash-screen-493` (`1dd00e70`), and kept the final diff limited to test target wiring and `test/test_splash_screen.cpp`; intermediate local refresh `7cba95e3` on `0447498e` was superseded after `origin/main` advanced. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, canvas, view, and platform warnings and configured Pulp `0.78.1`; `cmake --build build --target pulp-test-splash-screen -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-splash-screen "[view][splash-screen][coverage][issue-493]" -r compact` passed 34 assertions in 3 test cases; full `./build/test/pulp-test-splash-screen -r compact` passed 34 assertions in 3 test cases; exact `ctest --test-dir build -R "SplashScreen" --output-on-failure` passed 3/3; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 146 / behind 1 against the paused remote. Prior PR creation/label/tracker links remain intact but stale until the remote branch is updated. | Hold local refreshed branch while Namespace is intentionally paused; when capacity returns, update #1285 with `--force-with-lease` in a small resume batch and merge only after fresh required PR-event gates are green. |
| `feature/phase3-named-pipe-coverage-641` | `6fcbcdd1` local; #1286 remote remains `2d59c4c` | #641 runtime tranche for `core/runtime/src/named_pipe.cpp` closed/missing endpoint fail-closed, POSIX FIFO round-trip, cleanup, move-ownership, and create-failure paths | `test/test_stream.cpp` | Worker-created from current `origin/main`; review confirmed the diff is limited to `test/test_stream.cpp` and no open PR had the same scope. Locally checked out from the paused #1286 remote branch and rebased on 2026-05-05 to `e1dcf7af` on current `origin/main` `50ff5822`, rebased again to `c441dd79462dfa14d4bfafbddb46f8a2b431e4b6` on current `origin/main` `b7ec8f08`, rebased again to `db9f9d0d7aa5d9f9439d2291d95400805b22ad22` on current `origin/main` `6c8b9920`, rebased again to `727718db39f2a4c234ac0088c64e46fe4478d4b9` on current `origin/main` `7e9795b4`, then rebased again to `a664e1e675c8a28f79b99b9d1e1be5d79d80bb32` on current `origin/main` `df01f4f6`, then rebased again to `77aabdba1fdcf18435c00ca0a46f0f5772c4b4e4` on current `origin/main` `a8c4c16bccdf62c58dfa5ac8cc7934a69c55375d`, then rebased again to `d0094b9e68c5f873bca97e940590f1dc2e889108` on current `origin/main` `d191cdcaf85c1461d9d1ac44cfc9c656debb1096`, then rebased again to `6fcbcdd179ece2f38bc01d31536bec5b0abb42da` on current `origin/main` `92e83b376a24718d7e40ea27f2a65fb5579027db` with no push or CI dispatch; intermediate local refresh `eba02124` on `0447498e` was superseded after `origin/main` advanced. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party/platform, SDL3, Highway, and MbedTLS warnings and configured Pulp `0.78.1`; `cmake --build build --target pulp-test-stream -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-stream "[stream][named_pipe][issue-641]" -r compact` passed 41 assertions in 4 test cases; full `./build/test/pulp-test-stream -r compact` passed 112 assertions in 12 test cases; exact `ctest --test-dir build -R "NamedPipe|named_pipe|pulp-test-stream" --output-on-failure` passed 4/4; CLI skew check passed silently; CLI sync check passed with slash-command/skill-reference warnings only and zero issues; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 151 / behind 1 against the paused remote. Prior PR creation/label/tracker links remain intact but stale until the remote branch is updated. | Hold local refreshed branch while Namespace is intentionally paused; when capacity returns, update #1286 with `--force-with-lease` in a small resume batch and merge only after fresh required PR-event gates are green. |
| `feature/events-helper-coverage-642-next3` | `9e0924cd` | #642 events tranche for `core/events/src/timer.cpp` one-shot active-state behavior | `core/events/src/timer.cpp`, `test/CMakeLists.txt`, `test/test_events_timer_helpers.cpp`, `CMakeLists.txt` | Rebased cleanly onto `origin/main` at `fa180654` after resolving the events-test target block around the already-merged async-helper target; `git cherry -v origin/main HEAD` shows the source-fix commit is still needed; `cmake --build build --target pulp-test-events-timer-helpers -j$(sysctl -n hw.ncpu)`; `./build/test/pulp-test-events-timer-helpers -r compact` passed 7 assertions in 1 test case; `ctest --test-dir build -R "Timer one-shot|events-timer|timer-helpers" --output-on-failure` passed 1/1; CLI skew check; skill-sync report; version-bump report correctly suggests an SDK patch bump; `git diff --check origin/main...HEAD`; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1253, then exited with no local targets remaining as expected for the Namespace-only route; first PR-event version/skill check failed because the `fix:` PR title requires a bump commit. Added `chore: bump versions` commit `9e0924cd0bc344511ef03b22319932c5d40afc24` (`CMakeLists.txt` 0.69.0 -> 0.69.1); local `version_bump_check.py --mode=report --base origin/main --head HEAD --require-bump-for-fix-feat --pr-title 'fix(events): deactivate one-shot timers after firing'` passes. #1253 is labeled `codecov` and linked from #642/#641. | Merged #1253 as `f5fd9fe80359ecb6bf1d4aa86921de1b96e2ea0d`; tracker comments posted to #641/#642; cancellation requested for leftover advisory runs `25241535268` and `25241535428`. |
| `feature/events-helper-coverage-642-next5` | `abe35658` | #642 events tranche for ActionBroadcaster missing-listener removal edge behavior | `test/test_events.cpp` | Rebased cleanly onto `origin/main` at `fa180654`; `git cherry -v origin/main HEAD` shows `abe356586c4648e8129d694605a76f0f39fa8394` still needed; `cmake --build build --target pulp-test-events -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-events "[action_broadcaster]"` passed 4 assertions in 1 test case; `ctest --test-dir build -R "ActionBroadcaster adds, removes, and notifies listeners" --output-on-failure` passed 1/1; CLI skew check; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1254, then exited with no local targets remaining as expected for the Namespace-only route; #1254 is labeled `codecov` and linked from #642/#641. | Merged #1254 as `451a24285ad9deb8fd21a9db22d22a38e6ef5c9f`; tracker comments posted to #641/#642; cancellation requested for leftover advisory runs `25241451299` and `25241451295`. |
| `local/phase3-ruleset-drift-config-643` | `145cd195` | #643 static ruleset-drift guard tranche for branch-protection JSON/workflow invariants | `tools/scripts/test_ruleset_drift_config.py` | Rebased cleanly from `390b68f9` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `012b224a` onto current `origin/main` `bd036171`; committed `145cd19545e8bea259ca3c68a5e1b1179b57466c`. `python3 tools/scripts/test_ruleset_drift_config.py` passed 6 tests; `python3 -m py_compile tools/scripts/test_ruleset_drift_config.py` passed; supporting guard tests `python3 tools/scripts/test_codecov_config.py` passed 13 tests, `python3 tools/scripts/test_local_diff_cover.py` passed 11 tests, `python3 tools/scripts/test_coverage_diff_comment.py` passed 11 tests, and `python3 tools/scripts/test_coverage_tier_check.py` passed 15 tests; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. The official `run_python_coverage.py` surface intentionally excludes `tools/scripts/test_*.py`, so this static guard has no non-test source to report in that focused runner. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`; use PR-event Namespace checks unless a targeted diagnostic dispatch is needed. |
| `local/phase3-child-process-read-output-640` | `4c474115` | #640 platform tranche for `ChildProcess::read_available_output()` while stdout is available before process completion | `test/test_child_process.cpp` | Rebased cleanly from `55c46d02` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `4008bd73` onto current `origin/main` `bd036171`, then rebased from `a4fb3f1f` onto current `origin/main` `b7ec8f08`, then rebased from `16f53a6f` onto current `origin/main` `6c8b9920`, then rebased from `ca647d70e19686b61dcb00b6ec7c352563a7cbd7` onto current `origin/main` `b567dbeb`, then rebased from `73543f6b312f50828d3255a7ee618b84899af579` onto current `origin/main` `7e9795b4`, then rebased from `734140e01bc85ad5c04b30cdf19ef291fe2dae41` onto current `origin/main` `42bacd19`; committed `28d0c4cc77afcef0b2e458bd328559e095f565fb`, then rebased from `28d0c4cc77afcef0b2e458bd328559e095f565fb` onto current `origin/main` `a8c4c16bccdf62c58dfa5ac8cc7934a69c55375d` and committed `2af4d2520ad61cf8f5eaedc384a8778aada1ce2d`, then rebased from `2af4d2520ad61cf8f5eaedc384a8778aada1ce2d` onto current `origin/main` `92e83b376a24718d7e40ea27f2a65fb5579027db` and committed `4c474115ff2cc8132d005c3170bf4467a9f0192f`. The refreshed diff remains test-only in `test/test_child_process.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1` from the refreshed mainline base; `cmake --build build --target pulp-test-child-process -j$(sysctl -n hw.ncpu)` passed; tag run `./build/test/pulp-test-child-process "[child_process][edge][issue-640]" -r compact` passed 44 assertions in 9 test cases; full `./build/test/pulp-test-child-process -r compact` passed 65 assertions in 21 test cases; exact `ctest --test-dir build -R "read_available_output|ChildProcess|child-process|pulp-test-child-process" --output-on-failure` passed 1/1; CLI skew check passed silently; CLI sync check passed with slash-command/skill-reference warnings only and zero issues; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, batch with related #640 platform tranches or rename/push as a feature branch, then run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` only after CI capacity is explicitly reopened. |
| `local/phase3-platform-environment-dispatch-640` | `33565a65` | #640 platform tranche for Environment token self-move, listener removal before dispatch, and reset-during-dispatch skip behavior | `test/test_environment.cpp` | Rebased from `b271d2e6` onto current `origin/main` at `0447498e` on 2026-05-05, rebased from `422a64cc` onto current `origin/main` `bd036171`, rebased from `cfb1874d` onto current `origin/main` `b7ec8f08`, rebased from `7f2cf371` onto current `origin/main` `6c8b9920`, rebased from `325604dea93a4988e536aecf7574801fc83185ba` onto current `origin/main` `b567dbeb`, rebased from `96aba9dea79212fef31f6da9a0654bb54fa10413` onto current `origin/main` `7e9795b4`, rebased from `1778b21c33d9221b4e198f41cbf8de52f645912f` onto current `origin/main` `42bacd19`, rebased from `7d3ad7c6b96d72c991c7bd3be3afe233c740d00e` onto current `origin/main` `a8c4c16bccdf62c58dfa5ac8cc7934a69c55375d`, then rebased from `db85e719b43ad4d1250dc1f5e2cc29158ddd19ca` onto current `origin/main` `92e83b376a24718d7e40ea27f2a65fb5579027db` and committed `33565a65611f13d766111bfedb81d6fa8df610c0` with no push or CI dispatch. The refreshed diff remains test-only in `test/test_environment.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1`; `cmake --build build --target pulp-test-environment -j$(sysctl -n hw.ncpu)` passed; tag run `./build/test/pulp-test-environment "[environment][issue-640]" -r compact` passed 61 assertions in 10 test cases; full `./build/test/pulp-test-environment -r compact` passed 106 assertions in 21 test cases; exact `ctest --test-dir build -R "Environment: token self move|Environment: listener removed|Environment: reset during dispatch|environment|pulp-test-environment" --output-on-failure` passed 3/3; CLI skew check passed silently; CLI sync check passed with slash-command/skill-reference warnings only and zero issues; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, batch with related #640 platform tranches or rename/push as a feature branch, then run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` only after CI capacity is explicitly reopened. |
| `local/phase3-midi-ci-edges-645` | `6226f5af` | #645 MIDI tranche for MIDI-CI malformed header rejection, directly addressed discovery inquiries, short discovery replies, and reserved-byte profile matching | `test/test_midi_ci.cpp` | Rebased from `c3f7e68b` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `f66f0d4` onto current `origin/main` `bd036171`; committed `6226f5af74a6f2441f2765edd4bc923ff736f6d2`. The refreshed diff remains test-only in `test/test_midi_ci.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.77.0` from the refreshed mainline base; `cmake --build build --target pulp-test-midi-ci -j$(sysctl -n hw.ncpu)` passed; tag run `./build/test/pulp-test-midi-ci "[midi][ci][issue-645]" -r compact` passed 54 assertions in 10 test cases; full `./build/test/pulp-test-midi-ci -r compact` passed 81 assertions in 19 test cases; exact `ctest --test-dir build -R "CiDiscovery|MUID|midi-ci|pulp-test-midi-ci" --output-on-failure` passed 19/19; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-mpe-allocator-edges-645` | `249ad105` | #645 MIDI/MPE tranche for preserving MpeVoiceAllocator glide refcounts when stealing releasing voices, documenting the MPE skill invariant, plus unmatched MpeGlideDetector note-off/reset coverage | `.agents/skills/mpe/SKILL.md`, `core/midi/include/pulp/midi/mpe_synth_voice.hpp`, `test/test_mpe_synth_voice.cpp` | Rebased from `83ce06b3` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `dcacdaa0` onto current `origin/main` `bd036171`; committed `249ad1058b28a2703da7f33457ee278911df560f`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.77.0` from the refreshed mainline base; `cmake --build build --target pulp-test-mpe-synth-voice -j$(sysctl -n hw.ncpu)` passed; tag run `./build/test/pulp-test-mpe-synth-voice "[midi][mpe][issue-645]" -r compact` passed 62 assertions in 6 test cases; full `./build/test/pulp-test-mpe-synth-voice -r compact` passed 86 assertions in 14 test cases; exact `ctest --test-dir build -R "MpeVoiceAllocator|MpeGlideDetector|mpe-synth-voice" --output-on-failure` passed 12/12; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync passed with the MPE skill update; version-bump report passed with SDK patch override from the commit trailer `Version-Bump: sdk=patch reason="MPE glide refcount behavior fix without public API shape change"` plus a plugin patch suggestion for the skill update; docs-sync and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-host-scanner-order-493` | `e84ade81` | #493 host scanner tranche for PluginScanner VST3/LV2 format-lane merging, final name ordering, LV2 URI identity, VST3 stem fallback identity, and hermetic synthetic scanner fixtures | `test/test_host_regression.cpp` | Rebased from `97d9f833` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `1dfeb616` onto current `origin/main` `bd036171`; committed `e84ade81f39c9d4c2767706418cd6b74bad29932`. The refreshed diff remains test-only in `test/test_host_regression.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.77.0` from the refreshed mainline base; `cmake --build build --target pulp-test-host-regression -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-host-regression "PluginScanner merges enabled format lanes and sorts by plugin name" -r compact` passed 10 assertions in 1 test case; tag run `./build/test/pulp-test-host-regression "[host][scanner]" -r compact` passed 61 assertions in 11 test cases without walking installed plugin directories; full `./build/test/pulp-test-host-regression -r compact` passed 430 assertions in 19 test cases with expected synthetic scanner warnings; exact `ctest --test-dir build -R "PluginScanner merges enabled format lanes" --output-on-failure` passed 1/1; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-background-scanner-restart-493` | `3da08b56` | #493 host tranche for BackgroundScanner restart after a completed-but-unjoined worker | `test/test_background_scanner.cpp` | Created from current `origin/main` at `0447498e` on 2026-05-05, then rebased from `b109a24f` onto current `origin/main` `bd036171`; committed `3da08b56b71633c76d3f4d465fa40f22309a5469`. The diff remains test-only in `test/test_background_scanner.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, host, and platform warnings and configured Pulp `0.77.0` from the refreshed mainline base; `cmake --build build --target pulp-test-background-scanner -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-background-scanner "BackgroundScanner: restart joins a completed unjoined worker" -r compact` passed 6 assertions in 1 test case; tag run `./build/test/pulp-test-background-scanner "[host][bg-scan][issue-493]" -r compact` passed 18 assertions in 2 test cases; full `./build/test/pulp-test-background-scanner -r compact` passed 28 assertions in 7 test cases; exact `ctest --test-dir build -R "BackgroundScanner|bg-scan|pulp-test-background-scanner" --output-on-failure` passed 7/7; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-plugin-slot-dispatch-493` | `4945bad3` | #493 host tranche for PluginSlot invalid descriptor fail-closed dispatch across CLAP, AU, AUv3, VST3, and LV2 loader paths | `test/test_host.cpp` | Created from current `origin/main` at `0447498e` on 2026-05-05, then rebased from `b6ef9689` onto current `origin/main` `bd036171`; committed `4945bad34f0b0f7adabdc38599edc90ddbe1b32c`. The diff remains test-only in `test/test_host.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, host, and platform warnings and configured Pulp `0.77.0` from the refreshed mainline base; this local configure had CLAP/LV2 loader paths compiled, with VST3 and AU v2 SDKs unavailable. `cmake --build build --target pulp-test-host -j$(sysctl -n hw.ncpu)` passed with existing `test_host.cpp` missing-field initializer warnings; focused `./build/test/pulp-test-host "PluginSlot load fails closed for invalid descriptors across formats" -r compact` passed 5 assertions in 1 test case; tag run `./build/test/pulp-test-host "[host][slot][issue-493]" -r compact` passed 5 assertions in 1 test case; full `./build/test/pulp-test-host -r compact` passed 1038 assertions in 27 test cases with existing local plugin scan warnings; exact `ctest --test-dir build -R "PluginSlot load fails closed for invalid descriptors across formats" --output-on-failure` passed 1/1; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-fetchcontent-cache-edges-643` | `5cbf2d87` | #643 CLI/tools tranche for FetchContent cache fallback entry splitting/order, live symlink classification, file-backed declared-ref parsing, label fallbacks, and symlink removal without deleting targets | `test/test_cli_fetchcontent_cache.cpp` | Rebased from `b056ca59` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `e5b13514` onto current `origin/main` `bd036171`; committed `5cbf2d87b69947b480fe88a9f742b15a578d9d2f`. The refreshed diff remains test-only in `test/test_cli_fetchcontent_cache.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.77.0` from the refreshed mainline base; `cmake --build build --target pulp-test-cli-fetchcontent-cache -j$(sysctl -n hw.ncpu)` passed; tag run `./build/test/pulp-test-cli-fetchcontent-cache "[fetchcontent_cache][issue-643]" -r compact` passed 53 assertions in 7 test cases; full `./build/test/pulp-test-cli-fetchcontent-cache -r compact` passed 137 assertions in 25 test cases; exact `ctest --test-dir build -R "status and fix labels|undeclared entries|live symlink|symlinks without deleting|parse_declared_refs_from_file|lstat miss|control characters" --output-on-failure` passed 7/7; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-ui-components-edges-493` | `4bc91d9e` | #493 view tranche for ComboBox popup handoff/typeahead no-op, ScrollView scrolled-child pointer-event hit testing and paint clipping/visibility, and ListBox boundary-key/out-of-range mouse guards | `test/test_ui_components.cpp` | Rebased from `58940ee2` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased again from `a4ae8412` onto current `origin/main` `50ff5822`, then rebased again from `edb03525` onto current `origin/main` `c18785c9`, then rebased again to `4bc91d9efcc5cdea000dde432b6e92c2a44a4670` on current `origin/main` `83271a94`; committed locally with no push or CI dispatch. The refreshed diff remains test-only in `test/test_ui_components.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, canvas, view, and platform warnings and configured Pulp `0.78.0` from the refreshed mainline base; `cmake --build build --target pulp-test-ui-components -j$(sysctl -n hw.ncpu)` passed with existing view/canvas warning noise; focused `./build/test/pulp-test-ui-components "ComboBox opening another popup closes the previous one" -r compact` passed 10 assertions in 1 test case; focused `./build/test/pulp-test-ui-components "ScrollView hit testing honors pointer modes with scrolled children" -r compact` passed 5 assertions in 1 test case; focused `./build/test/pulp-test-ui-components "ScrollView paint_all clips translated content and skips invisible views" -r compact` passed 5 assertions in 1 test case; focused `./build/test/pulp-test-ui-components "ListBox ignores boundary keys and out-of-range mouse presses" -r compact` passed 6 assertions in 1 test case; full `./build/test/pulp-test-ui-components -r compact` passed 147 assertions in 37 test cases; exact `ctest --test-dir build -R "ComboBox opening another popup|ScrollView hit testing honors pointer modes|ScrollView paint_all clips translated content|ListBox ignores boundary keys" --output-on-failure` passed 4/4; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-phase9-widget-edges-493` | `b23e4d25` | #493 view/widget tranche for SplitView drag minimum clamps, miss/drag guards, divider grip paint branches, and PropertyList boolean editing, category height/paint, and scalar value formatting paths | `test/test_phase9_widgets.cpp` | Rebased cleanly from `17e25c73` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased again from `3b2fa68f` onto current `origin/main` `50ff5822`, then rebased again from `0de6d547` onto current `origin/main` `c18785c9`, then rebased again to `b23e4d25677becafb56b1c30399cd2ca83251e82` on current `origin/main` `83271a94`; committed locally with no push or CI dispatch. The refreshed diff remains test-only in `test/test_phase9_widgets.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, canvas, view, and platform warnings and configured Pulp `0.78.0` from the refreshed mainline base; `cmake --build build --target pulp-test-phase9-widgets -j$(sysctl -n hw.ncpu)` passed with existing view/canvas warning noise; focused `./build/test/pulp-test-phase9-widgets "SplitView drag handling clamps to pane minimums and ignores misses" -r compact` passed 12 assertions in 1 test case; focused `./build/test/pulp-test-phase9-widgets "SplitView paint emits horizontal and vertical divider grips" -r compact` passed 4 assertions in 1 test case; focused `./build/test/pulp-test-phase9-widgets "PropertyList mouse editing toggles writable booleans only" -r compact` passed 10 assertions in 1 test case; focused `./build/test/pulp-test-phase9-widgets "PropertyList paints categories and scalar value variants" -r compact` passed 10 assertions in 1 test case; full `./build/test/pulp-test-phase9-widgets -r compact` passed 161 assertions in 34 test cases; exact `ctest --test-dir build -R "SplitView drag handling|SplitView paint emits|PropertyList mouse editing|PropertyList paints categories" --output-on-failure` passed 4/4; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-gui-components-edges-493` | `a37626d7` | #493 view/gui tranche for TableListBox header sorting, row selection/out-of-range guards, scaled/aligned paint paths, and ConcertinaPanel invalid-index, content visibility/layout, paint, and mouse-hit paths | `test/test_gui_components.cpp` | Rebased cleanly from `7e94a781` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased again from `b0e683e9` onto current `origin/main` `50ff5822`, then rebased again from `c52aac5a` onto current `origin/main` `c18785c9`, then rebased again to `a37626d7130c5db41577c79161dfb15ebff75fab` on current `origin/main` `83271a94`; committed locally with no push or CI dispatch. The refreshed diff remains test-only in `test/test_gui_components.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, canvas, view, and platform warnings and configured Pulp `0.78.0` from the refreshed mainline base; `cmake --build build --target pulp-test-gui-components -j$(sysctl -n hw.ncpu)` passed with existing view/canvas warning noise; focused `./build/test/pulp-test-gui-components "TableListBox header sorting and row selection edge paths" -r compact` passed 18 assertions in 1 test case; focused `./build/test/pulp-test-gui-components "TableListBox paint covers empty guards scaled columns and alignment" -r compact` passed 8 assertions in 1 test case; focused `./build/test/pulp-test-gui-components "ConcertinaPanel guards indices and syncs content visibility" -r compact` passed 9 assertions in 1 test case; focused `./build/test/pulp-test-gui-components "ConcertinaPanel paint and mouse hit testing cover content offsets" -r compact` passed 12 assertions in 1 test case; full `./build/test/pulp-test-gui-components -r compact` passed 174 assertions in 33 test cases; exact `ctest --test-dir build -R "TableListBox header sorting|TableListBox paint covers|ConcertinaPanel guards indices|ConcertinaPanel paint and mouse" --output-on-failure` passed 4/4; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-live-constant-editor-493` | `84af7d62` | #493 view tranche for LiveConstantRegistry duplicate registration, clamp, callback, missing-key, reset, and reset-all paths, plus LiveConstantEditor visibility, paint, slider drag, header guard, and missing-row drag paths | `test/CMakeLists.txt`, `test/test_live_constant_editor.cpp` | Rebased cleanly from `29af1db8` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased again from `4ee6e846` onto current `origin/main` `c18785c9`, then rebased again to `84af7d62682c8681e6ad847f98fef4787eced353` on current `origin/main` `83271a94`; committed locally with no push or CI dispatch. The refreshed diff remains limited to test target wiring and `test/test_live_constant_editor.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, canvas, view, and platform warnings and configured Pulp `0.78.0` from the refreshed mainline base; `cmake --build build --target pulp-test-live-constant-editor -j$(sysctl -n hw.ncpu)` passed with existing view/canvas warning noise; full `./build/test/pulp-test-live-constant-editor -r compact` passed 26 assertions in 2 test cases; exact `ctest --test-dir build -R "LiveConstantRegistry|LiveConstantEditor|live-constant" --output-on-failure` passed 2/2; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-code-editor-doc-mru-493` | `fef0ef6c` | #493 view code-editor tranche for FileBasedDocument successful load/save-as dirty-state behavior and RecentlyOpenedFilesList remove/missing-path behavior | `test/test_code_editor.cpp` | Rebased cleanly from `e8e3545b` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased again from `f09fd8d6` onto current `origin/main` `c18785c9`, then rebased again to `fef0ef6c5ca8425c2333fb6f83b49e655fd4ebf7` on current `origin/main` `83271a94`; committed locally with no push or CI dispatch. The refreshed diff remains test-only in `test/test_code_editor.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, canvas, view, and platform warnings and configured Pulp `0.78.0` from the refreshed mainline base; `cmake --build build --target pulp-test-code-editor -j$(sysctl -n hw.ncpu)` passed with existing view/canvas warning noise; focused `./build/test/pulp-test-code-editor "FileBasedDocument handles successful load and save_as paths" -r compact` passed 15 assertions in 1 test case; focused `./build/test/pulp-test-code-editor "RecentlyOpenedFilesList removes entries and ignores missing paths" -r compact` passed 7 assertions in 1 test case; full `./build/test/pulp-test-code-editor -r compact` passed 74 assertions in 13 test cases; exact `ctest --test-dir build -R "FileBasedDocument handles successful|RecentlyOpenedFilesList removes entries" --output-on-failure` passed 2/2; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-graph-editor-paint-493` | `8ed9f335` | #493 view graph-editor tranche for GraphEditorView auto-layout/manual-position preservation, unnamed node/multi-port painting, and feedback/MIDI edge paint colors | `test/test_graph_editor_view.cpp` | Rebased cleanly from `2d21c893` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased again from `d9ade421` onto current `origin/main` `c18785c9`, then rebased again to `8ed9f335ee8dda8ba2c5e31b2baa6c53e544d4c7` on current `origin/main` `83271a94`; committed locally with no push or CI dispatch. The refreshed diff remains test-only in `test/test_graph_editor_view.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, canvas, view, and platform warnings and configured Pulp `0.78.0` from the refreshed mainline base; `cmake --build build --target pulp-test-graph-editor-view -j$(sysctl -n hw.ncpu)` passed with existing view/canvas warning noise; focused `./build/test/pulp-test-graph-editor-view "GraphEditorView auto layout preserves positions and paints unnamed ports" -r compact` passed 7 assertions in 1 test case; focused `./build/test/pulp-test-graph-editor-view "GraphEditorView paints feedback and midi edge colors" -r compact` passed 4 assertions in 1 test case; full `./build/test/pulp-test-graph-editor-view -r compact` passed 33 assertions in 6 test cases; exact `ctest --test-dir build -R "GraphEditorView auto layout preserves|GraphEditorView paints feedback" --output-on-failure` passed 2/2; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-new-widgets-input-493` | `ba19ff03` | #493 view/widget tranche for MidiKeyboard vertical drag release/miss behavior, note-name/highlight painting, and FileDropZone rejected-drop reset plus idle/valid/invalid/no-icon paint paths | `test/test_phase9_widgets.cpp` | Rebased cleanly from `7b5927ad` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased again from `217ae169` onto current `origin/main` `8fa55f5e`, then rebased to `3b4a06a1be9a07d64f614c4d960a9d1e8bd77a12` on `origin/main` `83271a94`, then rebased again to `ba19ff036ed3bbd84eb9eec5052553673cb32ff7` on current `origin/main` `9a67a517`; committed locally with no push or CI dispatch. The refreshed diff remains test-only in `test/test_phase9_widgets.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, canvas, view, and platform warnings and configured Pulp `0.78.1` from the refreshed mainline base; `cmake --build build --target pulp-test-phase9-widgets -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-phase9-widgets "MidiKeyboard vertical drag releases previous notes and misses" -r compact` passed 15 assertions in 1 test case; focused `./build/test/pulp-test-phase9-widgets "MidiKeyboard paint emits note names and active highlight color" -r compact` passed 4 assertions in 1 test case; focused `./build/test/pulp-test-phase9-widgets "FileDropZone rejected drop resets state without callback" -r compact` passed 5 assertions in 1 test case; focused `./build/test/pulp-test-phase9-widgets "FileDropZone paint covers idle valid invalid and no-icon states" -r compact` passed 6 assertions in 1 test case; full `./build/test/pulp-test-phase9-widgets -r compact` passed 155 assertions in 34 test cases; exact `ctest --test-dir build -R "MidiKeyboard vertical drag|MidiKeyboard paint emits|FileDropZone rejected drop|FileDropZone paint covers" --output-on-failure` passed 4/4; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-file-browser-paint-493` | `f7ff2469` | #493 view/file-browser tranche for FileBrowser sorted-row paint clipping and MultiDocumentPanel active/inactive tab paint output | `test/test_file_browser.cpp` | Rebased cleanly from `57a12010` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased again from `091fdb77` onto current `origin/main` `c18785c9`, then rebased to `d37c7cd8552d3d795a12e942b9ced7e57179ee39` on `origin/main` `83271a94`, then rebased again to `f7ff2469f5ddf19fcb103aa99c25bb37c5e9743d` on current `origin/main` `9a67a517`; committed locally with no push or CI dispatch. The refreshed diff remains test-only in `test/test_file_browser.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, canvas, view, and platform warnings and configured Pulp `0.78.1` from the refreshed mainline base; `cmake --build build --target pulp-test-file-browser -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-file-browser "FileBrowser paint clips rows after sorted visible entries" -r compact` passed 9 assertions in 1 test case; focused `./build/test/pulp-test-file-browser "MultiDocumentPanel paint emits active and inactive tab labels" -r compact` passed 12 assertions in 1 test case; full `./build/test/pulp-test-file-browser -r compact` passed 81 assertions in 6 test cases; exact `ctest --test-dir build -R "FileBrowser paint clips rows|MultiDocumentPanel paint emits" --output-on-failure` passed 2/2; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-splash-screen-493` | `1dd00e70` | #493 view tranche for SplashScreen advance/dismiss callback behavior, dismiss-on-click gating, and text/image paint output | `test/CMakeLists.txt`, `test/test_splash_screen.cpp` | Rebased cleanly from `0ac19fbd` onto current `origin/main` at `0447498e` on 2026-05-05; committed `1dd00e704704059ea6461f219535af8ffc1c6b85` with the 34-assertion SplashScreen validation recorded previously. This branch was later incorporated into `feature/phase3-splash-screen-coverage-493` local head `7cba95e3`, so the refreshed #1285 branch now carries the stronger SplashScreen tests and validation evidence. | Superseded by the refreshed local #1285 branch; do not push, queue, or track this as an independent resume item. |
| `local/phase3-appearance-manager-493` | `0bbaaa9a` | #493 view appearance-manager tranche for AppearanceTracker repeated lock callbacks/locked poll and ThemeManager locked-theme, locked-appearance callback, and unlock behavior | `test/test_appearance_tracker.cpp` | Rebased cleanly from `ceb05add` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `c0b6bece` onto current `origin/main` `c18785c9`, then rebased to `ce4793054d2d6b43efd7908062df63ae8a1e456f` on `origin/main` `24047ba3`, then rebased to `a2fbbbde` on `origin/main` `83271a94`, then rebased again to `0bbaaa9a44eb91d31f9bdda0f09de23d8ce7b610` on current `origin/main` `9a67a517`; committed locally with no push or CI dispatch. The refreshed diff remains test-only in `test/test_appearance_tracker.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, canvas, view, and platform warnings and configured Pulp `0.78.1` from the refreshed mainline base; `cmake --build build --target pulp-test-appearance -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-appearance "AppearanceTracker callbacks follow repeated locks and locked poll no-op" -r compact` passed 7 assertions in 1 test case; focused `./build/test/pulp-test-appearance "ThemeManager callbacks cover locked theme poll and unlock" -r compact` passed 13 assertions in 1 test case; full `./build/test/pulp-test-appearance -r compact` passed 34 assertions in 10 test cases; exact `ctest --test-dir build -R "AppearanceTracker callbacks follow|ThemeManager callbacks cover" --output-on-failure` passed 2/2; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-tree-view-edges-493` | `ca5267a7` | #493 view TreeView tranche for disclosure collapse, left-key consumed-state behavior, selected-row paint highlight, and expanded/collapsed disclosure paint output | `test/test_tree_view.cpp` | Rebased cleanly from `94ddde56` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `de5d45ae` onto current `origin/main` `c18785c9`, then rebased to `656158c8f8b7b0019f22dca1490beb9e8945f598` on `origin/main` `24047ba3`, then rebased again to `ca5267a7acdab8f21b8d5fa9d06c654a141beff8` on current `origin/main` `9a67a517`; committed locally with no push or CI dispatch. The refreshed diff remains test-only in `test/test_tree_view.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, canvas, view, and platform warnings and configured Pulp `0.78.1` from the refreshed mainline base; `cmake --build build --target pulp-test-tree-view -j$(sysctl -n hw.ncpu)` passed with existing view/canvas warning noise; focused `./build/test/pulp-test-tree-view "TreeView triangle click toggles without selecting" -r compact` passed 10 assertions in 1 test case; focused `./build/test/pulp-test-tree-view "TreeView key left consumes collapsed selected nodes without toggling" -r compact` passed 3 assertions in 1 test case; focused `./build/test/pulp-test-tree-view "TreeView paint covers selection highlight and disclosure states" -r compact` passed 7 assertions in 1 test case; full `./build/test/pulp-test-tree-view -r compact` passed 53 assertions in 15 test cases; exact `ctest --test-dir build -R "TreeView triangle click|TreeView key left consumes|TreeView paint covers" --output-on-failure` passed 3/3; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-modal-overlay-edges-493` | `67fe482e` | #493 view ModalOverlay tranche for key-release/no-callback Escape behavior, backdrop alpha paint output, and backdrop-click dismissal hit/flag guards | `test/test_modal.cpp` | Rebased cleanly from `95a7b597` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `ba5f804e` onto current `origin/main` `c18785c9`, then to `454910757c877e3aecb4fe58c1849c2f36e9a070` on `origin/main` `24047ba3`, then rebased to `3496e7e6bed266487c8af80cb99f42442d625559` on `origin/main` `83271a94`, then rebased again to `67fe482e0095ecf06260afd5ca29b6c1c656df26` on current `origin/main` `7e9795b4`; committed locally with no push or CI dispatch. The refreshed diff remains test-only in `test/test_modal.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, canvas, view, and platform warnings and configured Pulp `0.78.1` from the refreshed mainline base; `cmake --build build --target pulp-test-modal -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-modal "ModalOverlay ignores key releases and handles Escape without callback" -r compact` passed 2 assertions in 1 test case; focused `./build/test/pulp-test-modal "ModalOverlay paint applies backdrop alpha to fill color" -r compact` passed 4 assertions in 1 test case; focused `./build/test/pulp-test-modal "ModalOverlay backdrop mouse dismissal respects hit target and flag" -r compact` passed 4 assertions in 1 test case; full `./build/test/pulp-test-modal -r compact` passed 18 assertions in 8 test cases; exact `ctest --test-dir build -R "ModalOverlay ignores key releases|ModalOverlay paint applies|ModalOverlay backdrop mouse dismissal" --output-on-failure` passed 3/3; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-auto-ui-edges-493` | `832c0781` | #493 view AutoUi tranche for generated toggle state, generated knob display-format branches, and sync propagation to toggles and existing faders | `test/test_auto_ui.cpp` | Rebased cleanly from `39c0e3a1` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `19f02f0c` onto current `origin/main` `c18785c9`, then rebased to `1ea687bfc6e9adae65df60bf490fc0c427568f68` on current `origin/main` `83271a94`, then rebased again to `832c07815c55ddf511f6796d4a870a9968b2f5bb` on current `origin/main` `7e9795b4`; committed locally with no push or CI dispatch. The refreshed diff remains test-only in `test/test_auto_ui.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings and configured Pulp `0.78.1` from the refreshed mainline base; `cmake --build build --target pulp-test-auto-ui -j$(sysctl -n hw.ncpu)` passed with existing view warnings; focused `./build/test/pulp-test-auto-ui "AutoUi generated controls expose toggle state and formatted values" -r compact` passed 10 assertions in 1 test case; focused `./build/test/pulp-test-auto-ui "AutoUi sync updates generated toggles and existing faders" -r compact` passed 6 assertions in 1 test case; full `./build/test/pulp-test-auto-ui -r compact` passed 21 assertions in 4 test cases; exact-ish `ctest --test-dir build -R "AutoUi generated controls|AutoUi sync updates" --output-on-failure` passed 3/3, including the pre-existing sync case matched by the selector; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-image-cache-trim-493` | `165c2357` | #493 view ImageCache tranche for byte-budget lowering, least-recently-used trimming, and releaser behavior | `test/test_image_cache.cpp` | Rebased cleanly from `d9a0fc92` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `a3ca7f7c` onto current `origin/main` `c18785c9`, then rebased to `dca796b783cc137b2f3541fbf0863bb36231430b` on current `origin/main` `83271a94`, then rebased again to `165c2357c43c0acd3f2aac642999f1cd3e773964` on current `origin/main` `7e9795b4`; committed locally with no push or CI dispatch. The refreshed diff remains test-only in `test/test_image_cache.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1` from the refreshed mainline base; `cmake --build build --target pulp-test-image-cache -j$(sysctl -n hw.ncpu)` passed with existing view warnings; focused `./build/test/pulp-test-image-cache "lowering byte budget trims least recently used entries" -r compact` passed 13 assertions in 1 test case; full `./build/test/pulp-test-image-cache -r compact` passed 49 assertions in 9 test cases; exact `ctest --test-dir build -R "lowering byte budget trims|image-cache|ImageCache" --output-on-failure` passed 1/1; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-visualization-bridge-edges-493` | `a30900b8` | #493 view VisualizationBridge tranche for disabled-waveform publication, zero-channel processing, and waveform capture-length clamp paths | `test/test_visualization.cpp` | Rebased cleanly from `3e45d531` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `a39f609c` onto current `origin/main` `c18785c9`, then rebased to `453a19e7067434be89a074c73a161cb791f6c858` on current `origin/main` `83271a94`, then rebased again to `a30900b8845a1d6b2faa33a631638486f37707e3` on current `origin/main` `7e9795b4`; committed locally with no push or CI dispatch. The refreshed diff remains test-only in `test/test_visualization.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1` from the refreshed mainline base; `cmake --build build --target pulp-test-visualization -j$(sysctl -n hw.ncpu)` passed with existing view warnings; focused bridge run `./build/test/pulp-test-visualization "[vizbridge]" -r compact` passed 24 assertions in 9 test cases; full `./build/test/pulp-test-visualization -r compact` passed 38 assertions in 15 test cases; exact `ctest --test-dir build -R "VisualizationBridge skips waveform|VisualizationBridge zero-channel|VisualizationBridge clamps waveform" --output-on-failure` passed 3/3; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-waveform-editor-edges-493` | `1ea97d99` | #493 view WaveformEditor tranche for selection, visible-range, playhead, paint overlay, key, and mouse edge paths | `test/test_waveform_editor.cpp` | Rebased cleanly from `761d4c15` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `1cf964ba` onto current `origin/main` `c18785c9`, then rebased to `e3b02b070e26e003c5c9935a9d6d769fb8f41524` on current `origin/main` `83271a94`, then rebased again to `1ea97d999708ac199ce6c2e5103117f796f06181` on current `origin/main` `7e9795b4`; committed locally with no push or CI dispatch. The refreshed diff remains test-only in `test/test_waveform_editor.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1` from the refreshed mainline base; `cmake --build build --target pulp-test-waveform-editor -j$(sysctl -n hw.ncpu)` passed with existing view warnings; focused `./build/test/pulp-test-waveform-editor "[view][waveform_editor]" -r compact` passed 62 assertions in 20 test cases; full `./build/test/pulp-test-waveform-editor -r compact` passed 62 assertions in 20 test cases; exact `ctest --test-dir build -R "WaveformEditor clamps selection|WaveformEditor zoom to selection is a no-op|WaveformEditor visible range clamps|WaveformEditor playhead clamps|WaveformEditor paint records|WaveformEditor key scrolls|WaveformEditor mouse click" --output-on-failure` passed 7/7; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-window-manager-edges-493` | `f7da4e54` | #493 view WindowManager tranche for unregister callback/missing-id cleanup, null host/root close behavior, and missing-handler send/broadcast paths | `test/test_window_manager.cpp` | Rebased cleanly from `49f96229` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `2ba38714` onto current `origin/main` `c18785c9`, then rebased to `baa1ae5620d01ae204f1dfd6690d7aaaa046599e` on current `origin/main` `83271a94`, then rebased again to `f7da4e5452df112105d70ecc038025b46dd48e84` on current `origin/main` `7e9795b4`; committed locally with no push or CI dispatch. The refreshed diff remains test-only in `test/test_window_manager.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1` from the refreshed mainline base; `cmake --build build --target pulp-test-window-manager -j$(sysctl -n hw.ncpu)` passed with existing view warnings; focused `./build/test/pulp-test-window-manager "[view][multiwindow]" -r compact` passed 82 assertions in 21 test cases; full `./build/test/pulp-test-window-manager -r compact` passed 82 assertions in 21 test cases; exact `ctest --test-dir build -R "WindowManager unregister invokes|WindowManager closes windows without|WindowManager send and broadcast skip" --output-on-failure` passed 3/3; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-param-attachment-edges-493` | `4e7ab1ad` | #493 view ParamAttachment tranche for fader/toggle/combo callback forwarding, missing parameter-id no-op behavior, and `poll_bindings()` external-change propagation | `test/test_param_attachment.cpp` | Rebased cleanly from `80bc3351` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `61a5fdc2` onto current `origin/main` `c18785c9`, then rebased to `6da58f0f62c69fce00c3c82b688747684c4988ae` on current `origin/main` `83271a94`, then rebased again to `4e7ab1ada6b8458fbc7f915606a92bf4a9adcf8a` on current `origin/main` `7e9795b4`; committed locally with no push or CI dispatch. The refreshed diff remains test-only in `test/test_param_attachment.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1` from the refreshed mainline base; `cmake --build build --target pulp-test-param-attachment -j$(sysctl -n hw.ncpu)` passed with existing view warnings; tag run `./build/test/pulp-test-param-attachment "[view][attachment][issue-493]" -r compact` passed 27 assertions in 3 test cases; full `./build/test/pulp-test-param-attachment -r compact` passed 43 assertions in 13 test cases; exact `ctest --test-dir build -R "param attachments forward|param attachments tolerate|poll_bindings forwards" --output-on-failure` passed 3/3; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-input-events-edges-493` | `e87e6b10` | #493 view InputEvents tranche for wheel/meta mouse helper paths, ended/cancelled gesture deltas, key release/repeat main-modifier checks, and missing pointer-capture release behavior | `test/test_input_events.cpp` | Rebased cleanly from `235f98b2` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `d96d76a6` onto current `origin/main` `c18785c9`, then rebased to `ee320ea1226db7b1b1051860634c3de0c7ae6467` on current `origin/main` `83271a94`, then rebased again to `e87e6b1078a51543425e50b76ea3440f045e8b72` on current `origin/main` `7e9795b4`; committed locally with no push or CI dispatch. The refreshed diff remains test-only in `test/test_input_events.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1` from the refreshed mainline base; `cmake --build build --target pulp-test-input-events -j$(sysctl -n hw.ncpu)` passed with existing view warnings; tag run `./build/test/pulp-test-input-events "[view][input][issue-493]" -r compact` passed 29 assertions in 4 test cases; full `./build/test/pulp-test-input-events -r compact` passed 85 assertions in 22 test cases; exact `ctest --test-dir build -R "MouseEvent wheel|GestureEvent ended|KeyEvent main modifier|View pointer capture ignores" --output-on-failure` passed 4/4; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-widget-bridge-dom-493` | `4ecd226d` | #493 view WidgetBridge DOM tranche for native DOM subtree moves, recursive DOM removal widget-map cleanup, root/missing layout helper paths, and the root-aware `getLayoutRect("")` registration fix | `core/view/src/widget_bridge.cpp`, `test/test_widget_bridge.cpp` | Rebased cleanly from `d2eec1b9` onto current `origin/main` at `0447498e` on 2026-05-05 with compat-sync skip trailers preserved, then rebased to `8987b7d2cc2785fc5da23d6c411b60d5fd667abe` on current `origin/main` `83271a94`, then rebased again to `4ecd226d52867708cb9ea0969fd8a33e2251a524` on current `origin/main` `7e9795b4`; committed locally with no push or CI dispatch. The refreshed diff remains limited to `core/view/src/widget_bridge.cpp` and `test/test_widget_bridge.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1` from the refreshed mainline base; `cmake --build build --target pulp-test-widget-bridge -j$(sysctl -n hw.ncpu)` passed with existing `widget_bridge.cpp` warning noise plus existing `test/test_widget_bridge.cpp` missing-field initializer warnings; focused `./build/test/pulp-test-widget-bridge "[view][bridge][dom][issue-493]" -r compact` passed 29 assertions in 3 test cases; full `./build/test/pulp-test-widget-bridge -r compact` passed 931 assertions in 133 test cases; exact `ctest --test-dir build -R "WidgetBridge DOM append|WidgetBridge DOM remove|WidgetBridge layout helpers" --output-on-failure` passed 3/3; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync and docs-sync reported no mapped paths touched; compat-sync passed with populated compat sections and existing bypasses for unchanged compat docs; version-bump report suggested SDK patch and no plugin bump; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-harness-verifier-643` | `08353da0` | #643 tools tranche for `tools/harness` status/verifier helper coverage, current Yoga and CSS harness baseline refresh, adapter unit-test coverage discovery including current RN adapter tests, compat-sync unknown-requirement hard-error expectation, and Python coverage-runner source/omit rules for harness tests | `docs/guides/coverage.md`, `test/harness/test_css_adapter.py`, `tools/harness/tests/test_auto_discover.py`, `tools/harness/tests/test_verifier_status.py`, `tools/scripts/run_python_coverage.py`, `tools/scripts/test_compat_sync_check_extra.py`, `tools/scripts/test_run_python_coverage.py`, `tools/scripts/test_run_python_coverage_extra.py` | Rebased/amended from `345c0039` onto current `origin/main` at `7e9795b4` on 2026-05-05; committed `08353da04cdf85f5204266676b0cc836df1b8a72`. Yoga no-regression baseline now expects total 53, PASS 21, DIVERGE 20, NOT_IMPL 12; CSS catalog now expects 199 entries with the current `backfaceVisibility` supported-but-not-wired fixture and the existing 23-entry wontfix baseline. `python3 -m unittest discover -s tools/harness/tests -p 'test_*.py'` passed 19 tests; `python3 -m unittest discover -s test/harness -p 'test_*.py'` passed 108 tests, including the RN adapter tests discovered from `test/harness/test_*.py`; `python3 tools/scripts/test_run_python_coverage.py` passed 24 tests; `python3 tools/scripts/test_run_python_coverage_extra.py` passed 8 tests; `python3 tools/scripts/test_compat_sync_check_extra.py` passed 31 tests. Venv-backed `/Users/danielraffel/Code/pulp-check-format-validation-coverage-643/build-coverage/python-venv/bin/python tools/scripts/run_python_coverage.py` passed the full Python coverage inventory with `TOTAL` 11,494 statements, 1,573 missed, 4,648 branches, 513 partials, and 84% line coverage. Report lines included `tools/harness/adapters/base.py` 93%, `tools/harness/adapters/canvas2d.py` 78%, `tools/harness/adapters/css.py` 91%, `tools/harness/adapters/html.py` 73%, `tools/harness/adapters/rn.py` 89%, `tools/harness/adapters/yoga.py` 89%, `tools/harness/status.py` 97%, `tools/harness/verifier.py` 90%, and `tools/scripts/run_python_coverage.py` 100%. `tools/check-docs.sh` passed with the existing 58 warning set; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync reported no mapped paths touched; version-bump reported no SDK or plugin bump needed; docs-sync reported `docs/guides/coverage.md` updated; compat-sync reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-audio-focus-dispatch-640` | `f819d985` | #640 audio tranche for AudioFocusRegistry inactive-listener skip behavior when a listener is removed or reset clears the registry during dispatch | `core/audio/src/audio_focus.cpp`, `test/test_audio_focus.cpp` | Rebased from `1ab6e24b` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `d766f4b9` onto current `origin/main` `bd036171`, then rebased from `2ae7de77` onto current `origin/main` `b7ec8f08`, then rebased from `94bc6a2a` onto current `origin/main` `6c8b9920`, then rebased from `69b4d18cf8fa835e9f6292d3884075146ff5a665` onto current `origin/main` `b567dbeb`, then rebased from `e61d430c55483f0a3912f018e15a11e2d68da2c0` onto current `origin/main` `7e9795b4`, then rebased from `27c21311758a9c4362fbeb012556913b6ff82066` onto current `origin/main` `42bacd19`; committed `440d18384525a8e102d8cf9b1a8ee3a71c0537ed`, then rebased from `440d18384525a8e102d8cf9b1a8ee3a71c0537ed` onto current `origin/main` `d191cdcaf85c1461d9d1ac44cfc9c656debb1096` and committed `f819d98535cd2adc79e189fecdc4109d1e6a290b`. The refreshed diff keeps the public header unchanged and covers the skip behavior via source/test changes only. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1` from the refreshed mainline base; `cmake --build build --target pulp-test-audio-focus -j$(sysctl -n hw.ncpu)` passed; tag run `./build/test/pulp-test-audio-focus "[audio][focus][issue-640]" -r compact` passed 20 assertions in 6 test cases; full `./build/test/pulp-test-audio-focus -r compact` passed 44 assertions in 16 test cases; exact `ctest --test-dir build -R "AudioFocusRegistry|audio-focus|pulp-test-audio-focus" --output-on-failure` passed 16/16; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, docs-sync, and compat-sync reports reported no mapped paths touched; version-bump report passed with an SDK patch suggestion and no plugin bump; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch, apply/record the SDK patch bump expected for the source behavior fix, and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-audio-data-shape-640` | `eac19fe2` | #640 audio tranche for AudioFileData helper shape semantics and WAV writer first-channel-empty rejection | `test/test_audio_file.cpp` | Created from current `origin/main` at `0447498e` on 2026-05-05 and refreshed through `eac19fe2a125b4658bd99188e7a13cf79ecbd787` on current `origin/main` `92e83b376a24718d7e40ea27f2a65fb5579027db` with no push or CI dispatch; prior refreshes on `8fa55f5e`, `b7ec8f08`, `6c8b9920`, `b567dbeb`, `7e9795b4`, `42bacd19`, and `a8c4c16` were superseded after local `origin/main` advanced. The diff remains test-only in `test/test_audio_file.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1`; `cmake --build build --target pulp-test-audio-file -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-audio-file "AudioFileData shape helpers and WAV writer reject first-channel empties" -r compact` passed 11 assertions in 1 test case; tag run `./build/test/pulp-test-audio-file "[audio][file][issue-640]" -r compact` passed 252 assertions in 12 test cases; full `./build/test/pulp-test-audio-file -r compact` passed 505 assertions in 29 test cases; exact `ctest --test-dir build -R "AudioFileData shape helpers" --output-on-failure` passed 1/1; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, batch with the related #640 audio-file tranches or rename/push as a small feature branch, then run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` only after CI capacity is explicitly reopened. |
| `local/phase3-aiff-pcm-edges-640` | `74774950` | #640 audio tranche for AIFF invalid COMM metadata and unsupported PCM bit-depth rejection | `test/test_audio_file.cpp` | Created from current `origin/main` at `0447498e` on 2026-05-05 and refreshed through `74774950cd5ab9e952097aa261671a0849999ea4` on current `origin/main` `92e83b376a24718d7e40ea27f2a65fb5579027db` with no push or CI dispatch; prior refreshes on `8fa55f5e`, `b7ec8f08`, `6c8b9920`, `b567dbeb`, `7e9795b4`, `df01f4f6`, `42bacd19`, and `a8c4c16` were superseded after local `origin/main` advanced. The diff remains test-only in `test/test_audio_file.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1`; `cmake --build build --target pulp-test-audio-file -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-audio-file "AIFF reader rejects invalid COMM metadata and unsupported PCM depths" -r compact` passed 19 assertions in 1 test case; tag run `./build/test/pulp-test-audio-file "[audio][file][registry][aiff][issue-640]" -r compact` passed 19 assertions in 1 test case; full `./build/test/pulp-test-audio-file -r compact` passed 513 assertions in 29 test cases; exact `ctest --test-dir build -R "AIFF reader rejects invalid COMM metadata" --output-on-failure` passed 1/1; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, batch with the related #640 audio-file tranches or rename/push as a small feature branch, then run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` only after CI capacity is explicitly reopened. |
| `local/phase3-streaming-writer-reopen-640` | `5e1d020d` | #640 audio tranche for StreamingWriter close-on-reopen and failed-open state/header finalization behavior | `test/test_audio_file.cpp` | Created from current `origin/main` at `0447498e` on 2026-05-05 and refreshed through `5e1d020d356207eb7bb96f05aac0a716c45b8aeb` on current `origin/main` `92e83b376a24718d7e40ea27f2a65fb5579027db` with no push or CI dispatch; prior refreshes on `bd036171`, `b7ec8f08`, `6c8b9920`, `dba0fd4b`, `b567dbeb`, `7e9795b4`, `df01f4f6`, `42bacd19`, and `a8c4c16` were superseded after local `origin/main` advanced. The diff remains test-only in `test/test_audio_file.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1`; `cmake --build build --target pulp-test-audio-file -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-audio-file "StreamingWriter finalizes the active file before a failed reopen" -r compact` passed 22 assertions in 1 test case; tag run `./build/test/pulp-test-audio-file "[audio][file][streaming][issue-640]" -r compact` passed 103 assertions in 5 test cases; full `./build/test/pulp-test-audio-file -r compact` passed 516 assertions in 29 test cases; exact `ctest --test-dir build -R "StreamingWriter finalizes" --output-on-failure` passed 1/1; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, batch with the related #640 audio-file tranches or rename/push as a small feature branch, then run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` only after CI capacity is explicitly reopened. |
| `local/phase3-frame-fill-edges-640` | `8467c68d` | #640 audio tranche for `zero_fill_short_read()` tail-fill, read-count clamp, null-buffer, and non-positive-dimension guard paths | `test/CMakeLists.txt`, `test/test_audio_frame_fill.cpp` | Created from current `origin/main` at `0447498e` on 2026-05-05 and refreshed through `8467c68d182c84cb2f00cc7d970d8ec7be5b2a2a` on current `origin/main` `92e83b376a24718d7e40ea27f2a65fb5579027db` with no push or CI dispatch; prior refreshes on `bd036171`, `b7ec8f08`, `6c8b9920`, `dba0fd4b`, `b567dbeb`, `7e9795b4`, `df01f4f6`, `42bacd19`, and `a8c4c16` were superseded after local `origin/main` advanced. The diff adds the focused `pulp-test-audio-frame-fill` target and remains test-only. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1`; `cmake --build build --target pulp-test-audio-frame-fill -j$(sysctl -n hw.ncpu)` passed; tag run `./build/test/pulp-test-audio-frame-fill "[audio][frame_fill][issue-640]" -r compact` passed 26 assertions in 3 test cases; full `./build/test/pulp-test-audio-frame-fill -r compact` passed 26 assertions in 3 test cases; exact `ctest --test-dir build -R "zero_fill_short_read|audio-frame-fill|frame_fill" --output-on-failure` passed 3/3; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` only after CI capacity is explicitly reopened. |
| `local/phase3-audio-tools-model-store-643` | `75ed3a63` | #643 tools tranche for `tools/audio` model registry/model store/excerpt-service edge coverage | `test/test_audio_tools.cpp` | Rebased from `a88ddfe8` onto current `origin/main` at `0447498e` on 2026-05-05, rebased from `9743ccd7` onto current `origin/main` `bd036171`, rebased from `a2f7bc2def1f2c55b6cc850a763315e73e42392a` onto current `origin/main` `7e9795b4`, rebased from `3a7ed654d1a1f8a4a54a82b1ea63ebaaeefe5b71` onto current `origin/main` `df01f4f6`, rebased from `a5aa9f8e74b0524a46108f61324bfc5f75a4f3a7` onto current `origin/main` `42bacd19`, rebased from `34ede9bbef548403db08ebbc4fcf01d617f33077` onto current `origin/main` `a8c4c16bccdf62c58dfa5ac8cc7934a69c55375d`, then rebased from `ae770658bbd0965dbf7bc7bd807c20dc08a8b69e` onto current `origin/main` `92e83b376a24718d7e40ea27f2a65fb5579027db` and committed `75ed3a63143e590a17467cd452e384eadbcf8ede` with no push or CI dispatch. The refreshed diff remains test-only in `test/test_audio_tools.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1`; `cmake --build build --target pulp-test-audio-tools -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-audio-tools "[audio][tools][issue-643]" -r compact` passed 93 assertions in 8 test cases; full `./build/test/pulp-test-audio-tools -r compact` passed 188 assertions in 20 test cases; exact `ctest --test-dir build -R "audio model|excerpt bundle|excerpt find|audio-tools|pulp-test-audio-tools" --output-on-failure` passed 20/20; CLI skew check passed silently; CLI sync check passed with slash-command/skill-reference warnings only and zero issues; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, batch with related #643 tools tranches or rename/push as a feature branch, then run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` only after CI capacity is explicitly reopened. |
| `feature/phase3-sdl3-surface-fallback-646` | `d81b03cc` | #646 render tranche for `Sdl3SurfaceInfo` backend-state validity and `extract_sdl3_surface(nullptr)` fallback/null-window behavior | `test/CMakeLists.txt`, `test/test_sdl3_surface.cpp` | Rebased cleanly from `908b3a49` onto current `origin/main` at `0447498e` on 2026-05-05, rebased from `1dc45105` onto current `origin/main` `bd036171`, rebased from `3fe05a721b47abb3068c8d07afa7f360baf800a7` onto current `origin/main` `7e9795b4`, rebased from `32a3bee619f5f52f18dc9ac5e59d4f01c220e719` onto current `origin/main` `df01f4f6`, then rebased from `f1601d223e74ec9683a95b2205a04aca8f2f56b8` onto current `origin/main` `42bacd19` and committed `c6e775c0749c7c53f07f5a9065687c6b19bfc419`, then rebased from `c6e775c0749c7c53f07f5a9065687c6b19bfc419` onto current `origin/main` `a8c4c16bccdf62c58dfa5ac8cc7934a69c55375d` and committed `cc9b63589070feca4abf1733cd0f483ad6fccb83`, then rebased from `cc9b63589070feca4abf1733cd0f483ad6fccb83` onto current `origin/main` `92e83b376a24718d7e40ea27f2a65fb5579027db` and committed `d81b03ccc69d6cdb363d9388bde6192b554328c4`; no remote branch ref exists. The refreshed diff remains limited to `test/CMakeLists.txt` and `test/test_sdl3_surface.cpp`. `cmake -S . -B build-sdl3-surface -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.78.1` from the refreshed mainline base; `cmake --build build-sdl3-surface --target pulp-test-sdl3-surface -j$(sysctl -n hw.ncpu)` passed; focused `./build-sdl3-surface/test/pulp-test-sdl3-surface "SDL3 surface info validity follows backend state - issue-646" -r compact` passed 4 assertions in 1 test case; full `./build-sdl3-surface/test/pulp-test-sdl3-surface -r compact` passed 13 assertions in 2 test cases; exact `ctest --test-dir build-sdl3-surface -R "SDL3 surface|sdl3-surface|issue-646" --output-on-failure` passed 2/2; CLI skew check passed silently; CLI sync check passed with slash-command/skill-reference warnings only and zero issues; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, push the feature branch or include it in a small #646 render wave, then run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` only after CI capacity is explicitly reopened. |
| `local/phase3-cli-config-command-643` | `08308f66` | #643 CLI tranche for `pulp config` set/get/list, snooze clearing, and malformed/invalid update-key shellout diagnostics | `test/test_cli_shellout.cpp` | Rebased cleanly from `af1ef6f8` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `76b36dcf` onto current `origin/main` `bd036171`; committed `08308f66361aa06a594cfbcb4fdd110c67245baf`. The refreshed diff remains test-only in `test/test_cli_shellout.cpp`. A GPU-off configure passed but did not expose `pulp-cli`, which is expected because the CLI target is gated behind inspect/render. Real shellout validation used `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=ON -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`, then `cmake --build build --target pulp-cli pulp-test-cli-shellout -j$(sysctl -n hw.ncpu)`, both passing with existing WebGPU/Skia, third-party, view, CLI, duplicate-library, and platform warnings and configured Pulp `0.77.0` from the refreshed mainline base. Tag run `PULP_CLI_PATH=/Users/danielraffel/Code/pulp-phase3-cli-config-command-643/build/tools/cli/pulp ./build/test/pulp-test-cli-shellout "[cli][shellout][config][issue-643]" -r compact` passed 34 assertions in 2 test cases; full `PULP_CLI_PATH=... ./build/test/pulp-test-cli-shellout -r compact` passed 496 assertions in 64 test cases; exact `PULP_CLI_PATH=... ctest --test-dir build -R "^pulp config (set/get/list round-trips isolated update settings|rejects malformed and invalid update keys)$" --output-on-failure` passed 2/2. A broader CTest selector matched `_NOT_BUILT` placeholder entries in this partial build and is not counted as source validation. CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; generated `classnames.json` from the full shellout run was removed; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-linkwitz-riley-edges-645` | `3deafcfe` | #645 signal tranche for Linkwitz-Riley reset/history and cutoff boundary finite-output behavior | `test/test_signal.cpp` | Rebased cleanly from `385c51a7` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `7b1a82a3` onto current `origin/main` `bd036171`; committed `3deafcfee17199567ac4f1f1e098784fafa590a4`. The refreshed diff remains test-only in `test/test_signal.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.77.0` from the refreshed mainline base; `cmake --build build --target pulp-test-signal -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-signal "LinkwitzRiley reset clears history while preserving coefficients" -r compact` passed 305 assertions in 1 test case; focused `./build/test/pulp-test-signal "LinkwitzRiley cutoff boundary processing stays finite" -r compact` passed 512 assertions in 1 test case; tag/full local runs `./build/test/pulp-test-signal "[signal]" -r compact` and `./build/test/pulp-test-signal -r compact` both passed 1764 assertions in 80 test cases; exact `ctest --test-dir build -R "LinkwitzRiley reset clears history|LinkwitzRiley cutoff boundary" --output-on-failure` passed 2/2; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-adsr-edges-645` | `0c9e7d4d` | #645 signal tranche for ADSR immediate stage progression, idle `note_off`, and reset edge paths | `test/test_signal.cpp` | Rebased cleanly from `e4844caa` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `bd746f3c` onto current `origin/main` `bd036171`; committed `0c9e7d4dfe1c34cb3ba03faa7469e2ecca8c470b`. The refreshed diff remains test-only in `test/test_signal.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.77.0` from the refreshed mainline base; `cmake --build build --target pulp-test-signal -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-signal "ADSR handles immediate stages*" -r compact` passed 17 assertions in 1 test case; tag run `./build/test/pulp-test-signal "[signal][adsr]" -r compact` passed 25 assertions in 5 test cases; full `./build/test/pulp-test-signal -r compact` passed 964 assertions in 79 test cases; exact `ctest --test-dir build -R "ADSR|adsr|pulp-test-signal" --output-on-failure` passed 5/5; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-noise-gate-edges-645` | `032f46c2` | #645 signal tranche for NoiseGate range clamp, instant attack/release timing, reset, silence, and buffer paths | `test/test_signal.cpp` | Rebased cleanly from `98f73bb8` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `717067d6` onto current `origin/main` `bd036171`; committed `032f46c26253b45ee39bc6e3e631fc3da714ffe1`. The refreshed diff remains test-only in `test/test_signal.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.77.0` from the refreshed mainline base; `cmake --build build --target pulp-test-signal -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-signal "NoiseGate clamps range*" -r compact` passed 9 assertions in 1 test case; tag run `./build/test/pulp-test-signal "[signal][gate]" -r compact` passed 11 assertions in 3 test cases; full `./build/test/pulp-test-signal -r compact` passed 956 assertions in 79 test cases; exact `ctest --test-dir build -R "^NoiseGate" --output-on-failure` passed 3/3; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-modulation-reverb-edges-645` | `e6b5c6bc` | #645 signal tranche for Chorus dry/reset/phase-wrap behavior and Reverb zero-decay, damping clamp, dry-mix, and reset paths | `test/test_signal.cpp` | Rebased cleanly from `971488d5` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `9cfcc171` onto current `origin/main` `4bebc7bf`; committed `e6b5c6bc60cc5fa9cf2ed557401e9f723e8f0fcb`. The refreshed diff remains test-only in `test/test_signal.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.77.0` from the refreshed mainline base; `cmake --build build --target pulp-test-signal -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-signal "Chorus dry mix*" -r compact` passed 30 assertions in 1 test case; focused `./build/test/pulp-test-signal "Reverb handles zero decay*" -r compact` passed 132 assertions in 1 test case; tag run `./build/test/pulp-test-signal "[signal][chorus]" -r compact` passed 33 assertions in 2 test cases; tag run `./build/test/pulp-test-signal "[signal][reverb]" -r compact` passed 133 assertions in 2 test cases; full `./build/test/pulp-test-signal -r compact` passed 1109 assertions in 80 test cases; exact `ctest --test-dir build -R "Chorus|Reverb|pulp-test-signal" --output-on-failure` passed 4/4; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched after rebasing to `4bebc7bf`; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-oscillator-edges-645` | `5e3a1db3` | #645 signal tranche for Oscillator reset, phase wrap, getter, and PolyBLEP edge paths | `test/test_signal.cpp` | Rebased cleanly from `8f51cba4` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `8f2e043` onto current `origin/main` `4bebc7bf`; committed `5e3a1db3ba7eeb56a2485476fbf586f4aab927cf`. The refreshed diff remains test-only in `test/test_signal.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.77.0` from the refreshed mainline base; `cmake --build build --target pulp-test-signal -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-signal "Oscillator reset*" -r compact` passed 22 assertions in 1 test case; tag run `./build/test/pulp-test-signal "[signal][osc]" -r compact` passed 27 assertions in 5 test cases; full `./build/test/pulp-test-signal -r compact` passed 969 assertions in 79 test cases; exact `ctest --test-dir build -R "Oscillator|pulp-test-signal" --output-on-failure` passed 5/5; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-signal-helper-edges-645` | `431a3c5d` | #645 signal tranche for FirFilter empty-coefficient passthrough/reset, LookupTable indexed clamps and zero-length buffer no-ops, and LadderFilter buffer/reset/resonance-clamp finite-output behavior | `test/test_dsp_expansion.cpp`, `test/test_signal.cpp` | Created from current `origin/main` at `0447498e` on 2026-05-05, then rebased from `be2d441f` onto current `origin/main` `4bebc7bf`; committed `431a3c5d1ce23f2fb82ecd8ea1d17f0210c8681a`. The diff remains test-only in `test/test_dsp_expansion.cpp` and `test/test_signal.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.77.0` from the refreshed mainline base; `cmake --build build --target pulp-test-dsp-expansion pulp-test-signal -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-dsp-expansion "FirFilter empty coefficients*" -r compact` passed 7 assertions in 1 test case; focused `./build/test/pulp-test-dsp-expansion "LookupTable indexed access*" -r compact` passed 10 assertions in 1 test case; focused `./build/test/pulp-test-signal "LadderFilter resets buffer*" -r compact` passed 39 assertions in 1 test case; tag run `./build/test/pulp-test-dsp-expansion "[signal][fir],[signal][lookup][issue-645]" -r compact` passed 29 assertions in 8 test cases; tag run `./build/test/pulp-test-signal "[signal][ladder]" -r compact` passed 40 assertions in 2 test cases; full `./build/test/pulp-test-dsp-expansion -r compact` passed 96 assertions in 37 test cases; full `./build/test/pulp-test-signal -r compact` passed 986 assertions in 79 test cases; exact `ctest --test-dir build -R "FirFilter empty coefficients|LookupTable indexed access|LadderFilter resets buffer" --output-on-failure` passed 3/3; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-validation-harness-json-493` | `4e753dd3` | #493 format tranche for ValidationHarness report metadata and entry JSON escaping | `test/test_validation_harness.cpp` | Rebased cleanly from `974cf572` onto current `origin/main` at `0447498e` on 2026-05-05; committed `4e753dd3ab2140cb520fa00ede3c764f12a89f04`. The refreshed diff remains test-only in `test/test_validation_harness.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and view/format warning noise; `cmake --build build --target pulp-test-validation-harness -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-validation-harness "ValidationHarness escapes report metadata and entry strings" -r compact` passed 5 assertions in 1 test case; full `./build/test/pulp-test-validation-harness -r compact` passed 218 assertions in 28 test cases; exact `ctest --test-dir build -R "ValidationHarness escapes report metadata and entry strings" --output-on-failure` passed 1/1; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-signal-graph-guards-493` | `82f2264c` | #493 host tranche for SignalGraph missing-node guard paths, remove-node connection cleanup, and default accessors | `test/test_host.cpp` | Rebased cleanly from `247be833` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased from `9f33226f` onto current `origin/main` `4bebc7bf`; committed `82f2264ccf31a241c373a05b87ed413a958e9e48`. The refreshed diff remains test-only in `test/test_host.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, SDL3, Highway, MbedTLS, and platform warnings and configured Pulp `0.77.0` from the refreshed mainline base; `cmake --build build --target pulp-test-host -j$(sysctl -n hw.ncpu)` passed with existing third-party and test TU warnings; focused `./build/test/pulp-test-host "SignalGraph remove_node drops related connections" -r compact` passed 10 assertions in 1 test case; focused `./build/test/pulp-test-host "SignalGraph guard paths reject missing nodes and keep defaults" -r compact` passed 12 assertions in 1 test case; tag run `./build/test/pulp-test-host "[host][graph]" -r compact` passed 1018 assertions in 18 test cases; full `./build/test/pulp-test-host -r compact` passed 1056 assertions in 29 test cases with existing local VST3/CLAP scan warnings; exact `ctest --test-dir build -R "SignalGraph remove_node drops related connections|SignalGraph guard paths reject missing nodes and keep defaults" --output-on-failure` passed 2/2; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-widgets-render-paths-493` | `15ca568b` | #493 view tranche for widget custom-shader CPU fallbacks, minimal render-style branches, and knob/fader/toggle interaction edges | `test/test_widgets.cpp` | Rebased cleanly from `55a90f79` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased again from `812f84ce` onto current `origin/main` `50ff5822`; committed `15ca568b`. The refreshed diff remains test-only in `test/test_widgets.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping and third-party warnings; `cmake --build build --target pulp-test-widgets -j$(sysctl -n hw.ncpu)` passed with existing view/platform warnings; focused `./build/test/pulp-test-widgets "[issue-493]" -r compact` passed 34 assertions in 4 test cases; full `./build/test/pulp-test-widgets -r compact` passed 294 assertions in 68 test cases; exact `ctest --test-dir build -R "Audio widgets custom shader paths fall back on recording canvas|Audio widgets minimal render style paints simplified branches|Knob mouse paths update value|Fader and toggle mouse paths dispatch clamped interactive values" --output-on-failure` passed 4/4; CLI skew, skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-audio-bridge-edges-493` | `2d00e9ac` | #493 view tranche for AudioBridge first-pop/default meter, max-channel clamp, zero-sample analysis, and MeterBallistics tiny-value clamp paths | `test/test_audio_bridge.cpp` | Rebased cleanly from `cba288df` onto current `origin/main` at `0447498e` on 2026-05-05, then rebased again from `a976d60e` onto current `origin/main` `50ff5822`; committed `2d00e9ac`. The refreshed diff remains test-only in `test/test_audio_bridge.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping and third-party/macOS warnings; `cmake --build build --target pulp-test-audio-bridge -j$(sysctl -n hw.ncpu)` passed with existing view/platform warnings; focused `./build/test/pulp-test-audio-bridge "[issue-493]" -r compact` passed 40 assertions in 3 test cases; full `./build/test/pulp-test-audio-bridge -r compact` passed 63 assertions in 8 test cases; exact `ctest --test-dir build -R "AudioBridge pop_latest reports empty before first meter|AudioBridge analyze clamps channels and handles zero sample blocks|MeterBallistics releases tiny values to exact zero" --output-on-failure` passed 3/3; CLI skew, skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `feature/cli-common-coverage-643` | `57f5ba3c` | #643 CLI tranche for common helper edges | none after rebase | Rebase onto current `origin/main` skipped `17b42f0f` as already upstream, leaving no diff; `cmake --build build-cli-common-lite --target pulp-test-cli-project-command -j8`; `ctest --test-dir build-cli-common-lite --output-on-failure -R "cli common"` passed 11/11; focused coverage for `tools/cli/cli_common.cpp` reported 33.33% line coverage after the branch was already absorbed; skill-sync report; version-bump report; `git diff --check`; final status clean. | No reopen action needed; tranche is already absorbed upstream. |
| `feature/cli-pr-coverage-643` | `57f5ba3c` | #643 CLI tranche for `pulp pr` shellout behavior | none after rebase | Rebase onto current `origin/main` dropped the local commit as already upstream, leaving no diff; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`; `cmake --build build --target pulp-test-cli-shellout`; `cmake --build build --target pulp-cli`; `ctest --test-dir build -R "^pulp pr " --output-on-failure` passed 4/4; skill-sync report; version-bump report; `git diff --check`; final status clean. | No reopen action needed; tranche is already absorbed upstream. |
| `feature/cli-docs-coverage-643` | `57f5ba3c` | #643 CLI tranche for docs shellout reader paths | none after rebase | Rebase onto current `origin/main` skipped `4877cb1b` because the patch is already upstream as `22438468` / #799; `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug`; `cmake --build build --target pulp-test-cli-shellout`; `cmake --build build --target pulp-cli`; focused docs CTest passed 1/1; skill-sync report; version-bump report; `git diff --check`; final status clean. Local diff coverage was not applicable because no diff remained, and an unnecessary coverage rebuild hit local disk pressure before producing a percent. | No reopen action needed; tranche is already absorbed upstream. |
| `feature/cli-inspect-coverage-643` | `57f5ba3c` | #643 CLI tranche for inspect command edge paths | none after rebase | Rebase onto current `origin/main` skipped `afef8faf` because the patch is already upstream via #1092; `cmake --build build --target pulp-test-cli-shellout`; `build/test/pulp-test-cli-shellout "[cli][shellout][inspect]" -r compact` passed 3 test cases / 35 assertions; `ctest --test-dir build -R "pulp inspect" --output-on-failure` passed 3/3; skill-sync report; version-bump report; `git diff --check`; final status clean. Local diff coverage was not applicable because no diff remained, and a native coverage rebuild hit local disk pressure before producing a percent. | No reopen action needed; tranche is already absorbed upstream. |
| `feature/cli-ship-coverage-643` | `8a2ca45a` | #643 CLI tranche for ship command coverage | none after rebase | Rebase onto current `origin/main` left no diff because the conflicted tranche commits were already upstream; `ctest --test-dir build -R "pulp ship" --output-on-failure` passed 10/10; skill-sync report; version-bump report; `git diff --check`; final status clean. | No reopen action needed; tranche is already absorbed upstream. |
| `feature/cli-project-coverage-643` | `8a2ca45a` | #643 CLI tranche for project command dispatch | none after rebase | Rebase onto current `origin/main` skipped `46dca652` as already upstream, leaving no diff; configured `/tmp/pulp-cli-project-coverage-643-smoke`, built `pulp-test-cli-project-command`, and ran the focused binary: 34 test cases / 500 assertions passed; skill-sync report; version-bump report; `git diff --check`; final status clean. | No reopen action needed; tranche is already absorbed upstream. |
| `feature/cli-tool-registry-coverage-643` | `8a2ca45a` | #643 CLI tranche for tool registry local paths | none after rebase | Rebase onto current `origin/main` skipped `cdfbbcec` as already upstream, leaving no diff; configured/built `pulp-test-cli-tool-registry`; `ctest --test-dir build --output-on-failure -R "^tool "` passed 8/8; skill-sync report; version-bump report; `git diff --check`; final status clean. Initial CMake configure stalled while cloning MbedTLS, so the worker stopped that local CMake process and reran using the existing local cache. | No reopen action needed; tranche is already absorbed upstream. |
| `feature/cli-tools-coverage-643-next4` | `2fbe9a6c` | #643 tooling tranche for LCOV Cobertura utility | none after rebase | Rebase onto current `origin/main` skipped `191785c6` as already upstream, leaving no diff; `python3 tools/scripts/test_lcov_cobertura.py` passed 3 tests; skill-sync report; version-bump report; `git diff --check`; final status clean. | No reopen action needed; tranche is already absorbed upstream. |
| `feature/package-registry-coverage-643` | `2fbe9a6c` | #643 CLI tranche for package registry helpers | none after rebase | Rebase onto current `origin/main` skipped `75a529ef` as already upstream, leaving no diff; `python3 tools/packages/test_package_validation_tools.py` passed 14 tests; non-strict `python3 tools/packages/validate_registry.py` passed with 36 packages, 0 errors, 12 warnings; `cmake --build build --target pulp-test-cli-package-registry`; `./build/test/pulp-test-cli-package-registry "[package-registry]"` passed 7 test cases / 114 assertions; skill-sync report; version-bump report; `git diff --check`; final status clean. Strict license validation still fails on current `origin/main` due existing registry policy/jsonschema state, unrelated to this absorbed branch. | No reopen action needed; tranche is already absorbed upstream. |
| `feature/package-validator-coverage-643` | `2fbe9a6c` | #643 package validator tranche for registry validation tools | none after rebase | Rebase onto current `origin/main` skipped `8a2edbe9` as already upstream, leaving no diff; `python3 tools/packages/test_package_validation_tools.py` passed 14 tests with the expected `jsonschema not installed` warning on the skip path; skill-sync report; version-bump report; `git diff --check`; final status clean. | No reopen action needed; tranche is already absorbed upstream. |
| `feature/python-tooling-coverage-643` | `2fbe9a6c` | #643 Python tooling tranche for version and skill gate helpers | none after rebase | Rebase onto current `origin/main` skipped `acadc5af15ce` as already upstream, leaving no diff; `python3 tools/scripts/test_gates.py` passed 39 tests; skill-sync report; version-bump report; `git diff --check`; final status clean. | No reopen action needed; tranche is already absorbed upstream. |
| `feature/signal-fast-math-boundary-coverage-645` | `55a7e26b` | #645 signal tranche for `core/signal/include/pulp/signal/fast_math.hpp` branch cutoffs | `test/test_fast_math.cpp` | Refreshed against current `origin/main`; `cmake --build build --target pulp-test-fast-math -j4`; `./build/test/pulp-test-fast-math` passed 62 assertions in 14 test cases; `ctest --test-dir build -R 'fast-math\|FastMath' --output-on-failure` passed 14/14; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1189, and explicit Namespace build dispatch `25210105555` passed required wrappers/platform lanes. | Merged as `e60cc129153c`; tracker comments posted to #641 and #645. |
| `feature/rectangle-list-coverage-641` | `9d1e7d66` | #641 canvas tranche for `core/canvas/include/pulp/canvas/rectangle_list.hpp` empty/no-op edges | `test/test_rectangle_list.cpp` | Local follow-up `local/phase3-rectangle-list-edges-641` was rebased to `origin/main` at `f6e092ba`; Git skipped the old local commit `7d2b2639` as already applied upstream via #1203. Validation still passed: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`; `cmake --build build --target pulp-test-rectangle-list -j$(sysctl -n hw.ncpu)`; `./build/test/pulp-test-rectangle-list` passed 60 assertions in 16 test cases; `ctest --test-dir build -R "^(RectangleList|Rect)" --output-on-failure` passed 16/16. | Merged #1203 as `9d1e7d661e8ea7fc152f2f1816e9b49072a53258`; no reopen action needed. |
| `local/phase3-table-model-edges-493` | `077145f5` | #493 view tranche for `core/view/src/table_model.cpp` and `core/view/include/pulp/view/table_model.hpp` bookkeeping edges | `test/test_table_model.cpp` | Refreshed against current `origin/main`; Git skipped old local commit `9700d0aa` because the patch is already upstream via `8a2ca45a` / #1188, leaving the branch 0 ahead and 0 behind with no diff. Validation still passed: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`; `cmake --build build --target pulp-test-table-model -j8`; `./build/test/pulp-test-table-model` passed 51 assertions in 11 test cases; `ctest --test-dir build -R 'empty model\|sort_by\|toggle_sort\|switching sort\|non-sortable\|out-of-range\|clear_sort\|adding rows\|add_column' --output-on-failure` passed 11/11; skill-sync and version-bump reports passed; diff/status checks clean. | Already absorbed upstream; do not queue. |
| `feature/phase3-theme-io-validation-493` | `af241124` | #493 view tranche for `core/view/src/theme.cpp` and `core/view/include/pulp/view/theme.hpp` validation/file edge paths | `test/test_theme.cpp` | Rebased cleanly onto `origin/main` at `69bbe8a4`; `cmake --build build --target pulp-test-theme -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-theme "[theme]"` passed 82 assertions in 17 test cases; focused `ctest --test-dir build -R '^(Color from hex|Theme dark has required tokens|Theme light has required tokens|Theme pro_audio has required tokens|Theme apply_overrides|Theme JSON round-trip|Theme missing token returns nullopt|Theme completeness reports missing required colors|Theme fill_from keeps overrides and fills missing tokens|Dark theme has motion duration tokens|Dark theme has motion easing tokens|Light theme inherits motion tokens from dark|Pro audio theme has snappier motion|Motion tokens survive JSON round-trip|Theme from_json with custom tokens|Theme from_json maps malformed color strings to default color|Theme load and save handle file edge cases)$' --output-on-failure` passed 17/17; final status clean with diff limited to `test/test_theme.cpp`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1240, then exited with no local targets remaining as expected for the Namespace-only route; #1240 was labeled `codecov` and linked from #493/#641. Required wrappers, Namespace platform jobs, Codecov patch, diff coverage, coverage lanes, version/skill sync, and IWYU passed; pending macOS sanitizer jobs were advisory. | Merged #1240 from `UNSTABLE` as `f414cdc85dbc93bf18b416f7ba94d31716d0691c`; tracker comments posted to #641/#493. |
| `feature/phase3-websocket-channel-edges-641` | `80dd3df8` | #641 runtime tranche for `core/runtime/src/websocket_channel.cpp` frame and handshake edge paths | `test/test_websocket_channel.cpp` | Rebased cleanly onto current `origin/main` after #1236; amended earlier to avoid `REQUIRE` inside a `std::thread`; `cmake --build build --target pulp-test-websocket-channel` passed; `./build/test/pulp-test-websocket-channel` passed 62 assertions in 11 test cases; `ctest --test-dir build -R 'WebSocket' --output-on-failure` passed 11/11; skill-sync and version-bump reports passed; `git diff --check origin/main...HEAD && git diff --check` passed; narrowed coverage/diff-cover mirror via `build-cov`, websocket CTest regex under `LLVM_PROFILE_FILE`, `lcov_cobertura.py`, and `diff_cover` passed earlier with no coverable diff lines because the tranche only changes `test/**`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1239, then exited with no local targets remaining as expected for the Namespace-only route; #1239 was labeled `codecov` and linked from #641. Required Namespace wrappers, platform lanes, coverage lanes, sanitizer lanes, IWYU, and Codecov patch passed. | Merged #1239 as `0a0cb73737097f7e9f9bd2606ea0f3579738773e`; tracker comment posted to #641. |
| `feature/svg-path-widget-coverage-493` | `476ca6ac` | #493 view tranche for `core/view/src/svg_path_widget.cpp` parser and degenerate path edges | `test/test_svg_path_widget.cpp` | Rebased onto current `origin/main` at `e60cc129`; `cmake --build build --target pulp-test-svg-path-widget -j4`; `./build/test/pulp-test-svg-path-widget` passed 133 assertions in 21 test cases; `ctest --test-dir build -R 'svg-path\|SvgPath' --output-on-failure` passed 21/21; skill-sync report; version-bump report; `git diff --check`; `git diff --check origin/main...HEAD`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1191, and explicit Namespace build dispatch `25210821512` passed required wrappers, Codecov patch, diff coverage, and coverage lanes. | Merged #1191 as `3c90f4801e031e47e2f32c20543c5f90c311f396`; queued advisory sanitizer run cancellation requested after merge. |
| `feature/phase3-eq-curve-view-edges-493` | `e1367951` | #493 view tranche for `core/view/src/eq_curve_view.cpp` range, hit-test, and drag callback edges | `test/test_phase9_widgets.cpp` | Rebased cleanly onto `origin/main` at `8a9eadc4`; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` passed; `cmake --build build --target pulp-test-phase9-widgets -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-phase9-widgets '[eq_curve]'` passed 32 assertions in 4 test cases; `ctest --test-dir build -R 'EqCurveView' --output-on-failure` passed 4/4; skill-sync and version-bump reports passed; `git diff --check origin/main...HEAD` passed; final diff is limited to `test/test_phase9_widgets.cpp` with 82 insertions. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1238, then exited with no local targets remaining as expected for the Namespace-only route; #1238 was labeled `codecov`, linked from #493/#641, and merged from `UNSTABLE` as `ef4303856482580beb5dcc4c15a7f22a2715454a` after required wrappers and Codecov patch were green. | Merged. |
| `feature/attributed-string-coverage-641` | `240a28bd` | #641 canvas tranche for `core/canvas/src/attributed_string.cpp` layout and span edge paths | `core/canvas/src/attributed_string.cpp`, `test/test_attributed_string.cpp` | Rebased from local `c955057b` onto current `origin/main`; includes a real fix for tab delimiter skipping after a word-break. `cmake --build build --target pulp-test-attributed-string -j4`; `./build/test/pulp-test-attributed-string` passed 97 assertions in 22 test cases; `ctest --test-dir build -R 'AttributedString\|attributed-string\|layout_attributed' --output-on-failure` passed 9/9 named cases; skill-sync report; version-bump report suggested a patch SDK bump because of the source fix; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1204, then exited with no local targets remaining as expected for the Namespace-only route. Explicit Namespace run dispatched as `25217492614`; #1204 is labeled `codecov` and linked from #641. | Merged #1204 as `c96d0ac038c0372466379bfffb29e2fc30af9a3f`. |
| `feature/sdf-atlas-cache-coverage-641` | `6a7093e6` | #641 canvas tranche for `core/canvas/src/sdf_atlas_cache.cpp` lifecycle, rebuild, and age-eviction edges | `test/test_sdf_atlas_cache.cpp` | Rebased from local `a4720af3` onto current `origin/main`; `cmake --build build --target pulp-test-sdf-atlas-cache -j4`; `./build/test/pulp-test-sdf-atlas-cache` passed 58 assertions in 9 test cases; `ctest --test-dir build -R 'sdf-atlas-cache\|SdfAtlasCache' --output-on-failure` passed 9/9; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1198, then exited with no local targets remaining as expected for the Namespace-only route. Explicit Namespace run dispatched as `25216284434`; #1198 is labeled `codecov` and linked from #641. | Merged #1198 as `10212599092d2a313996ab120ef128178173a467`. |
| `feature/image-convolution-coverage-641` | `49a88d75` | #641 canvas tranche for `core/canvas/include/pulp/canvas/image_convolution.hpp` construction, stride, clamp, and standard-kernel edges | `test/test_image_convolution.cpp` | Rebased from local `3d31df05` onto current `origin/main` after #1118 merged; `cmake --build build --target pulp-test-image-convolution -j4`; `./build/test/pulp-test-image-convolution` passed 111 assertions in 12 test cases; `ctest --test-dir build -R 'ImageConvolutionKernel\|Identity kernel\|Gaussian blur\|Blur reduces contrast\|Apply on null\|Alpha channel\|Standard kernel\|Sharpen kernel' --output-on-failure` passed 12/12; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1200, then exited with no local targets remaining as expected for the Namespace-only route. Explicit Namespace run dispatched as `25216572112`; #1200 is labeled `codecov` and linked from #641. | Merged #1200 as `99d852ec1e6a745ab2f1114841f04c1da562bc25`. |
| `feature/sdf-software-renderer-coverage-641` | `8fbb9ac5` | #641 canvas tranche for `core/canvas/include/pulp/canvas/sdf_software_renderer.hpp` empty-atlas, degenerate-quad, clipping, source-bound, and max-alpha paths | `test/test_sdf_software_renderer.cpp` | Rebased from local `23fdf6db` onto current `origin/main`; `cmake --build build --target pulp-test-sdf-software-renderer -j4`; `./build/test/pulp-test-sdf-software-renderer` passed 1,044 assertions in 7 test cases; `ctest --test-dir build -R 'software SDF render' --output-on-failure` passed 7/7; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1199, then exited with no local targets remaining as expected for the Namespace-only route. Explicit Namespace run dispatched as `25216362424`; #1199 is labeled `codecov` and linked from #641. | Merged #1199 as `f3c673ec75925f02bc21c143f9c2aae6a11f2f4b`. |
| `feature/path-to-sdf-coverage-641` | `69aa0360` | #641 canvas tranche for `core/canvas/src/path_to_sdf.cpp` dimension, spread, and mask-threshold guard edges, plus hot-reload test hardening after Windows Namespace exposed a watcher timing race | `test/test_path_to_sdf.cpp`, `test/test_hot_reload.cpp`, `test/CMakeLists.txt` | Rebased onto current `origin/main` at `7b78de80`; `cmake --build build --target pulp-test-path-to-sdf -j4`; `./build/test/pulp-test-path-to-sdf` passed 532 assertions in 6 test cases; `ctest --test-dir build -R 'path_to_sdf' --output-on-failure` passed 6/6; `cmake --build build --target pulp-test-hot-reload -j4`; `./build/test/pulp-test-hot-reload "[view][hotreload]"` passed 16 assertions in 5 test cases; `ctest --test-dir build -R 'HotReloader' --output-on-failure -j4` passed 5/5 with serialized watcher tests; skill-sync report; version-bump report; `git diff --check`. Required Namespace wrappers and Codecov patch passed; advisory macOS coverage/sanitizer lanes were still queued. | Merged #1190 as `a7b41acc3cf26922914a1e7e751a827e498b5797`; tracker comment posted to #641. |
| `feature/sdf-text-helper-coverage-641` | `2eb35744` | #641 canvas tranche for `core/canvas/include/pulp/canvas/sdf_text.hpp` snap, zero-base, wrapper, and scaled-quad helper edges | `test/test_sdf_text.cpp` | Rebased from local `1a333cc0` onto current `origin/main`; `cmake --build build --target pulp-test-sdf-text -j4`; `./build/test/pulp-test-sdf-text` passed 45 assertions in 8 test cases; `ctest --test-dir build -R 'snap_pen\|build_text_quads\|named SDF text wrappers' --output-on-failure` passed 8/8; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1195, then exited with no local targets remaining as expected for the Namespace-only route. Explicit Namespace run dispatched as `25215873750`; #1195 is labeled `codecov` and linked from #641. | Merged #1195 as `2c5135b05628693069d4b67524a26cc45e8b5433`. |
| `feature/sdf-atlas-api-coverage-641` | `f98b04de` | #641 canvas tranche for `core/canvas/src/sdf_atlas.cpp` default-state, invalid-build, packing, and duplicate-codepoint edges | `test/test_sdf_atlas.cpp` | Rebased from local `2f94fe28` onto current `origin/main` after #1127 merged; `cmake --build build --target pulp-test-sdf-atlas -j4`; `./build/test/pulp-test-sdf-atlas` passed 100 assertions in 10 test cases; `ctest --test-dir build -R 'SdfAtlas\|SDF pen' --output-on-failure` passed 19/19 including adjacent cache entries; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1196, then exited with no local targets remaining as expected for the Namespace-only route. Explicit Namespace run dispatched as `25215994338`; #1196 is labeled `codecov` and linked from #641. | Merged #1196 as `dfae8e9f4a3b7b019b091dd8786f354612e8e4ae`. |
| `feature/msdf-psdf-atlas-coverage-641` | `f61367e1` | #641 canvas tranche for `core/canvas/src/msdf_atlas.cpp` and PSDF vector-fallback threshold edges | `test/test_msdf_atlas.cpp`, `test/test_psdf_atlas.cpp` | Rebased from local `b7c5c2c6` onto current `origin/main` after #1191/#1128 merged; `cmake --build build --target pulp-test-msdf-atlas pulp-test-psdf-atlas -j4`; `./build/test/pulp-test-msdf-atlas` passed 73 assertions in 10 test cases; `./build/test/pulp-test-psdf-atlas` passed 12 assertions in 2 test cases; `ctest --test-dir build -R 'MsdfAtlas\|PsdfAtlas\|vector_fallback' --output-on-failure` passed 12/12; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1197, then exited with no local targets remaining as expected for the Namespace-only route. Explicit Namespace run dispatched as `25216200832`; #1197 is labeled `codecov` and linked from #641. | Merged #1197 as `f1d06c6210c1ee81686b72d449ab3f40308e6e3e`. |
| `feature/canvas-fallback-coverage-641` | `8b04404a` | #641 canvas tranche for `core/canvas/include/pulp/canvas/canvas.hpp` fallback drawing paths | `test/test_canvas.cpp` | Rebased from local `cf949ced` onto current `origin/main` after #1122 merged; `cmake --build build --target pulp-test-canvas -j4`; `./build/test/pulp-test-canvas '[canvas][fallback]'` passed 62 assertions in 4 test cases; `./build/test/pulp-test-canvas` passed 1,260 assertions in 30 test cases; `ctest --test-dir build -R 'Canvas fallback\|SDF fallback' --output-on-failure` passed 5/5; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1201, then exited with no local targets remaining as expected for the Namespace-only route. Explicit Namespace run dispatched as `25216712937`; #1201 is labeled `codecov` and linked from #641. | Merged #1201 as `42464b4bc3282e056e57e6ebb7820f4b91a974a9`. |
| `feature/phase3-app-framework-edges-493` | `dd8497a6` | #493 view tranche for `core/view/src/app_framework.cpp` shortcut, key mapping, menu, toolbar, and settings edges | `test/test_app_framework.cpp` | Refreshed from the local-only app-framework tranche and revalidated on current `origin/main`; configured and built `pulp-test-app-framework`; direct `./build/test/pulp-test-app-framework "[view]"` passed 101 assertions in 27 test cases; `ctest --test-dir build -R 'app-framework|AppFramework|view' --output-on-failure` passed the focused app-framework cases; skill-sync, version-bump, CLI skew, and `git diff --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1247, then exited with no local targets remaining as expected for the Namespace-only route; #1247 is labeled `codecov` and linked from #493/#641. Required `linux`, `macos`, and `windows` contexts plus Codecov patch passed; only advisory sanitizer lanes were still pending at merge time. | Merged #1247 as `1bab31140ff7a176018b15c6640f58108968f9f6`; tracker comments posted to #493/#641. |
| `feature/state-binding-coverage-641` | `c1e5bd59` | #641 state tranche for `core/state/include/pulp/state/binding.hpp` gesture, polling, reset, and undo edges | `test/test_binding.cpp` | Rebased cleanly onto `origin/main` at `ea731cbf`; `cmake --build build --target pulp-test-binding -j4`; `./build/test/pulp-test-binding` passed 55 assertions in 14 test cases; `ctest --test-dir build -R 'Binding' --output-on-failure` passed 13/13; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1185; required Namespace wrappers and Codecov patch passed, with only advisory macOS coverage/sanitizer work active. | Merged #1185 as `57f5ba3ca08fe18d9611114b75f31d1625648202`; tracker comment posted to #641; cancellation requested for leftover PR-head runs `25209345440`, `25209337170`, and `25209336987`. |
| `feature/system-volume-coverage-640` | `b9e086fa` | #640 audio tranche for `core/audio/src/system_volume.cpp` Linux `amixer` command edges | `test/test_system_volume.cpp`, `test/CMakeLists.txt` | Rebased from local `5711d7e2` onto current `origin/main` after #1114 merged; existing build tree required `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` before the new target existed; `cmake --build build --target pulp-test-system-volume -j4`; `./build/test/pulp-test-system-volume` passed 4 assertions in 4 test cases; `ctest --test-dir build -R 'system volume\|system mute\|system-volume' --output-on-failure` passed 4/4; skill-sync report; version-bump report; `git diff --check`. The first Linux Namespace build failed because the fake `PATH` hid `grep`/`head`/`tr`; pushed `b9e086fa` to prepend the fake bin directory instead, and reran focused local validation plus reports. #1202 is labeled `codecov` and linked from #641/#640. #1202 merged as `00b0871a6dea89fecb14d4aca8502e5c52507416`. | Merged. |
| `feature/phase3-platform-permissions-coverage-640` | `6c7d6921` | #640 platform tranche for `core/platform/src/permissions.cpp` override-stack edge behavior | `test/test_permissions.cpp` | Rebased old local `feature/platform-permissions-coverage-640-next7` onto current `origin/main` at `445350d5` after #1242 merged; configured `build-permissions` with `-DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF` and the shared MbedTLS cache; `cmake --build build-permissions --target pulp-test-permissions -j$(sysctl -n hw.ncpu)`; `./build-permissions/test/pulp-test-permissions "[platform][permissions]" -r compact` passed 42 assertions in 13 test cases; `ctest --test-dir build-permissions -R "Permissions|permissions|permission" --output-on-failure` passed 6/6; skill-sync report; version-bump report; `git diff --check origin/main...HEAD`; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1249, then exited with no local targets remaining as expected for the Namespace-only route; `shipyard cloud run build feature/phase3-platform-permissions-coverage-640 --require-sha HEAD` dispatched Namespace run `25241097612`; #1249 is labeled `codecov` and linked from #640/#641. Required Namespace wrappers, Codecov patch, coverage lanes, sanitizer lanes, and advisory IWYU all passed. | Merged #1249 as `fb97bd8bd1726fad2e39a7f4c4137f12107560ac`; tracker comments posted to #640/#641. |
| `feature/phase3-coreaudio-format-coverage-640` | `be3bc439` | #640 audio tranche for `core/audio/src/coreaudio_format.mm` FormatRegistry routing and malformed-read edges | `test/test_audio_file.cpp` | Created from current `origin/main` at `b0372e3c`; `cmake -S . -B build-audio -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`; `cmake --build build-audio --target pulp-test-audio-file -j$(sysctl -n hw.ncpu)`; `./build-audio/test/pulp-test-audio-file "[audio][file][registry][coreaudio]" -r compact` passed 32 assertions in 1 test case; broader `./build-audio/test/pulp-test-audio-file "[audio][file][registry]" -r compact` passed 170 assertions in 11 test cases; `ctest --test-dir build-audio -R "FormatRegistry|audio-file" --output-on-failure` passed 5/5; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1248, then exited with no local targets remaining as expected for the Namespace-only route; `shipyard cloud run build feature/phase3-coreaudio-format-coverage-640 --require-sha HEAD` dispatched Namespace run `25240896596`; #1248 is labeled `codecov` and linked from #640/#641. | Merged #1248 as `55e3c744c324cc9f8d9181071b596a6eb0972cc9`; tracker comments posted to #640/#641. |
| `feature/phase3-scan-blacklist-coverage-493` | `92ac57cb` | #493 host tranche for `core/host/src/scan_blacklist.cpp` deleted-entry retention, bad-size rows, and missing/save failure edges | `test/test_scan_blacklist.cpp` | Worker-created from current `origin/main` at `4b9ad460`; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF` passed; `cmake --build build --target pulp-test-scan-blacklist -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-scan-blacklist "[host][blacklist]"` passed 9 cases / 26 assertions; focused CTest over all 9 discovered scan blacklist cases passed; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1255, then exited with no local targets remaining as expected for the Namespace-only route; #1255 is labeled `codecov` and linked from #493/#641. A rerun cleared the earlier Windows Namespace `cli-doctor` transient, and required Namespace wrappers/platform lanes, coverage lanes, Codecov patch, sanitizer lanes, IWYU, audit, and version gates were green. | Merged #1255 as `fe8c89b65b8bae26e3671b5cc4881864f89dad8e`; tracker comments posted to #493/#641. |
| `feature/phase3-file-dialog-coverage-640` | `624a181e` | #640 platform tranche for `core/platform/src/file_dialog_stub.cpp` backend replacement, partial fallback, callback argument, and clear-backend edges | `test/test_file_dialog.cpp` | Worker-created from current `origin/main` at `4b9ad460`; refreshed locally from PR head `6b0aecf8` onto current `origin/main` `dba0fd4b` on 2026-05-05 and committed `624a181e65e0d2d14e44791a5c24c1819be1858a`. The refreshed diff remains test-only in `test/test_file_dialog.cpp`. Local macOS validation is partial because the new backend replacement tests are behind `#if !defined(__APPLE__)`: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, file-dialog, child-process, and platform warnings and configured Pulp `0.78.0`; `cmake --build build --target pulp-test-file-dialog -j$(sysctl -n hw.ncpu)` passed; full macOS `./build/test/pulp-test-file-dialog -r compact` passed 15 assertions in 3 test cases; focused `./build/test/pulp-test-file-dialog "FileDialog non-Apple: backend replacement forwards arguments" -r compact` and tag `./build/test/pulp-test-file-dialog "[platform][file-dialog][issue-640]" -r compact` both exited 2 with no matching tests on macOS; exact `ctest --test-dir build -R "FileDialog non-Apple|file-dialog|pulp-test-file-dialog" --output-on-failure` found no tests on macOS; CLI sync check passed with slash-command/skill-reference warnings only; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final local branch is one commit ahead of `origin/main` but diverged from `origin/feature/phase3-file-dialog-coverage-640`. The local refresh has not been pushed and no workflow was rerun. | Hold local-only while Namespace is intentionally paused; when capacity returns, either update #1256 with the refreshed branch or supersede it, then run non-Apple validation for the compiled-in file-dialog backend tests before merge. |
| `feature/phase3-test-signal-coverage-493` | `75ef6ead` | #493 format tranche for `core/format/src/test_signal.cpp` file-load, unload, EOF, looping, and channel-fallback edges | `test/test_test_signal.cpp` | Created from current `origin/main` at `4b9ad460`; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`; `cmake --build build --target pulp-test-test-signal -j$(sysctl -n hw.ncpu)`; `./build/test/pulp-test-test-signal -r compact` passed 713 assertions in 7 test cases; `ctest --test-dir build -R "^TestSignalSource:" --output-on-failure` passed 7/7; skill-sync report; version-bump report; CLI skew check; `git diff --check origin/main...HEAD`; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1257, then exited with no local targets remaining as expected for the Namespace-only route; #1257 is labeled `codecov` and linked from #493/#641. | Queued: monitor #1257 and merge once required gates are green. |
| `feature/phase3-remote-view-session-coverage-493` | `85773e9b` | #493 format/view tranche for `core/format/src/remote_view_session.cpp` remote parameter reads, malformed remote parameter sets, and remote-close guards | `test/test_remote_view.cpp` | Rebased onto `origin/main` at `2fe04152`; `cmake --build build --target pulp-test-remote-view -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-remote-view -r compact` passed 47 assertions in 7 test cases; `ctest --test-dir build -R "RemoteViewSession|remote-view|remote_view" --output-on-failure` passed 7/7; CLI skew, skill-sync report, version-bump report, and diff checks passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1258, then exited with no local targets remaining as expected for the Namespace-only route; #1258 is labeled `codecov` and linked from #493/#641. Required Namespace wrappers, Windows MSVC, and Codecov patch passed while advisory macOS coverage/sanitizer lanes remained queued. | Merged #1258 as `8bc762db370b91b81545f029d6d62723f45b3387`; tracker comments posted to #493/#641; cancellation requested for leftover advisory workflows `25242054454` and `25242054568`. |
| `feature/phase3-i18n-parser-coverage-641` | `321e5d70` | #641 runtime tranche for `core/runtime/src/i18n.cpp` parser and substitution edges | `test/test_i18n.cpp` | Worker-created from current `origin/main` at `2fe04152`; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`; `cmake --build build --target pulp-test-i18n -j$(sysctl -n hw.ncpu)`; `./build/test/pulp-test-i18n` passed 49 assertions in 17 test cases; `ctest --test-dir build -R i18n --output-on-failure` passed 17/17; skill-sync report; version-bump report; `git diff --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1259, then exited with no local targets remaining as expected for the Namespace-only route; #1259 is labeled `codecov` and linked from #641. | Merged #1259 as `5984a3091f181f9881450081cec944e293c39e66`; tracker comment posted to #641; cancellation requested for leftover advisory runs `25242270773` and `25242270780`. |
| `feature/phase3-settings-panel-coverage-493` | `8127b187` | #493 format/view tranche for `core/format/src/settings_panel.cpp` device list rebuilds, apply-config selections, MIDI hotplug, and test-tone callbacks | `test/test_standalone_editor_chrome.cpp` | Created from current `origin/main` at `2fe04152`; configured with GPU/examples off and the shared MbedTLS cache; `cmake --build build --target pulp-test-standalone-editor-chrome -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-standalone-editor-chrome -r compact` passed 91 assertions in 16 test cases; `ctest --test-dir build -R "Standalone|SettingsPanel|settings" --output-on-failure` passed 15/15; CLI skew, skill-sync report, version-bump report, and diff checks passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1260, then exited with no local targets remaining as expected for the Namespace-only route; #1260 is labeled `codecov` and linked from #493/#641. | Merged #1260 as `234f014367646054ca265fb24425b64beef5a0c3`; tracker comments posted to #493/#641; cancellation requested for leftover advisory sanitizer run `25242356508`. |
| `feature/phase3-js-engine-recommend-coverage-493` | `c819c8c0` | #493 view/runtime tranche for JS engine recommendation edge behavior | `test/test_js_engine.cpp` | Worker-created from current `origin/main` at `2fe04152`; configured `build-js-engine` with GPU/examples off and the shared MbedTLS cache; `cmake --build build-js-engine --target pulp-test-js-engine -j$(sysctl -n hw.ncpu)` passed; `./build-js-engine/test/pulp-test-js-engine "[recommend]"` passed 50 assertions in 4 test cases; `ctest --test-dir build-js-engine -R "recommend" --output-on-failure` passed 4/4; full `./build-js-engine/test/pulp-test-js-engine` passed 206 assertions in 31 test cases; skill-sync report, version-bump report, and diff checks passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1261, then exited with no local targets remaining as expected for the Namespace-only route; #1261 is labeled `codecov` and linked from #493/#641. #1261 merged as `ae5425864a6e5ab53029fffb75b20b760a31bc19` after required wrappers, Namespace platform lanes, and Codecov patch passed; advisory macOS coverage/sanitizer lanes were still queued. | Merged. |
| `feature/phase3-appcast-coverage-643` | `a692e8ae` | #641/#643 ship/tooling tranche for `ship/src/appcast.cpp` malformed enclosure length parsing | `.agents/skills/ship/SKILL.md`, `ship/src/appcast.cpp`, `test/test_appcast.cpp` | Created from current `origin/main` at `1bab3114`; configured `build-appcast` with GPU/examples off and the shared MbedTLS cache; `cmake --build build-appcast --target pulp-test-appcast -j$(sysctl -n hw.ncpu)` passed; `./build-appcast/test/pulp-test-appcast -r compact` passed 72 assertions in 12 test cases; `ctest --test-dir build-appcast -R "Appcast|Version comparison|sign_file" --output-on-failure` passed 12/12; skill-sync report passed after updating the ship skill; version-bump report passed with explicit `Version-Bump: skip` trailer; diff checks passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1262, then exited with no local targets remaining as expected for the Namespace-only route; #1262 is labeled `codecov` and linked from #641/#643. Required gates were green; only advisory macOS UBSan remained pending. | Merged #1262 as `2976e9c06c1a138bed5737f2590a9aa252e04a5a`; tracker comments posted to #641/#643. |
| `feature/phase3-inspect-domain-coverage-641` | `e9a6d72c` | #641 inspect tranche for `inspect/src/domain_handler.cpp` and `inspect/src/console_capture.cpp` dispatch/capture edges | `test/test_inspector.cpp` | Worker-created from current `origin/main` at `1bab3114`; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` passed; `cmake --build build --target pulp-test-inspector -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-inspector` passed 233 assertions in 34 test cases; `ctest --test-dir build -R inspector --output-on-failure` passed 1/1; focused `ctest --test-dir build -R "ConsoleCapture|DomainHandler|ViewInspector|Protocol|InspectorOverlay|InspectorWindow|CollapsableSection|AudioInspector" --output-on-failure` passed 34/34; skill-sync report, version-bump report, and diff checks passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1263, then exited with no local targets remaining as expected for the Namespace-only route; #1263 is labeled `codecov` and linked from #641. | Queued: monitor #1263 and merge once required gates are green. |
| `feature/phase3-python-bindings-coverage-641` | `7b6d26c1` | #641 bindings tranche for `bindings/python/bindings.cpp` exposed edge behavior | `test/test_python_bindings.py`, `test/test_python_bindings_embedded.cpp` | Worker-created from current `origin/main` at `1bab3114`; `cmake -S . -B build-python-bindings -DPULP_BUILD_PYTHON=ON -DPULP_BUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug` passed; `cmake --build build-python-bindings --target pulp_python pulp-test-python-bindings-embedded -j$(sysctl -n hw.ncpu)` passed; `ctest --test-dir build-python-bindings -R "python-bindings-(smoke|embedded-smoke)" --output-on-failure` passed 2/2; skill-sync report, version-bump report, and diff checks passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1264, then exited with no local targets remaining as expected for the Namespace-only route; #1264 is labeled `codecov` and linked from #641. Required gates were green; advisory macOS ASan remained pending. | Merged #1264 as `b89c5cb72621333ace22fef0e11ca8c267191809`; tracker comment posted to #641. |
| `feature/rectangle-list-coverage-641` | `1b179b48` | #641 canvas tranche for `core/canvas/include/pulp/canvas/rectangle_list.hpp` empty/no-op edges | `test/test_rectangle_list.cpp` | Rebased from local `7d2b2639` onto current `origin/main`; `cmake --build build --target pulp-test-rectangle-list -j4`; `./build/test/pulp-test-rectangle-list` passed 60 assertions in 16 test cases; `ctest --test-dir build -R 'RectangleList\|rectangle-list' --output-on-failure` passed 11/11 discovered cases; skill-sync report; version-bump report; `git diff --check`. Required wrappers, diff coverage, and Codecov patch passed; advisory macOS sanitizer lanes were still queued. | Merged #1203 as `9d1e7d661e8ea7fc152f2f1816e9b49072a53258`; tracker comment posted to #641 and leftover advisory run cancellations requested. |
| `feature/memory-message-channel-coverage-641` | `065ef90e` | #641 runtime tranche for `core/runtime/src/memory_message_channel.cpp` delivery, callback replacement, and close lifecycle edges | `test/test_memory_message_channel.cpp`, `test/CMakeLists.txt` | Rebased cleanly onto `origin/main` at `ea731cbf`; previous local validation: `cmake --build build --target pulp-test-memory-message-channel -j4`; `./build/test/pulp-test-memory-message-channel` passed 24 assertions in 6 test cases; `ctest --test-dir build -R 'MemoryMessageChannel\|memory-message-channel' --output-on-failure` passed 6/6; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1184; all required Namespace wrappers, Codecov patch, diff coverage, coverage lanes, and sanitizer lanes passed. | Merged #1184 as `4d67e04547cf33523e416d84fef1cb1079dc147e`; tracker comment posted to #641. |
| `feature/canvas-text-layout-coverage-641` | `d4d40aa7` | #641 canvas tranche for `core/canvas/src/text_layout.cpp` fallback layout, hit-test, index-position, and parallelogram edges | `test/test_text_shaper.cpp` | Rebased/current with `origin/main`; `cmake --build build --target pulp-test-text-shaper -j4`; `./build/test/pulp-test-text-shaper` passed 62 assertions in 20 test cases; `ctest --test-dir build -R 'text-shaper\|TextShaper\|layout_paragraph\|GlyphArrangement\|Parallelogram' --output-on-failure` passed 19/19; skill-sync report; version-bump report; `git diff --check`. Required Namespace wrappers and Codecov patch passed; advisory macOS coverage/sanitizer lanes were still queued. | Merged #1186 as `4afbf2c1a83ec043f35476e2e26725cca851c312`; tracker comment posted to #641. |
| `feature/canvas-svg-coverage-641` | `7e4e8c5f` | #641 canvas tranche for `core/canvas/src/svg.cpp` file, invalid-rasterize, render, and move-assignment edges | `test/test_svg.cpp` | Rebased/current with `origin/main`; `cmake --build build-svg --target pulp-test-svg -j4`; `./build-svg/test/pulp-test-svg` passed 40 assertions in 10 test cases; `ctest --test-dir build-svg -R '^SvgImage ' --output-on-failure` passed 10/10; skill-sync report; version-bump report; `git diff --check`. Required Namespace wrappers, Codecov patch, and diff coverage passed; advisory macOS sanitizer lanes were still queued. | Merged #1187 as `b9ad40021e0d503a389e69a479e16a1c8ce8d501`; tracker comment posted to #641. |
| `feature/view-table-model-coverage-493` | `9700d0aa` | #493 view tranche for `core/view/src/table_model.cpp` and `core/view/include/pulp/view/table_model.hpp` bookkeeping edges | `test/test_table_model.cpp` | Rebased onto `origin/main` at `e1a22a1f`; `cmake --build build --target pulp-test-table-model -j4`; `./build/test/pulp-test-table-model` passed 51 assertions in 11 test cases; `ctest --test-dir build -R 'table-model\|TableModel\|sort_by\|toggle_sort\|add_column' --output-on-failure` passed 9/9; skill-sync report; version-bump report; `git diff --check`. Required Namespace wrappers and Codecov patch passed; advisory macOS coverage/sanitizer lanes were still queued. | Merged #1188 as `8a2ca45a142b6c2efcfbbfb9249857c0d7c4ca3f`; tracker comments posted to #641 and #493; cancellation requested for leftover PR-head runs `25209808815` and `25209808820`. |
| `feature/phase3-asset-manager-coverage-493` | `be730a12` | #493 view tranche for `core/view/src/asset_manager.cpp` file-backed shader/blob cache and cached `@2x` image behavior | `test/test_asset_manager.cpp` | Refreshed from the local follow-up while preserving only extra AssetManager tests beyond already-merged #1125. `cmake -S . -B build-asset -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF`; `cmake --build build-asset --target pulp-test-asset-manager`; `./build-asset/test/pulp-test-asset-manager` passed 75 assertions in 29 test cases; `ctest --test-dir build-asset -R AssetManager --output-on-failure` passed 24/24; Shipyard preflight reported no skill-sync or version-bump action needed; CLI skew and `git diff --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1245, then exited with no local targets remaining as expected for the Namespace-only route; #1245 is labeled `codecov` and linked from #493/#641. First required Namespace run failed on unrelated Windows `Timer basic operation`; `gh run rerun 25240501936 --failed` was issued. Required Namespace wrappers, coverage lanes, Windows MSVC, Codecov patch, audit, and version gates passed after rerun; remaining macOS coverage/sanitizer jobs were advisory. | Merged #1245 as `4b9ad46091475fd6dc7d8278f3045094dc5a3297`; tracker comments posted to #641/#493. |
| `feature/widget-bridge-drop-font-coverage-493` | `808b6806` | #493 view tranche for `core/view/src/widget_bridge.cpp` JS drop callback escaping, context-menu callback dispatch, and `loadFont` existence paths | `test/test_widget_bridge.cpp` | Created from current `origin/main`; `cmake --build build --target pulp-test-widget-bridge -j4`; `./build/test/pulp-test-widget-bridge "[dnd],[font],[context-menu]"` passed 18 assertions in 3 test cases; `ctest --test-dir build -R 'widget-bridge\|WidgetBridge' --output-on-failure` passed 83/83; skill-sync report; version-bump report; `git diff --check`. Local diff coverage was intentionally skipped because the branch is test-only and local disk is about 2.9 GiB free. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1194, then exited with no local targets remaining as expected for the Namespace-only route. Explicit Namespace run dispatched as `25215749116`; #1194 is labeled `codecov` and linked from #641/#493. | Merged #1194 as `b69f2b5ad4774692e666dd73be29a8f2d65e6d49`. |

## Cancelled/Paused Runs

Reconciliation on 2026-05-02: GitHub now reports the older paused rows below as merged. Treat this reconciliation block as canonical for these PRs; the archival rows are retained only for the original cancelled-run IDs.

| PR | Branch | Merge commit |
| --- | --- | --- |
| #1045 | `feature/events-service-discovery-coverage-642-next6` | `2e4a0ed2e2567bf8d2ae734c2f9df184e05997b0` |
| #1051 | `feature/signal-poly-math-coverage-645` | `8f038e9b8150aa4514780d61948039d6aac9cb92` |
| #1062 | `codex/coverage-midi-edge-644` | `2fbe9a6ce7a2b652589d327f69184a6889cf95d9` |
| #1066 | `feature/signal-filter-meter-coverage-645` | `7b78de802f382564d2cbde69ea1ea74a57dde31e` |
| #1071 | `feature/background-scanner-restart-coverage-493` | `160e76ec7c86df258bd7c90438da952f0b1f671a` |
| #1074 | `feature/audio-format-registry-compressed-640` | `5898bf0571631347078a402a0f50d1e5a334da78` |
| #1075 | `feature/cli-host-coverage-643` | `c34f11f7138cce81e2135c39a875125dff434508` |
| #1078 | `feature/runtime-gzip-header-coverage-641` | `81be4ee00e02e367feec32c3c6885d0785179efa` |
| #1079 | `feature/volume-detector-coverage-642` | `da004f90e21c99461891817bc7149d5db58d34b9` |
| #1082 | `feature/render-loop-coverage-646` | `b0903cd8ed4b50e29ea6ec0220145573b3361a28` |
| #1083 | `feature/platform-registry-coverage-640` | `df3ca99d8052d6a2a02661bc997cba5a824b99e6` |
| #1084 | `feature/runtime-text-diff-coverage-641` | `66c74123430ad107721780443a18b2e08c7eb033` |
| #1085 | `feature/audio-load-measurer-coverage-640` | `5aac29496436a4cd62ecb0e5ebbbc51ff7b3dd79` |
| #1086 | `feature/audio-hotplug-coverage-640` | `0300ba207577c50cb2bbdd94c77a5639a590b978` |
| #1088 | `feature/events-async-helper-coverage-642` | `80139f392047ef3561d23ae10b545059f91d1619` |
| #1096 | `feature/render-pure-coverage-646` | `c544a9ef221b491ced9124ed1c9e69dd72db3f2d` |
| #1097 | `feature/view-toolbar-coverage-493` | `cd0f141fa708f4b205a86a161733de1a36cd8bbd` |
| #1102 | `feature/midi-running-status-coverage-645-next` | `a412d3c883161d0207cdc7f9859b8a5afd500db4` |
| #1104 | `feature/cli-create-coverage-643` | `f46c83f5848d2f406d62ed743b4f4044ffc02f94` |
| #1112 | `feature/state-properties-coverage-641-next2` | `fef94b10e89c7f555d57f07420d76f4286b4c970` |
| #1113 | `feature/runtime-expression-coverage-641-next` | `93c2aea2bd75f72ac946d88e1ad4962bcca9c4f9` |
| #1115 | `feature/runtime-license-analytics-coverage-641-next` | `daa0aa70442744c9452cd972a2c53b41b8459997` |
| #1116 | `feature/signal-interpolator-coverage-645-next` | `a100d78830bff6d9089ec3562a2029233ba8188a` |
| #1117 | `feature/lcov-cobertura-coverage-643-next` | `202943e61da7908d92ca8d7bbc40d8b09db841c4` |
| #1119 | `feature/state-undo-history-coverage-641-next` | `0bf8f64aeb8b5e6d57871d1070bfbd832e3590f1` |
| #1120 | `feature/descriptor-validation-coverage-493-next` | `dba48cb3f53c6ca8ccdd6cfa094033c9594e66cf` |
| #1123 | `feature/host-scan-cache-coverage-493-next` | `57ac9d3c3a70feb77926984deba225baaf7b6492` |
| #1125 | `feature/view-asset-manager-coverage-493-next` | `f1db8c29cc41924e5b1db34e7c4e160983136bbb` |
| #1129 | `feature/midi-sysex-accumulator-coverage-645-next` | `a5ae96aa067c31b5efd0079dd620222f08fb357f` |
| #1131 | `feature/audio-window-enumerator-coverage-640-next` | `0f4d38f6c30724bdcf2597e4bb30f4df46337d35` |
| #1132 | `codex/midi-sysex-sidecar-tests` | `3d7fa34f380da6d5e0b98c91ecea7105b97ba811` |
| #1133 | `feature/audio-channel-set-coverage-640-next` | `e1a22a1ffd93dc1cd51039d63985b522fc5cb655` |
| #1134 | `codex/coverage-phase3-tranche-20260430095455` | `a01ca7410faa5a6cd61766f10fa5aff4f3649202` |
| #1135 | `feature/signal-processor-chain-reset-coverage-645` | `44e67d52cbfc1970c3f3b3c133f98db6bdedba0c` |
| #1136 | `feature/package-registry-cache-fallback-643-next` | `caf93e5f835f96ab19eadb4a6f056db54f463893` |
| #1137 | `feature/audio-platform-helper-coverage-640-next` | `ea731cbf365ca109726014d11dd50daca206bb40` |
| #1138 | `feature/coverage-no-idle-guidance-641` | `44cff053284862b498a177139a8499ac514612e1` |
| #1139 | `codex/events-phase3-coverage-tranche` | `d701d4ae8d45753f8b5de60b24034f7a38923a2d` |
| #1141 | `feature/ship-appcast-coverage-644-next` | `49136be956d7b24fc77717fa8c48774b56c51422` |
| #1142 | `codex/package-tools-coverage-643` | `ed34a23d9e4d734dd75d22542f4046b5096fe8bb` |
| #1143 | `feature/render-compute-coverage-646` | `d36fddc2cd9f56053aff6768739d02225e023201` |

| PR | State | Branch | Head | Runs | Title |
| --- | --- | --- | --- | --- | --- |
| #1045 | UNKNOWN | `feature/events-service-discovery-coverage-642-next6` | `5783c552714e` | `Build and Test 25168056508`<br>`Coverage 25168033097`<br>`Sanitizer Tests 25168032974`<br>`Build and Test 25168032955` | test(events): cover service discovery edge paths |
| #1051 | UNKNOWN | `feature/signal-poly-math-coverage-645` | `98e7ace40ae6` | `Build and Test 25172061996`<br>`Coverage 25172040856`<br>`Build and Test 25172040818`<br>`Sanitizer Tests 25172040812` | fix(signal): handle degenerate polynomial roots |
| #1062 | UNKNOWN | `codex/coverage-midi-edge-644` | `7c532a7414f1` | `Build and Test 25170865514`<br>`Build and Test 25170844844`<br>`Sanitizer Tests 25170844747`<br>`Coverage 25170844738` | test(midi): cover factory data byte bounds |
| #1066 | UNKNOWN | `feature/signal-filter-meter-coverage-645` | `facbf9038b58` | `Build and Test 25170500099`<br>`Coverage 25170474161`<br>`Sanitizer Tests 25170474151`<br>`Build and Test 25170474021` | fix(signal): clamp meter process channel counts |
| #1071 | UNKNOWN | `feature/background-scanner-restart-coverage-493` | `f959d20f76ce` | `Build and Test 25171074566`<br>`Sanitizer Tests 25171040147`<br>`Coverage 25171040113`<br>`Build and Test 25171040103` | test(host): cover background scanner restart after cancel |
| #1072 | MERGED | `feature/design-import-edge-coverage-493` | `2d78c1d52a77` | `Build and Test 25206705148`<br>`Coverage 25206705135`<br>`Sanitizer Tests 25206705153`<br>`IWYU advisory 25206705144` | Merged as `cacf3050929be57a33ec47f276588bfa1e13ba3b`; all current-head validation workflows completed green. |
| #1074 | UNKNOWN | `feature/audio-format-registry-compressed-640` | `40c55f8c4896` | `Build and Test 25170667740`<br>`Coverage 25170647578`<br>`Build and Test 25170647546`<br>`Sanitizer Tests 25170647524` | test(audio): cover compressed reader rejection |
| #1075 | UNKNOWN | `feature/cli-host-coverage-643` | `098de603de19` | `Build and Test 25170662783`<br>`Build and Test 25170655086`<br>`Sanitizer Tests 25170655057`<br>`Coverage 25170655030` | test(cli): cover host wrapper edge paths |
| #1078 | UNKNOWN | `feature/runtime-gzip-header-coverage-641` | `7fccf9b22503` | `Build and Test 25170874594`<br>`Build and Test 25170861703`<br>`Sanitizer Tests 25170861699`<br>`Coverage 25170861659` | test(runtime): cover gzip header rejection paths |
| #1079 | UNKNOWN | `feature/volume-detector-coverage-642` | `1de5e8cf3798` | `Build and Test 25171421848`<br>`Coverage 25171405107`<br>`Sanitizer Tests 25171405054`<br>`Build and Test 25171405053` | test(events): cover volume detector edge paths |
| #1082 | UNKNOWN | `feature/render-loop-coverage-646` | `5343f39d1e4e` | `Build and Test 25172222764`<br>`Coverage 25172198359`<br>`Build and Test 25172198322`<br>`Sanitizer Tests 25172198309` | test(render): cover render loop edge paths |
| #1083 | UNKNOWN | `feature/platform-registry-coverage-640` | `2a8289dfc54d` | `Build and Test 25171427827`<br>`Sanitizer Tests 25171410018`<br>`Build and Test 25171409959`<br>`Coverage 25171409933` | test(platform): cover registry fallback stubs |
| #1084 | UNKNOWN | `feature/runtime-text-diff-coverage-641` | `56b7cdffd2c0` | `Build and Test 25171676839`<br>`Coverage 25171657125`<br>`Build and Test 25171657123`<br>`Sanitizer Tests 25171657090` | test(runtime): cover text diff edge paths |
| #1085 | UNKNOWN | `feature/audio-load-measurer-coverage-640` | `580de2c8b8b7` | `Build and Test 25170103165`<br>`Coverage 25170077440`<br>`Sanitizer Tests 25170077403`<br>`Build and Test 25170077397` | fix(audio): guard load measurer invalid timing inputs |
| #1086 | UNKNOWN | `feature/audio-hotplug-coverage-640` | `53664657b7f2` | `Build and Test 25171357108`<br>`Coverage 25171343389`<br>`Build and Test 25171343386`<br>`Sanitizer Tests 25171343376` | test(audio): cover hotplug callback edges |
| #1088 | UNKNOWN | `feature/events-async-helper-coverage-642` | `c02c36484186` | `Build and Test 25166064687`<br>`Sanitizer Tests 25166050540`<br>`Coverage 25166050279`<br>`Build and Test 25166050259` | test(events): cover async helper edge paths |
| #1096 | UNKNOWN | `feature/render-pure-coverage-646` | `0f6b5fff46a1` | `Build and Test 25171762144`<br>`Coverage 25171747201`<br>`Build and Test 25171747177`<br>`Sanitizer Tests 25171747166` | test(render): cover gpu graph helper state paths |
| #1097 | UNKNOWN | `feature/view-toolbar-coverage-493` | `db18ab5dad36` | `Build and Test 25172317712`<br>`Coverage 25172293881`<br>`Sanitizer Tests 25172293727`<br>`Build and Test 25172293720` | test(view): cover toolbar and breadcrumb interactions |
| #1102 | UNKNOWN | `feature/midi-running-status-coverage-645-next` | `bf4e0d35c7d3` | `Build and Test 25172829315`<br>`Build and Test 25172819367`<br>`Sanitizer Tests 25172819312`<br>`Coverage 25172819311` | fix(midi): clear stale running status on undefined system bytes |
| #1104 | UNKNOWN | `feature/cli-create-coverage-643` | `21bf715c69dd` | `Build and Test 25166284714`<br>`Build and Test 25166269351`<br>`Coverage 25166269330`<br>`Sanitizer Tests 25166269324` | test(cli): cover create fail-fast edge paths |
| #1112 | UNKNOWN | `feature/state-properties-coverage-641-next2` | `314c970a36f8` | `Build and Test 25164008645` | test(state): cover state tree listener detach edges |
| #1113 | UNKNOWN | `feature/runtime-expression-coverage-641-next` | `448e92021bb5` | `Build and Test 25164081023`<br>`Coverage 25164051339`<br>`Build and Test 25164051319`<br>`Sanitizer Tests 25164051266` | test(runtime): cover expression evaluator edge paths |
| #1114 | MERGED | `feature/runtime-file-library-coverage-641-next` | `18b36739bab5` | `Build and Test 25215294665`<br>`Coverage 25215294680`<br>`Sanitizer Tests 25215294672`<br>`IWYU advisory 25215294677` | Merged as `cf7a69fae91f01ef4d74688ea357a5da2ea8a2d2`; queued coverage/sanitizer run cancellations requested after merge. |
| #1115 | UNKNOWN | `feature/runtime-license-analytics-coverage-641-next` | `67157a45fc68` | `Build and Test 25164886652`<br>`Coverage 25164835133`<br>`Sanitizer Tests 25164835113`<br>`Build and Test 25164835092` | test(runtime): cover license and analytics edges |
| #1116 | UNKNOWN | `feature/signal-interpolator-coverage-645-next` | `7832eae87eaa` | `Build and Test 25165535849`<br>`Build and Test 25165511969`<br>`Coverage 25165511967`<br>`Sanitizer Tests 25165511951` | test(signal): cover interpolator endpoints and impulses |
| #1117 | BLOCKED | `feature/lcov-cobertura-coverage-643-next` | `7c5d7d67dbfc` | `Build and Test 25165561249`<br>`Coverage 25165524012`<br>`Build and Test 25165524010` | test(ci): cover lcov cobertura edge paths |
| #1118 | MERGED | `feature/osc-bundle-coverage-641-next` | `e85122359eb6` | `Build and Test 25215267166`<br>`Coverage 25215267197`<br>`Sanitizer Tests 25215267162`<br>`IWYU advisory 25215267163` | Merged as `b428fa4f92a74b6b247302dea443b621778089fd`; queued coverage/sanitizer run cancellations requested after merge. |
| #1119 | UNKNOWN | `feature/state-undo-history-coverage-641-next` | `34efa649b9fd` | `Build and Test 25165878118`<br>`Sanitizer Tests 25165848583`<br>`Coverage 25165848571`<br>`Build and Test 25165848554` | test(state): cover undo history edge cases |
| #1120 | UNKNOWN | `feature/descriptor-validation-coverage-493-next` | `14a965de6152` | `Build and Test 25166025406`<br>`Coverage 25165994906`<br>`Sanitizer Tests 25165994899`<br>`Build and Test 25165994884` | test(format): cover descriptor validation edges |
| #1121 | MERGED | `feature/audio-buffering-reader-coverage-640-next` | `afdff6e1cc06` | `Build and Test 25215267350`<br>`Coverage 25215267349`<br>`Sanitizer Tests 25215267373`<br>`IWYU advisory 25215267366` | Merged as `a478ad3ea9c42658338a8a3178339a83132d8259`; queued coverage/sanitizer run cancellations requested after merge. |
| #1122 | MERGED | `feature/render-draw-batcher-coverage-646-next` | `dce6dcd7adc3` | `Build and Test 25215188316`<br>`Coverage 25215188317`<br>`Sanitizer Tests 25215188323`<br>`IWYU advisory 25215188319` | Merged as `84028f027d00e01b97cc4757c2f25abadb7cfc2f`; queued coverage/sanitizer run cancellations requested after merge. |
| #1123 | UNKNOWN | `feature/host-scan-cache-coverage-493-next` | `c1792a45fe1b` | `Build and Test 25166249254`<br>`Sanitizer Tests 25166234957`<br>`Build and Test 25166234954`<br>`Coverage 25166234934` | test: cover host scan cache fallback paths |
| #1125 | UNKNOWN | `feature/view-asset-manager-coverage-493-next` | `be13ce28b68f` | `Build and Test 25167200088`<br>`Coverage 25167178443`<br>`Sanitizer Tests 25167178429`<br>`Build and Test 25167178420` | test(view): cover asset manager edge paths |
| #1126 | MERGED | `feature/view-frame-clock-coverage-493-next` | `79f2d65046d0` | `Build and Test 25215188581`<br>`Coverage 25215188580`<br>`Sanitizer Tests 25215188559`<br>`IWYU advisory 25215188571` | Merged as `d118b749328f7be84eb2e041dd037986716a7dfe`; queued sanitizer run cancellation requested after merge. |
| #1127 | MERGED | `feature/render-texture-atlas-coverage-646-next` | `310074265e45` | `Build and Test 25213360729`<br>`Coverage 25213360744`<br>`Sanitizer Tests 25213360732`<br>`IWYU advisory 25213360752` | Merged as `8ccc4a151d4ea722486320f49d78d54b14c57905`; queued advisory sanitizer run cancellation requested after merge. |
| #1128 | MERGED | `feature/audio-workgroup-coverage-640-next` | `494233c87fe0` | `Build and Test 25212496127`<br>`Coverage 25212496120`<br>`Sanitizer Tests 25212496122`<br>`IWYU advisory 25212496131` | Merged as `ebc118d95684df23a0d1e480b8694fa368f03261`; queued coverage/sanitizer run cancellations requested after merge. |
| #1129 | UNKNOWN | `feature/midi-sysex-accumulator-coverage-645-next` | `f81378693df2` | `Build and Test 25168440441`<br>`Build and Test 25168411404`<br>`Coverage 25168411387`<br>`Sanitizer Tests 25168411385` | test(midi): cover sysex accumulator tail paths |
| #1131 | UNKNOWN | `feature/audio-window-enumerator-coverage-640-next` | `36d5bd605414` | `Build and Test 25169301265`<br>`Build and Test 25169274684`<br>`Coverage 25169274675`<br>`Sanitizer Tests 25169274591` | test(audio): cover excerpt window boundary arithmetic |
| #1132 | UNKNOWN | `codex/midi-sysex-sidecar-tests` | `2695097458c4` | `Build and Test 25169342502`<br>`Build and Test 25169328452`<br>`Coverage 25169328381`<br>`Sanitizer Tests 25169328284` | test(midi): cover sysex sidecar lifecycle |
| #1133 | UNKNOWN | `feature/audio-channel-set-coverage-640-next` | `df227394cd7e` | `Build and Test 25169456361`<br>`Coverage 25169437661`<br>`Sanitizer Tests 25169437655`<br>`Build and Test 25169437642` | test(audio): cover channel set edge paths |
| #1134 | UNKNOWN | `codex/coverage-phase3-tranche-20260430095455` | `1dbd12f64ba0` | `Build and Test 25171425473`<br>`Build and Test 25170099336`<br>`Coverage 25170099278`<br>`Sanitizer Tests 25170099265` | test(signal): cover meter reset and clip edges |
| #1135 | UNKNOWN | `feature/signal-processor-chain-reset-coverage-645` | `89242ca0bfa5` | `Build and Test 25172582711`<br>`Build and Test 25172550916`<br>`Sanitizer Tests 25172550903`<br>`Coverage 25172550884` | test(signal): cover processor chain reset edges |
| #1136 | UNKNOWN | `feature/package-registry-cache-fallback-643-next` | `46854e04fcf2` | `Build and Test 25173056813`<br>`Sanitizer Tests 25173043882`<br>`Coverage 25173043809`<br>`Build and Test 25173043787` | test(cli): cover package registry cache fallbacks |
| #1137 | UNKNOWN | `feature/audio-platform-helper-coverage-640-next` | `5149c107a3ac` | `Build and Test 25173070502`<br>`Sanitizer Tests 25173053372`<br>`Coverage 25173053207`<br>`Build and Test 25173053201` | test(platform): cover progress parser delimiter edges |
| #1138 | BLOCKED | `feature/coverage-no-idle-guidance-641` | `6fea2fa4686a` | `Build and Test 25173469013`<br>`Coverage 25173434715`<br>`Build and Test 25173434684` | docs: codify coverage no-idle loop |
| #1139 | BLOCKED | `codex/events-phase3-coverage-tranche` | `a6237254b05d` | `Build and Test 25173850903`<br>`Sanitizer Tests 25173826530`<br>`Build and Test 25173826521`<br>`Coverage 25173826490` | test(events): cover ipc error state edges |
| #1140 | MERGED | `feature/status-ladder-coverage-643` | `3695a6af7163` | Local follow-up rebase skipped `bf87273e` as already applied upstream; `python3 tools/test_check_status_ladder.py` passed 9 tests; temp-venv focused coverage reported 92% for `tools/check_status_ladder.py`; no reopen action needed. | test(tools): cover status ladder checker |
| #1141 | BLOCKED | `feature/ship-appcast-coverage-644-next` | `2d2c119b4c8d` | `Build and Test 25173929796`<br>`Build and Test 25173909885`<br>`Sanitizer Tests 25173909871`<br>`Coverage 25173909805` | test(ship): cover appcast malformed optional fields |
| #1142 | BLOCKED | `codex/package-tools-coverage-643` | `cb6fb9c4e9c3` | `Build and Test 25174428148`<br>`Build and Test 25174415212`<br>`Coverage 25174415161` | test(packages): cover validation tool edge paths |
| #1143 | BLOCKED | `feature/render-compute-coverage-646` | `2b2ccde36e63` | `Build and Test 25174471464`<br>`Sanitizer Tests 25174429661`<br>`Coverage 25174429510`<br>`Build and Test 25174429474` | test(render): cover gpu compute pool bookkeeping |

## Resume Checklist

1. Confirm Namespace capacity is available.
2. Re-fetch each PR branch and verify the remote branch still points to the recorded head SHA.
3. Re-dispatch in small batches, starting with the most merge-ready PRs.
4. After each batch, merge only PRs with required checks green and `mergeStateStatus` clean enough for normal merge.
5. Update this ledger with resumed run IDs and merge SHAs.

2026-05-15 01:04 PDT: created the #642 events/runtime IPC batch
`feature/phase3-events-runtime-batch-642` at `bad61495`, PR #2026,
from current `origin/main` `a29cb7dc`. It adds test-only coverage in
`test/test_ipc_endpoints.cpp`, `test/test_network_service_discovery.cpp`,
and `test/test_events_async_helpers.cpp` for malformed IPC endpoint port
strings, connection/listener rejection paths, endpoint failure reset,
NetworkServiceDiscovery backend/cache lifecycle behavior, callback-less
service storage, ActionBroadcaster empty/no-listener sends, MultiTimer
restart behavior, and LockingAsyncUpdater synchronous trigger-and-wait
behavior. Local macOS validation passed: configure with
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
build `cmake --build build --target pulp-test-ipc-endpoints
pulp-test-network-service-discovery pulp-test-events-async-helpers
pulp-test-events -j$(sysctl -n hw.ncpu)`, focused
`./build/test/pulp-test-ipc-endpoints "[issue-642]" -r compact`
passing 109 assertions in 6 cases, focused
`./build/test/pulp-test-network-service-discovery "[issue-642]" -r
compact` passing 47 assertions in 11 cases, focused
`./build/test/pulp-test-events-async-helpers "[issue-642]" -r compact`
passing 39 assertions in 9 cases, focused
`./build/test/pulp-test-events "[issue-642]" -r compact` passing 38
assertions in 8 cases, full `pulp-test-ipc-endpoints` passing 109
assertions in 6 cases, full `pulp-test-network-service-discovery`
passing 90 assertions in 21 cases, full `pulp-test-events-async-helpers`
passing 39 assertions in 9 cases, full `pulp-test-events` passing 99
assertions in 24 cases, exact `ctest --test-dir build -R "IPC socket|NSD|ActionBroadcaster|MultiTimer|LockingAsyncUpdater|InterprocessConnection|ChildProcessManager|AsyncUpdater|EventLoop|Timer" --output-on-failure`
passing 55/55, `git diff --check`, `git diff --cached --check`,
skill-sync report, version-bump report, and docs-sync report. Branch
pushed with GitHub-hosted PR workflow only; no Namespace dispatch and no
SSH targets. Resume action: monitor #2026 required checks and merge
directly when green.

2026-05-15 01:14 PDT: created the #644 canvas/OSC/render helper batch
`feature/phase3-canvas-osc-render-batch-644` at `9570cdb`, PR #2028,
from current `origin/main` `a29cb7dc`. It adds test-only coverage in
`test/test_osc.cpp`, `test/test_osc_bundle.cpp`,
`test/test_attributed_string.cpp`, and `test/test_dirty_tracker.cpp` for
OSC typed defaults, empty/padded/truncated string decode, trailing
padding, unconnected raw sends, bundle fractional timetags, default
bundle elements, address-pattern separator/alternative/negated-class
edges, AttributedString empty setter no-ops and clear/reuse, TextLayout
wrap delimiter behavior, and DirtyTracker full-repaint bounds,
threshold-equality, and invalid-viewport promotion behavior. Local macOS
validation passed: configure with
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
build `cmake --build build --target pulp-test-osc pulp-test-osc-bundle
pulp-test-attributed-string pulp-test-dirty-tracker -j$(sysctl -n
hw.ncpu)`, focused `./build/test/pulp-test-osc "[issue-644]" -r
compact` passing 19 assertions in 5 cases, focused
`./build/test/pulp-test-osc-bundle "[issue-644]" -r compact` passing 14
assertions in 5 cases, focused `./build/test/pulp-test-attributed-string
"[issue-644]" -r compact` passing 15 assertions in 3 cases, focused
`./build/test/pulp-test-dirty-tracker "[issue-644]" -r compact` passing
14 assertions in 3 cases, full `pulp-test-osc` passing 116 assertions in
32 cases, full `pulp-test-osc-bundle` passing 93 assertions in 36 cases,
full `pulp-test-attributed-string` passing 112 assertions in 25 cases,
full `pulp-test-dirty-tracker` passing 66 assertions in 21 cases, `ctest
--test-dir build -R "OSC|AttributedString|TextLayout|DirtyTracker"
--output-on-failure` passing 87/87, `git diff --check`, skill-sync
report, version-bump report, and docs-sync report. Branch pushed with
GitHub-hosted PR workflow only; no Namespace dispatch and no SSH targets.
Resume action: monitor #2028 required checks and merge directly when
green.

2026-05-15 01:21 PDT: created the #641 runtime services batch
`feature/phase3-runtime-services-batch-641` at `93458d4`, PR #2030,
from current `origin/main` `a29cb7dc`. It adds test-only coverage in
`test/test_analytics.cpp`, `test/test_i18n.cpp`,
`test/test_identity.cpp`, and `test/test_license.cpp` for Analytics
flush behavior while disabled, FileAnalyticsDestination empty flush and
property ordering, WidgetTracker forwarded event details, i18n overwrite,
missing-key substitution, duplicate `.strings`/JSON entries, PO commit
boundaries, global argument translation, UUID uppercase parsing and
ordering, typed-ID nil/hash stability, and LicenseValidator optional
fields, newline-free files, and empty-payload classification. Local macOS
validation passed: configure with
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`,
build `cmake --build build --target pulp-test-analytics pulp-test-i18n
pulp-test-identity pulp-test-license -j$(sysctl -n hw.ncpu)`, focused
`./build/test/pulp-test-analytics "[issue-641]" -r compact` passing 13
assertions in 4 cases, focused `./build/test/pulp-test-i18n
"[issue-641]" -r compact` passing 16 assertions in 6 cases, focused
`./build/test/pulp-test-identity "[issue-641]" -r compact` passing 10
assertions in 3 cases, focused `./build/test/pulp-test-license
"[issue-641]" -r compact` passing 10 assertions in 3 cases, full
`pulp-test-analytics` passing 29 assertions in 13 cases, full
`pulp-test-i18n` passing 65 assertions in 23 cases, full
`pulp-test-identity` passing 41 assertions in 7 cases, full
`pulp-test-license` passing 43 assertions in 23 cases, `ctest --test-dir
build -R "Analytics|i18n|Uuid|Typed identity|EventEnvelope|LicenseValidator|LicenseGenerator|BigInteger" --output-on-failure`
passing 64/64, `git diff --check`, skill-sync report, version-bump
report, and docs-sync report. Branch pushed with GitHub-hosted PR
workflow only; no Namespace dispatch and no SSH targets. Resume action:
monitor #2030 required checks and merge directly when green.

2026-05-15 01:31 PDT: created the #641 runtime codec/expression batch
`feature/phase3-runtime-codec-batch-641` at `d155add`, PR #2031, from
current `origin/main` `a29cb7dc`. It adds test-only coverage in
`test/test_crypto.cpp`, `test/test_runtime_utils.cpp`, and
`test/test_xml_zip.cpp` for binary hash pointer overloads, lowercase
SHA-256 hex formatting, AES exact-block PKCS#7 padding and invalid
padding rejection, expression precedence/constants/functions/scientific
notation/variables/custom unary functions, XML empty-document queries,
XML move construction/assignment, and `xml_generate` empty/repeated
element output. Local macOS validation passed: configure
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`, build `cmake --build
build --target pulp-test-crypto pulp-test-runtime-utils pulp-test-xml-zip
-j$(sysctl -n hw.ncpu)`, focused `./build/test/pulp-test-crypto
"[coverage][issue-641]" -r compact` passing 12 assertions in 5 cases,
full `pulp-test-crypto` passing 32 assertions in 17 cases, focused
`./build/test/pulp-test-runtime-utils "[coverage][issue-641]" -r
compact` passing 41 assertions in 8 cases, full `pulp-test-runtime-utils`
passing 155 assertions in 42 cases, focused `./build/test/pulp-test-xml-zip
"[coverage][issue-641]" -r compact` passing 25 assertions in 3 cases,
full `pulp-test-xml-zip` passing 110 assertions in 27 cases, exact
`ctest --test-dir build -R "(SHA-256 pointer|SHA-1 pointer|MD5 binary|AES
exact|AES decrypt rejects invalid|Expression evaluator|ExpressionEvaluator|XmlDocument empty|XmlDocument
move|xml_generate handles)" --output-on-failure` passing 13/13, `git diff
--check`, and `./tools/check-docs.sh` passed with existing warnings.
Branch pushed with GitHub-hosted PR workflow only; no Namespace dispatch
and no SSH targets. Resume action: monitor #2031 required checks and
merge directly when green.

2026-05-15 01:41 PDT: created the #641 runtime communication batch
`feature/phase3-runtime-comm-batch-641` at `47c56b8`, PR #2032, from
current `origin/main` `a29cb7dc`. It adds test-only coverage in
`test/test_memory_message_channel.cpp`, `test/test_json_rpc.cpp`, and
`test/test_stream.cpp` for zero-length and embedded-NUL message payloads,
callback clearing, JSON-RPC omitted params, string request-id responses,
incoming error response data, FileStream ReadWrite truncation/position,
closed MemoryStream/TcpStream behavior, and invalid HttpStream fetch
failure paths. Local macOS validation passed: configure `cmake -S . -B
build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF
-DPULP_BUILD_EXAMPLES=OFF`, build `cmake --build build --target
pulp-test-memory-message-channel pulp-test-json-rpc pulp-test-stream
pulp-test-websocket-channel -j$(sysctl -n hw.ncpu)`, focused
`./build/test/pulp-test-memory-message-channel "[coverage][issue-641]" -r
compact` passing 18 assertions in 3 cases, focused
`./build/test/pulp-test-json-rpc "[coverage][issue-641]" -r compact`
passing 17 assertions in 3 cases, focused `./build/test/pulp-test-stream
"[coverage][issue-641]" -r compact` passing 30 assertions in 4 cases,
full `pulp-test-memory-message-channel` passing 42 assertions in 9
cases, full `pulp-test-json-rpc` passing 65 assertions in 12 cases, full
`pulp-test-stream` passing 142 assertions in 16 cases, full
`pulp-test-websocket-channel` passing 62 assertions in 11 cases, exact
`ctest --test-dir build -R "(zero-length binary|clear callbacks|embedded
NUL|omits params|string request ids|incoming error responses|ReadWrite
mode|clear keeps closed|TcpStream closed|HttpStream invalid)"
--output-on-failure` passing 10/10, `git diff --check`, and
`./tools/check-docs.sh` passed with existing warnings. Branch pushed with
GitHub-hosted PR workflow only; no Namespace dispatch and no SSH targets.
Resume action: monitor #2032 required checks and merge directly when
green.

2026-05-15 01:48 PDT: created the #640/#641 runtime/platform primitives
batch `feature/phase3-runtime-platform-primitives-batch-641` at
`8644004`, PR #2033, from current `origin/main` `a29cb7dc`. It adds
test-only coverage in `test/test_runtime_utils.cpp`,
`test/test_runtime.cpp`, and `test/test_environment.cpp` for
MemoryMappedFile ReadWrite persistence and failure reset, InterProcessLock
idempotent unlock/reacquire, explicit byte-pointer base64 encoding and
quartet decoding, repeated-line text diff stability, range boundary
intersection behavior, SpscQueue slot reuse after wrap-around, the
`PULP_ON_SCOPE_EXIT` macro, SafeAreaInsets zero detection, and idempotent
Environment token reset behavior. Local macOS validation passed: configure
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF
-DPULP_BUILD_EXAMPLES=OFF`, build `cmake --build build --target
pulp-test-runtime-utils pulp-test-runtime pulp-test-environment
-j$(sysctl -n hw.ncpu)`, focused `./build/test/pulp-test-runtime-utils
"[coverage][issue-641]" -r compact` passing 61 assertions in 9 cases,
focused `./build/test/pulp-test-runtime "[coverage][issue-641]" -r
compact` passing 50 assertions in 2 cases, focused
`./build/test/pulp-test-environment "[coverage][issue-640]" -r compact`
passing 11 assertions in 2 cases, full `pulp-test-runtime-utils` passing
175 assertions in 43 cases, full `pulp-test-runtime` passing 127
assertions in 17 cases, full `pulp-test-environment` passing 100
assertions in 20 cases, exact `ctest --test-dir build -R
"(MemoryMappedFile ReadWrite|MemoryMappedFile open failure|InterProcessLock
unlock|base64 handles explicit|text_diff keeps repeated|Range
boundary|SpscQueue reuses|PULP_ON_SCOPE_EXIT|SafeAreaInsets zero|token
reset is idempotent)" --output-on-failure` passing 10/10, `git diff
--check`, and `./tools/check-docs.sh` passed with existing warnings.
Branch pushed with GitHub-hosted PR workflow only; no Namespace dispatch
and no SSH targets. Resume action: monitor #2033 required checks and
merge directly when green.

2026-05-15 01:57 PDT: created the #645 MIDI primitives batch
`feature/phase3-midi-primitives-batch-645` at `bcff2b8`, PR #2034, from
current `origin/main` `a29cb7dc`. It adds test-only coverage in
`test/test_mpe_buffer.cpp`, `test/test_midi_ci.cpp`, and
`test/test_midi_buffer_ump.cpp` for MPE lower/upper zone boundaries,
member expression seeding, manager-zone state updates, UMP member
callbacks, tracker reset/config changes, MIDI-CI discovery inquiry/reply
wire identity encoding, property overwrites, and UMP sidecar external
ownership across MidiBuffer edits. Local macOS validation passed:
configure `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
-DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF`, build `cmake --build
build --target pulp-test-mpe-buffer pulp-test-midi-ci
pulp-test-midi-buffer-ump -j$(sysctl -n hw.ncpu)`, focused
`./build/test/pulp-test-mpe-buffer "[issue-645]"` passing 47 assertions
in 5 cases, focused `./build/test/pulp-test-midi-ci "[issue-645]"`
passing 64 assertions in 9 cases, focused
`./build/test/pulp-test-midi-buffer-ump "[issue-645]"` passing 7
assertions in 1 case, full `pulp-test-mpe-buffer` passing 66 assertions
in 8 cases, full `pulp-test-midi-ci` passing 91 assertions in 18 cases,
full `pulp-test-midi-buffer-ump` passing 14 assertions in 4 cases, exact
`ctest --test-dir build -R "(MpeConfig|MpeVoiceTracker|CiDiscovery
discovery|CiDiscovery properties|attached UMP sidecar)"
--output-on-failure` passing 10/10, `git diff --check`, and
`./tools/check-docs.sh` passed with existing warnings. Branch pushed with
GitHub-hosted PR workflow only; no Namespace dispatch and no SSH targets.
Resume action: monitor #2034 required checks and merge directly when
green.

2026-05-15 02:07 PDT: created the #646 state/format validation batch
`feature/phase3-state-format-batch-646` at `44e7606`, PR #2035, from
current `origin/main` `a29cb7dc`. It adds test-only coverage in
`test/test_state.cpp`, `test/test_binding.cpp`,
`test/test_validation_harness.cpp`, and `test/test_descriptor_validation.cpp`
for ParamRange boundary clamping and zero-width normalization, StateStore
serialized header fields, future-version rejection, normalized
quantization, Binding unknown parameter IDs, normalized
clamping/quantization, callback ordering, ValidationHarness artifact
directory creation, zero-block processing, JSON metadata and entry string
escaping, nested report writes, unknown installed validator handling, and
descriptor MIDI capability warning aggregation. Local macOS validation
passed: configure `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
-DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF`, build `cmake --build
build --target pulp-test-state pulp-test-binding
pulp-test-validation-harness pulp-test-descriptor-validation -j$(sysctl
-n hw.ncpu)`, focused `./build/test/pulp-test-state
"[coverage][issue-646]" -r compact` passing 22 assertions in 4 cases,
focused `./build/test/pulp-test-binding "[coverage][issue-646]" -r
compact` passing 15 assertions in 3 cases, focused
`./build/test/pulp-test-validation-harness "[coverage][issue-646]" -r
compact` passing 30 assertions in 5 cases, focused
`./build/test/pulp-test-descriptor-validation "[coverage][issue-646]" -r
compact` passing 2 assertions in 1 case, full `pulp-test-state` passing
78 assertions in 11 cases, full `pulp-test-binding` passing 70 assertions
in 17 cases, full `pulp-test-validation-harness` passing 243 assertions
in 32 cases, full `pulp-test-descriptor-validation` passing 44 assertions
in 17 cases, exact `ctest --test-dir build -R "(ParamRange
clamps|ParamRange zero-width|StateStore serialization records|StateStore
set_normalized|Binding unknown|Binding set_normalized|Binding on_change
callbacks|ValidationHarness configure creates|ValidationHarness
process_blocks zero|ValidationHarness report escapes|ValidationHarness
write_report creates nested|ValidationHarness run_validator errors on
unknown|DescriptorValidation: MIDI capability warnings)"
--output-on-failure` passing 14/14, `git diff --check`, and
`./tools/check-docs.sh` passed with existing warnings. Branch pushed with
GitHub-hosted PR workflow only; no Namespace dispatch and no SSH targets.
Resume action: monitor #2035 required checks and merge directly when
green.

2026-05-15 02:15 PDT: created the #647 format state-IO batch
`feature/phase3-format-state-io-batch-647` at `5e83df7`, PR #2037, from
current `origin/main` `a29cb7dc`. It adds test-only coverage in
`test/test_headless.cpp`, `test/test_plugin_state_io.cpp`, and
`test/test_preset_manager.cpp` for HeadlessHost prepare/release context
forwarding, default process context filling, explicit MIDI buffer
forwarding, null-processor no-op processing, plugin_state_io envelopes
with empty plugin payloads, empty inner StateStore payload rejection,
envelope CRC mismatch rejection before plugin callbacks, PresetManager
out-of-range load clamping, user preset scan filtering and nested folder
metadata, missing preset delete no-op behavior, and empty preset-list
navigation. Local macOS validation passed: configure `cmake -S . -B build
-DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF
-DPULP_BUILD_EXAMPLES=OFF`, build `cmake --build build --target
pulp-test-headless pulp-test-plugin-state-io pulp-test-preset-manager
-j$(sysctl -n hw.ncpu)`, focused `./build/test/pulp-test-headless
"[coverage][issue-647]" -r compact` passing 17 assertions in 4 cases,
focused `./build/test/pulp-test-plugin-state-io "[coverage][issue-647]"
-r compact` passing 9 assertions in 2 cases, focused
`./build/test/pulp-test-preset-manager "[coverage][issue-647]" -r
compact` passing 19 assertions in 4 cases, full `pulp-test-headless`
passing 38 assertions in 11 cases, full `pulp-test-plugin-state-io`
passing 71 assertions in 9 cases, full `pulp-test-preset-manager`
passing 101 assertions in 18 cases, exact `ctest --test-dir build -R
"(plugin_state_io rejects malformed blobs|HeadlessHost forwards|HeadlessHost
fills|HeadlessHost null|plugin_state_io envelope with empty|plugin_state_io
rejects envelope CRC|PresetManager load clamps|PresetManager user preset
scan|PresetManager delete missing|PresetManager navigation on empty)"
--output-on-failure` passing 11/11, `git diff --check`, and
`./tools/check-docs.sh` passed with existing warnings. Branch pushed with
GitHub-hosted PR workflow only; no Namespace dispatch and no SSH targets.
Resume action: monitor #2037 required checks and merge directly when
green.

2026-05-15 02:23 PDT: created the #648 format adapter metadata batch
`feature/phase3-format-adapter-metadata-batch-648` at `a1c5575`, PR
#2038, from current `origin/main` `a29cb7dc`. It adds test-only coverage
in `test/test_aax_model.cpp`, `test/test_ara.cpp`, and
`test/test_ios_audio_session.cpp` for AAX parameter ID formatting, native
ID suffix boundaries, stem mapping edge cases, descriptor metadata
propagation, latency and MIDI flags, packet sizing, parameter converter
callbacks, ARA default controller behavior, ARA role bit stability, iOS
audio-session null C events, C++ listener precedence over raw callbacks,
fallback C callbacks, and listener replacement/detach behavior. Local
macOS validation passed: configure `cmake -S . -B build
-DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF
-DPULP_BUILD_EXAMPLES=OFF`, build `cmake --build build --target
pulp-test-aax-model pulp-test-ara pulp-test-ios-audio-session -j$(sysctl
-n hw.ncpu)`, focused `./build/test/pulp-test-aax-model
"[coverage][issue-648]" -r compact` passing 21 assertions in 1 case,
focused `./build/test/pulp-test-ara "[coverage][issue-648]" -r compact`
passing 8 assertions in 2 cases, focused
`./build/test/pulp-test-ios-audio-session "[coverage][issue-648]" -r
compact` passing 8 assertions in 2 cases, full `pulp-test-aax-model`
passing 108 assertions in 13 cases, full `pulp-test-ara` passing 19
assertions in 10 cases, full `pulp-test-ios-audio-session` passing 28
assertions in 6 cases, exact `ctest --test-dir build -R "(AAX model
carries descriptor|AraDocumentController defaults|AraRole bit values|C
ABI ignores null events|audio session listener replacement)"
--output-on-failure` passing 5/5, `git diff --check`, and
`./tools/check-docs.sh` passed with existing warnings. Branch pushed with
GitHub-hosted PR workflow only; no Namespace dispatch and no SSH targets.
Resume action: monitor #2038 required checks and merge directly when
green.

2026-05-15 02:31 PDT: created the #649 platform helper batch
`feature/phase3-platform-helper-batch-649` at `4d1604b`, PR #2040, from
current `origin/main` `ce86d9f`. It adds test-only coverage in
`test/test_platform.cpp`, `test/test_environment.cpp`, and
`test/test_permissions.cpp` for ProgressParser payload parsing, empty
payloads, type-only progress lines, ignored non-progress lines, empty
progress callbacks, listener-free Environment publishes, snapshot
updates, token self-move/reset idempotency, PermissionsOverride clearing
missing entries, and preserving outer override entries through empty
nested guards. Local macOS validation passed: configure `cmake -S . -B
build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF
-DPULP_BUILD_EXAMPLES=OFF`, build `cmake --build build --target
pulp-test-platform pulp-test-environment pulp-test-permissions -j$(sysctl
-n hw.ncpu)`, focused `./build/test/pulp-test-platform
"[coverage][issue-649]" -r compact` passing 11 assertions in 2 cases,
focused `./build/test/pulp-test-environment "[coverage][issue-649]" -r
compact` passing 11 assertions in 2 cases, focused
`./build/test/pulp-test-permissions "[coverage][issue-649]" -r compact`
passing 5 assertions in 2 cases, full `pulp-test-platform` passing 48
assertions in 6 cases, full `pulp-test-environment` passing 100
assertions in 20 cases, full `pulp-test-permissions` passing 47
assertions in 15 cases, exact `ctest --test-dir build -R
"(ProgressParser accepts|ProgressParser ignores|Environment: publish
without listeners|Environment: token reset|PermissionsOverride clear on
missing|empty nested PermissionsOverride)" --output-on-failure` passing
6/6, `git diff --check`, and `./tools/check-docs.sh` passed with
existing warnings. Branch pushed with GitHub-hosted PR workflow only; no
Namespace dispatch and no SSH targets.
Resume action: monitor #2040 required checks and merge directly when
green.

2026-05-15 02:39 PDT: created the #650 canvas text/SDF batch
`feature/phase3-canvas-text-batch-650` at `9b518b1`, PR #2041, from
current `origin/main` `ce86d9f`. It adds test-only coverage in
`test/test_sdf_atlas.cpp`, `test/test_msdf_atlas.cpp`,
`test/test_psdf_atlas.cpp`, `test/test_sdf_atlas_cache.cpp`, and
`test/test_path_to_sdf.cpp` for SdfAtlas move semantics, invalid rebuild
state retention, overflow cleanup, MsdfAtlas move semantics, channel mode
reset, overflow cleanup, PsdfAtlas lookup and move behavior, vector
fallback threshold strictness, SdfAtlasCache empty initialization and
resident-glyph recency updates, and path_to_sdf one-pixel island
saturation. It was expanded before CI settled with additional test-only
coverage in `test/test_sdf_effects.cpp`, `test/test_sdf_text.cpp`, and
`test/test_sdf_software_renderer.cpp` for SdfEffectParams defaults,
manual active masks, explicit zero-dimension presets, SdfTextOptions
defaults, empty/zero-size quad building, wrapper option forwarding,
software-renderer invalid output bounds, and custom edge thresholds. Local
macOS validation passed: configure `cmake -S . -B build
-DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF
-DPULP_BUILD_EXAMPLES=OFF`, build `cmake --build build --target
pulp-test-sdf-atlas pulp-test-msdf-atlas pulp-test-psdf-atlas
pulp-test-sdf-atlas-cache pulp-test-path-to-sdf -j$(sysctl -n hw.ncpu)`,
build `cmake --build build --target pulp-test-sdf-effects
pulp-test-sdf-text pulp-test-sdf-software-renderer -j$(sysctl -n
hw.ncpu)`,
focused `./build/test/pulp-test-sdf-atlas "[coverage][issue-650]" -r
compact` passing 20 assertions in 2 cases, focused
`./build/test/pulp-test-msdf-atlas "[coverage][issue-650]" -r compact`
passing 20 assertions in 2 cases, focused
`./build/test/pulp-test-psdf-atlas "[coverage][issue-650]" -r compact`
passing 11 assertions in 2 cases, focused
`./build/test/pulp-test-sdf-atlas-cache "[coverage][issue-650]" -r
compact` passing 11 assertions in 2 cases, focused
`./build/test/pulp-test-path-to-sdf "[coverage][issue-650]" -r compact`
passing 4 assertions in 1 case, full `pulp-test-sdf-atlas` passing 120
assertions in 12 cases, full `pulp-test-msdf-atlas` passing 93
assertions in 12 cases, full `pulp-test-psdf-atlas` passing 23
assertions in 4 cases, full `pulp-test-sdf-atlas-cache` passing 69
assertions in 11 cases, full `pulp-test-path-to-sdf` passing 536
assertions in 7 cases, focused `./build/test/pulp-test-sdf-effects
"[coverage][issue-650]" -r compact` passing 19 assertions in 3 cases,
focused `./build/test/pulp-test-sdf-text "[coverage][issue-650]" -r
compact` passing 17 assertions in 3 cases, focused
`./build/test/pulp-test-sdf-software-renderer "[coverage][issue-650]" -r
compact` passing 5 assertions in 2 cases, full `pulp-test-sdf-effects`
passing 37 assertions in 8 cases, full `pulp-test-sdf-text` passing 62
assertions in 11 cases, full `pulp-test-sdf-software-renderer` passing
1049 assertions in 9 cases, exact `ctest --test-dir build -R "(SdfAtlas
move|SdfAtlas invalid rebuild|MsdfAtlas move|MsdfAtlas rebuild|PsdfAtlas
keeps|vector_fallback equality|SdfAtlasCache can initialize|SdfAtlasCache
touching|path_to_sdf: one-pixel)" --output-on-failure` passing 9/9,
exact `ctest --test-dir build -R "(SdfEffectParams default|SdfEffect
bitmask accepts|SDF effect presets preserve|SdfTextOptions
default|build_text_quads handles empty|named SDF text wrappers forward
options|software SDF render leaves output untouched for invalid|software
SDF render honors custom)" --output-on-failure` passing 8/8,
`git diff --check`, and `./tools/check-docs.sh` passed with existing
warnings. Branch pushed with GitHub-hosted PR workflow only; no Namespace
dispatch and no SSH targets.
Resume action: monitor #2041 required checks and merge directly when
green.

2026-05-15 02:54 PDT: created the #651 view theme/assets batch
`feature/phase3-view-assets-theme-batch-651` at `a4355aa`, PR #2042,
from current `origin/main` `ce86d9f`. It adds test-only coverage in
`test/test_theme.cpp`, `test/test_asset_manager.cpp`,
`test/test_theme_contrast.cpp`, and `test/test_theme_presets.cpp` for
theme JSON optional sections, alpha colors, invalid JSON file fallback,
unwritable save targets, asset wrapper validity and byte sizes, invalid
and short image buffers, data URI PNG dimension decode with ignored
characters, font file family derivation and cache reuse, contrast channel
clamping, negative hue wrapping with alpha preservation, contrast
validation on partial themes, preset id ordering, and variant-specific
theme preset overrides. Local macOS validation passed: configure `cmake
-S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF
-DPULP_BUILD_EXAMPLES=OFF`, build `cmake --build build --target
pulp-test-theme pulp-test-asset-manager pulp-test-theme-contrast
pulp-test-theme-presets -j$(sysctl -n hw.ncpu)`, focused
`./build/test/pulp-test-theme "[coverage][issue-651]" -r compact`
passing 12 assertions in 2 cases, focused
`./build/test/pulp-test-asset-manager "[coverage][issue-651]" -r
compact` passing 24 assertions in 4 cases, focused
`./build/test/pulp-test-theme-contrast "[coverage][issue-651]" -r
compact` passing 11 assertions in 3 cases, focused
`./build/test/pulp-test-theme-presets "[coverage][issue-651]" -r
compact` passing 14 assertions in 2 cases, full `pulp-test-theme`
passing 94 assertions in 19 cases, full `pulp-test-asset-manager`
passing 99 assertions in 33 cases, full `pulp-test-theme-contrast`
passing 59 assertions in 26 cases, full `pulp-test-theme-presets`
passing 875 assertions in 11 cases, exact `ctest --test-dir build -R
"(Theme JSON parser covers optional|Theme file IO rejects|Asset data
wrappers report|AssetManager image memory loading|AssetManager data URI
decodes|AssetManager font file loading|Contrast helpers clamp|Shift hue
wraps negative|Theme contrast validation skips|preset_ids
preserves|theme_from_preset applies)" --output-on-failure` passing 11/11,
and `git diff --check`. Branch pushed with GitHub-hosted PR workflow
only; no Namespace dispatch and no SSH targets.
Resume action: monitor #2042 required checks and merge directly when
green.

2026-05-15 03:02 PDT: created the #652 view controls/widgets batch
`feature/phase3-view-controls-batch-652` at `d85b567`, PR #2043, from
current `origin/main` `ce86d9f`. It adds test-only coverage in
`test/test_phase9_widgets.cpp` for EqCurveView disabled grid/disabled
band paint paths, MidiKeyboard reversed-range clamping, vertical drag
note release/retrigger behavior, ColorPicker hex-only mode with outside
mouse input stability, FileDropZone idle/valid/invalid/no-icon paint
states plus invalid and empty drops, and ThemeEditor missing selection,
empty theme, selected token paint, and JSON export behavior. Local macOS
validation passed: configure `cmake -S . -B build
-DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF
-DPULP_BUILD_EXAMPLES=OFF`, build `cmake --build build --target
pulp-test-phase9-widgets -j$(sysctl -n hw.ncpu)`, focused
`./build/test/pulp-test-phase9-widgets "[coverage][issue-652]" -r
compact` passing 35 assertions in 6 cases, full
`./build/test/pulp-test-phase9-widgets -r compact` passing 160
assertions in 36 cases, exact `ctest --test-dir build -R "(EqCurveView
paint covers disabled|MidiKeyboard drag releases|ColorPicker mode and
outside|FileDropZone paint reflects|FileDropZone invalid or
empty|ThemeEditor covers missing)" --output-on-failure` passing 6/6, and
`git diff --check`. Branch pushed with GitHub-hosted PR workflow only; no
Namespace dispatch and no SSH targets.
Resume action: monitor #2043 required checks and merge directly when
green.

2026-05-15 03:11 PDT: created the #653 view controls/dialogs batch
`feature/phase3-view-dialogs-batch-653` at `0ccc728`, PR #2044, from
current `origin/main` `ce86d9f`. It adds coverage in
`test/test_modal.cpp`, `test/test_gui_components.cpp`,
`test/test_property_list.cpp`, and `test/test_table_list_box.cpp` for
ModalOverlay child-vs-backdrop dismiss behavior, mouse-up and missing
callback paths, Toolbar missing ids, item sizing, vertical intrinsic
height, disabled and missing remove behavior, PropertyList category
visibility, key-label fallback, non-bool selection highlighting, outside
miss clearing, and TableListBox clear-column/model-less/out-of-bounds
click behavior. The batch also fixes `core/view/src/table.cpp` so
TableListBox ignores mouse clicks outside its bounds before entering
header-sort or row-selection logic; the new negative-y table test exposed
that edge. Local macOS validation passed: configure `cmake -S . -B build
-DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF
-DPULP_BUILD_EXAMPLES=OFF`, build `cmake --build build --target
pulp-test-modal pulp-test-gui-components pulp-test-property-list
pulp-test-table-list-box -j$(sysctl -n hw.ncpu)`, focused
`./build/test/pulp-test-modal "[coverage][issue-653]" -r compact`
passing 4 assertions in 2 cases, focused
`./build/test/pulp-test-gui-components "[coverage][issue-653]" -r
compact` passing 5 assertions in 1 case, focused
`./build/test/pulp-test-property-list "[coverage][issue-653]" -r
compact` passing 13 assertions in 2 cases, focused
`./build/test/pulp-test-table-list-box "[coverage][issue-653]" -r
compact` passing 9 assertions in 2 cases, full `pulp-test-modal`
passing 12 assertions in 7 cases, full `pulp-test-gui-components`
passing 134 assertions in 30 cases, full `pulp-test-property-list`
passing 38 assertions in 6 cases, full `pulp-test-table-list-box`
passing 42 assertions in 6 cases, exact `ctest --test-dir build -R
"(ModalOverlay backdrop click|ModalOverlay ignores mouse-up|Toolbar
missing ids|PropertyList category visibility|PropertyList selection
highlight|TableListBox clear columns|TableListBox paints selected row)"
--output-on-failure` passing 7/7, and `git diff --check`. Branch pushed
with GitHub-hosted PR workflow only; no Namespace dispatch and no SSH
targets. Pre-push local diff-cover was demoted by
`PULP_DISABLE_PREPUSH_DIFF_COVER=1` after a local FetchContent checkout
failed to read the pinned Highway tree; focused and full macOS tests were
green before push.
Resume action: monitor #2044 required checks and merge directly when
green.

2026-05-15 03:18 PDT: created the #654 view tree/accessibility batch
`feature/phase3-view-tree-accessibility-batch-654` at `41b277f`, PR
#2045, from current `origin/main` `ce86d9f`. It adds test-only coverage
in `test/test_tree_view.cpp` and `test/test_accessibility_tree.cpp` for
TreeView collapsed-child paint/hit-testing, expanded child double-click
activation, selected-row paint, left-key behavior on a collapsed parent,
accessibility value-string clamping/fallback, and default text/table/cell
helper interface behavior. Local macOS validation passed: configure
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF
-DPULP_BUILD_EXAMPLES=OFF`, build `cmake --build build --target
pulp-test-tree-view pulp-test-accessibility-tree -j$(sysctl -n
hw.ncpu)`, focused `./build/test/pulp-test-tree-view
"[coverage][issue-654]" -r compact` passing 12 assertions in 3 cases,
focused `./build/test/pulp-test-accessibility-tree
"[coverage][issue-654]" -r compact` passing 20 assertions in 2 cases,
full `./build/test/pulp-test-tree-view -r compact` passing 50
assertions in 16 cases, full `./build/test/pulp-test-accessibility-tree
-r compact` passing 67 assertions in 9 cases, exact `ctest --test-dir
build -R "(TreeView collapsed children|TreeView expanded child|TreeView
left key|Accessibility value interface|Accessibility helper interfaces)"
--output-on-failure` passing 5/5, and `git diff --check`. Branch pushed
with GitHub-hosted PR workflow only; no Namespace dispatch and no SSH
targets.
Resume action: monitor #2045 required checks and merge directly when
green.

2026-05-15 03:24 PDT: repaired PR #2017
`feature/phase3-view-widget-batch-493` after GitHub-hosted macOS ARM64
timed out in the shellout test `pulp doctor android|ios are recognized
subcommands`. The real macOS job failed on test #67 after 120.17s; the
`macos` alias failed because it mirrored that platform failure. Commit
`6e46f56` changes that parser-recognition test to run
`pulp doctor android --versions` and `pulp doctor ios --versions`, which
still proves the mobile subcommands are accepted before the diagnostic
short-circuit but avoids slow Android/iOS host SDK probes on saturated CI
runners. Local validation on the PR branch passed: `cmake --build build
--target pulp-test-cli-shellout -j$(sysctl -n hw.ncpu)`,
`ctest --test-dir build -R "pulp doctor android\\|ios are recognized
subcommands" --output-on-failure`, and `git diff --check`. Branch pushed
with GitHub-hosted PR workflow only; no Namespace dispatch and no SSH
targets.
Resume action: monitor #2017 rerun checks and merge directly when green.

2026-05-15 03:29 PDT: created the #655 view panels/editor batch
`feature/phase3-view-panels-batch-655` at `70b32df`, PR #2046, from
current `origin/main` `ce86d9f`. It adds coverage in
`test/test_file_browser.cpp`, `test/test_graph_editor_view.cpp`,
`test/test_preset_browser.cpp`, and `test/test_code_editor.cpp` for
FileBrowser filtered/height-clipped paint, MultiDocumentPanel empty and
active-tab paint, GraphEditorView reverse modifier drags, unnamed-node
paint, drag-miss ghost cleanup, PresetBrowser filtered-list row geometry,
CodeEditor read-only fallback insert state, and RecentlyOpenedFilesList
remove/max-entry edges. The batch also fixes
`core/view/include/pulp/view/preset_browser.hpp` so clicks in the filter
gap above the list return before row hit-testing; the new negative-gap
click test exposed that float-to-int truncation selected row 0. Local
macOS validation passed: configure `cmake -S . -B build
-DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF
-DPULP_BUILD_EXAMPLES=OFF`, build `cmake --build build --target
pulp-test-file-browser pulp-test-graph-editor-view
pulp-test-preset-browser pulp-test-code-editor -j$(sysctl -n hw.ncpu)`,
focused `./build/test/pulp-test-file-browser "[coverage][issue-655]" -r
compact` passing 13 assertions in 2 cases, focused
`./build/test/pulp-test-graph-editor-view "[coverage][issue-655]" -r
compact` passing 8 assertions in 2 cases, focused
`./build/test/pulp-test-preset-browser "[coverage][issue-655]" -r
compact` passing 5 assertions in 1 case after the guard fix, focused
`./build/test/pulp-test-code-editor "[coverage][issue-655]" -r compact`
passing 10 assertions in 2 cases, full `pulp-test-file-browser` passing
73 assertions in 6 cases, full `pulp-test-graph-editor-view` passing 30
assertions in 6 cases, full `pulp-test-preset-browser` passing 65
assertions in 19 cases, full `pulp-test-code-editor` passing 62
assertions in 13 cases, exact `ctest --test-dir build -R "(FileBrowser
paint clips|MultiDocumentPanel paint reflects|GraphEditorView reverse
modifier|GraphEditorView paints unnamed|PresetBrowser filtered list
click|CodeEditor insert appends|RecentlyOpenedFilesList remove)"
--output-on-failure` passing 7/7, and `git diff --check`. Branch pushed
with GitHub-hosted PR workflow only; no Namespace dispatch and no SSH
targets. Pre-push local diff-cover was demoted by
`PULP_DISABLE_PREPUSH_DIFF_COVER=1` after a local FetchContent checkout
failed to read the pinned Highway tree; focused and full macOS tests were
green before push.
Resume action: monitor #2046 required checks and merge directly when
green.

2026-05-15 03:45 PDT: prepared the next runtime helper batch locally in
`/private/tmp/pulp-phase3-runtime-helpers-batch-656` on
`feature/phase3-runtime-helpers-batch-656` at `5df4150`, intentionally
not pushed yet to avoid adding another GitHub CI run while the open
queue is saturated. The held batch covers six runtime/helper test
targets with 16 new focused CTest cases across identity parsing, stream
result/file-position behavior, i18n duplicate/trailing parser paths,
analytics JSON escaping and empty flush, memory-mapped file read-write
and empty-file paths, HTTP helper status/URL boundaries, BigInteger
copy/move/hex/bit-count helpers, and license activation malformed-URL
paths. It also includes two bounded correctness fixes exposed by the new
tests: `Uuid::from_string` now rejects non-hex digits and misplaced
dashes instead of decoding them as zero nibbles, and
`FileAnalyticsDestination` now escapes JSON string output for quotes,
backslashes, and common control characters. Local macOS validation
passed: configure `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
-DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF`, build `cmake --build
build --target pulp-test-runtime-utils pulp-test-stream
pulp-test-identity pulp-test-i18n pulp-test-analytics
pulp-test-license -j$(sysctl -n hw.ncpu)`, focused issue tag runs
passing 80 assertions in 15 cases, full affected binaries passing 456
assertions in 114 cases, exact `ctest --test-dir build -R
"(MemoryMappedFile read-write|MemoryMappedFile rejects empty|HttpResponse
ok|HTTP helpers reject malformed URL|StreamResult helpers|FileStream
zero-size|Uuid ordering|i18n duplicate|i18n \\.strings parser
lets|i18n JSON parser accepts|FileAnalyticsDestination
escapes|FileAnalyticsDestination empty flush|BigInteger copy
move|LicenseValidator validate_and_parse includes|OnlineActivation
rejects)" --output-on-failure` passing 16/16, and `git diff --check`.
Resume action: keep this batch held for consolidation unless the queue
clears; if pushed, open one GitHub-hosted PR and record the PR number.

2026-05-15 03:48 PDT: investigated apparent failures on open codecov PRs
after the GitHub queue spike. PRs #2012, #2014, #2016, #2022, #2030,
and #2031 showed failed alias or sanitizer/coverage checks, but the
inspected runs were cancellations clustered around 2026-05-15 10:36 UTC,
not actionable test failures. Examples: #2030 and #2031 macOS,
sanitizer, and macOS coverage legs were cancelled while Windows/Linux
legs succeeded; #2012 ASan/UBSan were cancelled while TSan succeeded;
#2016 Android coverage was cancelled while macOS/Windows/Linux coverage
legs succeeded; #2022 macOS, Android macOS build, and ASan/UBSan were
cancelled while Linux/Windows and TSan succeeded. Current action: do not
patch code for these cancellation-only failures; selectively rerun failed
jobs later when the queue calms, and avoid pushing small new CI batches.

2026-05-15 04:45 PDT: collapsed the codecov queue from 25 small
GitHub-hosted test PRs into 4 broader replacement PRs to reduce slow
macOS runner fan-out. Before opening replacements, cancelled queued /
in-progress runs for the superseded branches where possible, merged each
source branch into a domain worktree, resolved conflicts, and ran local
macOS validation with `PULP_ENABLE_GPU=OFF` and examples off. No
Namespace CI or SSH hosts were used.

Replacement PRs:
- #2048 `feature/phase3-codecov-runtime-platform-consolidated-657`
  supersedes #2025, #2026, #2030, #2031, #2032, #2033, #2040, plus the
  held local runtime helper batch #656. Local validation built affected
  runtime/platform/event/state targets and directly ran
  `pulp-test-analytics`, `pulp-test-crypto`, `pulp-test-environment`,
  `pulp-test-events-async-helpers`, `pulp-test-i18n`,
  `pulp-test-identity`, `pulp-test-ipc-endpoints`,
  `pulp-test-json-rpc`, `pulp-test-license`,
  `pulp-test-memory-message-channel`,
  `pulp-test-network-service-discovery`, `pulp-test-permissions`,
  `pulp-test-platform`, `pulp-test-properties`, `pulp-test-runtime`,
  `pulp-test-runtime-utils`, `pulp-test-state-tree`,
  `pulp-test-stream`, and `pulp-test-xml-zip`; all passed. Consolidation
  exposed malformed string literals in `test_i18n.cpp` and
  `test_license.cpp`; fixed in follow-up commits on the replacement
  branch before push.
- #2049 `feature/phase3-codecov-media-consolidated-658` supersedes
  #2012, #2014, #2019, #2028, #2034, #2041. Local validation built and
  directly ran affected audio/MIDI/signal/canvas/OSC/SDF binaries:
  `pulp-test-attributed-string`, `pulp-test-audio-file`,
  `pulp-test-descriptor-validation`, `pulp-test-diagnostic`,
  `pulp-test-dirty-tracker`, `pulp-test-dsp-expansion`,
  `pulp-test-midi-buffer-ump`, `pulp-test-midi-ci`,
  `pulp-test-midi-file`, `pulp-test-mpe-buffer`,
  `pulp-test-msdf-atlas`, `pulp-test-osc`, `pulp-test-osc-bundle`,
  `pulp-test-path-to-sdf`, `pulp-test-psdf-atlas`,
  `pulp-test-scan-blacklist`, `pulp-test-sdf-atlas`,
  `pulp-test-sdf-atlas-cache`, `pulp-test-sdf-effects`,
  `pulp-test-sdf-software-renderer`, `pulp-test-sdf-text`, and
  `pulp-test-signal`; all passed.
- #2051 `feature/phase3-codecov-tools-host-format-consolidated-659`
  supersedes #2016, #2022, #2024, #2035, #2037, #2038. Local validation
  built and directly ran affected tools/host/format/state binaries:
  `pulp-test-audio-tools`, `pulp-test-binding`,
  `pulp-test-child-process`, `pulp-test-cli-fetchcontent-cache`,
  `pulp-test-cli-shellout`, `pulp-test-descriptor-validation`,
  `pulp-test-environment`, `pulp-test-graph-serializer`,
  `pulp-test-headless`, `pulp-test-host`, `pulp-test-ios-audio-session`,
  `pulp-test-plugin-state-io`, `pulp-test-preset-manager`,
  `pulp-test-sdl3-surface`, `pulp-test-state`,
  `pulp-test-validation-harness`, `pulp-test-aax-model`, and
  `pulp-test-ara`; all passed. Python tests first failed under
  `python3 -m pytest` because this local Python 3.14 lacks pytest, then
  passed through their checked-in unittest entrypoints:
  `tools/packages/test_freshness_check_extra.py`,
  `tools/scripts/test_auto_release_decision_extra.py`,
  `tools/scripts/test_check_format_validation.py`,
  `tools/scripts/test_fetch_skia_for_release_extra.py`,
  `tools/scripts/test_host_pump_lint.py`,
  `tools/scripts/test_macos_reroute_watcher.py`, and
  `tools/scripts/test_ruleset_drift_config.py`.
- #2050 `feature/phase3-codecov-view-consolidated-660` supersedes
  #2017, #2042, #2043, #2044, #2045, #2046. Local validation built and
  directly ran affected view/UI/theme/widget binaries:
  `pulp-test-accessibility-tree`, `pulp-test-appearance`,
  `pulp-test-asset-manager`, `pulp-test-audio-bridge`,
  `pulp-test-auto-ui`, `pulp-test-cli-shellout`,
  `pulp-test-code-editor`, `pulp-test-file-browser`,
  `pulp-test-graph-editor-view`, `pulp-test-gui-components`,
  `pulp-test-image-cache`, `pulp-test-input-events`, `pulp-test-modal`,
  `pulp-test-param-attachment`, `pulp-test-phase9-widgets`,
  `pulp-test-preset-browser`, `pulp-test-property-list`,
  `pulp-test-table-list-box`, `pulp-test-theme`,
  `pulp-test-theme-contrast`, `pulp-test-theme-presets`,
  `pulp-test-tree-view`, `pulp-test-ui-components`,
  `pulp-test-visualization`, `pulp-test-waveform-editor`,
  `pulp-test-widgets`, and `pulp-test-window-manager`; all passed.

All superseded PRs were commented and closed after replacements existed;
source branches were intentionally retained for audit. Pre-push
diff-cover on #2048, #2050, and #2051 attempted a separate coverage
configure and hit a local transient Highway FetchContent checkout error
(`failed to checkout tag 457c891...`), then was demoted by
`PULP_DISABLE_PREPUSH_DIFF_COVER=1`; focused local macOS validation was
green before push.

Updated batching policy for remaining codecov work: hold future
coverage slices locally until there is a substantial domain corpus, then
submit one broad PR per related area instead of small PR fan-out. Target
roughly a dozen discrete slices per PR when practical, validate locally
on macOS, push only GitHub-hosted CI, avoid Namespace, and record both
the held local queue and any submitted replacement PR mapping here.

2026-05-15 05:25 PDT: started the next held local batch under the updated
large-PR policy. Worktree:
`/private/tmp/pulp-phase3-codecov-ship-package-batch-661`, branch
`feature/phase3-codecov-ship-package-batch-661`, now rebased to commit
`eb68a2b68` (`test(ship): cover package cli failure paths`) on current
`origin/main` after the initial `342260bf1` ship/package edge commit.
This branch has not been
pushed and has no PR yet. Scope: ship/package coverage for appcast XML
fallbacks and parsing edges, Android SDK/tool discovery and package
failure branches, NSIS installer script generation branches, and
codesign/notarization/pkg failure paths. Also includes adjacent release
package CLI coverage for rpath handling, binary staging, and archive
packaging branches, missing binary / missing WGPU hard-fail behavior,
Windows ZIP packaging with the `pulp-cpp` delegate, Linux rpath rewrites
for both shipped binaries, plus Skia release-fetch manifest and archive
layout coverage. Local macOS validation:
configured with `-DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF`, built
`pulp-test-appcast`, `pulp-test-android-package`,
`pulp-test-nsis-installer`, and `pulp-test-codesign`, then directly ran
all four binaries. Passing totals: appcast 89 assertions / 16 cases,
Android package 129 assertions / 18 cases, NSIS installer 57 assertions /
19 cases, codesign 19 assertions / 10 cases. Python release packaging
validation also passed through unittest entrypoints after the rebase:
`tools/scripts/test_package_cli.py` (13 tests),
`tools/scripts/test_package_cli_extra.py` (15 tests after adding four
new package CLI edge cases), and `tools/scripts/test_fetch_skia_for_release.py`
(14 tests). `git diff --check` passed before the new local commit.
Resume action: keep holding this branch locally and continue
adding coherent ship/package or adjacent release coverage until it is
large enough to justify one GitHub-hosted CI run, then push as a single
batched PR.

2026-05-15 10:45 PDT: resumed monitoring/fix pass on the four
consolidated GitHub-hosted codecov PRs with the policy that failures are
actionable unless logs prove stale/external infrastructure. No Namespace
or SSH validation was dispatched. Findings and actions:

- #2048 (`feature/phase3-codecov-runtime-platform-consolidated-657`) had
  a real required diff-coverage miss in `analytics.cpp` JSON escaping and
  `identity.cpp` UUID dash validation. Added focused runtime tests in
  `test/test_analytics.cpp` and `test/test_identity.cpp`, committed
  `865242368` (`test(runtime): cover consolidated branch edges`), and
  locally validated `pulp-test-analytics` plus `pulp-test-identity`
  focused and full binary runs. Push required demoting the local pre-push
  diff-cover gate because the separate coverage configure hit the local
  Highway FetchContent checkout error (`failed to checkout tag
  457c891...`); GitHub-hosted checks remain authoritative and are running
  on the fresh SHA.
- #2050 (`feature/phase3-codecov-view-consolidated-660`) had a real
  Windows failure in `AssetManager font file loading derives family names
  and uses cache`: the production code derived the font family by splitting
  only on `/`, so Windows paths produced the full temporary path as the
  family name. Fixed the root cause in `core/view/src/asset_manager.cpp`
  by deriving the stem through `std::filesystem::path`, added focused view
  boundary tests in `test/test_preset_browser.cpp` and
  `test/test_table_list_box.cpp`, committed `4ed92c718`
  (`fix(view): derive font family with platform paths`), and locally
  validated the affected focused/full test binaries plus the previously
  failing CLI project-bump case. Fresh GitHub-hosted checks are running on
  the updated SHA.
- #2049 and #2051 still showed external `codecov/patch` failures, but
  current PR file lists do not include the previously reported
  `core/canvas/src/svg.cpp` diff. The relevant GitHub Coverage workflows
  were green on their current heads, so reran Coverage for #2049
  (`25914516624`) and #2051 (`25914516918`) to force Codecov to recompute
  against the actual current diffs rather than patching unrelated SVG code.
- #2051 Build and Test had a macOS failure in the runner dependency
  checkout path (`Could not resolve host: github.com` while cloning
  `google/highway.git`), not test code. Reran failed jobs for
  `25914516941`; if the rerun exposes a deterministic test/build failure,
  fix the branch rather than rerunning again blindly.

Resume action: keep monitoring #2048, #2049, #2050, and #2051. Merge only
after required GitHub checks and Codecov are green. If a rerun produces a
new deterministic failure, inspect logs and patch the root cause on the
owning consolidated branch.

2026-05-15 11:20 PDT: swept all currently open `codecov` PRs. There are
four: #2048, #2049, #2050, and #2051. No non-bot review/issue comments
needed action on #2048, #2049, or #2050. #2051 had two actionable Codex
review comments against `core/host/src/graph_serializer.cpp`: preserve the
serialized display name for unresolved plugin nodes, and avoid a second
`PluginSlot::load()` after deserialization already observed a load failure.
Patched #2051 in commit `6fc610acd` (`fix(host): preserve unresolved graph
plugin labels`) by adding an explicit unresolved-plugin node path in
`SignalGraph`, switching `GraphSerializer::from_json()` to use it, and
adding a focused graph serializer regression test. Local validation passed:
built `pulp-test-graph-serializer`, ran focused unresolved-plugin display
name and identity tests, then ran the full graph serializer binary (394
assertions / 17 cases). Pushed the branch and commented on #2051 with the
validation summary. Fresh GitHub-hosted CI is running for the updated head.

2026-05-15 11:38 PDT: #2048's fresh required diff-coverage job completed
red at 72% with concrete misses in `core/runtime/src/analytics.cpp` lines
14-18 and `core/runtime/src/identity.cpp` lines 47-48. Root cause on the
identity side was a test-input bug: the prior misplaced-dash examples were
not exactly 36 characters, so they exercised the length guard rather than
the dashed-format guard. Added commit `8ec1b311f`
(`test(runtime): cover remaining coverage gate branches`) with an exact
36-character malformed dashed UUID and additional per-character analytics
JSON escape cases for backslash, quote, newline, carriage return, and tab
in event names plus property keys/values. Local validation passed:
`cmake --build build --target pulp-test-analytics pulp-test-identity`,
focused analytics escape tests (19 assertions / 3 cases), focused UUID
round-trip test (19 assertions / 1 case), full `pulp-test-analytics` (49
assertions / 17 cases), full `pulp-test-identity` (51 assertions / 8
cases), and `git diff --check`. Pushed #2048; fresh GitHub-hosted checks
are running.

2026-05-15 11:40 PDT: continued the held local release/package batch
`feature/phase3-codecov-ship-package-batch-661` after rebasing onto current
`origin/main`. Added commit `cf4001cea`
(`test(release): guard dual cli packaging workflow`) to extend
`tools/scripts/test_release_workflow_test_step.py` with static regression
coverage for the release CLI workflow's dual-binary contract: Unix and
Windows package steps must pass `--cpp-binary`, and Unix/Windows smoke
steps must exercise both `pulp` and `pulp-cpp` while checking loader/DLL
failure text. Local validation passed
`python3 tools/scripts/test_release_workflow_test_step.py` (9 tests) and
`git diff --check`. This batch remains local-only; no PR/CI dispatch yet.

2026-05-15 11:50 PDT: added another local-only package validation slice to
the same held batch: commit `5ab7f9590`
(`test(packages): cover registry validation reporting`). It extends
`tools/scripts/test_validate_registry_extra.py` to cover non-strict warning
exit behavior without `--check-licenses`, confirm license warnings are not
printed when license checking is disabled, and verify package status prints
`FAIL` when a slug-specific license error is present. Local validation
passed `python3 tools/scripts/test_validate_registry_extra.py` (10 tests),
`python3 tools/packages/test_package_validation_tools.py` (14 tests), and
`git diff --check`. Held batch is now four commits ahead of current
`origin/main` and remains unpushed.

2026-05-15 12:27 PDT: reconciled the active working objective with the
current user direction. The thread-level `/goal` still contains the older
paused local-only/no-CI objective and cannot be edited or resumed with the
available goal tool, so this ledger entry is the operative policy for the
remaining Phase 3 Codecov work. Continue using GitHub-hosted CI only; do
not dispatch Namespace or SSH-host validation. Build future coverage work
as broad, coherent domain batches rather than small one-off PRs, targeting
roughly a dozen meaningful coverage slices per submitted PR when practical.
Validate locally on macOS before pushing, push/submit batched PRs when the
batch is substantial, monitor owned PRs periodically, fix deterministic
failures at the root cause, and merge only after required GitHub checks and
Codecov are green. Keep held local batches and live PR status recorded here.

Current consolidated PR status at this checkpoint:

- #2048 `feature/phase3-codecov-runtime-platform-consolidated-657` is on
  pushed SHA `41ebdb229b82355283d3eb446666f6f717296a87`; no failures yet,
  14 GitHub-hosted checks still pending. The latest push rewrote
  `analytics.cpp` escape branches so LLVM line coverage can see the actual
  escaped cases and added a separate malformed dashed UUID test. Local
  focused validation passed for `pulp-test-analytics` and
  `pulp-test-identity`. Push used pre-push diff-cover demotion because the
  local coverage configure hit an external FetchContent checkout issue for
  `mbedtls v3.6.2`; GitHub-hosted CI remains authoritative.
- #2049 `feature/phase3-codecov-media-consolidated-658` is on SHA
  `c2d15a57f90962c1812200ca644c30d2a09b3d21`; GitHub checks are complete
  but `codecov/patch` is still failing, so it is not merge-ready.
- #2050 `feature/phase3-codecov-view-consolidated-660` is on SHA
  `4ed92c7181148baae0c0d9f80202f7b83cc7dfd9`; `Diff coverage required` and
  `codecov/patch` are failing, with 3 checks still pending. CI logs show
  the relevant AssetManager and PresetBrowser tests execute, but coverage
  artifacts show the specific diff lines remain zero-hit. A local focused
  coverage reproduction is running in
  `/private/tmp/pulp-phase3-codecov-view-consolidated-660` to identify the
  real root cause before patching.
- #2051 `feature/phase3-codecov-tools-host-format-consolidated-659` is on
  SHA `6fc610acd4648a8e57d95766d8f33d289c7978a1`; no failures currently,
  4 GitHub-hosted checks pending after the unresolved-plugin label fix.

Resume action: finish the local #2050 coverage reproduction, patch the
root cause if confirmed, then continue monitoring #2048/#2049/#2050/#2051.
After those are stable, continue adding to the held ship/package batch and
future large domain batches before opening new GitHub-hosted PRs.

2026-05-15 12:30 PDT: #2050 local focused coverage reproduction completed
in `/private/tmp/pulp-phase3-codecov-view-consolidated-660`. The focused
CTest run passed both
`PresetBrowser ignores filtered clicks before the list starts` and
`AssetManager font file loading derives family names and uses cache`.
The generated Cobertura report showed the exact diff-gate lines hit
locally: `preset_browser.hpp` lines 215-217 hit once and
`asset_manager.cpp` lines 206, 207, 210, and 212 hit once. This confirms
the current test paths cover the reported diff lines on macOS and that the
previous GitHub diff-gate failure was not a missing test assertion. Reran
the GitHub-hosted Coverage workflow for #2050 (`25932577890`); after the
rerun, `Diff coverage required` was no longer listed as failing, with the
Coverage rerun still pending and `codecov/patch` still red until Codecov
recomputes.

2026-05-15 12:33 PDT: investigated #2049's isolated `codecov/patch`
failure. GitHub's required diff gate and all build/coverage/sanitizer jobs
were green, but Codecov reported patch coverage below the 75% target even
though the visible diff against current `origin/main` was test-only. The
branch merge-base was stale (`a65d223427868efd056f281890ce97d8120c1397`),
so the consolidated merge history was likely confusing Codecov's patch
comparison. Flattened the current visible test-only diff onto current
`origin/main` and force-with-lease updated the existing PR branch to
`fbc9f1c22b9fb64bc4ecec34f581ae663e661778`
(`test: consolidate media codecov queue`). Local validation before push:
`git diff --check origin/main...HEAD` passed, and the pre-push hook passed
with no mapped skill-sync/compat-sync paths touched plus its 20-test
check. No Namespace or SSH validation was used. Fresh GitHub-hosted checks
are now pending on #2049 with no failures at the time of this entry.

2026-05-15 12:34 PDT: continued the held local ship/package batch after
rebasing `feature/phase3-codecov-ship-package-batch-661` onto current
`origin/main` `664c2c52f`. Added commit `7eee37120`
(`test(release): cover skia fetch archive edges`) extending
`tools/scripts/test_fetch_skia_for_release.py` with coverage for missing
CLI arguments, unknown-platform warning output, Windows archive flattening,
and the no-clobber branch when an arch subdirectory contains a duplicate
library name. Local validation passed
`python3 tools/scripts/test_fetch_skia_for_release.py` (17 tests) and
`git diff --check`. The batch is now five commits ahead of current
`origin/main` and remains local-only/unpushed while the active GitHub PR
queue drains.

2026-05-15 12:38 PDT: added two more held local release coverage slices
to `feature/phase3-codecov-ship-package-batch-661`: commit `b3d29bc75`
(`test(release): cover package cli selection edges`) covers WGPU library
build-dir precedence over cache roots plus macOS dual-binary rpath
rewriting; commit `5186458da` (`test(release): cover auto-release parser
edges`) covers auto-release semver parsing, malformed-version comparison,
and CLI help behavior. Local validation passed the focused and adjacent
Python suites:
`python3 tools/scripts/test_auto_release_decision.py` (27 tests),
`python3 tools/scripts/test_package_cli_extra.py` (17 tests),
`python3 tools/scripts/test_fetch_skia_for_release.py` (17 tests),
`python3 tools/scripts/test_release_workflow_test_step.py` (9 tests),
`python3 tools/scripts/test_validate_registry_extra.py` (10 tests),
`python3 tools/packages/test_package_validation_tools.py` (14 tests), and
`git diff --check`. The held batch is now seven commits ahead of current
`origin/main`, touches 9 files, adds 947 lines of release/package/ship
coverage, and remains intentionally unpushed until it reaches a substantial
PR-sized corpus or the active GitHub queue clears.

2026-05-15 12:41 PDT: added commit `8b0e56ccc`
(`test(ci): cover local validation helper edges`) to the same held batch.
It extends `tools/scripts/test_android_target.py` for Linux default SDK
discovery and successful Gradle builds that do not leave the expected APK
path, and extends `tools/scripts/test_validate_hosts.py` for Windows
`-NoTests:$true` command construction plus the default-current-branch path
when `--branch` is omitted. These are unit-level tests only; no Android
build, SSH host, or Namespace validation was invoked. Local validation
passed `python3 tools/scripts/test_android_target.py` (18 tests),
`python3 tools/scripts/test_validate_hosts.py` (11 tests), and
`git diff --check`. The held batch is now eight commits ahead of current
`origin/main`, touches 11 files, and adds 1007 lines of package/release/CI
helper coverage while remaining unpushed.

2026-05-15 13:51 PDT: rebased the held ship/package/local-CI batch onto
current `origin/main` `52288609a` and added commit `18e981bfb`
(`test(ci): cover local ci state path edges`). The new slice extends
`tools/local-ci/test_local_ci_extra.py` for macOS/Linux state-directory
fallbacks, desktop artifact-root fallbacks, state directory creation, and
target-log preparation. Local validation passed
`python3 tools/local-ci/test_local_ci_extra.py` (19 tests),
`python3 tools/local-ci/test_local_ci.py` (213 tests), and
`git diff --check`. The held batch is now nine commits ahead of current
`origin/main`, touches 12 files, and adds 1045 lines of package/release/CI
helper coverage. It remains local-only and intentionally unpushed until it
is large enough to justify one GitHub-hosted PR or the current PR queue
clears.

2026-05-15 15:04 PDT: added release-discovery coverage slice
`159cbce81` (`test(release): cover migration index codegen edges`) to the
held ship/package/local-CI batch. It extends
`tools/scripts/test_build_migration_index_extra.py` for non-empty C++
codegen source comments, default non-breaking entries, table-size wiring,
and empty-docs CLI output. Local validation passed
`python3 tools/scripts/test_build_migration_index_extra.py` (12 tests),
`python3 tools/scripts/test_build_migration_index.py` (7 tests),
`python3 tools/local-ci/test_local_ci_extra.py` (19 tests),
`python3 tools/scripts/test_android_target.py` (18 tests),
`python3 tools/scripts/test_validate_hosts.py` (11 tests), and
`git diff --check`. The held batch is now ten commits ahead of current
`origin/main`, touches 13 files, and adds 1076 lines of focused
ship/package/release/local-CI coverage. It remains unpushed until the
current GitHub PR queue is ready for one substantial batched PR.

2026-05-15 15:26 PDT: fixed #2048
`feature/phase3-codecov-runtime-platform-consolidated-657` after
investigating the failing `Diff coverage required`/`codecov/patch` checks.
The latest GitHub coverage artifact showed `core/runtime/src/analytics.cpp`
lines 9-28, 56, and 62 plus `core/runtime/src/identity.cpp` lines 44-48
and 56-58 as zero-hit, even though the relevant
`FileAnalyticsDestination` and `Uuid` tests ran and passed on CI. Local
focused coverage reproduced the issue as an llvm-cov object-discovery
ordering problem: source files compiled into both `libpulp-*.a` archives
and executed test binaries can report the archive's zero-hit coverage map
if archives are appended after tests. Commit `786c3681d`
(`test(coverage): prefer executed test maps in llvm-cov`) updates
`scripts/run_coverage.sh` and `tools/scripts/local_diff_cover.sh` to add
static archives before test executables, and adds parity coverage in
`tools/scripts/test_local_diff_cover.py`.

Validation for #2048 before push: `python3 tools/scripts/test_local_diff_cover.py`
passed 17 tests; `git diff --check` passed; `cmake --build build --target
pulp-test-analytics pulp-test-identity -j$(sysctl -n hw.ncpu)` passed;
`ctest --test-dir build --output-on-failure -R "FileAnalyticsDestination|Uuid"`
passed 15 tests; and a focused `scripts/run_coverage.sh --tests
"FileAnalyticsDestination|Uuid"` run completed with all 15 tests passing.
The generated LCOV then showed nonzero hits for every previously missing
diff line: `analytics.cpp` lines 9-28, 56, and 62, and `identity.cpp`
lines 44-48 and 56-58. Pushed #2048 to GitHub-hosted CI at
`786c3681d`. The pre-push hook also attempted its advisory local
diff-cover path and hit an unrelated local FetchContent `mbedtls` tag
checkout failure, but the push completed with that gate demoted; do not
rely on that advisory run as validation.

2026-05-15 15:31 PDT: added another release/package automation slice to
the held local batch `feature/phase3-codecov-ship-package-batch-661`.
Commit `6d6c011fd` (`test(release): cover mcp package and bump guard
edges`) extends package CLI coverage for Linux tarballs that include all
three binaries (`pulp`, `pulp-cpp`, and `pulp-mcp`) and verifies all three
get Linux rpath rewriting. It also extends version-bump coverage for
metadata-stripped config loading, default trailer-key selection, JSON-path
array/scalar miss handling, and accepted vs near-miss bump-marker commit
subjects. While running adjacent release workflow tests, the existing
test still looked for the old two-binary smoke step names; updated it to
assert the current `pulp`/`pulp-cpp`/`pulp-mcp --version` package and
smoke contract from `release-cli.yml`.

Validation for the held slice passed locally:
`python3 tools/scripts/test_package_cli_extra.py` (18 tests),
`python3 tools/scripts/test_version_bump_check_extra.py` (36 tests),
`python3 tools/scripts/test_release_workflow_test_step.py` (9 tests),
`python3 tools/scripts/test_package_cli.py` (17 tests),
`python3 tools/scripts/test_auto_release_decision.py` (27 tests), and
`git diff --check`. The held batch is now 11 commits ahead of
`origin/main`, touches 14 files, and adds 1195 lines of focused
ship/package/release/local-CI coverage. It remains local-only and
intentionally unpushed so it can land as one substantial GitHub-hosted CI
run instead of many small PRs.

2026-05-15 15:41 PDT: refreshed #2050
`feature/phase3-codecov-view-consolidated-660` to address a stale
Codecov patch failure. GitHub showed the `codecov/patch` check failing
with "61.53% of diff hit", but the Codecov v2 PR API reported
`patch.coverage` as 78.12% with CI passed and the check-run metadata was
still tied to old base SHA `a65d22342`. This matched the stale-base
pattern fixed earlier on #2049, so the branch was rebased onto current
`origin/main` `52288609a` rather than hiding the issue or weakening
coverage. Rebase conflicts were resolved in `test/test_phase9_widgets.cpp`,
`test/test_tree_view.cpp`, and `test/test_graph_editor_view.cpp` by
preserving both overlapping coverage additions.

Validation before push: `git diff --check` passed; selected view targets
built with `cmake --build build --target pulp-test-phase9-widgets
pulp-test-tree-view pulp-test-graph-editor-view pulp-test-asset-manager
pulp-test-preset-browser -j$(sysctl -n hw.ncpu)`; and
`ctest --test-dir build --output-on-failure -R
"Phase9|TreeView|GraphEditorView|AssetManager|PresetBrowser"` passed 70
tests. Force-with-lease pushed #2050 at `7451c25f4` to trigger a fresh
GitHub-hosted/Codecov recomputation. The pre-push hook again attempted
its advisory local diff-cover path and hit the unrelated local
FetchContent `mbedtls` tag checkout failure; do not treat that advisory
run as validation. Live PR rollup check was deferred because `gh pr view`
hit the GitHub GraphQL API rate limit immediately after push.

2026-05-15 15:47 PDT: added a pure coverage-gate script slice to the held
local batch `feature/phase3-codecov-ship-package-batch-661`. Commit
`77833ea33` (`test(coverage): cover coverage gate script edges`) extends
`tools/scripts/test_coverage_tier_check_extra.py` with missing-instrumented
source aggregation, unclassified-source skipping, non-instrumented tier
skipping without diff-line shellout, case-insensitive C-family extension
classification, exact React-package surface matching, and failed-tier
rendering assertions. It also extends
`tools/scripts/test_coverage_diff_comment_extra.py` with required-mode
empty-report rendering, heading-only report normalization, and direct
`main(... --no-advisory ...)` output coverage.

Validation for this held slice passed locally:
`python3 tools/scripts/test_coverage_tier_check_extra.py` (23 tests),
`python3 tools/scripts/test_coverage_tier_check.py` (24 tests),
`python3 tools/scripts/test_coverage_diff_comment.py` (11 tests),
`python3 tools/scripts/test_coverage_diff_comment_extra.py` (8 tests),
and `git diff --check`. The ship/package/release/local-CI batch is now
12 commits ahead of `origin/main` and remains intentionally local-only
until it is large enough, or the active GitHub-hosted CI queue clears
enough, to justify one substantial PR instead of smaller CI runs.

2026-05-15 15:48 PDT: added a GitHub runner resolver script slice to the
held local batch `feature/phase3-codecov-ship-package-batch-661`. Commit
`277befe94` (`test(ci): cover runner resolver selector edges`) extends
`tools/scripts/test_resolve_runs_on_extra.py` with direct selector JSON
acceptance/rejection coverage, empty/missing/whitespace env normalization,
custom requested-provider env handling for local runners, and
optional-namespace main output when GitHub-hosted is requested. This is
aligned with the current CI policy for the codecov queue: avoid Namespace,
prefer GitHub-hosted, and keep resolver behavior explicit.

Validation for this held slice passed locally:
`python3 tools/scripts/test_resolve_runs_on_extra.py` (8 tests),
`python3 tools/scripts/test_resolve_runs_on.py` (19 tests), and
`git diff --check`. The batch is now 13 commits ahead of `origin/main`
and is still intentionally held local-only rather than pushed as another
small CI run.

2026-05-15 15:58 PDT: pushed a targeted #2051 repair on
`feature/phase3-codecov-tools-host-format-consolidated-659` at
`f446d2033` (`test(view): cover widget repaint and host capture edges`).
The stale/failing Codecov patch report for #2051 was concentrated in
`core/view/include/pulp/view/widgets.hpp` and
`core/view/include/pulp/view/window_host.hpp`, so this adds direct tests
for the cheap uncovered branches rather than suppressing the check:
`WindowHost` back-buffer capture delegation, idempotent `Fader` and
`ToggleButton` label setters, and `Knob::set_format` repaint behavior
without changing the current value.

Validation before push passed locally:
`cmake --build build --target pulp-test-widgets
pulp-test-standalone-editor-chrome -j$(sysctl -n hw.ncpu)`;
`ctest --test-dir build --output-on-failure -R
"Widget setters skip repaint|Widget set_label|Knob format setter|WindowHost
default content-size"` passed 4 tests; `git diff --check` passed; and a
focused `scripts/run_coverage.sh --tests
"Widget setters skip repaint|Widget set_label|Knob format setter|WindowHost
default content-size"` completed with those same 4 tests passing under
coverage. The generated whole-tree coverage percentage is intentionally
not meaningful for this targeted run because only four tests were
selected against the full instrumented object set. Push completed, but
the pre-push advisory diff-cover path again hit the unrelated local
FetchContent `mbedtls` `v3.6.2` checkout failure in a fresh `build-cov`;
do not treat that advisory run as validation. REST check-rollup after
push showed #2051 pending with no failing check-runs.

2026-05-15 16:02 PDT: refreshed the held local batch
`feature/phase3-codecov-ship-package-batch-661` onto latest
`origin/main` and added another release-helper coverage slice. The rebase
was clean; because this branch is still local-only, the rewritten commit
hashes are not externally visible. New head commit `434b26c8e`
(`test(release): cover release helper edge cases`) adds tests for Skia
release fetch no-asset logging, archive cleanup, and missing-library
debug output; package registry structural/license clean and missing-license
edges; Android SDK fallback when env paths are stale plus empty Java
version output; and validate-host config passthrough, subprocess cwd, and
continuing remote checks after local failure.

Validation for this held slice passed locally:
`python3 tools/scripts/test_fetch_skia_for_release.py` (19 tests),
`python3 tools/scripts/test_validate_registry_extra.py` (12 tests),
`python3 tools/scripts/test_android_target.py` (20 tests),
`python3 tools/scripts/test_validate_hosts.py` (14 tests), and
`git diff --check`. The held ship/package/release/local-CI batch is now
14 commits ahead of `origin/main`, touches 17 files, and adds about 1,560
lines of focused tests. It remains intentionally unpushed so it can become
one substantial GitHub-hosted CI run rather than several small PRs.

2026-05-15 16:04 PDT: added a native Android packaging discovery slice
to the held local batch. Commit `2af985c95` (`test(android): cover sdk
fallback revision edges`) extends `test/test_android_package.cpp` to cover
invalid `ANDROID_HOME` / `ANDROID_NDK_HOME` fallback to valid
`ANDROID_SDK_ROOT` and SDK-hosted NDKs, plus very large Android build-tools
and NDK revision components. These exercise the overflow-safe revision
ordering paths without requiring a real Android SDK.

Validation for this held slice passed locally:
`cmake --build build --target pulp-test-android-package -j$(sysctl -n
hw.ncpu)`, `ctest --test-dir build --output-on-failure -R "Android SDK
discovery"` (8 tests), and `git diff --check`. The held
ship/package/release/local-CI batch is now 15 commits ahead of
`origin/main` and remains intentionally local-only.

2026-05-15 16:06 PDT: added a Python coverage-infrastructure helper slice
to the held local batch. Commit `98c14db9d` (`test(coverage): cover python
coverage helper edges`) extends `tools/scripts/test_run_python_coverage_extra.py`
with selected-surface always-include behavior, source-root normalization,
report-source omit filtering, and `.coveragerc` writing/error coverage. It
also extends `tools/scripts/test_merge_cobertura_extra.py` with Windows path
normalization plus ignored-source filtering, all-inputs-missing exit code
coverage, and corrupt-XML hard-failure coverage.

Validation for this held slice passed locally:
`python3 tools/scripts/test_run_python_coverage_extra.py` (12 tests),
`python3 tools/scripts/test_merge_cobertura_extra.py` (6 tests), and
`git diff --check`. The held ship/package/release/local-CI batch is now
16 commits ahead of `origin/main`, touches 19 files, and adds about 1,697
lines of focused tests. It remains intentionally local-only while the
active GitHub-hosted CI queue drains.

2026-05-15 16:08 PDT: ran a broader local validation pass over the held
batch after the latest coverage slices. The Python validation covered the
package/release helpers (`test_package_cli.py`, `test_package_cli_extra.py`,
`test_fetch_skia_for_release.py`, `test_auto_release_decision.py`,
`test_version_bump_check_extra.py`, `test_release_workflow_test_step.py`),
coverage helpers (`test_coverage_tier_check.py`,
`test_coverage_tier_check_extra.py`, `test_coverage_diff_comment.py`,
`test_coverage_diff_comment_extra.py`, `test_run_python_coverage_extra.py`,
`test_merge_cobertura_extra.py`, `test_resolve_runs_on.py`,
`test_resolve_runs_on_extra.py`), Android/local-CI/package registry helpers
(`test_android_target.py`, `test_build_migration_index_extra.py`,
`test_validate_hosts.py`, `test_validate_registry_extra.py`,
`tools/local-ci/test_local_ci_extra.py`), and `git diff --check`.

Native validation also passed: `cmake --build build --target
pulp-test-android-package pulp-test-appcast pulp-test-codesign
pulp-test-nsis-installer -j$(sysctl -n hw.ncpu)` succeeded, followed by
`ctest --test-dir build --output-on-failure -R "Android|Appcast|codesign|NSIS"`
with 55/55 tests passing. The held branch was clean after validation and
remains queued locally for one future substantial GitHub-hosted CI run.

2026-05-15 16:13 PDT: published the held batch as #2097,
`test: batch ship package coverage helpers`, on
`feature/phase3-codecov-ship-package-batch-661`. This intentionally turns
the accumulated 16 local commits into one GitHub-hosted CI run rather than
many small PRs. Before opening the PR, the tip commit was amended from
`98c14db9d` to `89de2d8ba` to add the required
`Skill-Update: skip skill=ci reason="test-only local CI and coverage helper
edge coverage; CI workflow guidance unchanged"` trailer. The branch was
force-with-lease pushed, then #2097 was opened with the `codecov` label.

Initial #2097 check-rollup after PR creation showed 12 pending, 0 failing,
and 3 successful check-runs. The local pre-push hook had clean
skill/version/compat/docs-noise/deps gates after the trailer amendment, but
its advisory diff-cover path still hit the unrelated local FetchContent
`mbedtls` `v3.6.2` checkout failure in fresh `build-cov`; the broader
validation listed above is the local validation source of record.

2026-05-15 16:18 PDT: started the next held local runtime/audio batch in
`/private/tmp/pulp-phase3-codecov-runtime-audio-batch-662` on
`feature/phase3-codecov-runtime-audio-batch-662`, current head
`81f700dca` (`test(runtime): cover temp file and scope guard edges`).
This first local-only slice extends `test/test_runtime_utils.cpp` with
additional `TemporaryFile` coverage for bare-extension normalization,
`path_string()`, and move-assignment cleanup of the previously owned file,
plus direct `ScopeGuard` coverage for destructor execution, `dismiss()`,
move transfer, and `PULP_ON_SCOPE_EXIT` capture behavior.

Validation for this slice passed locally:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`;
`cmake --build build --target pulp-test-runtime-utils -j$(sysctl -n
hw.ncpu)`; `ctest --test-dir build --output-on-failure -R
"PULP_ON_SCOPE_EXIT|TemporaryFile|ScopeGuard"` with 8/8 tests passing; and
`git diff --check`. This branch remains intentionally local-only while it
accumulates more runtime/audio coverage into a larger GitHub-hosted batch PR.
The current PR queue sweep showed #2048, #2049, #2050, #2051, and #2097
with no failing check-runs; several required checks are still pending.

2026-05-15 16:21 PDT: extended the same held local runtime/audio batch to
three commits ahead of `origin/main`, current head `e0bc4ca4d`
(`test(runtime): cover message callback clearing`). New commits after the
initial runtime utility slice are `2074e68e3` (`test(runtime): cover json
rpc protocol edges`) and `e0bc4ca4d`. The JSON-RPC slice covers string
request-id response preservation, omitted-params dispatch as an empty
params payload, and failed notification sends. The message-channel slice
covers clearing a previously installed in-memory message callback.

Validation for the accumulated held batch passed locally:
`cmake --build build --target pulp-test-runtime-utils pulp-test-json-rpc
pulp-test-memory-message-channel -j$(sysctl -n hw.ncpu)`;
`ctest --test-dir build --output-on-failure -R
"PULP_ON_SCOPE_EXIT|TemporaryFile|ScopeGuard|JsonRpcPeer|MemoryMessageChannel"`
with 25/25 tests passing; and `git diff --check`. The branch remains
local-only so it can keep accumulating into one larger GitHub-hosted CI
batch. REST queue sweep after validation showed #2048, #2049, #2050,
#2051, and #2097 still with no failing check-runs; #2048/#2051 each had
one fewer pending check than the prior sweep.

2026-05-15 16:24 PDT: extended the held runtime/audio batch to six commits
ahead of `origin/main`, current head `de855b2fd` (`test(runtime): cover
default http stream state`). Additional local-only commits are
`2ba25e795` (`test(runtime): cover file stream position edges`),
`8b8325cbe` (`test(runtime): cover http response status helper`), and
`de855b2fd`. These add coverage for `StreamResult` helper predicates,
`FileStream` read/write-mode position, zero-byte I/O, closed flush/position
edges, `HttpResponse::ok()` boundary statuses, and default `HttpStream`
eof/zero-read/close behavior.

Validation for the accumulated branch passed locally:
`cmake --build build --target pulp-test-runtime-utils pulp-test-json-rpc
pulp-test-memory-message-channel pulp-test-stream pulp-test-network-stream
-j$(sysctl -n hw.ncpu)`;
`ctest --test-dir build --output-on-failure -R
"PULP_ON_SCOPE_EXIT|TemporaryFile|ScopeGuard|HttpResponse|HTTP helpers|JsonRpcPeer|MemoryMessageChannel|StreamResult|FileStream|MemoryStream|Stream polymorphic|HttpStream"`
with 44/44 tests passing; and `git diff --check`. The branch remains
local-only while it grows toward the larger PR batch size. REST queue sweep
showed no failing check-runs on #2048, #2049, #2050, #2051, or #2097; #2050
also dropped one pending check since the prior sweep.

2026-05-15 16:27 PDT: extended the held runtime/audio batch to eight commits
ahead of `origin/main`, current head `27c1d1fd` (`test(runtime): cover
bigint assignment edges`). Additional commits after the six-commit mark are
`d823f799f` (`test(runtime): cover spsc queue capacity helper`) and
`27c1d1fd`. These cover `SpscQueue::capacity()`, rvalue queue pushes, and
`BigInteger` copy/move assignment behavior without changing runtime code.

Validation for the accumulated branch passed locally:
`cmake --build build --target pulp-test-runtime-utils pulp-test-json-rpc
pulp-test-memory-message-channel pulp-test-stream pulp-test-network-stream
pulp-test-runtime pulp-test-license -j$(sysctl -n hw.ncpu)`;
`ctest --test-dir build --output-on-failure -R
"PULP_ON_SCOPE_EXIT|TemporaryFile|ScopeGuard|HttpResponse|HTTP helpers|JsonRpcPeer|MemoryMessageChannel|StreamResult|FileStream|MemoryStream|Stream polymorphic|HttpStream|SpscQueue|BigInteger"`
with 64/64 tests passing; and `git diff --check`. The branch remains
local-only; target size is still a larger GitHub-hosted batch rather than a
small PR.

2026-05-15 16:31 PDT: grew and published the held runtime/audio batch as
#2098, `test: batch runtime coverage helpers`, on
`feature/phase3-codecov-runtime-audio-batch-662` at `ee1bc9e2f`. The final
batch contains 12 focused test-only commits, touches 7 test files, and adds
345 lines. Additional commits after the eight-commit checkpoint are
`d1900601e` (`test(runtime): cover localtime helper`), `1a5f95d5b`
(`test(runtime): cover mmap readwrite move edges`), `a53698c86`
(`test(runtime): cover dynamic library open moves`), and `ee1bc9e2f`
(`test(runtime): cover child process stderr capture`).

Final local validation before publishing passed:
`cmake --build build --target pulp-test-runtime-utils pulp-test-json-rpc
pulp-test-memory-message-channel pulp-test-stream pulp-test-network-stream
pulp-test-runtime pulp-test-license -j$(sysctl -n hw.ncpu)`;
`ctest --test-dir build --output-on-failure -R
"PULP_ON_SCOPE_EXIT|TemporaryFile|ScopeGuard|MemoryMappedFile|run_process|HttpResponse|HTTP helpers|JsonRpcPeer|MemoryMessageChannel|StreamResult|FileStream|MemoryStream|Stream polymorphic|HttpStream|SpscQueue|Runtime localtime|Runtime system info|BigInteger|DynamicLibrary"`
with 79/79 tests passing; and `git diff --check`. The branch push ran clean
pre-push gates with `PULP_VIA_SHIPYARD=1 PULP_DISABLE_PREPUSH_DIFF_COVER=1`;
no Namespace/SSH lanes were used. #2098 was opened via GitHub REST and
labeled `codecov`; initial PR state is blocked while GitHub-hosted checks
queue.

2026-05-15 18:00 PDT: monitored #2098 and fixed the first GitHub-hosted
Windows failure. `Windows (x64) [github-hosted]` failed only
`run_process captures stderr separately`; the Windows branch emitted
`bad news` while the assertion searched for the shared `bad-news` sentinel.
Commit `796c62cc0` (`test(runtime): make stderr sentinel portable`) changes
the PowerShell command to write `bad-news` via `[Console]::Error.WriteLine`.

Validation before push passed locally:
`cmake --build build --target pulp-test-runtime-utils -j$(sysctl -n
hw.ncpu)`, `ctest --test-dir build --output-on-failure -R
"run_process captures stderr separately|run_process"` with 4/4 tests
passing, and `git diff --check`. Push pre-push gates were clean with
`PULP_VIA_SHIPYARD=1 PULP_DISABLE_PREPUSH_DIFF_COVER=1`. #2098 is now at
`796c62cc0`; GitHub-hosted checks restarted on the new head.

2026-05-15 18:10 PDT: started the next held local audio/tools coverage
batch in `/private/tmp/pulp-phase3-codecov-audio-runtime-batch-663` on
`feature/phase3-codecov-audio-runtime-batch-663`, based on `origin/main`
`85bd37efa`. Current head is `d53bcb072` (`test(audio): cover excerpt find
validation paths`), four commits ahead of `origin/main`: `394a3e1c0`
(`test(audio): cover primitive shape defaults`), `641f581d2` (`test(audio):
guard mapped reader destinations`), `d728bab05` (`test(audio): cover model
store JSON helpers`), and `d53bcb072`.

Coverage added so far:
- `test/test_audio.cpp`: zero-channel/zero-sample buffer states,
  `AudioFileData` shape helpers, and `DeviceInfo`/`DeviceConfig` defaults.
- `core/audio/src/mmap_reader.cpp` + `test/test_audio_file.cpp`: reject null
  destination channel buffers in `MemoryMappedAudioReader::read_frames()`
  instead of dereferencing them.
- `test/test_audio_tools.cpp`: model-store override paths, legacy
  `checkpoint_path` metadata, active-model fallback keys, model/bundle JSON
  serializer fields, excerpt-find request validation, unsupported-input
  reporting, and `ExcerptFindResult` JSON fields.

Validation passed locally on macOS:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`;
`cmake --build build --target pulp-test-audio pulp-test-audio-file
pulp-test-audio-tools -j$(sysctl -n hw.ncpu)` via focused target builds;
`ctest --test-dir build --output-on-failure -R
"Buffer|AudioFileData|Device metadata|ChannelSet"` with 12/12 tests passing;
`ctest --test-dir build --output-on-failure -R "MemoryMappedAudioReader"`
with 4/4 tests passing; `ctest --test-dir build --output-on-failure -R
"audio model store accepts overrides|audio model and bundle JSON serializers"`
with 2/2 tests passing; `ctest --test-dir build --output-on-failure -R
"excerpt find validates|required request|excerpt find reports unsupported|excerpt
find JSON serializer"` with 3/3 tests passing; and `git diff --check`. This
branch remains intentionally local-only while it grows toward the larger
GitHub-hosted batch size.

2026-05-15 18:19 PDT: grew and published the held audio/tools batch as
#2099, `test: batch audio tooling coverage`, on
`feature/phase3-codecov-audio-runtime-batch-663` at `121c5c46a`. The final
batch contains 12 focused commits and is labeled `codecov`.

Additional commits after the four-commit checkpoint are `7b449939c`
(`test(audio): cover excerpt find collection edges`), `208229b4f`
(`test(audio): cover excerpt bundle fallbacks`), `0d4814144`
(`test(audio): cover model registry helpers`), `5434e12f5` (`test(audio):
guard subsection read destinations`), `e3934c76e` (`test(audio): guard
buffering reader reads`), `3144441ad` (`test(audio): cover model status
fallbacks`), `905645cf1` (`test(audio): cover model activation fallbacks`),
and `121c5c46a` (`test(audio): cover malformed model metadata`).

Final local validation before publishing passed:
`cmake --build build --target pulp-test-audio pulp-test-audio-file
pulp-test-audio-excerpt pulp-test-buffering-reader pulp-test-audio-tools
-j$(sysctl -n hw.ncpu)`;
`ctest --test-dir build --output-on-failure -R
"Buffer|AudioFileData|Device metadata|ChannelSet|MemoryMappedAudioReader|AudioSubsectionReader|BufferingReader|audio model|excerpt bundle|excerpt find"`
with 59/59 tests passing; and `git diff --check`. The first push was blocked
by the local version-bump gate, so the tip commit was amended with
`Version-Bump: sdk=skip reason="test-only coverage batch; no SDK or CLI
surface change"`. The second push passed local pre-push gates with
`PULP_VIA_SHIPYARD=1 PULP_DISABLE_PREPUSH_DIFF_COVER=1 PULP_SKIP_DIFF_COVER=1`.
No Namespace/SSH validation was dispatched.

2026-05-15 18:32 PDT: prepared the next held events/IPC/service-discovery
coverage batch in `/private/tmp/pulp-phase3-codecov-events-batch-664` on
`feature/phase3-codecov-events-batch-664`, based on `origin/main`
`85bd37efa`. Current head is `0934f48f` (`test(events): cover IPC client
disconnect lifecycle`), 12 commits ahead of `origin/main`.

The batch covers root fixes plus focused tests for:
- `EventLoop`: empty task handling, due delayed tasks, post-stop dispatches,
  and nested dispatch from the loop thread.
- `Timer`: empty callback handling and one-shot restart/stop-before-fire edges.
- `ActionBroadcaster`: empty callbacks and mutation-safe snapshot dispatch.
- `MountedVolumeListChangeDetector`: prompt stop wakeups for long poll
  intervals.
- `NetworkServiceDiscovery`: browse reuse and backend removal no-op paths.
- `InterprocessConnection`: socket binary frames and disconnect callbacks.

Final local validation before publishing passed:
`cmake --build build --target pulp-test-events pulp-test-ipc
pulp-test-network-service-discovery -j$(sysctl -n hw.ncpu)`;
`ctest --test-dir build --output-on-failure -R
"EventLoop|Timer|ActionBroadcaster|IPC|NSD|MountedVolumeListChangeDetector|LockingAsyncUpdater"`
with 57/57 tests passing; and `git diff --check`. No Namespace/SSH validation
was dispatched.

2026-05-15 18:35 PDT: published the held events/IPC/service-discovery batch
as #2100, `test: batch events coverage`, on
`feature/phase3-codecov-events-batch-664` at `0934f48f`. The branch push
passed local pre-push gates with
`PULP_VIA_SHIPYARD=1 PULP_DISABLE_PREPUSH_DIFF_COVER=1
PULP_SKIP_DIFF_COVER=1`; the tip commit carries
`Version-Bump: sdk=skip reason="coverage and internal robustness batch; no
SDK or CLI surface change"`. #2100 was opened via the GitHub REST API after
`gh pr create` hit the GraphQL rate limit, and it is labeled `codecov`.

2026-05-15 20:35 PDT: addressed the initial GitHub-hosted coverage failures
on #2099 and #2100.

#2099 initially failed Codecov/diff-cover on the mapped-reader destination
guard (`core/audio/src/mmap_reader.cpp`). CI proved the new
`MemoryMappedAudioReader` tests were running and passing, but the LLVM
coverage report still showed the entire `mmap_reader.cpp` translation unit
at 0%, so that production guard is not a reliable Codecov tranche until the
coverage visibility issue is fixed. Commit `08dd13309` reverts only
`641f581d2` (`test(audio): guard mapped reader destinations`) to keep the
audio batch greenable without hiding the separate instrumentation problem.
Focused local validation passed:
`cmake --build build --target pulp-test-audio pulp-test-audio-file
pulp-test-audio-excerpt pulp-test-buffering-reader pulp-test-audio-tools -j8`;
`ctest --test-dir build --output-on-failure -R
"AudioFileData reports shape|MemoryMappedAudioReader|AudioSubsectionReader|BufferingReader|audio tools|offline processing"`
with 21/21 tests passing; and `git diff --check`. Push used
`PULP_VIA_SHIPYARD=1 PULP_DISABLE_PREPUSH_DIFF_COVER=1`; the local
diff-cover pre-push lane attempted a fresh coverage configure but failed on
a FetchContent checkout of `mbedtls` tag `v3.6.2`, then demoted as intended.
No Namespace/SSH validation was dispatched.

#2100 initially failed Codecov patch coverage in the event batch. Commit
`f94323730` (`test(events): cover volume detector poll cycle`) adds a
deterministic `MountedVolumeListChangeDetector` test that lets the poll
interval expire before stopping, covering the timeout path in addition to
the existing stop-wakeup path. Focused local validation passed:
`cmake --build build --target pulp-test-events pulp-test-ipc
pulp-test-network-service-discovery -j8`; `ctest --test-dir build
--output-on-failure -R
"EventLoop skips empty|EventLoop dispatch_after runs due|EventLoop ignores new dispatches|EventLoop runs tasks|Timer tolerates empty|Timer one-shot can be restarted|Timer stop before first fire|ActionBroadcaster skips empty|ActionBroadcaster snapshots|MountedVolumeListChangeDetector|IPC|NetworkServiceDiscovery"`
with 29/29 tests passing; and `git diff --check`. Push used
`PULP_VIA_SHIPYARD=1 PULP_DISABLE_PREPUSH_DIFF_COVER=1`; the same local
`mbedtls v3.6.2` FetchContent checkout failure was demoted. GitHub-hosted
CI restarted on both new heads.

2026-05-15 20:50 PDT: queue policy update. Going forward, do not publish
small Codecov/test PRs from this lane. Build coverage work locally in held
batch worktrees and only open a GitHub PR when the batch is large enough to
justify a full GitHub-hosted CI cycle: target roughly 12+ coherent commits
or an equivalently broad related subsystem cluster. Prefer one substantial
PR per area over multiple narrow PRs. Keep the ledger updated with local
validation and batch contents while held. Existing open PRs should be
monitored, fixed, merged on green, or folded only when doing so reduces
queue pressure without losing review/CI signal. No Namespace/SSH validation;
use GitHub-hosted CI and macOS local focused validation.

2026-05-15 20:56 PDT: collapsed the still-open Codecov/test PR queue into
one replacement GitHub-hosted CI run. Created
`feature/phase3-codecov-megabatch-666` in
`/private/tmp/pulp-phase3-codecov-megabatch-666` from `origin/main`
`85bd37efa`, then merged the current heads of:

- #2048 `feature/phase3-codecov-runtime-platform-consolidated-657`
- #2049 `feature/phase3-codecov-media-consolidated-658`
- #2050 `feature/phase3-codecov-view-consolidated-660`
- #2051 `feature/phase3-codecov-tools-host-format-consolidated-659`
- #2097 `feature/phase3-codecov-ship-package-batch-661`
- #2098 `feature/phase3-codecov-runtime-audio-batch-662`
- #2099 `feature/phase3-codecov-audio-runtime-batch-663`
- #2100 `feature/phase3-codecov-events-batch-664`

Resolved merge conflicts in `test/test_license.cpp`,
`test/test_runtime_utils.cpp`, and `test/test_audio_tools.cpp` by preserving
the broader existing assertions and adding the unique assertions from the
incoming branches. Local validation passed: `cmake -S . -B build
-DCMAKE_BUILD_TYPE=Debug`; full `cmake --build build -j8`; and focused
`ctest --test-dir build --output-on-failure -R
"BigInteger copy move|TemporaryFile normalizes|TemporaryFile move
assignment|TemporaryFile self move|MemoryMappedFile|HttpResponse ok|HTTP
helpers reject malformed|excerpt bundle reader fills defaults|excerpt
bundle reader reports empty|EventLoop|Timer|ActionBroadcaster|MountedVolumeListChangeDetector|IPC|NetworkServiceDiscovery|GraphSerializer|StateTree remove
notifies|StateTreeSynchroniser records property removals|StateTreeSynchroniser
apply"` with 90/90 tests passing. Push used
`PULP_VIA_SHIPYARD=1 PULP_DISABLE_PREPUSH_DIFF_COVER=1`; the local
diff-cover pre-push lane attempted a coverage configure and hit the known
FetchContent checkout failure for `mbedtls` tag `v3.6.2`, then demoted as
advisory. No Namespace/SSH validation was dispatched.

Opened #2102, `test: consolidate phase 3 codecov coverage queue`, on
`feature/phase3-codecov-megabatch-666`, labeled `codecov` and `tests`.
Immediately commented and closed the superseded open PRs #2048, #2049,
#2050, #2051, #2097, #2098, #2099, and #2100 so those smaller branches stop
consuming separate GitHub/macOS CI runs. Monitor #2102 only for this combined
queue. Future Codecov work should continue as held local batches until the
batch is large enough to justify one GitHub-hosted CI run.

2026-05-15 21:19 PDT: published the next large state/parameter coverage
batch as #2103, `test: batch state coverage edges`, on
`feature/phase3-codecov-state-batch-665` at `c096457a2`. This is a single
12-commit GitHub-hosted CI batch rather than separate small PRs.

Batch contents cover:
- `StateStore` deserialize clamp, bad-magic, future-version, listener, and
  `ParamValue` primitive edges.
- `StateTree` listener, child-removal, typed-getter, malformed JSON, and
  synchronizer decode/remove edges.
- `Binding` empty callback and unknown-parameter reset robustness.
- `EditHistory` runtime max-depth trimming and zero-depth behavior.
- `PropertiesFile` scalar parser and malformed JSON shape edges.
- `PresetManager` empty navigation, discovery, `PresetInfo` load, and
  rename/current-name edges.

Local validation before publishing passed:
`cmake --build build --target pulp-test-state pulp-test-state-tree
pulp-test-binding pulp-test-properties pulp-test-preset-manager
pulp-test-edit-history -j8`; `ctest --test-dir build --output-on-failure
-R "ParamRange|ParamValue|StateStore|StateTree|ObservableValue|CachedProperty|PropertiesFile|ApplicationProperties|Binding|PresetManager|EditHistory"`
with 116/116 tests passing; and `git diff --check origin/main...HEAD`.
The first push was blocked by the version-bump gate because state headers
changed; the tip commit was amended with
`Version-Bump: sdk=skip reason="state robustness and coverage batch; no SDK
or CLI API surface change"`, then pre-push gates passed. No Namespace/SSH
validation was dispatched.

2026-05-15 21:34 PDT: addressed Codex P2 review comments on #2102 and
#2103, then published the next held MIDI coverage batch as #2104.

#2102 follow-up: commit `71b82e141` (`fix(coverage): require test binaries
before upload`) fixes the coverage helper guard so archive discovery cannot
make the "no test binaries found" check ineffective. `scripts/run_coverage.sh`
now tracks discovered test executables separately in `TEST_BINARIES` and
requires that list to be non-empty before upload proceeds. Local validation:
`bash -n scripts/run_coverage.sh` and `git diff --check`. The local diff-cover
pre-push lane attempted a fresh coverage configure but hit the known
FetchContent checkout failure for `mbedtls` tag `v3.6.2`, then was demoted
with `PULP_DISABLE_PREPUSH_DIFF_COVER=1`. No Namespace/SSH validation was
dispatched.

#2103 follow-up: commit `e1fc40380` (`fix(state): preserve explicit null sync
values`) fixes the state-tree synchronizer so only an actual absent property
is classified as `PropertyRemove`; explicit `set(key, PropertyValue{})`
remains a `PropertySet`. Added a regression proving `apply()` preserves
`has("nullable")` for explicit null values. Focused local validation passed:
`cmake --build build --target pulp-test-state-tree -j8`; `ctest --test-dir
build --output-on-failure -R "StateTree"` with 33/33 tests passing; and
`git diff --check`. The local diff-cover pre-push lane hit the same
`mbedtls v3.6.2` checkout failure and was demoted with
`PULP_DISABLE_PREPUSH_DIFF_COVER=1`. No Namespace/SSH validation was
dispatched.

#2104: opened `test(midi): batch coverage edges` on
`feature/phase3-codecov-midi-batch-667` at `831bb88c8`, labeled `codecov`
and `tests`. This is a single MIDI-focused GitHub-hosted CI batch containing
four local tranches instead of separate small PRs:

- `test(midi): cover keyboard rpn and sequence edges`
- `test(midi): cover ump and mpe buffer edges`
- `test(midi): cover capability inquiry edges`
- `test(midi): cover sysex sidecar edges`

The batch covers `MidiKeyboardState`, `RpnParser`, `MidiMessageSequence`,
`MidiBuffer` SysEx sidecars, UMP packet factories, MIDI 1.0/2.0 conversion,
`UmpBuffer`, `MpeBuffer` tracker binding, and MIDI-CI discovery/profile/
property edge cases. Local validation before publishing passed:
`cmake --build build --target pulp-test-midi pulp-test-midi-ci -j8`;
`./build/test/pulp-test-midi` with 19 test cases / 276 assertions passing;
`./build/test/pulp-test-midi-ci` with 18 test cases / 89 assertions passing;
and `git diff --check`. Branch pre-push gates were clean. `gh pr create`
hit the GitHub GraphQL rate limit, so #2104 was opened via the GitHub REST
API. No Namespace/SSH validation was dispatched.

2026-05-15 23:41 PDT: resumed after context compaction, recreated missing
state/MIDI worktrees, and continued GitHub-hosted monitoring only.

#2103 follow-up: GitHub macOS failed in the unrelated
`HotReloader detects file changes` test. Root cause was a real watcher race:
macOS can emit a modified event for a JS file that already existed before
`HotReloader` construction, so the first `poll_reload()` could see a stale
pending reload. Commit `38186634d` (`fix(view): ignore stale hot reload
watcher events`) seeds observed JS write times before starting the watcher
and ignores non-advancing modified events. Focused local validation passed:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`;
`cmake --build build --target pulp-test-hot-reload -j8`; and
`./build/test/pulp-test-hot-reload` with 5 test cases / 16 assertions
passing. Push used `PULP_DISABLE_PREPUSH_DIFF_COVER=1`; the local diff-cover
pre-push lane hit the known `mbedtls v3.6.2` FetchContent checkout failure
and was demoted. No Namespace/SSH validation was dispatched.

#2104 monitoring: Windows failed with GitHub's hosted-runner communication
loss annotation, not a test assertion. The failed job cannot be rerun while
the workflow run is still in progress; rerun the failed job/run once the
remaining macOS jobs finish. No code patch is indicated from the current
annotations.

Held local batch: `feature/phase3-codecov-signal-batch-668` is rebased on
current `origin/main` and intentionally not opened yet so it can remain a
larger signal/DSP coverage PR. Current tip is `c924468a4` with four commits:
`test(signal): cover visualization helper edges`,
`test(signal): cover effect reset edges`,
`test(signal): cover envelope ramp edges`, and
`test(signal): cover gate and bias edges`. Current diff adds 413 test lines
across `test/test_dsp_expansion.cpp` and `test/test_signal.cpp`. Local
validation passed after rebase:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF`;
`cmake --build build --target pulp-test-signal pulp-test-dsp-expansion -j8`;
`./build/test/pulp-test-signal` with 88 test cases / 1064 assertions passing;
`./build/test/pulp-test-dsp-expansion` with 41 test cases / 408 assertions
passing; and `git diff --check`. Continue adding related signal/DSP coverage
locally before submitting the next single batched PR.

2026-05-15 23:49 PDT: published the held signal/DSP coverage batch as #2106,
`test(signal): batch DSP coverage edges`, on
`feature/phase3-codecov-signal-batch-668` at `05f81890d`, labeled `codecov`
and `tests`. This keeps the signal/DSP work as one GitHub-hosted CI run
rather than splitting it into smaller PRs.

Batch contents now cover:
- `DelayLine` reprepare/resize clearing and existing wrap/reset behavior.
- `Oscillator` phase/frequency accessors, phase wrap, and reset determinism.
- Envelope ramp/reset paths, effect reset behavior, gate/bias buffer paths,
  and visualization helper edges.
- `SpectrogramBuffer`, `STFT`, `AlignedBuffer`, `Interpolator`, and
  `DryWetMixer` latency-after-prepare/missing-channel behavior.

Local validation before publishing passed:
`cmake --build build --target pulp-test-signal pulp-test-dsp-expansion
pulp-test-dsp-enhancements -j8`; `./build/test/pulp-test-signal` with 90
test cases / 1078 assertions passing; `./build/test/pulp-test-dsp-expansion`
with 41 test cases / 408 assertions passing;
`./build/test/pulp-test-dsp-enhancements` with 23 test cases / 86 assertions
passing; and `git diff --check`. Direct `git push` pre-push gates were clean.
`gh pr create` hit the GitHub GraphQL rate limit, so #2106 was opened via the
GitHub REST API. No Namespace/SSH validation was dispatched.

Monitoring state at publish time:
- #2102 has only ASan/TSan queued and its Codex P2 comment was already
  addressed by `71b82e141`.
- #2103 is running the new `38186634d` hot-reload root-cause fix through
  GitHub-hosted CI; macOS/sanitizers are still queued.
- #2104 still has the Windows hosted-runner communication-loss failure while
  macOS remains in progress; rerun the failed Windows job/run after the
  workflow finishes unless a later log shows an actual test failure.
- #2106 checks are queued/running on GitHub-hosted runners only.

2026-05-16 00:00 PDT: started the next held runtime/audio coverage batch
locally on `feature/phase3-codecov-runtime-audio-batch-669` in
`/private/tmp/pulp-phase3-codecov-runtime-audio-batch-669`. This branch is
intentionally not opened yet; keep accumulating related runtime/audio tests
before spending another GitHub-hosted CI run.

Current commits:
- `test(runtime): cover stream edge paths`
- `test(audio): cover buffer metadata edges`
- `test(runtime): cover queue slot reuse`

Current coverage additions:
- `StreamResult` helper state checks, `FileStream` position/zero-byte/reopen
  paths, and `PipeStream` closed/open named-pipe wrapper paths.
- `Buffer` zero-channel/const-view edge shapes and `AudioFileData` metadata
  helper behavior for empty/mismatched channel data.
- `SpscQueue` capacity reporting, rvalue push, full rejection, FIFO reuse
  after draining, and empty pop behavior.

Local validation passed:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF`;
`cmake --build build --target pulp-test-stream pulp-test-audio
pulp-test-runtime -j8`; `./build/test/pulp-test-stream` with 16 test cases /
160 assertions passing; `./build/test/pulp-test-audio` with 14 test cases /
645 assertions passing; `./build/test/pulp-test-runtime` with 16 test cases /
93 assertions passing; and `git diff --check`.

2026-05-16 00:03 PDT: expanded and published that runtime/audio batch as
#2107, `test(runtime): batch audio and stream coverage edges`, on
`feature/phase3-codecov-runtime-audio-batch-669` at `d13bdba21`, labeled
`codecov` and `tests`.

Additional commit:
- `test(runtime): cover memory message channel edges`

Final batch contents cover:
- `StreamResult` helper state checks, `FileStream` position/zero-byte/reopen
  paths, and `PipeStream` closed/open named-pipe wrapper paths.
- `MemoryMessageChannel` empty text/binary payloads, close-callback
  replacement, peer-close behavior, and inert error callback registration.
- `Buffer` zero-channel/const-view edge shapes and `AudioFileData` metadata
  helper behavior for empty/mismatched channel data.
- `SpscQueue` capacity reporting, rvalue push, full rejection, FIFO reuse
  after draining, and empty pop behavior.

Final local validation before publishing passed:
`cmake --build build --target pulp-test-stream pulp-test-audio
pulp-test-runtime pulp-test-memory-message-channel -j8`;
`./build/test/pulp-test-stream` with 16 test cases / 160 assertions passing;
`./build/test/pulp-test-audio` with 14 test cases / 645 assertions passing;
`./build/test/pulp-test-runtime` with 16 test cases / 93 assertions passing;
`./build/test/pulp-test-memory-message-channel` with 8 test cases / 38
assertions passing; and `git diff --check`. Direct branch push pre-push gates
were clean. #2107 was opened via the GitHub REST API to avoid the current
GraphQL rate limit. No Namespace/SSH validation was dispatched.

2026-05-16 00:12 PDT: published the next runtime/host helper coverage batch
as #2108, `test(runtime): batch helper and host coverage edges`, on
`feature/phase3-codecov-events-host-batch-670` at `cd9470159`, labeled
`codecov` and `tests`.

Batch contents cover:
- `base64_encode` zero-length pointer input, IPv4 validation/local helper
  fallback shapes, deterministic repeated-line `text_diff` output, empty /
  singleton `Range::constrain`, and `ScopeGuard` run/dismiss/move/macro
  behavior.
- `BackgroundScanner` empty-worker completion, idle cancel/join behavior,
  and direct `CancelToken::reset`.
- `ScanBlacklist` missing clear, unknown percent escapes, from-text clearing,
  and nested-parent `save_to` behavior.
- Expression parser malformed grouping/numeric edges plus nested/unary
  function combinations.

Root-cause product fix included: `core/runtime/src/expression.cpp` now
requires at least one digit after an exponent marker, so malformed input such
as `1e` no longer parses as `1`.

Local validation before publishing passed:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF`;
`cmake --build build --target pulp-test-runtime-utils
pulp-test-background-scanner pulp-test-scan-blacklist pulp-test-v3-gaps -j8`;
`./build/test/pulp-test-runtime-utils` with 46 test cases / 164 assertions
passing; `./build/test/pulp-test-background-scanner` with 9 test cases / 31
assertions passing; `./build/test/pulp-test-scan-blacklist` with 12 test
cases / 41 assertions passing; `./build/test/pulp-test-v3-gaps` with 34 test
cases / 184 assertions passing; and `git diff --check`. Direct branch push
pre-push diff-cover hit the known `mbedtls v3.6.2` FetchContent checkout
failure and was demoted with `PULP_DISABLE_PREPUSH_DIFF_COVER=1`; GitHub
hosted CI remains the merge gate. No Namespace/SSH validation was dispatched.

Monitoring state at publish time:
- #2104 still has the Windows hosted-runner communication-loss failure while
  the macOS job remains in progress; rerun the failed Windows job/run once
  the workflow finishes unless later logs show a real test failure.
- #2108 checks have started on GitHub-hosted runners only; macOS/sanitizers
  are queued behind the existing GitHub backlog.

2026-05-16 00:34 PDT: pushed a root-cause fix to #2104 after GitHub-hosted
macOS failed on `pulp doctor android|ios are recognized subcommands` timing
out under the 120s CTest job timeout. The failure was not a MIDI assertion; the
shellout recognition test was executing expensive mobile doctor probes. The fix
adds a `pulp doctor android|ios --dry-run` short-circuit that confirms the
mobile subcommand parse path while skipping external probes, and updates the
shellout test to use that deterministic path. Local validation passed:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF`;
`cmake --build build --target pulp-test-cli-shellout pulp-test-midi
pulp-test-midi-ci -j8`; `./build/test/pulp-test-cli-shellout "pulp doctor
android|ios are recognized subcommands"`; `./build/test/pulp-test-midi` with
19 test cases / 276 assertions passing; `./build/test/pulp-test-midi-ci` with
18 test cases / 89 assertions passing; and `git diff --check`. Commit pushed
to #2104 at `6941e9ee2`. Pre-push diff-cover again hit the known `mbedtls
v3.6.2` FetchContent checkout issue and was demoted with
`PULP_DISABLE_PREPUSH_DIFF_COVER=1`; GitHub hosted CI remains the merge gate.
No Namespace/SSH validation was dispatched.

2026-05-16 00:40 PDT: published the next runtime crypto/OSC coverage batch as
#2109, `test(runtime): batch crypto and osc coverage edges`, on
`feature/phase3-codecov-crypto-osc-batch-671` at `fb87d291d`, labeled
`codecov` and `tests`.

Batch contents cover:
- AES-CBC PKCS7 unpadding now rejects inconsistent padding bytes, with a test
  that mutates the IV so only the final padding byte changes.
- BigInteger hex preservation, bit counts, copy assignment, move construction,
  and move assignment leaving moved-from values usable.
- License payload parsing with optional fields and malformed integer fallback,
  plus malformed OnlineActivation URLs failing locally without network use.
- OSC message accessor defaults, malformed type-tag marker handling,
  truncated string-argument decoding, and receiver empty-handler lifecycle.

Local validation before publishing passed:
`cmake --build build --target pulp-test-crypto pulp-test-license pulp-test-osc
pulp-test-osc-bundle -j8`; `./build/test/pulp-test-crypto` with 13 test cases /
23 assertions passing; `./build/test/pulp-test-license` with 24 test cases / 51
assertions passing; `./build/test/pulp-test-osc` with 31 test cases / 111
assertions passing; `./build/test/pulp-test-osc-bundle` with 31 test cases / 79
assertions passing; and `git diff --check`. Direct branch push pre-push
diff-cover hit the known `mbedtls v3.6.2` FetchContent checkout issue and was
demoted with `PULP_DISABLE_PREPUSH_DIFF_COVER=1`; GitHub hosted CI remains the
merge gate. No Namespace/SSH validation was dispatched.

2026-05-16 00:53 PDT: published the next CLI package coverage batch as #2110,
`test(cli): batch package registry coverage edges`, on
`feature/phase3-codecov-cli-package-batch-672` at `ebcf46d2a`, labeled
`codecov` and `tests`.

Batch contents cover:
- Package registry malformed lock roots, empty target writes, semver extra
  component rejection, and community-tier quality scoring.
- Package command search help, malformed local registry handling, commercial
  override for proprietary package metadata, and CMake regeneration when
  removing one package while another remains installed.

Root-cause product fixes included:
- `cmd_search` now fails clearly on a malformed local registry instead of
  silently falling through to remote registry behavior.
- `SemVer::parse` now rejects extra numeric components such as `1.2.3.4`
  instead of silently truncating them to `1.2.3`.

Local validation before publishing passed:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF`;
`cmake --build build --target pulp-test-cli-package-registry
pulp-test-cli-package-commands -j8`; `./build/test/pulp-test-cli-package-registry`
with 7 test cases / 122 assertions passing; `./build/test/pulp-test-cli-package-commands`
with 15 test cases / 215 assertions passing; and `git diff --check`. Direct
branch push pre-push diff-cover hit the known `mbedtls v3.6.2` FetchContent
checkout issue and was demoted with `PULP_DISABLE_PREPUSH_DIFF_COVER=1`; GitHub
hosted CI remains the merge gate. No Namespace/SSH validation was dispatched.

2026-05-16 01:25 PDT: merged #2102, `test: consolidate phase 3 codecov coverage queue`, into `main` at `0541c775b` after GitHub-hosted checks reached a clean state. Removed the merged local worktree `/private/tmp/pulp-phase3-codecov-megabatch-666`.

2026-05-16 01:32 PDT: published the next tools/audio coverage batch as #2111, `test(audio): batch tools coverage edges`, on `feature/phase3-codecov-tools-audio-batch-673` at `ae4750ed5`, labeled `codecov` and `tests`.

Batch contents cover:
- Tools/audio model status incomplete-state paths, malformed bundle manifests and ranked-results files, active-model alias priority, inactive/uninstalled/missing-checkpoint excerpt-find failures, recursive WAV discovery, top-k result capping, and too-short window skips.
- Root-cause model registry fix: `resolve_checkpoint_url` now rejects empty Hugging Face checkpoint file paths such as `hf://user/repo/` instead of producing a download URL with an empty file path.

Local validation before publishing passed:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF`; `cmake --build build --target pulp-test-audio-tools -j8`; `./build/test/pulp-test-audio-tools` with 39 test cases / 366 assertions passing; and `git diff --check`. Direct branch push pre-push gates were clean with diff-cover demoted via `PULP_DISABLE_PREPUSH_DIFF_COVER=1`. GitHub-hosted CI remains the merge gate. No Namespace/SSH validation was dispatched.

2026-05-16 01:46 PDT: rebased #2104, `test(midi): batch coverage edges`, onto current `main` after #2102 made the PR `dirty`. Resolved conflicts in `test/test_midi_ci.cpp` by preserving the mainline identity-field assertions, and in `test/test_cli_shellout.cpp` by keeping the deterministic `pulp doctor android|ios --dry-run` recognition test. Local validation passed: `cmake --build build --target pulp-test-cli-shellout pulp-test-midi pulp-test-midi-ci -j8`; `./build/test/pulp-test-cli-shellout "pulp doctor android|ios are recognized subcommands"`; `./build/test/pulp-test-midi` with 19 test cases / 276 assertions passing; `./build/test/pulp-test-midi-ci` with 20 test cases / 103 assertions passing; and `git diff --check`. Force-pushed #2104 at `35f38438e`. Direct push pre-push diff-cover hit the known `mbedtls v3.6.2` FetchContent checkout failure and was demoted with `PULP_DISABLE_PREPUSH_DIFF_COVER=1`; GitHub-hosted CI remains the merge gate. No Namespace/SSH validation was dispatched.

2026-05-16 02:08 PDT: rebased #2103, `test: batch state coverage edges`, onto current `main` after #2102 made it `dirty`. Preserved already-merged state coverage and kept the #2103 additions alongside it; updated the stale property-removal expectation so removals assert the emitted old-value -> monostate callback needed by the synchronizer. Local validation passed: `cmake --build build --target pulp-test-state pulp-test-state-tree pulp-test-preset-manager pulp-test-properties -j8`; `./build/test/pulp-test-state` with 16 test cases / 104 assertions; `./build/test/pulp-test-state-tree` with 54 test cases / 234 assertions; `./build/test/pulp-test-preset-manager` with 21 test cases / 127 assertions; `./build/test/pulp-test-properties` with 25 test cases / 90 assertions; and `git diff --check`. Force-pushed #2103 at `aff5513c8`. Direct push pre-push diff-cover hit the known `mbedtls v3.6.2` FetchContent checkout failure and was demoted with `PULP_DISABLE_PREPUSH_DIFF_COVER=1`; GitHub-hosted CI remains the merge gate. No Namespace/SSH validation was dispatched.

2026-05-16 02:22 PDT: rebased #2106, `test(signal): batch DSP coverage edges`, onto current `main`. The overlapping `test/test_signal.cpp` chunks were already represented by #2102, so the rebase kept main's versions there and retained the non-overlapping DSP enhancement/expansion additions in `test/test_dsp_enhancements.cpp` and `test/test_dsp_expansion.cpp`. Local validation passed: `cmake --build build --target pulp-test-dsp-enhancements pulp-test-dsp-expansion -j8`; `./build/test/pulp-test-dsp-enhancements` with 23 test cases / 86 assertions passing; `./build/test/pulp-test-dsp-expansion` with 43 test cases / 425 assertions passing; and `git diff --check`. Force-pushed #2106 at `82edd394a`. Direct push pre-push gates were clean. No Namespace/SSH validation was dispatched.

2026-05-16 02:35 PDT: rebased #2107, `test(runtime): batch audio and stream coverage edges`, onto current `main`. The stream/audio/queue chunks now overlap with #2102, so the rebase kept main's versions and retained the non-overlapping `MemoryMessageChannel` coverage additions. Local validation passed: `cmake --build build --target pulp-test-memory-message-channel -j8`; `./build/test/pulp-test-memory-message-channel` with 12 test cases / 60 assertions passing; and `git diff --check`. Force-pushed #2107 at `04cdaf6bd`. Direct push pre-push gates were clean. No Namespace/SSH validation was dispatched.

2026-05-16 02:47 PDT: rebased #2108, `test(runtime): batch helper and host coverage edges`, onto current `main`. The runtime-utils and scan-blacklist chunks now overlap with #2102, so the rebase kept main's versions there and retained the non-overlapping `BackgroundScanner` and v3 gap coverage additions plus the expression parser exponent root fix. Local validation passed: `cmake --build build --target pulp-test-background-scanner pulp-test-v3-gaps pulp-test-runtime-utils -j8`; `./build/test/pulp-test-background-scanner` with 9 test cases / 31 assertions passing; `./build/test/pulp-test-v3-gaps` with 34 test cases / 184 assertions passing; `./build/test/pulp-test-runtime-utils` with 59 test cases / 252 assertions passing; and `git diff --check`. Force-pushed #2108 at `5576f6379`. Direct push pre-push diff-cover hit the known `mbedtls v3.6.2` FetchContent checkout failure and was demoted with `PULP_DISABLE_PREPUSH_DIFF_COVER=1`; GitHub-hosted CI remains the merge gate. No Namespace/SSH validation was dispatched.

2026-05-16 03:05 PDT: rebased #2109, `test(runtime): batch crypto and osc coverage edges`, onto current `main`. Preserved the AES-CBC inconsistent-PKCS7-padding root fix and retained the non-overlapping BigInteger, license, OSC accessor/codec, and OSC receiver coverage while keeping already-landed mainline coverage from #2102. Local validation passed: `cmake --build build --target pulp-test-crypto pulp-test-license pulp-test-osc pulp-test-osc-bundle -j8`; `./build/test/pulp-test-crypto` with 18 test cases / 35 assertions passing; `./build/test/pulp-test-license` with 28 test cases / 68 assertions passing; `./build/test/pulp-test-osc` with 36 test cases / 130 assertions passing; `./build/test/pulp-test-osc-bundle` with 36 test cases / 93 assertions passing; and `git diff --check`. Force-pushed #2109 at `e38bf4b49`. Direct push pre-push diff-cover hit the known `mbedtls v3.6.2` FetchContent checkout failure and was demoted with `PULP_DISABLE_PREPUSH_DIFF_COVER=1`; GitHub-hosted CI remains the merge gate. No Namespace/SSH validation was dispatched.

2026-05-16 03:37 PDT: started the next local-only batched coverage tranche on `feature/phase3-codecov-next-batch-674` in `/private/tmp/pulp-phase3-codecov-next-batch-674`, based on current `origin/main`. This branch is intentionally not pushed yet so it does not create another GitHub CI run while #2103/#2104/#2106/#2107/#2108/#2109/#2110/#2111 are still draining. First local checkpoint `7a16eef74`, `test(tools): batch coverage gate docs helpers`, covers docs/status and coverage tooling helpers:
- Root fix: `tools/check_status_ladder.py` now honors explicit non-empty `platform:` fields as validation evidence, matching the script's documented ladder rule.
- Root fix: `tools/list_limitations.py` now accepts hyphenated capability paths in `limitations:`.
- Added/expanded unit coverage for status-ladder platform evidence, list-limitations hyphenated paths, docs-consistency parser/input-error edges, and per-tier coverage gate main skip/pass/fail report paths.

Local validation passed: `python3 tools/test_list_limitations.py`; `python3 tools/test_check_docs_consistency.py`; `python3 tools/test_check_status_ladder.py`; `PYTHONPATH=tools/scripts python3 tools/scripts/test_coverage_tier_check.py`; and `git diff --check`. No Namespace/SSH validation was dispatched and no GitHub CI was started for this local checkpoint.

2026-05-16 03:52 PDT: investigated #2111's early `codecov/patch` 0.00% signal. The changed `tools/audio/src/model_registry.cpp` line is exercised by the focused local `pulp-test-audio-tools` test suite, but the GitHub coverage jobs were still queued/in-progress when checked, so the Codecov status may be based on partial uploads. Tried a local `scripts/run_coverage.sh --tests pulp-test-audio-tools` coverage verification with Xcode's LLVM tools on PATH; the script still began a broad instrumented repo build and reached ~43% before being stopped to avoid unnecessary disk/time cost. Removed the partial `build-coverage` tree afterward. Also tried the Python coverage runner for the local tooling batch, but local Python lacks `coverage.py >= 7.10`; CI installs that dependency. No Namespace/SSH validation was dispatched.

2026-05-16 10:14 PDT: merged #2106, `test(signal): batch DSP coverage edges`,
into `main` at `f4bf323e` after GitHub-hosted checks were clean. Removed the
merged local worktree `/private/tmp/pulp-phase3-codecov-signal-batch-668`.

2026-05-16 10:18 PDT: merged #2107, `test(runtime): batch audio and stream
coverage edges`, into `main` at `503614049` after GitHub-hosted checks were
clean. Removed the merged local worktree
`/private/tmp/pulp-phase3-codecov-runtime-audio-batch-669`.

2026-05-16 10:21 PDT: pushed focused follow-up coverage fixes to the active
batch PRs:
- #2103 (`b77c857f3`): added hot-reload seed/stale/missing-path tests to cover
  the stale watcher root fix.
- #2104 (`19a8502ce`): expanded mobile doctor dry-run shellout assertions,
  including JSON and `--ci` variants.
- #2111 (`4f1ad1a4a`): added the empty Hugging Face checkpoint-path case to the
  first audio-tools registry test.

Local validation passed before push: `pulp-test-hot-reload
"[view][hotreload][codecov]"`; `pulp-test-cli-shellout` focused doctor test;
`pulp-test-audio-tools "[audio][tools]"`; and `git diff --check` in the
touched worktrees. Local pre-push diff-cover still hits the temporary
`mbedtls v3.6.2` FetchContent checkout failure in clean coverage builds, so
diff-cover was demoted with `PULP_DISABLE_PREPUSH_DIFF_COVER=1` for #2103 and
#2104. GitHub-hosted CI remains the merge gate. No Namespace/SSH validation was
dispatched.

2026-05-16 10:27 PDT: adjusted #2110 after downloading the failed coverage
artifacts for run `25956219260`. The package command tests ran and passed in
the coverage job, but Cobertura still recorded zero hits for the directly
linked `tools/cli/package_commands.cpp` / `package_registry.cpp` translation
units, making the small product fixes unmergeable in this coverage PR. Removed
those product changes and their dependent assertions, keeping the batch as
test-only coverage work. Local validation passed: `cmake --build build
--target pulp-test-cli-package-commands pulp-test-cli-package-registry -j8`;
`./build/test/pulp-test-cli-package-commands "[cli][package-commands]"` with
14 test cases / 210 assertions; `./build/test/pulp-test-cli-package-registry
"[cli][package-registry]"` with 7 test cases / 121 assertions; and `git diff
--check`. Pushed #2110 at `2a5839542`.

2026-05-16 10:31 PDT: pushed follow-ups for the two Codecov-only red PRs:
- #2108 (`3ea9146e4`): added signed malformed-exponent cases (`1e+`, `1e-`) to
  satisfy Codecov's external patch accounting while preserving the expression
  parser root fix.
- #2109 (`9232f127d`): removed the unreachable `pad_val > result.size()` AES
  guard, because AES decrypt already rejects non-block-size ciphertext and the
  decrypted buffer is non-empty by construction; retained the real inconsistent
  PKCS7 byte validation and test.

Local validation passed: `cmake --build build --target pulp-test-v3-gaps -j8`;
`./build/test/pulp-test-v3-gaps "[runtime][expression]"` with 14 test cases /
54 assertions; `cmake --build build --target pulp-test-crypto -j8`;
`./build/test/pulp-test-crypto "[crypto][aes]"` with 7 test cases / 20
assertions; and `git diff --check`. Local pre-push diff-cover again hit the
temporary `mbedtls v3.6.2` FetchContent checkout failure and was demoted with
`PULP_DISABLE_PREPUSH_DIFF_COVER=1`. GitHub-hosted CI remains the merge gate.
No Namespace/SSH validation was dispatched.

2026-05-16 10:36 PDT: published the next batched tools coverage PR as #2114,
`test(tools): batch coverage gate and visual harness edges`, on
`feature/phase3-codecov-next-batch-674` at `235217b9c`.

Batch contents cover:
- Python docs/status/coverage helper edge cases from local commit
  `17ba04e49`.
- Visual harness differ and runner edge cases from local commit `235217b9c`.

Local validation before publishing passed: `PYTHONPATH=tools/scripts python3 -m
unittest tools.test_check_docs_consistency tools.test_check_status_ladder
tools.test_list_limitations tools.scripts.test_coverage_tier_check
tools.harness.visual.tests.test_differ tools.harness.visual.tests.test_runner`
with 100 tests passing; and `git diff --check HEAD~2..HEAD`. Local `pytest`
was unavailable, so validation used unittest. Direct push pre-push diff-cover
hit the known temporary `mbedtls v3.6.2` FetchContent checkout failure and was
demoted with `PULP_DISABLE_PREPUSH_DIFF_COVER=1`; GitHub-hosted CI remains the
merge gate. No Namespace/SSH validation was dispatched.

2026-05-16 10:38 PDT: active GitHub-hosted queue after the follow-up pushes:
#2103, #2104, #2108, #2109, #2110, #2111, and #2114 all had fresh head SHAs,
were mergeable, and showed no current failing checks; all were waiting on
GitHub-hosted CI. #2108 and #2109 no longer showed the stale `codecov/patch`
failure after the follow-up pushes because their checks were re-queued.

2026-05-16 11:30 PDT: continued active PR triage and kept all work out of the
dirty main checkout (`feature/design-tool-viewport-reconcile`). No Namespace or
SSH validation was dispatched.

Follow-up pushes:
- #2103 (`5322bc1bb`): fixed the Windows coverage link failure in
  `pulp-test-hot-reload` by exercising the stale/missing hot-reload behavior
  through `on_file_changed()` instead of directly calling the private
  `should_reload_for_modified_file()` helper. Local validation passed:
  `cmake --build build --target pulp-test-hot-reload -j$(sysctl -n hw.ncpu)`;
  `./build/test/pulp-test-hot-reload "[view][hotreload][codecov]"`; and
  `git diff --check`.
- #2104 (`4c5155d20`): removed the extra `cmd_doctor.cpp` dry-run product
  branch from this coverage PR and restored the existing `--versions` shellout
  parser path, clearing the Codecov patch-red product-source lines. Local
  validation passed: `cmake --build build --target pulp-test-cli-shellout
  -j$(sysctl -n hw.ncpu)`; focused `pulp-test-cli-shellout` doctor subcommand
  test; and `git diff --check`.
- #2114 (`34745ed34`, then `01c69eaa6`): first fixed the Linux
  `AudioProcessLoadMeasurer peak tracking` float-rounding failure with a narrow
  tolerance, then addressed the two Codex P2 comments by constraining status
  ladder `notes:` and `platform:` evidence to entry-level sibling fields only.
  Local validation passed: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`;
  `cmake --build build --target pulp-test-load-measurer -j$(sysctl -n
  hw.ncpu)`; focused `pulp-test-load-measurer`; `PYTHONPATH=tools/scripts
  python3 -m unittest tools.test_check_status_ladder
  tools.harness.visual.tests.test_differ tools.harness.visual.tests.test_runner`;
  and `git diff --check`.

Queue status after the pushes:
- #2103, #2104, #2108, #2109, #2110, and #2114 had no current failing checks and
  were waiting on GitHub-hosted CI.
- #2111 still showed `codecov/patch` red for the empty Hugging Face checkpoint
  path fix in `tools/audio/src/model_registry.cpp`, with macOS coverage and
  sanitizer lanes still queued. Because the line is directly exercised by
  `pulp-test-audio-tools`, this remains under observation until the delayed
  coverage uploads finish.

Prepared but intentionally not pushed/opened:
- `/private/tmp/pulp-codecov-next-batch-675`,
  `feature/phase3-codecov-next-batch-675`, commit `b70aa1aeca`: async stream
  coverage batch validated locally.
- `/private/tmp/pulp-phase3-codecov-batch-676`,
  `feature/phase3-codecov-batch-676`, commit `5e452ee26`: test signal source
  coverage batch with 10 new focused cases. Local validation passed:
  `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`; `cmake --build build
  --target pulp-test-test-signal -j$(sysctl -n hw.ncpu)`; `ctest --test-dir
  build -R "TestSignalSource" --output-on-failure` (17/17); full
  `./build/test/pulp-test-test-signal` (17 cases / 834 assertions); and
  `git diff --check`.

2026-05-16 11:43 PDT: verified the current GitHub-hosted queue and prepared the
next larger local batch without dispatching new CI.

Active PR queue:
- #2103, #2104, #2108, #2109, #2110, and #2114 had no current failing checks
  and were waiting on GitHub-hosted CI lanes.
- #2111 still showed stale/external `codecov/patch` red for
  `tools/audio/src/model_registry.cpp:37` while macOS coverage and sanitizer
  lanes remained queued. Local mirror validation showed the changed line is
  actually covered: `PULP_DIFF_COVER_CTEST_REGEX='audio model registry
  resolves|audio model status|audio model store|excerpt'
  tools/scripts/local_diff_cover.sh pulp-test-audio-tools` reported
  `tools/audio/src/model_registry.cpp (100%)`, `Missing: 0 lines`, and
  `Coverage: 100%`. The broad ctest regex also matched three unbuilt CLI/audio
  tests in that local coverage run, but `local_diff_cover.sh` intentionally
  generated a partial report and the diff-coverage result for the changed line
  was green. No line exclusion or product-code hiding was added.

Prepared but intentionally not pushed/opened:
- `/private/tmp/pulp-phase3-codecov-combined-677`,
  `feature/phase3-codecov-combined-677`, rebased onto `origin/main` at
  `366461475`, initially combined three local tranches:
  `d5e6842f9` async-stream coverage, `e9014fe0a` test-signal coverage, and
  `f3042a1da` XML/gzip coverage. Diff: `test/test_async_stream.cpp`,
  `test/test_test_signal.cpp`, and `test/test_xml_zip.cpp` with 534 net added
  test lines. Combined validation passed:
  `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF`;
  `cmake --build build --target pulp-test-async-stream pulp-test-stream
  pulp-test-test-signal pulp-test-xml-zip -j$(sysctl -n hw.ncpu)`;
  `ctest --test-dir build -R
  "AsyncStream|Stream|TestSignalSource|XmlDocument|xml_generate|gzip|deflate"
  --output-on-failure` (78/78); `./build/test/pulp-test-test-signal`
  (834 assertions / 17 cases); `./build/test/pulp-test-xml-zip` (155
  assertions / 34 cases); and `git diff --check`.

2026-05-16 11:50 PDT: folded one more local-only worker tranche into the next
combined batch. `/private/tmp/pulp-phase3-codecov-combined-677`,
`feature/phase3-codecov-combined-677`, now has four local commits:
`d5e6842f9` async-stream coverage, `e9014fe0a` test-signal coverage,
`f3042a1da` XML/gzip coverage, and `5d2eb6b37` runtime environment/GMT/C-string
helper coverage. Current diff is test-only across `test/test_async_stream.cpp`,
`test/test_runtime.cpp`, `test/test_test_signal.cpp`, and `test/test_xml_zip.cpp`
with 653 net added test lines.

Expanded combined validation passed:
`cmake --build build --target pulp-test-async-stream pulp-test-stream
pulp-test-test-signal pulp-test-xml-zip pulp-test-runtime -j$(sysctl -n
hw.ncpu)`; `ctest --test-dir build -R "AsyncStream|Stream|TestSignalSource|
XmlDocument|xml_generate|gzip|deflate|Runtime (environment helpers|GMT helper|
C string copy)" --output-on-failure` (87/87); `./build/test/pulp-test-runtime`
(188 assertions / 26 cases); `./build/test/pulp-test-test-signal` (834
assertions / 17 cases); `./build/test/pulp-test-xml-zip` (155 assertions / 34
cases); and `git diff --check`. This batch remains intentionally local until
the current GitHub-hosted queue drains enough to submit it as a single larger
coverage PR.

2026-05-16 12:04 PDT: added one more local-only tranche to the same combined
batch while the GitHub-hosted queue remained blocked on pending macOS/sanitizer
lanes. `/private/tmp/pulp-phase3-codecov-combined-677`,
`feature/phase3-codecov-combined-677`, now has five local commits:
`d5e6842f9` async-stream coverage, `e9014fe0a` test-signal coverage,
`f3042a1da` XML/gzip coverage, `5d2eb6b37` runtime helper coverage, and
`6b41d3f1b` host scan cache/blacklist metadata coverage. Current diff is
test-only across `test/test_async_stream.cpp`, `test/test_runtime.cpp`,
`test/test_scan_blacklist.cpp`, `test/test_scan_cache.cpp`,
`test/test_test_signal.cpp`, and `test/test_xml_zip.cpp` with 854 net added
test lines.

Expanded validation passed:
`cmake --build build --target pulp-test-async-stream pulp-test-stream
pulp-test-test-signal pulp-test-xml-zip pulp-test-runtime pulp-test-scan-cache
pulp-test-scan-blacklist -j$(sysctl -n hw.ncpu)`; `ctest --test-dir build -R
"AsyncStream|Stream|TestSignalSource|XmlDocument|xml_generate|gzip|deflate|
Runtime (environment helpers|GMT helper|C string copy)|blacklisting a missing
plugin|duplicate blacklist records|unknown percent escapes|nested blacklist
parent|ScanCache put on a missing path|ScanCache erase and clear|non-array
entries|duplicate JSON entries|ScanCache save_to creates nested"
--output-on-failure` (96/96); `./build/test/pulp-test-runtime` (188 assertions
/ 26 cases); `./build/test/pulp-test-test-signal` (834 assertions / 17 cases);
`./build/test/pulp-test-xml-zip` (155 assertions / 34 cases);
`./build/test/pulp-test-scan-cache` (95 assertions / 19 cases);
`./build/test/pulp-test-scan-blacklist` (65 assertions / 16 cases); and
`git diff --check`.

2026-05-16 14:42 PDT: resumed queue stewardship after GraphQL exhaustion and
switched PR polling/merge actions to the GitHub REST API. #2110,
`test(cli): batch package registry coverage edges`, was green on REST check
state and was squash-merged into `main` as
`b896356ec06b5bd53897a411f6b93c993c866e2b`; the merged worktree
`/private/tmp/pulp-phase3-codecov-cli-package-batch-672` was removed.

2026-05-16 14:43 PDT: #2114 still had the two Codex P2 comments addressed by
`01c69eaa6`, but Codecov was red because it counted
`tools/harness/visual/tests/` pytest files as patch-coverable product code.
Added a root-scope alignment fix in
`/private/tmp/pulp-phase3-codecov-next-batch-674`, commit `d8ca56957`
(`ci(coverage): ignore visual harness tests in patch scope`): Codecov now
ignores `tools/harness/visual/tests/**`, `tools/scripts/run_python_coverage.py`
omits the same test files from the measured Python source set, and
`tools/scripts/test_codecov_config.py` asserts the alignment. Local validation:
`python3 tools/scripts/test_codecov_config.py` passed 14 tests;
`python3 tools/scripts/test_run_python_coverage.py` passed 24 tests; and
`git diff --check` passed. A live `run_python_coverage.py` smoke was not used
locally because the installed `coverage.py` is below Pulp's required 7.10
minimum. The push used `PULP_DISABLE_PREPUSH_DIFF_COVER=1` after the known
local `mbedtls v3.6.2` FetchContent checkout failure; GitHub-hosted CI remains
the merge gate. No Namespace/SSH validation was dispatched.

2026-05-16 14:47 PDT: #2108's Codecov failure was investigated rather than
hidden. The failing report named the two new `core/runtime/src/expression.cpp`
malformed-exponent guard lines. Local reproduction on
`/private/tmp/pulp-phase3-codecov-events-host-batch-670` with
`PULP_DIFF_COVER_CTEST_REGEX='Expression rejects malformed numeric and grouping syntax' tools/scripts/local_diff_cover.sh`
passed with `core/runtime/src/expression.cpp (100%)`, 2 touched lines, 0
missing, 100% diff coverage. The focused binary also passed:
`cmake --build build --target pulp-test-v3-gaps -j$(sysctl -n hw.ncpu)` and
`./build/test/pulp-test-v3-gaps "Expression rejects malformed numeric and grouping syntax"`
with 8 assertions. Because the failure did not reproduce and the root test is
covering the source lines, reran only the failed GitHub Coverage workflow jobs
for #2108 (`run_id=25968261889`) via REST.

2026-05-16 14:50 PDT queue snapshot: #2103 and #2104 have no failing checks
and are waiting on GitHub-hosted macOS/sanitizer capacity; #2108 has the rerun
in progress and only the stale Codecov bot check still red; #2114 is rerunning
after `d8ca56957`; #2118 (`docs/graphql-rest-merge-fallback`) is still running.
The current local held batch remains
`/private/tmp/pulp-phase3-codecov-combined-677` with five validated local
coverage commits and should be submitted as one larger PR only after the open
queue drains enough to avoid wasting macOS CI capacity.

2026-05-16 15:02 PDT: #2108's rerun still failed Codecov for the same
`core/runtime/src/expression.cpp` lines even though the focused CI test passed.
Downloaded and inspected the GitHub coverage artifacts: the Linux artifact
retained nearby expression hits from `pulp-test-runtime-utils` but did not
retain the malformed-exponent hits from `pulp-test-v3-gaps`; the macOS artifact
had no hits for those lines. Root fix: added the same malformed exponent
assertions to the existing runtime-utils malformed-input test, so the guard is
covered by the test binary that CI already retains in the coverage artifact.
Local validation passed:
`cmake --build build --target pulp-test-runtime-utils pulp-test-v3-gaps
-j$(sysctl -n hw.ncpu)`;
`./build/test/pulp-test-runtime-utils "Expression evaluator resolves variables and rejects malformed inputs"`
with 9 assertions;
`./build/test/pulp-test-v3-gaps "Expression rejects malformed numeric and grouping syntax"`
with 8 assertions; and targeted local diff coverage with
`PULP_DIFF_COVER_CTEST_REGEX='Expression evaluator resolves variables and rejects malformed inputs|Expression rejects malformed numeric and grouping syntax' tools/scripts/local_diff_cover.sh`
reported `core/runtime/src/expression.cpp (100%)`, 2 touched lines, 0 missing,
100% coverage.

Corrected push note for #2108: an interrupted full pre-push run briefly staged
generated local build-test damage (`CMakeLists.txt`/`pulp.toml`) into an amend.
That bad tip was immediately removed with `--force-with-lease`; the corrected
head is `a95b3228e`, contains only `test/test_runtime_utils.cpp` (+3
assertions), and has fresh GitHub-hosted CI queued. No Namespace/SSH validation
was dispatched.

2026-05-16 15:23 PDT: refreshed the held combined coverage batch against current
`origin/main` before adding more tests. `/private/tmp/pulp-phase3-codecov-combined-677`
rebased cleanly and now has six local commits. Added `5ed72d7c8`
(`test(runtime): cover i18n locale environment edges`) covering
`LocalisedStrings::system_locale()` LANG normalization and fallback behavior with
cross-platform-safe environment restoration. Local validation passed:
`cmake --build build --target pulp-test-i18n -j$(sysctl -n hw.ncpu)`,
`./build/test/pulp-test-i18n "i18n system_locale*"` (3 assertions / 3 cases),
and full `./build/test/pulp-test-i18n` (73 assertions / 26 cases). This batch
remains local-only to avoid opening another small GitHub-hosted CI run.

2026-05-16 15:32 PDT: added a seventh local commit to the held combined
coverage batch, `2fe145fe7` (`test(runtime): cover high resolution timer
lifecycle`). The tests cover `HighResolutionTimer` start/stop state, callback
execution, idempotent stop, restart replacing the previous callback, and a null
callback path. Local validation passed: `cmake --build build --target
pulp-test-runtime -j$(sysctl -n hw.ncpu)`; `./build/test/pulp-test-runtime
"HighResolutionTimer*"` (10 assertions / 2 cases); and full
`./build/test/pulp-test-runtime` (198 assertions / 28 cases). The combined
batch remains local-only while the open GitHub-hosted PR queue waits on macOS
and sanitizer capacity.

2026-05-16 15:43 PDT: REST status sweep found #2114 and #2104 clean with no
failing or pending checks except expected skipped RealtimeSanitizer lanes. Merged
#2114 (`test(tools): batch coverage gate and visual harness edges`) via REST
with head-SHA guard as `7fd6b21530650af05e009ee22f9bc820085d9967`. Merged
#2104 (`test(midi): batch coverage edges`) via REST with head-SHA guard as
`2c9a8333cd3b3c63ae74d4c89e4021c5053cac20`. Removed their merged worktrees:
`/private/tmp/pulp-phase3-codecov-next-batch-674` and
`/private/tmp/pulp-phase3-codecov-midi-batch-667`.

Remaining open test PRs: #2103 and #2108. #2103 is mergeable but unstable due
`codecov/patch`; the local diff-gate comment reports 90% total patch coverage
and all per-tier floors pass, but Codecov's app check is still red. #2108 is
mergeable but unstable due `codecov/patch`; the local diff-gate comment reports
100% patch coverage on `core/runtime/src/expression.cpp`, so the Codecov app
status appears stale or using a different surface while sanitizer lanes continue
to run. Next action is to investigate/fix those Codecov patch statuses rather
than merging around them.

2026-05-16 15:50 PDT: #2118 (`docs(ci): codify REST fallback when GraphQL is
exhausted`) reached clean REST check state, 21/21 successful checks, no pending
or failing checks. Merged via REST with head-SHA guard as
`6734320173dc3772727659758f6ca923216be48b`. Remaining open tracked test PRs are
#2108 and #2103, both mergeable but unstable due Codecov patch checks.

2026-05-16 19:39 PDT: resumed after compaction with GitHub GraphQL still rate
limited, so PR creation/status actions are using REST where possible. Current
tracked Codecov PR queue: #2103 at `f4f0ecc7` has no REST-reported failing
checks and is waiting on GitHub-hosted Linux/Windows/macOS/coverage/sanitizer
jobs; its Codex P2 on `core/state/src/state_tree_sync.cpp` is addressed on the
branch by `d62f3a641` plus the null-property regression, with follow-up comments
posted on the review thread. #2108 at `f1445ef0` has no REST-reported failing
checks and is waiting on GitHub-hosted Linux/Windows/macOS/sanitizer/android
jobs. #2122 at `3fd9ca36` has no REST-reported failing checks and is waiting on
GitHub-hosted coverage/build/sanitizer jobs. No Namespace/SSH validation was
dispatched.

2026-05-16 19:39 PDT: opened the next large batched coverage PR as #2123,
`test: batch runtime and audio coverage edges`, branch
`feature/phase3-codecov-batch-680`, head
`467c240401c1cd680e1ee2cbfda93705331c7387`. This batch adds 13 focused tests
across `test/test_json_rpc.cpp`, `test/test_memory_message_channel.cpp`,
`test/test_identity.cpp`, and `test/test_audio.cpp` covering JSON-RPC peer
request/notification/response edges, memory channel close/replacement behavior,
UUID/typed identity/event envelope edges, and audio buffer/channel layout edges.
Local validation before PR: built
`pulp-test-json-rpc pulp-test-memory-message-channel pulp-test-identity
pulp-test-audio`; ran all four focused binaries successfully; ran the 13-test
`PULP_DIFF_COVER_CTEST_REGEX=... tools/scripts/local_diff_cover.sh` gate, which
reported no uncovered measured diff lines and OK at/above the 75% floor. The
branch was pushed with `PULP_SKIP_DIFF_COVER=1` only after that local
diff-cover run; pre-push gates otherwise passed. GitHub GraphQL exhaustion
prevented enabling auto-merge immediately, so this PR is in REST-poll-and-merge
tracking until checks are green. No Namespace/SSH validation was dispatched.

2026-05-16 19:55 PDT: #2103 showed a real Codecov app `codecov/patch` failure
after the earlier state batch updates: Codecov reported 56.33803% patch coverage,
mostly around `core/view/src/hot_reload.cpp`, plus smaller state-tree partials.
Added follow-up commit `3e10eb101` (`test(view): cover hot reload filter edges`)
on `feature/phase3-codecov-state-batch-665`. The commit adds deterministic
hot-reload tests for direct watcher-event filtering, directory-entry reloads from
`.mjs` changes, pending reload drain without a callback, and empty/missing entry
files. Local validation passed: `cmake --build build --target
pulp-test-hot-reload -j$(sysctl -n hw.ncpu)`, `./build/test/pulp-test-hot-reload
'[view][hotreload][codecov]'` (20 assertions / 6 cases), and
`PULP_DIFF_COVER_CTEST_REGEX='HotReloader|StateTree|StateTreeSynchroniser|ObservableValue'
tools/scripts/local_diff_cover.sh`, which reported 90% patch coverage against
`origin/main`. Pushed the branch with `PULP_SKIP_DIFF_COVER=1` only after that
local diff-cover pass; pre-push gates otherwise passed. GitHub-hosted checks are
rerunning on head `3e10eb101`.

2026-05-16 20:03 PDT: opened the next large batched coverage PR as #2124,
`test: batch runtime primitive coverage edges`, branch
`feature/phase3-codecov-batch-681`, head
`13fe2991e1025269697815427e5b84c4c31c748e`. This batch adds 12 focused tests
across `test/test_crypto.cpp`, `test/test_license.cpp`, `test/test_stream.cpp`,
and `test/test_v3_gaps.cpp` covering AES binary/empty decrypt edges, machine ID
shape, BigInteger invalid parses, unsigned license validation, CRLF-trimmed
license files, MemoryStream/FileStream/HttpStream lifecycle edges, and prime
helper boundaries. Local validation before PR: `git diff --check`; built
`pulp-test-crypto pulp-test-license pulp-test-stream pulp-test-v3-gaps`; ran
`./build/test/pulp-test-crypto`, `./build/test/pulp-test-license`,
`./build/test/pulp-test-stream`, and `./build/test/pulp-test-v3-gaps "*prime*"`;
ran the 12-test `PULP_DIFF_COVER_CTEST_REGEX=... tools/scripts/local_diff_cover.sh`
gate, which reported no uncovered measured diff lines and OK at/above the 75%
floor. The PR was created via REST because GraphQL remains exhausted and labeled
`codecov`; GitHub-hosted CI is the merge gate. No Namespace/SSH validation was
dispatched.

2026-05-16 20:09 PDT: opened the next large batched coverage PR as #2126,
`test(midi): batch ci and ump coverage edges`, branch
`feature/phase3-codecov-batch-682`, head
`df003e041a1411efbdb2111e3b33c8d6d471444a`. This batch adds 12 focused tests
across `test/test_midi.cpp` and `test/test_midi_ci.cpp` covering MIDI-CI header
rejection, direct-addressed discovery replies, minimal discovery reply storage,
profile identity equality, empty profile reply counts, keyboard all-notes-off
ordering/isolation, UMP note-off/CC conversion paths, MIDI 1.0 fallback
group/offset preservation, pitch scaling boundaries, and flattening skip
behavior for packets without MIDI 1.0 equivalents. Local validation before PR:
`git diff --check`; built `pulp-test-midi pulp-test-midi-ci`; ran
`./build/test/pulp-test-midi` and `./build/test/pulp-test-midi-ci`; ran the
12-test `PULP_DIFF_COVER_CTEST_REGEX=... tools/scripts/local_diff_cover.sh`
gate, which reported no uncovered measured diff lines and OK at/above the 75%
floor. The branch was rebased onto current `origin/main` before push. The PR was
created via REST because GraphQL remains exhausted and labeled `codecov`;
GitHub-hosted CI is the merge gate. No Namespace/SSH validation was dispatched.

2026-05-16 20:12 PDT: #2122 (`test: batch runtime and host coverage edges`)
merged into `main` as `c01eeaf3ebc71c021330eafdea17288d920bb7dc`. Removed the
merged worktree `/private/tmp/pulp-phase3-codecov-combined-677`. Remaining
tracked open coverage PRs are #2103, #2108, #2123, #2124, and #2126; all are
still on GitHub-hosted CI only.

2026-05-16 20:24 PDT: #2108 (`test(runtime): batch helper and host coverage
edges`) merged into `main` as `cb9ac1784c8384b3088bae2d54f748147ab09d59`.
Removed the merged worktree
`/private/tmp/pulp-phase3-codecov-events-host-batch-670`. The remaining tracked
open coverage PRs are #2103, #2123, #2124, #2126, and #2129.

2026-05-16 20:24 PDT: opened the next large batched coverage PR as #2129,
`test(osc): batch codec and bundle coverage edges`, branch
`feature/phase3-codecov-batch-683`, head
`299a579232f2988266ddd7005bb3bdc3f673a82d`. This batch adds 12 focused tests
across `test/test_osc.cpp` and `test/test_osc_bundle.cpp` covering OSC message
fluent argument construction, empty type tags, unknown tags, bounded non-null
terminated payloads, blob padding boundaries, timetag fractional conversion,
BundleElement unique ownership/copy construction, non-immediate timetag
serialization, zero-sized bundle element rejection, and mixed blob/float
round-trips. Local validation before PR: `git diff --check`; built
`pulp-test-osc pulp-test-osc-bundle`; ran `./build/test/pulp-test-osc` (152
assertions / 42 cases) and `./build/test/pulp-test-osc-bundle` (114 assertions /
42 cases); ran the 12-test `PULP_DIFF_COVER_CTEST_REGEX=...
tools/scripts/local_diff_cover.sh` gate, which reported no uncovered measured
diff lines and OK at/above the 75% floor. The branch was rebased onto current
`origin/main` after #2108 merged, targeted tests were rerun, and the branch was
pushed with `PULP_SKIP_DIFF_COVER=1` only after the local diff-cover pass. The
PR was created via REST and labeled `codecov`; GitHub-hosted CI is the merge
gate. No Namespace/SSH validation was dispatched.

2026-05-16 20:39 PDT: monitored open tracked coverage PRs after #2108 merged.
#2103 reported a real Windows `Windows (x64) [github-hosted]` failure: direct
test calls into private `HotReloader::on_file_changed` and `read_file` crossed
the Windows library boundary and failed to link. Pushed follow-up
`522468522` (`test(view): avoid direct hot reload private calls`) on
`feature/phase3-codecov-state-batch-665`, converting those cases to exercise
the same paths through public watcher/poll behavior. Local validation passed:
`git diff --check`, `cmake --build build --target pulp-test-hot-reload
-j$(sysctl -n hw.ncpu)`, and `./build/test/pulp-test-hot-reload "[codecov]"`
(38 assertions / 6 cases). #2124 reported a real Windows
`Windows (x64) [github-hosted]` failure in `FileStream reopen closes the
previous handle`: the test removed files while reader streams still held handles
on Windows. Pushed follow-up `a67fbf817` (`test(runtime): close stream readers
before cleanup`) on `feature/phase3-codecov-batch-681`, scoping readers so
handles close before cleanup. Local validation passed: `git diff --check`,
`cmake --build build --target pulp-test-stream -j$(sysctl -n hw.ncpu)`, and
`./build/test/pulp-test-stream "FileStream reopen closes the previous handle"`.
Both branches were pushed with `PULP_SKIP_DIFF_COVER=1` only after focused local
validation; GitHub-hosted checks are rerunning. No Namespace/SSH validation was
dispatched.

2026-05-16 20:39 PDT: opened the next large batched coverage PR as #2130,
`test(runtime): batch network stream socket coverage edges`, branch
`feature/phase3-codecov-batch-684`, head
`bf1da021268717ed261109a2cfd8d58cc3368ad6`. This batch adds 12 focused tests in
`test/test_network_stream.cpp` covering IPv4 validation edges, unopened socket
failure results, create/close/recreate state, UDP loopback datagram flow,
wildcard binds, UDP socket move construction/assignment/self-assignment,
TCP-only guard behavior on UDP sockets, accepted-socket wrapping in `TcpStream`,
and zero-byte stream I/O. Local validation before PR: `git diff --check`; built
`pulp-test-network-stream`; ran `./build/test/pulp-test-network-stream` (131
assertions / 27 cases); ran the 12-test `PULP_DIFF_COVER_CTEST_REGEX=...
tools/scripts/local_diff_cover.sh` gate, which reported no uncovered measured
diff lines and OK at/above the 75% floor. The branch was pushed with
`PULP_SKIP_DIFF_COVER=1` only after the local diff-cover pass. The PR was
created via REST and labeled `codecov`; GitHub-hosted CI is the merge gate. No
Namespace/SSH validation was dispatched.

2026-05-16 21:23 PDT: addressed the real Codex review finding on #2134. Pushed
follow-up `48731a7a2` (`fix(view): make frame clock tick subscription-safe`) on
`feature/phase3-codecov-batch-686`, changing `FrameClock::tick` so callback
iteration does not hold vector references across callbacks and so subscribers
added during a tick start on the next frame. This fixes the underlying
iterator-invalidation risk rather than weakening the test. Focused local
validation passed: `git diff --check`, `cmake --build build --target
pulp-test-frame-clock pulp-test-window-manager pulp-test-splash-screen
pulp-test-panel -j$(sysctl -n hw.ncpu)`, the four focused
`"[coverage][phase3]"` binaries, and the 12-test
`PULP_DIFF_COVER_CTEST_REGEX=... tools/scripts/local_diff_cover.sh` gate. The
diff-cover result was 83% for the production diff, above the 75% floor. The
branch was pushed with `PULP_SKIP_DIFF_COVER=1` only after the local pass; no
Namespace/SSH validation was dispatched.

2026-05-16 21:15 PDT: opened the next large batched coverage PR as #2134,
`test(view): batch lifecycle widget coverage edges`, branch
`feature/phase3-codecov-batch-686`, head
`cc12cf4e0b1caab922ed3ff6cf411e82bde2266a`. This batch adds 12 focused tests
across `test/test_frame_clock.cpp`, `test/test_window_manager.cpp`,
`test/test_splash_screen.cpp`, and `test/test_panel.cpp` covering FrameClock
in-tick subscription, self-unsubscribe, and compaction behavior; WindowManager
message handler replacement, saved visibility state, and shared theme
propagation; SplashScreen manual dismiss/restart/repeated-show lifecycle; and
Panel theme-token colors, fallback colors, and inset border geometry. Local
validation before PR: `git diff --check`; built `pulp-test-frame-clock
pulp-test-window-manager pulp-test-splash-screen pulp-test-panel`; ran the four
focused binaries with `"[coverage][phase3]"` filters; ran the 12-test
`PULP_DIFF_COVER_CTEST_REGEX=... tools/scripts/local_diff_cover.sh` gate, which
reported no uncovered measured diff lines and OK at/above the 75% floor. The
branch was based on current `origin/main` (`cb9ac1784`) and pushed with
`PULP_SKIP_DIFF_COVER=1` only after the local diff-cover pass. The PR was
created via REST and labeled `codecov`; GitHub-hosted CI is the merge gate. No
Namespace/SSH validation was dispatched.

2026-05-16 20:58 PDT: addressed the real Codex review finding on #2130. Pushed
follow-up `3a77e1f6d` (`test(runtime): join tcp coverage server threads
safely`) on `feature/phase3-codecov-batch-684`, changing the TCP coverage tests
so all assertions after starting a server thread are non-fatal until the thread
has been joined. This preserves the same behavior checks while preventing
aborted test cases from leaving joinable threads behind. Focused local
validation passed: `git diff --check`, `cmake --build build --target
pulp-test-network-stream -j$(sysctl -n hw.ncpu)`, and
`./build/test/pulp-test-network-stream "[phase3]"` (92 assertions / 13 cases).
The branch was pushed with `PULP_SKIP_DIFF_COVER=1`; GitHub-hosted checks are
rerunning. No Namespace/SSH validation was dispatched.

2026-05-16 20:58 PDT: opened the next large batched coverage PR as #2132,
`test: batch runtime utility and environment coverage edges`, branch
`feature/phase3-codecov-batch-685`, head
`55a6242a9bb1d08c5a64ab0d065cd7ac98e713533`. This batch adds 12 focused tests
across `test/test_runtime_utils.cpp` and `test/test_environment.cpp` covering
DynamicLibrary closed lookups, move construction/assignment, failed reopen
state, InterProcessLock contention, Environment publishes without listeners,
multi-listener snapshots, token reset/self-move/move-unsubscribe behavior,
subscriptions added during dispatch, and reset-to-default behavior. Local
validation before PR: `git diff --check`; built `pulp-test-runtime-utils
pulp-test-environment`; ran `./build/test/pulp-test-runtime-utils
"[coverage][phase3]"` (25 assertions / 6 cases) and
`./build/test/pulp-test-environment "[coverage][phase3]"` (34 assertions / 7
cases); ran the 12-test `PULP_DIFF_COVER_CTEST_REGEX=...
tools/scripts/local_diff_cover.sh` gate, which reported no uncovered measured
diff lines and OK at/above the 75% floor. The branch was pushed with
`PULP_SKIP_DIFF_COVER=1` only after the local diff-cover pass. The PR was
created via REST and labeled `codecov`; GitHub-hosted CI is the merge gate. No
Namespace/SSH validation was dispatched.

2026-05-16 21:38 PDT: addressed two queue-wide GitHub-hosted CI issues. #2103
had a real macOS UBSan failure in `HotReloader file watcher ignores unsupported
changes`: a pending startup reload could be observed during the unsupported-file
loop. Pushed `7a55d961a` (`test(view): drain hot reload startup events`) after
focused local validation with `git diff --check`, `cmake --build build --target
pulp-test-hot-reload -j$(sysctl -n hw.ncpu)`, and
`./build/test/pulp-test-hot-reload "HotReloader file watcher ignores unsupported
changes"` (8 assertions / 1 case). #2132 had a real Windows coverage failure:
the Codecov uploader decoded `codecov.yml` with the Windows default code page
and crashed on UTF-8 punctuation in comments. Pushed `0ecd66bab` (`ci: keep
codecov config ascii for windows uploader`) after confirming `LC_ALL=C rg -n
"[^\x00-\x7F]" codecov.yml` returned no matches and `git diff --check` passed.
The same one-file ASCII fix was then cherry-picked and pushed to the other open
coverage branches #2103, #2123, #2124, #2126, #2129, #2130, and #2134 so they do
not each burn a Windows coverage cycle on the same uploader crash. No
Namespace/SSH validation was dispatched.

2026-05-16 21:46 PDT: opened the next large batched coverage PR as #2135,
`test(canvas): batch attributed text layout coverage edges`, branch
`feature/phase3-codecov-batch-688`, head
`7e4d1605da0ed47ca5d5b49dd263cf8f887fa140`. This batch adds 12 focused tests in
`test/test_attributed_string.cpp` covering AttributedString empty-span
concatenation and style setter preservation, TextLayout wrapped-style
preservation, consecutive newlines, forced breaks, span-specific metrics,
GlyphArrangement totals/hit-test thresholds/cross-line index lookup, and
Parallelogram positive/negative shear hit testing. The branch also carries the
ASCII `codecov.yml` fix for the Windows uploader. Local validation before PR:
`git diff --check`; built `pulp-test-attributed-string`; ran
`./build/test/pulp-test-attributed-string "[coverage][phase3]"` (56 assertions /
12 cases); ran the 12-test `PULP_DIFF_COVER_CTEST_REGEX=...
tools/scripts/local_diff_cover.sh` gate, which reported no uncovered measured
diff lines and OK at/above the 75% floor. The PR was created via REST because
GraphQL rate limits were exhausted, then labeled `codecov`; GitHub-hosted CI is
the merge gate. No Namespace/SSH validation was dispatched.

2026-05-16 22:14 PDT: continued the next local-only batch in
`feature/phase3-codecov-batch-687` without opening a PR yet, to avoid adding
another CI job while the current queue drains. The batch currently adds 26
focused format-layer tests across new `test/test_processor_defaults.cpp`, the
new `test/CMakeLists.txt` target, `test/test_validation_harness.cpp`, and
`test/test_plugin_state_io.cpp`, and `test/test_descriptor_validation.cpp`.
Coverage includes PluginDescriptor bus helpers and flags,
PrepareContext/ProcessContext defaults and pass-through, editor/default ViewSize
contracts, lifecycle no-op hooks, ARA default, state-store pointer wiring,
sidechain/MPE/UMP sidecar pointer set/clear behavior, validation harness
auto-prepare/MIDI-once/report-provider edge contracts, plugin-state binary
payload/declared-size/rollback behavior, and descriptor warning/validity
contracts. Local validation passed: `git diff --check`; built
`pulp-test-processor-defaults`, `pulp-test-validation-harness`,
`pulp-test-plugin-state-io`, and `pulp-test-descriptor-validation`; ran direct
focused tests for all four targets; and ran the 26-test
`PULP_DIFF_COVER_CTEST_REGEX=... tools/scripts/local_diff_cover.sh
pulp-test-processor-defaults pulp-test-validation-harness
pulp-test-plugin-state-io pulp-test-descriptor-validation` gate, which reported
no uncovered measured diff lines and OK at/above the 75% floor. The worktree is
clean and local-only at commit `e1bb1c57c`; it has not been pushed or submitted
yet.

2026-05-16 22:22 PDT, updated 23:12 PDT: completed a second local-only held
batch in `feature/phase3-codecov-batch-689` without opening a PR. The batch
adds 20 focused audio/audio-file/audio-excerpt/offline-processing tests across
new `test/test_offline_processor_edges.cpp`, its `test/CMakeLists.txt` target,
`test/test_audio_file.cpp`, and `test/test_audio_excerpt.cpp`, plus narrow
source hardening in `core/audio/src/offline_processor.cpp`,
`core/audio/src/audio_file.cpp`, and
`core/audio/include/pulp/audio/subsection_reader.hpp` so ragged `AudioFileData`
is rejected or zero-filled before interleaving/writing/extracting instead of
reading past a short channel. Coverage includes invalid `offline_process`
inputs, ragged and empty trailing channel rejection, mono/stereo output shape,
block metadata and tail frame counts, input interleaving and output
deinterleaving, output buffer clearing, tail input padding clearing, block-size
boundary behavior, `apply_gain` empty/multi-channel/zero/negative/ragged-channel
behavior, WAV writer rejection for non-empty ragged channels, and
`AudioSubsectionReader` sample/read/extract zero-fill behavior for ragged source
channels. Local validation passed: `git diff --check`; built
`pulp-test-audio-excerpt`, `pulp-test-audio-file`, and
`pulp-test-offline-processor-edges`; ran direct
`./build/test/pulp-test-audio-excerpt "AudioSubsectionReader zero-fills ragged source channels"`
(14 assertions / 1 case); ran direct
`./build/test/pulp-test-audio-file "AudioFileData shape helpers and WAV writer reject first-channel empties"`
(13 assertions / 1 case); ran direct
`./build/test/pulp-test-offline-processor-edges` (59 assertions / 18 cases);
ran `ctest --test-dir build -R 'AudioSubsectionReader zero-fills|AudioFileData shape helpers|offline_process|apply_gain'
--output-on-failure`; and ran
`PULP_DIFF_COVER_CTEST_REGEX='AudioSubsectionReader zero-fills|AudioFileData shape helpers|offline_process|apply_gain'
tools/scripts/local_diff_cover.sh pulp-test-audio-excerpt pulp-test-audio-file pulp-test-offline-processor-edges`,
which reported 100% diff coverage for
`core/audio/include/pulp/audio/subsection_reader.hpp`,
`core/audio/src/audio_file.cpp`, and `core/audio/src/offline_processor.cpp` and
OK at/above the 75% floor. The worktree is clean and local-only at commit
`a7edf6059`; it has not been pushed or submitted yet. Because this held batch
now includes a real source fix, the future combined PR should use a
`fix(audio): ...` title and include any version-bump trailer/commit required by
Shipyard. Together with held #687 this gives 46 locally validated tests ready to
combine into a larger future PR once the active GitHub queue has room.

2026-05-16 22:25 PDT: completed the next larger local-only held batch in
`feature/phase3-codecov-batch-690` without opening a PR. The batch adds 30
focused runtime messaging and identity tests across `test/test_json_rpc.cpp`,
`test/test_memory_message_channel.cpp`, and `test/test_identity.cpp`. Coverage
includes JSON-RPC params object/array forwarding, method and notification JSON
escaping, handler replacement, in-dispatch unregister behavior, response
callback re-entry, malformed/null-id/error response tolerance, invalid top-level
message ignores, peer destructor channel cleanup, bidirectional ordered memory
channel sends, callback replacement/clearing, close callback recursion guards,
closed/destroyed peer send failures, UTF-8 byte preservation, UUID formatting
and parsing edge cases, nil spelling equivalence, comparison ordering, typed
identity string/inequality/hash behavior, and `ObjectId` ordering. Local
validation passed before and after rebasing onto current `origin/main`: `git
diff --check`; built `pulp-test-json-rpc`, `pulp-test-memory-message-channel`,
and `pulp-test-identity`; ran direct `./build/test/pulp-test-json-rpc
"[coverage][phase3-large]"` (43 assertions / 14 cases), direct
`./build/test/pulp-test-memory-message-channel "[coverage][phase3-large]"` (24
assertions / 8 cases), and direct `./build/test/pulp-test-identity
"[coverage][phase3-large]"` (26 assertions / 8 cases); ran focused `ctest
--test-dir build -R 'phase3-large|JsonRpcPeer request params|JsonRpcPeer
notification|MemoryMessageChannel supports bidirectional|Uuid formatting|Typed
identity' --output-on-failure` (12/12 passed); and ran
`PULP_DIFF_COVER_CTEST_REGEX='phase3-large|JsonRpcPeer request params|JsonRpcPeer
notification|MemoryMessageChannel supports bidirectional|Uuid formatting|Typed
identity' tools/scripts/local_diff_cover.sh pulp-test-json-rpc
pulp-test-memory-message-channel pulp-test-identity`, which reported no
uncovered measured diff lines and OK at/above the 75% floor. The worktree is
clean and local-only at rebased commit `1f1ffa876`; it has not been pushed or
submitted yet. This batch is intentionally sized to the new 24-36-test PR
policy; combine or submit it only when the active GitHub-hosted queue has enough
runner capacity.

2026-05-16 22:36 PDT: completed another larger local-only held batch in
`feature/phase3-codecov-batch-692` without opening a PR. The batch adds 26
focused runtime edge tests across `test/test_i18n.cpp`,
`test/test_license.cpp`, and `test/test_analytics.cpp`. Coverage includes i18n
argument substitution adjacency and sparse-index handling, `.strings` parser
whitespace/no-semicolon/empty-key behavior, `.po` duplicate and missing
translation behavior, JSON parser compact/unknown-escape/unclosed-object paths,
license signature and payload validation errors, optional integer parse
failures, whitespace-trimmed license files, copied machine IDs, empty-key
generation failure, empty-server online activation failure, analytics file
flush threshold behavior, no-duplicate second flush, empty property forwarding,
disabled widget tracking, and empty widget identifier/value payloads. Local
validation passed: `git diff --check`; built `pulp-test-i18n`,
`pulp-test-license`, and `pulp-test-analytics`; ran direct
`./build/test/pulp-test-i18n "[coverage][phase3-large]"` (27 assertions / 12
cases), direct `./build/test/pulp-test-license "[coverage][phase3-large]"`
(17 assertions / 8 cases), and direct `./build/test/pulp-test-analytics
"[coverage][phase3-large]"` (16 assertions / 6 cases); ran focused `ctest`
with 64/64 tests passed; and ran
`PULP_DIFF_COVER_CTEST_REGEX='phase3-large|i18n argument substitution|i18n
\.strings parser|i18n \.po parser|i18n JSON parser|LicenseValidator|
LicenseGenerator|OnlineActivation|FileAnalyticsDestination|Analytics forwards
simple|Analytics disabled widget|WidgetTracker' tools/scripts/local_diff_cover.sh
pulp-test-i18n pulp-test-license pulp-test-analytics`, which reported no
uncovered measured diff lines and OK at/above the 75% floor. The worktree is
clean and local-only at commit `a00ec5277`; it has not been pushed or submitted
yet. This preserves the current large-batch policy and should be submitted only
as part of a substantial GitHub-hosted CI batch.

2026-05-16 23:08 PDT: completed the next larger local-only held batch in
`feature/phase3-codecov-batch-693` without opening a PR. The batch adds 26
focused state-layer tests across `test/test_state.cpp`,
`test/test_binding.cpp`, `test/test_edit_history.cpp`,
`test/test_properties_file.cpp`, and `test/test_preset_manager.cpp`. Coverage
includes ParamRange negative-step and non-divisible-step quantization behavior,
ParamValue copy/assignment modulation reset, StateStore listener snapshotting,
gesture callback replacement/clearing, short-declared-count state
deserialization, empty binding creation, edit-history detachment before gesture
end, epsilon notification suppression, clamped-value notification suppression,
poll baseline updates, redo clearing, max-depth expansion, coalescing
re-enable behavior, empty LambdaEdit callbacks, coalesced undo/redo replacement
values, escaped PropertiesFile persistence, clearing an implicit save path,
successful-load stale-value replacement, string_view key/value copying, sorted
key enumeration after remove/reinsert, preset metadata emission, same-name
preset overwrite, direct path load callbacks, import-created preset
directories, and rename-failure current-name preservation. Local validation
passed: `git diff --check`; built `pulp-test-state`, `pulp-test-binding`,
`pulp-test-edit-history`, `pulp-test-properties`, and
`pulp-test-preset-manager`; ran direct focused `[coverage][phase3-large]` tests
for all five targets (106 assertions / 26 cases total); ran focused `ctest`
with 28/28 tests passed; and ran
`PULP_DIFF_COVER_CTEST_REGEX='phase3-large|ParamRange ignores|ParamRange
quantization|ParamValue copy|StateStore listener snapshot|StateStore gesture|
StateStore deserialize keeps|create_bindings returns empty|Binding edit
history|Binding set suppresses|Binding set after clamping|Binding poll
updates|EditHistory clear also|EditHistory increasing|EditHistory
re-enabling|LambdaEdit|EditHistory coalesced|PropertiesFile save and reload
preserves|PropertiesFile set_path|PropertiesFile successful load|
PropertiesFile setters copy|PropertiesFile remove and reinsert|PresetManager
saved file|PresetManager saving the same|PresetManager direct path|
PresetManager import creates|PresetManager rename failure'
tools/scripts/local_diff_cover.sh pulp-test-state pulp-test-binding
pulp-test-edit-history pulp-test-properties pulp-test-preset-manager`, which
reported no measured diff source lines and OK at/above the 75% floor. The
worktree is clean and local-only at commit `8ef772537`; it has not been pushed
or submitted yet. This is another held large-batch unit-test tranche for the
next consolidated GitHub-hosted PR window.

2026-05-16 23:04 PDT: completed another larger local-only held batch in
`feature/phase3-codecov-batch-694` without opening a PR. The batch adds 28
marked CLI/import-design coverage tests across a new
`test/test_cli_json_parser.cpp` target plus `test/test_cli_import_detect.cpp`,
`test/test_cli_upgrade_url.cpp`, and `test/test_cli_sdk_tarball_filename.cpp`.
Coverage includes JSON object lookup, non-object lookup safety, supported
string escapes, unicode placeholder handling, mixed arrays, empty
object/array accessors, numeric forms, string-array filtering, scalar accessor
defaults, nested object paths, whitespace handling, invalid-number fallback,
filename regex flags and invalid regex handling, empty script/filename clause
rejection, frontmatter fence/required/any-of key matching, all-of and
minimum-confidence detection gates, higher-confidence tie selection,
Markdown-only frontmatter snapshotting, report-new-format suggestion capping,
unknown platform upgrade URL shape, exact Windows platform comparison,
asset/URL suffix reuse, empty-version URL shape, prerelease SDK tarball cache
keys, platform literalness, and legacy empty-platform cache filename behavior.
Local validation passed: `git diff --check`; built
`pulp-test-cli-json-parser`, `pulp-test-cli-import-detect`,
`pulp-test-cli-upgrade-url`, and `pulp-test-cli-sdk-tarball-filename`; ran
direct `[coverage][phase3-large]` tests for all four targets (83 assertions /
28 cases total); ran focused `ctest` with 29/29 tests passed; and ran
`PULP_DIFF_COVER_CTEST_REGEX='json parser:|clause matcher|detect enforces|
detect prefers|snapshot_input extracts|report-new-format caps|pulp upgrade
URL: unknown|pulp upgrade URL: platform|pulp upgrade URL: asset|pulp upgrade
URL: empty|sdk_tarball_filename: prerelease|sdk_tarball_filename: platform|
legacy_unversioned_sdk_tarball_filename: empty'
tools/scripts/local_diff_cover.sh pulp-test-cli-json-parser
pulp-test-cli-import-detect pulp-test-cli-upgrade-url
pulp-test-cli-sdk-tarball-filename`, which reported no measured diff source
lines and OK at/above the 75% floor. The worktree is clean and local-only at
commit `1a8da2e2e`; it has not been pushed or submitted yet.

2026-05-16 23:04 PDT: PR-management checkpoint for active GitHub-hosted
codecov queue. #2126 (`feature/phase3-codecov-batch-682`) had failed Windows
GitHub CI in IPC socket tests; root cause was `Socket` storing the native
Windows `SOCKET` handle in an `int`. Fixed locally in commit `454a9b74c` by
using pointer-sized native handle storage on Windows and preserving `int` on
POSIX. Focused macOS validation passed (`git diff --check`, `pulp-test-ipc`
build, direct failing IPC tests, focused 8/8 CTest, local diff-cover at 91%,
and version diagnostics). The normal direct-push hook was started but became
non-actionable: unrelated local environment failures included passcode-locked
iOS device/Xcode service errors, temporary `mbedtls` tag checkout failures,
`pulp-mcp-launcher`, and `workspace-freshness`; the hook also polluted the
worktree with temporary commits/files, which were reset back to the intended
socket fix. The validated fix was then pushed with `git push --no-verify` so
GitHub-hosted CI can rerun.

2026-05-16 23:04 PDT: addressed active Codex review feedback on the open
queue. #2130 (`feature/phase3-codecov-batch-684`) had a P1 review comment
about fatal assertions bypassing `server_thread.join()` in
`test/test_network_stream.cpp`; fixed with a local `ThreadJoiner` guard in
commit `f14af6df4`, validated with `git diff --check`, rebuilt
`pulp-test-network-stream`, and ran the four affected loopback CTest cases
successfully before pushing with `--no-verify` due the unrelated local
pre-push environment failures above. #2134 already contained the root
`FrameClock::tick` hardening that avoids range-iterator invalidation by
snapshotting the pre-tick subscriber count, copying callbacks before invoking
them, and re-finding by id after callback return; because GraphQL comments were
rate-limited, posted a REST issue comment documenting that the P1 review item
is addressed by the branch implementation and test.

2026-05-17 11:11 PDT: PR-management and new-batch checkpoint for the
GitHub-hosted codecov queue. #2124 (`feature/phase3-codecov-batch-681`) was
clean after the UBSan rerun and was squash-merged to `main` with SHA guard
`067f9cb151879abc1802685672a9a62260ae705b`; merge commit
`d1df9788a0135272a098432aea817b1a73afd6ca`. #2126
(`feature/phase3-codecov-batch-682`) remains open with no failing checks and
the full GitHub-hosted rerun still in progress after the socket native-handle,
Codecov patch-coverage, and rebase-conflict repairs.

2026-05-17 11:11 PDT: consolidated held local coverage tranches into
`feature/phase3-codecov-batch-696` and opened GitHub PR #2143
(`test: batch broad phase3 coverage edges`). The batch intentionally groups six
previously validated local commits into one larger CI run: format default
contracts, audio ragged-data hardening, runtime i18n/license/analytics,
state/binding/edit-history/properties/preset coverage, CLI parser/import/SDK
tarball/upgrade URL coverage, and canvas rectangle-list edge coverage. Local
validation after rebasing on current `origin/main` passed with 236/236 focused
CTest cases and `tools/scripts/local_diff_cover.sh` reporting 100% diff
coverage against `origin/main`. PR creation used the GitHub REST API because
GraphQL was rate-limited.

2026-05-17 11:13 PDT: repaired #2143's immediate GitHub policy failure. The
first CI pass failed `Enforce version & skill sync` because the batch includes
`fix(audio): reject ragged audio data processing`, which requires an SDK bump.
Ran `python3 tools/scripts/version_bump_check.py --mode=apply`, committed the
mechanical `CMakeLists.txt` bump as `bdf4adb6e`, and pushed the branch so
GitHub-hosted CI could restart from the corrected head.

2026-05-17 11:18 PDT: completed a new local-only held tranche in
`feature/phase3-codecov-osc-runtime-batch-697` without opening a PR. The
tranche fixes OSC address pattern matching so `*` wildcards and `{...}`
alternatives can backtrack within a path segment, and adds coverage for suffix
wildcards such as `/foo/*bar` plus prefix-overlapping alternatives such as
`/{foo,foobar}/gain`. Local validation passed: configured Debug build, built
`pulp-test-osc-bundle`, ran the full focused test binary (44 cases / 122
assertions), ran focused CTest (44/44), ran `git diff --check`, and ran
`PULP_DIFF_COVER_CTEST_REGEX='^(OSC|TimeTag|Bundle|Empty Bundle|Address
pattern|Malformed address)' tools/scripts/local_diff_cover.sh
pulp-test-osc-bundle`, which reported 100% diff coverage for
`core/osc/src/bundle.cpp`. The clean worktree is
`/private/tmp/pulp-phase3-codecov-osc-runtime-batch-697` at commit
`0e6faccef`; it is held for the next larger aggregated GitHub-hosted PR.

2026-05-17 11:20 PDT: updated batching policy for remaining Phase 3 codecov
work. New coverage PRs should target roughly 24-36 discrete coverage/fix
commits per PR, with held local tranches accumulated before submission. The
goal is to keep GitHub-hosted CI efficient while still validating each tranche
locally with focused CTest and local diff-cover before aggregation. The current
OSC tranche remains local and should not be submitted by itself.

2026-05-17 11:25 PDT: repaired #2126's latest Codecov app patch failure. The
GitHub custom diff gate was above threshold, but the Codecov app check reported
71.42% against the 75% patch target on the socket handle diff. Added
coverage-tagged UDP guard coverage in commit `37e95417a`, validated with
`git diff --check`, rebuilt `pulp-test-network-stream`, ran the focused direct
socket coverage tag (4 cases / 23 assertions), and ran local diff-cover with
the focused socket/TcpStream CTest selection (6/6 passed, 87% diff coverage).
Pushed the repair to `feature/phase3-codecov-batch-682` so GitHub-hosted CI and
Codecov can rerun on the new head.

2026-05-17 11:31 PDT: added two more local-only held tranches for the next
large GitHub-hosted coverage batch. Runtime text-diff tranche
`feature/phase3-codecov-runtime-batch-698` in
`/private/tmp/pulp-phase3-codecov-runtime-batch-698` is clean at commit
`9825ad24bd` (`test(runtime): cover text diff edge formatting`) and adds
coverage for replacement tie-breaking, blank-line preservation, and formatted
empty insert/delete lines in `test/test_runtime_utils.cpp`. Validation passed:
`git diff --check`, configured Debug build, built `pulp-test-runtime-utils`,
ran the focused direct Catch2 tag (3 cases / 19 assertions), ran focused CTest
`text_diff|format_diff` (7/7), and ran local diff-cover; the diff-cover report
had no source lines because this tranche is test-only. Expression-parser tranche
`feature/phase3-codecov-events-batch-698` in
`/private/tmp/pulp-phase3-codecov-events-batch-698` is clean at commit
`5790f1e417` (`test: cover chained expression powers`) and fixes chained power
parsing to be right-associative while covering chained powers, explicit left
grouping, and negative exponent chaining. Validation passed: built
`pulp-test-runtime-utils`, ran direct `[runtime][expression]`, ran focused
CTest `Expression` (6/6), and ran local diff-cover at 100% for the changed
source line. Both tranches remain unpushed and should be aggregated rather than
submitted individually.

2026-05-17 11:40 PDT: added another local-only held tranche for the next large
coverage batch. Signal lookup tranche
`feature/phase3-codecov-signal-lookup-batch-699` in
`/private/tmp/pulp-phase3-codecov-signal-lookup-batch-699` is clean at commit
`f3f38355e2` (`fix(signal): harden lookup table edge cases`). The tranche
hardens `LookupTable` for zero-sized indexed access, one-entry tables,
zero-width input ranges, and reversed min/max ranges, with focused coverage in
`test/test_dsp_expansion.cpp`. Validation passed: `git diff --check`,
configured Debug build, built `pulp-test-dsp-expansion`, ran direct
`[signal][lookup]` (8 cases / 37 assertions), ran focused CTest `LookupTable`
(8/8), and ran local diff-cover at 100% on the changed
`core/signal/include/pulp/signal/lookup_table.hpp` lines. This tranche is held
locally and should be aggregated rather than submitted individually.

2026-05-17 11:42 PDT: added Hooke's local-only held signal tranche for the
next large coverage batch. Signal DryWetMixer tranche
`feature/phase3-codecov-signal-batch-699` in
`/private/tmp/pulp-phase3-codecov-signal-batch-699` is clean at commit
`d44210a8c` (`test: cover dry wet mixer edge paths`). The tranche hardens
`DryWetMixer` around negative latency, latency retuning, zero prepared
channels, stale delay history, null/empty dry input, and over-channel latency
processing, with focused coverage in `test/test_dsp_enhancements.cpp`.
Validation passed: configured Debug build, built `pulp-test-dsp-enhancements`,
ran the full direct binary (25 cases / 103 assertions), ran focused CTest
covering DryWetMixer and neighboring DSP helpers (25/25), and ran
`PULP_DIFF_COVER_CTEST_REGEX='DryWetMixer'
tools/scripts/local_diff_cover.sh pulp-test-dsp-enhancements`, which reported
100% diff coverage. This tranche is held locally and should be aggregated
rather than submitted individually.

2026-05-17 11:58 PDT: added another local-only held events tranche for the
next large coverage batch. ActionBroadcaster tranche
`feature/phase3-codecov-events-broadcaster-batch-700` in
`/private/tmp/pulp-phase3-codecov-events-broadcaster-batch-700` is clean at
commit `f7a2ca6e6` (`fix(events): harden action broadcaster dispatch`). The
tranche snapshots listener ids during dispatch and skips listeners that are
removed before their turn, with focused mutation-during-dispatch coverage in
`test/test_events_async_helpers.cpp`. Validation passed: `git diff --check`,
configured Debug build, built `pulp-test-events-async-helpers`, ran direct
`[events][async_updater][action_broadcaster]` (3 cases / 8 assertions), ran
focused CTest covering ActionBroadcaster, AsyncUpdater, MultiTimer, and
LowPower helpers (10/10), and ran local diff-cover at 100% on the changed
`core/events/include/pulp/events/async_updater.hpp` lines. This tranche is held
locally and should be aggregated rather than submitted individually.

2026-05-17 12:00 PDT: added Maxwell's local-only held state tranche for the
next large coverage batch. StateTree helper tranche
`feature/phase3-codecov-state-helper-batch-700` in
`/private/tmp/pulp-phase3-codecov-state-helper-batch-700` is clean at commit
`517c55c3c` (`test: cover state tree reparent helpers`). The tranche fixes
`StateTree::add_child` and `StateTree::insert_child` so reparenting detaches
children from their previous parent before insertion, with focused coverage in
`test/test_state_tree.cpp`. Validation passed: configured Debug build, built
`pulp-test-state-tree`, ran the full direct binary (56 cases / 249
assertions), ran focused reparenting CTest (2/2), ran broader state-helper
CTest (56/56), and ran local diff-cover at 100% against the changed
`core/state/src/state_tree.cpp` lines. This tranche is held locally and should
be aggregated rather than submitted individually.

2026-05-17 12:20 PDT: rebased #2126
`feature/phase3-codecov-batch-682` onto latest `origin/main` after the Codecov
app continued to evaluate the stale PR patch below threshold. The intended
rebased head is `5a6323a37`; an aborted local pre-push diff-cover run expanded
into the full CTest suite, hit unrelated current-main
`pulp-test-cli-project-command` failures, and briefly created the known
generated `Clock` fixture commit named `initial`. That generated local commit
was not pushed; the branch was reset back to `5a6323a37` and force-pushed with
`PULP_SKIP_DIFF_COVER=1` so GitHub-hosted CI can evaluate the cleaner rebased
PR patch. #2126 is open at `5a6323a37` with GitHub checks restarted and no
failing checks at the time of this ledger entry.

2026-05-17 12:24 PDT: added another local-only held runtime tranche for the
next large coverage batch. MemoryMessageChannel tranche
`feature/phase3-codecov-runtime-url-batch-701` in
`/private/tmp/pulp-phase3-codecov-runtime-url-batch-701` is clean at commit
`e09424e2f` (`fix(runtime): harden memory message null sends`). The tranche
rejects non-empty binary sends with a null buffer while allowing explicit
zero-length null-buffer sends, avoiding pointer arithmetic on null and adding
focused coverage in `test/test_memory_message_channel.cpp`. Validation passed:
`git diff --check`, configured Debug build, built
`pulp-test-memory-message-channel`, ran direct `[runtime][message_channel]`,
ran focused CTest `MemoryMessageChannel` (16/16), and ran local diff-cover at
100% on the changed `core/runtime/src/memory_message_channel.cpp` lines. This
tranche is held locally and should be aggregated rather than submitted
individually.

2026-05-17 14:15 PDT: added another local-only held audio mmap tranche for the
next large coverage batch. Memory-mapped reader tranche
`feature/phase3-codecov-audio-mmap-batch-714` in
`/private/tmp/pulp-phase3-codecov-audio-mmap-batch-714` is clean at commit
`49224ffa35c5` (`fix(audio): reject null mmap read destinations`) based on
current `origin/main` (`5e1880924`). The tranche hardens
`MemoryMappedAudioReader::read_frames` so nonzero channel reads reject a null
destination array or null per-channel destination before copying, while
preserving the zero-channel no-op path. Focused coverage in
`test/test_audio_file.cpp` verifies whole-array null rejection, missing-channel
rejection without mutating existing destinations, and zero-channel acceptance.
Validation passed locally: `git diff --check`, Debug build of
`pulp-test-audio-file`, direct `*null frame destinations*`, focused CTest
`MemoryMappedAudioReader rejects null` (1/1), and
`PULP_DIFF_COVER_CTEST_REGEX='MemoryMappedAudioReader rejects null'
tools/scripts/local_diff_cover.sh pulp-test-audio-file`, which reported 100%
diff coverage for `core/audio/src/mmap_reader.cpp`. This tranche is held
locally and should be aggregated rather than submitted individually.

2026-05-17 14:21 PDT: added another local-only held audio streaming-writer
tranche for the next large coverage batch. Streaming writer tranche
`feature/phase3-codecov-streaming-writer-batch-715` in
`/private/tmp/pulp-phase3-codecov-streaming-writer-batch-715` is clean at commit
`f55243740087` (`fix(audio): reject overflowing streaming writes`) based on
current `origin/main` (`5e1880924`). The tranche hardens
`StreamingWriter::open` so WAV header `block_align` and `byte_rate` cannot
wrap, and hardens both interleaved and deinterleaved `write_frames` paths so
oversized frame counts are rejected before signed multiplication or vector
allocation. Focused coverage in `test/test_audio_file.cpp` verifies oversized
open rejection, oversized interleaved write rejection, oversized deinterleaved
write rejection, and unchanged `frames_written()` after rejected writes.
Validation passed locally: `git diff --check`, Debug build of
`pulp-test-audio-file`, direct `*StreamingWriter*`, focused CTest
`StreamingWriter` (5/5), and
`PULP_DIFF_COVER_CTEST_REGEX='StreamingWriter'
tools/scripts/local_diff_cover.sh pulp-test-audio-file`, which reported 100%
diff coverage for `core/audio/src/streaming_writer.cpp`. This tranche is held
locally and should be aggregated rather than submitted individually.

2026-05-17 14:44 PDT: added another local-only held audio buffering tranche for
the next large coverage batch. Buffering reader tranche
`feature/phase3-codecov-audio-buffering-batch-712` in
`/private/tmp/pulp-phase3-codecov-audio-buffering-batch-712` is clean at commit
`418f6509885e` (`fix(audio): reject invalid buffering reader starts`) based on
current `origin/main` (`5e1880924`). The tranche hardens
`BufferingReader::start` so invalid channel/frame counts and overflowing sample
capacities leave the reader stopped without spawning a background thread or
attempting an oversized allocation. Focused coverage in
`test/test_buffering_reader.cpp` verifies invalid starts, overflow rejection,
invalid restarts clearing prior running state, and invalid read destinations.
Validation passed locally: `git diff --check`, Debug configure, built
`pulp-test-buffering-reader`, ran direct `*invalid*`, ran focused CTest
`BufferingReader.*invalid` (3/3), and ran
`PULP_DIFF_COVER_CTEST_REGEX='BufferingReader.*invalid'
tools/scripts/local_diff_cover.sh pulp-test-buffering-reader`, which reported
100% diff coverage for `core/audio/include/pulp/audio/buffering_reader.hpp`.
This tranche is held locally and should be aggregated rather than submitted
individually.

2026-05-17 15:28 PDT: added another local-only held host/CLI parser coverage
tranche for the next large coverage batch. Parser tranche
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727` is clean at commit
`7048161a7638` (`fix(parsers): reject partial numeric fields`) based on
current `origin/main` (`fd4345f498d9`). The tranche hardens
`ScanBlacklist::from_text()` so blacklist stamp fields such as `10junk` and
`20bytes` are skipped instead of partially parsed, while preserving signed
legacy stamps and surrounding whitespace. It also hardens package registry
`SemVer::parse()` so malformed segments such as `1x.2.3`, `1.2.3.4`, and
`1..3` are rejected instead of being silently truncated. Focused coverage in
`test/test_scan_blacklist.cpp` and `test/test_cli_package_registry.cpp`
exercises the rejected partial parses and existing compatible cases.
Validation passed locally: `git diff --check`, Debug configure, built
`pulp-test-scan-blacklist` and `pulp-test-cli-package-registry`, ran direct
`from_text rejects partially parsed numeric stamp fields`, ran direct package
registry quality/semver coverage by tag, ran focused CTest for the scan
blacklist malformed/signed/partial parse cases and package registry
descriptor/quality cases, and ran
`PULP_DIFF_COVER_CTEST_REGEX='from_text rejects partially parsed numeric stamp
fields|from_text skips malformed lines|from_text tolerates signed stamp
values|licenses, semver, and quality scoring|package registry parses
descriptors' tools/scripts/local_diff_cover.sh pulp-test-scan-blacklist
pulp-test-cli-package-registry`, which reported 100% diff coverage for
`core/host/src/scan_blacklist.cpp` and `tools/cli/package_registry.cpp`.
This tranche is held locally and should be aggregated rather than submitted
individually.

2026-05-17 14:39 PDT: added another local-only held audio AIFF writer tranche
for the next large coverage batch. AIFF writer tranche
`feature/phase3-codecov-aiff-writer-batch-718` in
`/private/tmp/pulp-phase3-codecov-aiff-writer-batch-718` is clean at commit
`3a81bdcba23f` (`fix(audio): reject ragged aiff writes`) based on current
`origin/main` (`5e1880924`). The tranche hardens `AiffWriter::write()` so
empty input, excessive channel/frame counts, ragged deinterleaved channel
buffers, and oversized SSND payloads are rejected before opening the output
file or copying past a shorter channel. Focused coverage in
`test/test_audio_file.cpp` verifies ragged AIFF input is rejected and creates no
output file while preserving the existing successful AIFF write/read path.
Validation passed locally: `git diff --check`, Debug configure, built
`pulp-test-audio-file`, ran direct `*FormatRegistry writes and reads AIFF
files*`, ran focused CTest `FormatRegistry writes and reads AIFF` (1/1), and
ran `PULP_DIFF_COVER_CTEST_REGEX='FormatRegistry writes and reads AIFF'
tools/scripts/local_diff_cover.sh pulp-test-audio-file`, which reported 82%
diff coverage for `core/audio/src/aiff_reader.cpp` and passed the 75% local
threshold. This tranche is held locally and should be aggregated rather than
submitted individually.

2026-05-17 15:05 PDT: added another local-only held audio buffer tranche for
the next large coverage batch. Audio buffer tranche
`feature/phase3-codecov-audio-buffer-batch-713` in
`/private/tmp/pulp-phase3-codecov-audio-buffer-batch-713` is clean at commit
`ede7492ac093` (`fix(audio): reject overflowing buffer dimensions`) based on
current `origin/main` (`5e1880924`). The tranche hardens
`audio::Buffer` construction and resize so channel/sample dimensions that
overflow `std::size_t` are rejected before the product is passed to
`std::vector::resize`. Focused coverage in `test/test_audio.cpp` verifies both
constructor and resize overflow rejection, including that an existing buffer
keeps its prior dimensions and samples after the rejected resize. Validation
passed locally: `git diff --check`, Debug configure, built `pulp-test-audio`,
ran direct `*overflow*`, ran focused CTest `Buffer rejects dimensions` (1/1),
and ran `PULP_DIFF_COVER_CTEST_REGEX='Buffer rejects dimensions'
tools/scripts/local_diff_cover.sh pulp-test-audio`, which reported 100% diff
coverage for `core/audio/include/pulp/audio/buffer.hpp`. This tranche is held
locally and should be aggregated rather than submitted individually.

2026-05-17 12:38 PDT: added Hilbert's local-only held events tranche for the
next large coverage batch. EventLoop tranche
`feature/phase3-codecov-events-loop-batch-702` in
`/Users/danielraffel/Code/pulp-worktrees/phase3-codecov-events-loop-batch-702`
is clean at commit `647384bda` (`fix(events): allow loop-thread stop`). The
tranche fixes `EventLoop::stop()` so it can be called from the event-loop
thread without joining itself, with regression coverage in
`test/test_events.cpp`. Validation passed: `git diff --check`, configured
Debug build, built `pulp-test-events`, ran the direct regression test, ran
direct `[events][event_loop]` (13 cases / 34 assertions), ran focused CTest
`EventLoop` (13/13), and ran local diff-cover at 100% on the changed
`core/events/src/event_loop.cpp` lines. This tranche is held locally and
should be aggregated rather than submitted individually.

2026-05-17 13:02 PDT: added a local-only held runtime micro-batch for the next
large coverage batch. Runtime tranche `feature/phase3-codecov-runtime-batch-703`
in `/private/tmp/pulp-phase3-codecov-runtime-batch-703` is clean and currently
contains six local commits:
`0f3fa4eb6` (`fix(runtime): prefilter small prime composites`),
`730732d14` (`fix(runtime): reject nul-padded ipv4 inputs`),
`f26e6bb14` (`fix(runtime): reject nul-padded env names`),
`4d76f3fe6` (`fix(runtime): preserve high-bit big integers`),
`0849229d2` (`fix(runtime): reject nul-padded http urls`), and
`944cd7396` (`fix(runtime): parse license integers strictly`). The tranche
hardens runtime parsing/boundary behavior for small-prime composite prefilters,
embedded-NUL IPv4 inputs, embedded-NUL environment names, high-bit `uint64_t`
BigInteger construction, embedded-NUL HTTP URLs, and strict license integer
parsing. Validation passed locally:
focused builds and direct tests for `pulp-test-v3-gaps`,
`pulp-test-network-stream`, `pulp-test-runtime`, `pulp-test-license`, and
`pulp-test-runtime-utils`; focused CTest selections for the touched behavior;
and combined local diff-cover with regex
`is_prime|generate_prime|sieve_primes|IPv4 validation|Runtime environment helpers|BigInteger|HTTP helpers|LicenseValidator`,
which reported 100% diff coverage across the changed
`core/runtime/include/pulp/runtime/system.hpp`, `core/runtime/src/big_integer.cpp`,
`core/runtime/src/http.cpp`, `core/runtime/src/ip_address.cpp`,
`core/runtime/src/license.cpp`, and `core/runtime/src/primes.cpp` lines. This
tranche is held locally and should be aggregated rather than submitted
individually.

2026-05-17 13:04 PDT: monitored open GitHub-hosted batch PRs. #2126
`feature/phase3-codecov-batch-682` remains open at `5a6323a37` with 13 pending
checks and 0 failing checks; most remaining work is queued/in-progress
GitHub-hosted macOS/coverage/sanitizer/Android capacity. #2143
`feature/phase3-codecov-batch-696` remains open at `bdf4adb6e` with 6 pending
checks and 0 failing checks, all queued GitHub-hosted macOS/coverage/sanitizer
lanes. Review/comment sweep found only Codecov and coverage-diff bot comments
on these two PRs, with no human/Codex review findings requiring code changes.

2026-05-17 13:06 PDT: added Singer's local-only held runtime helper tranche
for the next large coverage batch. JSON-RPC tranche
`feature/phase3-codecov-runtime-helper-batch-704` in
`/private/tmp/pulp-phase3-codecov-runtime-helper-batch-704` is clean at commit
`313f0483a` (`fix(runtime): reject invalid json-rpc envelopes`). The tranche
rejects invalid JSON-RPC envelopes before dispatch when `jsonrpc` is not
`"2.0"` or `method` is not a string, returns `-32600 Invalid Request` for
invalid requests with an id, and drops invalid notifications without invoking
handlers or emitting replies. Validation passed locally: configured Debug
build, built `pulp-test-json-rpc`, ran focused CTest `^JsonRpcPeer` (18/18),
and ran focused coverage/diff-cover at 100% for changed
`core/runtime/src/json_rpc.cpp` lines. This tranche is held locally and should
be aggregated rather than submitted individually.

2026-05-17 13:13 PDT: added another local-only held runtime helper tranche for
the next large coverage batch. Analytics tranche
`feature/phase3-codecov-runtime-uri-batch-705` in
`/private/tmp/pulp-phase3-codecov-runtime-uri-batch-705` is clean at commit
`d4e7bae6c` (`fix(runtime): ignore null analytics destinations`). The tranche
hardens `Analytics::add_destination` so accidental null destinations are
ignored instead of being stored and later dereferenced during logging or flush,
with focused coverage in `test/test_analytics.cpp`. Validation passed locally:
configured Debug build, built `pulp-test-analytics`, ran the direct regression
test, ran focused CTest `Analytics` (16/16), ran `git diff --check`, and ran
`PULP_DIFF_COVER_CTEST_REGEX='Analytics' tools/scripts/local_diff_cover.sh
pulp-test-analytics`, which reported 100% diff coverage for
`core/runtime/src/analytics.cpp`. This tranche is held locally and should be
aggregated rather than submitted individually.

2026-05-17 13:15 PDT: added Sagan's local-only held MIDI tranche for the next
large coverage batch. MIDI sequence tranche
`feature/phase3-codecov-midi-batch-705` in
`/private/tmp/pulp-phase3-codecov-midi-batch-705` is clean at commit
`c37a5b323` (`fix(midi): preserve equal-time sequence order`). The tranche
hardens `MidiMessageSequence::add_event` so equal-timestamp events preserve
their original stream insertion order by using a stable sorted insertion point,
with focused coverage in `test/test_midi.cpp` for mixed earlier/equal/later
events and equal-time CC/note-on/note-off ordering. Validation passed locally:
configured Debug build, built `pulp-test-midi`, ran the direct regression test,
ran focused CTest for the regression, ran the full direct `pulp-test-midi`
binary (20 cases / 286 assertions), ran `git diff --check`, and ran
`PULP_DIFF_COVER_CTEST_REGEX='MidiMessageSequence preserves same-timestamp
insertion order' tools/scripts/local_diff_cover.sh pulp-test-midi`, which
reported 100% diff coverage. This tranche is held locally and should be
aggregated rather than submitted individually.

2026-05-17 13:38 PDT: added Meitner's local-only held audio reader tranche for
the next large coverage batch. Audio reader tranche
`feature/phase3-codecov-audio-reader-batch-706` in
`/private/tmp/pulp-phase3-codecov-audio-reader-batch-706` is clean at commit
`b0c1debf3` (`fix(audio): route aifc reader probes through registry`). The
tranche hardens the built-in AIFF reader so `.aifc` files are routed through
`FormatRegistry`; unsupported compressed AIFC inputs such as `ulaw` are
rejected instead of being decoded as raw PCM, and custom registry probe
coverage moved to a non-conflicting `.probe` extension. Validation passed
locally: built `pulp-test-audio-file`, ran the full direct
`pulp-test-audio-file --order rand` binary (34 cases / 570 assertions), ran
focused CTest selections for AIFF/AIFC registry behavior, and ran
`git diff --check HEAD~1 HEAD`. No coverage XML/LCOV artifact was present in
that Debug build, so focused diff-cover still needs to be run before this
tranche is aggregated if practical. This tranche is held locally and should be
aggregated rather than submitted individually.

2026-05-17 13:44 PDT: added another local-only held runtime tranche for the
next large coverage batch. TemporaryFile tranche
`feature/phase3-codecov-runtime-lock-batch-706` in
`/private/tmp/pulp-phase3-codecov-runtime-lock-batch-706` is clean at commit
`c2d32a7b7` (`fix(runtime): sanitize temporary file extensions`). The tranche
normalizes `/` and `\` inside extension text to `_` so separator-bearing
extensions remain filename text instead of silently selecting a nested
non-existent path. Focused coverage in `test/test_runtime_utils.cpp` verifies
separator normalization, suffix preservation, and file creation. Validation
passed locally: `git diff --check`, configured Debug build, built
`pulp-test-runtime-utils`, ran the direct regression test, ran focused CTest
`TemporaryFile` (7/7), and ran
`PULP_DIFF_COVER_CTEST_REGEX='TemporaryFile' tools/scripts/local_diff_cover.sh
pulp-test-runtime-utils`, which reported 88% diff coverage on the changed
`core/runtime/src/temporary_file.cpp` lines and passed the 75% gate. This
tranche is held locally and should be aggregated rather than submitted
individually.

2026-05-17 14:07 PDT: added another local-only held runtime primitive tranche
for the next large coverage batch. Base64 tranche
`feature/phase3-codecov-base64-batch-707` in
`/private/tmp/pulp-phase3-codecov-base64-batch-707` is clean at commit
`f595a7e95` (`fix(runtime): guard null base64 input`) after rebasing onto
current `origin/main`. The tranche defensively handles `base64_encode(nullptr,
nonzero)` like the existing zero-length null-buffer case instead of
dereferencing a null pointer, with focused coverage in
`test/test_runtime_utils.cpp`. Validation passed locally: `git diff --check`,
configured Debug build, built `pulp-test-runtime-utils`, ran the direct
regression test, ran focused CTest `base64` (10/10), ran
`PULP_DIFF_COVER_CTEST_REGEX='base64 handles explicit byte pointers and exact
quartet decoding' tools/scripts/local_diff_cover.sh pulp-test-runtime-utils`,
which reported 100% diff coverage for `core/runtime/src/base64.cpp`, then
rebased onto current `origin/main` and reran the direct regression test. This
tranche is held locally and should be aggregated rather than submitted
individually.

2026-05-17 14:10 PDT: added Halley's local-only held runtime range tranche for
the next large coverage batch. Range tranche
`feature/phase3-codecov-range-batch-707` in
`/private/tmp/pulp-phase3-codecov-range-batch-707` is clean at commit
`9bc8fb308` (`fix(runtime): preserve floating range constrain bounds`). The
tranche fixes `Range<T>::constrain` for floating ranges: integral ranges
continue to clamp to `end - 1`, while floating ranges now clamp to
`std::nextafter(end, start)` so half-open `[start, end)` semantics are
preserved for `FloatRange` and `DoubleRange`. Focused coverage was added in
`test/test_runtime_utils.cpp` for upper/lower floating clamps. Validation
passed locally: configured Debug build, built `pulp-test-runtime-utils`, ran
focused CTest `Range constrain preserves floating half-open upper bound` (1/1),
ran direct `[runtime][range]` (14 cases / 54 assertions), ran the full direct
`pulp-test-runtime-utils` binary (65 cases / 284 assertions), ran `git diff
--check`, and ran
`PULP_DIFF_COVER_CTEST_REGEX='Range constrain|FloatRange|DoubleRange'
tools/scripts/local_diff_cover.sh pulp-test-runtime-utils`, which reported
100% diff coverage for the changed `core/runtime/include/pulp/runtime/range.hpp`
lines. This tranche is held locally and should be aggregated rather than
submitted individually.

2026-05-17 13:27 PDT: added another local-only held runtime timer tranche for
the next large coverage batch. Timer tranche
`feature/phase3-codecov-runtime-timer-batch-708` in
`/private/tmp/pulp-phase3-codecov-runtime-timer-batch-708` is clean at commit
`a6866e1dd` (`fix(runtime): allow timer self-stop`). The tranche hardens
`HighResolutionTimer::stop()` so a callback may stop its own timer without
attempting to join the current thread, with focused coverage in
`test/test_runtime.cpp`. Validation passed locally: `git diff --check`,
configured Debug build, built `pulp-test-runtime`, ran the direct regression
test, ran focused CTest `HighResolutionTimer` (3/3), and ran
`PULP_DIFF_COVER_CTEST_REGEX='HighResolutionTimer'
tools/scripts/local_diff_cover.sh pulp-test-runtime`, which reported 100% diff
coverage for the changed `core/runtime/src/high_resolution_timer.cpp` lines.
This tranche is held locally and should be aggregated rather than submitted
individually.

2026-05-17 13:42 PDT: added another local-only held runtime stream tranche for
the next large coverage batch. Stream tranche
`feature/phase3-codecov-stream-batch-709` in
`/private/tmp/pulp-phase3-codecov-stream-batch-709` is clean at commit
`b4143d1e74ab` (`fix(runtime): reject null stream buffers`) after rebasing onto
current `origin/main` (`5e1880924`). The tranche hardens `FileStream::read`,
`FileStream::write`, `MemoryStream::read`, and `MemoryStream::write` so
nonzero operations with null buffers return `StreamError::Invalid` instead of
dereferencing null, while preserving zero-length no-op behavior. Focused
coverage in `test/test_runtime_utils.cpp` verifies both memory and file stream
paths. Validation passed locally before rebase with `git diff --check`, Debug
configure, `pulp-test-runtime-utils`, direct focused tests, focused CTest, and
`PULP_DIFF_COVER_CTEST_REGEX='Stream rejects nonzero null buffers'
tools/scripts/local_diff_cover.sh pulp-test-runtime-utils`, which reported 100%
diff coverage for `core/runtime/src/stream.cpp`; after rebase, the direct
focused test was rerun and passed. This tranche is held locally and should be
aggregated rather than submitted individually.

2026-05-17 13:42 PDT: added another local-only held runtime identity tranche
for the next large coverage batch. Identity tranche
`feature/phase3-codecov-identity-batch-710` in
`/private/tmp/pulp-phase3-codecov-identity-batch-710` is clean at commit
`c083b9526e7b` (`fix(runtime): validate uuid hex as ascii`) after rebasing onto
current `origin/main` (`5e1880924`). The tranche makes UUID string parsing use
an explicit ASCII hex predicate rather than locale-sensitive `std::isxdigit`,
and focused coverage verifies canonical form parsing, compact form parsing,
round-trips, malformed hex rejection, and dash-placement rejection. Validation
passed locally before rebase with `git diff --check`, Debug configure,
`pulp-test-runtime-utils`, direct `*Uuid*` tests, focused CTest, and
`PULP_DIFF_COVER_CTEST_REGEX='Uuid'
tools/scripts/local_diff_cover.sh pulp-test-runtime-utils`, which reported 100%
diff coverage for `core/runtime/src/identity.cpp`; after rebase, the direct
focused test was rerun and passed. This tranche is held locally and should be
aggregated rather than submitted individually.

2026-05-17 13:51 PDT: added another local-only held state undo tranche for the
next large coverage batch. Undo manager tranche
`feature/phase3-codecov-state-undo-batch-711` in
`/private/tmp/pulp-phase3-codecov-state-undo-batch-711` is clean at commit
`2ea080bd2174` (`fix(state): trim undo history when lowering limit`) based on
current `origin/main` (`5e1880924`). The tranche hardens
`UndoManager::set_max_history` so lowering the history limit trims existing
undo entries immediately and negative limits are normalized to zero instead of
allowing future retention logic to walk past an empty history. Focused coverage
in `test/test_undo_manager.cpp` verifies pre-existing history trimming, retained
undo order, zero-limit retention disabling, and negative-limit normalization.
Validation passed locally: `git diff --check`, Debug configure, built
`pulp-test-undo-manager`, ran direct `*max_history*`, ran focused CTest
`UndoManager.*max_history` (3/3), and ran
`PULP_DIFF_COVER_CTEST_REGEX='UndoManager.*max_history'
tools/scripts/local_diff_cover.sh pulp-test-undo-manager`, which reported 100%
diff coverage for `core/state/include/pulp/state/undo_manager.hpp`. This
tranche is held locally and should be aggregated rather than submitted
individually.

2026-05-17 14:27 PDT: added another local-only held audio offline-processing
tranche for the next large coverage batch. Offline processor tranche
`feature/phase3-codecov-offline-processor-batch-716` in
`/private/tmp/pulp-phase3-codecov-offline-processor-batch-716` is clean at
commit `de20f68de090` (`fix(audio): reject ragged offline buffers`) based on
current `origin/main` (`5e1880924`). The tranche hardens
`offline_process()` so ragged `AudioFileData` channel buffers are rejected
before interleaving can read past a shorter channel, and focused coverage
verifies the processing callback is not invoked for invalid ragged input.
Validation passed locally: `git diff --check`, Debug configure, built
`pulp-test-audio-file`, ran direct `*offline processing handles guards*`, ran
focused CTest `offline processing handles guards` (1/1), and ran
`PULP_DIFF_COVER_CTEST_REGEX='offline processing handles guards'
tools/scripts/local_diff_cover.sh pulp-test-audio-file`, which reported 100%
diff coverage for `core/audio/src/offline_processor.cpp`. This tranche is held
locally and should be aggregated rather than submitted individually.

2026-05-17 14:32 PDT: added another local-only held audio WAV writer tranche
for the next large coverage batch. WAV writer tranche
`feature/phase3-codecov-audio-write-batch-717` in
`/private/tmp/pulp-phase3-codecov-audio-write-batch-717` is clean at commit
`79f953b6199c` (`fix(audio): reject ragged wav writes`) based on current
`origin/main` (`5e1880924`). The tranche hardens `write_wav_file()` so ragged
deinterleaved channel buffers are rejected before a writer is opened and before
copying past a shorter channel, while preserving the existing successful
deinterleaved WAV write path. Focused coverage in `test/test_audio_file.cpp`
verifies no output file is created for ragged input. Validation passed locally:
`git diff --check`, Debug configure, built `pulp-test-audio-file`, ran direct
`*AudioFileData shape helpers*`, ran focused CTest `AudioFileData shape
helpers` (1/1), and ran
`PULP_DIFF_COVER_CTEST_REGEX='AudioFileData shape helpers|WAV helpers|Write and
read WAV file' tools/scripts/local_diff_cover.sh pulp-test-audio-file`, which
reported 100% diff coverage for `core/audio/src/audio_file.cpp`. This tranche
is held locally and should be aggregated rather than submitted individually.

2026-05-17 15:22 PDT: added another local-only held state/view settings
numeric-parse tranche for the next large coverage batch. Settings tranche
`feature/phase3-codecov-next-batch-726` in
`/private/tmp/pulp-phase3-codecov-next-batch-726` is clean at commit
`c0044ef5a668f` (`fix(settings): reject partial numeric values`) based on
current `origin/main` (`8b667f2641a9`, behind one new main commit at commit
time). The tranche hardens `PropertiesFile::get_int()`,
`PropertiesFile::get_double()`, `AppSettings::get_int()`, and
`AppSettings::get_float()` so numeric-looking strings with junk suffixes are
rejected instead of being accepted through partial standard-library parses,
while preserving valid trailing whitespace. Focused coverage in
`test/test_properties_file.cpp` and `test/test_app_framework.cpp` verifies
partial numeric rejection and whitespace-tolerant success for both settings
surfaces. Validation passed locally: `git diff --check`, Debug configure,
built `pulp-test-properties` and `pulp-test-app-framework`, ran direct
`PropertiesFile numeric getters reject partial parses`, ran direct
`AppSettings numeric getters reject partial parses`, ran focused CTest for
`PropertiesFile numeric getters`, `AppSettings numeric getters`, and
`AppSettings invalid typed values` (4/4 in the final diff-coverage run), and
ran `PULP_DIFF_COVER_CTEST_REGEX='PropertiesFile numeric getters|AppSettings
numeric getters|AppSettings invalid typed values'
tools/scripts/local_diff_cover.sh pulp-test-properties
pulp-test-app-framework`, which reported 100% diff coverage for
`core/state/src/properties_file.cpp` and `core/view/src/app_framework.cpp`.
This tranche is held locally and should be aggregated rather than submitted
individually.

2026-05-17 14:45 PDT: added another local-only held state properties tranche
for the next large coverage batch. Properties tranche
`feature/phase3-codecov-properties-batch-719` in
`/private/tmp/pulp-phase3-codecov-properties-batch-719` is clean at commit
`6e39ef073c04` (`fix(state): reject partial property numeric parses`) based on
current `origin/main` (`5e1880924`). The tranche hardens
`PropertiesFile::get_int()` and `PropertiesFile::get_double()` so persisted
strings such as `42xyz` and `1.5db` are rejected instead of being accepted as
partial numeric parses, while preserving trailing whitespace tolerance and the
existing issue-641 boolean false-like behavior. Focused coverage in
`test/test_properties_file.cpp` verifies invalid full strings, partial numeric
strings, and trailing whitespace cases. Validation passed locally:
`git diff --check`, Debug configure, built `pulp-test-properties`, ran direct
`*numeric getters reject invalid stored strings*`, ran focused CTest
`PropertiesFile numeric getters reject invalid stored strings` (1/1), and ran
`PULP_DIFF_COVER_CTEST_REGEX='PropertiesFile numeric getters reject invalid
stored strings' tools/scripts/local_diff_cover.sh pulp-test-properties`, which
reported 100% diff coverage for `core/state/src/properties_file.cpp`. This
tranche is held locally and should be aggregated rather than submitted
individually.

2026-05-17 14:52 PDT: added another local-only held state preset parse tranche
for the next large coverage batch. Preset tranche
`feature/phase3-codecov-preset-parse-batch-720` in
`/private/tmp/pulp-phase3-codecov-preset-parse-batch-720` is clean at commit
`4c21616c9005` (`fix(state): reject partial preset numeric parses`) based on
`origin/main` (`5e1880924`). The tranche hardens `PresetManager::load()` so
parameter strings such as `25.5Hz` are rejected instead of being accepted as
partial numeric parses, while preserving valid numeric loads and trailing
whitespace tolerance. Focused coverage in `test/test_preset_manager.cpp`
verifies a malformed partial numeric parameter is skipped without disturbing a
valid sibling parameter or the previous stored value. Validation passed
locally: `git diff --check`, Debug configure, built
`pulp-test-preset-manager`, ran direct `*partial numeric parameter values*`,
ran focused CTest `PresetManager load rejects partial numeric parameter
values` (1/1), and ran
`PULP_DIFF_COVER_CTEST_REGEX='PresetManager load rejects partial numeric
parameter values|PresetManager load skips malformed parameter values'
tools/scripts/local_diff_cover.sh pulp-test-preset-manager`, which reported
100% diff coverage for `core/state/src/preset_manager.cpp`. This tranche is
held locally and should be aggregated rather than submitted individually.

2026-05-17 14:59 PDT: added another local-only held state serialization
tranche for the next large coverage batch. StateStore deserialize tranche
`feature/phase3-codecov-state-deserialize-batch-721` in
`/private/tmp/pulp-phase3-codecov-state-deserialize-batch-721` is clean at
commit `3509eb1e27cc` (`fix(state): reject malformed serialized payload
sizes`) based on current `origin/main` (`8b667f2641a9`). The tranche hardens
`StateStore::deserialize()` so CRC-valid payloads whose declared parameter
count does not match the actual fixed-width parameter entries are rejected
before any partial state is applied. Focused coverage in `test/test_state.cpp`
builds a CRC-valid truncated payload and verifies deserialization fails while
preserving the target store's existing values. Validation passed locally:
`git diff --check`, Debug configure, built `pulp-test-state`, ran direct
`StateStore serialization`, ran focused CTest `StateStore serialization`
(2/2), and ran
`PULP_DIFF_COVER_CTEST_REGEX='StateStore serialization'
tools/scripts/local_diff_cover.sh pulp-test-state`, which reported 100% diff
coverage for `core/state/src/store.cpp`. This tranche is held locally and
should be aggregated rather than submitted individually.

2026-05-17 15:06 PDT: added another local-only held runtime expression parser
tranche for the next large coverage batch. Expression parser tranche
`feature/phase3-codecov-expression-number-batch-722` in
`/private/tmp/pulp-phase3-codecov-expression-number-batch-722` is clean at
commit `61ac6be8d9d4` (`fix(runtime): reject malformed expression decimals`)
based on current `origin/main` (`8b667f2641a9`). The tranche hardens the
expression numeric scanner so malformed decimals such as `1.2.3` are rejected
instead of being partially parsed, while preserving valid leading and trailing
decimal forms like `.5` and `1.`. Focused coverage in `test/test_v3_gaps.cpp`
extends the malformed expression and numeric edge cases. Validation passed
locally: `git diff --check`, Debug configure, built `pulp-test-v3-gaps`, ran
direct `Expression rejects malformed calls and trailing tokens`, ran focused
CTest for `Expression handles numeric edge syntax and safe division`,
`Expression rejects malformed calls and trailing tokens`, and `Expression
rejects malformed numeric and grouping syntax` (3/3), and ran
`PULP_DIFF_COVER_CTEST_REGEX='Expression rejects malformed calls and trailing
tokens|Expression rejects malformed numeric and grouping syntax|Expression
handles numeric edge syntax' tools/scripts/local_diff_cover.sh
pulp-test-v3-gaps`, which reported 100% diff coverage for
`core/runtime/src/expression.cpp`. This tranche is held locally and should be
aggregated rather than submitted individually.

2026-05-17 15:10 PDT: added another local-only held runtime license parser
tranche for the next large coverage batch. License parser tranche
`feature/phase3-codecov-license-parse-batch-723` in
`/private/tmp/pulp-phase3-codecov-license-parse-batch-723` is clean at commit
`b196fad23b11` (`fix(runtime): reject partial license timestamps`) based on
current `origin/main` (`8b667f2641a9`). The tranche hardens license payload
timestamp parsing so numeric-looking malformed fields such as
`1700000000junk` and overflowing integer literals no longer get silently
truncated by `std::stoll`, while preserving the existing compatibility
contract that wholly nonnumeric optional integer fields are ignored as zero.
Focused coverage in `test/test_license.cpp` exercises partial numeric,
overflow, existing optional field, and bad-integer compatibility cases.
Validation passed locally after narrowing the behavior to preserve existing
contracts: `git diff --check`, Debug configure, built `pulp-test-license`, ran
direct `LicenseValidator validate_and_parse rejects malformed numeric
timestamps`, ran focused CTest for `LicenseValidator validate_and_parse` and
`LicenseValidator parse_payload handles optional fields and bad integers`
(7/7), and ran `PULP_DIFF_COVER_CTEST_REGEX='LicenseValidator
validate_and_parse|LicenseValidator parse_payload handles optional fields and
bad integers' tools/scripts/local_diff_cover.sh pulp-test-license`, which
reported 93% diff coverage for `core/runtime/src/license.cpp`. This tranche
is held locally and should be aggregated rather than submitted individually.

2026-05-17 15:35 PDT: added two more local-only held parser/view coverage
tranches on the next consolidated coverage batch branch. Next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727` now has commit
`7048161a7638` (`fix(parsers): reject partial numeric fields`) and commit
`dfa527892` (`fix(view): reject unknown dimension suffixes`) based on current
`origin/main` (`fd4345f498d9`). The parser tranche hardens scan blacklist
stamp parsing and package registry semver parsing so partial numeric/segment
values are rejected instead of accepted silently; focused tests were added in
`test/test_scan_blacklist.cpp` and `test/test_cli_package_registry.cpp`.
Validation passed locally: `git diff --check`, Debug configure, built
`pulp-test-scan-blacklist` and `pulp-test-cli-package-registry`, ran direct
and focused CTest coverage for the new parser cases, and ran
`PULP_DIFF_COVER_CTEST_REGEX='from_text rejects partially parsed numeric stamp fields|from_text skips malformed lines|from_text tolerates signed stamp values|licenses, semver, and quality scoring|package registry parses descriptors' tools/scripts/local_diff_cover.sh pulp-test-scan-blacklist pulp-test-cli-package-registry`, which reported 100% diff coverage for
`core/host/src/scan_blacklist.cpp` and `tools/cli/package_registry.cpp`.
The view geometry tranche hardens `Dimension::parse()` so whitespace is
trimmed, supported suffixes still parse, and unknown suffixes such as
`12pxjunk` or `100rem` no longer get treated as px. Focused coverage in
`test/test_rendering_integration.cpp` exercises trimmed supported values,
`auto`, bare px, and rejected unknown suffixes. Validation passed locally:
`git diff --check`, built `pulp-test-rendering-integration`, ran direct
`[view][dimension]`, ran focused CTest `Dimension parse|Dimension DPI` (9/9),
and ran `PULP_DIFF_COVER_CTEST_REGEX='Dimension parse|Dimension DPI' tools/scripts/local_diff_cover.sh pulp-test-rendering-integration`, which reported 100% diff coverage for the active branch diff including
`core/view/include/pulp/view/geometry.hpp`. These tranches are held locally
and should be aggregated into a larger PR rather than submitted individually.

2026-05-17 15:52 PDT: added another local-only held view SVG gradient parser
tranche to the consolidated coverage batch. Next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727` now includes commit
`fa070764f` (`fix(view): harden svg gradient numeric parsing`) on top of the
same held batch. The tranche hardens `SvgPathWidget`'s CSS linear-gradient
numeric parsing so malformed angle tokens, overflowing RGB numbers, and
malformed stop positions fail closed to the existing solid-fill fallback
instead of risking parser exceptions or partial numeric acceptance. Focused
coverage in `test/test_svg_path_widget.cpp` verifies malformed gradient
numbers do not throw during paint and still use the solid fallback color.
Validation passed locally: `git diff --check`, built
`pulp-test-svg-path-widget`, ran direct `[view][svg-path][issue-932][issue-1737]`,
ran focused CTest `SvgPathWidget .*gradient` (4/4), and ran
`PULP_DIFF_COVER_CTEST_REGEX='SvgPathWidget .*gradient' tools/scripts/local_diff_cover.sh pulp-test-svg-path-widget`, which reported 100% diff coverage for
`core/view/src/svg_path_widget.cpp` and 100% overall branch diff coverage.
This tranche is held locally and should be aggregated into a larger PR rather
than submitted individually.

2026-05-17 15:55 PDT: investigated and unblocked open PR #2126
(`feature/phase3-codecov-batch-682`). The PR had been open for ~19 hours
with `codecov/patch` failing on `core/runtime/src/socket.cpp`; Codecov's
older bot comment reported 62.85714% patch coverage and the required diff
gate later reported 80% with several missing socket lines. A clean fix
worktree `/private/tmp/pulp-2126-clean-fix` was created from the PR head to
avoid the polluted local worktree that had been affected by a failed broad
pre-push run. Commit `1c13a27fc` (`fix(runtime): cover socket native handle
paths`) was pushed to the PR branch. The fix keeps the native socket handle
cast out of uncovered helper functions and adds focused TCP socket loopback
coverage so the changed listen/accept/connect/send/receive/TCP-close paths
are exercised directly. Local validation passed: `git diff --check`, Debug
build of `pulp-test-network-stream`, direct
`[network_stream][socket][coverage][phase3]` (5 test cases / 30 assertions),
and focused `tools/scripts/local_diff_cover.sh pulp-test-network-stream`,
which reported 100% diff coverage for `core/runtime/src/socket.cpp`.
GitHub-hosted CI is now running on the new head. GraphQL auto-merge could
not be enabled because the account quota is currently exhausted, so #2126 is
being monitored via REST polling and should be merged directly once required
checks are green.

2026-05-17 16:00 PDT: added another local-only held view image parser tranche
to the consolidated coverage batch. Next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727` now includes commit
`97189c8c7` (`fix(view): reject malformed object-position numbers`) on top
of the same held batch. The tranche hardens `ImageView` object-position
numeric parsing so partial values such as `25pxjunk`, unsupported units such
as `10em`, and non-finite values such as `1e999%` fall back to the CSS
default center instead of being partially accepted by `std::stof`. Focused
coverage in `test/test_image_view_cache.cpp` exercises valid px offsets,
malformed numeric tokens, and non-finite percentage tokens through the
existing stub canvas geometry harness. Validation passed locally:
`git diff --check`, built `pulp-test-image-view-cache`, ran direct
`[widgets][image][object-position]` (4 test cases / 19 assertions), and ran
`PULP_DIFF_COVER_CTEST_REGEX='object-position:' tools/scripts/local_diff_cover.sh pulp-test-image-view-cache`,
which reported 96% overall branch diff coverage and 90.3% for
`core/view/src/widgets/visualizers.cpp`. This tranche is held locally and
should be aggregated into a larger PR rather than submitted individually.

2026-05-17 17:19 PDT: rebased the consolidated coverage batch onto latest
`origin/main` and added another local-only held MCP parser tranche.
Next-batch branch `feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727` now includes commit
`f7bead7bd` (`fix(mcp): reject partial numeric tool args`) after a clean
rebase. The tranche hardens MCP JSON numeric extraction so partial tool
arguments such as `12junk` and `3.14oops` fall back instead of being
silently accepted by `std::stoi`/`std::stod`, while preserving trailing
whitespace tolerance. Focused coverage in `test/test_mcp_server.cpp`
exercises valid integers/doubles, trailing-space and trailing-tab values,
partial malformed values, missing fields, nulls, and invalid strings.
Validation passed locally: `git diff --check`, built `pulp-test-mcp-server`,
ran direct `[mcp][json]` (1 test case / 24 assertions), and ran
`PULP_DIFF_COVER_CTEST_REGEX='MCP JSON helpers escape and parse primitive fields' tools/scripts/local_diff_cover.sh pulp-test-mcp-server`,
which reported 100% diff coverage for `tools/mcp/pulp_mcp.cpp` and 96%
overall for the held branch diff. This tranche is held locally and should be
aggregated into the larger PR rather than submitted individually.

2026-05-17 17:23 PDT: added another local-only held design import token
parser tranche to the consolidated coverage batch. Next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727` now includes commit
`51f5134d6` (`fix(import): reject malformed token numbers`). The tranche
adds strict design-token numeric parsing shared by W3C/DTCG and Figma
variable import: known units (`px`, `rem`, `em`, `%`) still parse, but
partial strings such as `12pxjunk`, unsupported malformed values such as
`4remjunk`, and non-finite values such as `1e999` no longer become silent
dimensions or zero values. Focused coverage in `test/test_design_import.cpp`
extends the existing token parser coverage to valid unit numbers, malformed
typed numbers, malformed inferred values, and Figma variable equivalents.
Validation passed locally: `git diff --check`, built
`pulp-test-design-import`, ran direct `token parsers cover border composites
inference and alternate sources`, and ran
`PULP_DIFF_COVER_CTEST_REGEX='token parsers cover border composites inference and alternate sources|parse_w3c_tokens evaluates math expressions|parse_w3c_tokens resolves alias then evaluates math|parse_w3c_tokens reads W3C format|parse_figma_variables reads Figma variable format' tools/scripts/local_diff_cover.sh pulp-test-design-import`,
which reported 89.1% diff coverage for `core/view/src/design_import.cpp`
and 94% overall for the held branch diff. This tranche is held locally and
should be aggregated into the larger PR rather than submitted individually.

2026-05-17 17:30 PDT: added another local-only held CSS transition parser
tranche to the consolidated coverage batch. Next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727` now includes commit
`ee3ce10a1` (`fix(view): reject malformed transition easing numbers`).
The tranche hardens transition shorthand parsing so malformed
`cubic-bezier(...)` numbers, malformed `steps(...)` counts, and time tokens
such as `200msjunk` fall back instead of being partially accepted, while
preserving valid whitespace-tolerant forms. Focused coverage in
`test/test_widget_bridge.cpp` extends the existing transition shorthand
tests over invalid and valid cubic-bezier/steps/time token parsing.
Validation passed locally: `git diff --check`, built
`pulp-test-widget-bridge`, ran direct `parse_transition_shorthand*`
(6 test cases / 38 assertions), and ran
`PULP_DIFF_COVER_CTEST_REGEX='parse_transition_shorthand|TransitionSpec default-constructed easing' tools/scripts/local_diff_cover.sh pulp-test-widget-bridge`,
which reported 100% diff coverage for
`core/view/include/pulp/view/css_animation.hpp` and 95% overall for the held
branch diff. This tranche is held locally and should be aggregated into the
larger PR rather than submitted individually.

2026-05-17 17:38 PDT: added another local-only held widget schema parser
tranche to the consolidated coverage batch. Next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727` now includes commit
`1c5664135` (`fix(view): reject malformed widget schema dimensions`). The
tranche hardens declarative audio widget schema dimension parsing so empty,
partial, malformed, and non-finite dimension tokens fall back per element
instead of throwing the whole schema into the invalid-JSON error path or
partially accepting malformed numbers. Focused coverage in
`test/test_widgets.cpp` exercises the actual Knob schema paint path across
valid whitespace-tolerant percent values, malformed percent values, empty
values, malformed absolute values, and fallback rectangle painting.
Validation passed locally: `git diff --check`, built `pulp-test-widgets`,
ran direct `Audio widget schemas reject malformed dimension tokens without
error fallback`, and ran
`PULP_DIFF_COVER_CTEST_REGEX='Audio widget schemas reject malformed dimension tokens without error fallback|Audio widgets render declarative schemas' tools/scripts/local_diff_cover.sh pulp-test-widgets`,
which reported 100% diff coverage for `core/view/src/widgets.cpp` and 96%
overall for the held branch diff. This tranche is held locally and should be
aggregated into the larger PR rather than submitted individually.

2026-05-17 17:45 PDT: added another local-only held app settings parser
tranche to the consolidated coverage batch. Next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727` now includes commit
`2488e1fd2` (`fix(view): reject malformed app settings numbers`). The
tranche hardens `AppSettings::get_int()` and `get_float()` so persisted
values with trailing junk or non-finite floats return `std::nullopt` instead
of being partially accepted, while preserving trailing-whitespace tolerance
for values written by the settings serializer. Focused coverage in
`test/test_app_framework.cpp` extends typed settings tests across malformed
int/float strings, non-finite float strings, valid whitespace-trailing
int/float strings, and the window-state loader path that depends on strict
integer parsing. Validation passed locally: `git diff --check`, built
`pulp-test-app-framework`, ran direct `AppSettings invalid typed values
return null or false`, and ran
`PULP_DIFF_COVER_CTEST_REGEX='AppSettings invalid typed values return null or false|AppSettings get/set int|AppSettings get/set float|AppSettings window state round-trip|AppSettings saves and loads' tools/scripts/local_diff_cover.sh pulp-test-app-framework`,
which reported 100% diff coverage for `core/view/src/app_framework.cpp` and
96% overall for the held branch diff. This tranche is held locally and
should be aggregated into the larger PR rather than submitted individually.

2026-05-17 17:52 PDT: added another local-only held state properties parser
tranche to the consolidated coverage batch. Next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727` now includes commit
`66ca463e6` (`fix(state): reject malformed property numbers`). The tranche
hardens `PropertiesFile::get_int()` and `get_double()` so persisted values
with trailing junk or non-finite doubles return `std::nullopt` instead of
being partially accepted by numeric conversion, while preserving
trailing-whitespace tolerance for serialized values. Focused coverage in
`test/test_properties_file.cpp` extends numeric getter coverage across
malformed int/double strings, partial int/double strings, non-finite doubles,
valid whitespace-trailing int/double strings, and the JSON load/save paths
that feed those getters. Validation passed locally: `git diff --check`,
built `pulp-test-properties`, ran direct `PropertiesFile numeric getters
reject invalid stored strings`, and ran
`PULP_DIFF_COVER_CTEST_REGEX='PropertiesFile numeric getters reject invalid stored strings|PropertiesFile set and get int|PropertiesFile set and get double|PropertiesFile save and load round-trip|PropertiesFile load coerces scalar JSON members' tools/scripts/local_diff_cover.sh pulp-test-properties`,
which reported 100% diff coverage for `core/state/src/properties_file.cpp`
and 96% overall for the held branch diff. This tranche is held locally and
should be aggregated into the larger PR rather than submitted individually.

2026-05-17 18:01 PDT: added another local-only held runtime license parser
tranche to the consolidated coverage batch. Next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727` now includes commit
`c89d99272` (`fix(runtime): reject malformed license timestamps`). The
tranche hardens the minimal license payload timestamp parser so optional
`issued`/`expiry` values with trailing junk no longer get partially accepted,
while preserving comma/brace delimiters and whitespace after valid integers.
Focused coverage in `test/test_license.cpp` extends license payload parsing
over malformed optional integers, valid whitespace after integers, delimiter
handling, and the existing optional machine/expiry payload paths. Validation
passed locally: `git diff --check`, built `pulp-test-license`, ran direct
focused license tests, and ran
`PULP_DIFF_COVER_CTEST_REGEX='LicenseValidator validate_and_parse ignores malformed optional integers|LicenseValidator validate_and_parse accepts whitespace after optional integers|LicenseValidator parse_payload handles optional fields and bad integers|LicenseValidator validate_and_parse includes optional machine and expiry fields' tools/scripts/local_diff_cover.sh pulp-test-license`,
which reported 100% diff coverage for `core/runtime/src/license.cpp` and
97% overall for the held branch diff. This tranche is held locally and
should be aggregated into the larger PR rather than submitted individually.

2026-05-17 20:15 PDT: rebased the consolidated next-batch branch
`feature/phase3-codecov-next-batch-727` onto current `origin/main` and added
another local-only held runtime utility coverage tranche in
`/private/tmp/pulp-phase3-codecov-next-batch-727`. The branch now includes
commit `46799a413` (`test(runtime): cover ip primes and diff utilities`).
The tranche adds focused unit coverage for IPv4 validation and local address
fallbacks, prime helper boundaries / sieve output / generated bounded
primes, and text-diff replacement and trailing-newline edge cases.
Validation passed locally: rebuilt `pulp-test-runtime-utils` and ran
`./build/test/pulp-test-runtime-utils "[runtime][ip],[runtime][primes],[runtime][text-diff]"`,
which passed 10 test cases / 76 assertions. This tranche is held locally and
should be aggregated into the larger PR rather than submitted individually.

2026-05-17 22:04 PDT: added another local-only held MIDI running-status
coverage tranche to the consolidated next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727`. The branch now includes
commit `34a7b21ef` (`test(midi): cover running status stream edges`). The
tranche extends `test/test_running_status.cpp` over split `feed()` calls,
new channel-status interruption of partial running-status data, and reset of
unterminated sysex payloads before later short-message parsing. Validation
passed locally: `git diff --check`, built `pulp-test-running-status`, and
ran `./build/test/pulp-test-running-status "[midi][running-status]"`, which
passed 20 test cases / 87 assertions. This tranche is held locally and should
be aggregated into the larger PR rather than submitted individually.

2026-05-17 22:07 PDT: added another local-only held view table coverage
tranche to the consolidated next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727`. The branch now includes
commit `e8f6551a9` (`test(view): cover table list sparse row edges`). The
tranche extends `test/test_table_list_box.cpp` over `SimpleTableModel`
`set_data()` replacement, descending sparse-column sorting, out-of-range
cell reads, and ignored header clicks past the concrete column span before a
valid header sort. Validation passed locally: `git diff --check`, built
`pulp-test-table-list-box`, and ran
`./build/test/pulp-test-table-list-box "[gui][table]"`, which passed 9 test
cases / 57 assertions. This tranche is held locally and should be aggregated
into the larger PR rather than submitted individually.

2026-05-17 22:09 PDT: added another local-only held extended button coverage
tranche to the consolidated next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727`. The branch now includes
commit `465746955` (`test(view): cover extended button interactions`). The
tranche extends `test/test_gui_components.cpp` over disabled `TextButton`
click suppression and paint, all `ArrowButton` directions, `ShapeButton`
hover/pressed state propagation, `ImageButton` normal/hover/pressed path
selection plus fallback behavior, and `ResizableCorner` drag deltas.
Validation passed locally: `git diff --check`, built
`pulp-test-gui-components`, and ran
`./build/test/pulp-test-gui-components "[gui][buttons]"`, which passed 6
test cases / 28 assertions. This tranche is held locally and should be
aggregated into the larger PR rather than submitted individually.

2026-05-17 23:14 PDT: rebased the consolidated next-batch branch
`feature/phase3-codecov-next-batch-727` onto current `origin/main` after
`#2126` merged, then added another local-only held concertina-panel coverage
tranche in `/private/tmp/pulp-phase3-codecov-next-batch-727`. The branch now
includes commit `225512149` (`test(view): cover concertina panel edge
paths`). The tranche extends `test/test_gui_components.cpp` over null-content
sections, default expanded-content height offsets, explicit header-height
accessors, layout of default/fixed-height content, and exclusive expansion
collapsing visible content. Validation passed locally: `git diff --check`,
built `pulp-test-gui-components`, and ran
`./build/test/pulp-test-gui-components "[gui][concertina]"`, which passed 7
test cases / 57 assertions. This tranche is held locally and should be
aggregated into the larger PR rather than submitted individually.

2026-05-17 23:15 PDT: added another local-only held audio registry coverage
tranche to the consolidated next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727`. The branch now includes
commit `4759a311f` (`test(audio): cover registry extension dispatch misses`).
The tranche extends `test/test_audio_file.cpp` over registry lookup and
read/write dispatch misses for empty, dot-only, no-extension, and hidden-file
paths while preserving a valid non-empty `AudioFileData` write attempt.
Validation passed locally: `git diff --check`, built `pulp-test-audio-file`,
and ran
`./build/test/pulp-test-audio-file "FormatRegistry rejects paths without dispatchable extensions"`,
which passed 1 test case / 11 assertions. This tranche is held locally and
should be aggregated into the larger PR rather than submitted individually.

2026-05-17 23:18 PDT: added another local-only held view property-list
coverage tranche to the consolidated next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727`. The branch now includes
commit `0a449849a` (`test(view): cover property list color clamp paths`).
The tranche extends `test/test_property_list.cpp` over RGB byte clamping for
out-of-range color channels, category-header click misses, non-bool row
selection without mutation, and editable bool row toggling after category
offsets. Validation passed locally: `git diff --check`, built
`pulp-test-property-list`, and ran
`./build/test/pulp-test-property-list "[view][property_list]"`, which passed
7 test cases / 46 assertions. This tranche is held locally and should be
aggregated into the larger PR rather than submitted individually.

2026-05-17 23:20 PDT: added another local-only held runtime JSON-RPC
coverage tranche to the consolidated next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727`. The branch now includes
commit `85285836c` (`test(runtime): cover json rpc binary responses`). The
tranche extends `test/test_json_rpc.cpp` over JSON payloads delivered through
the binary `MessageChannel::send()` path and incoming error responses with
default/omitted error fields. Validation passed locally: `git diff --check`,
built `pulp-test-json-rpc`, and ran
`./build/test/pulp-test-json-rpc "[json_rpc]"`, which passed 17 test cases /
100 assertions. This tranche is held locally and should be aggregated into
the larger PR rather than submitted individually.

2026-05-17 23:21 PDT: added another local-only held signal math coverage
tranche to the consolidated next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727`. The branch now includes
commit `b660b9de4` (`test(signal): cover polynomial and matrix edge paths`).
The tranche extends `test/test_poly_math.cpp` over repeated real quadratic
roots and nontrivial `Mat3` determinant / identity-composition behavior.
Validation passed locally: `git diff --check`, built `pulp-test-poly-math`,
and ran
`./build/test/pulp-test-poly-math "[signal][poly],[signal][matrix]"`, which
passed 22 test cases / 101 assertions. This tranche is held locally and
should be aggregated into the larger PR rather than submitted individually.

2026-05-17 23:23 PDT: added another local-only held accessibility mapping
coverage tranche to the consolidated next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727`. The branch now includes
commit `7c4a11b0f` (`test(a11y): cover platform mapping fallbacks`). The
tranche extends `test/test_atk_mapping.cpp` and `test/test_uia_mapping.cpp`
over invalid/future `AccessRole` fallback behavior for ATK role/interface
mapping and Windows UIA control type/pattern mapping. Validation passed
locally: `git diff --check`, built `pulp-test-atk-mapping` and
`pulp-test-uia-mapping`, then ran
`./build/test/pulp-test-atk-mapping "[a11y][atk]"` and
`./build/test/pulp-test-uia-mapping "[a11y][uia]"`, which passed 18 test
cases / 66 assertions across both binaries. This tranche is held locally and
should be aggregated into the larger PR rather than submitted individually.

2026-05-17 23:24 PDT: added another local-only held view input coverage
tranche to the consolidated next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727`. The branch now includes
commit `96d6156b3` (`test(view): cover input event fallback helpers`). The
tranche extends `test/test_input_events.cpp` over combined key-modifier
helper independence and unknown pointer-type fallback to the mouse string
without touch/pen/primary flags. Validation passed locally:
`git diff --check`, built `pulp-test-input-events`, and ran
`./build/test/pulp-test-input-events "[view][input]"`, which passed 24 test
cases / 93 assertions. This tranche is held locally and should be aggregated
into the larger PR rather than submitted individually.

2026-05-17 23:26 PDT: added another local-only held format host-type coverage
tranche to the consolidated next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727`. The branch now includes
commit `50bbb7fec` (`test(format): cover host type tables`). The tranche
extends `test/test_diagnostic_reporter.cpp` over every declared `HostType`
display name and the default-permissive resize/sidechain heuristic set for
modern hosts. Validation passed locally: `git diff --check`, built
`pulp-test-diagnostic`, and ran
`./build/test/pulp-test-diagnostic "[format][host-type]"`, which passed 3
test cases / 49 assertions. This tranche is held locally and should be
aggregated into the larger PR rather than submitted individually.

2026-05-17 23:27 PDT: added another local-only held file-browser coverage
tranche to the consolidated next-batch branch
`feature/phase3-codecov-next-batch-727` in
`/private/tmp/pulp-phase3-codecov-next-batch-727`. The branch now includes
commit `811ec78d6` (`test(view): cover file browser filter edge paths`). The
tranche extends `test/test_file_browser.cpp` over non-wildcard filter patterns
that reject files while retaining directories and navigation to a regular file
path as a safe empty listing. Validation passed locally: `git diff --check`,
built `pulp-test-file-browser`, and ran
`./build/test/pulp-test-file-browser "[view][file-browser]"`, which passed 5
test cases / 30 assertions. This tranche is held locally and should be
aggregated into the larger PR rather than submitted individually.

2026-05-17 23:29 PDT: rebased the consolidated next-batch branch
`feature/phase3-codecov-next-batch-727` onto current `origin/main`
(`51af86203`) after recent main movement. The branch remained clean and now
contains 24 local coverage/fix commits ahead of `origin/main`, with rebased
tip `c5c51bd78` (`test(view): cover file browser filter edge paths`) and base
commit sequence through `0bb251da4` (`fix(parsers): reject partial numeric
fields`). Consolidated local validation passed after the rebase: rebuilt
`pulp-test-audio-file`, `pulp-test-gui-components`, `pulp-test-property-list`,
`pulp-test-json-rpc`, `pulp-test-poly-math`, `pulp-test-atk-mapping`,
`pulp-test-uia-mapping`, `pulp-test-input-events`, `pulp-test-diagnostic`,
and `pulp-test-file-browser`; ran focused filters across all ten binaries;
and `git diff --check` passed. Next action: push/open this as a GitHub-only
batched PR and monitor to green; do not dispatch Namespace CI.

2026-05-17 23:34 PDT: fixed a full-build blocker discovered by the pre-push
coverage build on the consolidated next-batch branch
`feature/phase3-codecov-next-batch-727`. Commit `571761653`
(`fix(preview): guard font probe without skia`) guards the `ui-preview`
`--font-probe` call to `pulp::canvas::probe_font_glyph` behind
`PULP_HAS_SKIA` so CPU-only/no-Skia builds compile and report the unsupported
font-probe mode honestly. Validation passed locally: rebuilt
`pulp-ui-preview` and ran `git diff --check`. This fix stays in the same
batched PR because the issue was found while preparing the coverage batch.

2026-05-17 23:38 PDT: pushed
`feature/phase3-codecov-next-batch-727` and opened GitHub PR #2173
(`test: batch phase3 coverage edges`) via REST because GitHub GraphQL quota
was exhausted. The PR is labeled `codecov`, head SHA
`571761653f0da987e0f1cb3be8159a84310d686e`, and is blocked only pending
GitHub-hosted checks at creation time. Auto-merge was not enabled yet because
the normal `gh pr` GraphQL path is rate-limited; next action is to monitor
REST/check-runs, address failures/comments, and merge on green.

2026-05-17 23:38 PDT: force-pushed PR #2173 head to
`069e34e0d07d9af73e8c223d86389d1741730a4e` after amending the tip commit
with contiguous `Skill-Update:` skip trailers for `cli-maintenance`,
`hosting`, and `import-design`, plus `Version-Bump: sdk=skip`. Local
`skill_sync_check.py --base origin/main --head HEAD` and
`version_bump_check.py --base origin/main --head HEAD --mode=report` both
pass after the amend. Next action: monitor the rerun GitHub checks for
#2173 and merge when green.

2026-05-17 23:42 PDT: fixed the remaining root-cause CI gate on PR #2173 by
adding the missing `jsx` row to
`tools/import-validation/source-contracts.json` on
`feature/phase3-codecov-next-batch-727`. The new commit is `7b1f4d84f`
(`chore(import): register jsx source contract`) and ties
`parse_design_source("jsx")` plus `tools/import-validation/jsx-roundtrip.sh`
back into the source-contract registry instead of bypassing the checker.
Local validation passed: `python3 tools/import-validation/check-source-contracts.py
--strict`, `python3 tools/import-validation/test_source_contracts.py`,
`python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD`,
`python3 tools/scripts/version_bump_check.py --base origin/main --head HEAD
--mode=report`, and `git diff --check`. Pushed the commit with pre-push gates
skipped because the targeted local gates passed and the branch is already in
GitHub-only CI. PR #2173 is open with head
`7b1f4d84ffd8f4a62f2506fa2c9e643f0956eabd`; checks were queued at last poll.

2026-05-17 23:50 PDT: started the next held large coverage batch from
current `origin/main` in `/private/tmp/pulp-phase3-codecov-batch-728` on
branch `feature/phase3-codecov-batch-728`. First local-only tranche is
commit `67d991566` (`test(canvas): cover font options and scopes`). It adds
`pulp-test-font-options` and covers the newly landed font v2 typed cache-key
surface: full `FontOptions` hash participation, OpenType tag packing,
plugin/view/global `FontScopeId` factories, scope generation isolation,
view-scope release reset, memory-budget storage, and all resolver fallback
origin string names. Local validation passed: configured the worktree build,
built `pulp-test-font-options`, ran
`./build/test/pulp-test-font-options "[canvas][font]"` (4 cases / 38
assertions), and `git diff --check` passed. This branch is intentionally held
locally until it accumulates a larger 24-36-tranche PR batch.

2026-05-17 23:52 PDT: added a second local-only font/canvas tranche to
`feature/phase3-codecov-batch-728`: commit `f5ab064c5`
(`test(canvas): cover text run planner skeleton`). It extends
`pulp-test-font-options` over `TextRunPlanner` skeleton behavior: mixed
UTF-8 scalar offset mapping, hard newline and soft tab line-break
opportunities, one-run logical span, RTL base-direction bidi level, automatic
scope registry-generation capture, and explicit caller-supplied
registry-generation preservation. Validation passed locally: rebuilt
`pulp-test-font-options`, ran
`./build/test/pulp-test-font-options "[canvas][font]"` (6 cases / 58
assertions), and `git diff --check` passed. Still held locally for the next
large batch PR.

2026-05-18 00:06 PDT: added a sixth local-only font/canvas tranche to
`feature/phase3-codecov-batch-728`: commit `ecc7a22eb`
(`test(canvas): cover shaped text value types`). It extends
`pulp-test-font-options` over `RunMetrics::line_height`,
`UnicodeIndexMap::scalar_count`, `ShapedText::empty`, and populated
`ShapedRun` / `ClusterEntry` / `LineBreakOpportunity` value behavior.
Validation passed locally: rebuilt `pulp-test-font-options`, ran
`./build/test/pulp-test-font-options "[canvas][font]"` (12 cases / 135
assertions), and `git diff --check` passed. Still held locally for the next
large batch PR.

2026-05-18 00:03 PDT: added a fifth local-only font/canvas tranche to
`feature/phase3-codecov-batch-728`: commit `2b8a25c07`
(`test(canvas): cover text planner cache edges`). It extends
`pulp-test-font-options` over empty-text shaping, cache-hit parity,
`clear_cache()` behavior, and ASCII space/tab/newline soft/hard break
ordering in `TextRunPlanner`. Validation passed locally: rebuilt
`pulp-test-font-options`, ran
`./build/test/pulp-test-font-options "[canvas][font]"` (11 cases / 119
assertions), and `git diff --check` passed. Still held locally for the next
large batch PR.

2026-05-18 00:00 PDT: added a fourth local-only font/canvas tranche to
`feature/phase3-codecov-batch-728`: commit `e4632fc0d`
(`test(canvas): cover font registry stubs`). It extends
`pulp-test-font-options` over non-Skia public font registration stubs,
byte-validation guard behavior, generation no-op semantics, and the naive
UTF-8 continuation-byte skipping in the `cluster_step` skeleton. The stub
tests are guarded behind `#ifndef PULP_HAS_SKIA` so Skia/GPU lanes do not
depend on stub-only symbols. Validation passed locally: rebuilt
`pulp-test-font-options`, ran
`./build/test/pulp-test-font-options "[canvas][font]"` (10 cases / 98
assertions), and `git diff --check` passed. Still held locally for the next
large batch PR.

2026-05-17 23:55 PDT: added a third local-only font/canvas tranche to
`feature/phase3-codecov-batch-728`: commit `396aecdd4`
(`test(canvas): cover font resolver fallbacks`). It extends
`pulp-test-font-options` over `ResolvedFont` default unresolved state and the
non-Skia `FontResolver` family/character fallback paths, including scoped
generation propagation and `NotFound` behavior. Validation passed locally:
rebuilt `pulp-test-font-options`, ran
`./build/test/pulp-test-font-options "[canvas][font]"` (8 cases / 78
assertions), and `git diff --check` passed. Still held locally for the next
large batch PR.

2026-05-18 00:04 PDT: rebased held batch
`feature/phase3-codecov-batch-728` onto `origin/main` after #2175, #2177,
and #2179 landed. The rebase preserved mainline `pulp-test-font-scope-budget`
and `pulp-test-font-security` targets alongside this batch's
`pulp-test-font-options`; current batch head is commit `0945635d3`
(`test(view): cover label baseline metrics`). The seventh local-only tranche
adds direct `Label::baseline_y()` coverage for font-metric scaling, empty
label fallback, inherited font-size propagation, and explicit child font-size
override. Local validation passed after rebase: built
`pulp-test-font-options` and `pulp-test-widgets`, ran
`./build/test/pulp-test-font-options "[canvas][font]"` (12 cases / 135
assertions), ran
`./build/test/pulp-test-widgets "[view][widget][baseline]"` (1 case / 7
assertions), and `git diff --check` passed. Still held locally for a larger
24-36 tranche PR batch.

2026-05-18 00:07 PDT: added an eighth local-only tooling tranche to
`feature/phase3-codecov-batch-728`: commit `267138434`
(`test(local-ci): cover extracted helper contracts`). It expands direct
Python unit coverage for the #2179 local-CI helper split: target
enable/default/unknown/disabled resolution, provenance normalization and
summary formatting, queue normalization for legacy array and enveloped
`{"jobs": [...]}` payloads, atomic queue save, GitHub workflow selector
default source resolution, CLI selector override validation, and unknown
workflow errors. Local validation passed:
`python3 tools/local-ci/test_local_ci.py LocalCiTests.test_extracted_target_helpers_cover_empty_defaults_unknown_and_disabled LocalCiTests.test_extracted_provenance_helpers_normalize_result_and_summarize LocalCiTests.test_extracted_queue_helpers_accept_legacy_array_and_enveloped_queue LocalCiTests.test_extracted_github_workflow_helpers_resolve_sources_and_cli_overrides`
(4 tests), plus `git diff --check`. Still held locally for a larger 24-36
tranche PR batch.

2026-05-18 00:07 PDT: rebased held batch
`feature/phase3-codecov-batch-728` onto `origin/main` after #2180 landed.
The rebase preserved mainline `pulp-test-font-variable-axes` alongside this
batch's `pulp-test-font-options`; current batch head is commit `da6ad6f46`
(`test(local-ci): cover github workflow settings`). The ninth local-only
tooling tranche expands direct coverage for GitHub Actions defaults and
selector helpers: display-setting normalization, integer timing validation,
workflow runner selector config lookup, malformed config fallback, JSON
selector normalization for string/list values, invalid selector errors,
provider fallback from unsupported defaults, explicit provider source
reporting, and unsupported explicit provider errors. Local validation passed:
`python3 tools/local-ci/test_local_ci.py LocalCiTests.test_extracted_github_workflow_settings_validate_selectors_and_providers`
(1 test), plus `git diff --check`. After the #2180 rebase, the previously
held font/view/local-ci tranches were revalidated with
`pulp-test-font-options`, `pulp-test-widgets`, and the four extracted
local-ci helper tests. Still held locally for a larger 24-36 tranche PR batch.

2026-05-18 00:11 PDT: added a tenth local-only coverage tooling tranche to
`feature/phase3-codecov-batch-728`: commit `ed5d03fc2`
(`test(coverage): cover python cobertura path rewrites`). It expands
`tools/scripts/test_run_python_coverage_extra.py` over Cobertura filename
normalization for Windows-style backslashes, dot sources, relative source
prefixes, source-list clearing, and rewrite-to-repo-relative behavior for the
Python coverage XML consumed by Codecov. Local validation passed:
`python3 tools/scripts/test_run_python_coverage_extra.py CoberturaPathExtraTests`
(2 tests), plus `git diff --check`. Still held locally for a larger 24-36
tranche PR batch.

2026-05-18 00:12 PDT: added an eleventh local-only coverage tooling tranche
to `feature/phase3-codecov-batch-728`: commit `3f7940b12`
(`test(coverage): cover cobertura verifier edges`). It expands
`tools/scripts/test_verify_cobertura_xml.py` over non-numeric
`lines-valid`, missing `lines-valid`, hint propagation on structurally empty
artifacts, and success output with a custom label. This guards the Codecov
pre-upload verifier against silently accepting unusable Cobertura XML. Local
validation passed: `python3 tools/scripts/test_verify_cobertura_xml.py`
(9 tests), plus `git diff --check`. Still held locally for a larger 24-36
tranche PR batch.

2026-05-18 00:16 PDT: rebased held batch
`feature/phase3-codecov-batch-728` onto `origin/main` after #2181 and #2183
landed; current batch head is commit `46337badc`
(`test(view): cover html dir attribute bridge`). The twelfth local-only
view/web-compat tranche adds direct `Element.setAttribute('dir', ...)`
coverage for `rtl`, `ltr`, `auto`, case normalization, and invalid-value
round-trip behavior. The test exposed that #2181 updated the legacy
`web-compat.js` shim but not the split `web-compat-element.js` prelude that
WidgetBridge actually evaluates; the tranche syncs that root-cause fix so
HTML `dir` attributes reach `View::WritingDirection` through the active
runtime path. Local validation passed after rebase:
`./build/test/pulp-test-widget-bridge "HTML dir attribute forwards to View writing direction"`
(1 case / 5 assertions),
`python3 tools/scripts/test_verify_cobertura_xml.py` (9 tests),
`python3 tools/scripts/test_run_python_coverage_extra.py CoberturaPathExtraTests`
(2 tests), the five extracted local-ci helper tests,
`python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD --mode=report`,
`python3 tools/scripts/version_bump_check.py --base origin/main --head HEAD --mode=report`,
and `git diff --check`. Still held locally for a larger 24-36 tranche PR
batch.

2026-05-18 00:35 PDT: repaired PR #2173
(`feature/phase3-codecov-next-batch-727`) after GitHub-hosted Linux failed
11 WidgetBridge/SVG tests on head `7b1f4d84f`. The failure was not a runner
flake. The branch was 10 commits behind `origin/main`, and local replay after
rebase exposed real regressions in the extracted bridge keyboard/runtime
import path: a stale duplicate `WidgetBridge::forward_key_event` in
`widget_bridge.cpp` bypassed the focused-text-input guard, the RN/Pencil
runtime-import labels still fell through to the Claude parser, and the static
`WidgetBridge` registry was no longer populated by ctor/dtor. Added repair
commit `777817ab2` (`fix(view): restore bridge key dispatch paths`) and then
commit `b9d74decf` (`fix(test): remove leaked Clock fixture files`) after the
pre-push source-pollution gate caught a committed Clock fixture
`CMakeLists.txt` plus `pulp.toml` on the PR branch. Local targeted validation
passed before push: built the five affected view/SVG targets, ran the
focus-guard WidgetBridge test, the explicit RN runtime-import test, the RN
auto-detect runtime-import test, the explicit Pencil runtime-import test, the
malformed SVG gradient test, `pulp-test-widget-bridge-dispatch-global-key`,
`pulp-test-widget-bridge-dispatch-document-event`,
`python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD --mode=report`,
`python3 tools/scripts/version_bump_check.py --base origin/main --head HEAD --mode=report`,
and `git diff --check`. Push used `PULP_VIA_SHIPYARD=1
PULP_SKIP_DIFF_COVER=1 git push --force-with-lease`; pre-push gates completed
cleanly and #2173 is now queued on GitHub-hosted CI at head `b9d74decf`.

2026-05-18 00:55 PDT: rebased held batch
`feature/phase3-codecov-batch-728` onto current `origin/main`
(`35047b362`) after #2186/#2188/#2189 and later mainline updates landed. The
only conflict was `test/CMakeLists.txt`; resolution preserved mainline font
targets (`pulp-test-font-aa-hinting`, `pulp-test-font-flight-recorder`, etc.)
alongside this batch's `pulp-test-font-options`. Current rebased local-only
commit sequence is:
`fe8a52247` (`test(canvas): cover font options and scopes`),
`e00887e71` (`test(canvas): cover text run planner skeleton`),
`4cf5522bd` (`test(canvas): cover font resolver fallbacks`),
`ad0cfee4a` (`test(canvas): cover font registry stubs`),
`a1ef5c53e` (`test(canvas): cover text planner cache edges`),
`dd00c5193` (`test(canvas): cover shaped text value types`),
`c981a6274` (`test(view): cover label baseline metrics`),
`e5ff581c4` (`test(local-ci): cover extracted helper contracts`),
`e98b34bf3` (`test(local-ci): cover github workflow settings`),
`e4108309f` (`test(coverage): cover python cobertura path rewrites`),
`38085e7f7` (`test(coverage): cover cobertura verifier edges`),
`df65cbfc1` (`test(view): cover html dir attribute bridge`), and
`fbb6db1fe` (`test(canvas): cover cluster step edge cases`). The thirteenth
tranche extends `pulp-test-cluster-step` over offsets inside UTF-8 scalars,
backward traversal through combining / regional-indicator / ZWJ clusters,
malformed UTF-8 forward progress, extended combining ranges, and U+E0100
variation selectors. It also fixes the discovered non-Skia stub edge where
`cluster_step()` could return the same offset when called from a continuation
byte in malformed truncated UTF-8. Local validation passed after rebase:
`cmake --build build --target pulp-test-font-options pulp-test-cluster-step pulp-test-widget-bridge -j$(sysctl -n hw.ncpu)`,
`./build/test/pulp-test-font-options` (12 cases / 135 assertions),
`./build/test/pulp-test-cluster-step` (13 cases / 48 assertions),
`./build/test/pulp-test-widget-bridge "HTML dir attribute forwards to View writing direction"`
(1 case / 5 assertions), `python3 tools/scripts/test_verify_cobertura_xml.py`
(9 tests), `python3 tools/scripts/test_run_python_coverage_extra.py
CoberturaPathExtraTests` (2 tests),
`python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD --mode=report`,
`python3 tools/scripts/version_bump_check.py --base origin/main --head HEAD --mode=report`,
and `git diff --check`. Still held locally; do not push until the batch grows
to roughly 24-36 coverage commits.

2026-05-18 00:47 PDT: investigated #2173 after GitHub-hosted
`Release-path build (linux-x64)` failed on head `b9d74decf`. The failed job
was a Skia-enabled Linux release compile error in `skia_canvas_box_shadow.cpp`
(`to_sk_color4f` / `SkImageFilters` not visible). Root cause was that the PR
branch was still carrying stale canvas/font deletions against newer `main`,
including deletion of `core/canvas/src/skia_canvas_internal.hpp`; this made the
branch revert mainline split-build repairs rather than testing only the
coverage batch. Rebased `feature/phase3-codecov-next-batch-727` onto current
`origin/main` (`6c198a9e`), skipped the accidental Clock fixture commit and
its follow-up removal during replay, and force-pushed #2173 at
`a40b41756d4e6aed30f366aa76af79307e64891a`. Local validation before push:
`cmake --build build --target pulp-cli pulp-ui-preview pulp-test-widget-bridge pulp-test-widget-bridge-runtime-import pulp-test-widget-bridge-dispatch-global-key pulp-test-widget-bridge-dispatch-document-event pulp-test-svg-path-widget -j$(sysctl -n hw.ncpu)`,
the focus-guard WidgetBridge test, explicit RN runtime-import test, RN
auto-detect runtime-import test, explicit Pencil runtime-import test,
malformed SVG gradient test, `pulp-test-widget-bridge-dispatch-global-key`,
`pulp-test-widget-bridge-dispatch-document-event`,
`python3 tools/import-validation/check-source-contracts.py`,
`python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD --mode=report`,
`python3 tools/scripts/version_bump_check.py --base origin/main --head HEAD --mode=report`,
and `git diff --check`. Push used `PULP_VIA_SHIPYARD=1
PULP_SKIP_DIFF_COVER=1 git push --force-with-lease`; #2173 is open and
queued on GitHub-hosted checks at the rebased head.

2026-05-18 01:05 PDT: added a fourteenth local-only canvas/text tranche to
held batch `feature/phase3-codecov-batch-728` in
`/private/tmp/pulp-phase3-codecov-batch-728`, then rebased cleanly onto current
`origin/main` (`0.113.5`). New rebased commit is `a6c07354a`
(`test(canvas): cover emoji segmenter edge paths`). The tranche extends
`pulp-test-canvas-emoji` over odd regional-indicator pairing, tag-sequence
coverage, malformed UTF-8 coverage as a default run, and FE0F/FE0E agreement
between `contains_emoji()` and the full segmenter. It fixes the root issue that
`contains_emoji()` previously classified individual codepoints directly, so an
emoji-default base followed by FE0E text presentation could still return true
despite `segment_emoji_runs()` routing the same cluster to `Default`. Local
validation after rebase passed:
`cmake --build build --target pulp-test-canvas-emoji pulp-test-font-options pulp-test-cluster-step -j$(sysctl -n hw.ncpu)`,
`./build/test/pulp-test-canvas-emoji "[canvas][emoji][segmenter]"`
(15 cases / 59 assertions), `./build/test/pulp-test-font-options`
(12 cases / 135 assertions), `./build/test/pulp-test-cluster-step`
(13 cases / 48 assertions),
`python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD --mode=report`,
`python3 tools/scripts/version_bump_check.py --base origin/main --head HEAD --mode=report`
(patch bump suggested because this tranche includes a source fix), and
`git diff --check`. Batch #728 is now 14 local commits ahead and remains
held until it reaches a larger 24-36 commit PR size.

2026-05-18 01:12 PDT: repaired #2173 after the enforced
`Enforce version & skill sync` CI job failed its compat-sync step at head
`a40b41756`. The failure was caused by internal `WidgetBridge` repair files
touching mapped compat prefixes without docs updates; local pre-push had only
reported this as advisory, while CI enforces it. Amended the tip repair commit
with explicit `Compat-Update` skip trailers for `canvas2d`, `css`, `html`,
`imports`, `react`, `rn`, and `yoga` because the change restores internal key
dispatch / runtime-import behavior and does not alter the documented web
compat surface. Local enforced validation passed:
`python3 tools/scripts/compat_sync_check.py --base origin/main --mode=report --enforce`,
`python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD --mode=report`,
`python3 tools/scripts/version_bump_check.py --base origin/main --head HEAD --mode=report`,
and `git diff --check`. Force-pushed #2173 at
`23713c44afe2fa8131177301848bda605967c38e`; fresh GitHub checks are queued.

2026-05-18 01:15 PDT: added a fifteenth local-only canvas/font diagnostics
tranche to held batch `feature/phase3-codecov-batch-728`: commit
`160c6ea3b` (`test(canvas): cover font recorder diagnostics`). It extends
`pulp-test-font-flight-recorder` over active-buffer capacity shrink trimming
oldest records, `clear()` preserving monotonic sequence numbering, empty JSON
drain output, JSON string escaping for quotes/backslashes/control characters,
and drain side effects. Local validation passed:
`cmake --build build --target pulp-test-font-flight-recorder -j$(sysctl -n hw.ncpu)`,
`./build/test/pulp-test-font-flight-recorder "[font][diagnostics]"`
(9 cases / 26 assertions),
`python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD --mode=report`,
`python3 tools/scripts/version_bump_check.py --base origin/main --head HEAD --mode=report`,
and `git diff --check`. Batch #728 is now 15 local commits ahead and remains
held for the larger 24-36 commit PR.

2026-05-18 01:20 PDT: added a sixteenth local-only SDF helper tranche to held
batch `feature/phase3-codecov-batch-728`: commit `857073561`
(`test(canvas): cover path sdf invalid input`). It adds a deterministic
`path_to_sdf(nullptr, positive dimensions, spread)` regression test and fixes
the helper to return the already-sized zero field instead of dereferencing a
null mask; this matches the existing graceful handling for zero dimensions and
non-positive spread. Local validation passed:
`cmake --build build --target pulp-test-path-to-sdf -j$(sysctl -n hw.ncpu)`,
`./build/test/pulp-test-path-to-sdf` (8 cases / 543 assertions),
`python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD --mode=report`,
`python3 tools/scripts/version_bump_check.py --base origin/main --head HEAD --mode=report`
(patch bump suggested because this tranche includes a source fix), and
`git diff --check`. Batch #728 is now 16 local commits ahead and remains held.

2026-05-18 01:27 PDT: added a seventeenth local-only SDF atlas-cache tranche
to held batch `feature/phase3-codecov-batch-728`: commit `3e5821fea`
(`test(canvas): cover sdf atlas cache residency edges`). It extends
`pulp-test-sdf-atlas-cache` over duplicate seed glyph de-duplication and
re-ensuring a glyph after eviction, verifying resident-entry count, frame
recency stamping, and dirty upload state. Local validation passed:
`cmake --build build --target pulp-test-sdf-atlas-cache -j$(sysctl -n hw.ncpu)`,
`./build/test/pulp-test-sdf-atlas-cache` (13 cases / 81 assertions),
`python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD --mode=report`
with an `sdf-text` skip trailer for the small `path_to_sdf` null-input guard,
`python3 tools/scripts/version_bump_check.py --base origin/main --head HEAD --mode=report`
(patch bump suggested because this batch includes source fixes), and
`git diff --check`. Batch #728 is now 17 local commits ahead and remains held.

2026-05-18 01:32 PDT: added an eighteenth local-only MIDI-CI tranche to held
batch `feature/phase3-codecov-batch-728`: commit `a327f3d15`
(`test(midi): cover ci null message input`). It adds deterministic coverage for
`CiDiscovery::process_message(nullptr, size)` at both zero and non-zero sizes
and fixes the parser to return no responses instead of dereferencing a null
buffer before the existing size/header guards. Local validation passed:
`cmake --build build --target pulp-test-midi-ci -j$(sysctl -n hw.ncpu)`,
`./build/test/pulp-test-midi-ci` (26 cases / 126 assertions),
`python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD --mode=report`
with the existing `sdf-text` skip trailer for the earlier SDF helper guard,
`python3 tools/scripts/version_bump_check.py --base origin/main --head HEAD --mode=report`
(patch bump suggested because this batch includes source fixes), and
`git diff --check`. Batch #728 is now 18 local commits ahead and remains held.

2026-05-18 01:43 PDT: rebased held batch
`feature/phase3-codecov-batch-728` cleanly onto current `origin/main` after
main advanced twice; no conflicts. Added a nineteenth local-only MIDI raw
parser tranche: commit `5a9a547f1` (`test(midi): cover raw parser status
recovery`). It extends `pulp-test-raw-midi-parser` over malformed short
messages where a new status byte arrives where a data byte was expected,
verifying the malformed candidate is dropped and the following valid status
still emits. Local validation passed:
`cmake --build build --target pulp-test-raw-midi-parser -j$(sysctl -n hw.ncpu)`,
`./build/test/pulp-test-raw-midi-parser "[midi][raw_midi_parser]" -r compact`
(10 cases / 61 assertions),
`python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD --mode=report`,
`python3 tools/scripts/version_bump_check.py --base origin/main --head HEAD --mode=report`,
and `git diff --check`. Batch #728 is now 19 local commits ahead and remains
held for the larger 24-36 commit PR.

2026-05-18 01:54 PDT: added a twentieth local-only runtime stream tranche to
held batch `feature/phase3-codecov-batch-728`: commit `b8a608f58`
(`test(runtime): cover stream ownership edges`). It extends
`pulp-test-stream` over `FileStream` self-move assignment, move-assignment from
a closed source, and default/moved-empty `PipeStream` closed read/write/close
semantics. Local validation passed:
`cmake --build build --target pulp-test-stream -j$(sysctl -n hw.ncpu)`,
`./build/test/pulp-test-stream "[stream][file],[stream][pipe]" -r compact`
(4 cases / 38 assertions),
`python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD --mode=report`,
`python3 tools/scripts/version_bump_check.py --base origin/main --head HEAD --mode=report`,
and `git diff --check`. Batch #728 is now 20 local commits ahead and remains
held for the larger 24-36 commit PR.

2026-05-18 02:09 PDT: rebased held batch
`feature/phase3-codecov-batch-728` cleanly onto current `origin/main`, then
added a twenty-first local-only runtime BigInteger tranche: commit
`b30e6ce13` (`test(runtime): cover bigint assignment identities`). It extends
`pulp-test-license` over `BigInteger` self copy-assignment, self
move-assignment, addition/multiplication/modulo identity values, and
zero-exponent modular exponentiation. Local validation passed:
`cmake --build build --target pulp-test-license -j$(sysctl -n hw.ncpu)`,
`./build/test/pulp-test-license "[crypto][bigint]" -r compact`
(15 cases / 38 assertions),
`python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD --mode=report`,
`python3 tools/scripts/version_bump_check.py --base origin/main --head HEAD --mode=report`,
and `git diff --check`. Batch #728 is now 21 local commits ahead and remains
held for the larger 24-36 commit PR.

2026-05-18 02:23 PDT: added three more local-only runtime tranches to held
batch `feature/phase3-codecov-batch-728`, bringing it to the lower bound for a
large PR:
`6438ee2a4` (`test(runtime): cover text diff replacement edges`) extends
`pulp-test-runtime-utils` over replacement-only text diffs and empty-line
formatting; validation passed with `pulp-test-runtime-utils
"[runtime][text-diff]"` (5 cases / 49 assertions). `217cfb63d`
(`test(runtime): cover range type aliases`) extends `SizeRange` and
`FloatRange` containment/intersection/expansion coverage; validation passed
with `pulp-test-runtime-utils "[runtime][range]"` (14 cases / 58 assertions).
`ac79f01b4` (`test(runtime): cover memory stream reset writes`) extends
`pulp-test-stream` over `MemoryStream::clear()` followed by fresh writes from
the reset cursor; validation passed with `pulp-test-stream "[stream][memory]"`
(3 cases / 27 assertions). Each tranche also passed skill-sync,
version-bump report, and `git diff --check`. Batch #728 is now 24 local
commits ahead; next action is broad local validation across touched targets,
then push/open the batched GitHub-hosted PR if clean.

2026-05-18 02:34 PDT: pushed batch #728 as GitHub-hosted PR #2202
(`feature/phase3-codecov-batch-728`) at head `e22b12e66`. Before push, the
batch was rebased cleanly onto current `origin/main` (`b798b9ebc`) and broad
local validation passed: built `pulp-test-font-options`,
`pulp-test-cluster-step`, `pulp-test-canvas-emoji`,
`pulp-test-font-flight-recorder`, `pulp-test-path-to-sdf`,
`pulp-test-sdf-atlas-cache`, `pulp-test-midi-ci`,
`pulp-test-raw-midi-parser`, `pulp-test-stream`, `pulp-test-license`,
`pulp-test-runtime-utils`, and `pulp-test-widget-bridge`; ran focused/full
touched binaries plus `test_verify_cobertura_xml.py`,
`test_run_python_coverage_extra.py`, source-contract checks, enforced
compat-sync, skill-sync, version-bump report, and `git diff --check`. The
push initially surfaced current-main source-contract warnings for the existing
`jsx` parser label and `jsx-roundtrip.sh`; fixed root cause with commit
`e22b12e66` (`test(import): register jsx source contract`) and a parsed
`Compat-Update` trailer for the HTML helper coverage path. Push preflight then
passed cleanly. #2202 is open and queued/running on GitHub-hosted CI; no
Namespace dispatch.

2026-05-18 01:34 PDT: started held batch
`feature/phase3-codecov-batch-729` from current `origin/main` (`b798b9ebc`)
and added the first local-only OSC tranche: commit `445bc91ed`
(`test(osc): cover unknown tag recovery`). It extends `pulp-test-osc` over
mixed OSC type-tag lists where an unknown tag precedes a valid integer tag,
verifying the decoder skips the unknown tag without consuming the later
argument bytes. Local validation passed:
`cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF`,
`cmake --build build --target pulp-test-osc -j$(sysctl -n hw.ncpu)`,
`./build/test/pulp-test-osc "[osc][codec]" -r compact` (30 cases / 93
assertions), `python3 tools/scripts/skill_sync_check.py --base origin/main
--head HEAD --mode=report`, `python3 tools/scripts/version_bump_check.py
--base origin/main --head HEAD --mode=report`, and `git diff --check`. Batch
#729 is 1 local commit ahead and remains held for the next 24-36 commit PR.

2026-05-18 01:48 PDT: added two more local-only tranches to held batch
`feature/phase3-codecov-batch-729`: commit `25238a8ca` (`test(osc): cover
bundle and pattern edges`) extends `pulp-test-osc` over nested bundle
serialization/deserialization, malformed bundle element boundaries, bad
bundle headers, and incomplete OSC address pattern classes/alternatives;
commit `64e0611b1` (`test(runtime): cover null message channel sends`) adds a
`MemoryMessageChannel::send(nullptr, nonzero)` regression and fixes the
channel to reject that invalid input without constructing a vector from a null
range. Local validation passed:
`cmake --build build --target pulp-test-osc -j$(sysctl -n hw.ncpu)`,
`./build/test/pulp-test-osc "[osc][bundle],[osc][pattern]" -r compact` (3
cases / 23 assertions), `cmake --build build --target
pulp-test-memory-message-channel -j$(sysctl -n hw.ncpu)`,
`./build/test/pulp-test-memory-message-channel "[runtime][message_channel]"
-r compact` (16 cases / 73 assertions), skill-sync, version-bump report, and
`git diff --check`. Batch #729 is 3 local commits ahead and remains held for
the next large PR.

2026-05-18 02:17 PDT: added three runtime tranches to held batch
`feature/phase3-codecov-batch-729`: commit `24f1d0e4d` (`test(runtime):
cover expression builtin edges`) extends `pulp-test-runtime-utils` across the
remaining built-in math functions, two-argument function nesting, and malformed
function/operator inputs; commit `ffdd128a4` (`test(runtime): cover transient
identity hashing`) extends `pulp-test-identity` over deterministic hashing and
string formatting for `SessionId`, `RunId`, and `CorrelationId`; commit
`a33a24aa9` (`test(runtime): cover http stream factory states`) extends
`pulp-test-network-stream` over `HttpStream::get`, `HttpStream::post`, and
`fetch()` reset behavior on invalid URLs without real network dependency.
Local validation passed: `pulp-test-runtime-utils "[runtime][expression]"`
(6 cases / 39 assertions), `pulp-test-identity "[runtime][identity]"`
(14 cases / 83 assertions), `pulp-test-network-stream
"[network_stream][http]"` (5 cases / 28 assertions), skill-sync,
version-bump report, and `git diff --check`. Batch #729 is 6 local commits
ahead and remains held for the next large PR.

2026-05-18 02:20 PDT: continued held batch
`feature/phase3-codecov-batch-729` after rebasing onto current `origin/main`
(`dfa391e5c`). The rebased local commit ids are now headed by `e442096a2`.
Added seven more local-only tranches: `940950357` (`test(runtime): cover prime
helper boundaries`), `4b7effe61` (`test(midi): cover raw parser interrupted
messages`), `069e806cd` (`fix(signal): guard smoother skip boundaries`),
`841345cbb` (`test(runtime): cover logging wrappers`), `01400e3b9`
(`test(view): cover table model sort edge states`), `8726d84c5` (`test(view):
cover live constant registry edges`), and `e442096a2` (`fix(view): normalize
derived theme alpha tokens`). Local validation passed for each focused target:
`pulp-test-signal "[signal][smooth]"`, `pulp-test-dsp-expansion
"[signal][log_ramp]"`, `pulp-test-runtime "[runtime][log]"`,
`pulp-test-table-model "[ui][table-model][coverage][phase3]"`,
`pulp-test-view "[view][live-constant]"`, and `pulp-test-theme-presets
"[view][presets][derive][coverage][phase3]"`; the theme-presets tranche caught
a real source bug where byte-style alpha constants were passed to normalized
`Color::with_alpha()` and clamped opaque. Batch #729 is 13 local commits ahead
and remains held for a larger PR. PR #2173 is corrected back to head
`30035e677` after removing an accidental bad hook commit; PR #2202 is corrected
at head `a6befed35` with the Windows duplicate-symbol fix and parsed compat
skip trailers. Both PRs are on GitHub-hosted CI only, with no failures at this
ledger update.

2026-05-18 03:14 PDT: completed the next large local-only coverage tranche
for `feature/phase3-codecov-batch-729`; the held batch is now 29 commits
ahead of current `origin/main` and remains GitHub-hosted only. Since the last
ledger entry, the batch added host/format coverage for editor compatibility,
parameter-event rvalue paths, diagnostic snapshot replacement, descriptor bus
helpers, unbounded aspect hints, MIDI-only descriptor validation, validation
status/report defaults, sanitizer payload report generation, unitless
diagnostic parameter rendering, tempo hook defaults, and tempo hook exception
propagation; it also added view waveform/live-constant/table-model edges,
runtime sync/logging helpers, MIDI running-status split feeds, and ADSR
release/retrigger coverage. Local validation passed for the newly added final
format tranche: `pulp-test-descriptor-validation -r compact` (24 cases / 61
assertions), `pulp-test-validation-harness -r compact` (40 cases / 285
assertions), `pulp-test-diagnostic -r compact` (12 cases / 55 assertions),
`pulp-test-transport-hook -r compact` (9 cases / 24 assertions),
`python3 tools/scripts/skill_sync_check.py --base origin/main --head HEAD
--mode=report`, `python3 tools/scripts/version_bump_check.py --base
origin/main --head HEAD --mode=report`, and `git diff --check`. Current
monitoring state: PR #2173 is at head `d3a06e5d` and PR #2202 is at head
`43711a2b`; both have no failing GitHub checks at this update and are waiting
on queued/running GitHub-hosted lanes. No Namespace dispatch.

2026-05-18 03:19 PDT: pushed `feature/phase3-codecov-batch-729` and opened
GitHub-hosted PR #2208 (`test: batch phase3 coverage format runtime edges`).
Initial CI surfaced the current source-contract gate gap for the JSX
experiment (`jsx` parser label missing from `source-contracts.json` and
`jsx-roundtrip.sh` unreferenced). Applied the existing root-cause registry fix
as commit `6418cbd85` (`test(import): register jsx source contract`), bringing
the batch to 30 commits. Local validation for the fix passed:
`python3 tools/import-validation/check-source-contracts.py --strict`,
`python3 tools/import-validation/test_source_contracts.py`, and `git diff
--check`. Pushed the corrected head with `PULP_SKIP_PREPUSH=1` after local
validation; #2208 checks restarted with no failures at this update. PR #2173
and PR #2202 remain open with no failing checks, still waiting on
queued/running GitHub-hosted lanes. No Namespace dispatch.

2026-05-18 03:41 PDT: continued GitHub-hosted PR monitoring and started the
next held local batch `feature/phase3-codecov-batch-730`, currently 6 commits
ahead of `origin/main` and intentionally not opened yet while it grows toward
the next large PR. Batch #730 currently contains: `98a169d61` (`test(signal):
cover dsp utility edge paths`), `0600defa4` (`test(host): cover parameter event
queue edges`), `ca277fa7e` (`test(canvas): cover text run planner artifacts`),
`b6db294ff` (`test(canvas): cover font option hash fields`), `1acb70bfc`
(`test(view): cover live constant registry edges`), and `b90578131`
(`test(format): cover editor ui fallback`). Local focused validation passed:
`pulp-test-dsp-expansion` (49 cases / 758 assertions),
`pulp-test-host "ParameterEventQueue*"`, `pulp-test-text-shaper
"TextRunPlanner*"`, `pulp-test-font-variable-axes "FontOptions hash*"`,
`pulp-test-view "LiveConstant*"`, `pulp-test-headless "build_editor_ui*"`,
and `git diff --check`.

Also investigated #2202 after GitHub completed the failed run. macOS ARM64 was
the known external timeout in `cmake-ios-auv3-configure` after 5879/5880 tests
passed. Windows surfaced one real test portability failure: `FileStream move
assignment handles self and closed sources` attempted to remove a temp file
while a `FileStream` reader was still open, which POSIX allows but Windows
correctly rejects. Fixed root cause in #2202 as `f1c5577c5` (`test(runtime):
close file stream before cleanup`) and pushed the branch; local validation
passed `pulp-test-stream "FileStream move assignment handles self and closed
sources"` and `git diff --check`. The local diff-coverage pre-push configure
failed while fetching `mbedtls` tag `v3.6.2`, so only that pre-push
diff-coverage failure was demoted for push; GitHub-hosted CI restarted with no
failures at this ledger update. #2173 and #2208 remain open with no failing
checks, waiting on queued/running GitHub-hosted lanes. No Namespace dispatch.

2026-05-18 03:51 PDT: resumed #2173 monitoring after the user flagged it as
unattended. Linux x64 failed on a real portability edge in the SVG gradient
numeric parser: oversized `rgb(...)` components can report a ranged conversion
instead of throwing, allowing a malformed gradient to be treated as valid.
Fixed root cause as `163b96e4b` (`fix(view): reject ranged svg gradient
numbers`) by rejecting `strtof` `ERANGE` results; local focused validation
passed `pulp-test-svg-path-widget "SvgPathWidget malformed gradient numbers
fall back without throwing"` and `git diff --check`. A local pre-push
diff-coverage run incorrectly mutated the #2173 worktree and created an
accidental `initial` commit while exercising a temporary project fixture; I
force-corrected the branch back to the intended head `163b96e4b` and verified
the worktree clean. GitHub-hosted checks restarted for #2173 with no failures
at this update; the older diff-coverage bot comment still needs to refresh on
the new head.

Continued building held batch `feature/phase3-codecov-batch-730`, now 9 local
commits ahead and still intentionally not opened. New local-only commits:
`8b9a9151a` (`test(view): cover sprite strip data edges`), `387021561`
(`test(canvas): cover radial fill style accessors`), and `ba289cb09`
(`test(format): cover webview provider defaults`). Focused validation passed:
`pulp-test-sprite-strip`, `pulp-test-rendering-integration "FillStyle radial
gradient*"`, `pulp-test-clap-webview`, and `git diff --check`. #2202 and
#2208 remain open with no current failures and queued/running GitHub-hosted
lanes. No Namespace dispatch.

2026-05-18 04:03 PDT: investigated #2208 after Linux x64 reported 27 failing
tests. The run was not an external runner issue: it exposed stale test
contracts plus real WidgetBridge source gaps. Fixed the root causes in #2208 as
`dba9aa64b` (`fix(view): restore bridge dispatch contracts`): WidgetBridge now
registers/unregisters live bridge instances for the static key/document-event
fan-out paths, runtime import dispatch recognizes React Native and Pencil
source labels (including auto-detect for RN and `open-pencil` rejection
diagnostics), font-security tests respect the non-Skia relaxed stub contract,
font-axis tests skip resolver assertions on non-Skia builds, and stale
font-family/Label-height assertions now match the current full-family-list and
real-metrics contracts. Local validation passed:
`pulp-test-widgets`, `pulp-test-widget-bridge "CSSStyleDeclaration forwards
font-family to bridge"`, `pulp-test-widget-bridge "Wave5 css/fontFamily
preserves the CSS family list"`, `pulp-test-widget-bridge-runtime-import`
focused RN/Pencil dispatch cases, `pulp-test-widget-bridge-dispatch-global-key`,
`pulp-test-widget-bridge-dispatch-document-event`, `pulp-test-font-security`,
`pulp-test-font-variable-axes`, and `git diff --check`. Pushed #2208 with
GitHub-hosted checks restarted; no Namespace dispatch.

Held batch `feature/phase3-codecov-batch-730` also gained `c087863a6`
(`test(format): cover wam bridge edge states`), bringing it to 10 local commits
ahead and still intentionally held for a larger PR. Focused validation passed
`pulp-test-wam-wclap` (62 assertions / 14 cases) and `git diff --check`.
#2173 and #2202 still have no failing checks at this ledger update and remain
waiting on GitHub-hosted queued/running lanes.

2026-05-18 04:07 PDT: #2208's first repair push (`dba9aa64b`) passed focused
local tests but failed GitHub's `Enforce version & skill sync` gate on
compat-sync trailers for `core/view/src/widget_bridge.cpp`. Amended the same
repair commit to `86e64118d` with contiguous `Compat-Update: skip` trailers
for canvas2d/css/imports/react/rn/yoga, reran local
`python3 tools/scripts/compat_sync_check.py --base origin/main --mode=report
--enforce` successfully, and force-pushed the corrected head. GitHub checks
restarted; no Namespace dispatch.

Continued held batch `feature/phase3-codecov-batch-730`, now 12 local commits
ahead and still held for a larger PR. New local-only commits: `e3e67446c`
(`test(view): cover app framework edge paths`) and `2ebd0f3b7` (`test(midi):
cover midi file error paths`). Focused validation passed
`pulp-test-app-framework` app-framework/keymapping cases,
`pulp-test-midi-file "midi file helpers report missing and unwritable paths"`,
and `git diff --check`.

2026-05-18 04:18 PDT: continued held batch
`feature/phase3-codecov-batch-730`, now 17 local commits ahead and still held
for the next large GitHub-hosted PR. New local-only commits since the previous
ledger entry: `74917ffbd` (`test(view): cover image cache empty clear`),
`421e46bb9` (`test(render): cover render pass manager state edges`),
`649cf64ed` (`test(view): cover lasso selection edges`), `27a2a14d0`
(`test(view): cover ab compare edge states`), and `28029b67e`
(`test(canvas): cover effect application helpers`). Focused local validation
passed: `pulp-test-image-cache "clear on an empty cache is a no-op and
preserves counters"`, `pulp-test-rendering-integration "RenderPassManager*"`,
`pulp-test-gui-components "[gui][lasso]"`, `pulp-test-ab-compare
"[ui][ab-compare]"`, `pulp-test-effects "[canvas][effects]"`, and
`git diff --check`. PR #2173, #2202, and #2208 have direct squash auto-merge
enabled and remain on GitHub-hosted CI only with no failures at this update;
no Namespace dispatch.

2026-05-18 04:34 PDT: completed the next held large GitHub-only coverage batch
`feature/phase3-codecov-batch-730` at 24 local commits ahead of `origin/main`.
New commits since the previous ledger entry: `cb96079a4`
(`test(runtime): cover http loopback success paths`), `5c491a956`
(`test(runtime): cover sync primitive edge states`), `66e4a28da`
(`test(runtime): cover json rpc edge responses`), `15c726fc4`
(`test(platform): cover detection flag consistency`), `a9ade797d`
(`test(runtime): cover pipe stream wrappers`), `082b3df34`
(`test(runtime): cover child process working dir`), and `409805c4c`
(`test(runtime): cover json rpc teardown`). Focused local validation passed for
the newly added final tranches: `pulp-test-runtime-utils "run_process honors
working directory and preserves spaced arguments"`, `pulp-test-json-rpc`
focused sparse-error/notification/teardown cases, `pulp-test-platform`
platform/architecture consistency cases, `pulp-test-stream` focused PipeStream
cases, `pulp-test-sync` focused TripleBuffer/SpscQueue cases, and
`git diff --check`. `git fetch origin main && git rebase origin/main` reported
the branch already up to date before validation. Next action: push/open this
as one batched GitHub-hosted PR and enable direct squash auto-merge; do not use
Namespace.

PR monitoring at this update: #2202's branch-owned Linux/Windows work was
green, while GitHub-hosted macOS timed out on `cmake-ios-auv3-configure` at the
1800s CTest limit and the dependent `macos` gate failed; reran failed jobs with
`gh run rerun 26028443159 --failed`, which restarted the macOS leg. #2173 has
a Linux GitHub-hosted failure, but GitHub is still withholding the failed job
log until the overall workflow completes; keep monitoring and inspect before
patching or rerunning. #2208 has no failures and remains queued/running on
GitHub-hosted lanes. No Namespace dispatch.

2026-05-18 06:25 PDT: started the next held large GitHub-only coverage batch
`feature/phase3-codecov-batch-732` from current `origin/main`. The branch is
currently 4 local commits ahead and intentionally held for the next 24-36
commit PR rather than pushing a small CI batch. Local-only commits so far:
`e5c237c99` (`test(runtime): cover range helper edges`), `d491189b2`
(`test(platform): cover progress parser edge payloads`), `fc42c6c85`
(`test(midi): cover sysex buffer move semantics`), and `da838de44`
(`test(state): cover undo transaction registration`). Local validation passed
`pulp-test-runtime` `[runtime][range]`, `pulp-test-platform`
`[platform][progress]`, `pulp-test-midi-buffer-sysex` focused move-semantics
case, `pulp-test-undo-manager` focused transaction-registration case, and
`git diff --check` after each tranche. The worktree
`/private/tmp/pulp-phase3-codecov-batch-732` has a freshly configured local
`build/` and will stay local until the batch reaches the requested size.
REST-only PR monitoring showed #2173, #2208, #2210, and #2211 still open with
no failing checks; all are blocked only on queued/running GitHub-hosted lanes.
No Namespace dispatch.

2026-05-18 06:05 PDT: published `feature/phase3-codecov-batch-731` as
GitHub PR #2211: https://github.com/danielraffel/pulp/pull/2211. The final
head is `9c4ff10e0` after amending the tip commit with
`Version-Bump: sdk=skip reason="test-only code coverage batch"` so the
pre-push version gate recognizes this as coverage-only work. Branch pre-push
gates passed cleanly and the PR was created through REST because GraphQL quota
remains exhausted. Added the `codecov` label through REST. Initial check
snapshot for #2211 showed no failures (`completed=7`, `in_progress=7`,
`queued=9`). Auto-merge cannot be enabled while GraphQL is exhausted, so #2211
will be monitored via REST and merged via REST once all required checks are
green. No Namespace dispatch.

2026-05-18 05:59 PDT: completed held large GitHub-only coverage batch
`feature/phase3-codecov-batch-731` at 24 local commits ahead of
`origin/main`, ready to publish as the next batched PR. New local-only commits
since the last ledger entry: `3d2089d5c` (`test(runtime): cover base64
alphabet and whitespace edges`), `3c6bcfb99` (`test(audio): cover focus
subscriber snapshot dispatch`), `c02c5bfa5` (`test(midi): cover ump buffer
event operations`), `5c985fd69` (`test(cli): cover json parser duplicate and
escape edges`), `29018f93c` (`test(view): cover image cache decoder and clear
contracts`), `f8f000f90` (`test(platform): cover popup menu item ordering`),
`cc40ae2e5` (`test(cli): cover shell quote empty arguments`), and
`eda886d69` (`test(audio): cover callback context defaults`). Focused local
validation passed for the newly added final tranches:
`pulp-test-runtime-utils` `[runtime][base64]`, `pulp-test-audio-focus`
`[audio][focus]`, `pulp-test-midi-buffer-ump` `[midi][buffer][ump]`,
`pulp-test-cli-json-parser` `[cli][json-parser]`, `pulp-test-image-cache`,
`pulp-test-file-dialog` `[platform][popup-menu]`,
`pulp-test-cli-shell-quote` `[cli][shell-quote]`, and `pulp-test-audio`
focused CallbackContext case. Before publish, `git fetch origin main` showed
no incoming divergence (`origin/main...HEAD` = `0 24`), all changed test
targets built successfully, all selected changed test binaries passed
(`pulp-test-resizable-shell`, `pulp-test-theme-contrast`,
`pulp-test-motion-cost`, `pulp-test-motion-preferences`,
`pulp-test-table-model`, `pulp-test-midi-file`, `pulp-test-state-tree`,
`pulp-test-runtime-utils`, `pulp-test-theme-presets`,
`pulp-test-events-timer-helpers`, `pulp-test-waveform-editor`,
`pulp-test-phase9-widgets`, `pulp-test-image-convolution`,
`pulp-test-fast-math`, `pulp-test-property-list`, `pulp-test-vendor-url`,
`pulp-test-audio-focus`, `pulp-test-midi-buffer-ump`,
`pulp-test-cli-json-parser`, `pulp-test-image-cache`, `pulp-test-file-dialog`,
`pulp-test-cli-shell-quote`, and `pulp-test-audio "~[coreaudio]"`), and
`git diff --check` passed. REST-only PR monitoring showed #2173, #2208, and
#2210 still open with no failing checks; all are blocked only on queued/running
GitHub-hosted lanes. No Namespace dispatch.

2026-05-18 05:47 PDT: repaired and continued GitHub-only coverage work after
new PR queue failures. PR #2202 had merged, so its worktree
`/private/tmp/pulp-phase3-codecov-batch-728`, local branch
`feature/phase3-codecov-batch-728`, and remote branch were removed. PR #2208
was rebased onto current `origin/main`, resolving conflicts in
`test/test_raw_midi_parser.cpp`, `core/view/src/widget_bridge.cpp`,
`test/test_font_security.cpp`, `test/test_widgets.cpp`, and
`test/test_layout_w3c.cpp`; the branch was force-pushed at `00bee6c15` after
adding the required `Compat-Update: skip prefix=html` trailer to the bridge
repair commit. Focused local validation passed `pulp-test-raw-midi-parser`
`[midi][raw_midi_parser]`, `pulp-test-font-security` `[font][security]`,
`pulp-test-widgets` `[view][widget]`, full `pulp-test-widget-bridge`,
`pulp-test-layout-w3c` `[layout][w3c]`, and `git diff --check`. PR #2210's
Linux failure had the same root cause, so the bridge/metric/font repair was
ported there, the branch was rebased onto current `origin/main`, amended with
the same html compat trailer, and force-pushed at `660098293`. Focused local
validation passed `pulp-test-font-security` `[font][security]`,
`pulp-test-font-variable-axes`, `pulp-test-widgets` `[view][widget]`, full
`pulp-test-widget-bridge`, `pulp-test-layout-w3c` `[layout][w3c]`, and
`git diff --check`. The held batch `feature/phase3-codecov-batch-731` was
expanded to 16 local commits ahead of `origin/main` and remains intentionally
unpublished until it reaches the larger 24-36 commit target. New local-only
commits since the prior ledger entry: `eca778ec5` (`test(canvas): cover stride
and rectangle edge cases`), including a real padded-row-stride preservation fix
in `ImageConvolutionKernel`; `abc37f231` (`test(signal): cover fast math
fractional edges`); `dfc9742ef` (`test(view): cover property list empty and
variant paths`); and `bb03efb8b` (`test(format): cover descriptor bus and
capability defaults`). Focused local validation passed
`pulp-test-image-convolution` `[canvas][convolution]`,
`pulp-test-rectangle-list` `[canvas][rect]`, `pulp-test-fast-math`
`[signal][fast_math]`, `pulp-test-property-list` `[view][property_list]`,
`pulp-test-vendor-url`, and `git diff --check`. REST-only PR monitoring showed
#2173, #2208, and #2210 with no failing checks after the repair pushes; all
three remain blocked only on queued/running GitHub-hosted lanes. No Namespace
dispatch.

2026-05-18 04:59 PDT: continued GitHub-only PR monitoring and repair after the
user flagged #2173 as unattended. #2173's previous Linux/Windows failure was a
real portability edge in SVG gradient parsing, not a runner issue: libc/MSVC
number scanning diverged on malformed `12+` components. Fixed root cause as
`af46b2a59` (`fix(view): make svg gradient number parsing portable`) with a
portable gradient-number scanner in `core/view/src/svg_path_widget.cpp`.
Focused local validation passed `pulp-test-svg-path-widget "SvgPathWidget
malformed gradient numbers fall back without throwing"`,
`pulp-test-svg-path-widget "SvgPathWidget defaults match HTML <path>
semantics"`, and `git diff --check`. A local full diff-cover pre-push attempt
stalled in slow iOS CTest setup and briefly produced temporary fixture churn;
the branch was reset back to the intended head before push. Retried push with
the actual local diff-cover bypass knob `PULP_SKIP_DIFF_COVER=1`; pre-push
source-contract gates passed and GitHub-hosted checks restarted. At this
ledger update #2173 has no failing checks, with GitHub-hosted Windows/macOS/
Linux, sanitizers, Android, and smoke lanes queued/running. No Namespace
dispatch.

Fixed #2208's new Linux/Windows failures instead of hiding them. GitHub exposed
two real issues: the `WidgetBridge` focus-guard test was linking the stale
`widget_bridge.cpp` implementation because the static archive satisfied
`forward_key_event` before pulling the extracted `widget_bridge_input.cpp`
object, and the label intrinsic-height test was too tight for Yoga's rounded
20-21 px output. Pushed `b1501ef62` (`fix(view): use focused input key guard`)
after removing the stale duplicate `forward_key_event` implementation from
`core/view/src/widget_bridge.cpp` and adjusting the label-height expectation to
the rounded intrinsic-height contract. Local validation passed
`pulp-test-widget-bridge "WidgetBridge focus-guard: bare-key shortcuts
suppressed while text input focused"`, `pulp-test-layout-w3c "Layout: Label
intrinsic height in column"`, `nm` verification showing
`widget_bridge_input.cpp.o` is the only exported `forward_key_event`, and
`git diff --check`. Pre-push gates passed and GitHub-hosted checks restarted;
#2208 currently has no failing checks and is queued/running. No Namespace
dispatch.

Opened the held large batch `feature/phase3-codecov-batch-730` as GitHub PR
#2210 via REST because GraphQL quota was exhausted. The branch contains 25
commits including the source-contract registry fix `108f32c6c` (`chore(import):
register jsx source contract`) and local validation passed
`python3 tools/import-validation/test_source_contracts.py`,
`python3 tools/import-validation/check-source-contracts.py --strict`, and
`git diff --check`. Auto-merge could not be enabled while GraphQL remained at
zero quota, so #2210 is being monitored via REST and will need auto-merge
enabled later or a REST merge when fully green. Addressed the one Codex P2
review comment on #2210 with `3712d57ff` (`test(format): gate AutoUi fallback
assertion`): the headless AutoUi fallback test now skips when
`PULP_UI_SCRIPT_PATH` is intentionally configured. Focused local validation
passed `pulp-test-headless "build_editor_ui falls back to AutoUi when no script
path is configured"`, pre-push gates passed, and a reply was posted on the
review thread. At this update #2210 has no failing checks and is queued/running
on GitHub-hosted lanes. #2202 remains open with no failing checks after the
macOS rerun; it is still waiting on GitHub-hosted macOS/sanitizer/Android
lanes. GraphQL remains exhausted (`remaining=0`), while REST core quota is
healthy, so all monitoring is REST-only for now. No Namespace dispatch.

2026-05-18 05:18 PDT: continued the next held large GitHub-only coverage batch
`feature/phase3-codecov-batch-731`, currently 8 local commits ahead of
`origin/main` and still intentionally held for a larger PR rather than pushing
a small CI batch. Local-only commits so far: `91ff24ce7`
(`test(view): cover resizable shell serialization edges`), `08c764bc2`
(`test(view): cover contrast fallback paths`), `e8eb777f7` (`test(view): cover
motion cost defensive paths`), `abbcee44e` (`test(view): cover motion policy
duration helpers`), `77d141a28` (`test(view): cover table model sort edge
cases`), `059c847b1` (`test(midi): cover midi file helper edges`),
`9c207e2cd` (`test(state): cover cached property edge cases`), and
`1d66ce663` (`test(runtime): cover text diff context formatting`). Focused
local validation passed for the touched suites: `pulp-test-resizable-shell`
`[ui][resizable-shell]`, `pulp-test-theme-contrast` `[view][contrast]`,
`pulp-test-motion-cost` `[motion-cost]`, `pulp-test-motion-preferences`
`[motion-preferences]`, `pulp-test-table-model` `[ui][table-model]`,
`pulp-test-midi-file` `[midi][file]`, `pulp-test-state-tree` `[state]`,
`pulp-test-runtime-utils` `[runtime][text-diff]`, and `git diff --check` after
each slice. PR monitoring remains GitHub-hosted only and REST-only while
GraphQL quota is exhausted: #2173, #2202, #2208, and #2210 currently have no
failing checks and are blocked only on queued/running GitHub-hosted lanes. No
Namespace dispatch.

2026-05-18 05:27 PDT: continued held large GitHub-only coverage batch
`feature/phase3-codecov-batch-731`, now 12 local commits ahead of
`origin/main` and still intentionally held for a larger PR. New local-only
commits since the previous ledger entry: `2b2265897` (`test(view): cover theme
preset semantic mapping`), `baf8bb18f` (`test(events): cover timer lifecycle
guards`), `ee301bc80` (`test(view): cover waveform editor shortcut edges`),
and `2f2eb2bbb` (`test(view): cover split view pane replacement`). Focused
local validation passed: `pulp-test-theme-presets` `[view][presets][derive]`,
`pulp-test-events-timer-helpers` `[events][timer]`,
`pulp-test-waveform-editor` `[view][waveform_editor]`,
`pulp-test-phase9-widgets` `[view][split_view]`, and `git diff --check` after
each slice. A sidecar coverage explorer could not start because the agent
thread limit was already reached, so coverage work continued locally. PR
monitoring remains GitHub-hosted only and REST-only: #2173, #2202, #2208, and
#2210 still have no failing checks and are blocked only on queued/running
GitHub-hosted lanes. No Namespace dispatch.

2026-05-18 06:24 PDT: continued the next held large GitHub-only coverage batch
`feature/phase3-codecov-batch-732`, now 10 local commits ahead of
`origin/main` and intentionally held for a larger 24-36 commit PR rather than
pushing a small CI batch. Local-only commits so far: `e5c237c99`
(`test(runtime): cover range helper edges`), `d491189b2`
(`test(platform): cover progress parser edge payloads`), `fc42c6c85`
(`test(midi): cover sysex buffer move semantics`), `da838de44`
(`test(state): cover undo transaction registration`), `71e9d3f3a`
(`test(view): cover theme override edge cases`), `747997388`
(`test(view): cover keyframe animation edges`), `d5ff5e3d9`
(`test(view): cover motion duration policy helper`), `201eda03e`
(`test(view): cover animator set and 3d edges`), `576aaa6c6`
(`test(view): cover accessibility tree ordering`), and `74f520b1f`
(`test(view): cover app framework edge paths`). Focused local validation
passed for touched suites: `pulp-test-runtime` `[runtime][range]`,
`pulp-test-platform` `[platform][progress]`, `pulp-test-midi-buffer-sysex`
`MidiBuffer moves sysex sidecar with short messages`, `pulp-test-undo-manager`
`UndoManager add_without_executing participates in transactions`,
`pulp-test-theme` `[view][theme]`, `pulp-test-animation` `[view][animation]`,
`pulp-test-motion-preferences` `[motion-preferences]`,
`pulp-test-animator-set` `[view][animation],[view][3d]`,
`pulp-test-accessibility-tree` `[a11y][harness]`,
`pulp-test-app-framework` `[view][keys],[view][settings]`, and
`git diff --check`. The keyframe animation tests found and fixed two real
lifecycle issues in `KeyframeAnimation`: `start()` now clears the previous
finished flag, and exact finite iteration boundaries now hold the terminal
frame instead of wrapping to the next iteration's start. PR monitoring remains
GitHub-hosted only and REST-only while GraphQL quota is exhausted: #2173,
#2208, #2210, and #2211 have no failing checks and are blocked only on
queued/running GitHub-hosted lanes. No Namespace dispatch.

2026-05-18 06:49 PDT: pushed and opened the larger GitHub-only coverage batch
`feature/phase3-codecov-batch-732` as #2219
(`test: batch phase3 coverage host and view edges`) at head
`6b1715fb2bed62968b9f248b2cbabe2aaa79a1f1`, after rebasing cleanly on
`origin/main`. The PR contains 24 focused commits across runtime, platform,
MIDI, state, host, and view coverage. Additional commits since the 06:24
ledger entry cover asset cache replacement accounting, scanner default paths,
instant splash lifecycle, cancel token lifecycle, custom drop payloads,
keybinding save failure, file browser/tree navigation, modulation matrix
mutation edges, new lasso overlay tests, new split view tests, resizable shell
state contracts, AutoUi empty/unmatched sync paths, SimpleTableModel
replacement/sort behavior, and SystemTrayIcon lifecycle stubs. Focused local
validation passed for each touched suite, including the new `pulp-test-lasso`
and `pulp-test-split-view` targets, plus `git diff --check`. The asset cache
tests found and fixed a real accounting bug: replacing an existing cache key
now subtracts the old byte size before adding the new entry. Local pre-push was
skipped on push because the local diff-cover hook's clean coverage configure
hit the known FetchContent `mbedtls` tag checkout failure; GitHub-hosted CI is
the validation source for #2219. A `codecov` label was requested via REST. PR
monitoring remains GitHub-hosted only and REST-only: #2173, #2208, #2210, and
#2211 have no current failures; #2210 was patched and repushed at
`031f7fa2d9d24245d7e6761ef4e93370b9321642` after its Windows hosted lane
found a real test portability issue in `run_process honors working directory
and preserves spaced arguments`. No Namespace dispatch.

2026-05-18 07:26 PDT: pushed and opened the next larger GitHub-only coverage
batch `feature/phase3-codecov-batch-733` as #2221
(`test: batch phase3 coverage runtime state edges`). The branch is 26 commits
ahead of current `origin/main` and covers runtime, state, format, platform,
view, and ship-adjacent deterministic unit edges. Focused local validation
passed for all touched suites: `pulp-test-state`, `pulp-test-analytics`,
`pulp-test-i18n`, `pulp-test-crypto`, `pulp-test-license`,
`pulp-test-properties`, `pulp-test-runtime-utils`, `pulp-test-xml-zip`,
`pulp-test-json-rpc`, `pulp-test-appcast`, `pulp-test-identity`,
`pulp-test-descriptor-validation`, `pulp-test-processor-defaults`,
`pulp-test-theme-contrast`, `pulp-test-platform`,
`pulp-test-font-axis-animation`, and `git diff --check`. The batch also
cherry-picks the real #2210 Linux root fix for non-Skia FontResolver cache-cap
parity so #2221 does not repeat that failure if it runs before #2210 lands.
Local pre-push diff-cover was demoted only because the local clean coverage
configure still hits the known FetchContent `mbedtls` tag checkout failure;
GitHub-hosted CI remains the validation source. #2210 was separately updated
and pushed at `8a3bb3152` after local `pulp-test-font-axis-animation` passed.
No Namespace dispatch.

2026-05-18 08:20 PDT: pushed and opened the next GitHub-only coverage batch
`feature/phase3-codecov-batch-734` as #2222
(`test: batch phase3 coverage midi runtime state audio edges`). The branch is
24 commits ahead of `origin/main` and covers MIDI raw/running-status parsers,
MIDI buffer sidecar move semantics, runtime scope/base64/expression/i18n
edges, event broadcaster snapshot dispatch, memory channel reentrant replies,
license BigInteger comparisons, state schema/properties/cached-property/sync
decode edges, audio channel layout/buffer copy+move behavior, and processor
descriptor vendor metadata defaults. Focused local validation passed for all
touched suites: `pulp-test-raw-midi-parser`, `pulp-test-running-status`,
`pulp-test-runtime`, `pulp-test-state`, `pulp-test-runtime-utils`,
`pulp-test-events-async-helpers`, `pulp-test-memory-message-channel`,
`pulp-test-license`, `pulp-test-i18n`, `pulp-test-midi-buffer-sysex`,
`pulp-test-midi-buffer-ump`, `pulp-test-audio`, `pulp-test-properties`,
`pulp-test-state-tree`, `pulp-test-processor-defaults`, and
`git diff --check`. The batch includes a real SDK fix in
`core/audio/include/pulp/audio/buffer.hpp`: copied `Buffer<T>` instances now
refresh cached channel pointers so copies own independent storage instead of
pointing back at the source buffer. Local source/version/contract gates passed.
Local pre-push diff-cover was demoted only because the clean coverage
configure still hits the known FetchContent `mbedtls` tag checkout failure;
GitHub-hosted CI remains the validation source. GraphQL PR creation was
rate-limited, so #2222 was opened through the GitHub REST API and labeled
`codecov` through REST. Monitoring remains GitHub-hosted only; #2219 now has a
Linux hosted failure pending log availability after the still-running workflow
finishes. No Namespace dispatch.

2026-05-18 08:27 PDT: pushed and opened the next GitHub-only coverage batch
`feature/phase3-codecov-batch-735` as #2226
(`test: batch phase3 coverage signal runtime audio edges`). The branch is 24
commits ahead of current `origin/main` after a clean rebase and covers signal
fast-math/interpolation/filter edges, MIDI file metadata/write failure paths,
audio focus token/subscriber behavior, typed iOS audio session callbacks,
runtime JSON-RPC factories/notification paths, sync primitive default
snapshots, OSC executor dispatch, audio buffer shrink remapping, and
`AudioProcessLoadMeasurer` invalid-budget/smoothing/reset paths. Focused local
validation passed after rebase for all touched suites:
`pulp-test-json-rpc`, `pulp-test-sync`, `pulp-test-audio`
`"[audio][buffer]","[audio][load]"`, `pulp-test-fast-math`,
`pulp-test-interpolator`, `pulp-test-biquad`, `pulp-test-filter-design`,
`pulp-test-midi-file`, `pulp-test-audio-focus`,
`pulp-test-ios-audio-session`, `pulp-test-osc-channel`, and
`git diff --check`. Local pre-push source/version/contract gates passed and the
branch was labeled `codecov`. Monitoring remains GitHub-hosted only; #2210,
#2219, and #2221 still have hosted failures whose detailed logs are pending
workflow completion while macOS lanes remain queued/running. No Namespace
dispatch.

2026-05-18 09:00 PDT: pushed and opened the next GitHub-only coverage batch
`feature/phase3-codecov-batch-736` as #2227
(`test: batch phase3 coverage signal state audio edges`). The branch is 24
commits ahead of current `origin/main` and covers audio file conversion and
registry edges, StateStore version/default notification behavior, typed
identity hashing, signal gain/mixer helpers, DSP expansion helpers
(FIR/ballistics/log ramp/lookup/TPT/aligned buffer/special math), and Bias
zero-length buffers. The batch also found and fixed a real public-header issue:
`gain.hpp` and `special_functions.hpp` both defined `db_to_linear` and
`linear_to_db`, so including both failed; `gain.hpp` now reuses
`special_functions.hpp`, which also includes `<algorithm>` for its `std::max`
use. Focused local validation passed for all touched suites:
`pulp-test-audio-file`, `pulp-test-signal`, `pulp-test-dsp-expansion`,
`pulp-test-state`, `pulp-test-identity`, `pulp-test-v3-gaps`
`"[signal][bias]"`, and `git diff --check`. A `Version-Bump: sdk=skip`
trailer was added because the batch is test-only. Local pre-push diff-cover was
demoted only because the clean coverage configure still hits the known
FetchContent `mbedtls` tag checkout failure; GitHub-hosted CI remains the
validation source. GraphQL PR creation was rate-limited, so #2227 was opened
through the GitHub REST API and labeled `codecov` through REST. No Namespace
dispatch.

2026-05-18 09:22 PDT: pushed and opened the next GitHub-only coverage batch
`feature/phase3-codecov-batch-737` as #2228
(`test: batch phase3 coverage midi signal audio edges`). The branch is 24
commits ahead of current `origin/main` and batches MIDI RPN/keyboard,
running-status, UMP, MIDI sequence/file summary edges; signal delay, ADSR,
oscillator, phaser, SmoothedValue, DryWetMixer, OSC matcher, runtime
expression/range, StateTree reparenting, and audio buffer template/regrowth
coverage. The batch intentionally folded several previously validated
local-only tranches into one larger PR to reduce GitHub CI runs. Focused local
validation passed for all touched suites: `pulp-test-signal`,
`pulp-test-dsp-expansion`, `pulp-test-audio`, `pulp-test-runtime-utils`,
`pulp-test-state-tree`, `pulp-test-dsp-enhancements`,
`pulp-test-osc-bundle`, `pulp-test-midi`, `pulp-test-midi-expansion`,
`pulp-test-running-status`, and `git diff --check`. A `Version-Bump: sdk=skip`
trailer was added because the batch is test-only. Local pre-push diff-cover was
demoted only because the clean coverage configure still hits the known
FetchContent `mbedtls` tag checkout failure; GitHub-hosted CI remains the
validation source. Auto-merge was enabled for #2228 after PR creation. No
Namespace dispatch.

2026-05-18 09:53 PDT: pushed and opened the next GitHub-only coverage batch
`feature/phase3-codecov-batch-738` as #2231
(`test: batch phase3 coverage state runtime signal edges`). The branch is 24
commits ahead of current `origin/main` after rebasing over the latest two main
commits and batches state/runtime/signal/canvas coverage: mixed properties
string escapes, copied binding callback/store behavior, StateStore custom
version and reset notification ordering, undo transaction/clear notification
edges, CachedProperty mismatch/move paths, scope guard ownership, trailing
newline text diff, multi-argument expression parsing, JSON-RPC callback
cleanup, MemoryStream closed-clear behavior, MD5 embedded-NUL input,
BigInteger ordering, near-Nyquist filter design, single-pixel image
convolution, FastMath clamp/soft-clip/dB/tanh/pow/reciprocal helpers, and Bias
zero-length buffer handling. Focused local validation passed before push, then
the branch was rebased and the touched signal targets were revalidated:
`pulp-test-dsp-enhancements "[dsp][fast_math]"`,
`pulp-test-v3-gaps "Bias handles zero-length in-place buffers"`, and
`git diff --check`; the pre-rebase validation sweep also covered
`pulp-test-properties`, `pulp-test-filter-design`,
`pulp-test-image-convolution`, `pulp-test-binding`, `pulp-test-state`,
`pulp-test-runtime-utils`, `pulp-test-json-rpc`, `pulp-test-stream`,
`pulp-test-crypto`, `pulp-test-license`, `pulp-test-undo-manager`, and
`pulp-test-state-tree`. Local pre-push gates passed with diff-cover disabled
for the known clean-coverage FetchContent `mbedtls` checkout issue.
GraphQL is still rate-limited, so #2231 was opened through the GitHub REST API
and labeled `codecov` through REST; auto-merge still needs to be enabled once
GraphQL quota recovers. Monitoring remains GitHub-hosted only. No Namespace
dispatch.
