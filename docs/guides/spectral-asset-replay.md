# Save and replay a two-asset spectral morph

The finite profile-1 morph can be saved as a bounded `.pulpgraph` file. It
keeps the geometry decoded from its assets (FFT N a power of two from 256 to
16384, a hop that divides N and is at most N/2, 1 to 16 channels), direct live
Audio control input, typed stages and synthesis. Node ids identify nodes in the
file; node names are labels and may repeat. Loading reconstructs a stopped authoring graph;
the caller prepares it before rendering.

```cpp
#include <pulp/host/graph_serializer.hpp>

auto saved = pulp::host::GraphSerializer::to_file(graph, "morph.pulpgraph");
if (!saved.ok)
    return saved.error;

pulp::host::SignalGraph replay;
auto loaded = pulp::host::GraphSerializer::from_file(replay, "morph.pulpgraph");
if (!loaded.ok)
    return loaded.error;
if (!replay.prepare(48000, 4096))
    return std::string("prepare refused");
```

Format 6's `spectral_assets` table embeds canonical SFRM bytes. Each record
binds schema `pulp.analyzed-spectral-asset`, version 1, the original positive
sample count, the frame count, the canonical 32-byte frame contract, and the
canonical bytes' SHA256. The frame count is not free metadata: the reader
derives it from the contract's FFT size, hop and channel count and the original
sample count through the same finite stream rule the source factory and runtime
use, and refuses a record whose geometry or extent the rule does not admit or
whose stored count differs, before any asset bytes are decoded. Its content ID hashes that metadata and byte digest with a versioned
domain separator. A morph node names its exact two records. Existing identity
and real-mask stages retain their closed stage kind, contract and exact gains;
synthesis retains its contract. Edges retain their spectral kind explicitly.

The codec itself still carries no original source length or graph epoch. The
saved asset envelope supplies original length; the runtime supplies the new
graph epoch at preparation. Asset bytes are immutable and owned after load.
No callback decoding, path resolver, plugin coercion or second scheduler is
introduced.

The spectral persistence profile admits Audio boundaries, the fused asset
source, existing identity/mask stages and synthesis. Mixed hosted-plugin,
Processor, opaque Custom state, sample-region and unsupported edge contracts
are refused. Existing non-spectral serialization keeps its previous writer
bytes and restore contract. Spectral graphs are written at format 6, a version
format-5 readers refuse instead of loading spectral nodes as unresolved Custom
nodes and dropping the embedded assets on re-save. Ordinary v5 files migrate to
v6 in memory without changing their bytes.

A spectral graph is saveable only when it is complete. The writer refuses, and
`to_file` reports `serialization_refused`, when the graph holds a
`SpectralAnalysis` node (live analysis is not persisted; only the asset source
is), a node gain other than 1, two nodes with the same name (the reader
requires unique names), a non-finite editor layout coordinate, or a chain the
reader cannot prepare at the saved rate with 4096-sample blocks (for example a
source with no synthesis or output yet). The writer does not name which rule
failed. A spectral graph also has no saved-identity envelope:
`to_saved_document` returns an empty string for it, so `pulp graph diff` and
`GraphEditHistory` do not cover spectral graphs.

Before replacing a destination, the reader validates the complete envelope,
asset digests and references, exact contracts and shapes, typed connections,
and existing compiler/bank admission. Corrupt, missing, mismatched, unsupported
or oversized data leaves the previous authoring and prepared state unchanged.
A running spectral controller must be stopped before replacement. Saved graph
files remain bounded at 64 MiB; prepared spectral storage remains bounded at
64 MiB and blocks at 4096 samples. Frame edge storage grows with the block
size, so admission is checked at a block size. Saving checks the block the
graph is prepared for, or its `GraphLimits::max_block_size` (at most 4096) when
it is not prepared. Loading checks the destination's
`GraphLimits::max_block_size`, at most 4096: a graph that fits the bank only at
smaller blocks loads into a destination whose limits name that block, and is
refused by one that may be prepared at 4096. Arbitrary seeks, loops and hot asset
replacement remain unsupported.

The replay fixture keeps `graph.pulpgraph`, `asset-a.sfrm`, `asset-b.sfrm` and
the exact little-endian binary32 `control.f32` timeline, bound by
`inputs.sha256`. The public consumer validates those saved inputs before
rendering. It computes expected audio with the independent direct float64
DFT/Hann/IDFT/WOLA oracle and checks identical output under blocks 32, 64, 128
and irregular partitions, including the exact 320-sample prefix and finite
tail. The fixture manifest is an input-integrity record, not a signature or
an authorization to load arbitrary plugins.

Functional replay does not establish real-time qualification. The full paced
sample-rate/block matrix, callback/jitter/budget bounds, supported sanitizer
lanes and remaining G10 obligations still require separate evidence.
