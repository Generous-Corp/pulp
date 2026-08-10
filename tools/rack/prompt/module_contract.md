# Generating a VCV Rack module

You are generating one Eurorack-style module for VCV Rack, built on the Pulp audio SDK.

Output **exactly two fenced blocks and nothing else** — no commentary, no explanation:

````
```json manifest
{ ...the layout manifest... }
```

```cpp dsp
// ...the module's DSP...
```
````

---

## 1. The manifest

One JSON object describing the panel and the module's controls. Fields:

```jsonc
{
  "slug": "GATEDLY",          // PERMANENT identity. A-Z0-9 only, no spaces. Unique.
  "name": "GATE",             // shown on the panel. See the name-length rule below.
  "description": "One line. This is what the Module Browser searches.",
  "tags": ["Clock modulator"],// MUST come from the tag list in §5. Canonical spelling only.
  "hp": 6,                    // panel width in HP. See §2.
  "poly_follows": "IN_INPUT", // OPTIONAL. Required if you tag "Polyphonic".

  "sections": [               // OPTIONAL. Labelled dividers for dense panels (12HP+).
    { "label": "SHAPE", "y_mm": 60.0 }
  ],

  "params": [
    { "id": 0, "ident": "RATE_PARAM", "name": "Rate", "label": "RATE",
      "unit": " Hz", "min_value": -4.0, "max_value": 4.0, "default_value": 0.0,
      "kind": "Knob", "x_mm": 15.24, "y_mm": 30.0,
      "display_base": 2.0, "display_multiplier": 2.0 }
  ],
  "inputs":  [ { "id": 0, "ident": "CLK_INPUT",  "name": "Clock", "label": "CLK",
                 "role": "Clock", "x_mm": 9.0,  "y_mm": 84.0 } ],
  "outputs": [ { "id": 0, "ident": "OUT_OUTPUT", "name": "Gate",  "label": "OUT",
                 "role": "Gate",  "x_mm": 21.5, "y_mm": 84.0 } ],
  "lights":  [ { "id": 0, "ident": "ACT_LIGHT", "name": "Active", "label": "",
                 "x_mm": 15.24, "y_mm": 66.0, "color": "green", "size": "small" } ]
}
```

**Ids must start at 0 and be contiguous within each of params / inputs / outputs.**
Rack serialises params by index — a gap or a duplicate silently corrupts saved patches.

`kind` — one of: `KnobLarge` `Knob` `KnobSmall` `Trimpot` `Toggle` `SwitchThree` `LightButton` `Slider`
`role` — one of: `Audio` `Cv` `Pitch` `Gate` `Trigger` `Clock`
`color` — `green` `red` `blue` `yellow` `white` `green_red` `rgb`   ·   `size` — `tiny` `small` `medium` `large`

**Switches need position labels**, or the tooltip shows a number:
`"kind": "SwitchThree", "labels": ["Off", "Half", "Full"]` (exactly 3; `Toggle` takes exactly 2).

**Normalled inputs** — a jack with a value when nothing is patched. This is what makes a module
useful in isolation, and it is pervasive in real modules:
`"normal_volts": 10.0` (a constant) or `"normal_to": "OTHER_INPUT_IDENT"` (chain from another jack).

**Arrays** for repeated controls (sequencer steps, mixer strips) — never hand-place these:
```jsonc
"param_array": [{
  "ident_fmt": "STEP%d_PARAM", "label_fmt": "%d", "name_fmt": "Step %d",
  "count": 8, "id_start": 0,
  "grid": { "cols": 4, "x0_mm": 12.0, "y0_mm": 40.0, "dx_mm": 12.0, "dy_mm": 16.0 },
  "template": { "min_value": -2.0, "max_value": 2.0, "default_value": 0.0,
                "kind": "KnobSmall", "unit": " V" }
}]
```
(`input_array`, `output_array`, `light_array` work identically.)

## 2. Geometry — these are hard constraints, not style

- Panel is **128.6933 mm tall**, always. Width is **HP × 5.08 mm**.
- Usable width is `HP × 5.08`. Keep every widget at least its own radius from each edge.
- **Screws occupy y ≈ 2.5 mm and y ≈ 126.2 mm** — put nothing there.
- The header (name + accent rule) occupies **y < 15 mm**. First control at y ≥ 26.
- Drawn radii, in mm: `KnobLarge` 9.2 · `Knob` 6.1 · `KnobSmall` 4.3 · `Trimpot` 2.9 ·
  jack 4.1 · light 1.4 · `Slider` is 3.0 wide × 14.0 tall.
- **A label sits ABOVE its control**, and the emitter places it automatically — you only supply
  `label`. Leave ≥ 9 mm of vertical space above any control that has one.
