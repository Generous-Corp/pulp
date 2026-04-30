# Phase 3 Codecov Queue Pause Ledger

Last updated: 2026-04-30 EDT

This local ledger records the open `codecov` PR validation runs paused to free Namespace capacity for higher-priority work. Branches, PRs, commits, labels, and tracker comments stay intact; only queued GitHub Actions validation attempts are cancellable and replaceable.

## Pause Policy

- Do not dispatch new Codecov Namespace runs while this pause is active.
- Continue local-only tranche preparation, review, and failure triage.
- Merge `UNSTABLE` PRs only when GitHub branch protection shows all required checks are green and the PR can merge normally.
- Resume Codecov in small batches by branch and pinned head SHA using `shipyard cloud run build <branch> --require-sha <sha>` or the workflow-specific equivalent.

## Snapshot Summary

- Open `codecov` PRs in snapshot: 73
- Queued/in-progress Codecov-branch workflow runs selected for cancellation: 193
- Selected workflows: Build and Test, Coverage, Sanitizer Tests

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

## Local-Only Work Prepared During Pause

These branches were prepared after the Namespace pause began. They have
not been pushed, PR'd, or dispatched to Namespace.

| Branch | Head | Scope | Files | Local Validation | Resume Action |
| --- | --- | --- | --- | --- | --- |
| `local/phase3-docs-generate-coverage-643` | `1f1b0bbf` | #643 tooling tranche for `tools/docs_generate.py` paths | `tools/scripts/test_docs_generate.py` | `python3 tools/scripts/test_docs_generate.py`; `python3 tools/scripts/test_run_python_coverage.py`; skill-sync report; version-bump report; `git diff --check`. Full local Python coverage command blocked because this machine lacks `coverage.py >= 7.10`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |
| `local/phase3-list-limitations-coverage-643` | `9040b518` | #643 tooling tranche for `tools/list_limitations.py` paths | `tools/scripts/test_list_limitations.py` | `python3 tools/scripts/test_list_limitations.py`; `python3 tools/test_list_limitations.py`; `python3 tools/scripts/test_run_python_coverage.py`; skill-sync report; version-bump report; `git diff --check`. Full local Python coverage command blocked because this machine lacks `coverage.py >= 7.10`. | When capacity returns, rename/push as a feature branch, run `shipyard pr --skip-target mac --skip-target ubuntu --skip-target windows`, then dispatch Namespace with `shipyard cloud run build <branch> --require-sha HEAD`. |

## Cancelled/Paused Runs

