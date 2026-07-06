# RT-safe retirement patterns

Live-swap and hot-reload all share one hard rule: **a resource the audio thread
was using (a `Processor`, an IR convolution state, a compiled graph snapshot, a
GPU/device handle) must never be *freed* on the audio thread.** Freeing there can
block (allocator lock), run an arbitrary destructor, or trip a use-after-free if
a callback is still inside the old object. Pulp uses four RT-safe retirement
patterns; pick by how the audio thread hands the old resource off. (Live-swap
plan item 2.3.)

| Pattern | Where | How the audio thread hands off | Frees on |
|---|---|---|---|
| **Quiescence-lock** | `ProcessorHotSwapSlot` | Control thread takes the writer lock, which *proves* no audio reader is inside `process()`; the displaced instance is returned/retained and dropped after unlock. | control thread |
| **Retire-ring** | `ConvolverIrSwapper` (SPSC ring) | Audio thread `try_push`es the displaced state into a bounded ring (never blocks/frees); a worker drains + frees. | worker thread (`drain_old`) |
| **RCU-drain** | `SignalGraph` live snapshot | Audio thread reads an atomically-published snapshot; edits publish a new one and retire the old after a grace period when no reader holds it. | control thread |
| **Generation-ack** | `SampleSlotBank` | Publisher bumps a generation; the audio thread acks the generation it has adopted; the publisher frees the prior generation only once acked. | publisher thread |

## Invariants every pattern must hold

1. **No alloc / lock / free on the audio thread** during the swap or steady
   state. Verified with `RtAllocationProbe` (no-alloc) + the sanitizer (TSan)
   build (no race / UAF). The default-off crossfade paths keep the steady state
   allocation-free.
2. **Bounded hand-off.** The audio thread's step (push to a ring, publish an
   atomic, mark a flag) is O(1) and wait-free. Any unbounded work (building the
   next state, freeing the old) happens off-thread.
3. **Capacity is reserved BEFORE the swap commits.** A hand-off that could fail
   inline (a full retire ring) must be gated up front so the audio thread never
   has to free as a fallback. **Count the hand-offs:** a step that retires *two*
   resources at once (e.g. a completed crossfade fade-out **plus** the just-
   displaced state) must reserve **two** slots, not one — a single-slot check
   there leaks (the second retire fails and the pointer is orphaned). This bit
   the convolver crossfade in review (item 2.1b) and is the subtlest failure
   mode of the retire-ring pattern.

## Crossfade vs instant retire

The hot-swap slot and the convolver both offer an **opt-in crossfade** (via the
shared `signal::TransitionMixer`, item 2.1): the displaced resource keeps
rendering in parallel from its own history for a short window, blended old→new,
then retires via its pattern above once the fade completes. The default remains
an **instant** swap (retire immediately), so the steady state is unchanged for
callers who don't opt in.
