# Modulation and utility toolkit

The layer complex DSP composes from. Sources that move, wires that shape what
they move, events that fire things, envelopes that shape the firing, and the
routing between them — so a plugin's `process()` reads like a patch rather than
like three hundred lines of inline modulation arithmetic.

Everything here lives in `pulp::signal` and follows the house conventions:
`FooT<SampleType = float>` with `Foo` / `Foo64` aliases, `prepare(sample_rate)`
for anything that computes coefficients, and an explicit RT contract in every
header. All of it is allocation-free after `prepare()` and covered by the
allocation-probe roster.

The three event-only counters (`ClockDividerT`, `ClockMultT`, and `TrigDelayT`)
contain no sample-valued state. Their `Foo64` spellings therefore alias the same
precision-independent implementation instead of adding a dummy template
parameter.

`LpgT` uses `SampleType` for its sample-valued audio/control state and public
sample-valued controls. Consequently `Lpg64` keeps those calculations in double
precision rather than only widening the final return value.

```cpp
#include <pulp/signal/lfo.hpp>       // or the umbrella <pulp/signal/signal.hpp>
#include <pulp/signal/envelope.hpp>
#include <pulp/signal/lpg.hpp>
#include <pulp/signal/mod_tools.hpp>
```

## What is in it

| Header | What it gives you |
|--------|-------------------|
| `rng.hpp` | `Xorshift32`, `gaussian()`, `unit_from()`, `OuWalkT`, `DriftT` |
| `lfo.hpp` | `LfoT` — the full-option low-frequency oscillator |
| `mod_tools.hpp` | `SlewLimiterT`, `SampleHoldT`, `AttenuverterT`, `RectifierT`, `ComparatorT`, `QuantizerT`, `CurveT`, the shared curve functions, and bipolar/unipolar conversions |
| `trigger.hpp` | `TriggerDetectT`, `GateGenT`, `ClockDividerT`, `ClockMultT`, `BurstGenT`, `TrigDelayT` |
| `envelope.hpp` | `ArT`, `AdT`, `AhdT`, `DahdsrT`, `ModEnvT`, `TransientDetectorT` |
| `vca.hpp` | `VcaT` |
| `lpg.hpp` | `LpgT` |
| `mod_matrix.hpp` | `ModMatrixT` |
| `units.hpp` | dB / MIDI / cents / time conversions and the shared `Division` table |
| `chaos.hpp` | `LogisticMapT` |

`AdsrT` (`adsr.hpp`) is the existing keyboard envelope and is unchanged. The
toolkit is additive.

## Two conventions worth knowing up front

**Randomness is deterministic.** Every generator is seeded explicitly and never
reads a clock or `std::random_device`. Two instances with the same seed produce
bit-identical streams forever, so an offline bounce, a golden-file test, and the
live render all agree. `reset()` rewinds to *your* seed, not to a default one.

**Curves are labelled from the musician's end.** One law shapes every envelope
stage and every burst spacing:

```
shaped(p) = (1 - e^(-k·p)) / (1 - e^(-k)),   k = 8·curve
```

`curve = +1` is "exponential" and `-1` is "logarithmic" in the sense a drum rack
means them — on a *rising* stage that is slow-then-fast, and on a *falling* one
it is fast-then-tailing. `curve_rise()` and `curve_fall()` apply the law with the
sign each direction needs, so `+1` means the same thing on every stage of every
envelope. Use those two rather than `stage_curve()` directly.

## Control-signal utility API

The utility types in `mod_tools.hpp` are available as `FooT<SampleType>`
templates, with `Foo` and `Foo64` aliases for `float` and `double`. The free
curve and conversion functions use `float`. Setters are control-side operations.
`process()`, `reset()`, accessors, and the free functions allocate nothing and
are safe to call from the audio thread.

### Curve functions

The four shaping functions accept normalized progress and clamp `p` to `[0, 1]`.
`bi_to_uni()` and `uni_to_bi()` accept signal values and deliberately do not
clamp, so values outside their nominal input ranges remain outside the nominal
output ranges.

| Function | Behavior |
|----------|----------|
| `stage_curve(p, curve)` | Applies the raw stage law. Both arguments are clamped (`p` to `[0, 1]`, `curve` to `[-1, 1]`); `curve = 0` is linear. Prefer the direction-specific helpers for envelope stages. |
| `curve_rise(p, curve)` | Maps progress to a rising `0 -> 1` stage. `+1` is slow-then-fast exponential; `-1` is fast-then-easing logarithmic. |
| `curve_fall(p, curve)` | Maps progress to a falling `1 -> 0` stage. `+1` drops quickly into a long exponential tail; `-1` holds and then falls. |
| `smoothstep(p)` | Applies `3p² - 2p³` after clamping `p` to `[0, 1]`, producing zero slope at both ends. |
| `bi_to_uni(x)` | Converts bipolar to unipolar with `x * 0.5 + 0.5`. |
| `uni_to_bi(x)` | Converts unipolar to bipolar with `x * 2 - 1`. |

