# Phase 3 Codecov Queue Pause Ledger

Last updated: 2026-05-05 02:36 PDT

This local ledger records the open `codecov` PR validation runs paused to free Namespace capacity for higher-priority work, plus the small-batch resume queue. Branches, PRs, commits, labels, and tracker comments stay intact; queued GitHub Actions validation attempts are cancellable and replaceable.

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

## Snapshot Summary

- Open `codecov` PRs in snapshot: 73
- Queued/in-progress Codecov-branch workflow runs selected for cancellation: 193
- Selected workflows: Build and Test, Coverage, Sanitizer Tests

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
2026-05-05 02:11 PDT: rebased/amended
`local/phase3-child-process-read-output-640` from `02c06848` to
`55c46d02`, covering `ChildProcess::read_available_output()` while a
spawned process is still running, and rebased/amended
`local/phase3-audio-focus-dispatch-640` from `3c36343d` to
`1ab6e24b`, then rebased it again to `d766f4b9`, covering
AudioFocusRegistry inactive-listener skip behavior when a listener is
removed or the registry is reset during dispatch,
and rebased `local/phase3-platform-environment-dispatch-640` from
`49d0611a` to `b271d2e6`, then rebased it again to `422a64cc`,
covering Environment token self-move, listener removal before dispatch,
and reset-during-dispatch skip behavior. All three remain unpushed and
undispatched.

Additional local-only progress at 2026-05-04 16:37 PDT: refreshed
`feature/phase3-sdl3-surface-fallback-646` to `908b3a49`, covering
SDL3 native-surface fallback/null-window behavior. It has no remote
branch ref and remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 16:50 PDT: added
`local/phase3-cli-config-command-643` at `af1ef6f8`, covering
`pulp config` set/get/list and invalid-key shellout behavior against an
isolated `PULP_HOME`. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 17:03 PDT: added
`local/phase3-linkwitz-riley-edges-645` at `385c51a7`, covering
Linkwitz-Riley reset/history and cutoff boundary finite-output behavior
for the #645 signal tranche. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 23:34 PDT: refreshed
`local/phase3-validation-harness-json-493` from `15fac4aa` to
`974cf572`, covering ValidationHarness report metadata and entry JSON
escaping for the #493 format tranche on current `origin/main`. It
remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 23:37 PDT: refreshed
`local/phase3-signal-graph-guards-493` from `adcf5dcf` to
`247be833`, covering SignalGraph missing-node guards, remove-node
connection cleanup, and default accessors for the #493 host tranche on
current `origin/main`. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 23:39 PDT: refreshed
`local/phase3-widgets-render-paths-493` from `fefb7e94` to `55a90f79`,
covering widget custom-shader CPU fallbacks, minimal render-style
branches, and knob/fader/toggle interaction edges for the #493 view
tranche on current `origin/main`. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 23:41 PDT: refreshed
`local/phase3-audio-bridge-edges-493` from `255b0516` to `cba288df`,
covering AudioBridge first-pop/default meter, max-channel clamp,
zero-sample analysis, and MeterBallistics tiny-value clamp paths for the
#493 view tranche on current `origin/main`. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 18:04 PDT: added
`local/phase3-adsr-edges-645` at `e4844caa`, covering ADSR immediate
stage progression, idle `note_off`, and reset paths for the #645
signal tranche. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 18:09 PDT: added
`local/phase3-noise-gate-edges-645` at `98f73bb8`, covering NoiseGate
range clamp, instant attack/release timing, reset, silence, and buffer
paths for the #645 signal tranche. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 18:17 PDT: added
`local/phase3-modulation-reverb-edges-645` at `971488d5`, covering
Chorus dry/reset/phase-wrap behavior and Reverb zero-decay, damping
clamp, dry-mix, and reset paths for the #645 signal tranche. It
remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 18:21 PDT: added
`local/phase3-oscillator-edges-645` at `8f51cba4`, covering
Oscillator reset, phase wrap, getter, and PolyBLEP edge paths for the
#645 signal tranche. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 18:31 PDT, refreshed at
2026-05-05 02:07 PDT: rebased/amended
`local/phase3-audio-focus-dispatch-640` from `3c36343d` to
`1ab6e24b`, then rebased it again to `d766f4b9`, covering
AudioFocusRegistry inactive-listener skip behavior when a listener is
removed or the registry is reset during dispatch for the #640 audio
tranche. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 18:42 PDT, refreshed at
2026-05-05 02:09 PDT: added `local/phase3-audio-tools-model-store-643`
at `a88ddfe8`, then rebased it to `9743ccd7`, covering `tools/audio`
model registry URL resolution, legacy/malformed model metadata,
model/bundle JSON serialization/defaults, and excerpt-find guard and
unsupported-input paths for the #643 tools tranche. It remains unpushed
and undispatched.

Additional local-only progress at 2026-05-04 18:50 PDT, refreshed at
2026-05-05 02:11 PDT: rebased
`local/phase3-platform-environment-dispatch-640` from `49d0611a` to
`b271d2e6`, then rebased it again to `422a64cc`, covering Environment
token self-move, listener removal before dispatch, and
reset-during-dispatch skip behavior for the #640 platform tranche. It
remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 18:59 PDT, refreshed at
2026-05-04 23:23 PDT, refreshed at 2026-05-05 02:14 PDT: rebased
`local/phase3-midi-ci-edges-645` from `9b7e547f` to `c3f7e68b`, then
rebased it again to `f66f0d4`, covering MIDI-CI malformed
universal/sub-ID guard paths, directly addressed discovery inquiries,
short discovery replies, and reserved-byte profile matching for the
#645 MIDI tranche. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 19:07 PDT, refreshed at
2026-05-05 02:16 PDT: rebased/amended
`local/phase3-mpe-allocator-edges-645` from `fc198dec` to `83ce06b3`,
then rebased it again to `dcacdaa0`, fixing the MpeVoiceAllocator
release-steal glide refcount path, documenting the releasing-steal
invariant in the MPE skill, and covering unmatched MpeGlideDetector
note-off/reset behavior for the #645 MIDI/MPE tranche. It remains
unpushed and undispatched.

Additional local-only progress at 2026-05-04 19:20 PDT, refreshed at
2026-05-05 02:19 PDT: rebased `local/phase3-host-scanner-order-493`
from `c3b79d68` to `97d9f833`, then rebased it again to `1dfeb616`,
covering PluginScanner VST3/LV2 format-lane merging, final name
ordering, LV2 URI identity, VST3 stem fallback identity, and hermetic
scanner fixture paths for the #493 host tranche. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 19:32 PDT, refreshed at
2026-05-05 02:23 PDT: rebased
`local/phase3-fetchcontent-cache-edges-643` from `dd7433d8` to
`b056ca59`, then rebased it again to `e5b13514`, covering FetchContent
cache fallback entry splitting/order, live symlink classification,
file-backed declared-ref parsing, label fallbacks, and symlink removal
without deleting targets for the #643 CLI/tools tranche. It remains
unpushed and undispatched.

Additional local-only progress at 2026-05-04 23:44 PDT, refreshed at
2026-05-05 02:26 PDT: rebased `local/phase3-ui-components-edges-493`
from `33d5f737` to `16cc8df1`, then to `58940ee2`, then to `a4ae8412`,
covering ComboBox popup handoff/typeahead no-op, ScrollView
scrolled-child pointer-event hit testing and paint clipping/visibility,
and ListBox boundary-key and out-of-range mouse guards for the #493
view tranche on current `origin/main`. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 23:47 PDT, refreshed at
2026-05-05 02:31 PDT: rebased
`local/phase3-phase9-widget-edges-493` from `d5000a24` to `d9ffd85b`,
then to `17e25c73`, then to `3b2fa68f`, covering SplitView drag minimum
clamps, miss/drag guards, divider grip paint branches, and PropertyList
boolean editing, category height/paint, and scalar value formatting paths
for the #493 view/widget tranche on current `origin/main`. It remains
unpushed and undispatched.

Additional local-only progress at 2026-05-04 23:49 PDT, refreshed at
2026-05-05 02:34 PDT: rebased `local/phase3-gui-components-edges-493`
from `30d414ea` to `b26a0e5f`, then to `7e94a781`, then to `b0e683e9`, covering
TableListBox header sorting, selection and out-of-range row guards,
scaled/aligned painting, and ConcertinaPanel invalid-index, content
visibility/layout, paint, and mouse hit paths for the #493 view/gui
tranche on current `origin/main`. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 23:52 PDT, refreshed at
2026-05-05 01:08 PDT, 2026-05-05 01:10 PDT, and 2026-05-05
02:36 PDT: rebased
`local/phase3-live-constant-editor-493` from `aeae2883` to
`0e4a677e`, then to `238736d1`, then to `29af1db8`, then to
`4ee6e846`, covering
LiveConstantRegistry duplicate registration, clamp, callback, missing
key, reset, and reset-all paths, plus LiveConstantEditor visibility,
paint, slider drag, header guard, and missing-row drag paths for the
#493 view tranche on current `origin/main`. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 23:54 PDT, refreshed at
2026-05-05 01:14 PDT: rebased `local/phase3-code-editor-doc-mru-493`
from `8595aab3` to `c97574e6`, then to `e8e3545b`,
covering FileBasedDocument successful load/save-as dirty-state behavior
and RecentlyOpenedFilesList remove/missing-path behavior for the #493
view code-editor tranche on current `origin/main`. It remains unpushed
and undispatched.

Additional local-only progress at 2026-05-04 20:36 PDT, refreshed at
2026-05-04 23:59 PDT and 2026-05-05 01:17 PDT: rebased
`local/phase3-graph-editor-paint-493` from `0258cfa4` to `31d8d7cc`,
then to `2d21c893`, covering GraphEditorView
auto-layout/manual-position preservation, unnamed node/multi-port
painting, and feedback/MIDI edge paint colors for the #493 view
graph-editor tranche on current `origin/main`. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 20:43 PDT, refreshed at
2026-05-05 00:02 PDT and 2026-05-05 01:20 PDT: rebased
`local/phase3-new-widgets-input-493` from `741caf74` to `251cfa5d`,
then to `7b5927ad`, covering MidiKeyboard vertical drag
release/miss behavior and note-name/highlight painting, plus
FileDropZone rejected-drop reset behavior and idle/valid/invalid/no-icon
paint paths for the #493 view/widget tranche on current `origin/main`.
It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 20:51 PDT, refreshed at
2026-05-05 00:04 PDT and 2026-05-05 01:22 PDT: rebased
`local/phase3-file-browser-paint-493` from `74dd1dd0` to `74bc4483`,
then to `57a12010`, covering FileBrowser sorted-row paint
clipping and MultiDocumentPanel active/inactive tab paint output for the
#493 view/file-browser tranche on current `origin/main`. It remains
unpushed and undispatched.

