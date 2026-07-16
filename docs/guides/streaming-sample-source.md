# Streaming sample source

`pulp::audio::StreamingSampleSource` plays a sequential sample through a
resident preload head and an SPSC tail ring. Its audio-thread `pull()` call does
not allocate, lock, wait, read a file, or invoke a decoder. One owned, joinable
background thread fills the tail; deterministic tests can disable the thread
and call `pump_background()` explicitly.

This is a narrow transport primitive, not Pulp's complete polyphonic sampler
backend. It supports sequential one-shot playback and fully resident loops.
Streamed loops, fractional/pitched reads, crossfade prefetch, multi-voice
scheduling, a global memory governor, and starvation fades require the shared
page-cache sampler service described in the sampler hardening plan.

## File-backed playback

`make_memory_mapped_frame_reader()` adapts a file to the source's `FrameReader`
contract. The returned object retains its mapped reader for the callback's
lifetime. WAV and uncompressed AIFF/AIFF-C `NONE` files use bounded ranged
decode from mapped bytes; they are not decoded completely before playback.

The adapter reports `supports_ranged_read`. A false value means the active file
format uses `MemoryMappedAudioReader`'s decode-once fallback. Applications with
a strict streaming-memory contract should reject that source or import it into
a streamable representation before note-on.

```cpp
auto file = pulp::audio::make_memory_mapped_frame_reader(path);
if (!file.valid || !file.supports_ranged_read)
    return false;

pulp::audio::StreamingSampleSource source;
pulp::audio::StreamingSampleSourceConfig config{
    .channels = file.channels,
    .total_frames = file.total_frames,
    .sample_rate = file.sample_rate,
    .preload_frames = 8192,
    .ring_capacity_frames = 32768,
    .read_chunk_frames = 4096,
};
return source.prepare(config, std::move(file.reader));
```

`prepare()` synchronously fills the preload and primes the ring, so it belongs
on a control or loading thread. `release()` stops and joins the worker before
freeing storage. Destruction calls `release()`.

## Underruns

`pull()` never blocks for missing tail data. It writes the available frames,
zero-fills the remaining requested output, and increments `underrun_frames`.
The source position advances only for frames actually delivered, so recovered
data stays in source order. This preserves source material but stretches wall
clock time during a miss; production sampler recovery must choose and document
its own timeline policy.

Use `stats()` for telemetry. A no-underrun render can be compared exactly with
resident playback. Once a deliberate underrun occurs, reproducibility requires
the same injected I/O schedule and recovery policy.

## Thread contract

- `prepare()`, `reset()`, and `release()`: control/quiescent thread.
- `pull()` and `finished()`: audio thread after prepare.
- `pump_background()`: the owned reader thread, or one deterministic test
  driver when `start_background_thread` is false.
- `FrameReader`: one background caller at a time; it may seek, decode, allocate,
  and lock, but it must not retain the destination view after returning.

Do not share one `StreamingSampleSource` between concurrent audio callbacks.
