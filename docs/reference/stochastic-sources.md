# Stochastic signal sources

Pulp's reusable stochastic sources share one deterministic randomness layer.
They never read a clock, device identifier, or operating-system entropy source.
A public seed and a reset are enough to reproduce a render exactly, including
when the same sample sequence is divided into different process blocks.

## LFSR foundation

Include `<pulp/signal/lfsr.hpp>` and use `Lfsr` for `float` or `Lfsr64` for
`double`. `LfsrT` is the canonical configurable Fibonacci register behind the
existing Rungler. Register length is 2 through 32 bits. Each clock shifts toward
the most-significant bit and inserts the parity of `state & feedback_mask`, XOR
an optional external bit.

The default eight-bit mask, `0x8e`, has a tested period of 255 for every nonzero
seed. Caller-supplied masks are accepted as patch material and make no automatic
period claim. Zero is a legal absorbing seed until an external high bit is
clocked.

Each register bit can have a signed weight in caller-defined units. Output is
the offset plus the weights of high bits. `minimum_output()` and
`maximum_output()` report the exact configured bounds. Weight or offset updates
that could make either bound non-finite are rejected. `RunglerT` composes this
register but preserves its existing DAC transfer function and public behavior.

## Dust impulses

Include `<pulp/signal/dust.hpp>` and use `Dust` or `Dust64`. Density is in Hz on
`[0, sample_rate]`. Each sample is an independent Bernoulli trial with
probability `density_hz / sample_rate`, so the expected rate is exactly the
configured density, inter-event intervals are geometric, and at most one event
can occur at a sample position.

Level is a dimensionless magnitude on `[0, 1]`. Event amplitudes are selectable:

- `constant`: exactly `level`
- `uniform_unipolar`: uniform on `[0, level)`
- `uniform_bipolar`: uniform on `[-level, level)`

Event timing and event amplitude have independent streams derived from the same
public seed. Changing the amplitude law or level does not move any event.

## Continuous noise tilt

Include `<pulp/signal/noise_tilt.hpp>` and use `NoiseTilt` or `NoiseTilt64`.
Tilt is a power-spectral-density slope in dB per octave on `[-6, +6]`, designed
over 31.25 Hz through the lower of 16 kHz and 40 percent of sample rate. Gain is
normalized at 1 kHz. Level is a dimensionless multiplier on `[0, 1]`.

This source reuses `NoiseSourceT`'s canonical seeded white stream and a fixed
cascade of `BiquadT` high shelves. At zero tilt its output is bit-identical to
the canonical white stream. The filtered distribution remains zero-mean but is
not uniform away from zero tilt.

## Realtime and lifecycle contract

`process()`, `clock()`, block processing, seed access, and reset are bounded and
perform no allocation, locking, or I/O. LFSR and Dust have no recursive
floating-point state. Noise Tilt inherits Biquad's denormal snapping, snaps its
scaled output, and clears filter state if a non-finite result is ever observed.
All three sources report zero algorithmic latency and zero tail.

Call parameter setters and `prepare()` from the control side. Then call the
sample or block process methods from the audio thread. A reset rewinds all
random and filter state without changing the selected seed or parameters.