Additional local-only progress at 2026-05-04 20:57 PDT, refreshed at
2026-05-05 00:06 PDT and 2026-05-05 01:26 PDT: rebased
`local/phase3-splash-screen-493` from `38f2c93d` to `0e7ebab3`,
then to `0ac19fbd`, covering SplashScreen advance/dismiss
callback behavior, dismiss-on-click gating, and text/image paint output
for the #493 view tranche on current `origin/main`. It remains unpushed
and undispatched.

Additional local-only progress at 2026-05-04 21:03 PDT, refreshed at
2026-05-05 00:09 PDT and 2026-05-05 01:29 PDT: rebased
`local/phase3-appearance-manager-493` from `b96abb6b` to `31d4e8ff`,
then to `ceb05add`, covering AppearanceTracker repeated lock
callbacks and locked poll no-op behavior, plus ThemeManager locked-theme,
locked-appearance callback, and unlock behavior for the #493 view
appearance-manager tranche on current `origin/main`. It remains unpushed
and undispatched.

Additional local-only progress at 2026-05-04 21:09 PDT, refreshed at
2026-05-05 00:12 PDT and 2026-05-05 01:32 PDT: rebased
`local/phase3-tree-view-edges-493` from `76b99bee` to `dc87d5fe`,
then to `94ddde56`, covering TreeView disclosure collapse,
left-key consumed-state behavior, selected-row paint highlight, and
expanded/collapsed disclosure paint output for the #493 view TreeView
tranche on current `origin/main`. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 21:17 PDT, refreshed at
2026-05-05 00:16 PDT and 2026-05-05 01:34 PDT: rebased
`local/phase3-modal-overlay-edges-493` from `e40432f2` to `0f30dc7a`,
then to `95a7b597`, covering ModalOverlay
key-release/no-callback Escape behavior, backdrop alpha paint output,
and backdrop-click dismissal hit/flag guards for the #493 view
ModalOverlay tranche on current `origin/main`. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 21:28 PDT, refreshed at
2026-05-05 00:19 PDT and 2026-05-05 01:38 PDT: rebased
`local/phase3-auto-ui-edges-493` from `485f6cd5` to `545493ad`,
then to `39c0e3a1`, covering AutoUi generated toggle state,
generated knob display-format branches, and sync propagation to toggles
and existing faders for the #493 view AutoUi tranche on current
`origin/main`. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 21:34 PDT, refreshed at
2026-05-05 00:21 PDT and 2026-05-05 01:42 PDT: rebased
`local/phase3-image-cache-trim-493` from `1afb279e` to `312a008f`, then
to `d9a0fc92`, covering ImageCache byte-budget lowering,
least-recently-used trimming, and releaser behavior for the #493 view
ImageCache tranche on current `origin/main`. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 21:42 PDT, refreshed at
2026-05-05 00:25 PDT and 2026-05-05 01:44 PDT: rebased
`local/phase3-visualization-bridge-edges-493` from `10405bc2` to
`dc41a698`, then to `3e45d531`, covering VisualizationBridge disabled-waveform,
zero-channel, and waveform capture-length clamp paths for the #493 view
tranche on current `origin/main`. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 21:49 PDT, refreshed at
2026-05-05 00:28 PDT and 2026-05-05 01:47 PDT: rebased
`local/phase3-waveform-editor-edges-493` from `a199b6dc` to
`9e28c887`, then to `761d4c15`, covering WaveformEditor selection,
visible-range, and playhead clamps, paint overlay output, key
scroll/release handling, and mouse selection extension paths for the
#493 view tranche on current `origin/main`. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 21:56 PDT, refreshed at
2026-05-05 00:31 PDT and 2026-05-05 01:49 PDT: rebased
`local/phase3-window-manager-edges-493` from `5d62f37a` to `7fd5052a`,
then to `49f96229`, covering WindowManager unregister
callback/missing-id cleanup, null host/root close behavior, and
missing-handler send/broadcast paths for the #493 view tranche on
current `origin/main`. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 22:02 PDT, refreshed at
2026-05-05 00:34 PDT and 2026-05-05 01:51 PDT: rebased
`local/phase3-param-attachment-edges-493` from `0d6600ec` to
`cf7957c3`, then to `80bc3351`, covering ParamAttachment
fader/toggle/combo callback forwarding, missing parameter-id no-op
behavior, and `poll_bindings()` external-change propagation for the
#493 view tranche on current `origin/main`. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 22:08 PDT, refreshed at
2026-05-05 00:36 PDT and 2026-05-05 01:54 PDT: rebased
`local/phase3-input-events-edges-493` from `58a06b63` to `c7d18912`,
then to `235f98b2`, covering InputEvents wheel/meta mouse helper paths,
ended/cancelled gesture deltas, key release/repeat main-modifier
checks, and missing pointer-capture release behavior for the #493 view
tranche on current `origin/main`. It remains unpushed and undispatched.

Additional local-only progress at 2026-05-04 22:16 PDT, refreshed at
2026-05-05 00:41 PDT and 2026-05-05 01:56 PDT: rebased/amended
`local/phase3-widget-bridge-dom-493` from `40629315` to `df183299`,
then to `d2eec1b9`, covering WidgetBridge native DOM subtree moves,
recursive DOM removal widget-map cleanup, root/missing layout helper
paths, and the root-aware `getLayoutRect("")` registration fix for the
#493 view tranche on current `origin/main`. It remains unpushed and
undispatched.

