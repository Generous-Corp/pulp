# GPU hints — `gpu` spans (Dawn / Graphite)

Domain knowledge for investigating GPU-side cost in a Pulp trace. Pulp renders
through Skia Graphite on a Dawn (WebGPU) backend; `gpu` spans cover the record
→ submit → present tail of the frame pipeline.

## CPU submit time vs real GPU time — the distinction that matters

Most plugin frameworks only see **CPU submit time** — how long the CPU spent
building and handing off the command buffer. Pulp has **real per-pass GPU-side
timing** via `core/render/include/pulp/render/gpu_timestamps.hpp` (GPU
timestamp queries, resolved and decoded — `decode_resolved_ticks`), wired into
the `gpu` spans as resolved durations. So a `gpu` span can carry the actual GPU
milliseconds for a pass, not just the CPU-side submit cost.

This changes the diagnosis: a cheap CPU submit followed by an expensive GPU pass
is a *GPU-bound* frame (the shader/fill/blend work is the cost); an expensive
CPU submit is a *record/encode* cost (too many draw calls, state churn). Read
both before concluding.

## Submit / present stalls (wall-time-vs-CPU-time in the GPU domain)

A `frame` span dominated by a present/submit child that was **waiting** is a
pacing/back-pressure stall, not compute. The CPU blocked waiting for the GPU to
drain a previous frame or for the swapchain to release an image. Apply the
harness rule: check whether the span was running or blocked. A blocked present
is fixed by reducing GPU work per frame or adjusting frame pacing — not by
optimizing the code inside the span.

```sql
-- Per-pass GPU cost ranking:
SELECT name, COUNT(*) AS n, SUM(dur)/1e6 AS total_ms, MAX(dur)/1e6 AS max_ms
FROM slice WHERE category='gpu' AND dur>=0
GROUP BY name ORDER BY total_ms DESC;
```

## Graphite record cost

The Skia/Graphite record step builds the draw list before submit. Fat record
cost with cheap GPU passes means the CPU is doing too much per frame — often
excessive draw calls, or re-recording content that could be cached. Correlate
with `canvas` dirty-rect churn (see `hints_frame.md`): re-recording a large
dirty area every frame inflates both.

## Startup: device + context init is a one-time `gpu` cost

The single largest startup cost is frequently Dawn device + Graphite context
init (~600 ms in the flagship worked example), which appears as a `gpu`/`render`
span pair on the main thread before the first frame. It is **one-time per
process** but is re-paid **per editor open** if not warmed — the classic
"cache it once per process" fix. Check whether a second open repeats the same
`gpu` init span identically.

## Traps

- **GPU-host gotcha.** `gpu` spans require the GPU window host. If the binary
  was built without Skia (`PULP_HAS_SKIA=OFF`), it falls back to a CPU-only host
  and the render path is different — a trace from such a build will not carry
  the real GPU-pass timing. Confirm the capture came from a GPU build before
  reasoning about GPU ms. (See the `skia-gpu-build` skill.)
- **GPU ms lags CPU submit in wall-clock.** GPU work completes after the CPU
  submitted it; align passes to the frame by the timestamps, not by naive
  ts-ordering against CPU spans.
- **Don't attribute a present stall to the present code.** The stall is usually
  upstream GPU work or swapchain pacing; follow the blocker.
