# Inspecting a graph-owned custom node

Custom-node diagnostics inspect the actual instance owned by a prepared
`SignalGraph`. Type enumeration, saved state and worker completions do not
establish which GPU output an audio callback selected.

Register a `CustomNodeDiagnosticsDescriptor` after its exact custom type/version.
This companion leaves the positional `CustomNodeType` aggregate unchanged.
Replacing a custom type withdraws its diagnostic companion; register the matching
inspector again. Prepared-topology edits carry, remove and publish companions
alongside their type registrations.

For example, after registering and preparing the GPU realization:

```cpp
auto identity = graph.custom_node_diagnostic_handle(node_id);
pulp::host::space::convolution::GpuConvolutionDiagnostics report;
auto result = graph.query_custom_node_diagnostics(
    identity.handle, pulp::host::space::convolution::kGpuDiagnosticSchema,
    report, pulp::host::CustomNodeDiagnosticConsistency::LiveApproximate);
if (result.availability == pulp::host::CustomNodeDiagnosticAvailability::Available) {
    // report.report.lanes[0].delivery.gpu_blocks is selected output, not completion.
}
```

Retry `Busy` later on the control thread. After reprepare, obtain a new handle;
never automatically relabel a stale result as belonging to the new generation.
To reconcile delivery totals, first stop and join the sole process caller, then
query with `AudioCallerStopped` before release. This ordering is the caller's
responsibility, not an operation performed by the query.

On a control thread, obtain `custom_node_diagnostic_handle(node_id)`, then call
`query_custom_node_diagnostics(handle, schema, report)`. The report must be an
owned trivially-copyable value of the schema's exact size, at most 4096 bytes.
The callback receives bytes and copies with `memcpy`, so caller buffer alignment
does not leak into provider casts. Reports contain no borrowed instance pointers,
strings or callback closures. Treat the result availability as authoritative;
non-available reads clear in-bound output buffers. Oversized buffers are rejected
without traversing caller memory.

`Busy` means lifecycle mutation holds the graph lock or a swap edit is open.
`StaleHandle` means graph identity/generation changed or the authored node does
not match the live instance. Prepare, release and topology changes invalidate
old handles. `MissingNode`, `Unsupported`, `NotPrepared` and `SchemaMismatch`
remain distinct from an available report with zero GPU deliveries.

The query tries the existing graph mutation lock, retaining its debug ownership
checks. It copies `Slot::live()`'s control-thread shared owner under that lock;
it never acquires an RCU reader pin while holding the mutation lock. This matches
existing prepared-topology ownership and cannot invert release's reader drain.
Prepare, replacement, removal and release cannot change the inspected instance
until the callback returns. Graph destruction must still be serialized by its
caller, as with every other graph method. No diagnostic work is added to the
audio callback. Generic type-key lookup may allocate on this control-thread
path; the query is not RT-safe. A user-supplied inspector must not allocate, wait, reenter graph
methods or mutate lifecycle; a try-lock is not a wall-time guarantee for arbitrary
provider code.

Live reports are approximate independent atomic loads. `AudioCallerStopped`
records the caller's explicit promise that the sole audio process caller is
stopped. It does not stop processing itself, stop workers, certify physical
retirement or prove unique terminal outcomes per block. Read stopped delivery
totals before release resets the state; prove drain separately. Worker stats may
still advance even with the audio caller stopped.

## Forge GPU convolution

`space::convolution::gpu_convolution_diagnostics()` supplies the companion for
`space.convolution_reverb_gpu` version 1. Its fixed schema
`kGpuDiagnosticSchema` returns `GpuConvolutionDiagnostics`, including the actual
engine's preparation, authenticated provider capability, block size, latency and
two independent lanes of transport stats and selected-delivery counters.
Preparation generation is returned in the query envelope. Counts represent
internal transport quanta, not arbitrary host callback partitions. Generic worker
output is never relabeled as GPU output.

A live parameter wrapper must register its diagnostic companion under its exact
wrapped type/version and unwrap only its privately owned instance before invoking
the original inspector. Preserve the original producer identity in the companion's
`producer_type_id`/`producer_type_version`; the envelope separately preserves the
registered alias. Never cast a wrapper instance directly to `GpuInstance`.

CPU-only/default types have no GPU inspector and report `Unsupported`. The C++
seam does not yet add a unified-controls operation. That follow-on should expose
the same closed statuses and fixed typed payload after installed product proof.
Focused fake tests and source compilation do not establish actual plugin GPU
delivery, numerical correctness, physical teardown or a performance improvement.