`kCurveSpan` is the fixed strength of the shared stage law (`8.0f`).

### `SlewLimiterT`

Rate-limits a target with independent rise and fall times. The default is
linear mode, a value of `0`, and `10 ms` in both directions. Call `prepare()`
with the actual sample rate before processing.

| Method | Behavior |
|--------|----------|
| `prepare(sample_rate)` | Sets the sample rate and updates the time coefficients. A non-positive value is normalized to `1 Hz`. |
| `set_mode(mode)` | Selects `Mode::linear` or `Mode::exponential`. |
| `set_rise_ms(ms)` | Sets the upward time; negative values become `0`. In linear mode this is the duration of a full-scale `0 -> 1` move. In exponential mode it is the one-pole time constant. |
| `set_fall_ms(ms)` | Sets the downward time with the same clamping and mode-dependent meaning as `set_rise_ms()`. |
| `set_times_ms(rise, fall)` | Sets both directional times. |
| `reset(value = 0)` | Moves the current output to `value` without a ramp. |
| `process(target)` | Advances one sample toward `target` and returns the new value. Zero or sub-sample times jump immediately. |
| `current()` | Returns the current output without advancing. |
| `mode()` | Returns the active mode. |

### `SampleHoldT`

Latches an input on a clock's low-to-high transition. Its default held value is
`0` and glide is disabled.

| Method | Behavior |
|--------|----------|
| `prepare(sample_rate)` | Prepares the internal slew limiter. Required before using a non-zero glide time. |
| `set_glide_ms(ms)` | Sets equal rise and fall glide times; `0` or a negative value disables glide and produces steps. |
| `set_glide_mode(mode)` | Selects the internal `SlewLimiterT::Mode`. |
| `reset(value = 0)` | Sets the held and output values and clears the remembered clock state. A high clock on the next call is therefore a new rising edge. |
| `process(input, clock)` | Latches `input` only on a rising edge, then returns either the held value or the next glided value. |
| `held()` | Returns the value captured at the last rising edge, before glide. |

### `AttenuverterT`

Applies `y = x * gain + offset`. It defaults to identity (`gain = 1`,
`offset = 0`).

| Method or constant | Behavior |
|--------------------|----------|
| `kMaxGain` | Maximum gain magnitude, `2`. |
| `set_gain(gain)` | Sets gain, clamped to `[-2, 2]`; negative values invert. |
| `set_offset(offset)` | Sets the additive offset without clamping. |
| `process(x)` | Returns `x * gain + offset`. |
| `gain()` | Returns the effective, clamped gain. |
| `offset()` | Returns the offset. |

### `RectifierT`

Defaults to `Mode::full_wave`.

| Method | Behavior |
|--------|----------|
| `set_mode(mode)` | Selects `Mode::full_wave` (`abs(x)`) or `Mode::half_wave` (`max(0, x)`). |
| `process(x)` | Applies the selected rectification mode. |

### `ComparatorT`

Turns a signal into a stateful gate with hysteresis. It defaults to threshold
`0`, gate low, and an automatic margin on each side of the threshold of
`max(0.001, 0.05 * abs(threshold))`.

| Method or constant | Behavior |
|--------------------|----------|
| `kDefaultHysteresisFraction` | Automatic hysteresis fraction, `0.05`. |
| `kMinAutoHysteresis` | Automatic hysteresis floor, `0.001`. |
| `set_threshold(threshold)` | Sets the threshold and recomputes automatic hysteresis unless `set_hysteresis()` has selected an explicit value. |
| `set_hysteresis(hysteresis)` | Uses the absolute value as the margin on each side of the threshold and keeps it fixed across later threshold changes. The full dead band is twice this value. Construct or assign a fresh instance to return to automatic hysteresis. |
| `reset(gate = false)` | Sets the current gate state. |
| `process(x)` | Goes high only above `threshold + hysteresis` and low only below `threshold - hysteresis`; equality preserves the current state. |
| `gate()` | Returns the current gate state without processing. |
| `hysteresis()` | Returns the active margin on either side of the threshold. |

### `QuantizerT`

