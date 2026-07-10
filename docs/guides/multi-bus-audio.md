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
| AU v2 **instrument** (`aumu`) | one AU **output element** per declared bus | n/a (no input) | Logic/Live/Cubase list each aux output and route it to its own channel. |
| AU v2 **effect** (`aufx`/`aumf`) | single main output only | inactive Sidechain bus | AUEffectBase is `AUBase(ci,1,1)` and pulls only input element 0, so a declared sidechain surfaces as an *inactive* bus (`sidechain_input()` returns null gracefully). Live sidechain-audio into an AU effect is not delivered by the stock render path. |
| AU v3  | descriptor bus 0 + one input bus 1 today | input bus 1 | Additional aux outputs not yet surfaced. |

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
  sidechain, wire a second input pin and confirm the plugin receives it
  (VST3/CLAP only — see the AU-effect caveat above).

Record the DAW + version and the pass/fail per bullet in the PR description; this
checklist is a human step, not a CI gate.
