# Observe one SignalGraph edge

`SignalGraph::attach_audio_edge_probe` observes one named audio edge on the
serial executor. Ordinary and sidechain edges record source-side peak and RMS
before downstream mixing or delay compensation; a feedback edge records the
retained input before the current capture, timestamped at the receiving block.
`attach_control_edge_probe` and `attach_event_edge_probe` observe automation
and MIDI edges in the same way (see below). A source is measured immediately after execution,
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

Attach/detach require quiesced processing: `process()` and retained
execution-snapshot rendering must be stopped first. Live attach or detach while
an audio callback is in flight is unsupported. The graph does not enforce this:
attach/detach replace the probe on the published snapshot that the callback
reads without the mutation lock, so the caller owns the quiesce, exactly as for
`prepare()`. They allocate/free only on the control thread. A failed
attach leaves the existing attachment untouched. A successful replacement starts
with an empty ring, zero loss count, sequence zero and timestamp zero. Reprepare
or a newly published topology retires the attachment. Ordinary topology mutation
unpublishes the graph, so reads then return empty and status is `Unprepared`.
A transport reset or execution-mode change detaches at the next admitted block;
old queued records remain readable with their original identity. Reattach after
the discontinuity to start a new observation.

An audio probe returns `UnsupportedKind` for MIDI/event and control/parameter
edges; feedback and sidechain audio edges are observed as described above.
Parallel, legacy and anticipative execution return `UnsupportedExecution` for
every probe kind. Missing edges, invalid named handles, unprepared graphs
and an active swap edit have separate statuses. Sample-region internal edges
that have no serial-plan edge are unsupported. No measurement work runs without
an attached probe; an unattached serial node checks only a null pointer.

The focused `[edge-probe]` tests use public graph APIs to check source-versus-output
identity, audio equivalence, timestamp/overflow accounting, reset/topology behavior,
unsupported requests, concurrent reading and callback allocation absence. These
are macOS functional and allocation checks, not the roadmap's paced deadline or
cross-platform realtime qualification.

## Control and event edges

`attach_control_edge_probe(source, destination_node, parameter_id)` observes one
prepared sparse or dense automation edge. The destination is a node id plus a
public parameter id, because automation edges have no destination audio port.
Each record holds the edge's per-edge mapped and slewed contribution at block
start (`value`) and end (`value_end`), after per-edge PDC for dense edges and
before destination mixing and bounds clamping; `payload_id` is the parameter id.

`attach_event_edge_probe(source, destination)` observes one MIDI edge after the
source publishes and before the destination consumes. Records carry the event
count, the first event's status and offset, and the source buffer's drop count
(`payload_count`, `payload_status`, `payload_offset`, `payload_dropped`).

Both use the same 256-record ring, identity, reset/reattach and quiesce rules as
audio probes, and both refuse parallel routing with `UnsupportedExecution`.

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
`Unprepared`, `Busy`, `Detached`), and each refusal's `message` names the cause
and the change that clears it (for example, `MissingEdge` points at the graph
file's connections array, and `UnsupportedKind` names the matching
`--edge-kind`). Other input failures have descriptive codes such as
`invalid_block_frames` or `input_limit`. A refusal exits 2.

`--edge-kind audio|control|event` selects the probe (default `audio`):

- `audio` requires `--destination-port` and reports
  `observation: source_before_mixing_and_pdc`.
- `event` requires `--destination-port` on a MIDI connection. Before the first
  block the command seeds the source MIDI input with one fixed note-on (note 60,
  velocity 100, offset 0), so the first record has `payload_count` 1 and later
  blocks report 0. It reports `observation: events_before_destination_consumer`.
- `control` requires `--destination-param ID` (a non-negative integer parameter
  id) instead of `--destination-port`. The CLI and MCP accept this schema, but
  graphs loaded by the offline probe admit only built-in input, output, gain and
  MIDI nodes and so cannot contain an automation edge: a control request always
  refuses with `MissingEdge`. Control probes are available only through the C++
  API today.

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
`source_node`, `source_port`, `destination_node`, `destination_port`, optional
`edge_kind` and integer `destination_param`, and optional integer `block_frames`. It returns the same CLI JSON as text and marks refusals
with `isError: true`. It runs its matched sibling CLI with a 30-second deadline
and a 2 MiB output cap, including outside a source checkout.

The `examples/graph-edge-probe` standalone consumer links only installed
`Pulp::host`. It checks a source gain of 0.5 before a downstream gain of 0.25,
unchanged rendered audio, clocks, identity, loss and refusals. The installed
consumer test also invokes actual CLI/MCP binaries against its saved fixture.
This is bounded offline functional proof, not paced realtime qualification.

The `examples/graph-control-event-probe` consumer drives the public executor
directly and checks control and event publication. Its two planted runs
(`--planted-missing-publication`, `--planted-missing-event-publication`) skip
processing and must stop with the exact `rule=control-publication` or
`rule=event-publication` refusal message. They are refusal-message controls:
they prove the consumer reports a missing publication by name, not that a CI
mutation of the executor is caught.

### Optional introspection sections

The same read-only `probe` command can include deterministic control-thread
introspection sections:

```sh
pulp graph probe graph.pulpgraph --input input.wav \
  --source-node source --source-port audio \
  --destination-node downstream --destination-port audio \
  --plan --latency --node-loads --json
```

`--plan` adds the authored node and connection view, `--latency` adds the
prepared total, per-node input/output latency, and per-connection PDC delays,
and `--node-loads` opts into per-node callback timing and reports the bounded
latest-value snapshots. The timing option is intentionally opt-in: an
unattached or ordinary graph does not read per-node clocks, preserving the
zero-cost audio path. The installed MCP `pulp_graph` operation mirrors these
three booleans and invokes the matching sibling CLI, so its JSON sections and
refusals stay identical to the CLI surface.

Requests that cannot be satisfied fail closed with a typed refusal rather than
being represented as audio measurements or fabricated zero values.
