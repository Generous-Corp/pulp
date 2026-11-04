# Observe one SignalGraph audio edge

`SignalGraph::attach_audio_edge_probe` observes one named ordinary audio edge
on the serial executor. It records source-side peak and RMS before downstream
mixing or delay compensation. A source is measured immediately after execution,
before another node can reuse its scratch. This is a diagnostic observation;
it neither changes audio nor inserts a graph node.

```cpp
auto source = graph.node(gain)->out("audio");
auto destination = graph.node(output)->in("audio");
// Stop processing before prepare, attach, detach, or replacing an attachment.
graph.prepare(48000, 128);
auto status = graph.attach_audio_edge_probe(source, destination);
if (status != pulp::host::SignalGraph::EdgeProbeStatus::Attached)
    return; // Handle the explicit refusal in the application's control thread.
const auto identity = graph.audio_edge_probe_identity();
// Resume processing. One control-thread consumer can drain while audio runs.
pulp::graph::GraphRuntimeProbeSample sample;
while (graph.read_audio_edge_probe(sample)) {
    // sample.sequence, timestamp_samples, edge_index, frames, peak, rms
}
const auto lost_blocks = graph.audio_edge_probe_dropped();
// Stop processing before graph.detach_audio_edge_probe().
```

The ring holds 256 records. Full rings drop the newest record and increment
`audio_edge_probe_dropped()`; processing never waits for the reader. Sequence
numbers count delivered records. Sample timestamps count frames from attachment,
including blocks lost to overflow, and are independent of host transport time.
Variable block sizes advance by their actual frame count. A failed node produces
no record for that block, but its admitted block still advances the probe clock.
Zero-size, oversized and unprepared calls produce no record and do not advance
it. Non-finite source samples remain non-finite in RMS; the probe is not an audio
quality verdict.

`audio_edge_probe_identity()` binds records to this graph's authoring receipt,
connection identity, and endpoint IDs/ports. The record's dense `edge_index` is
local to this attachment and must never be reused to identify a later topology.
Disconnecting and reconnecting the same endpoints gives a new connection identity.
Read identity and records on the same control thread; do not race them with
control-thread topology edits. Reads serialize with graph mutations, while the
producer only uses bounded arithmetic and lock-free ring atomics.

Attach/detach require all graph processing, including retained execution-snapshot
rendering, to be stopped. They allocate/free only on the control thread. A failed
attach leaves the existing attachment untouched. A successful replacement starts
with an empty ring, zero loss count, sequence zero and timestamp zero. Reprepare
or a newly published topology retires the attachment. Ordinary topology mutation
unpublishes the graph, so reads then return empty and status is `Unprepared`.
A transport reset or execution-mode change detaches at the next admitted block;
old queued records remain readable with their original identity. Reattach after
the discontinuity to start a new observation.

Unsupported requests return `UnsupportedKind` for MIDI/event, control/parameter,
feedback and sidechain edges. Parallel, legacy and anticipative execution return
`UnsupportedExecution`. Missing edges, invalid named handles, unprepared graphs
and an active swap edit have separate statuses. Sample-region internal edges
that have no serial-plan edge are unsupported. No measurement work runs without
an attached probe; an unattached serial node checks only a null pointer.

The focused `[edge-probe]` tests use public graph APIs to check source-versus-output
identity, audio equivalence, timestamp/overflow accounting, reset/topology behavior,
unsupported requests, concurrent reading and callback allocation absence. These
are macOS functional and allocation checks, not the roadmap's paced deadline or
cross-platform realtime qualification. CLI/MCP probes, control/event capture,
latency/PDC maps and broad plan introspection remain separate work.

## Observe a saved graph offline

```sh
pulp graph probe graph.pulpgraph --input input.wav \
  --source-node source --source-port audio \
  --destination-node downstream --destination-port audio \
  --block-frames 64 --json
```

Node names must be unique. The command prepares the graph, attaches the named
edge, processes the supplied audio, and drains after each block. It emits JSON
only, including without `--json`. It does not write the graph or audio files.
`code` is `Attached` on success; attachment refusals retain the C++ enum spelling
(`UnknownPort`, `MissingEdge`, `UnsupportedKind`, `UnsupportedExecution`,
`Unprepared`, `Busy`, `Detached`). Other input failures have descriptive codes
such as `invalid_block_frames` or `input_limit`. A refusal exits 2.

Limits are a 1 MiB graph, 128 nodes, 512 connections, 1024 ports, built-in
input/output/gain/MIDI nodes, and an audio input with 1–8 channels, 8000–192000 Hz,
1–480000 frames, and at most 4096 blocks. Block size is 1–4096 frames, default 64.
Plugin/custom nodes are refused. Nonfinite input or measured peak/RMS is refused
before publishing a sample report; JSON never substitutes zero for these values.
The command reports source-side observations, not downstream output quality.

The report includes `records` (`sequence`, `timestamp_samples`, `edge_index`,
`frames`, `peak`, `rms`), `identity`, `dropped`, input `frames` and `sample_rate`.
The identity's graph lifetime, generation and connection counters are decimal
strings to preserve 64-bit precision. Graph lifetime is process-local; do not
use it as a persisted semantic fingerprint or compare it across CLI processes.

The installed MCP `pulp_graph` operation `probe` accepts `path`, `input`,
`source_node`, `source_port`, `destination_node`, `destination_port`, and optional
integer `block_frames`. It returns the same CLI JSON as text and marks refusals
with `isError: true`. It runs its matched sibling CLI with a 30-second deadline
and a 2 MiB output cap, including outside a source checkout.

The `examples/graph-edge-probe` standalone consumer links only installed
`Pulp::host`. It checks a source gain of 0.5 before a downstream gain of 0.25,
unchanged rendered audio, clocks, identity, loss and refusals. The installed
consumer test also invokes actual CLI/MCP binaries against its saved fixture.
This is bounded offline functional proof, not paced realtime qualification,
control/event capture, or a complete latency/PDC map.