| PR | State | Branch | Head | Runs | Title |
| --- | --- | --- | --- | --- | --- |
| #1045 | UNKNOWN | `feature/events-service-discovery-coverage-642-next6` | `5783c552714e` | `Build and Test 25168056508`<br>`Coverage 25168033097`<br>`Sanitizer Tests 25168032974`<br>`Build and Test 25168032955` | test(events): cover service discovery edge paths |
| #1051 | UNKNOWN | `feature/signal-poly-math-coverage-645` | `98e7ace40ae6` | `Build and Test 25172061996`<br>`Coverage 25172040856`<br>`Build and Test 25172040818`<br>`Sanitizer Tests 25172040812` | fix(signal): handle degenerate polynomial roots |
| #1062 | UNKNOWN | `codex/coverage-midi-edge-644` | `7c532a7414f1` | `Build and Test 25170865514`<br>`Build and Test 25170844844`<br>`Sanitizer Tests 25170844747`<br>`Coverage 25170844738` | test(midi): cover factory data byte bounds |
| #1066 | UNKNOWN | `feature/signal-filter-meter-coverage-645` | `facbf9038b58` | `Build and Test 25170500099`<br>`Coverage 25170474161`<br>`Sanitizer Tests 25170474151`<br>`Build and Test 25170474021` | fix(signal): clamp meter process channel counts |
| #1071 | UNKNOWN | `feature/background-scanner-restart-coverage-493` | `f959d20f76ce` | `Build and Test 25171074566`<br>`Sanitizer Tests 25171040147`<br>`Coverage 25171040113`<br>`Build and Test 25171040103` | test(host): cover background scanner restart after cancel |
| #1072 | UNKNOWN | `feature/design-import-edge-coverage-493` | `858791335582` | `Build and Test 25172032962`<br>`Build and Test 25172003397`<br>`Sanitizer Tests 25172003385`<br>`Coverage 25172003360` | test(view): cover design import edge paths |
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
| #1114 | UNKNOWN | `feature/runtime-file-library-coverage-641-next` | `796e36a2896a` | `Build and Test 25164337154`<br>`Build and Test 25164286882`<br>`Sanitizer Tests 25164286847`<br>`Coverage 25164286830` | test(runtime): cover file and library helper edges |
| #1115 | UNKNOWN | `feature/runtime-license-analytics-coverage-641-next` | `67157a45fc68` | `Build and Test 25164886652`<br>`Coverage 25164835133`<br>`Sanitizer Tests 25164835113`<br>`Build and Test 25164835092` | test(runtime): cover license and analytics edges |
| #1116 | UNKNOWN | `feature/signal-interpolator-coverage-645-next` | `7832eae87eaa` | `Build and Test 25165535849`<br>`Build and Test 25165511969`<br>`Coverage 25165511967`<br>`Sanitizer Tests 25165511951` | test(signal): cover interpolator endpoints and impulses |
| #1117 | BLOCKED | `feature/lcov-cobertura-coverage-643-next` | `7c5d7d67dbfc` | `Build and Test 25165561249`<br>`Coverage 25165524012`<br>`Build and Test 25165524010` | test(ci): cover lcov cobertura edge paths |
| #1118 | UNKNOWN | `feature/osc-bundle-coverage-641-next` | `6735bc0d519b` | `Build and Test 25165563643`<br>`Build and Test 25165526241`<br>`Coverage 25165526234`<br>`Sanitizer Tests 25165526222` | test(osc): cover malformed bundle edges |
| #1119 | UNKNOWN | `feature/state-undo-history-coverage-641-next` | `34efa649b9fd` | `Build and Test 25165878118`<br>`Sanitizer Tests 25165848583`<br>`Coverage 25165848571`<br>`Build and Test 25165848554` | test(state): cover undo history edge cases |
| #1120 | UNKNOWN | `feature/descriptor-validation-coverage-493-next` | `14a965de6152` | `Build and Test 25166025406`<br>`Coverage 25165994906`<br>`Sanitizer Tests 25165994899`<br>`Build and Test 25165994884` | test(format): cover descriptor validation edges |
| #1121 | UNKNOWN | `feature/audio-buffering-reader-coverage-640-next` | `363a1692a771` | `Build and Test 25166210546`<br>`Build and Test 25166195268`<br>`Coverage 25166195230`<br>`Sanitizer Tests 25166195212` | test(audio): cover buffering reader tail lifecycle paths |
| #1122 | UNKNOWN | `feature/render-draw-batcher-coverage-646-next` | `c1a8b031c54d` | `Build and Test 25166218297`<br>`Sanitizer Tests 25166203865`<br>`Build and Test 25166203856`<br>`Coverage 25166203850` | test(render): cover draw batcher merge blockers |
| #1123 | UNKNOWN | `feature/host-scan-cache-coverage-493-next` | `c1792a45fe1b` | `Build and Test 25166249254`<br>`Sanitizer Tests 25166234957`<br>`Build and Test 25166234954`<br>`Coverage 25166234934` | test: cover host scan cache fallback paths |
| #1125 | UNKNOWN | `feature/view-asset-manager-coverage-493-next` | `be13ce28b68f` | `Build and Test 25167200088`<br>`Coverage 25167178443`<br>`Sanitizer Tests 25167178429`<br>`Build and Test 25167178420` | test(view): cover asset manager edge paths |
| #1126 | UNKNOWN | `feature/view-frame-clock-coverage-493-next` | `e8dca439b850` | `Build and Test 25167592036`<br>`Coverage 25167561161`<br>`Sanitizer Tests 25167561146`<br>`Build and Test 25167561113` | test(view): cover frame clock lifecycle edges |
| #1127 | UNKNOWN | `feature/render-texture-atlas-coverage-646-next` | `7b6a7bfc4f8b` | `Build and Test 25167987526`<br>`Sanitizer Tests 25167952725`<br>`Coverage 25167952691`<br>`Build and Test 25167952688` | test(render): cover texture atlas full-capacity edges |
| #1128 | UNKNOWN | `feature/audio-workgroup-coverage-640-next` | `e4875168d49b` | `Build and Test 25168404842`<br>`Build and Test 25168364432`<br>`Coverage 25168364417`<br>`Sanitizer Tests 25168364375` | test(audio): cover workgroup fallback lifecycle |
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
| #1140 | BLOCKED | `feature/status-ladder-coverage-643` | `bf87273e0534` | `Build and Test 25173851676`<br>`Coverage 25173829139`<br>`Build and Test 25173829138` | test(tools): cover status ladder checker |
| #1141 | BLOCKED | `feature/ship-appcast-coverage-644-next` | `2d2c119b4c8d` | `Build and Test 25173929796`<br>`Build and Test 25173909885`<br>`Sanitizer Tests 25173909871`<br>`Coverage 25173909805` | test(ship): cover appcast malformed optional fields |
| #1142 | BLOCKED | `codex/package-tools-coverage-643` | `cb6fb9c4e9c3` | `Build and Test 25174428148`<br>`Build and Test 25174415212`<br>`Coverage 25174415161` | test(packages): cover validation tool edge paths |
| #1143 | BLOCKED | `feature/render-compute-coverage-646` | `2b2ccde36e63` | `Build and Test 25174471464`<br>`Sanitizer Tests 25174429661`<br>`Coverage 25174429510`<br>`Build and Test 25174429474` | test(render): cover gpu compute pool bookkeeping |

## Resume Checklist

1. Confirm Namespace capacity is available.
2. Re-fetch each PR branch and verify the remote branch still points to the recorded head SHA.
3. Re-dispatch in small batches, starting with the most merge-ready PRs.
4. After each batch, merge only PRs with required checks green and `mergeStateStatus` clean enough for normal merge.
5. Update this ledger with resumed run IDs and merge SHAs.
