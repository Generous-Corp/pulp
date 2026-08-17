# Reverse buffer

`<pulp/signal/reverse_buffer.hpp>` provides `ReverseBufferT`, a prepared mono
streaming effect that captures fixed-size windows forward and emits the
preceding window backward. Instantiate one processor per channel when channels
must reverse independently.

This is the reusable transport primitive, not a looper, sampler voice, beat
repeat, tempo wrapper, feedback delay, or reverse-reverb recipe. It deliberately
owns no tempo division, triggering, pitch, overdub, feedback, diffusion, or
wet/dry policy. Character Delay's internal segmenter remains specialized for
continuously slewed window lengths and modulated fractional reads.

## State and timing

The deterministic state machine has four observable states:

- `unprepared`: non-bypassed processing emits silence.
- `bypassed`: finite input passes through sample-exactly and is not captured.
- `priming`: one complete input window is captured while silence is emitted.
- `running`: one buffer captures forward while the preceding buffer plays
  backward.

For a four-sample raw window, input `1 2 3 4 5 6 7 8` produces
`0 0 0 0 4 3 2 1`. `latency_samples()` and
`startup_latency_samples()` therefore report the configured window length.
This is a reorder effect rather than a pure delay: individual sample delays
range from one through `2N-1`. `tail_samples()` reports that conservative finite
`2N-1` bound. Hard bypass is direct and therefore makes `latency_samples()` and
the tail zero; `startup_latency_samples()` retains the configured `N` so callers
can inspect the buffering requirement before re-engaging.

`set_bypassed()` is a hard transport transition. Bypass outputs finite input
exactly and captures nothing; entering or leaving it discards both logical
windows, so re-engagement always primes from fresh input instead of replaying a
stale fragment. `reset()` and `discard_history()` preserve capacity,
configuration, and bypass state while returning the transport to priming.
Call `discard_history()` after a host seek or discontinuous source reposition;
the streaming primitive does not provide random-access seeking.

## Boundary and lifecycle contract

`Config::boundary_fade_samples = 0` preserves the exact rectangular reversed
sequence. A value of two or greater applies a raised-cosine fade to zero at
both ends of every playback window, eliminating the full-scale splice between
unrelated adjacent windows. The fade must fit in half the window. This explicit
choice avoids silently changing exact sample values for callers that already
own a transition mixer.

`prepare(maximum_window_samples)` is the sole allocating operation and replaces
both buffers transactionally. `configure(Config)` validates and publishes the
complete fixed window/fade configuration transactionally; an accepted change
discards history, while rejection preserves live playback. After preparation,
sample/block processing, exact same-buffer in-place processing, bypass changes,
reset, and discard allocate no memory and take no locks. A non-finite input
emits silence and discards history so invalid audio cannot persist.

```cpp
pulp::signal::ReverseBuffer reverse;
reverse.configure({.window_samples = 24000, .boundary_fade_samples = 64});
reverse.prepare(48000); // prepared capacity; configuration may later use <= it
reverse.process_block(input, output, frames);
```
