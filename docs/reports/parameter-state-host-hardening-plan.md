# Parameter, State, and Host Hardening Plan

## Goal

Close seven host-facing and real-time-safety gaps in Pulp's parameter, state,
and host-integration surfaces, without replacing Pulp's `Processor` model or
introducing framework-specific authoring abstractions.

This is a living implementation ledger. Status, validation evidence, and review
findings are updated as the work progresses.

## Design principles

Two invariants drive the parameter work:

- **Normalized transport and typed plain-value semantics are separate layers.**
  The host contract speaks normalized values; the typed layer owns plain-value
  formatting and parsing, and format/parse are a reversible pair on that layer.
- **Display labels are data, not a formatting side effect.** One ordered label
  table drives both host enumeration and text parsing, so the two can never
  disagree.

## Design decisions

1. Add an explicit `ParamKind` (`Continuous`, `Integer`, `Toggle`, `Enum`) to
   `ParamInfo`; never infer kind from `step`, name, or unit. Discreteness,
   booleanness, step count, and choice strings are explicit host metadata rather
   than heuristics.
2. Add ordered value labels for enum/toggle parameters. Labels define index
   semantics and reversible text tokens.
3. Keep Pulp's raw/plain `float` storage and normalized host boundary. Centralize
   conversion, formatting, and parsing helpers so adapters do not reimplement
   policy.
4. Make parsing fallible. Custom `from_string` wins; labels are matched next;
   numeric parsing is allowed for continuous/integer parameters only, so an enum
   or toggle never silently accepts arbitrary numeric input. All successful
   results are finite, clamped, and snapped.
5. Preserve source compatibility: existing aggregate `ParamInfo` declarations
   default to `Continuous`; legacy stepped parameters continue to work, but only
   explicitly typed parameters receive richer host semantics.
6. Represent bypass as an independent flag, not a name comparison. This permits
   a toggle that is not bypass and localized/non-English bypass names.

## Implementation ledger

| Area | Status | Completion gate |
|---|---|---|
| Parameter semantics and canonical text conversion | Core adapter work complete; broader validation pending | All adapters consume the shared model; enum/toggle/stepped/text tests pass |
| Published custom-state snapshots | Complete | Host saves copy immutable published bytes; large-state test does no serialization in save callback |
| Standalone/headless sidechain injection | Complete | Independent main/sidechain files and harness buffers; unconnected bus is silence |
| Declarative supported bus layouts | Complete | Multiple layouts reach VST3, AU, CLAP, AAX model, standalone selection |
| Process mode | Complete on baseline and revalidated | `ProcessMode`, render-speed hint, and offline-quality helper are propagated and tested |
| Background tasks and AudioTap | Complete | Bounded, pre-warmed, lifetime-safe task lanes and whole-frame tap behavior |
| ABI exception shields | Complete | Author callbacks cannot unwind across format C/Objective-C boundaries |
| Architectural review | Complete | Must-fix findings resolved |
| Adversarial review | Complete | Correctness gaps resolved and claims revalidated |

## Constraints

- No stateless-descriptor rewrite.
- No Rust-style derive system transplanted into C++.
- No unbounded allocation, thread creation, blocking, or host calls from
  `Processor::process()`.
- Existing parameter initializers and saved sessions remain compatible.
- Format adapters translate policy; they do not each own separate semantics.
- Release builds and focused tests are the truth.

## Validation log

- Baseline audit found that current `origin/main` already contains
  `ProcessMode`, `RenderSpeedHint`, `ProcessContext::is_realtime()`,
  `is_offline()`, and `allows_offline_quality_work()`. This item will be
  validation/documentation rather than duplicate implementation.
- Release configure started for the focused CLAP entry test target.
- Release build verified with `-O3 -DNDEBUG` for `pulp-test-clap-entry`.
- `pulp-test-clap-entry`: PASS, 284 assertions in 18 test cases. Covers custom
  parser precedence, locale-independent numeric conversion, enum label
  round-trip, rejection of numeric enum text, unit validation, and snapping.
- `pulp-test-lv2-adapter`: PASS, 105 assertions in 13 test cases, including
  LV2 integer/enumeration properties and labeled scale points.
- `pulp-test-aax-model`: PASS, 137 assertions in 16 test cases. The AAX model
  now carries the canonical metadata and advertises the number of discrete
  values rather than re-deriving an interval count in the runtime.
- `PulpGain_AUv3`: Release build PASS after AUv3 value strings and canonical
  format/parse callback integration.
- `pulp-test-plugin-state-io`: PASS, 164 assertions in 15 test cases. A 2 MiB
  published immutable snapshot round-trips while the legacy live serializer is
  provably not invoked by the host save path. Plugins that do not publish retain
  the existing compatibility fallback.
- `pulp-test-headless`: PASS, 218 assertions in 26 test cases. Independent
  main/sidechain buffers reach separate process buses without aliasing; the
  ordinary no-sidechain path clears the sidechain view instead of retaining a
  stale bus from the previous block.
- `pulp-test-headless`: expanded PASS, 225 assertions in 27 test cases. Offline
  rendering accepts a separate sidechain `AudioFileData`, preserves independent
  widths and block schedules, rejects mismatched sample rates/frame counts, and
  supplies silence beyond sidechain data during a rendered tail.
- Declarative `supported_bus_layouts` now constrains the shared Processor
  validator, drives CLAP `audio-ports-config`, AUv2/AUv3 capability reporting,
  AAX component expansion, VST3's existing arrangement negotiation, and
  headless selection. The layout type preserves the existing two-vector
  aggregate field order for source compatibility.
