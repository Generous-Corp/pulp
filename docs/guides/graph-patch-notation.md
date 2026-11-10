# SignalGraph patch notation

The bounded SignalGraph profile of patch-language v1 builds the existing
`SignalGraph` through named typed mutations. It does not execute DSP itself.
This implements part of G2; graph-file live reload and the unsupported contracts
below remain required successors.

## Discover and author

Use `pulp graph catalog --json` to discover the default runtime constructors.
An application registers its existing `ProcessorRestoreDescriptor` or
`CustomNodeType` once in `pulp::host::GraphPatchCatalog`, then asks `to_json()`
for actual named ports and Processor parameter ranges. This projection calls
trusted factories on the control thread. Factories must return fresh instances;
finish registration before use and do not access a catalog concurrently.
The default CLI/MCP catalog contains audio input/output, gain and MIDI boundary
constructors. Application Processor factories belong to the application's C++
catalog; the tools do not load arbitrary plugin binaries or code from notation.

The installed consumer uses the following ten-line patch. Its Processor's
existing descriptor declares MIDI capabilities and its StateStore declares
`level`; neither list is copied into a notation vocabulary.

```text
# flow 1
input : pulp/audio_in
midi : pulp/midi_in
voice : "com.example.PatchGain"
out : pulp/audio_out
midi.audio >> voice.midi
input.audio >> voice.audio
voice.audio >> out.audio
input.audio ~> voice.level @ 0.5
voice.level = 0.75
```

The same graph uses these public C++ authoring calls after the example's
Processor registration (14 lines):

```cpp
GraphPatchCatalog catalog;
register_patch_gain(catalog);
auto graph = catalog.make_graph();
auto input = graph->add_input_node(2, "input");
auto midi = graph->add_midi_input_node("midi");
auto voice = catalog.instantiate(*graph, "com.example.PatchGain", 1, "voice");
auto out = graph->add_output_node(2, "out");
graph->connect(graph->node(midi)->out("audio"), graph->node(voice)->in("midi"));
graph->connect(graph->node(input)->out("audio"), graph->node(voice)->in("audio"));
graph->connect(graph->node(voice)->out("audio"), graph->node(out)->in("audio"));
auto instance = graph->processor_node_instance(voice);
const auto& level = instance->parameter_catalog().front();
graph->connect_automation(input, 0, voice, level.id, level.range.min, 1.0f);
instance->state_store().set_value(level.id, 0.75f);
```

Check each typed mutation result in application error handling. The consumer
uses the parser's shared diagnostics for the same mutations.

The MIDI boundary's existing named port is `audio` with kind `midi`.
Registered Processors expose `midi` alongside their existing audio channel
ports. Audio bus counts and port indexes retain their existing meaning.

Declarations use `name : Plugin/Module`; quote exact dotted type IDs and append
`@ integer` for a non-default type version. Node names are unique ASCII
identifiers. Ports and parameters accept single or JSON double quotes.
`>>` connects named ports, chains infer an intermediate output only when exactly
one exists, and the final hop may fan out with commas. Numeric parameter values
must be finite and exactly representable by their catalog range and step.
`#` starts a comment outside quotes when preceded by whitespace.

`source.port ~> destination.parameter @ depth` uses the existing sparse
control-rate automation API, with an unsmoothed Replace route. A source in
[0,1] maps to `[parameter.min, parameter.min + depth * span]`; depth is in [0,1].
This is a range mapping, not an additive modulation promise. Runtime clamping
and event consumption remain the existing Processor adapter's responsibility.

## Public API and tools

Include `<pulp/host/graph_patch.hpp>` and link installed `Pulp::host`.
`parse_graph_patch(text, catalog)` returns an isolated candidate graph or a
`GraphPatchDiagnostic`; `format_graph_patch(graph, catalog)` emits canonical
text. `load_graph_patch_json` and `load_graph_patch_file` provide strict saved
graph validation. Failed calls return no graph, leaving the caller's live graph
untouched. Factory side effects remain the application's responsibility.

```sh
pulp graph validate patch.pat --json
pulp graph fmt patch.pat
pulp graph save patch.pat --output patch.pulpgraph
pulp graph fmt patch.pulpgraph --notation
```

