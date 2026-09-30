# Observing the effective spectral mask

`SpectralMaskProcessorT::set_effective_frame_observer(context, observer)` exposes
the exact gain table applied by the existing CPU mask processor. Install or
remove it while processing is stopped. Preparation must have succeeded first;
a successful reprepare clears the observer and requires installation again.
Failed preparation preserves the prior state and observer.

The callback signature is:

```cpp
void observe(void* context, const SpectralMaskTable& table,
             std::uint64_t frame_ordinal) noexcept;
```

For the double-precision processor, use its corresponding `Table` alias. The
callback runs on the audio owner once for each successfully processed coherent
channel-group frame, after publication adoption, audio-owner layout override,
transition interpolation and mask application. Stereo produces one notification,
not two. It also applies to direct `process_frame()` callers.

The table reference is borrowed only until the callback returns. Copy required
gains into bounded, prepared storage if another thread will consume them. The
observer must not allocate, wait, throw, retain the reference or reenter the
processor. A full capture queue is the consumer's responsibility to report;
this observer does not block processing or silently retry publication.

The ordinal counts successful frames since prepare or reset, including frames
processed while no observer was installed. Invalid/failed frames do not advance
it. Reset retains the observer, settles the existing mask transition, and
restarts the ordinal at zero. Ordinal exhaustion refuses further frame processing
until reset/reprepare rather than wrapping. The ordinal is not a stream epoch,
host block ID, sample timestamp, or GPU completion identity. The integration
owner supplies that context, particularly for direct frame processing where the
processor cannot infer source chronology.

This seam lets a downstream execution engine reuse the CPU authority's evaluated
gains without a second layout compiler or transition state machine. It does not
submit GPU work, implement a worker queue, or guarantee realtime GPU scheduling.
For WOLA processing, a delivered output can contain contributions from several
observed tables. Preserve the ordered control history, not just the newest table.

Tests compare interrupted transition values to the actually multiplied samples,
check control publication followed by audio-owner override, verify coherent
multi-channel notifications and reset/reprepare semantics, and compare observed
and unobserved streaming output under different host partitions. A planted
observer-before-interpolation fault must fail the sample/table checks. The
allocation guard counts ordinary C++ new/new[]; it does not interpose arbitrary
C allocations or prove a bound on callback execution time.

This is internal observability behind the existing spectral-mask capability.
Its musical parameters and Forge exposure remain owned by the existing
spectral-mask node/catalog; it introduces no new automatable control or node.
