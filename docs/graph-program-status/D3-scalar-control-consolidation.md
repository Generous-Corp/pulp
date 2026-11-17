# D3: one scalar Control library

## What is done

`core/signal/include/pulp/signal/control_math.hpp` is the canonical home of
the shared scalar Control operations: `finite_control`, `control_min`,
`control_max`, `control_clamp`, `safe_control_divide`, `integrator_step` and
`ControlIntegratorT`. `ControlIntegratorT::process` computes through
`integrator_step`, so the stateful and pure integrator forms share one
implementation.

`core/signal/include/pulp/signal/indexed_control.hpp` includes
`control_math.hpp`. Its four redundant spellings are `[[deprecated]]`
forwarding wrappers with a migration message naming the canonical function:

| Deprecated | Canonical |
|---|---|
| `finite_control_sample` | `finite_control` |
| `control_minimum` | `control_min` |
| `control_maximum` | `control_max` |
| `control_safe_divide` | `safe_control_divide` |

No public spelling was removed. Hysteresis (`hysteresis_step`,
`HysteresisT`), slew (`slew_step`, `SlewT`), `map_clip`/`MapClipT` and
`IndexedControlT` are unique behaviour and stay as they are. `SlewT` and
`SlewLimiterT` (`mod_tools.hpp`) differ in units, configuration and zero-rate
behaviour, so neither is an alias of the other.

In-tree callers use the canonical names: the graph Control nodes in
`core/host/src/control_index_nodes.cpp` already did, and the tests that used
the indexed spellings were converted. The migration note is
`docs/migrations/unreleased/control-scalar-canonical-names.md`.

## Evidence

- `test/test_control_scalar_parity.cpp` (`pulp-test-control-scalar-parity`):
  pinned canonical values for NaN, infinity, signed zero, denormals,
  saturation, reversed bounds, reset and refused configuration, for float and
  double; bit-identical forwarding of every deprecated spelling over a
  16-value edge grid; bit-identical agreement of the min, max and safe-divide
  graph nodes with the canonical functions over the same grid; integrator
  node agreement with `ControlIntegratorT`, directly and through
  `SignalGraph::process_control_checked`.
- `control-scalar-installed-headers` ctest: stages the signal headers with the
  SDK header install rule and builds a consumer against that prefix only. The
  canonical consumer builds with deprecation as an error; the legacy consumer
  builds with default diagnostics and returns canonical bits; the legacy
  source under deprecation-as-error must fail and name every deprecated
  spelling and `pulp/signal/control_math.hpp`.

## Remaining: removal at a breaking SDK release

Remove the four deprecated wrappers from `indexed_control.hpp` only at the
next announced breaking SDK release, and only after at least two ordinary
releases that carried the migration note. A graph-document format version
bump does not authorize this C++ API removal. The removal change:

1. Delete `finite_control_sample`, `control_minimum`, `control_maximum` and
   `control_safe_divide` from `indexed_control.hpp`.
2. Delete `check_deprecated_spellings_forward` and its test case from
   `test/test_control_scalar_parity.cpp`, and `legacy.cpp` plus the legacy and
   strict targets from `test/fixtures/control_scalar_consumer/`, with the
   matching steps in `test/cmake/test_control_scalar_consumer.cmake`.
3. Write a `breaking = true` migration note that repeats the table above.
4. Refresh the agent-capability manifest for `indexed_control.hpp`.