Snaps a value to evenly spaced levels. It defaults to eight levels across
`[0, 1]`.

| Method | Behavior |
|--------|----------|
| `set_range(lo, hi)` | Sets the output endpoints. Use ascending endpoints (`lo <= hi`); equal endpoints produce the single value `lo`. |
| `set_steps(count)` | Sets the number of output levels, clamped to `[1, 1024]`. Two levels means the two endpoints, not two intervals. |
| `process(x)` | Clamps to the configured range and returns the nearest level. With one level it always returns `lo`; ties follow `std::lround`. |
| `steps()` | Returns the effective, clamped level count. |

### `CurveT`

Shapes a normalized control value. It defaults to `Shape::stage_curve` with a
linear curve value of `0`.

| Method | Behavior |
|--------|----------|
| `set_shape(shape)` | Selects `Shape::stage_curve` or `Shape::smoothstep`. |
| `set_curve(curve)` | Stores the stage curve, clamped to `[-1, 1]`. It does not affect output while smoothstep mode is active and applies again after switching back. |
| `process(x)` | Clamps `x` to `[0, 1]` and applies the selected shape. Stage-curve mode uses the rising-stage convention. |

## Choosing an envelope

| Use | Reach for |
|-----|-----------|
| A pad or drone VCA, gate-CV amplitude, a duck-and-recover shape | `ArT` |
| Percussion, plucks, filter pings — anything fired by an event | `AdT` |
| A drum voice or sample: transient and body at full level, then a tail | `AhdT` |
| A keyboard instrument with sustain | `AdsrT` (existing) |
| Layered or orchestral patches; the default mod-matrix source | `DahdsrT` |
| Per-hit modulation shaping with a signed depth | `ModEnvT` |
| "Did something get hit", independent of how loud it was | `TransientDetectorT` |

An ADSR with sustain pinned to 1 is an `ArT` with three more parameters to
explain. Reach for the smaller shape.

## `VcaT` vs `LpgT`

Both turn a control into a level, and they are not interchangeable.

`VcaT` changes amplitude and nothing else — the right tool for tremolo,
auto-pan, choppers, sidechain-style pumping, and AM. It is exactly unity at full
control, so a VCA left open is bit-transparent.

`LpgT` is a vactrol model: one control moves amplitude **and** cutoff together,
because that is how a struck object behaves — as its level falls, its highs die
first. A pinged LPG reads as *a thing that was hit*. Its closing is also
deliberately not exponential (fast at first, then lengthening), which is the
decay contour an RC envelope misses.

Use `LpgT` for percussion, plucks, mallets, and for a delay's feedback path
where each echo should get darker as well as quieter. Use `VcaT` for sustained
material that must keep a constant timbre, and use the envelope family when you
need transient accuracy — the vactrol's lag and droop are character, and
character is the wrong tool when you need precision.

---

## The patch cookbook

Complete patches from these primitives, with starting settings. Every one of
them is a composition of two to six objects; none of them needs a new class.

### P1 — Tremolo and auto-pan

```cpp
LfoT lfo;             lfo.prepare(sr); lfo.set_rate_hz(5.5);
AttenuverterT depth;  depth.set_gain(0.3f); depth.set_offset(0.7f);
VcaT vca;             vca.prepare(sr); vca.set_response(Vca::Response::exponential);

out = vca.process(in, depth.process(lfo.next()));
```

The exponential response reads more "amp-like" than linear, because loudness
perception is closer to logarithmic than the control is.

For auto-pan, drive two VCAs from `next_quadrature()`. The pair is constant
power by construction (`sin² + cos² = 1`), which two offset triangles are not.

### P2 — Sidechain pump with no compressor

```cpp
TriggerDetectT kick;  kick.prepare(sr); kick.set_threshold(0.5f);
ArT duck;             duck.prepare(sr); duck.set_attack_ms(5.0);    // the duck
                                        duck.set_release_ms(220.0); // the recover
AttenuverterT invert; invert.set_gain(-0.8f); invert.set_offset(1.0f);
VcaT vca;             vca.prepare(sr);
```

Deterministic, and immune to the kick's level changing — the pump is driven by
the *event*, not by the amplitude. That is the property a compressor sidechain
does not have.

### P3 — Trance gate

```cpp
LfoT gate; gate.prepare(sr);
gate.set_period_samples(units::division_to_samples(units::Division::sixteenth, bpm, sr));
gate.set_wave(Lfo::Wave::square);
gate.set_pulse_width(0.6f);          // the pulse width IS the gate length

SlewLimiterT edge; edge.prepare(sr); edge.set_times_ms(2.0f, 2.0f);  // the click knob
```

