# Shared-memory spectral sessions

`pulp::gpu_audio::GpuSpectralMaskSession` provides persistent Hann-windowed
FFT/mask/inverse-FFT processing through Dawn's Metal shared-memory provider.
The default API accepts immutable, real, nonnegative gains from DC to Nyquist.
An explicit opt-in also accepts effective gain snapshots per hop.
It is an optional SDK building block, not an audio callback implementation.

All methods must run on one serialized, non-realtime owner. The plugin supplies
its own bounded callback bridge, pipeline lead, and continuously advanced CPU
fallback. Each planar input hop is copied into a shared slot; each completed
output hop is copied out. The provider avoids WebGPU payload uploads and
readbacks. This is not a claim that all CPU copies disappear.

## Processing and recovery

Create the session with FFT size, hop, channels, sample rate, slot count, and
gains. FFT size must be a power of two from 256 through 16384. Hop must divide
the FFT size and be no greater than half of it. Sequence numbers are contiguous
within the session's process-unique epoch. A failed creation may still return a
session owning partially prepared resources; retain it until `release()`
confirms their physical retirement.

1. Call `submit_hop` with one planar hop. A refusal does not advance spectral
   history. Retry that same sequence/input, or drain and recreate the session.
2. Call `service` and `receive` on the serialized owner. A returned result names
   its epoch and source sequence. `delivered` means physical success and an
   output copy, not acceptance by an audio callback. Check `late` and your own
   deadline before accepting it.
3. The first failed result poisons the whole epoch. New submissions are
   refused; later physical completions are still retired but never delivered.
   Persistent FFT history cannot be repaired by accepting a later success.
4. Call `release` with callback users stopped. If it returns false, retain the
   session and retry: physical retirement is still pending. Recreate only after
   successful release to obtain clean history and a new epoch.

Intrinsic latency is `fft_size + hop` samples. Any plugin pipeline lead adds
to that value. This API has no in-place reset and makes no realtime scheduling
promise.

## Evidence

`diagnostics()` separates CPU copy bytes, runtime WebGPU transfer calls,
provider identity, and physical terminal counts. Successful release retains a
final snapshot including work retired during release itself. The release flag
is set only after the arena's physical barrier succeeds. A failed barrier is
not a release receipt.

For authenticated SDK builds, enable `PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF`.
`configured_revision_verified` distinguishes the pinned build from mere
agreement between the Dawn header and procedure table. The hardware probe
`pulp-gpu-shared-spectral-probe` requires the configured provider fixture and
checks output against an independent CPU spectral engine, latency, sequencing,
slot capacity, transfer counters, and release with in-flight work. A passing
functional probe is not a speedup or callback-deadline result.

## Effective gains supplied per hop

Use `create_with_per_hop_gains(config)` to prepare a program that reads a
complete gain snapshot from each imported input slot. Submit with
`submit_hop_with_gains(input, sequence, gains, deadline)`. Gains must contain
`fft_size/2+1` finite nonnegative values. The call copies them before returning;
the caller can reuse its source table immediately. The leased slot remains
immutable until physical retirement and CPU release. Capacity refusal does not
advance sequence or adopt the rejected table.

This is a transport seam for an existing control authority. It neither compiles
layouts nor adopts latest values nor interpolates transitions. Supply the exact
effective table chosen by the CPU authority for the analysis frame generated
by that hop. Initial FFT-fill hops do not generate an analysis frame. Output
contains overlap-add contributions from several previous tables, so an output
sequence is not a claim that every sample uses one current control generation.
The caller must preserve ordered input and control history together.

Ordinary `create` retains the immutable program and refuses
`submit_hop_with_gains`. Ordinary `submit_hop` on the opt-in program uses the
initial configured table for that hop, not the most recently supplied table.
This explicit rule avoids mutable last-value state across retries. Per-hop gain
bytes count toward `cpu_input_bytes`; metadata remains excluded as before.
There are no new runtime WebGPU uploads, copies or readbacks. CPU table copies
remain, and this feature makes no claim of lower callback or total CPU cost.

The spectral probe queues three different tables without receiving, mutates the
source arrays immediately, checks capacity refusal/retry, and compares every
output against the existing CPU spectral engine. It retains immutable-mode,
latency, retirement and failure-history controls. Full plugin automation,
callback-safe table capture and host validation are separate work.
