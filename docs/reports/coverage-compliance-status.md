# Coverage Compliance Status

Last reviewed: 2026-05-05 07:24 PDT

This is the durable tracker for the repo-wide coverage compliance
program under `#641`.

## Current Phase

- Phase 1 - Representation / Codecov truth: complete.
- Phase 2 - Gap planning: complete enough to execute from the tracker
  map and tranche issues.
- Phase 3 - Gap closure: active. The work is now small, focused
  coverage PRs that move represented components toward their tier
  targets.

The authoritative live tracker remains `#641`. The high-leverage
host/format/view tracker remains `#493`. Component tranche trackers are:

- `#640` audio / platform
- `#642` events
- `#643` CLI / tools
- `#644` ship
- `#645` MIDI / signal
- `#646` render
- `#493` host / format / view

## Goal

Get Codecov as close as practical to the intended first-party local
source surface, then use that corrected baseline to plan and close the
real measured test gaps.

Target tiers remain unchanged in `ci/coverage-targets.yaml`:

- `80%`: `audio`, `format`, `host`, `midi`, `signal`, `platform`
- `70%`: `render`, `view`, `cli`
- `50%`: `events`, `runtime`, `state`, `canvas`, `osc`, `ship`, `tools`

## Latest Complete Codecov Snapshot

Latest complete Codecov `main` report observed while updating this doc:

- commit: `e9c994d1bb2c42f1a50e4775a5886b6348808fb0`
- PR: `#851` - `test(signal): cover meter channel-count guards`
- state: complete
- overall tracked coverage: `48.40%`
- covered lines: `34,608 / 71,492`
- missed lines: `35,511`
- partial lines: `1,373`
- files: `560`
- sessions: `4`

Current component coverage from the same Codecov API snapshot:

| Component | Coverage | Target | Gap |
| --- | ---: | ---: | ---: |
| `audio` | `37.20%` | `80%` | `42.80` |
| `platform` | `38.95%` | `80%` | `41.05` |
| `host` | `44.16%` | `80%` | `35.84` |
| `cli` | `40.84%` | `70%` | `29.16` |
| `format` | `51.92%` | `80%` | `28.08` |
| `view` | `45.17%` | `70%` | `24.83` |
| `midi` | `55.72%` | `80%` | `24.28` |
| `render` | `64.43%` | `70%` | `5.57` |
| `signal` | `75.40%` | `80%` | `4.60` |
| `tools` | `46.93%` | `50%` | `3.07` |

Components currently above their configured Phase 3 tier floor in this
snapshot include `canvas`, `events`, `runtime`, `state`, `osc`, and
`ship`. Non-tier reporting components include `android`, `apple`,
`linux`, `windows`, `dsl`, and `inspect`.

## Queue State

The active queue is intentionally fluid during Phase 3. Treat `#641`
and the component tracker comments as the live source of truth, then
verify with GitHub before reporting or merging:

```bash
gh pr list --repo danielraffel/pulp --label codecov --state open \
  --json number,title,headRefOid,mergeStateStatus,mergeable,updatedAt,url
```

Do not preserve stale point-in-time queue lists in this document. When
a queue snapshot is useful, post it as a tracker comment with concrete
dates, PR numbers, and head SHAs.

## Namespace Pause Override

As of 2026-05-04 16:09 PDT, the Phase 3 Codecov queue remains paused
to keep Namespace capacity available for other projects. During the
pause, do not dispatch CI, rerun workflows, update PR branches, push,
or merge Codecov tranches. Continue only read-only queue checks, local
validation in existing worktrees, and ledger reconciliation.

Use `docs/reports/phase3-codecov-queue-pause-ledger.md` for the
current paused queue snapshot and resume notes. The normal operating
loop below resumes only after Namespace capacity returns.

