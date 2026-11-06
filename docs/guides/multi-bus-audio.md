# Multi-bus audio (aux outputs & sidechain)

A Pulp `PluginDescriptor` declares its bus topology as vectors of `BusInfo`:

```cpp
PluginDescriptor descriptor() const override {
    PluginDescriptor d;
    d.category = PluginCategory::Instrument;
    d.input_buses  = {};                         // instrument: no audio input
    d.output_buses = {                           // 1 main + 7 aux, all stereo
        {"Voice 1 (Main)", 2}, {"Voice 2", 2}, /* … */ {"Voice 8", 2},
    };
    d.accepts_midi = true;
    return d;
}
```

To address the aux buses, override the richer surface and write each bus by
index — voice `v` into `audio.outputs[v]`:

```cpp
void process(ProcessBuffers& audio, MidiBuffer& midi_in, MidiBuffer&,
             const ProcessContext&) override {
    for (std::size_t b = 0; b < audio.outputs.size(); ++b) {
        auto& bus = audio.outputs[b];
        if (!bus.active() || bus.buffer.num_channels() == 0) continue;  // disconnected
        voices_[b].render(bus.buffer, bus.buffer.num_samples());
    }
}
```

A processor that only implements the simple `process(output, input, …)` writes
just the main bus; the format adapters pre-zero every routed output bus, so aux
buses read silence rather than uninitialised memory. See
`examples/pulp-multi-out` for a complete 8-voice / 8-bus instrument.

## How each format exposes buses

| Format | Output buses | Sidechain input | Notes |
|--------|--------------|-----------------|-------|
| VST3   | `addAudioOutput` per declared bus (main + aux) | second input bus (`kAux`) | Full multi-in/out. |
| CLAP   | one audio-output port per declared bus | second input port | Full multi-in/out. |
| AU v2 **instrument** (`aumu`) | one AU **output element** per declared bus | main input **element** 0 + second input element named "Side Chain" when input buses are declared | Logic/Live/Cubase list each aux output and route it to its own channel. Declare the inputs `optional`; the adapter pulls each connected element itself and leaves an unconnected one inactive. A side chain needs a main input bus in front of it (elements are positional). |
| AU v2 **effect** (`aufx`/`aumf`) | single main output only | second input **element** named "Side Chain" | A declared sidechain bus adds input element 1, so Logic offers its Side Chain pop-up. The adapter pulls element 1 before the main render and delivers it as the Sidechain bus; while the host leaves it unconnected `sidechain_input()` returns null. |
| AU v3  | descriptor bus 0 + one input bus 1 today | the input bus after the main input (bus 0 when the main input declares 0 channels) | Additional aux outputs not yet surfaced. Instruments with declared inputs get the same input buses as effects. |
| AAX | main stem | one optional **mono** sidechain | An Instrument-category plug-in may declare its main input `optional` (an input stem on a generator); an effect's main input must be required. |
| LV2 | one audio port per output channel | the second input bus's ports, marked `lv2:isSideChain` | Ports stay one flat run; optional buses carry `lv2:connectionOptional`, and the runtime routes the second bus to `sidechain_input()`. |

## Instruments that also process audio

An instrument that can take audio (a vocoder, a granular processor with an
internal oscillator) declares its inputs `optional`:

```cpp
d.category = PluginCategory::Instrument;
d.input_buses = {{"Main In", 2, true}, {"Side Chain", 2, true}};
```

Each format then exposes them the way its hosts expect: VST3 advertises every
optional bus as `kAux` and not default-active, which is what Ableton Live's
instrument Sidechain chooser keys on (a required instrument input would be a
`kMain` bus Live cannot feed from the chooser); CLAP lists the ports, which
Bitwig routes from any track; AU v2 and AU v3 create the input elements/buses
described above, so Logic shows its Side Chain pop-up on the instrument; AAX
carries an input stem on the generator; LV2 marks the ports optional. The
standalone host feeds the configured input device into the main input at the
device's width and has no side chain.

Disconnected buses are always delivered inactive/silent **without reordering**
the bus→buffer mapping: `audio.outputs[b].info.index` is stable, so a plugin can
always address a bus by its declared index.

## Manual DAW bus-visibility checklist (NOT automated)

`auval` validates that a multi-output `aumu` negotiates its output elements and
renders each bus, and the headless tests
(`pulp-test-au-v2-busses`, `pulp-multi-out-test`) assert the bus count / names /
roles / routing. **None of that proves a real DAW shows and routes the aux
outputs** — that is verified by hand. Load `examples/pulp-multi-out` (or your
plugin) and confirm:

- **Logic Pro** (AU): add the instrument on a Software Instrument track; the
  aux outputs appear as *Multi-Output* options. Create aux/return channels for
  "Voice 2"…"Voice 8" and confirm each voice's notes meter only on its own
  channel.
- **Ableton Live** (VST3/AU): the instrument shows extra audio-output choices in
  the device's output routing (or per-return in the mixer); route each to a
  separate audio track and confirm isolation.
- **Cubase / Nuendo** (VST3): "Activate Outputs" lists every declared output
  bus; enable the aux outputs and confirm the channels appear in the mixer.
- **Reaper** (VST3/CLAP): the FX shows N output pins; wire pins 3–16 to child
  tracks / hardware outs and confirm per-voice isolation. For an effect
  sidechain, wire a second input pin and confirm the plugin receives it.
- **Logic Pro** (AU effect sidechain): insert the effect on an audio track; the
  plug-in header shows a *Side Chain* pop-up. Pick another track as the source
  and confirm the plugin reacts to it (and stops reacting with *None*).

Record the DAW + version and the pass/fail per bullet in the PR description; this
checklist is a human step, not a CI gate.