- **Widgets must not overlap.** Two controls need ≥ (r1 + r2 + 0.5) mm between centres.
- **A label must not land on another widget either.** The label sits in the ~9 mm band above
  its own control, and anything else drawn in that band collides with it — most often a light
  placed just above a jack. The validator rejects `label 'IN' collides with widget X`. Give a
  light its own row, or move it beside the control rather than above it.
- **Name length**: at 3 HP the name must be ≤ 4 characters. At 6 HP ≤ 8. Wider is freer.
- Typical column x positions: 3 HP → centre only (7.62). 6 HP → 8.8 and 21.7.
  8 HP → 11.8 and 28.8. 12 HP → 4 columns at 9.6, 23.2, 36.8, 50.4.
- **Check the arithmetic before using two columns.** A panel is `HP × 5.08` mm wide, and two
  `Knob`s need 12.7 mm between centres (6.1 + 6.1 + 0.5). At 6 HP the panel is 30.48 mm, so
  two mediums side by side fit only just — hence 8.8 and 21.7, which is 12.9 apart. Two
  `KnobLarge` (9.2 each) need 18.9 mm and **do not fit at 6 HP at all**: use `KnobSmall`, or
  stack the two controls in separate rows instead of side by side. Doing the subtraction is
  cheaper than being rejected for it.

**Rows**: controls sharing a y get one shared label baseline, so keep a row's controls the same
`kind` where you can.

## 3. Voltage standards — every module must obey these

| Signal | Standard |
|---|---|
| Audio | **±5 V** |
| CV, unipolar | **0…10 V** |
| CV, bipolar | **±5 V** |

**Every parameter must change the output ON ITS OWN.** The behaviour gate moves one
parameter at a time from its default, leaves every other control where it started, and fails
any knob whose full range produces no measurable change. So a control that only matters in
combination with another reads as broken:

- A **CV-amount** knob is inert if the thing it scales sits at a default where modulation
  cannot show. If a module has `FALL` and `FALL CV`, `FALL` must default somewhere the CV can
  visibly push it — not at a floor or ceiling where scaling it changes nothing.
- Prefer fewer controls that each do something to more controls that only work together. A
  4 HP module with two live knobs is better than one with five, three of which the gate --
  and the user -- will find dead.

**A parameter's default must leave the knob somewhere it can move from.** Two rejections come
up constantly and both are about where a knob STARTS:

- **A bipolar range must default to its centre.** `-1..1` defaults to `0`; `-2..2` defaults to
  `0`. A bipolar control that starts at `0.5` or `1.0` shows a knob pointing off to one side
  for what the user reads as the neutral setting.
- **Nothing defaults to the top or bottom of its range.** A `0..1` parameter defaulting to
  `1.0` gives a knob with nowhere left to turn; the module appears to have a broken control
  before it is even patched. Choose a value inside the range that sounds right — for a mix or
  amount, that is usually the middle.
| Pitch | **1 V/oct**, `f = f0 · 2^V`; f0 = C4 = 261.6256 Hz (audio), 2 Hz (LFO/clock) |
| Gate / trigger | **10 V** when active; a trigger is ~1 ms |
| Trigger detection | Schmitt: rises at **1.0 V**, falls at **0.1 V** |

Constants are in `pulp::format::rack::volts`: `kAudioPeak`, `kCvUnipolar`, `kCvBipolar`,
`kGateHigh`, `kSchmittLow`, `kSchmittHigh`, `voct_to_hz(v, ref)`, `kPitchRefHz`, `kLfoRefHz`.

## 4. The DSP

A C++ file. Rack calls `process()` **once per sample** — there is no block. Pulp's primitives are
already per-sample, so use them directly.

Template — follow this shape exactly, substituting SLUG:

```cpp
#include "plugin.hpp"

#include <pulp/format/rack/module_descriptor.hpp>
// ...only the pulp/signal headers you actually use...

#include <algorithm>
#include <cmath>

namespace {

namespace V = pulp::format::rack::volts;

struct SLUGModule : rack::engine::Module {
    using L = forge_modular::SLUGLayout;

    // state here (per-channel arrays if polyphonic)

    SLUGModule() {
        forge_modular::config_SLUG(this);       // generated: params, ports, lights
        // configBypass(L::IN_INPUT, L::OUT_OUTPUT);   // if it has an audio thru-path
    }

    void onSampleRateChange(const SampleRateChangeEvent& e) override {
        // re-prepare anything rate-dependent
    }

    void process(const ProcessArgs& args) override {
        // args.sampleRate, args.sampleTime
    }
};

struct SLUGWidget : rack::app::ModuleWidget {
    explicit SLUGWidget(SLUGModule* m) {
        setModule(m);
        setPanel(rack::createPanel(
            rack::asset::plugin(pluginInstance, "res/SLUG.svg"),
            rack::asset::plugin(pluginInstance, "res/SLUG-dark.svg")));
        forge_modular::place_SLUG(this, m);
    }
};

}  // namespace

rack::plugin::Model* modelSLUG = rack::createModel<SLUGModule, SLUGWidget>("SLUG");
```