Additional local-only progress at 2026-05-04 22:34 PDT, refreshed at
2026-05-04 22:46 PDT, 2026-05-04 23:04 PDT, 2026-05-05 00:50 PDT,
and 2026-05-05 02:02 PDT:
rebased/amended `local/phase3-harness-verifier-643` from `51c0c8bf`
to `bdb9cd42`, then to `7b7cc19d`, then to `e9be30f6`, then to
`b94b48c0`, covering `tools/harness` status/verifier helper paths,
current yoga and CSS harness baselines, adapter unit-test discovery from
`test/harness/test_*.py` including the current RN adapter coverage,
compat-sync's unknown-requirement hard-error expectation, and Python
coverage-runner discovery/omit rules so harness tests participate in
the measured Python tools lane without transient adapter fixtures being
reported as source. It remains unpushed and undispatched.

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
  2026-05-04 at `390b68f9`; keep it behind measured-source tranches because it is a
  static guard/test-file-only tranche.
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
| `feature/phase3-design-import-bundle-coverage-493` | `923b04f1` | #493 view/import tranche for `core/view/src/design_import.cpp` Claude bundle malformed JSON, skipped asset, referenced-JS indexing, and bundled classname extraction edges | `test/test_design_import_claude_bundle.cpp` | Worker-created from current `origin/main`; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` passed; rebased cleanly and `cmake --build build --target pulp-test-design-import-claude-bundle -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-design-import-claude-bundle` passed 45 assertions in 8 test cases; requested ctest pattern `design-import-claude-bundle|DesignImportClaudeBundle|Claude bundle` exited 0 with no tests found; registered-case pattern `ctest --test-dir build -R "parse_claude_bundle|extract_claude_classnames reads bundled" --output-on-failure` passed 8/8; CLI skew, skill-sync report, version-bump report, `git diff --check`, and `git diff --cached --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1269, then exited with no local targets remaining as expected for the Namespace-only route; #1269 is labeled `codecov` and linked from #493/#641. Windows Namespace failed `cli-doctor-versions-json` missing `"projects":`, which matches the shared CI/CTest robustness issue covered by #1278; macOS Namespace is still running on the old workflow. | After #1278 lands or the current workflow finishes, rerun/rebase #1269 and merge once required gates are green. |
| `feature/phase3-widgets-paint-coverage-493` | `f4fce52a` | #493 view tranche for `core/view/src/widgets.cpp` Knob/Fader sprite-strip paint, vertical RangeSlider paint, and SpectrumView invalid-dB paint edges | `test/test_widgets.cpp` | Worker-created from current `origin/main`; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` passed; `cmake --build build --target pulp-test-widgets -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-widgets` passed 259 assertions in 63 test cases; `ctest --test-dir build -R "Widgets|widgets" --output-on-failure` passed 1/1; CLI skew, skill-sync report, version-bump report, `git diff --check`, and `git diff --cached --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1270, then exited with no local targets remaining as expected for the Namespace-only route; #1270 is labeled `codecov` and linked from #493/#641. Required Namespace wrappers, Windows MSVC, and Codecov patch passed; Windows coverage failed on a WGPU DLL copy issue while advisory macOS coverage/sanitizer lanes were still queued. | Merged #1270 as `910029f6f7cebe7b20f01a003c249dc938838d80`; tracker comments posted to #493/#641; cancellation requested for leftover advisory workflows `25243597556` and `25243597558`. |
| `feature/phase3-create-targets-coverage-643` | `62ca4512` | #643 CLI helper tranche for `tools/cli/create_targets.cpp` optional format suffixes, duplicate suppression, and empty standalone app target filtering | `test/test_cli_create_targets.cpp` | Created from current `origin/main` at `451a2428`; configured `build` with `-DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF` and the shared MbedTLS cache; `cmake --build build --target pulp-test-cli-create-targets -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-cli-create-targets -r compact` passed 5 assertions in 5 test cases; `ctest --test-dir build -R "CLI create targets|cli-create-targets|create targets" --output-on-failure` passed 5/5; CLI skew, skill-sync report, version-bump report, `git diff --check`, and `git diff --cached --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1271, then exited with no local targets remaining as expected for the Namespace-only route; #1271 is labeled `codecov` and linked from #641/#643. | Queued: monitor #1271 and merge once required gates are green. |
| `feature/phase3-import-design-tool-coverage-493` | `bc463fcf` | #493 tooling/import tranche for `tools/import-design/pulp_import_design.cpp` help/error, dry-run token export, and Stitch import write paths | `test/CMakeLists.txt`, `test/test_import_design_tool.cpp` | Worker-created from current `origin/main` at `5984a309`; configured and built `pulp-test-import-design-tool`; `./build/test/pulp-test-import-design-tool` passed 29 assertions in 3 test cases; `ctest --test-dir build -R "import-design-tool|pulp import-design" --output-on-failure` passed 3/3; CLI skew, skill-sync report, version-bump report, `git diff --check`, and `git diff --cached --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1272, then exited with no local targets remaining as expected for the Namespace-only route; #1272 is labeled `codecov` and linked from #493/#641. Required Namespace wrappers, Windows MSVC, and Codecov patch passed. Android coverage failed on the earlier SDK/download transient (`dl.google.com` resolution / build-tools install), and advisory macOS coverage/sanitizer lanes were still queued. | Merged #1272 as `cb641ff19c16ce0115e3dae7c04e5c2fa5a62760`; tracker comments posted to #493/#641; cancellation requested for leftover advisory workflows `25243703306` and `25243703313`. |
| `feature/phase3-package-commands-coverage-493` | `c52cd486` | #493 package-command tranche for `tools/cli/package_commands.cpp` target compatibility warning behavior | `test/test_cli_package_commands.cpp` | Worker-created from current `origin/main` at `5984a309`; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` passed; `cmake --build build --target pulp-test-cli-package-commands -j$(sysctl -n hw.ncpu)` passed; focused Catch2 case passed 6 assertions in 1 test case; full `./build/test/pulp-test-cli-package-commands` passed 195 assertions in 12 test cases; `ctest --test-dir build -R "package commands|Package|pulp-test-cli-package-commands" --output-on-failure` passed 1/1; CLI skew, skill-sync report, version-bump report, and `git diff --check origin/main...HEAD` passed. Targeted `local_diff_cover.sh` was attempted but expanded into unrelated full CTest entries after building the target, so the worker interrupted the optional run after package-command cases had passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1273, then exited with no local targets remaining as expected for the Namespace-only route; #1273 is labeled `codecov` and linked from #493/#641. | Queued: monitor #1273 and merge once required gates are green. |
| `feature/phase3-cli-ship-coverage-643-next` | `c924f1e8` | #643 CLI ship tranche for deterministic `pulp ship sign` missing-identity guidance in a valid project/build cache | `test/test_cli_ship_shellout.cpp` | Worker-created from current `origin/main`; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` passed; `cmake --build build --target pulp-test-cli-ship-shellout -j$(sysctl -n hw.ncpu)` passed; focused missing-identity shellout case passed; full `./build/test/pulp-test-cli-ship-shellout` passed 11 assertions in 11 test cases; `ctest --test-dir build -R "pulp ship|cli-ship|ship-shellout" --output-on-failure` passed 11/11; CLI skew, skill-sync report, version-bump report, `git diff --check`, and Xcode-toolchain-backed `local_diff_cover.sh pulp-test-cli-ship-shellout` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1274, then exited with no local targets remaining as expected for the Namespace-only route; #1274 is labeled `codecov` and linked from #641/#643. | Queued: monitor #1274 and merge once required gates are green. |
| `feature/phase3-cli-audio-command-coverage-643` | `cb0a4acb` | #643 CLI audio command parser tranche for deterministic `pulp audio` usage, parser errors, and missing-bundle JSON behavior | `test/test_cli_shellout.cpp` | Worker-created from current `origin/main`; REST PR lookup confirmed #1287 is open at `cb0a4acbca31e4f7b51e4c2f3c92226b76ba7318` and labeled `codecov`. Local validation on 2026-05-04 passed `cmake --build build --target pulp-test-cli-shellout -j$(sysctl -n hw.ncpu)`, focused Catch2 cases `pulp audio usage and parser errors are deterministic` and `pulp audio read-bundle json reports missing bundle errors`, focused `ctest --test-dir build -R "pulp audio|cli-shellout|audio usage" --output-on-failure` with 2/2 tests passed, full `./build/test/pulp-test-cli-shellout -r compact` with 55 assertions in 55 test cases, and `git diff --check origin/main...HEAD && git diff --check`. | Queued: monitor #1287 and merge once required gates are green; do not rerun or update the branch while Namespace is intentionally paused. |
| `codecov-phase3-android-kotlin-runtime-coverage-641` | `56e1b7b0` | #641 Android/Kotlin tranche for `PulpMidiManager` device discovery, open/close lifecycle, MIDI send-port reuse, and transport fallback | `android/app/src/test/kotlin/com/pulp/midi/PulpMidiManagerTest.kt` | Worker-created from current `origin/main`; `JAVA_HOME=/opt/homebrew/opt/openjdk@17 ANDROID_HOME="$HOME/Library/Android/sdk" ./gradlew :app:testDebugUnitTest --tests com.pulp.midi.PulpMidiManagerTest` passed; `JAVA_HOME=/opt/homebrew/opt/openjdk@17 ANDROID_HOME="$HOME/Library/Android/sdk" ./gradlew :app:jacocoDebugUnitTestReport` passed; CLI skew, skill-sync report with Android skill bypass trailer, version-bump report, and `git diff --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1275, then exited with no local targets remaining as expected for the Namespace-only route; #1275 is labeled `codecov` and linked from #641. | Queued: monitor #1275 and merge once required gates are green. |
| `feature/phase3-popup-menu-coverage-640` | `c66d74e4` | #640 platform tranche for popup menu item/separator bookkeeping and non-Apple fallback no-selection behavior | `test/test_platform.cpp` | Created from current `origin/main` at `910029f6`; configured `build` with `-DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF` and the shared MbedTLS cache after a first configure hit a slow fresh MbedTLS clone; `cmake --build build --target pulp-test-platform -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-platform -r compact` passed 37 assertions in 4 test cases; exact `ctest --test-dir build -R "^(Platform detection|PopupMenu records items and separators|Windows registry helpers fail closed on non-Windows platforms)$" --output-on-failure` passed 3/3. A broader `registry` regex also matched unrelated NOT_BUILT registry placeholders in this partial build, so exact discovered names are the valid CTest evidence. CLI skew, skill-sync report, version-bump report, and `git diff --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1276, then exited with no local targets remaining as expected for the Namespace-only route; #1276 is labeled `codecov` and linked from #640/#641. | Queued: monitor #1276 and merge once required gates are green. |
| `phase3-midi-ump-coverage-645` | `a1dc2508` | #645 MIDI tranche for deterministic UMP channel voice helper factory encoding | `test/test_ump_buffer_conversion.cpp` | Worker-created from current `origin/main`; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug` passed; `cmake --build build --target pulp-test-ump-buffer-conversion -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-ump-buffer-conversion` passed 139 assertions in 17 test cases; `ctest --test-dir build -R "ump-buffer-conversion|UmpPacket remaining" --output-on-failure` passed 1/1; CLI skew, skill-sync report, version-bump report, and `git diff --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1277, then exited with no local targets remaining as expected for the Namespace-only route; #1277 is labeled `codecov` and linked from #645/#641. | Queued: monitor #1277 and merge once required gates are green. |
| `fix/phase3-ci-doctor-unblock-643` | `a2bdd728` | #643 CI robustness fix for shared `cli-doctor` CTest instability and Android coverage SDK install retries | `.github/workflows/coverage.yml`, `tools/cli/CMakeLists.txt` | Worker-created after queue triage classified #1269 and older #1255 failures as shared CI/CTest robustness issues and #1272 Android coverage as an SDK/download transient. Local validation passed: `cmake -S . -B build-phase3-ci-doctor -G Ninja -DPULP_BUILD_TESTS=ON -DPULP_ENABLE_GPU=ON`; `cmake --build build-phase3-ci-doctor --target pulp-cli -j4`; `ctest --test-dir build-phase3-ci-doctor --output-on-failure -R "^cli-doctor(-versions|-ci|-validators|-versions-json|-caches-empty|-caches-json)?$"`; `actionlint .github/workflows/coverage.yml`; `git diff --check`; CLI skew. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1278, then exited with no local targets remaining as expected for the Namespace-only route; #1278 is labeled `codecov`. | Queued: merge #1278 when required gates are green, then rerun/rebase PRs blocked only by the shared doctor/Android robustness failures. |
| `feature/android-kotlin-coverage-phase3-tranche` | `b845c453` | #641 Android/Kotlin tranche for `PulpAudioController` lifecycle edges | `android/app/src/test/kotlin/com/pulp/audio/PulpAudioControllerTest.kt` | Worker-created from current `origin/main` without overlapping #1275; local validation passed `./gradlew :app:testDebugUnitTest --tests com.pulp.audio.PulpAudioControllerTest`, `./gradlew :app:testDebugUnitTest :app:jacocoDebugUnitTestReport`, CLI skew, skill-sync report with Android skill bypass trailer, version-bump report, and `git diff --check origin/main...HEAD`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1279, then exited with no local targets remaining as expected for the Namespace-only route; #1279 is labeled `codecov` and linked from #641. Initial Android Build Windows job failed inside `actions/cache@v5` before Gradle/test code ran; `gh run rerun 25244691254 --failed` requested and the rerun passed both macOS and Windows Android-build jobs. | Queued: monitor #1279 and merge once required gates are green. |
| `feature/phase3-mmap-reader-extra-640` | `867fedc5` | #640 audio tranche for `MemoryMappedAudioReader` unsupported-file fail-closed and EOF no-copy edges | `test/test_audio_file.cpp` | Created from current `origin/main` at `414f9e9a`; configured `build` with `-DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF` and the shared MbedTLS cache; `cmake --build build --target pulp-test-audio-file -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-audio-file "MemoryMappedAudioReader*" -r compact` passed 41 assertions in 3 test cases; `ctest --test-dir build -R "MemoryMappedAudioReader" --output-on-failure` passed 3/3; skill-sync report, version-bump report, `git diff --check`, and `git diff --check origin/main...HEAD` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1280, then exited with no local targets remaining as expected for the Namespace-only route; #1280 is labeled `codecov` and linked from #640/#641. | Queued: monitor #1280 and merge once required gates are green. |
| `feature/phase3-version-diag-edge-coverage-643` | `bc77c8df` | #643 CLI tranche for `tools/cli/version_diag.cpp` manifest-reader and empty-project-root edges | `test/test_cli_version_diag.cpp` | Worker-created from current `origin/main` at `414f9e9a` after checking open PR scopes to avoid #1271/#1273/#1274/#1278; `cmake --build build --target pulp-test-cli-version-diag -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-cli-version-diag` passed 92 assertions in 35 test cases; `ctest --test-dir build -R "(manifest has no version field|empty project root)" --output-on-failure` passed; focused LLVM coverage for `tools/cli/version_diag.cpp` reported 87.50% line coverage; CLI skew check, `git diff --check HEAD~1..HEAD`, skill-sync, and version-bump passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1281, then exited with no local targets remaining as expected for the Namespace-only route; #1281 is labeled `codecov` and linked from #641/#643. Required gates were green; only advisory macOS sanitizer/coverage lanes were pending at merge time. | Merged #1281 as `01ecdaf109fc31a995efc82650dad8cf2a31d3e0`; tracker comments posted to #641/#643. |
| `feature/view-text-editor-coverage-493-next` | `03e5e3cd` | #493 view tranche for `core/view/src/text_editor.cpp` navigation edges | `test/test_text_editor.cpp` | Worker-created from current `origin/main` at `414f9e9a` after rebasing away inherited audio commits; final PR diff is limited to `test/test_text_editor.cpp`; `cmake --build build --target pulp-test-text-editor -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-text-editor` passed 148 assertions in 37 test cases; `ctest --test-dir build -R 'TextEditor' --output-on-failure` passed 37/37; focused LLVM coverage for `core/view/src/text_editor.cpp` reported 79.13% line, 59.74% branch, and 88.57% function coverage; CLI skew and `git diff --check` passed; `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1282, then exited with no local targets remaining as expected for the Namespace-only route; #1282 is labeled `codecov` and linked from #493/#641. | Queued: monitor #1282 and merge once required gates are green. |
| `feature/python-inspect-helper-coverage-641-next` | `96087469` | #641 Python bindings tranche for `ParamRange` clamp/zero-span and `MidiBuffer` clear/reuse edges | `test/test_python_bindings.py` | Rebased cleanly onto current `origin/main` at `414f9e9a`; diff is limited to `test/test_python_bindings.py`; first fresh configure was interrupted because it tried to clone MbedTLS in the worktree, then rerun with `-DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`; `cmake -S . -B build-python-helper -DPULP_BUILD_PYTHON=ON -DPULP_BUILD_TESTS=ON -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DCMAKE_BUILD_TYPE=Debug -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed; `cmake --build build-python-helper --target pulp_python pulp-test-python-bindings-embedded -j$(sysctl -n hw.ncpu)` passed; `ctest --test-dir build-python-helper -R "python-bindings-(smoke|embedded-smoke)" --output-on-failure` passed 2/2; skill-sync report, version-bump report, `git diff --check origin/main...HEAD`, and `git diff --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1283, then exited with no local targets remaining as expected for the Namespace-only route; #1283 is labeled `codecov` and linked from #641. After #1264 merged, #1283 became dirty/conflicting in `test/test_python_bindings.py`; rebased onto `origin/main` at `91660df1`, kept both #1264's binding assertions and this tranche's clamp/reuse assertions, reran the same focused Python binding build/CTest plus CLI skew, skill-sync, version-bump, and diff checks, and force-with-lease pushed `960874696639d0a631da44ba75d5a219fa95a61f`. | Fresh PR-event checks are running for `96087469`; merge once required gates are green. |
| `feature/phase3-splash-screen-coverage-493` | `d3cd9f47` | #493 view tranche for `core/view/src/splash_screen.cpp` lifecycle, click-dismiss, and paint fallback/image edges | `test/test_splash_screen.cpp`, `test/CMakeLists.txt` | Created from current `origin/main` at `91660df1`; configured `build` with `-DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF` and the shared MbedTLS cache; `cmake --build build --target pulp-test-splash-screen -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-splash-screen -r compact` passed 25 assertions in 3 test cases; `ctest --test-dir build -R "SplashScreen|splash-screen|splash" --output-on-failure` passed 3/3; CLI skew, skill-sync report, version-bump report, `git diff --check origin/main...HEAD`, and `git diff --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1285, then exited with no local targets remaining as expected for the Namespace-only route; #1285 is labeled `codecov` and linked from #493/#641. | Queued: monitor #1285 and merge once required gates are green. |
| `feature/phase3-named-pipe-coverage-641` | `2d59c4c` | #641 runtime tranche for `core/runtime/src/named_pipe.cpp` closed/missing endpoint fail-closed, POSIX FIFO round-trip, cleanup, move-ownership, and create-failure paths | `test/test_stream.cpp` | Worker-created from current `origin/main`; review confirmed the diff is limited to `test/test_stream.cpp` and no open PR had the same scope; rebased cleanly onto current `origin/main`; `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_TEXT_SHAPING=OFF -DPULP_BUILD_EXAMPLES=OFF` passed; `cmake --build build --target pulp-test-stream -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-stream -r compact` passed; `ctest --test-dir build -R "NamedPipe|Stream" --output-on-failure` passed; CLI skew, skill-sync report, version-bump report, `git diff --check origin/main...HEAD`, and `git diff --check` passed. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1286, then exited with no local targets remaining as expected for the Namespace-only route; #1286 is labeled `codecov` and linked from #641. | Queued: monitor #1286 and merge once required gates are green. |
| `feature/events-helper-coverage-642-next3` | `9e0924cd` | #642 events tranche for `core/events/src/timer.cpp` one-shot active-state behavior | `core/events/src/timer.cpp`, `test/CMakeLists.txt`, `test/test_events_timer_helpers.cpp`, `CMakeLists.txt` | Rebased cleanly onto `origin/main` at `fa180654` after resolving the events-test target block around the already-merged async-helper target; `git cherry -v origin/main HEAD` shows the source-fix commit is still needed; `cmake --build build --target pulp-test-events-timer-helpers -j$(sysctl -n hw.ncpu)`; `./build/test/pulp-test-events-timer-helpers -r compact` passed 7 assertions in 1 test case; `ctest --test-dir build -R "Timer one-shot|events-timer|timer-helpers" --output-on-failure` passed 1/1; CLI skew check; skill-sync report; version-bump report correctly suggests an SDK patch bump; `git diff --check origin/main...HEAD`; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1253, then exited with no local targets remaining as expected for the Namespace-only route; first PR-event version/skill check failed because the `fix:` PR title requires a bump commit. Added `chore: bump versions` commit `9e0924cd0bc344511ef03b22319932c5d40afc24` (`CMakeLists.txt` 0.69.0 -> 0.69.1); local `version_bump_check.py --mode=report --base origin/main --head HEAD --require-bump-for-fix-feat --pr-title 'fix(events): deactivate one-shot timers after firing'` passes. #1253 is labeled `codecov` and linked from #642/#641. | Merged #1253 as `f5fd9fe80359ecb6bf1d4aa86921de1b96e2ea0d`; tracker comments posted to #641/#642; cancellation requested for leftover advisory runs `25241535268` and `25241535428`. |
| `feature/events-helper-coverage-642-next5` | `abe35658` | #642 events tranche for ActionBroadcaster missing-listener removal edge behavior | `test/test_events.cpp` | Rebased cleanly onto `origin/main` at `fa180654`; `git cherry -v origin/main HEAD` shows `abe356586c4648e8129d694605a76f0f39fa8394` still needed; `cmake --build build --target pulp-test-events -j$(sysctl -n hw.ncpu)` passed; `./build/test/pulp-test-events "[action_broadcaster]"` passed 4 assertions in 1 test case; `ctest --test-dir build -R "ActionBroadcaster adds, removes, and notifies listeners" --output-on-failure` passed 1/1; CLI skew check; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1254, then exited with no local targets remaining as expected for the Namespace-only route; #1254 is labeled `codecov` and linked from #642/#641. | Merged #1254 as `451a24285ad9deb8fd21a9db22d22a38e6ef5c9f`; tracker comments posted to #641/#642; cancellation requested for leftover advisory runs `25241451299` and `25241451295`. |
| `local/phase3-ruleset-drift-config-643` | `390b68f9` | #643 static ruleset-drift guard tranche for branch-protection JSON/workflow invariants | `tools/scripts/test_ruleset_drift_config.py` | Rebased cleanly onto local `origin/main` at `1fe671df` on 2026-05-04; `python3 tools/scripts/test_ruleset_drift_config.py` passed with 6 tests; `python3 -m py_compile tools/scripts/test_ruleset_drift_config.py` passed; supporting guard tests `test_codecov_config.py`, `test_local_diff_cover.py`, `test_coverage_diff_comment.py`, and `test_coverage_tier_check.py` passed; skill-sync, version-bump, docs-sync, and compat-sync reports passed; `git diff --check origin/main...HEAD && git diff --check` passed; final status clean and ahead 1. The official `run_python_coverage.py` surface intentionally excludes `tools/scripts/test_*.py`, so this static guard has no non-test source to report in that focused runner. | Hold behind measured-source tranches; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`; use PR-event Namespace checks unless a targeted diagnostic dispatch is needed. |
| `local/phase3-child-process-read-output-640` | `55c46d02` | #640 platform tranche for `ChildProcess::read_available_output()` while stdout is available before process completion | `test/test_child_process.cpp` | Rebased/amended from `02c06848` onto current `origin/main` at `4fd67d84` on 2026-05-04; committed `55c46d021fe12d81f4a341970fcc5d3bc0721769`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-child-process -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-child-process "read_available_output drains stdout while process is running" -r compact` passed 5 assertions in 1 test case; full `./build/test/pulp-test-child-process -r compact` passed 65 assertions in 21 test cases; `ctest --test-dir build -R "child-process|read_available_output|ChildProcess" --output-on-failure` passed 1/1; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-platform-environment-dispatch-640` | `422a64cc` | #640 platform tranche for Environment token self-move, listener removal before dispatch, and reset-during-dispatch skip behavior | `test/test_environment.cpp` | Rebased from `b271d2e6` onto current `origin/main` at `0447498e` on 2026-05-05; committed `422a64cc60a6ff94f7d8c761e92da395240ed027`. The refreshed diff remains test-only in `test/test_environment.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-environment -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-environment "[environment][issue-640]" -r compact` passed 61 assertions in 10 test cases; full `./build/test/pulp-test-environment -r compact` passed 106 assertions in 21 test cases; `ctest --test-dir build -R "Environment|environment|pulp-test-environment" --output-on-failure` passed 21/21; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-midi-ci-edges-645` | `f66f0d4` | #645 MIDI tranche for MIDI-CI malformed header rejection, directly addressed discovery inquiries, short discovery replies, and reserved-byte profile matching | `test/test_midi_ci.cpp` | Rebased from `c3f7e68b` onto current `origin/main` at `0447498e` on 2026-05-05; committed `f66f0d46594ca6d84f91ee0bd58d909aedab8e03`. The refreshed diff remains test-only in `test/test_midi_ci.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-midi-ci -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-midi-ci "[midi][ci][issue-645]" -r compact` passed 54 assertions in 10 test cases; full `./build/test/pulp-test-midi-ci -r compact` passed 81 assertions in 19 test cases; `ctest --test-dir build -R "CiDiscovery|MUID|midi-ci|pulp-test-midi-ci" --output-on-failure` passed 19/19; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-mpe-allocator-edges-645` | `dcacdaa0` | #645 MIDI/MPE tranche for preserving MpeVoiceAllocator glide refcounts when stealing releasing voices, documenting the MPE skill invariant, plus unmatched MpeGlideDetector note-off/reset coverage | `.agents/skills/mpe/SKILL.md`, `core/midi/include/pulp/midi/mpe_synth_voice.hpp`, `test/test_mpe_synth_voice.cpp` | Rebased from `83ce06b3` onto current `origin/main` at `0447498e` on 2026-05-05; committed `dcacdaa04504279af6975e400aec3b28b49696a1`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-mpe-synth-voice -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-mpe-synth-voice "[midi][mpe][issue-645]" -r compact` passed 62 assertions in 6 test cases; full `./build/test/pulp-test-mpe-synth-voice -r compact` passed 86 assertions in 14 test cases; `ctest --test-dir build -R "MpeVoiceAllocator|MpeGlideDetector|mpe-synth-voice" --output-on-failure` passed 12/12; skill-sync passed with the MPE skill update; version-bump report passed with SDK patch override from the commit trailer `Version-Bump: sdk=patch reason="MPE glide refcount behavior fix without public API shape change"` plus a plugin patch suggestion for the skill update; docs-sync and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-host-scanner-order-493` | `1dfeb616` | #493 host scanner tranche for PluginScanner VST3/LV2 format-lane merging, final name ordering, LV2 URI identity, VST3 stem fallback identity, and hermetic synthetic scanner fixtures | `test/test_host_regression.cpp` | Rebased from `97d9f833` onto current `origin/main` at `0447498e` on 2026-05-05; committed `1dfeb616246b05b1c3e520f535925d4957a6aa79`. The refreshed diff remains test-only in `test/test_host_regression.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-host-regression -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-host-regression "PluginScanner merges enabled format lanes and sorts by plugin name" -r compact` passed 10 assertions in 1 test case; tag run `./build/test/pulp-test-host-regression "[host][scanner]" -r compact` passed 61 assertions in 11 test cases without walking installed plugin directories; full `./build/test/pulp-test-host-regression -r compact` passed 430 assertions in 19 test cases with expected synthetic scanner warnings; exact `ctest --test-dir build -R "PluginScanner merges enabled format lanes" --output-on-failure` passed 1/1; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-fetchcontent-cache-edges-643` | `e5b13514` | #643 CLI/tools tranche for FetchContent cache fallback entry splitting/order, live symlink classification, file-backed declared-ref parsing, label fallbacks, and symlink removal without deleting targets | `test/test_cli_fetchcontent_cache.cpp` | Rebased from `b056ca59` onto current `origin/main` at `0447498e` on 2026-05-05; committed `e5b135147dfeb6d6fc8098abf77ad14f8b8811da`. The refreshed diff remains test-only in `test/test_cli_fetchcontent_cache.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-cli-fetchcontent-cache -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-cli-fetchcontent-cache "[fetchcontent_cache][issue-643]" -r compact` passed 53 assertions in 7 test cases; full `./build/test/pulp-test-cli-fetchcontent-cache -r compact` passed 137 assertions in 25 test cases; exact `ctest --test-dir build -R "status and fix labels|undeclared entries|live symlink|symlinks without deleting|parse_declared_refs_from_file|lstat miss|control characters" --output-on-failure` passed 7/7; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-ui-components-edges-493` | `a4ae8412` | #493 view tranche for ComboBox popup handoff/typeahead no-op, ScrollView scrolled-child pointer-event hit testing and paint clipping/visibility, and ListBox boundary-key/out-of-range mouse guards | `test/test_ui_components.cpp` | Rebased from `58940ee2` onto current `origin/main` at `0447498e` on 2026-05-05; committed `a4ae84122f2431db1a8132397c4262a27958c791`. The refreshed diff remains test-only in `test/test_ui_components.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-ui-components -j$(sysctl -n hw.ncpu)` passed with existing view warnings; focused `./build/test/pulp-test-ui-components "ComboBox opening another popup closes the previous one" -r compact` passed 10 assertions in 1 test case; focused `./build/test/pulp-test-ui-components "ScrollView hit testing honors pointer modes with scrolled children" -r compact` passed 5 assertions in 1 test case; focused `./build/test/pulp-test-ui-components "ScrollView paint_all clips translated content and skips invisible views" -r compact` passed 5 assertions in 1 test case; focused `./build/test/pulp-test-ui-components "ListBox ignores boundary keys and out-of-range mouse presses" -r compact` passed 6 assertions in 1 test case; full `./build/test/pulp-test-ui-components -r compact` passed 147 assertions in 37 test cases; exact `ctest --test-dir build -R "ComboBox opening another popup|ScrollView hit testing honors pointer modes|ScrollView paint_all clips translated content|ListBox ignores boundary keys" --output-on-failure` passed 4/4; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-phase9-widget-edges-493` | `3b2fa68f` | #493 view/widget tranche for SplitView drag minimum clamps, miss/drag guards, divider grip paint branches, and PropertyList boolean editing, category height/paint, and scalar value formatting paths | `test/test_phase9_widgets.cpp` | Rebased cleanly from `17e25c73` onto current `origin/main` at `0447498e` on 2026-05-05; committed `3b2fa68f7c18505a306f0181249ec119d2b6e151`. The refreshed diff remains test-only in `test/test_phase9_widgets.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-phase9-widgets -j$(sysctl -n hw.ncpu)` passed with existing view warnings; focused `./build/test/pulp-test-phase9-widgets "SplitView drag handling clamps to pane minimums and ignores misses" -r compact` passed 12 assertions in 1 test case; focused `./build/test/pulp-test-phase9-widgets "SplitView paint emits horizontal and vertical divider grips" -r compact` passed 4 assertions in 1 test case; focused `./build/test/pulp-test-phase9-widgets "PropertyList mouse editing toggles writable booleans only" -r compact` passed 10 assertions in 1 test case; focused `./build/test/pulp-test-phase9-widgets "PropertyList paints categories and scalar value variants" -r compact` passed 10 assertions in 1 test case; full `./build/test/pulp-test-phase9-widgets -r compact` passed 161 assertions in 34 test cases; exact `ctest --test-dir build -R "SplitView drag handling|SplitView paint emits|PropertyList mouse editing|PropertyList paints categories" --output-on-failure` passed 4/4; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-gui-components-edges-493` | `b0e683e9` | #493 view/gui tranche for TableListBox header sorting, row selection/out-of-range guards, scaled/aligned paint paths, and ConcertinaPanel invalid-index, content visibility/layout, paint, and mouse-hit paths | `test/test_gui_components.cpp` | Rebased cleanly from `7e94a781` onto current `origin/main` at `0447498e` on 2026-05-05; committed `b0e683e960a99e84d5879c1da5538afcebcd0eca`. The refreshed diff remains test-only in `test/test_gui_components.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-gui-components -j$(sysctl -n hw.ncpu)` passed with existing view warnings; focused `./build/test/pulp-test-gui-components "TableListBox header sorting and row selection edge paths" -r compact` passed 18 assertions in 1 test case; focused `./build/test/pulp-test-gui-components "TableListBox paint covers empty guards scaled columns and alignment" -r compact` passed 8 assertions in 1 test case; focused `./build/test/pulp-test-gui-components "ConcertinaPanel guards indices and syncs content visibility" -r compact` passed 9 assertions in 1 test case; focused `./build/test/pulp-test-gui-components "ConcertinaPanel paint and mouse hit testing cover content offsets" -r compact` passed 12 assertions in 1 test case; full `./build/test/pulp-test-gui-components -r compact` passed 174 assertions in 33 test cases; exact `ctest --test-dir build -R "TableListBox header sorting|TableListBox paint covers|ConcertinaPanel guards indices|ConcertinaPanel paint and mouse" --output-on-failure` passed 4/4; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-live-constant-editor-493` | `4ee6e846` | #493 view tranche for LiveConstantRegistry duplicate registration, clamp, callback, missing-key, reset, and reset-all paths, plus LiveConstantEditor visibility, paint, slider drag, header guard, and missing-row drag paths | `test/CMakeLists.txt`, `test/test_live_constant_editor.cpp` | Rebased cleanly from `29af1db8` onto current `origin/main` at `0447498e` on 2026-05-05; committed `4ee6e84620871e22f81707f2246c4cc8fec20959`. The refreshed diff remains limited to test target wiring and `test/test_live_constant_editor.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-live-constant-editor -j$(sysctl -n hw.ncpu)` passed; full `./build/test/pulp-test-live-constant-editor -r compact` passed 26 assertions in 2 test cases; exact `ctest --test-dir build -R "LiveConstantRegistry|LiveConstantEditor|live-constant" --output-on-failure` passed 2/2; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-code-editor-doc-mru-493` | `e8e3545b` | #493 view code-editor tranche for FileBasedDocument successful load/save-as dirty-state behavior and RecentlyOpenedFilesList remove/missing-path behavior | `test/test_code_editor.cpp` | Rebased cleanly from `c97574e6` onto current `origin/main` at `c4dc8625` on 2026-05-05; committed `e8e3545b7a6103650c7512fd71056a8226183e57`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-code-editor -j$(sysctl -n hw.ncpu)` passed with existing view warnings; focused `./build/test/pulp-test-code-editor "FileBasedDocument handles successful load and save_as paths" -r compact` passed 15 assertions in 1 test case; focused `./build/test/pulp-test-code-editor "RecentlyOpenedFilesList removes entries and ignores missing paths" -r compact` passed 7 assertions in 1 test case; full `./build/test/pulp-test-code-editor -r compact` passed 74 assertions in 13 test cases; exact `ctest --test-dir build -R "FileBasedDocument handles successful|RecentlyOpenedFilesList removes entries" --output-on-failure` passed 2/2; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-graph-editor-paint-493` | `2d21c893` | #493 view graph-editor tranche for GraphEditorView auto-layout/manual-position preservation, unnamed node/multi-port painting, and feedback/MIDI edge paint colors | `test/test_graph_editor_view.cpp` | Rebased cleanly from `31d8d7cc` onto current `origin/main` at `c4dc8625` on 2026-05-05; committed `2d21c8934ce1a17432a5ffd35ac7b04a884240a1`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-graph-editor-view -j$(sysctl -n hw.ncpu)` passed with existing view warning noise; focused `./build/test/pulp-test-graph-editor-view "GraphEditorView auto layout preserves positions and paints unnamed ports" -r compact` passed 7 assertions in 1 test case; focused `./build/test/pulp-test-graph-editor-view "GraphEditorView paints feedback and midi edge colors" -r compact` passed 4 assertions in 1 test case; full `./build/test/pulp-test-graph-editor-view -r compact` passed 33 assertions in 6 test cases; exact `ctest --test-dir build -R "GraphEditorView auto layout preserves|GraphEditorView paints feedback" --output-on-failure` passed 2/2; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-new-widgets-input-493` | `7b5927ad` | #493 view/widget tranche for MidiKeyboard vertical drag release/miss behavior, note-name/highlight painting, and FileDropZone rejected-drop reset plus idle/valid/invalid/no-icon paint paths | `test/test_phase9_widgets.cpp` | Rebased cleanly from `251cfa5d` onto current `origin/main` at `c4dc8625` on 2026-05-05; committed `7b5927adf917d4f85cf36dc4b785c1564c855125`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-phase9-widgets -j$(sysctl -n hw.ncpu)` passed with existing dependency and view warning noise; focused `./build/test/pulp-test-phase9-widgets "MidiKeyboard vertical drag releases previous notes and misses" -r compact` passed 15 assertions in 1 test case; focused `./build/test/pulp-test-phase9-widgets "MidiKeyboard paint emits note names and active highlight color" -r compact` passed 4 assertions in 1 test case; focused `./build/test/pulp-test-phase9-widgets "FileDropZone rejected drop resets state without callback" -r compact` passed 5 assertions in 1 test case; focused `./build/test/pulp-test-phase9-widgets "FileDropZone paint covers idle valid invalid and no-icon states" -r compact` passed 6 assertions in 1 test case; full `./build/test/pulp-test-phase9-widgets -r compact` passed 155 assertions in 34 test cases; exact `ctest --test-dir build -R "MidiKeyboard vertical drag|MidiKeyboard paint emits|FileDropZone rejected drop|FileDropZone paint covers" --output-on-failure` passed 4/4; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-file-browser-paint-493` | `57a12010` | #493 view/file-browser tranche for FileBrowser sorted-row paint clipping and MultiDocumentPanel active/inactive tab paint output | `test/test_file_browser.cpp` | Rebased cleanly from `74bc4483` onto current `origin/main` at `c4dc8625` on 2026-05-05; committed `57a12010db4e56ce41649704e6f107bc45b1dc9c`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-file-browser -j$(sysctl -n hw.ncpu)` passed with existing dependency and view warning noise; focused `./build/test/pulp-test-file-browser "FileBrowser paint clips rows after sorted visible entries" -r compact` passed 9 assertions in 1 test case; focused `./build/test/pulp-test-file-browser "MultiDocumentPanel paint emits active and inactive tab labels" -r compact` passed 12 assertions in 1 test case; full `./build/test/pulp-test-file-browser -r compact` passed 81 assertions in 6 test cases; exact `ctest --test-dir build -R "FileBrowser paint clips rows|MultiDocumentPanel paint emits" --output-on-failure` passed 2/2; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-splash-screen-493` | `0ac19fbd` | #493 view tranche for SplashScreen advance/dismiss callback behavior, dismiss-on-click gating, and text/image paint output | `test/CMakeLists.txt`, `test/test_splash_screen.cpp` | Rebased cleanly from `0e7ebab3` onto current `origin/main` at `c4dc8625` on 2026-05-05; committed `0ac19fbd26f08488d57dae8d621922a9653e4719`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-splash-screen -j$(sysctl -n hw.ncpu)` passed with existing dependency and view warning noise; focused `./build/test/pulp-test-splash-screen "SplashScreen advance walks fade hold and dismissal callback" -r compact` passed 14 assertions in 1 test case; focused `./build/test/pulp-test-splash-screen "SplashScreen mouse dismissal respects dismiss-on-click setting" -r compact` passed 8 assertions in 1 test case; focused `./build/test/pulp-test-splash-screen "SplashScreen paint emits loading text or configured image" -r compact` passed 12 assertions in 1 test case; full `./build/test/pulp-test-splash-screen -r compact` passed 34 assertions in 3 test cases; exact `ctest --test-dir build -R "SplashScreen" --output-on-failure` passed 3/3; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; do not push as an independent branch until #1285 is checked because an older splash-screen PR path exists. When capacity returns, reconcile against #1285 first, then either update/supersede that PR or rename/push this local branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-appearance-manager-493` | `ceb05add` | #493 view appearance-manager tranche for AppearanceTracker repeated lock callbacks/locked poll and ThemeManager locked-theme, locked-appearance callback, and unlock behavior | `test/test_appearance_tracker.cpp` | Rebased cleanly from `31d4e8ff` onto current `origin/main` at `c4dc8625` on 2026-05-05; committed `ceb05adda47f19dba6d8a2b0ed3bc255863f3a6c`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-appearance -j$(sysctl -n hw.ncpu)` passed with existing view warning noise; focused `./build/test/pulp-test-appearance "AppearanceTracker callbacks follow repeated locks and locked poll no-op" -r compact` passed 7 assertions in 1 test case; focused `./build/test/pulp-test-appearance "ThemeManager callbacks cover locked theme poll and unlock" -r compact` passed 13 assertions in 1 test case; full `./build/test/pulp-test-appearance -r compact` passed 34 assertions in 10 test cases; exact `ctest --test-dir build -R "AppearanceTracker callbacks follow|ThemeManager callbacks cover" --output-on-failure` passed 2/2; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-tree-view-edges-493` | `94ddde56` | #493 view TreeView tranche for disclosure collapse, left-key consumed-state behavior, selected-row paint highlight, and expanded/collapsed disclosure paint output | `test/test_tree_view.cpp` | Rebased cleanly from `dc87d5fe` onto current `origin/main` at `c4dc8625` on 2026-05-05; committed `94ddde56a4a41edd4d10ef7330e752c105b26df3`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-tree-view -j$(sysctl -n hw.ncpu)` passed with existing dependency and view warning noise; focused `./build/test/pulp-test-tree-view "TreeView triangle click toggles without selecting" -r compact` passed 10 assertions in 1 test case; focused `./build/test/pulp-test-tree-view "TreeView key left consumes collapsed selected nodes without toggling" -r compact` passed 3 assertions in 1 test case; focused `./build/test/pulp-test-tree-view "TreeView paint covers selection highlight and disclosure states" -r compact` passed 7 assertions in 1 test case; full `./build/test/pulp-test-tree-view -r compact` passed 53 assertions in 15 test cases; exact `ctest --test-dir build -R "TreeView triangle click|TreeView key left consumes|TreeView paint covers" --output-on-failure` passed 3/3; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-modal-overlay-edges-493` | `95a7b597` | #493 view ModalOverlay tranche for key-release/no-callback Escape behavior, backdrop alpha paint output, and backdrop-click dismissal hit/flag guards | `test/test_modal.cpp` | Rebased cleanly from `0f30dc7a` onto current `origin/main` at `c4dc8625` on 2026-05-05; committed `95a7b59794c8d3ebc32e8c6c1a1c6be388735721`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-modal -j$(sysctl -n hw.ncpu)` passed with existing dependency and view/platform warning noise; focused `./build/test/pulp-test-modal "ModalOverlay ignores key releases and handles Escape without callback" -r compact` passed 2 assertions in 1 test case; focused `./build/test/pulp-test-modal "ModalOverlay paint applies backdrop alpha to fill color" -r compact` passed 4 assertions in 1 test case; focused `./build/test/pulp-test-modal "ModalOverlay backdrop mouse dismissal respects hit target and flag" -r compact` passed 4 assertions in 1 test case; full `./build/test/pulp-test-modal -r compact` passed 18 assertions in 8 test cases; exact `ctest --test-dir build -R "ModalOverlay ignores key releases|ModalOverlay paint applies|ModalOverlay backdrop mouse dismissal" --output-on-failure` passed 3/3; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-auto-ui-edges-493` | `39c0e3a1` | #493 view AutoUi tranche for generated toggle state, generated knob display-format branches, and sync propagation to toggles and existing faders | `test/test_auto_ui.cpp` | Rebased cleanly from `545493ad` onto current `origin/main` at `c4dc8625` on 2026-05-05; committed `39c0e3a12e64dae3ef62fd45f076883c5a7c5f8b`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-auto-ui -j$(sysctl -n hw.ncpu)` passed with existing view/platform warning noise; focused `./build/test/pulp-test-auto-ui "AutoUi generated controls expose toggle state and formatted values" -r compact` passed 10 assertions in 1 test case; focused `./build/test/pulp-test-auto-ui "AutoUi sync updates generated toggles and existing faders" -r compact` passed 6 assertions in 1 test case; full `./build/test/pulp-test-auto-ui -r compact` passed 21 assertions in 4 test cases; exact-ish `ctest --test-dir build -R "AutoUi generated controls|AutoUi sync updates" --output-on-failure` passed 3/3, including the pre-existing sync case matched by the selector; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-image-cache-trim-493` | `d9a0fc92` | #493 view ImageCache tranche for byte-budget lowering, least-recently-used trimming, and releaser behavior | `test/test_image_cache.cpp` | Rebased cleanly from `312a008f` onto current `origin/main` at `c4dc8625` on 2026-05-05; committed `d9a0fc928d483e32b34f14aef8f89c1bdb8bc2e7`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-image-cache -j$(sysctl -n hw.ncpu)` passed with existing view/platform warning noise; focused `./build/test/pulp-test-image-cache "lowering byte budget trims least recently used entries" -r compact` passed 13 assertions in 1 test case; full `./build/test/pulp-test-image-cache -r compact` passed 49 assertions in 9 test cases; exact `ctest --test-dir build -R "lowering byte budget trims|image-cache|ImageCache" --output-on-failure` passed 1/1; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-visualization-bridge-edges-493` | `3e45d531` | #493 view VisualizationBridge tranche for disabled-waveform publication, zero-channel processing, and waveform capture-length clamp paths | `test/test_visualization.cpp` | Rebased cleanly from `dc41a698` onto current `origin/main` at `c4dc8625` on 2026-05-05; committed `3e45d53193d9d7b8cdd9375429b3e940e4927e75`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-visualization -j$(sysctl -n hw.ncpu)` passed with existing view/platform warning noise; focused bridge run `./build/test/pulp-test-visualization "[vizbridge]" -r compact` passed 24 assertions in 9 test cases; full `./build/test/pulp-test-visualization -r compact` passed 38 assertions in 15 test cases; exact `ctest --test-dir build -R "VisualizationBridge skips waveform|VisualizationBridge zero-channel|VisualizationBridge clamps waveform" --output-on-failure` passed 3/3; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-waveform-editor-edges-493` | `761d4c15` | #493 view WaveformEditor tranche for selection, visible-range, playhead, paint overlay, key, and mouse edge paths | `test/test_waveform_editor.cpp` | Rebased cleanly from `9e28c887` onto current `origin/main` at `c4dc8625` on 2026-05-05; committed `761d4c15a963ef00d1679021927143499c7599ef`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-waveform-editor -j$(sysctl -n hw.ncpu)` passed with existing view/platform warning noise; focused `./build/test/pulp-test-waveform-editor "[view][waveform_editor]" -r compact` passed 62 assertions in 20 test cases; full `./build/test/pulp-test-waveform-editor -r compact` passed 62 assertions in 20 test cases; exact `ctest --test-dir build -R "WaveformEditor clamps selection|WaveformEditor zoom to selection is a no-op|WaveformEditor visible range clamps|WaveformEditor playhead clamps|WaveformEditor paint records|WaveformEditor key scrolls|WaveformEditor mouse click" --output-on-failure` passed 7/7; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-window-manager-edges-493` | `49f96229` | #493 view WindowManager tranche for unregister callback/missing-id cleanup, null host/root close behavior, and missing-handler send/broadcast paths | `test/test_window_manager.cpp` | Rebased cleanly from `7fd5052a` onto current `origin/main` at `c4dc8625` on 2026-05-05; committed `49f96229f8efa53a0edb4994672409ba74030ee1`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-window-manager -j$(sysctl -n hw.ncpu)` passed with existing view/platform warning noise; focused `./build/test/pulp-test-window-manager "[view][multiwindow]" -r compact` passed 82 assertions in 21 test cases; full `./build/test/pulp-test-window-manager -r compact` passed 82 assertions in 21 test cases; exact `ctest --test-dir build -R "WindowManager unregister invokes|WindowManager closes windows without|WindowManager send and broadcast skip" --output-on-failure` passed 3/3; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-param-attachment-edges-493` | `80bc3351` | #493 view ParamAttachment tranche for fader/toggle/combo callback forwarding, missing parameter-id no-op behavior, and `poll_bindings()` external-change propagation | `test/test_param_attachment.cpp` | Rebased cleanly from `cf7957c3` onto current `origin/main` at `c4dc8625` on 2026-05-05; committed `80bc33514185db729d9f923ce104228cda6582b1`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-param-attachment -j$(sysctl -n hw.ncpu)` passed with existing view/platform warning noise; focused `./build/test/pulp-test-param-attachment "[view][attachment][issue-493]" -r compact` passed 27 assertions in 3 test cases; full `./build/test/pulp-test-param-attachment -r compact` passed 43 assertions in 13 test cases; exact `ctest --test-dir build -R "param attachments forward|param attachments tolerate|poll_bindings forwards" --output-on-failure` passed 3/3; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-input-events-edges-493` | `235f98b2` | #493 view InputEvents tranche for wheel/meta mouse helper paths, ended/cancelled gesture deltas, key release/repeat main-modifier checks, and missing pointer-capture release behavior | `test/test_input_events.cpp` | Rebased cleanly from `c7d18912` onto current `origin/main` at `c4dc8625` on 2026-05-05; committed `235f98b2a1291a1cf31eec580d6f74ce9543f0c2`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-input-events -j$(sysctl -n hw.ncpu)` passed with existing view/platform warning noise; focused `./build/test/pulp-test-input-events "[view][input][issue-493]" -r compact` passed 29 assertions in 4 test cases; full `./build/test/pulp-test-input-events -r compact` passed 85 assertions in 22 test cases; exact `ctest --test-dir build -R "MouseEvent wheel|GestureEvent ended|KeyEvent main modifier|View pointer capture ignores" --output-on-failure` passed 4/4; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-widget-bridge-dom-493` | `d2eec1b9` | #493 view WidgetBridge DOM tranche for native DOM subtree moves, recursive DOM removal widget-map cleanup, root/missing layout helper paths, and the root-aware `getLayoutRect("")` registration fix | `core/view/src/widget_bridge.cpp`, `test/test_widget_bridge.cpp` | Rebased cleanly from `df183299` onto current `origin/main` at `c4dc8625` on 2026-05-05 with compat-sync skip trailers preserved; committed `d2eec1b9fb1c439571c5df901030f084e2929c32`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-widget-bridge -j$(sysctl -n hw.ncpu)` passed with existing view/platform warning noise plus existing `test/test_widget_bridge.cpp` missing-field initializer warnings; focused `./build/test/pulp-test-widget-bridge "[view][bridge][dom][issue-493]" -r compact` passed 29 assertions in 3 test cases; full `./build/test/pulp-test-widget-bridge -r compact` passed 815 assertions in 113 test cases; exact `ctest --test-dir build -R "WidgetBridge DOM append|WidgetBridge DOM remove|WidgetBridge layout helpers" --output-on-failure` passed 3/3; skill-sync and docs-sync reports passed or reported no mapped paths touched; compat-sync passed after `Compat-Update: skip` trailers for `canvas2d`, `css`, `html`, `imports`, `react`, `rn`, and `yoga`; version-bump report suggested SDK patch and no plugin bump; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-harness-verifier-643` | `b94b48c0` | #643 tools tranche for `tools/harness` status/verifier helper coverage, current yoga and CSS harness baseline refresh, adapter unit-test coverage discovery including current RN adapter tests, compat-sync unknown-requirement hard-error expectation, and Python coverage-runner source/omit rules for harness tests | `docs/guides/coverage.md`, `test/harness/test_css_adapter.py`, `tools/harness/tests/test_auto_discover.py`, `tools/harness/tests/test_verifier_status.py`, `tools/scripts/run_python_coverage.py`, `tools/scripts/test_compat_sync_check_extra.py`, `tools/scripts/test_run_python_coverage.py`, `tools/scripts/test_run_python_coverage_extra.py` | Rebased/amended from `e9be30f6` onto current `origin/main` at `c4dc8625` on 2026-05-05; committed `b94b48c0075c9269993c6f453978d52b839e24f8`. Yoga no-regression baseline now expects total 53, PASS 10, DIVERGE 27, NOT_IMPL 16; CSS wontfix baseline now expects 23 current catalog entries. Initial post-rebase harness run exposed the expected Yoga baseline drift, then `python3 -m unittest discover -s tools/harness/tests -p 'test_*.py'` passed 19 tests; `python3 -m unittest discover -s test/harness -p 'test_*.py'` passed 107 tests, including the RN adapter tests discovered from `test/harness/test_*.py`; `python3 tools/scripts/test_run_python_coverage.py` passed 24 tests; `python3 tools/scripts/test_run_python_coverage_extra.py` passed 8 tests; `python3 tools/scripts/test_compat_sync_check_extra.py` passed 31 tests. Venv-backed `/Users/danielraffel/Code/pulp-check-format-validation-coverage-643/build-coverage/python-venv/bin/python tools/scripts/run_python_coverage.py` passed the full Python coverage inventory with `TOTAL` 11,474 statements, 1,588 missed, 4,640 branches, 509 partials, and 84% line coverage. Report lines included `tools/harness/adapters/base.py` 93%, `tools/harness/adapters/canvas2d.py` 77%, `tools/harness/adapters/css.py` 91%, `tools/harness/adapters/html.py` 73%, `tools/harness/adapters/rn.py` 89%, `tools/harness/adapters/yoga.py` 89%, `tools/harness/status.py` 97%, `tools/harness/verifier.py` 90%, and `tools/scripts/run_python_coverage.py` 100%. `tools/check-docs.sh` passed with the existing 58 warning set; skill-sync reported no mapped paths touched; version-bump reported no SDK or plugin bump needed; docs-sync reported `docs/guides/coverage.md` updated; compat-sync reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-audio-focus-dispatch-640` | `d766f4b9` | #640 audio tranche for AudioFocusRegistry inactive-listener skip behavior when a listener is removed or reset clears the registry during dispatch | `core/audio/src/audio_focus.cpp`, `test/test_audio_focus.cpp` | Rebased from `1ab6e24b` onto current `origin/main` at `0447498e` on 2026-05-05; committed `d766f4b96c7deb493d9693ffa35183238d8d34ec`. The refreshed diff keeps the public header unchanged and covers the skip behavior via source/test changes only. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-audio-focus -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-audio-focus "AudioFocusRegistry: listener removed mid-dispatch is skipped" -r compact` passed 7 assertions in 1 test case; focused `./build/test/pulp-test-audio-focus "AudioFocusRegistry: reset during dispatch clears later callbacks" -r compact` passed 3 assertions in 1 test case; tag run `./build/test/pulp-test-audio-focus "[audio][focus][issue-640]" -r compact` passed 20 assertions in 6 test cases; full `./build/test/pulp-test-audio-focus -r compact` passed 44 assertions in 16 test cases; `ctest --test-dir build -R "AudioFocusRegistry|audio-focus|pulp-test-audio-focus" --output-on-failure` passed 16/16; skill-sync, docs-sync, and compat-sync reports passed or reported no mapped paths touched after correcting one local docs-sync invocation that used the unsupported `--head` flag; version-bump report passed with an SDK patch suggestion and no plugin bump; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-audio-tools-model-store-643` | `9743ccd7` | #643 tools tranche for `tools/audio` model registry/model store/excerpt-service edge coverage | `test/test_audio_tools.cpp` | Rebased from `a88ddfe8` onto current `origin/main` at `0447498e` on 2026-05-05; committed `9743ccd7a59cc68ad987350458a23fbe64415cb8`. The refreshed diff remains test-only in `test/test_audio_tools.cpp`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping, third-party, and platform warnings; `cmake --build build --target pulp-test-audio-tools -j$(sysctl -n hw.ncpu)` passed with existing third-party/platform warnings; focused `./build/test/pulp-test-audio-tools "[audio][tools][issue-643]" -r compact` passed 93 assertions in 8 test cases; full `./build/test/pulp-test-audio-tools -r compact` passed 188 assertions in 20 test cases; `ctest --test-dir build -R "audio model|excerpt bundle|excerpt find|audio-tools|pulp-test-audio-tools" --output-on-failure` passed 20/20; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `feature/phase3-sdl3-surface-fallback-646` | `908b3a49` | #646 render tranche for `Sdl3SurfaceInfo` backend-state validity and `extract_sdl3_surface(nullptr)` fallback/null-window behavior | `test/CMakeLists.txt`, `test/test_sdl3_surface.cpp` | Rebased cleanly onto local `origin/main` at `1fe671df` on 2026-05-04; full head SHA `908b3a49cc9fb89b8c64c19fd36eed074800eb2a`; no remote branch ref exists. `cmake -S . -B build-sdl3-surface -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed; `cmake --build build-sdl3-surface --target pulp-test-sdl3-surface -j$(sysctl -n hw.ncpu)` passed with existing third-party SDL3/Highway and platform warnings; focused `./build-sdl3-surface/test/pulp-test-sdl3-surface "SDL3 surface info validity follows backend state - issue-646" -r compact` passed 4 assertions in 1 test case; full `./build-sdl3-surface/test/pulp-test-sdl3-surface -r compact` passed 13 assertions in 2 test cases; `ctest --test-dir build-sdl3-surface -R "SDL3 surface|sdl3-surface|issue-646" --output-on-failure` passed 2/2; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD && git diff --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, push the feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-cli-config-command-643` | `af1ef6f8` | #643 CLI tranche for `pulp config` set/get/list, snooze clearing, and malformed/invalid update-key shellout diagnostics | `test/test_cli_shellout.cpp` | Created from local `origin/main` at `9e69d472` on 2026-05-04; committed `af1ef6f85b82c866ac84733644d8904e9c16d5cf`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed and `cmake --build build --target pulp-test-cli-shellout -j$(sysctl -n hw.ncpu)` compiled the shellout test TU, but GPU-off does not build the CLI target. Real shellout validation used `cmake -S . -B build-cli -DCMAKE_BUILD_TYPE=Debug -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar`, then `cmake --build build-cli --target pulp-test-cli-shellout -j$(sysctl -n hw.ncpu)`, `cmake --build build-cli --target pulp-cli -j$(sysctl -n hw.ncpu)`, and `cmake --build build-cli --target pulp-import-design -j$(sysctl -n hw.ncpu)`, all passed with existing third-party/platform warnings. Focused `./pulp-test-cli-shellout "pulp config set/get/list round-trips isolated update settings" -r compact` from `build-cli/test` passed 21 assertions in 1 test case; focused `./pulp-test-cli-shellout "pulp config rejects malformed and invalid update keys" -r compact` passed 13 assertions in 1 test case; tag run `./pulp-test-cli-shellout "[config]" -r compact` passed 34 assertions in 2 test cases; `ctest --test-dir build-cli -R "pulp config" --output-on-failure` passed 3/3. Full `./pulp-test-cli-shellout -r compact` initially exposed the existing `pulp import-design` delegate requirement for `build/tools/import-design/pulp-import-design`; after `cmake --build build --target pulp-import-design -j$(sysctl -n hw.ncpu)`, the full shellout binary passed 463 assertions in 55 test cases. skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD && git diff --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-linkwitz-riley-edges-645` | `385c51a7` | #645 signal tranche for Linkwitz-Riley reset/history and cutoff boundary finite-output behavior | `test/test_signal.cpp` | Created from local `origin/main` at `9e69d472` on 2026-05-04; committed `385c51a76beb75082087f89f2cf541023c7951fa`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping/third-party deprecation warnings; `cmake --build build --target pulp-test-signal -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-signal "LinkwitzRiley reset clears history while preserving coefficients" -r compact` passed 305 assertions in 1 test case; focused `./build/test/pulp-test-signal "LinkwitzRiley cutoff boundary processing stays finite" -r compact` passed 512 assertions in 1 test case; tag/full local runs `./build/test/pulp-test-signal "[signal]" -r compact` and `./build/test/pulp-test-signal -r compact` both passed 1764 assertions in 80 test cases; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD` and `git diff --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-adsr-edges-645` | `e4844caa` | #645 signal tranche for ADSR immediate stage progression, idle `note_off`, and reset edge paths | `test/test_signal.cpp` | Created from local `origin/main` at `b9417b41` on 2026-05-04; committed `e4844caa66c21178e69397d206330eb52e7190ff`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping/third-party warnings; `cmake --build build --target pulp-test-signal -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-signal "ADSR handles immediate stages*" -r compact` passed 17 assertions in 1 test case; tag run `./build/test/pulp-test-signal "[signal][adsr]" -r compact` passed 25 assertions in 5 test cases; full `./build/test/pulp-test-signal -r compact` passed 964 assertions in 79 test cases; `ctest --test-dir build -R "ADSR|adsr|pulp-test-signal" --output-on-failure` passed 5/5; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD` and `git diff --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-noise-gate-edges-645` | `98f73bb8` | #645 signal tranche for NoiseGate range clamp, instant attack/release timing, reset, silence, and buffer paths | `test/test_signal.cpp` | Created from local `origin/main` at `b9417b41` on 2026-05-04; committed `98f73bb8c24afabf2b0738b7fb65d32a2b65bd59`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping/third-party warnings; `cmake --build build --target pulp-test-signal -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-signal "NoiseGate clamps range*" -r compact` passed 9 assertions in 1 test case; tag run `./build/test/pulp-test-signal "[signal][gate]" -r compact` passed 11 assertions in 3 test cases; full `./build/test/pulp-test-signal -r compact` passed 956 assertions in 79 test cases; exact `ctest --test-dir build -R "^NoiseGate" --output-on-failure` passed 3/3; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD` and `git diff --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-modulation-reverb-edges-645` | `971488d5` | #645 signal tranche for Chorus dry/reset/phase-wrap behavior and Reverb zero-decay, damping clamp, dry-mix, and reset paths | `test/test_signal.cpp` | Created from local `origin/main` at `b9417b41` on 2026-05-04; committed `971488d5f4ee6c576e9aca25595574bbe713eda3`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping/third-party warnings; `cmake --build build --target pulp-test-signal -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-signal "Chorus dry mix*" -r compact` passed 30 assertions in 1 test case; focused `./build/test/pulp-test-signal "Reverb handles zero decay*" -r compact` passed 132 assertions in 1 test case; tag run `./build/test/pulp-test-signal "[signal][chorus]" -r compact` passed 33 assertions in 2 test cases; tag run `./build/test/pulp-test-signal "[signal][reverb]" -r compact` passed 133 assertions in 2 test cases; full `./build/test/pulp-test-signal -r compact` passed 1109 assertions in 80 test cases; exact `ctest --test-dir build -R "Chorus|Reverb|pulp-test-signal" --output-on-failure` passed 4/4; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD` and `git diff --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-oscillator-edges-645` | `8f51cba4` | #645 signal tranche for Oscillator reset, phase wrap, getter, and PolyBLEP edge paths | `test/test_signal.cpp` | Created from local `origin/main` at `b9417b41` on 2026-05-04; committed `8f51cba434ba556c28fe05777aa1d4d1ccb1bbaf`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping/third-party warnings; `cmake --build build --target pulp-test-signal -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-signal "Oscillator reset*" -r compact` passed 22 assertions in 1 test case; tag run `./build/test/pulp-test-signal "[signal][osc]" -r compact` passed 27 assertions in 5 test cases; full `./build/test/pulp-test-signal -r compact` passed 969 assertions in 79 test cases; exact `ctest --test-dir build -R "Oscillator|pulp-test-signal" --output-on-failure` passed 5/5; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD` and `git diff --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-validation-harness-json-493` | `974cf572` | #493 format tranche for ValidationHarness report metadata and entry JSON escaping | `test/test_validation_harness.cpp` | Rebased cleanly from `15fac4aa` onto current `origin/main` at `4fd67d84` on 2026-05-04; committed `974cf57243c6d60aaee27a390d6ccf7c49f3d6f5`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping and third-party warnings; `cmake --build build --target pulp-test-validation-harness -j$(sysctl -n hw.ncpu)` passed; focused `./build/test/pulp-test-validation-harness "ValidationHarness escapes report metadata and entry strings" -r compact` passed 5 assertions in 1 test case; full `./build/test/pulp-test-validation-harness -r compact` passed 218 assertions in 28 test cases; exact `ctest --test-dir build -R "ValidationHarness escapes report metadata and entry strings" --output-on-failure` passed 1/1; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-signal-graph-guards-493` | `247be833` | #493 host tranche for SignalGraph missing-node guard paths, remove-node connection cleanup, and default accessors | `test/test_host.cpp` | Rebased cleanly from `adcf5dcf` onto current `origin/main` at `4fd67d84` on 2026-05-04; committed `247be8331cbfbb921a45f91f669722d95499cbf3`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping and third-party warnings; `cmake --build build --target pulp-test-host -j$(sysctl -n hw.ncpu)` passed with existing third-party and test TU warnings; focused `./build/test/pulp-test-host "SignalGraph remove_node drops related connections" -r compact` passed 10 assertions in 1 test case; focused `./build/test/pulp-test-host "SignalGraph guard paths reject missing nodes and keep defaults" -r compact` passed 12 assertions in 1 test case; tag run `./build/test/pulp-test-host "[host][graph]" -r compact` passed 1018 assertions in 18 test cases; full `./build/test/pulp-test-host -r compact` passed 1056 assertions in 29 test cases with existing local plugin scan warnings; exact `ctest --test-dir build -R "SignalGraph remove_node drops related connections|SignalGraph guard paths reject missing nodes and keep defaults" --output-on-failure` passed 2/2; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-widgets-render-paths-493` | `55a90f79` | #493 view tranche for widget custom-shader CPU fallbacks, minimal render-style branches, and knob/fader/toggle interaction edges | `test/test_widgets.cpp` | Rebased cleanly from `fefb7e94` onto current `origin/main` at `4fd67d84` on 2026-05-04; committed `55a90f792451c4dfb03f1d8638a6548038987a8e`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping and third-party warnings; `cmake --build build --target pulp-test-widgets -j$(sysctl -n hw.ncpu)` passed with existing view/platform warnings; focused `./build/test/pulp-test-widgets "[issue-493]" -r compact` passed 34 assertions in 4 test cases; full `./build/test/pulp-test-widgets -r compact` passed 314 assertions in 72 test cases; exact `ctest --test-dir build -R "Audio widgets custom shader paths fall back on recording canvas|Audio widgets minimal render style paints simplified branches|Knob mouse paths update value|Fader and toggle mouse paths dispatch clamped interactive values" --output-on-failure` passed 4/4; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
| `local/phase3-audio-bridge-edges-493` | `cba288df` | #493 view tranche for AudioBridge first-pop/default meter, max-channel clamp, zero-sample analysis, and MeterBallistics tiny-value clamp paths | `test/test_audio_bridge.cpp` | Rebased cleanly from `255b0516` onto current `origin/main` at `4fd67d84` on 2026-05-04; committed `cba288dff0491dd38705097e6128838f9aa0f337`. `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DPULP_ENABLE_GPU=OFF -DPULP_BUILD_EXAMPLES=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed with existing text-shaping and third-party/macOS warnings; `cmake --build build --target pulp-test-audio-bridge -j$(sysctl -n hw.ncpu)` passed with existing view/platform warnings; focused `./build/test/pulp-test-audio-bridge "[issue-493]" -r compact` passed 40 assertions in 3 test cases; full `./build/test/pulp-test-audio-bridge -r compact` passed 63 assertions in 8 test cases; exact `ctest --test-dir build -R "AudioBridge pop_latest reports empty before first meter|AudioBridge analyze clamps channels and handles zero sample blocks|MeterBallistics releases tiny values to exact zero" --output-on-failure` passed 3/3; skill-sync, version-bump, docs-sync, and compat-sync reports passed or reported no mapped paths touched; `git diff --check origin/main...HEAD`, `git diff --check`, and `git diff --cached --check` passed; final status clean and ahead 1. | Hold local-only while Namespace is intentionally paused; when capacity returns, rename/push as a feature branch and run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` in a small resume batch. |
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
| `feature/phase3-file-dialog-coverage-640` | `6b0aecf8` | #640 platform tranche for `core/platform/src/file_dialog_stub.cpp` backend replacement, partial fallback, callback argument, and clear-backend edges | `test/test_file_dialog.cpp` | Worker-created from current `origin/main` at `4b9ad460`; `cmake -S . -B build-file-dialog-coverage-fast -DCMAKE_BUILD_TYPE=Debug -DPULP_BUILD_EXAMPLES=OFF -DPULP_ENABLE_GPU=OFF -DFETCHCONTENT_SOURCE_DIR_MBEDTLS=/Users/danielraffel/Library/Caches/Pulp/fetchcontent-src/mbedtls-v3.6.2-tar` passed; `cmake --build build-file-dialog-coverage-fast --target pulp-test-file-dialog -j$(sysctl -n hw.ncpu)` passed; `./build-file-dialog-coverage-fast/test/pulp-test-file-dialog "[file-dialog]"` passed; `ctest --test-dir build-file-dialog-coverage-fast -R "FileDialog|file-dialog|pulp-test-file-dialog" --output-on-failure` passed; skill-sync report; version-bump report; `git diff --check`. `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows` created #1256, then exited with no local targets remaining as expected for the Namespace-only route; #1256 is labeled `codecov` and linked from #640/#641. | Queued: monitor #1256 and merge once required gates are green. |
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
