# Nonlinear shaping

`<pulp/signal/nonlinear_shaping.hpp>` provides three focused processors for
effects and instruments that need richer harmonic generation than the legacy
`WaveShaper` curves:

- `MultistageWavefolderT` applies one to eight sequential triangle-fold stages.
  Later stages receive progressively more drive, so stage count changes the
  transfer function rather than repeating an idempotent clamp. Every stage has
  fixed-capacity offset, symmetry, and DC-coupling controls.
- `ChebyshevHarmonicShaperT` exposes coefficients for harmonics 1 through 16.
  It evaluates the series by recurrence and translates every even polynomial
  so a zero input still produces zero output.
- `NonlinearRingModulatorT` owns a bandlimited sine, triangle, or square carrier
  and offers an ideal multiplier plus a quasi-static diode-ring transfer law.
  Carrier frequency, bipolar/unipolar mode, modulation index, output polarity,
  nonlinear drive, and phase are explicit controls.

The float aliases omit the `T<float>` suffix; each processor also has a `64`
alias for double precision.

## Aliasing and timing

All three processors use `NonlinearAliasPolicy`:

| Policy | Processing | Exact host latency at standard quality |
|---|---|---:|
| `oversample_4x` | Two-stage linear-phase FIR; shipping default | 76 samples |
| `oversample_2x` | One-stage linear-phase FIR | 64 samples |
| `off` | Raw host-rate transfer function | 0 samples |

The reported latency comes directly from `OversamplerT`; callers should query
`latency_samples()` rather than restating the table. The transfer functions
have no feedback or autonomous decay, but the finite FIR response still needs
draining: `tail_samples()` reports exactly twice the selected linear-phase
latency (152 samples at 4x, 128 at 2x, and zero when off).

A DC-coupled wavefolder offset is the exception: it emits a constant for zero
input, so `tail_samples()` returns Pulp's infinite-tail sentinel `-1` while its
configured `dc_output()` is nonzero. Centering every active stage with
`dc_coupling = 0` restores the finite FIR tail.

`off` is useful for an intentional aliased sound, CPU-floor measurements, and
tests that compare the antialiased path with a naive reference. It is not an
antialiased quality tier. The default 4x path is the appropriate starting point
for bright input, deep folding, high Chebyshev orders, and carrier sum
sidebands near Nyquist.

## Realtime lifecycle

Call `prepare(sample_rate)` and all setters outside the audio callback.
Preparation configures the shared oversampler and may allocate. After that,
`process()`, `process_block()`, and `reset()` allocate no memory and take no
locks. Block processing accepts in-place buffers and treats a null input or
output as a no-op. It is a sample loop over the same persistent state, so
splitting an input into different block sizes is deterministic.

A non-finite audio sample returns zero and resets filter and oscillator state.
Non-finite parameter writes are ignored. The raw wavefolder is bounded to
`[-1, 1]` with zero stage offsets and to `[-2, 2]` when local zero-point
subtraction is active; the Chebyshev input is constrained to its mathematical
design domain `[-1, 1]`.

Changing alias policy resets the FIR histories. The ring modulator also resets
carrier phase and the shared virtual-analog oscillator's pending BLEP/BLAMP
correction, so a quality switch has one deterministic restart rule.
Internally, carrier phase is offset by the upsampling FIR's half of the reported
round-trip latency, aligning it with the delayed audio presented to the
nonlinear callback. The public phase remains the logical input-timeline phase.

## Transfer-function observables

The folder and Chebyshev shaper expose `small_signal_gain()` so graph builders
and tests can inspect the exact derivative at the origin. Their centered
transfer functions and the balanced ring model expose zero as `dc_output()`.
`shape()` evaluates the raw transfer law directly when a UI, plot, or offline
analysis needs the curve without advancing oversampling state.

### Wavefolder stage law

For stage input `x`, stage drive `g`, offset `o`, and symmetry `s`, the folder
evaluates `triangle(g*x + o + s*x*x)`. The quadratic term makes the positive and
negative halves differ while preserving the default slope at zero. All stage
offsets and symmetries default to zero, preserving the plain driven cascade.

Each stage's `dc_coupling` controls how much of that stage's zero-input
operating point is passed to the next stage. At one, the raw offset is fully DC
coupled. At zero, `triangle(o)` is subtracted locally. Intermediate values
interpolate those two static transfer laws. `set_dc_coupling()` is a convenience
that writes all eight fixed slots; `set_stage_dc_coupling()` changes one slot.
This is not a stateful DC blocker or high-pass filter: it adds no state,
latency, reset behavior, or tail beyond the selected alias FIR.

The raw folder projects finite inputs to its documented normalized transfer
domain `[-64, 64]` before the first stage. That range is far beyond normal audio
level while keeping the symmetry term and trigonometric reduction finite for
every finite caller input.

### Ring laws and carrier modes

At index zero both ring models are exactly dry. At index one the ideal model is
`audio * carrier`. Bipolar mode uses the virtual-analog waveform in `[-1, 1]`;
unipolar mode maps it to `0.5 * (waveform + 1)`. Output inversion is a separate
post-model choice.

The `diode_ring` model uses the balanced static law
`(logcosh(d*(c+x/2)) - logcosh(d*(c-x/2))) / d`. `logcosh` uses an
absolute-value/`log1p` difference identity without forming either large
`logcosh` term, so finite extreme inputs cannot overflow. The raw carrier
argument is projected to its public `[-1, 1]` domain. It is a
bounded-cost, quasi-static diode-ring character model inspired by the parallel
static nonlinearities in Julian Parker's
[simple digital model](https://www.dafx.de/paper-archive/2011/Papers/66_e.pdf).
It does not claim to solve the transformers, diode switching transients, or a
circuit ODE.

An external audio-rate carrier input is not supported by this processor. Pulp's
current shared oversampler has one input; accepting a second host-rate signal
without jointly upsampling it would leave its sum sidebands aliased. A future
external-carrier overload therefore needs a reusable synchronized multi-input
oversampling path rather than a nominal two-argument wrapper.

## Breakpoint transfer curves

`<pulp/signal/transfer_curve.hpp>` provides fixed-capacity breakpoint transfer
functions. Each point uses the shared modulation-language curve vocabulary for
the following segment, so envelopes and transfer functions agree on linear,
exponential, logarithmic, smoothstep, and hold shapes. The processor validates
and publishes complete snapshots but deliberately leaves oversampling and any
antiderivative-antialiasing policy to its caller.