### Available DSP

**You MUST build the module from these where one fits.** They are tested, they are shared with
the DAW products, and hand-rolling something that already exists here is a defect, not a
shortcut. Write DSP inline ONLY when nothing below covers it — and if you do, say so in a
one-line comment explaining what was missing.

Rack's own `rack::dsp::PulseGenerator` and `rack::dsp::SchmittTrigger` are fine for gate/trigger
plumbing, but **not** as substitutes for the signal processing below.

<!--DSP_VOCABULARY-->

### Idioms you must follow

```cpp
// Reading a clock or gate input — persistent member state, never a local bool.
// Member declaration:
pulp::signal::HystereticTriggerDetectT<float> edge_;
// Constructor or onSampleRateChange setup:
edge_.set_thresholds(V::kSchmittHigh, V::kSchmittLow);
// Per-sample use; true only on the rising edge:
const bool rising = edge_.process(v);

// 1V/oct: sum in VOLTS, convert once. This is what makes tracking exact.
float volts = knob + inputs[L::VOCT_INPUT].getPolyVoltage(c);
float hz = V::voct_to_hz(volts);

// Emitting a trigger (10 V, ~1 ms)
rack::dsp::PulseGenerator pulse;      // member
pulse.trigger(1e-3f);                 // on the event
outputs[L::OUT_OUTPUT].setVoltage(pulse.process(args.sampleTime) ? V::kGateHigh : 0.f);

// Polyphony (only if you declared poly_follows)
const int ch = forge_modular::channels_SLUG(this);
outputs[L::OUT_OUTPUT].setChannels(ch);
for (int c = 0; c < ch; ++c) { /* ...per-channel state... */ }

// A NORMALLED input — use the GENERATED accessor, never re-implement the normal.
// The accessor exists ONLY for inputs that declared a `normal`, and is named
// read_<SLUG>_<PORT ID>_INPUT. Calling it for a plain input is a compile error:
// nothing was generated for that port.
float cv = forge_modular::read_SLUG_CV_INPUT(this, c);

// A PLAIN input — read it straight from Rack. There is no generated accessor.
float in = inputs[L::IN_INPUT].getPolyVoltage(c);

// Lights: smooth them, or they strobe at audio rate
lights[L::ACT_LIGHT].setBrightnessSmooth(x, args.sampleTime);
```

### Rules

- **Every param must be read** in `process()`. A knob wired to nothing is worse than no knob.
- **Never allocate** in `process()` — no `new`, no `std::vector` resize, no locks.
- Outputs that are expensive should check `.isConnected()` first.
- Reset state in `onSampleRateChange`.
- **Never touch `APP` in a constructor.** Assume 48 kHz there; Rack calls
  `onSampleRateChange` when the module is added, which is where the real rate arrives. A
  constructor that reads `APP->engine->getSampleRate()` cannot be instantiated outside Rack,
  and every generated module is driven headlessly by the behavioural gate before it ships.
- Clamp anything that could blow up: filter cutoff to `[20, sampleRate*0.45]`, gains to sane ranges.

## 5. Tags — use ONLY these, spelled exactly

Arpeggiator, Attenuator, Blank, Chorus, Clock generator, Clock modulator, Compressor, Controller,
Delay, Digital, Distortion, Drum, Dual, Dynamics, Effect, Envelope follower, Envelope generator,
Equalizer, Expander, External, Filter, Flanger, Function generator, Granular, Hardware clone,
Limiter, Logic, Low-frequency oscillator, Low-pass gate, MIDI, Mixer, Multiple, Noise, Oscillator,
Panning, Phaser, Physical modeling, Polyphonic, Quad, Quantizer, Random, Recording, Reverb,
Ring modulator, Sample and hold, Sampler, Sequencer, Slew limiter, Speech, Switch, Synth voice,
Tuner, Utility, Visual, Vocoder, Voltage-controlled amplifier, Waveshaper

## 6. What makes a module good rather than merely valid

- **Small and composable** beats a monolith. 3 HP is the most common width in the ecosystem.
- **CV inputs on the parameters that matter** — this is the entire point of modular. A knob with
  no CV input is a missed opportunity.
- **Normal your inputs** so the module does something sensible unpatched.
- Give every port and param a **real name** — Rack shows it on hover.
- Prefer a few controls that interact interestingly over many that do not.