Latest local-only progress recorded during the pause: #645 signal
tranche `local/phase3-linkwitz-riley-edges-645` at `7b1a82a3`, refreshed from
`385c51a7` onto current `origin/main` `0447498e`, covering
Linkwitz-Riley reset/history and cutoff boundary finite-output behavior,
plus #493 format tranche `local/phase3-validation-harness-json-493` at
`4e753dd3`, refreshed from `974cf572` onto current `origin/main` `0447498e` and
covering ValidationHarness report metadata and entry JSON escaping,
plus #493 host tranche `local/phase3-signal-graph-guards-493`
at `9f33226f`, refreshed from `247be833` onto current `origin/main` `0447498e` and
covering SignalGraph guard/default behavior. All remain unpushed and
undispatched. Additional #493 view progress is queued locally as
`local/phase3-widgets-render-paths-493` at `15ca568b`, refreshed from
`55a90f79` onto current `origin/main` `0447498e`, then refreshed again
onto current `origin/main` `50ff5822`, and covering widget custom-shader
fallbacks, minimal render-style branches, and knob/fader/toggle
interaction edges, plus
`local/phase3-audio-bridge-edges-493` at `2d00e9ac`, refreshed from
`cba288df` onto current `origin/main` `0447498e`, then refreshed again
onto current `origin/main` `50ff5822`, and covering AudioBridge
first-pop, max-channel clamp, zero-sample analysis, and MeterBallistics
tiny-value clamp paths. Both remain unpushed and undispatched.
Additional #645 signal progress is queued locally as
`local/phase3-adsr-edges-645` at `bd746f3c`, refreshed from
`e4844caa` onto current `origin/main` `0447498e`, covering ADSR
immediate stage, idle `note_off`, and reset edge paths, plus
`local/phase3-noise-gate-edges-645` at `717067d6`, refreshed from
`98f73bb8` onto current `origin/main` `0447498e`, covering NoiseGate
range clamp, instant timing, reset, silence, and buffer paths, plus
`local/phase3-modulation-reverb-edges-645` at `9cfcc171`, refreshed from
`971488d5` onto current `origin/main` `0447498e`, covering
Chorus dry/reset/phase-wrap behavior and Reverb zero-decay, damping
clamp, dry-mix, and reset paths, plus
`local/phase3-oscillator-edges-645` at `8f2e043`, refreshed from
`8f51cba4` onto current `origin/main` `0447498e`, covering
Oscillator reset, phase wrap, getter, and PolyBLEP edge paths, plus
`local/phase3-signal-helper-edges-645` at `be2d441f`, created from
current `origin/main` `0447498e`, covering FirFilter empty-coefficient
passthrough/reset, LookupTable indexed clamps and zero-length buffer
no-ops, and LadderFilter buffer/reset/resonance-clamp finite-output
paths. All five remain unpushed and undispatched.
Additional #646 render progress is queued locally as
`feature/phase3-sdl3-surface-fallback-646` at `1dc45105`, refreshed
from `908b3a49` onto current `origin/main` `0447498e`, covering SDL3
native-surface validity and null-window fallback behavior. It has no
remote branch ref and remains unpushed and undispatched.
Additional #640 audio progress is queued locally as
`local/phase3-audio-focus-dispatch-640` at `d766f4b9`, refreshed from
`1ab6e24b` onto current `origin/main` `0447498e`, covering
AudioFocusRegistry inactive-listener skip behavior when a listener is
removed or the registry is reset during dispatch. It remains unpushed and
undispatched. Additional #640 audio progress is queued locally as
`local/phase3-audio-data-shape-640` at `cdfa7ee1`, created from current
`origin/main` `0447498e`, covering AudioFileData helper shape semantics
and WAV writer first-channel-empty rejection. It remains unpushed and
undispatched. Additional #640 audio progress is queued locally as
`local/phase3-aiff-pcm-edges-640` at `2dd20995`, created from current
`origin/main` `0447498e`, covering AIFF invalid COMM metadata and
unsupported PCM bit-depth rejection. It remains unpushed and
undispatched. Additional #640 audio progress is queued locally as
`local/phase3-streaming-writer-reopen-640` at `6f8dad35`, created from
current `origin/main` `0447498e`, covering StreamingWriter close-on-reopen
and failed-open state/header finalization behavior. It remains unpushed
and undispatched. Additional #640 audio progress is queued locally as
`local/phase3-frame-fill-edges-640` at `1611b10d`, created from current
`origin/main` `0447498e`, covering `zero_fill_short_read()` tail-fill,
read-count clamp, null-buffer, and non-positive-dimension guard paths. It
remains unpushed and undispatched. Additional #643 tools progress is
queued locally as
`local/phase3-audio-tools-model-store-643` at `9743ccd7`, refreshed from
`a88ddfe8` onto current `origin/main` `0447498e`, covering `tools/audio`
model registry URL resolution, legacy/malformed model metadata,
model/bundle JSON serialization/defaults, and excerpt-find guard and
unsupported-input paths. It remains unpushed and undispatched.
Additional #643 CLI progress is queued locally as
`local/phase3-cli-config-command-643` at `76b36dcf`, refreshed from
`af1ef6f8` onto current `origin/main` `0447498e`, covering `pulp config`
set/get/list, snooze clearing, malformed update keys, and invalid
update-key diagnostics against an isolated `PULP_HOME`. It remains
unpushed and undispatched.
Additional #643 static CI guard progress is queued locally as
`local/phase3-ruleset-drift-config-643` at `012b224a`, refreshed from
`390b68f9` onto current `origin/main` `0447498e`, covering
branch-protection JSON/workflow invariants for ruleset drift checks. The
official Python coverage runner intentionally excludes
`tools/scripts/test_*.py`, so this guard has no measured non-test source
surface in that focused runner. It remains unpushed and undispatched.
Additional #640 platform progress is queued locally as
`local/phase3-platform-environment-dispatch-640` at `422a64cc`,
refreshed from `b271d2e6` onto current `origin/main` `0447498e`,
covering Environment token self-move, listener removal before dispatch,
and reset-during-dispatch skip behavior. It remains unpushed and
undispatched. Additional #640 platform progress is queued locally as
`local/phase3-child-process-read-output-640` at `4008bd73`, refreshed from
`55c46d02` onto current `origin/main` `0447498e`, covering
`ChildProcess::read_available_output()` while stdout is available
before process completion. It remains unpushed and undispatched.
Additional #645 MIDI progress is queued locally as
`local/phase3-midi-ci-edges-645` at `f66f0d4`, refreshed from
`c3f7e68b` onto current `origin/main` `0447498e`, covering MIDI-CI
malformed header rejection, directly addressed discovery inquiries,
short discovery replies, and reserved-byte profile matching. It remains
unpushed and undispatched. Additional #645 MIDI/MPE progress is queued
locally as `local/phase3-mpe-allocator-edges-645` at `dcacdaa0`,
refreshed from `83ce06b3` onto current `origin/main` `0447498e`, fixing
the MpeVoiceAllocator release-steal glide refcount path, documenting the
releasing-steal invariant in the MPE skill, and covering unmatched
MpeGlideDetector note-off/reset behavior. It remains unpushed and
undispatched. Additional #493 host scanner progress is
queued locally as `local/phase3-host-scanner-order-493` at `97d9f833`,
refreshed from `c3b79d68` onto current `origin/main`, then refreshed
again to `1dfeb616` on `origin/main` `0447498e`, covering
PluginScanner VST3/LV2 format-lane merging, final name ordering, LV2
URI identity, VST3 stem fallback identity, and hermetic scanner fixture
paths. It remains unpushed and undispatched. Additional #493 host progress
is queued locally as `local/phase3-background-scanner-restart-493` at
`b109a24f`, created from current `origin/main` `0447498e`, covering
BackgroundScanner restart after a completed-but-unjoined worker. It
remains unpushed and undispatched. Additional #493 host progress is
queued locally as `local/phase3-plugin-slot-dispatch-493` at
`b6ef9689`, created from current `origin/main` `0447498e`, covering
PluginSlot invalid descriptor fail-closed dispatch across CLAP, AU,
AUv3, VST3, and LV2 loader paths. It remains unpushed and
undispatched.
Additional #643 CLI/tools progress is queued locally as
`local/phase3-fetchcontent-cache-edges-643` at `e5b13514`, refreshed
from `b056ca59` onto current `origin/main` `0447498e`, covering
FetchContent cache fallback entry parsing, live symlink classification,
file-backed declared-ref parsing, label fallbacks, and symlink removal
without deleting targets. It remains unpushed and undispatched.
Additional #493 view progress is queued locally as
`local/phase3-ui-components-edges-493` at `a4ae8412`, refreshed from
`58940ee2` onto current `origin/main` `0447498e`, covering ComboBox
popup handoff/typeahead no-op, ScrollView scrolled-child pointer-event
hit testing and paint clipping/visibility, and ListBox boundary-key and
out-of-range mouse guards. It remains unpushed and undispatched.
Additional #493 view/widget progress is queued locally as
`local/phase3-phase9-widget-edges-493` at `3b2fa68f`, refreshed from
`17e25c73` onto current `origin/main` `0447498e` and covering SplitView drag
minimum clamps, miss/drag guards, divider grip paint branches, and
PropertyList boolean editing, category height/paint, and scalar value
formatting paths. It remains unpushed and undispatched.
Additional #493 view/gui progress is queued locally as
`local/phase3-gui-components-edges-493` at `b0e683e9`, refreshed from
`7e94a781` onto current `origin/main` `0447498e` and covering TableListBox header
sorting, selection and out-of-range row guards, scaled/aligned painting,
and ConcertinaPanel invalid-index, content visibility/layout, paint, and
mouse hit paths. It remains unpushed and undispatched.
Additional #493 view progress is queued locally as
`local/phase3-live-constant-editor-493` at `4ee6e846`, refreshed from
`29af1db8` onto current `origin/main` `0447498e` and covering LiveConstantRegistry
duplicate registration, clamp, callback, missing key, reset, and
reset-all paths, plus LiveConstantEditor visibility, paint, slider drag,
header guard, and missing-row drag paths. It remains unpushed and
undispatched.
Additional #493 view/code-editor progress is queued locally as
`local/phase3-code-editor-doc-mru-493` at `f09fd8d6`, refreshed from
`e8e3545b` onto current `origin/main` `0447498e` and covering FileBasedDocument
successful load/save-as dirty-state behavior and RecentlyOpenedFilesList
remove/missing-path behavior. It remains unpushed and undispatched.
Additional #493 view/graph-editor progress is queued locally as
`local/phase3-graph-editor-paint-493` at `d9ade421`, refreshed from
`2d21c893` onto current `origin/main` `0447498e` and covering GraphEditorView
auto-layout/manual-position preservation, unnamed node/multi-port
painting, and feedback/MIDI edge paint colors. It remains unpushed and
undispatched.
Additional #493 view/widget progress is queued locally as
`local/phase3-new-widgets-input-493` at `217ae169`, refreshed from
`7b5927ad` onto current `origin/main` `0447498e` and covering MidiKeyboard vertical
drag release/miss behavior and note-name/highlight painting, plus
FileDropZone rejected-drop reset behavior and idle/valid/invalid/no-icon
paint paths. It remains unpushed and undispatched.
Additional #493 view/file-browser progress is queued locally as
`local/phase3-file-browser-paint-493` at `091fdb77`, refreshed from
`57a12010` onto current `origin/main` `0447498e` and covering FileBrowser
sorted-row paint clipping and MultiDocumentPanel active/inactive tab
paint output. It remains unpushed and undispatched.
Additional #493 view progress is reconciled locally on the #1285 resume
path as `feature/phase3-splash-screen-coverage-493` at `6ee48b54`,
refreshed from paused remote head `d3cd9f47` onto current `origin/main`
`50ff5822` and incorporating/superseding
`local/phase3-splash-screen-493` (`1dd00e70`), after superseding local
refresh `7cba95e3` on `0447498e`. It covers SplashScreen
advance/dismiss callback behavior, dismiss-on-click gating, and
text/image paint output. It remains unpushed and undispatched; #1285
still points at `d3cd9f47` until Namespace capacity returns.
Additional #493 view progress is queued locally as
`local/phase3-appearance-manager-493` at `c0b6bece`, refreshed from
`ceb05add` onto current `origin/main` `0447498e` and covering AppearanceTracker
repeated lock callbacks and locked poll no-op behavior, plus ThemeManager
locked-theme, locked-appearance callback, and unlock behavior. It remains
unpushed and undispatched.
Additional #493 view progress is queued locally as
`local/phase3-tree-view-edges-493` at `de5d45ae`, refreshed from
`94ddde56` onto current `origin/main` `0447498e` and covering TreeView disclosure
collapse, left-key consumed-state behavior, selected-row paint highlight,
and expanded/collapsed disclosure paint output. It remains unpushed and
undispatched.
Additional #493 view progress is queued locally as
`local/phase3-modal-overlay-edges-493` at `ba5f804e`, refreshed from
`95a7b597` onto current `origin/main` `0447498e` and covering ModalOverlay
key-release/no-callback Escape behavior, backdrop alpha paint output,
and backdrop-click dismissal hit/flag guards. It remains unpushed and
undispatched.
Additional #493 view progress is queued locally as
`local/phase3-auto-ui-edges-493` at `19f02f0c`, refreshed from
`39c0e3a1` onto current `origin/main` `0447498e` and covering AutoUi generated
toggle state, generated knob display-format branches, and sync
propagation to toggles and existing faders. It remains unpushed and
undispatched.
Additional #493 view progress is queued locally as
`local/phase3-image-cache-trim-493` at `a3ca7f7c`, refreshed from
`d9a0fc92` onto current `origin/main` `0447498e` and covering ImageCache
byte-budget lowering, least-recently-used trimming, and releaser
behavior. It remains unpushed and undispatched.
Additional #493 view progress is queued locally as
`local/phase3-visualization-bridge-edges-493` at `a39f609c`,
refreshed from `3e45d531` onto current `origin/main` `0447498e` and covering
VisualizationBridge disabled-waveform, zero-channel, and waveform
capture-length clamp paths. It remains unpushed and undispatched.
Additional #493 view progress is queued locally as
`local/phase3-waveform-editor-edges-493` at `1cf964ba`, refreshed
from `761d4c15` onto current `origin/main` `0447498e` and covering WaveformEditor
selection, visible-range, and playhead clamps, paint overlay output,
key scroll/release handling, and mouse selection extension paths. It
remains unpushed and undispatched.
Additional #493 view progress is queued locally as
`local/phase3-window-manager-edges-493` at `2ba38714`, refreshed from
`49f96229` onto current `origin/main` `0447498e` and covering WindowManager
unregister callback/missing-id cleanup, null host/root close behavior,
and missing-handler send/broadcast paths. It remains unpushed and
undispatched.
Additional #493 view progress is queued locally as
`local/phase3-param-attachment-edges-493` at `61a5fdc2`, refreshed
from `80bc3351` onto current `origin/main` `0447498e` and covering ParamAttachment
fader/toggle/combo callback forwarding, missing parameter-id no-op
behavior, and `poll_bindings()` external-change propagation. It remains
unpushed and undispatched.
Additional #493 view progress is queued locally as
`local/phase3-input-events-edges-493` at `d96d76a6`, refreshed from
`235f98b2` onto current `origin/main` `0447498e` and covering InputEvents
wheel/meta mouse helper paths, ended/cancelled gesture deltas, key
release/repeat main-modifier checks, and missing pointer-capture
release behavior. It remains unpushed and undispatched.
Additional #493 view progress is queued locally as
`local/phase3-widget-bridge-dom-493` at `d332d795`, refreshed from
`d2eec1b9` onto current `origin/main` `0447498e` and covering WidgetBridge native
DOM subtree moves, recursive DOM removal widget-map cleanup,
root/missing layout helper paths, and the root-aware `getLayoutRect("")`
registration fix. It remains unpushed and undispatched. Additional #643
tools progress is queued locally as
`local/phase3-harness-verifier-643` at `345c0039`, refreshed from
`b94b48c0` onto current `origin/main` `0447498e` and covering `tools/harness`
status/verifier helper paths, current yoga and CSS harness baselines,
adapter unit-test discovery from
`test/harness/test_*.py` including the current RN adapter coverage,
compat-sync's unknown-requirement hard-error expectation, and Python
coverage-runner discovery/omit rules for harness tests. It remains
unpushed and undispatched. Additional #493 format/view progress is
queued locally as `local/phase3-settings-panel-edges-493` at
`f076664e`, created from current `origin/main` `0447498e`, covering
SettingsPanel no-device fallback sample-rate/buffer-size lists, latency
label refresh, input-channel fallback apply behavior, and test-tone
disable/reselect callback paths. It remains unpushed and undispatched.
Additional #493 format progress is queued locally as
`local/phase3-headless-host-edges-493` at `950b73f4`, created from
current `origin/main` `0447498e`, covering HeadlessHost MIDI-overload
process-context defaults, release forwarding, and null-processor
prepare/process/release guard behavior. It remains unpushed and
undispatched.
Additional #643 tools progress is queued locally as
`local/phase3-check-format-validation-coverage-643` at `4a0217fb`,
refreshed from `76a2541b` onto current `origin/main` `0447498e`,
covering `tools/check_format_validation.py` parser, mode, reporting, and
read-error branches. It remains unpushed and undispatched.
Additional #643 CLI ship progress is refreshed locally as
`feature/phase3-cli-ship-coverage-643-next` at `844f501b`, rebased from
the paused #1274 remote head `c924f1e8` onto current `origin/main`
`50ff5822` after superseding local refresh `64c4424c` on `0447498e`,
covering `pulp ship sign` missing-identity guidance in a valid
project/build-cache shellout path. The refreshed branch remains
unpushed and undispatched; #1274 still points at the old remote head
until Namespace capacity returns.
Additional #643 CLI audio progress is refreshed locally as
`feature/phase3-cli-audio-command-coverage-643` at `c88ec0f3`, rebased
from the paused #1287 remote head `cb0a4acb` onto current `origin/main`
`50ff5822` after superseding local refresh `353a6afd` on `0447498e`,
covering deterministic `pulp audio` usage/parser errors and
missing-bundle JSON behavior through real CLI shellout tests. The
refreshed branch remains unpushed and undispatched; #1287 still points at
the old remote head until Namespace capacity returns.
Additional #641 runtime progress is refreshed locally as
`feature/phase3-named-pipe-coverage-641` at `e1dcf7af`, rebased from the
paused #1286 remote head `2d59c4c` onto current `origin/main` `0447498e`,
then refreshed again onto current `origin/main` `50ff5822`, covering
NamedPipe closed/missing endpoint fail-closed behavior, POSIX FIFO
round-trip, cleanup, move-ownership, and create-failure paths. The
refreshed branch remains unpushed and undispatched; #1286 still points at
the old remote head until Namespace capacity returns.
Additional #643 CLI helper progress is refreshed locally as
`feature/phase3-create-targets-coverage-643` at `ad7aebbe`, rebased from
the paused #1271 remote head `62ca4512` onto current `origin/main`
`50ff5822`, covering create-target optional format suffixes, duplicate
suppression, and empty standalone app target filtering. The refreshed
branch remains unpushed and undispatched; #1271 still points at the old
remote head until Namespace capacity returns.
Additional #493 CLI package progress is refreshed locally as
`feature/phase3-package-commands-coverage-493` at `5fa859ab`, rebased
from the paused #1273 remote head `c52cd486` onto current `origin/main`
`50ff5822`, covering package target compatibility warnings when an
installed package does not support a newly added target. The refreshed
branch remains unpushed and undispatched; #1273 still points at the old
remote head until Namespace capacity returns.
Additional #493 design-import progress is refreshed locally as
`feature/phase3-design-import-bundle-coverage-493` at `ddd6e9af`,
rebased from the paused #1269 remote head `923b04f1` onto current
`origin/main` `50ff5822`, covering Claude bundle malformed template/
manifest handling, malformed asset skips, referenced-JS indexing, and
bundled classname extraction through font-face-leading styles. The
refreshed branch remains unpushed and undispatched; #1269 still points
at the old remote head until Namespace capacity returns.
Additional #640 audio progress is refreshed locally as
`feature/phase3-mmap-reader-extra-640` at `815ab8b3`, rebased from the
paused #1280 remote head `ed2ba7bf` onto current `origin/main`
`50ff5822`, covering `MemoryMappedAudioReader` unsupported-file
fail-closed and EOF no-copy behavior. The refreshed branch remains
unpushed and undispatched; #1280 still points at the old remote head
until Namespace capacity returns.
Additional #493 view progress is refreshed locally as
`feature/view-text-editor-coverage-493-next` at `40606e1d`, rebased from
the paused #1282 remote head `03e5e3cd` onto current `origin/main`
`50ff5822` after superseding local refresh `f377ef5c` on `0447498e`,
covering TextEditor key-up/unhandled-key, modifier/word and shift
navigation, delete/redo, shift-click, and exact double-click word
selection paths. The refreshed branch remains unpushed and
undispatched; #1282 still points at the old remote head until Namespace
capacity returns.

