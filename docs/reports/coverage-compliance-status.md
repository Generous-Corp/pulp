# Coverage Compliance Status

Last reviewed: 2026-05-05 15:03 PDT

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
tranche `local/phase3-linkwitz-riley-edges-645` at `3deafcfe`,
refreshed from `385c51a7` to `7b1a82a3`, then onto current
`origin/main` `bd036171`, covering
Linkwitz-Riley reset/history and cutoff boundary finite-output behavior,
plus #493 format tranche `local/phase3-validation-harness-json-493` at
`4e753dd3`, refreshed from `974cf572` onto current `origin/main` `0447498e` and
covering ValidationHarness report metadata and entry JSON escaping,
plus #493 host tranche `local/phase3-signal-graph-guards-493`
at `82f2264c`, refreshed from `247be833` onto current `origin/main` `0447498e`,
then refreshed again from `9f33226f` onto current `origin/main` `4bebc7bf`,
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
`local/phase3-adsr-edges-645` at `0c9e7d4d`, refreshed from
`e4844caa` onto current `origin/main` `0447498e`, then refreshed again
from `bd746f3c` onto current `origin/main` `bd036171`, covering ADSR
immediate stage, idle `note_off`, and reset edge paths, plus
`local/phase3-noise-gate-edges-645` at `032f46c2`, refreshed from
`98f73bb8` onto current `origin/main` `0447498e`, then refreshed again
from `717067d6` onto current `origin/main` `bd036171`, covering
NoiseGate range clamp, instant timing, reset, silence, and buffer paths, plus
`local/phase3-modulation-reverb-edges-645` at `e6b5c6bc`, refreshed from
`971488d5` onto current `origin/main` `0447498e`, then refreshed again
from `9cfcc171` onto current `origin/main` `4bebc7bf`, covering Chorus
dry/reset/phase-wrap behavior and Reverb zero-decay, damping clamp,
dry-mix, and reset paths, plus
`local/phase3-oscillator-edges-645` at `5e3a1db3`, refreshed from
`8f51cba4` onto current `origin/main` `0447498e`, then refreshed again
from `8f2e043` onto current `origin/main` `4bebc7bf`, covering Oscillator
reset, phase wrap, getter, and PolyBLEP edge paths, plus
`local/phase3-signal-helper-edges-645` at `431a3c5d`, created from
current `origin/main` `0447498e`, then refreshed onto current
`origin/main` `4bebc7bf`, covering FirFilter empty-coefficient
passthrough/reset, LookupTable indexed clamps and zero-length buffer
no-ops, and LadderFilter buffer/reset/resonance-clamp finite-output
paths. All five remain unpushed and undispatched.
Additional #646 render progress is queued locally as
`feature/phase3-sdl3-surface-fallback-646` at `3fe05a72`, refreshed
from `908b3a49` to `1dc45105`, then onto current `origin/main`
`bd036171`, covering SDL3 native-surface validity and null-window
fallback behavior. It has no remote branch ref and remains unpushed and
undispatched.
Additional #640 audio progress is queued locally as
`local/phase3-audio-focus-dispatch-640` at `69b4d18c`, refreshed from
`1ab6e24b` to `d766f4b9`, then onto current `origin/main` `bd036171`,
then onto current `origin/main` `b7ec8f08`, then onto current
`origin/main` `6c8b9920`, then to `e61d430c` on current
`origin/main` `b567dbeb`, covering AudioFocusRegistry inactive-listener
skip behavior when a listener is removed or the registry is reset during
dispatch. It remains unpushed and undispatched. Additional #640 audio
progress is queued locally as
`local/phase3-audio-data-shape-640` at `b2eb2cfb`, created from current
`origin/main` `0447498e`, then refreshed onto current `origin/main`
`8fa55f5e`, then onto current `origin/main` `b7ec8f08`, then onto
current `origin/main` `6c8b9920`, then onto current `origin/main`
`b567dbeb`, covering
AudioFileData helper shape semantics and WAV writer first-channel-empty
rejection. It remains unpushed and undispatched.
Additional #640 audio progress is queued locally as
`local/phase3-aiff-pcm-edges-640` at `6965d8d8`, created from current
`origin/main` `0447498e`, then refreshed onto current `origin/main`
`8fa55f5e`, then onto current `origin/main` `b7ec8f08`, then onto
current `origin/main` `6c8b9920`, then onto current `origin/main`
`b567dbeb`, covering AIFF invalid COMM metadata and unsupported PCM
bit-depth rejection. It remains
unpushed and undispatched. Additional
#640 audio progress is queued locally as
`local/phase3-streaming-writer-reopen-640` at `9875e436`, created from
current `origin/main` `0447498e`, then refreshed from `6f8dad35` onto
current `origin/main` `bd036171`, then onto current `origin/main`
`b7ec8f08`, then onto current `origin/main` `6c8b9920`, then onto
current `origin/main` `b567dbeb`, covering StreamingWriter
close-on-reopen and failed-open state/header
finalization behavior. It remains unpushed and undispatched. Additional
#640 audio progress is
queued locally as
`local/phase3-frame-fill-edges-640` at `3ccadef4`, created from current
`origin/main` `0447498e`, then refreshed from `1611b10d` onto current
`origin/main` `bd036171`, then onto current `origin/main` `b7ec8f08`,
then onto current `origin/main` `6c8b9920`, then onto current
`origin/main` `dba0fd4b`, then onto current `origin/main` `b567dbeb`,
covering
`zero_fill_short_read()` tail-fill, read-count clamp, null-buffer, and
non-positive-dimension guard paths. It remains unpushed and
undispatched. Additional #643 tools progress is
queued locally as
`local/phase3-audio-tools-model-store-643` at `a2f7bc2d`, refreshed from
`a88ddfe8` to `9743ccd7`, then onto current `origin/main` `bd036171`,
covering `tools/audio` model registry URL resolution, legacy/malformed
model metadata, model/bundle JSON serialization/defaults, and
excerpt-find guard and unsupported-input paths. It remains unpushed and
undispatched.
Additional #643 CLI progress is queued locally as
`local/phase3-cli-config-command-643` at `08308f66`, refreshed from
`af1ef6f8` to `76b36dcf`, then onto current `origin/main` `bd036171`,
covering `pulp config`
set/get/list, snooze clearing, malformed update keys, and invalid
update-key diagnostics against an isolated `PULP_HOME`. It remains
unpushed and undispatched.
Additional #643 static CI guard progress is queued locally as
`local/phase3-ruleset-drift-config-643` at `145cd195`, refreshed from
`390b68f9` to `012b224a`, then onto current `origin/main` `bd036171`,
covering
branch-protection JSON/workflow invariants for ruleset drift checks. The
official Python coverage runner intentionally excludes
`tools/scripts/test_*.py`, so this guard has no measured non-test source
surface in that focused runner. It remains unpushed and undispatched.
Additional #640 platform progress is queued locally as
`local/phase3-platform-environment-dispatch-640` at `325604de`,
refreshed from `b271d2e6` to `422a64cc`, then onto current `origin/main`
`bd036171`, then onto current `origin/main` `b7ec8f08`, then onto current
`origin/main` `6c8b9920`, then to `96aba9de` on current
`origin/main` `b567dbeb`, covering Environment token self-move, listener
removal before dispatch, and reset-during-dispatch skip behavior. It
remains unpushed and undispatched. Additional #640 platform progress is
queued locally as
`local/phase3-child-process-read-output-640` at `ca647d70`, refreshed
from `55c46d02` to `4008bd73`, then onto current `origin/main`
`bd036171`, then onto current `origin/main` `b7ec8f08`, then onto
current `origin/main` `6c8b9920`, then to `73543f6b` on current
`origin/main` `b567dbeb`, covering
`ChildProcess::read_available_output()` while stdout is available before
process completion. It remains unpushed and undispatched.
Additional #645 MIDI progress is queued locally as
`local/phase3-midi-ci-edges-645` at `6226f5af`, refreshed from
`c3f7e68b` to `f66f0d4`, then onto current `origin/main`
`bd036171`, covering MIDI-CI
malformed header rejection, directly addressed discovery inquiries,
short discovery replies, and reserved-byte profile matching. It remains
unpushed and undispatched. Additional #645 MIDI/MPE progress is queued
locally as `local/phase3-mpe-allocator-edges-645` at `249ad105`,
refreshed from `83ce06b3` to `dcacdaa0`, then onto current
`origin/main` `bd036171`, fixing the MpeVoiceAllocator release-steal
glide refcount path, documenting the releasing-steal invariant in the
MPE skill, and covering unmatched MpeGlideDetector note-off/reset
behavior. It remains unpushed and
undispatched. Additional #493 host scanner progress is
queued locally as `local/phase3-host-scanner-order-493` at `97d9f833`,
refreshed from `c3b79d68` to `97d9f833`, then to `1dfeb616`, then to
`e84ade81` on current `origin/main` `bd036171`, covering
PluginScanner VST3/LV2 format-lane merging, final name ordering, LV2
URI identity, VST3 stem fallback identity, and hermetic scanner fixture
paths. It remains unpushed and undispatched. Additional #493 host progress
is queued locally as `local/phase3-background-scanner-restart-493` at
`3da08b56`, created from current `origin/main` `0447498e`, then
refreshed onto current `origin/main` `bd036171`, covering
BackgroundScanner restart after a completed-but-unjoined worker. It
remains unpushed and undispatched. Additional #493 host progress is
queued locally as `local/phase3-plugin-slot-dispatch-493` at
`4945bad3`, created from current `origin/main` `0447498e`, then
refreshed onto current `origin/main` `bd036171`, covering PluginSlot
invalid descriptor fail-closed dispatch across CLAP, AU, AUv3, VST3,
and LV2 loader paths. It remains unpushed and
undispatched.
Additional #643 CLI/tools progress is queued locally as
`local/phase3-fetchcontent-cache-edges-643` at `5cbf2d87`, refreshed
from `b056ca59` to `e5b13514`, then onto current `origin/main`
`bd036171`, covering
FetchContent cache fallback entry parsing, live symlink classification,
file-backed declared-ref parsing, label fallbacks, and symlink removal
without deleting targets. It remains unpushed and undispatched.
Additional #493 view progress is queued locally as
`local/phase3-ui-components-edges-493` at `4bc91d9e`, refreshed from
`58940ee2` onto current `origin/main` `0447498e`, then refreshed again
onto current `origin/main` `50ff5822`, then refreshed again onto current
`origin/main` `c18785c9`, then refreshed again onto current `origin/main`
`83271a94`, covering ComboBox popup handoff/typeahead
no-op, ScrollView scrolled-child pointer-event hit testing and paint
clipping/visibility, and ListBox boundary-key and out-of-range mouse
guards. It remains unpushed and undispatched.
Additional #493 view/widget progress is queued locally as
`local/phase3-phase9-widget-edges-493` at `b23e4d25`, refreshed from
`17e25c73` onto current `origin/main` `0447498e`, then refreshed again
onto current `origin/main` `50ff5822`, then refreshed again onto current
`origin/main` `c18785c9`, then refreshed again onto current `origin/main`
`83271a94`, covering SplitView drag minimum clamps,
miss/drag guards, divider grip paint branches, and PropertyList boolean
editing, category height/paint, and scalar value formatting paths. It
remains unpushed and undispatched.
Additional #493 view/gui progress is queued locally as
`local/phase3-gui-components-edges-493` at `a37626d7`, refreshed from
`7e94a781` onto current `origin/main` `0447498e`, then refreshed again
onto current `origin/main` `50ff5822`, then refreshed again onto current
`origin/main` `c18785c9`, then refreshed again onto current `origin/main`
`83271a94`, covering TableListBox header sorting,
selection and out-of-range row guards, scaled/aligned painting, and
ConcertinaPanel invalid-index, content visibility/layout, paint, and
mouse hit paths. It remains unpushed and undispatched.
Additional #493 view progress is queued locally as
`local/phase3-live-constant-editor-493` at `84af7d62`, refreshed from
`29af1db8` onto current `origin/main` `0447498e`, then refreshed again
onto current `origin/main` `c18785c9`, then refreshed again onto current
`origin/main` `83271a94`, covering LiveConstantRegistry
duplicate registration, clamp, callback, missing key, reset, and
reset-all paths, plus LiveConstantEditor visibility, paint, slider drag,
header guard, and missing-row drag paths. It remains unpushed and
undispatched.
Additional #493 view/code-editor progress is queued locally as
`local/phase3-code-editor-doc-mru-493` at `fef0ef6c`, refreshed from
`e8e3545b` onto current `origin/main` `0447498e`, then refreshed again
onto current `origin/main` `c18785c9`, then refreshed again onto current
`origin/main` `83271a94`, covering FileBasedDocument
successful load/save-as dirty-state behavior and RecentlyOpenedFilesList
remove/missing-path behavior. It remains unpushed and undispatched.
Additional #493 view/graph-editor progress is queued locally as
`local/phase3-graph-editor-paint-493` at `8ed9f335`, refreshed from
`2d21c893` onto current `origin/main` `0447498e`, then refreshed again
onto current `origin/main` `c18785c9`, then refreshed again onto current
`origin/main` `83271a94`, covering GraphEditorView
auto-layout/manual-position preservation, unnamed node/multi-port
painting, and feedback/MIDI edge paint colors. It remains unpushed and
undispatched.
Additional #493 view/widget progress is queued locally as
`local/phase3-new-widgets-input-493` at `ba19ff03`, refreshed from
`7b5927ad` onto current `origin/main` `0447498e`, then refreshed again
onto current `origin/main` `8fa55f5e`, then refreshed again onto current
`origin/main` `83271a94`, then refreshed again onto current
`origin/main` `9a67a517`, covering MidiKeyboard vertical drag
release/miss behavior and note-name/highlight painting, plus
FileDropZone rejected-drop reset behavior and idle/valid/invalid/no-icon
paint paths. It remains unpushed and undispatched.
Additional #493 view/file-browser progress is queued locally as
`local/phase3-file-browser-paint-493` at `f7ff2469`, refreshed from
`57a12010` onto current `origin/main` `0447498e`, then refreshed again
onto current `origin/main` `c18785c9`, then refreshed again onto current
`origin/main` `83271a94`, then refreshed again onto current
`origin/main` `9a67a517`, covering FileBrowser sorted-row paint clipping
and MultiDocumentPanel active/inactive tab paint output. It remains
unpushed and undispatched.
Additional #493 view progress is reconciled locally on the #1285 resume
path as `feature/phase3-splash-screen-coverage-493` at `437d9ac5`,
refreshed from paused remote head `d3cd9f47` onto current `origin/main`
`50ff5822`, then `b7ec8f08`, then `6c8b9920`, then `24047ba3`, and
incorporating/superseding
`local/phase3-splash-screen-493` (`1dd00e70`), after superseding local
refresh `7cba95e3` on `0447498e`. It covers SplashScreen
advance/dismiss callback behavior, dismiss-on-click gating, and
text/image paint output. It remains unpushed and undispatched; #1285
still points at `d3cd9f47` until Namespace capacity returns.
Additional #493 view progress is queued locally as
`local/phase3-appearance-manager-493` at `0bbaaa9a`, refreshed from
`ceb05add` onto current `origin/main` `0447498e`, then refreshed again
onto current `origin/main` `c18785c9`, then onto current `origin/main`
`24047ba3`, then onto current `origin/main` `83271a94`, then onto
current `origin/main` `9a67a517`, covering AppearanceTracker repeated
lock callbacks and locked poll no-op behavior, plus ThemeManager
locked-theme, locked-appearance callback, and unlock behavior. It remains
unpushed and undispatched.
Additional #493 view progress is queued locally as
`local/phase3-tree-view-edges-493` at `ca5267a7`, refreshed from
`94ddde56` onto current `origin/main` `0447498e`, then refreshed again
onto current `origin/main` `c18785c9`, then onto current `origin/main`
`24047ba3`, then onto current `origin/main` `9a67a517`, covering
TreeView disclosure collapse, left-key consumed-state behavior,
selected-row paint highlight, and expanded/collapsed disclosure paint
output. It remains unpushed and undispatched.
Additional #493 view progress is queued locally as
`local/phase3-modal-overlay-edges-493` at `67fe482e`, refreshed from
`95a7b597` onto current `origin/main` `0447498e`, then refreshed again
onto current `origin/main` `c18785c9`, then onto current `origin/main`
`83271a94`, then refreshed again onto current `origin/main` `7e9795b4`,
covering ModalOverlay key-release/no-callback Escape behavior, backdrop
alpha paint output, and backdrop-click dismissal hit/flag guards. It
remains unpushed and undispatched.
Additional #493 view progress is queued locally as
`local/phase3-auto-ui-edges-493` at `832c0781`, refreshed from
`39c0e3a1` onto current `origin/main` `0447498e`, then refreshed again
onto current `origin/main` `c18785c9`, then onto current `origin/main`
`83271a94`, then onto current `origin/main` `7e9795b4`, covering AutoUi generated toggle
state, generated knob display-format branches, and sync propagation to
toggles and existing faders. It remains unpushed and undispatched.
Additional #493 view progress is queued locally as
`local/phase3-image-cache-trim-493` at `165c2357`, refreshed from
`d9a0fc92` onto current `origin/main` `0447498e`, then refreshed again
onto current `origin/main` `c18785c9`, then onto current `origin/main`
`83271a94`, then onto current `origin/main` `7e9795b4`, covering ImageCache byte-budget
lowering, least-recently-used trimming, and releaser behavior. It
remains unpushed and undispatched.
Additional #493 view progress is queued locally as
`local/phase3-visualization-bridge-edges-493` at `a30900b8`,
refreshed from `3e45d531` onto current `origin/main` `0447498e`, then
refreshed again onto current `origin/main` `c18785c9`, then onto current
`origin/main` `83271a94`, then onto current `origin/main` `7e9795b4`, covering
VisualizationBridge disabled-waveform, zero-channel, and waveform
capture-length clamp paths. It remains unpushed and undispatched.
Additional #493 view progress is queued locally as
`local/phase3-waveform-editor-edges-493` at `e3b02b07`, refreshed
from `761d4c15` onto current `origin/main` `0447498e`, then refreshed
again onto current `origin/main` `c18785c9`, then onto current
`origin/main` `83271a94`, covering WaveformEditor selection,
visible-range, and playhead clamps, paint overlay output, key scroll/release
handling, and mouse selection extension paths. It remains unpushed and
undispatched.
Additional #493 view progress is queued locally as
`local/phase3-window-manager-edges-493` at `baa1ae56`, refreshed from
`49f96229` onto current `origin/main` `0447498e`, then refreshed again
onto current `origin/main` `c18785c9`, then onto current `origin/main`
`83271a94`, covering WindowManager unregister callback/missing-id cleanup,
null host/root close behavior, and missing-handler send/broadcast paths.
It remains unpushed and undispatched.
Additional #493 view progress is queued locally as
`local/phase3-param-attachment-edges-493` at `6da58f0f`, refreshed
from `80bc3351` onto current `origin/main` `0447498e`, then refreshed
again onto current `origin/main` `c18785c9`, then onto current
`origin/main` `83271a94`, covering ParamAttachment fader/toggle/combo
callback forwarding, missing parameter-id no-op behavior, and
`poll_bindings()` external-change propagation. It remains unpushed and
undispatched.
Additional #493 view progress is queued locally as
`local/phase3-input-events-edges-493` at `ee320ea1`, refreshed from
`235f98b2` onto current `origin/main` `0447498e`, then refreshed again
onto current `origin/main` `c18785c9`, then onto current `origin/main`
`83271a94`, covering InputEvents wheel/meta mouse helper paths,
ended/cancelled gesture deltas, key release/repeat main-modifier checks,
and missing pointer-capture release behavior. It remains unpushed and
undispatched.
Additional #493 view progress is queued locally as
`local/phase3-widget-bridge-dom-493` at `8987b7d2`, refreshed from
`d2eec1b9` onto current `origin/main` `0447498e`, then onto current
`origin/main` `83271a94`, covering WidgetBridge native DOM subtree moves,
recursive DOM removal widget-map cleanup, root/missing layout helper paths,
and the root-aware `getLayoutRect("")` registration fix. It remains
unpushed and undispatched. Additional #643
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
`local/phase3-check-format-validation-coverage-643` at `96b9dfd8`,
refreshed from `76a2541b` onto current `origin/main` `0447498e`, then
onto current `origin/main` `24047ba3`,
covering `tools/check_format_validation.py` parser, mode, reporting, and
read-error branches. It remains unpushed and undispatched.
Additional #643 CLI ship progress is refreshed locally as
`feature/phase3-cli-ship-coverage-643-next` at `d113ca6c`, rebased from
the paused #1274 remote head `c924f1e8` onto current `origin/main`
`50ff5822` after superseding local refresh `64c4424c` on `0447498e`,
then onto current `origin/main` `b7ec8f08`, then onto current
`origin/main` `cf5ea658`, then onto current `origin/main` `24047ba3`,
covering `pulp ship sign` missing-identity guidance in a valid
project/build-cache shellout path. The refreshed branch remains
unpushed and undispatched; #1274 still points at the old remote head
until Namespace capacity returns.
Additional #643 CLI audio progress is refreshed locally as
`feature/phase3-cli-audio-command-coverage-643` at `fc736f3a`, rebased
from the paused #1287 remote head `cb0a4acb` onto current `origin/main`
`50ff5822` after superseding local refresh `353a6afd` on `0447498e`,
then onto current `origin/main` `b7ec8f08`, then onto current
`origin/main` `cf5ea658`, then onto current `origin/main` `24047ba3`,
covering deterministic
`pulp audio` usage/parser errors and
missing-bundle JSON behavior through real CLI shellout tests. The
refreshed branch remains unpushed and undispatched; #1287 still points at
the old remote head until Namespace capacity returns.
Additional #641 runtime progress is refreshed locally as
`feature/phase3-named-pipe-coverage-641` at `db9f9d0d`, rebased from the
paused #1286 remote head `2d59c4c` onto current `origin/main` `0447498e`,
then refreshed again onto current `origin/main` `50ff5822`, then
`b7ec8f08`, then `6c8b9920`, covering NamedPipe closed/missing endpoint fail-closed
behavior, POSIX FIFO round-trip, cleanup, move-ownership, and
create-failure paths. The
refreshed branch remains unpushed and undispatched; #1286 still points at
the old remote head until Namespace capacity returns.
Additional #643 CLI helper progress is refreshed locally as
`feature/phase3-create-targets-coverage-643` at `05f0acda`, rebased from
the paused #1271 remote head `62ca4512` onto current `origin/main`
`50ff5822`, then onto current `origin/main` `b7ec8f08`, then
`cf5ea658`, then onto current `origin/main` `24047ba3`, covering
create-target optional format suffixes, duplicate
suppression, and empty standalone app target filtering. The refreshed
branch remains unpushed and undispatched; #1271 still points at the old
remote head until
Namespace capacity returns.
Additional #493 CLI package progress is refreshed locally as
`feature/phase3-package-commands-coverage-493` at `106b1cf9`, rebased
from the paused #1273 remote head `c52cd486` onto current `origin/main`
`50ff5822`, then onto current `origin/main` `b7ec8f08`, then
`cf5ea658`, then onto current `origin/main` `24047ba3`, covering package target compatibility warnings when an
installed package does not support a newly added target. The refreshed
branch remains unpushed and
undispatched; #1273 still points at the old remote head until Namespace
capacity returns.
Additional #493 design-import progress is refreshed locally as
`feature/phase3-design-import-bundle-coverage-493` at `45b77a0b`,
rebased from the paused #1269 remote head `923b04f1` onto current
`origin/main` `50ff5822`, then onto current `origin/main` `cf5ea658`,
then onto current `origin/main` `24047ba3`, covering Claude bundle malformed template/manifest handling, malformed
asset skips, referenced-JS indexing, and bundled classname extraction
through font-face-leading styles. The
refreshed branch remains unpushed and undispatched; #1269 still points
at the old remote head until Namespace capacity returns.
Additional #640 audio progress is refreshed locally as
`feature/phase3-mmap-reader-extra-640` at `092d2db7`, rebased from the
paused #1280 remote head `ed2ba7bf` onto current `origin/main`
`50ff5822`, then onto current `origin/main` `b7ec8f08`, then onto current
`origin/main` `6c8b9920`, then onto current `origin/main` `24047ba3`, covering
`MemoryMappedAudioReader` unsupported-file fail-closed and EOF no-copy
behavior. The refreshed branch remains unpushed and undispatched; #1280
still points at the old remote head until Namespace capacity returns.
Additional #493 view progress is refreshed locally as
`feature/view-text-editor-coverage-493-next` at `0656ed85`, rebased from
the paused #1282 remote head `03e5e3cd` onto current `origin/main`
`50ff5822`, then onto `b7ec8f08`, then `cf5ea658`, then onto current
`origin/main` `24047ba3`, after superseding local refresh `f377ef5c` on
`0447498e`, covering TextEditor
key-up/unhandled-key, modifier/word and shift navigation, delete/redo,
shift-click, and exact double-click word selection paths. The refreshed
branch remains unpushed and
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
