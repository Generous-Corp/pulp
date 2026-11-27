# API and Header Design

How to shape public headers and APIs in Pulp. The principles are informed by
the design of [cycfi/q](https://github.com/cycfi/q) (MIT), a C++ audio DSP
library. Pulp credits it as inspiration only. No code is copied, and the
rules below are stated in Pulp's own terms and types.

Read this before adding a public header under `core/*/include/`, a new
`pulp::signal` processor, or a new top-level API. The same checklist is in the
`api-design` skill.

Code snippets marked **illustrative** show a shape and do not compile against
the current headers. Everything else is checked against `origin/main`.

## 1. Layering

The audio core's headers need only the standard library plus small
infrastructure (`pulp/runtime`, `pulp/simd`). Formats, I/O and UI are opt-in
layers above it:

```
core/signal, core/audio, core/state     audio core: std + small infra
core/format                             Processor and format adapters
core/view, core/render                  UI (Vellum builds on this layer)
```

A DSP header must never pull in a format adapter, a window host, a canvas or a
GPU type. Heavy dependencies stay in the layer that needs them, so a plugin
that renders audio only does not compile Skia headers.

Good: a signal header whose includes are all `pulp/signal`, `pulp/runtime`,
`pulp/simd` and `<std>` (see `biquad.hpp`).

Bad (illustrative): a `pulp/signal` filter that includes
`<pulp/view/view.hpp>` to expose a "draw yourself" method. The drawing code
belongs in `core/view`, calling a pure function exported by `core/signal`.

## 2. Strong types for quantities

Public DSP APIs should prefer a strong type or a literal over a bare `float`
when the quantity has a unit: frequency, time, gain. A call site then reads
unambiguously and a swapped argument fails to compile.

Apply this to new APIs. Do not churn stable ones: a signature that shipped with
`float freq_hz` keeps it, and the unit stays in the parameter name.

Today `BiquadT::set_coefficients` takes four adjacent scalars, which is easy
to misorder:

```cpp
// Compiles against core/signal/include/pulp/signal/biquad.hpp.
pulp::signal::Biquad filter;
filter.set_coefficients(pulp::signal::Biquad::Type::lowpass,
                        /*freq_hz=*/1000.0f, /*q=*/0.707f,
                        /*sample_rate=*/48000.0f);
```

A new API in the same position would take distinct types:

```cpp
// Illustrative: Pulp has no shared Hz or Decibels type today.
struct Hz { float value; };
struct Decibels { float value; };

void set_peak(Hz center, float q, Decibels gain, Hz sample_rate);
set_peak(Hz{1000.0f}, 0.707f, Decibels{-3.0f}, Hz{48000.0f});
```

If a new API needs a unit type that no header owns yet, add one small shared
type rather than a private copy per header.

## 3. Small, composable processors

Build DSP from small processors that each do one thing, then compose them
inside `process()`. `Processor` is the plugin-level unit, and the
`pulp::signal` helpers (`Biquad`, `DcBlocker`, `DelayLine` and the rest) are
the building blocks. A fixed chain is composition, not a graph; see
`docs/reference/processing-models.md` for when `SignalGraph` is justified.

Good:

```cpp
// Compiles against core/signal/include/pulp/signal/{biquad,dc_blocker}.hpp.
struct Tone {
    pulp::signal::DcBlocker<float> dc;
    pulp::signal::Biquad lowpass;

    float process(float x) { return lowpass.process(dc.process(x)); }
};
```

Bad (illustrative): one `ToneStageWithDcAndFilterAndMeter` class that owns the
filter, the DC blocker and a meter publisher, so none of the three can be
tested or reused alone.

Use C++20 concepts to constrain composition where a constraint clarifies the
contract, for example "any type with `float process(float)`". Do not add a
concept only to restate what the signature already says.

## 4. Separate processing, model and presentation

Three things stay apart:

| Concern | Pulp type | Rule |
|---|---|---|
| Processing | `format::Processor` | Audio thread only; reads parameters, never touches views |
| Model | `state::StateStore`, parameters | Owns values and ranges; thread-safe reads |
| Presentation | `Processor::editor_factory()` and `format::Editor` | Reads and writes the model through ports; never reaches into the processor |

Good: `define_parameters()` registers the model, `process()` reads atomics from
it, and the editor binds to the same parameters.

```cpp
// Compiles against core/format/include/pulp/format/processor.hpp.
void define_parameters(pulp::state::StateStore& store) override {
    store.add_parameter({.id = 1, .name = "Gain", .range = {-60, 12, 0}});
}
```

Bad (illustrative): `process()` calling into a widget to read a knob value, or
a view caching a parameter that the processor also owns.

When a class computes an intermediate that a widget or test could draw or
verify, expose it as a pure function. That rule has its own section: see
"Expose the Pipeline, Not Just the Endpoints" in `CLAUDE.md`.

## 5. Lean headers

- Include only what the header itself uses. Prefer a forward declaration for a
  type used by pointer or reference in a declaration.
- No surprise transitive includes: a header must not be the reason another file
  compiles. If a `.cpp` needs `<vector>`, it includes `<vector>`.
- Do not add an include of a heavy header to a widely included one. One such
  edge can recompile hundreds of translation units.
- Put heavy implementation details in `detail/` or the `.cpp`.

Good:

```cpp
// Declaration needs only a forward declaration.
namespace pulp::state { class StateStore; }
void bind_meter(pulp::state::StateStore& store);
```

Bad: including `<pulp/state/store.hpp>` from a header that only names
`StateStore&`.

`tools/scripts/header_fanout_guard.py` enforces part of this. It counts how
many translation units include each tracked header and fails a change that
grows a header past its ceiling in `header_fanout_guard.json`. It cannot see
a header that is merely heavier than it should be, so review includes by hand.

## 6. Documentation order

Order docs as fundamentals, then graduated examples, then reference:

1. Fundamentals: what the component is, the invariant it keeps, and the
   real-time contract, in a few sentences at the top of the header or guide.
2. Examples: a minimal use first, then one example per added concept. Each
   example compiles or is marked illustrative.
3. Reference: the full parameter, range and edge-case tables.

A reader who stops after the first paragraph should still not misuse the API.

## Checklist

Before opening a PR that adds or changes a public header:

- The header includes nothing from a layer above its own.
- New quantities with units use a strong type, or the unit is in the name.
- The processor is one job, and composes through the existing helpers.
- Processing, model and presentation do not reach into each other.
- Intermediates a UI or test could draw are exported as pure functions.
- Includes are minimal, with forward declarations where they suffice.
- Docs follow fundamentals, examples, reference, and examples compile.