## Phase 3 Operating Loop

Phase 3 should not become a wait-for-CI loop. Keep this cycle running
until the finish criteria below are met:

1. Monitor open `codecov` PRs.
2. If a PR is `CLEAN` and checks are complete, squash merge it manually,
   delete the branch, and update `#641` plus the component tracker.
3. If a check fails, inspect the failed job logs, patch the branch, push,
   and comment with the root cause and fix.
4. If all open PRs are only pending or queued, immediately continue the
   next focused coverage tranche in a separate worktree.
5. Keep tranche scope small: one subsystem slice, focused local
   validation, Codecov patch/diff proof, tracker link, then PR.

Namespace is the default validation lane for this program. Use
`shipyard pr` as the PR orchestrator, and prefer Namespace-backed CI
targets (`shipyard cloud run build <branch>` when invoking the cloud
lane directly). Local VMs are fallback only when Namespace is
unavailable; GitHub-hosted-only validation is last resort.

Subagents are useful for this loop when their write scopes are
disjoint. One agent can monitor merge/failure state while other agents
prepare non-overlapping coverage tranches in separate worktrees.

## Phase 3 Finish Line

Phase 3 is done when the planned represented-surface tranches have been
implemented or explicitly deferred, and the remaining gaps are
documented rather than silently ignored.

