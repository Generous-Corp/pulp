# Parameter, State, and Host Hardening Plan

## Goal

Close seven host-facing and real-time-safety gaps identified by comparing Pulp
with Truce 6.1, JUCE, and iPlug2, without replacing Pulp's `Processor` model or
copying framework-specific authoring abstractions.

This is a living implementation ledger. Status, validation evidence, and review
findings are updated as the work progresses.

## Baseline

- Worktree: `/Users/danielraffel/Code/pulp-parameter-state-hardening`
- Branch: `feature/parameter-state-hardening`
- Base: `origin/main` at `8efe1f9865ff792e53b7436f55e6a142eb1d9d5f`
- Reference trees: `/Users/danielraffel/Code/truce`,
  `/Users/danielraffel/Code/JUCE`, `/Users/danielraffel/Code/iPlug2`

## Comparative parameter findings

### Truce

Truce records `Float`, `Int`, `Bool`, or `Enum` independently from the numeric
range. This prevents a stepped float from accidentally becoming an integer and
lets every adapter advertise the original author intent. Enum variant names are
owned by the enum type, and all adapters route text parsing back through the
parameter implementation.

Useful invariant: semantic type is primary metadata; range and step are not a
substitute for type.

### JUCE

JUCE models float, int, bool, and choice as distinct parameter classes. The
common host contract exposes normalized values, while each typed parameter owns
plain-value formatting and parsing. `isDiscrete`, `isBoolean`, `getNumSteps`,
and choice strings are explicit host metadata rather than heuristics.

Useful invariant: normalized transport and typed plain-value semantics are
separate layers, and format/parse are a reversible pair on the typed layer.

### iPlug2

iPlug2 stores an explicit parameter type plus display-text mappings. Boolean is
a two-value enum with customizable labels. Enum and boolean parsing accepts
known labels and deliberately does not silently fall back to arbitrary numeric
input; continuous/integer parsing may use numeric fallback and then constrains
the result.

Useful invariant: display labels are data, not formatting side effects, and the
same table should drive both host enumeration and text parsing.

### Pulp decision

Adopt a hybrid:

1. Add an explicit `ParamKind` (`Continuous`, `Integer`, `Toggle`, `Enum`) to
   `ParamInfo`; never infer kind from `step`, name, or unit.
2. Add ordered value labels for enum/toggle parameters. Labels define index
   semantics and reversible text tokens.
3. Keep Pulp's raw/plain `float` storage and normalized host boundary. Centralize
   conversion, formatting, and parsing helpers so adapters do not reimplement
   policy.
4. Make parsing fallible. Custom `from_string` wins; labels are matched next;
   numeric parsing is allowed for continuous/integer parameters only. All
   successful results are finite, clamped, and snapped.
5. Preserve source compatibility: existing aggregate `ParamInfo` declarations
   default to `Continuous`; legacy stepped parameters continue to work, but only
   explicitly typed parameters receive richer host semantics.
6. Represent bypass as an independent flag, not a name comparison. This permits
   a toggle that is not bypass and localized/non-English bypass names.

## Implementation ledger

| Area | Status | Completion gate |
|---|---|---|
| Parameter semantics and canonical text conversion | Core adapter work complete; broader validation pending | All adapters consume the shared model; enum/toggle/stepped/text tests pass |
| Published custom-state snapshots | Pending | Host saves copy immutable published bytes; large-state test does no serialization in save callback |
| Standalone/headless sidechain injection | Pending | Independent main/sidechain files and harness buffers; unconnected bus is silence |
| Declarative supported bus layouts | Pending | Multiple layouts reach VST3, AU, CLAP, AAX model, standalone selection |
| Process mode | Already present on baseline; verification pending | `ProcessMode`, render-speed hint, and offline-quality helper are propagated and tested |
| Background tasks and AudioTap | Pending | Bounded, pre-warmed, lifetime-safe task lanes and whole-frame tap behavior |
| ABI exception shields | Pending | Author callbacks cannot unwind across format C/Objective-C boundaries |
| Architectural review | Pending | Must-fix findings resolved |
| Adversarial review | Pending | Correctness gaps resolved and claims revalidated |

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

## Review log

Not started.
