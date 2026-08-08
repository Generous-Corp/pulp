# Reusable analysis front ends

Pulp provides fixed-capacity signal-layer conditioning for spectrum displays and
streaming oscilloscope capture:

```cpp
#include <pulp/signal/spectrum_trace.hpp>
#include <pulp/signal/scope_capture.hpp>
```

`SpectrumTraceT` consumes a one-sided FFT magnitude frame from DC through
Nyquist with exactly `fft_size / 2 + 1` amplitude-dBFS bins. It does not perform
an FFT or hide its window, hop, or upstream latency. It aggregates configured
linear or logarithmic bands in linear power, applies optional A or C weighting
as an additive dB correction, and supplies per-frame attack/release smoothing
plus peak hold and decay. A band with no FFT center inside its authored edges
stays at the configured floor; it never borrows a nearby bin. DC and Nyquist
participate only when their centers lie inside those edges, and A/C-weighted DC
maps exactly to the configured floor. The default 0.5 attack and 0.15 release
coefficients match the existing EQ spectrum overlay.

`ScopeCaptureT` accepts streaming samples in arbitrary block partitions. It
supports immediate, rising-edge, falling-edge, and either-edge level triggers,
hysteresis, exact pretrigger history, one-shot or continuous capture, and an
exact sample-count holdoff. Its reported completion latency is the number of
post-trigger samples required before the capture is ready; it is not processor
latency and must not be reported to a host for compensation. In continuous
mode, the most recently completed frame remains ready and unchanged while the
next capture is in progress.

Both templates support `float` and `double`, reject invalid configuration
transactionally, sanitize non-finite input to a finite floor or zero, and use
only fixed arrays during processing. They allocate no memory, acquire no locks,
and perform bounded work on the audio thread. Publication to a UI remains a
separate caller-owned concern, normally through Pulp's latest-value handoff.
The returned frame references are same-thread views, not concurrent lock-free
publication objects. Spectrum sample rates are explicitly bounded from 1 Hz
through 384 kHz.

The existing `pulp::audio::OnsetDetector` remains a background/offline analyzer:
it allocates novelty, FFT workspace, magnitude, and marker vectors per request.
The built-in key/tempo fallback's chroma calculation also remains internal and
offline; it is a fixed-resolution heuristic without a standalone streaming
hop/latency contract. Neither is advertised as a realtime analysis source.
