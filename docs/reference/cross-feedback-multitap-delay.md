# Cross-feedback multitap delay

`<pulp/signal/cross_feedback_multitap_delay.hpp>` provides a wet-only stereo
multitap delay for effects that need reusable time-domain taps rather than a
spectral-delay engine. `CrossFeedbackMultitapDelayT` has eight fixed tap slots;
each active tap exposes delay, signed level, pan, stereo width, and feedback
weight.

## Feedback stability

Each channel's feedback source is a weighted sum of the active tap reads. The
weights are divided by `max(1, sum(abs(weights)))`, so their absolute sum never
exceeds one. The global matrix is

```text
gain * [ 1-cross    cross   ]
       [   cross  1-cross  ]
```

where `cross` is between zero and one and signed `gain` is clamped to ±0.95.
Every matrix row therefore has absolute sum `abs(gain) < 1`. Pulp's existing
linear-interpolating `DelayLineT` supplies convex fractional reads, so it does
not increase the infinity norm. These three bounds make the complete recursive
path stable without a limiter or saturator.

At `cross = 0` echoes remain in their input channel. At `cross = 1` each return
is written to the opposite channel, producing the classic ping-pong sequence.
Intermediate values continuously blend those topologies.

## Tap output placement

Pan is the centre of a tap's stereo image and width is the separation between
its left and right sources. Both sources use equal-power pan gains. The default
`pan = 0, width = 1` preserves the delayed left and right channels; width zero
places both at the tap pan position.

The processor emits wet signal only. Its direct input is not delayed, so it
reports zero algorithmic latency. With feedback disabled, tail length is the
latest audible active tap rounded up to samples. An audible output combined
with an active feedback route reports the infinite-tail sentinel `-1`;
unprepared or fully muted configurations report zero.

## Realtime lifecycle

`prepare(sample_rate, maximum_delay_ms)` transactionally allocates two bounded
delay histories and requires capacity for at least one sample. Tap values may
be configured before preparation and are then bounded against the established
capacity; later tap setters use that capacity directly. All setters are
control-thread operations. After
preparation, `process_sample()`, `process_block()`, and constant-time `reset()`
allocate no memory or take locks. Block processing supports same-channel
in-place buffers and is invariant to block partitioning. Null buffers are
no-ops. A non-finite input or an unrepresentable internal result clears history
and emits silence.
