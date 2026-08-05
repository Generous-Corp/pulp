# design-panel-instrument

An **instrument** whose editor is an imported design, rendered natively.

The sibling [`design-panel-plugin`](../design-panel-plugin/) proves the same
path for an audio effect. This one exists because an instrument is not an
effect with the buses relabelled:

- it declares **no audio input bus**, so a host offers it on an instrument
  track rather than an insert slot,
- it is driven entirely by **MIDI**,
- it reports an **infinite tail**, because a note can arrive at any time and a
  release runs up to eight seconds past the last note-off,
- on Apple it registers as component type **`aumu`** via `PULP_AU_INSTRUMENT`,
  not `aufx`.

A design panel that works on an effect tells you nothing about whether the same
panel survives an instrument's descriptor, and hosts scan the two differently.

## The panel

`panel.design.pulp.json` is the DesignIR for **kelvin**, one of the
agent-authored panels in [`test/fixtures/agent-panels/`](../../test/fixtures/agent-panels/).
It was written by a model given Forge's designed-panel brief, and it binds five
macros — attack, release, cutoff, resonance, drive — plus an output meter.

The IR travels **with the plugin** as an embedded string, the shape Forge's C++
export emits. A takeaway plugin has no importer, no browser and no `ui.js`
around it at run time, so anything the editor needs has to be in the binary.

## The DSP

`kelvin_synth_dsp.hpp` is eight voices of subtractive synthesis. The panel's
five macros map onto two processors Pulp already ships — `AdsrT` for the
envelope and `AnalogVcfT`, which carries cutoff, resonance and drive together —
so the file is voice allocation and an oscillator, not a filter
reimplementation.

The filter is **per voice**, not on the summed mix: on a resonant lowpass that
is the whole character, and a shared filter makes a held chord's resonance
shift audibly whenever a new note arrives.

The oscillator is a naive saw. It aliases above a few kHz, and that is honest —
this example exists to prove the panel drives audible DSP, not to be the
oscillator someone ships.

## Build and test

```bash
cmake --build build --target pulp-design-synth-test -j8
./build/examples/design-panel-instrument/pulp-design-synth-test
```

The tests assert the failures that are otherwise **silent**. The effect example
once shipped with a panel bound to macro names the plugin did not declare: zero
of five controls resolved, and it passed dlopen, auval, clap-validator, a
headless screenshot and the native-render invariants. It rendered beautifully
and drove nothing. So:

- an instrument with no notes is **exactly** silent, not approximately,
- note-off frees the voice, and the tail is actually quiet,
- cutoff changes **brightness**, measured as high-frequency energy — comparing
  total energy would not distinguish a filter from a gain,
- a repeated note retriggers rather than stacking,
- **every** `pulpParamKey` the design declares resolves to a parameter,
- the descriptor really is an instrument: `aumu`, MIDI in, no input bus,
  infinite tail.