### P4 — Random-step acid filter

```cpp
LfoT clock;      clock.set_period_samples(/* 1/16 */);
ComparatorT cmp; cmp.set_threshold(0.0f);
SampleHoldT sh;  sh.prepare(sr);
QuantizerT q;    q.set_range(200.0f, 6000.0f); q.set_steps(12);
SlewLimiterT glide; glide.prepare(sr); glide.set_mode(SlewLimiter::Mode::exponential);
                    glide.set_times_ms(20.0f, 20.0f);
```

Fully deterministic per seed. The `QuantizerT` is what makes it read as a
sequence rather than as noise, and the slew is what stops it clicking.

### P5 — Ratchet bongo

```cpp
TriggerDetectT hit;   hit.prepare(sr);
BurstGenT burst;      burst.prepare(sr); burst.set_count(4);
                      burst.set_spacing_ms(40.0);
                      burst.set_spacing_curve(-1.0f);   // decelerating: a drag
                      burst.set_levels(1.0f, 0.45f);
AhdT body;            body.prepare(sr); /* 1 ms / 4 ms / 30 ms */
LpgT cell;            cell.prepare(sr); cell.set_decay_ms(150.0); cell.set_colour(0.5f);

if (auto h = burst.process(hit.process(trigger)); h.fired) {
    body.trigger(h.level);
    cell.strike(Lpg::velocity_to_strike(h.level));
}
out = cell.process(noise.next_bipolar() * body.next());
```

Six primitives and it is a playable instrument. The roll crescendos on its own:
a re-strike into a still-conducting cell starts from the cell's current state,
which is what makes an LPG roll sound like a roll instead of a machine gun.

### P6 — Delayed vibrato

```cpp
LfoT vibrato; vibrato.prepare(sr);
vibrato.set_rate_hz(5.8);
vibrato.set_delay_ms(400.0);
vibrato.set_fade_in_ms(600.0);
vibrato.set_mode(Lfo::Mode::retrig);
// on note-on: vibrato.retrigger();

const float ratio = units::cents_to_ratio(25.0f * vibrato.next());
```

The performance cliché done right — vibrato that arrives as the note settles,
with no envelope wired to the depth.

### P7 — Breathing pad

A looping `DahdsrT` with synced stage times into a gated `LpgT` at
`colour = 0.7`. Rhythmic spectral swells, groove-locked, from two primitives.
Looping skips the sustain stage: an envelope waiting for a note-off cannot also
be a cycling source.

### P8 — Live echo

An `LpgT` in a delay's feedback path, struck from a `TransientDetectorT` on the
dry input. Echoes darken per pass **and** duck open with the playing. A static
lowpass in the loop gives constant colour at falling level, which is the
giveaway of a cheap echo.

### P9 — Generative drift layer

`OuWalkT` into a `QuantizerT(7)` into a `SlewLimiterT` on any timbre parameter,
plus `DriftT::pitch_factor()` on tune. The "nothing repeats, nothing jumps"
analog-life layer, with one knob (`sigma`) from subtle to seasick.

---

## Where this replaces existing code

The exact follow-up migration inventory is:

- private LFOs in `chorus.hpp` and `phaser.hpp`, plus the four-shape oscillator
  in the Forge lo-fi catalog's `lfo` node;
- xorshift32 implementations in `lofi_chain.hpp`, `noise_source.hpp`,
  `character_delay/primitives.hpp`, `drum/clap.hpp`, and
  `fdn/modulation.hpp`; spectral processors also carry a separate xorshift64
  family whose sequence compatibility must be handled independently;
- walk/drift implementations in `character_delay/primitives.hpp` and
  `fdn/modulation.hpp`;
- the drum low-pass gate implemented in `lowpass_gate.hpp` and used directly
  by `drum/membrane.hpp`;
- the FDN transient ducker in `fdn/stages.hpp`.

These are named migration targets, not permission for silent swaps. The Forge
catalog node's triangle runs in the opposite phase to `LfoT`'s, PRNG migrations
can change deterministic renders, and character primitives can change baked
sound. Each migration needs its own compatibility proof.

## In Forge

Five of these primitives are exposed to Forge's effect lane as catalog nodes
(`pulp/host/forge_modulation_catalog.hpp`): `mod_lfo`, `lpg`, `slew`,
`transient`, and `trig_env`. They follow the existing CV convention, where a
control signal is a unipolar `[0, 1]` signal on an ordinary audio port, so
modulation stays ordinary graph topology.
