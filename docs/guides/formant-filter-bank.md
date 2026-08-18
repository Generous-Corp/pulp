# Formant filter bank

`pulp::signal::FormantFilterBankT` is a fixed-capacity audio filter for vowel and
other source-filter colour. It is deliberately separate from formant analysis:
it does not estimate resonances from speech, track a voice, or shift an STFT
spectral envelope. The caller supplies one to five ordered resonant peaks.

```cpp
#include <pulp/signal/formant_filter_bank.hpp>

pulp::signal::FormantFilterBank filter;
filter.prepare(48000.0);
filter.set_transition_samples(128);
filter.configure_vowel_morph(pulp::signal::FormantVowel::a,
                              pulp::signal::FormantVowel::u, 0.35);
filter.process_block(samples, frame_count); // in place
```

Each `FormantSpec` contains frequency, bandwidth, and relative gain. A complete
recipe is rejected unless every value is finite and bounded, frequencies are
strictly increasing, and `Q = frequency / bandwidth` lies in the documented
range. Rejection leaves the requested and processing state unchanged.

Retunes crossfade two complete banks, so coefficients never pass through an
unstable interpolated state. The default fade is 64 samples;
`set_transition_samples(0)` explicitly opts into immediate block-boundary
replacement. The built-in A/E/I/O/U recipes use five peaks and
`configure_vowel_morph()` interpolates their frequencies geometrically. The
filter has zero latency. `tail_seconds()` reports the slowest section's ideal
-60 dB amplitude decay estimate.

The output uses a conservative normalization: linear formant weights sum to the
selected headroom gain (3 dB by default). This bounds the bank's steady-state
frequency response without clipping or adding a nonlinear limiter. One object
contains one recursive path; use one object per channel.