- Layout validation: `pulp-test-clap-entry` focused port suite PASS (54
  assertions/2 cases), `pulp-test-aax-model` PASS (142/17),
  `pulp-test-processor-layout-latency` focused suite PASS (15/4), and
  `pulp-test-headless` focused suite PASS (222/26). `PulpGain_AUv3` Release
  build also passes with AU channel-capability integration.
- Added typed `BackgroundTaskLane`: fixed-capacity ordered lanes report
  overflow, latest-value lanes coalesce bursts, each lane has one prewarmed
  consumer (serialized per type), and separate lanes run concurrently. Focused
  tests PASS (108 assertions/2 cases).
- Added `AudioTap` over the existing fixed planar SPSC ring. Overflow drops
  complete frames across every channel; focused stereo alignment test PASS
  (6 assertions/1 case).
- Canonical parameter format/parse, plugin custom-state save/load/rollback, and
  ViewBridge editor creation/size/open/close/resize callbacks now contain
  author exceptions and return safe defaults. Throwing-callback tests PASS for
  parameter text (2 assertions) and state IO (2 assertions); `pulp-format`
  Release build passes with editor guards.
- Full focused regression after implementation: CLAP 302 assertions/19 cases,
  LV2 105/13, AAX model 142/17, plugin-state IO 166/16, headless 337/30,
  Processor layout/latency/mode 129/20, and planar ring/AudioTap 60/7 all PASS.
  `PulpGain_AUv3` also builds successfully in Release.

## Review log

### Architectural review

- Found and fixed a source-compatibility regression where adding the layout
  name before the existing input/output vectors changed positional aggregate
  initialization. The name is now the trailing optional field.
- Kept task concurrency compositional: one serialized worker per typed lane,
  rather than a global pool that would couple unrelated plugin work. All
  producer storage is fixed before realtime use.
- Reused `PlanarAudioRingBuffer` for AudioTap instead of introducing a second
  ring implementation with different overflow semantics.
- Extended the existing shared parameter/state/view seams so formats inherit
  one policy rather than accumulating adapter-specific exception behavior.

### Adversarial review

- Found a startup race in latest-value task lanes: the worker could establish
  its initial generation after the producer published, treating the newest task
  as stale. Generation is now reset before thread creation; the focused suite
  passed 20 consecutive stress repetitions.
- Found that a throwing plugin-state rollback callback still reached a debug
  assertion and aborted the process. Rollback failure now remains a reported
  restore failure and never becomes a host-process abort.
- Found and removed a duplicate include introduced in the AUv2 layout patch.
- Expanded editor containment to active scripted-session lookup in addition to
  create/size/open/close/resize callbacks.
- Rechecked selected CLAP layouts at activation (not only metadata query), AAX
  per-layout native IDs, unconnected sidechain silence, large snapshot save
  behavior, enum numeric rejection, and full focused regression. No unresolved
  must-fix finding remains.

### Pre-merge review

- Code-health review found that new sidechain/task coverage pushed
  `test_headless.cpp` from 969 to 1,108 lines and mixed unrelated
  responsibilities. Extracted `pulp-test-format-hardening`; the original suite
  is now 984 lines and the focused suite owns sidechain/task behavior.
- Adversarial review found raw `try/catch` in the public background-task header,
  which failed Pulp's `-fno-exceptions` portability contract, and a `noexcept`
  author handler that would terminate on an exception. The lane now uses the
  portable exception macros, contains handler failures, continues to later
  tasks, and passes both a throwing-handler test and a direct `-fno-exceptions`
  header compile.
- Found `supported_bus_layouts` inserted before the existing trailing
  `supports_f64_audio` descriptor field. Moved it to the true end and added a
  legacy positional-initializer regression test.
- Separated named descriptor layouts from `Processor::BusesLayout`; the latter
  remains the original two-vector negotiation type, eliminating warnings and
  avoiding host metadata in the runtime proposal contract.
- Found explicit enum/integer/toggle parameters could still store fractional
  values through direct, RT, normalized, or deserialize paths when no numeric
  step was supplied. Centralized explicit-kind quantization in `StateStore`
  while preserving legacy continuous direct-set behavior.
- Focused review-fix validation passes: format hardening 127 assertions/5
  cases, explicit-kind StateStore 5/1, layout/source compatibility 17/5, CLAP
  port/config 54/2, and headless layout selection 4/1.
- Final affected-suite run passes: StateStore 1,825 assertions/98
  cases, CLAP 302/19, LV2 105/13, AAX model 142/17, plugin-state IO 166/16,
  headless 214/26, format hardening 127/5, Processor layout/latency/mode 131/21,
  and AudioTap/ring 60/7. `PulpGain_AUv3` builds successfully in Release.
- Changed-header standalone check compiled nine public headers. The AUv2 header
  could not be checked by that local script because this build lacks the
  external `AudioUnitSDK/AUMIDIEffectBase.h`; the AUv3 Objective-C++ adapter
  build above covers the locally available Apple adapter surface, and cloud CI
  owns the AUv2 SDK-enabled header lane.
- The first governed macOS run exposed three stale semantic seams: a continuous
  VST3 gain with a quantization step was advertised as 840 discrete steps, two
  AUv2 tests still rejected the agreed numeric text fallback, and AAX fixtures
  relied on step inference instead of declaring enum kind. Semantic discreteness
  now comes only from `ParamKind`; AUv2 fallback tests assert successful
  round-trips; AAX enum fixtures declare their kind and labels. The exact three
  failures pass in Release, followed by StateStore 1,825/98, CLAP 302/19, LV2
  105/13, and AAX model 142/17.