MCP `pulp_graph` supports `validate`, `fmt` and `catalog` with the same CLI
implementation; `catalog` needs no path. `fmt` is read-only. Diagnostics share
`valid`, `code`, `line`, `node`, `port`, `rule`, `suggested_fix` and `message`
across the public API and tools. Wrong port, wrong kind, instantaneous cycle,
duplicate, invalid number/range and unsupported state failures are explicit.
JSON document diagnostics have line zero; notation diagnostics identify the line.
CLI success also reports graph counts.

The strict saved-graph path restores nodes using `GraphSerializer` and
`ProcessorRestoreRegistry`, then validates and rebuilds every original edge.
It refuses dangling/index/kind/cycle/duplicate edges instead of accepting the
legacy loader's intentional dropped-edge behavior. `GraphSerializer` itself
remains tolerant for compatibility. Existing non-validation `.pulpgraph` CLI
operations retain that legacy loading behavior; notation inputs and notation
formatting use strict validation.

## Bounds and unsupported contracts

Notation accepts at most 1 MiB, 128 nodes, 512 edges and 16384 bytes per line.
Strict JSON uses the existing serializer file limit and the same node/edge
bounds. Canonical output sorts declarations by name and connection spellings;
numeric values use round-trippable float spellings. IDs are regenerated;
notation preserves authored topology and supported values, not editor layout,
prepared runtime history, publication policy or native DSP qualification.

The notation profile refuses hosted binary plugins, spectral nodes, sample
regions, opaque Processor/custom state, non-stereo audio boundaries,
non-default node scalars outside Gain, feedback, sidechain, dense modulation,
and sparse ranges that cannot be expressed exactly as a depth. Symbolic enum
values and duplicate port-name suffixes are not implemented in this profile;
use exact unique runtime names and numeric values. A type not constructible in
the supplied catalog is unsupported. Saved graphs with supported registered
opaque state may validate through the existing restore contract, but formatting
refuses to discard it. Preserve richer state with `GraphSerializer` and its
owning typed APIs. This bounded profile is not full G2 completion.

## Installed consumer and evidence

`examples/graph-patch-consumer` is a standalone `find_package(Pulp COMPONENTS
host)` project. Its Processor plus registration and main are 100 lines
of glue. It restores the ten-line notation through `.pulpgraph`, then renders
257 frames independently checked against 0.375 for the first 64 frames (one
injected MIDI note) and 0.25 afterward. The numeric oracle follows directly
from input 0.5 times modulation level 0.5 plus the MIDI increment 0.125.

The focused C++ suite covers API named MIDI, stale/foreign handles, audio and
MIDI sharing a node pair, deterministic roundtrip, unsupported contracts,
negative diagnostics and several render block partitions. The installed CMake
fixture stages a fresh SDK, checks the public header digest, copies this
consumer outside the source tree and runs it. Its tool parity phase compares
all failure fields across C++/CLI/MCP and checks notation/save roundtrip.

## Required graph-file reload successor

`tools/cli/cli_common.cpp::snapshot_timestamps` feeds `watch_loop`, used by
`cmd_dev` and `cmd_loop` for rebuild/relaunch. Adding `.pat` or `.pulpgraph` to
that extension set alone cannot publish a graph to a running host. The successor
needs a host-owned live graph authority and an IPC lifecycle that routes a
strictly parsed candidate into the existing
`SignalGraph::PreparedTopologyEdit` prepare/commit boundary with generation,
failure and continuity receipts. Process replacement and prepared topology
publication are separate operations. No live graph-file reload is claimed here.

## Catalog-only authoring trial

An independent agent read only this guide and the emitted CLI catalog, with no
headers or C++ source. In about 40 seconds it authored and validated a separate
gain patch, then authored a reordered ten-line Processor/MIDI/modulation patch
and passed the installed consumer's numeric/roundtrip check. Initial attempts
omitted the required version directive; the diagnostic supplied the correction.
A renamed-node attempt exposed a consumer-harness assumption (lookup by the
name `midi`); the consumer now locates its MIDI boundary by runtime node type.
The trial used an existing Processor registration and did not independently
author registration glue. That ergonomic acceptance remains unmeasured.
