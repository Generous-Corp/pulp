# Sample-granular graphs

Pulp sample regions let a graph author describe a bounded scalar DSP program inside
an ordinary `SignalGraph`. The graph remains the public composition model: a
sample region is a declared set of registered scalar kernels, explicit boundary
connections, and promoted parameters. The canonical executor lowers an admitted
region to one prepared `Processor` path; callers do not instantiate a second DSP
engine.

## Declare a region

Create a disposable `SignalGraph` edit, register the built-in sample kernels, add
nodes, and declare a region over those nodes. A kernel descriptor declares its
stable type and version, scalar input/output counts, state size and alignment,
causality, and the scalar process contract. The authoring API rejects unknown
nodes, duplicate membership, unsupported edges, unresolved kernels, invalid
state limits, and non-causal feedback before publication.

A one-sample recurrence uses an explicit `UnitDelay` node. It is the causal
boundary that makes feedback legal; an ordinary graph cycle remains refused.
Connect audio and promoted scalar parameters through the sample-region APIs so
the resulting parameter contract can be frozen with the Processor's ordinary
`StateStore` manifest. Do not use an ad-hoc capability bit or a block-size field
to describe this scalar, per-frame ABI.

```cpp
host::SignalGraph edit;
host::register_builtin_sample_region_types(edit);
// add scalar nodes, explicit UnitDelay nodes, and region connections
const auto declared = edit.declare_sample_region(std::move(region));
if (!declared.accepted) /* inspect declared.proof.message */;
const auto proof = edit.prove_sample_region(region_id);
```

## Prepare and publish

Freeze the promoted parameter contract before exposing the Processor. Bind it to
one owner-local `StateStore`, prepare the candidate at the host sample rate and
maximum block size, then publish the edit through the normal graph transaction.
The callback uses prepared storage and has no allocation or blocking admission
path. A rejected edit remains unpublished and reports a structured refusal.

Runtime edits use immutable snapshots. A state cell is retained only when its
region, node, kernel type/version, descriptor, and prepared configuration are an
exact identity match. The old and new bindings cannot execute concurrently:
`SampleRegionExecutionDomain::try_admit()` fails immediately on contention or
when a newer generation has already been adopted. This preserves the next
output for unchanged delay identities while allowing changed identities to start
fresh state.

## Bake and serialize

`bake(graph)` lowers an admitted graph to a `BakedGraphProcessor`. A signed
`.pulpbake` is untrusted input: signature verification and bounded parsing happen
before reconstruction, and the reconstructed graph is proved and baked again.
Temporal history is runtime state and is never serialized as authored graph state.
Use the installed `examples/sample-region-allpass` consumer to exercise graph
JSON, signed bake/load, reload, and regular/irregular callback schedules against
an independent scalar oracle.

## Proof and portability

A useful proof has three independently inspectable parts: the declared contract,
reachable execution through the canonical graph executor, and a focused positive
and negative result with source, executable, artifact, and log hashes. Native,
VST3, CLAP, AU, WebCLAP, and WAM projections should all consume the same
`Processor`/graph contract. A generated catalog row or a registered test without
an executed receipt does not establish reachability.

See the allpass example and the [SignalGraph reference](../reference/signal-graph.md)
for the public vocabulary. The sample-region surface is experimental and bounded;
unsupported latency, state, automation, or projection cases must remain explicit
refusals until their contracts and compatibility proofs exist.