Practical finish criteria for the current plan:

- `audio`, `platform`, `host`, `format`, `midi`, and `signal` move
  materially toward the `80%` tier, with any hardware-bound platform
  shims either covered through deterministic seams or documented as
  deferred.
- `view`, `render`, and `cli` move toward the `70%` tier, prioritizing
  large represented files before small tail cleanup.
- `tools` crosses the `50%` infrastructure floor, then further tooling
  slices are handled only when they are high leverage or support the
  coverage system itself.
- `canvas`, `events`, `runtime`, `state`, `osc`, and `ship` stay above
  their `50%` infrastructure floor while ordinary feature work continues
  to satisfy the required diff-coverage gate.
- Deferred surfaces remain explicit in the tracker set: authored
  JavaScript (`#659`), Node bindings plus shell/PowerShell
  classification (`#657`), optional native surfaces (`#737`), and
  mobile runtime / simulator coverage (`#77`).

## Next Tranche Order

Use the latest complete Codecov file ranking and the tracker issues to
choose the next small PR. As of the `c05384aa` snapshot, the highest
remaining misses are concentrated in:

- `#640`: `audio` / `platform`, especially platform device shims and
  deterministic platform helpers.
- `#493`: `host` / `format` / `view`, especially real slot adapters,
  `widget_bridge.cpp`, and macOS view-host surfaces.
