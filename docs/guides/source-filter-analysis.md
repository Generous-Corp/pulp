# Source-filter analysis

`<pulp/signal/source_filter_analysis.hpp>` provides two independent analysis
foundations:

- real-cepstrum spectral-envelope smoothing with optional iterative
  true-envelope refinement; and
- autocorrelation linear prediction with reflection coefficients, prediction
  error, an explicit stability test, and an all-pole magnitude response.

These are analysis primitives, not a pitch corrector, formant tracker, runtime
filter, or complete voice model.

## Ownership and execution

`CepstralEnvelopeAnalyzerT` and `LpcAnalyzerT` are mutable, single-owner objects.
`prepare()` allocates their bounded workspaces and belongs on a control or
offline thread. Analysis calls allocate nothing, retain none of their borrowed
spans, and must not run concurrently on the same analyzer. Caller-provided
input and output spans must not overlap unless an API explicitly permits it.
When C++ exceptions are enabled, both typed preparation APIs translate
`std::bad_alloc` and `std::length_error` to `AllocationFailure`; invalid geometry or allocation
failure leaves any previously prepared analyzer and published LPC result
unchanged.

The cepstral analyzer retains one FFT, `fft_size` complex cepstrum values, and
one `fft_size / 2 + 1` scalar work array. The LPC analyzer retains
`4 * order + 1` scalar values. `checked_retained_bytes()` exposes both storage
budgets before preparation.

Both analyzers have `float` and `double` aliases. FFT arithmetic follows the
selected scalar type. Autocorrelation sums, Levinson-Durbin numerators, LPC
error rescaling, and all-pole evaluation use `long double` intermediates and
publish results in the selected scalar type.

## Cepstral envelopes

`CepstralEnvelopeAnalyzerT::estimate()` accepts exactly `fft_size / 2 + 1`
finite **log-magnitude** bins from DC through Nyquist. It constructs the even
log spectrum, transforms it to the real cepstrum, keeps quefrencies
`|q| <= order`, and transforms back. Each true-envelope pass smooths the
pointwise maximum of the original spectrum and the current envelope.

The geometry is explicit:

- `fft_size` is a power of two from 256 through 1,048,576;
- `order` is from zero through `fft_size / 2`;
- true-envelope iterations are from zero through 1,024; and
- `convergence_tolerance` is a finite, nonnegative log-magnitude residual.

A zero tolerance always executes the configured number of refinement passes.
A positive tolerance stops after a pass when no positive
`log_magnitude - log_envelope` residual exceeds the tolerance.

Before either transform runs, a conservative range proof bounds FFT butterfly
growth from the FFT size, scalar maximum, and complete iteration budget. Finite
input outside that bound returns `NumericalOverflow`; no hidden normalization
changes accepted spectra. All rejectable conditions are checked before the
transform starts, so the caller's output span remains unchanged on rejection.

The analyzer does not invent a linear-domain floor. The caller owns conversion
to log magnitude and its noise-floor policy. `SpectralEnvelopeShifterT` retains
its established group-RMS conversion: power is floored at the larger of
`frame_peak_power * 1e-4` and `1e-24`, then converted with `0.5 * log(power)`.
Its extracted analyzer uses zero convergence tolerance, preserving the fixed
legacy iteration path.

## Linear prediction

`scaled_autocorrelation()` computes a biased autocorrelation sequence. It first
divides the input by its maximum absolute sample, accumulates each lag with the
same sample-count divisor, and returns that scale separately. This avoids
squaring extreme source samples during correlation and preserves a
positive-semidefinite Toeplitz sequence.

`LpcAnalyzerT` solves the predictor convention

```text
A(z) = 1 + a[0] z^-1 + ... + a[order - 1] z^-order
```

with Levinson-Durbin recursion. It publishes predictor coefficients, one
reflection coefficient per recursion stage, normalized prediction-error power,
the input scale, and prediction-error power in the original sample-squared
units. Rescaling the error is checked separately; if it cannot be represented
as a finite value in the selected scalar type, analysis returns
`PredictionErrorOverflow` and publishes no partial model.

Orders are from 1 through 256 and input must contain more samples than the
order. The default rank and stability margin is 64 scalar epsilons. A zero or
nonfinite input, nonpositive residual, residual below the relative rank floor,
or reflection coefficient at the unit-circle margin fails closed. A failed
analysis clears the coefficient and reflection views.

No window, pre-emphasis, DC removal, or stationarity decision is implicit.
Callers choose those policies before analysis.

## All-pole response and stability

`lpc_stability()` performs an explicit Schur step-down recursion; finite
coefficients alone are not treated as proof of stability.
`all_pole_magnitude_response()` runs that check before touching its output, then
evaluates `gain / |A(e^jw)|` at `fft_size / 2 + 1` bins. It requires:

- the same FFT geometry bounds as the cepstral analyzer;
- a finite positive sample rate;
- a finite nonnegative numerator gain;
- caller workspace exactly equal to the LPC order; and
- finite denominators and response values above the configured denominator
  floor and within the selected scalar range.

The helper is allocation-free and costs `O(order * fft_size)` work. It describes
an analysis curve; it does not instantiate or process an all-pole filter.

## Unsupported formant extraction

This API does not turn local peaks in an all-pole response into formant
candidates. Peak picking alone does not robustly establish pole frequency,
bandwidth, pole pairing, or continuity across frames and must not be presented
as state-of-the-art formant tracking.

A future formant layer needs, at minimum, a numerically stable polynomial root
solver, conjugate-pole pairing, explicit sample-rate/order/frequency/bandwidth
rejection rules, and validation against both controlled vocal-tract syntheses
and a representative labeled speech corpus. Until those prerequisites exist,
formant frequencies and bandwidths are explicitly unsupported.
