# Headphone crossfeed

`<pulp/signal/headphone_crossfeed.hpp>` provides `HeadphoneCrossfeedT`, a
fixed-storage stereo processor that reduces the unnatural isolation of hard-panned
material on headphones. It is intentionally a small speaker-like crossfeed block,
not an HRTF renderer or a binaural-localization model.

## Topology

Each input channel follows two feed-forward paths:

1. a zero-delay direct path to the same output channel; and
2. a delayed, one-pole-low-pass path to the opposite output channel.

For crossfeed gain `g`, the output is normalized as
`(direct + g * filtered_opposite) / (1 + g)`. Consequently identical constant
left and right inputs retain unity DC gain after the filter settles. There is no
feedback: the only recursive state is a stable one-pole low-pass whose pole is
`exp(-2*pi*cutoff/sample_rate)`, strictly inside the unit circle.

The default settings are 50% amount, 0.25 ms interaural delay, and a 700 Hz
crossfeed cutoff. Amount maps linearly to a maximum opposite-channel gain of
0.35. Delay is bounded to 0–1 ms and uses linear fractional-sample interpolation.
Cutoff is bounded to 100–20000 Hz and to 45% of the active sample rate.

## Timing and realtime contract

The direct path is not delayed, so `latency_samples()` is zero. Active crossfeed
has an asymptotic one-pole decay and reports the infinite-tail sentinel `-1`;
bypass or zero amount reports zero tail.

Call `prepare()` and setters outside the audio callback. Storage is fixed at
compile time for the maximum supported 768 kHz sample rate, so preparation and
all processing paths allocate no memory. `process_block()` supports in-place
stereo buffers and is identical to repeated `process_sample()` calls. Null
buffers are no-ops. Non-finite audio resets both channel histories and emits
digital silence.

Disabling the processor is an exact sample-for-sample bypass while delay and
filter histories continue to follow the input. Re-enabling therefore resumes a
warm crossfeed path. Call `reset()` when a cold restart is required.