- `#643`: `cli` / `tools`, especially large command modules and
  low-coverage Python helpers.
- `#645`: `midi` / `signal`, with `signal` close to the 80% tier and
  `midi` still led by platform MIDI shims and MPE tracker paths.
- `#646`: `render`, now close to the 70% tier and best handled through
  GPU-off-safe helper targets when possible.

Keep the tranche size small: one subsystem slice, local focused
validation, Codecov patch/diff proof, tracker update, then merge when
GitHub branch protection is green and `mergeStateStatus` is `CLEAN`.

## Control-Plane Invariants

- Every top-level `core/*` directory must have matching subsystem
  entries in `codecov.yml` `flags:` and `component_management:`.
- Platform axes must match live repo path conventions, including
  `core/**/win/**` for Windows.
- Swift upload artifacts must be repo-relative LCOV with `SF:` paths
  rooted under `apple/Sources/**`.
- Android Kotlin coverage must run on the always-on Coverage workflow,
  not only on path-gated Android workflow commits.
- Upload-only flags stay `os-linux`, `os-macos`, and `os-windows`.
- `codecov.yml` `ignore:` must stay aligned with
  `scripts/run_coverage.sh` `COVERAGE_IGNORE_REGEX`.
- The authoritative program tracker is `#641`, not the older language
  umbrella `#568`.

## Validation Commands

For doc-only refreshes:

```bash
tools/check-docs.sh
PULP_ENFORCE_PREPUSH=1 python3 tools/scripts/skill_sync_check.py --base origin/main --mode=report
python3 tools/scripts/version_bump_check.py --base origin/main --config tools/scripts/versioning.json --mode=report
git diff --check
```
