# Notation cookbook: what the default catalog can express

`pulp graph validate` checks a `.pat` file against the default catalog:
`pulp/audio_in`, `pulp/audio_out`, `pulp/gain`, `pulp/midi_in` and
`pulp/midi_out`. Anything else must be registered by an application in C++, so
the CLI refuses it. This page gives, for four common recipes, either a patch
that validates or the exact refusal plus what to register. Every output below
was produced by the CLI built from this tree. Start with
[First graph in five minutes](graph-first-steps.md) if you have not built it.

The two registration routes are a `ProcessorRestoreDescriptor` (a `Processor`
with a `StateStore`, giving automatable parameters) or a `CustomNodeType`
(a graph utility node), both registered once in
`pulp::host::GraphPatchCatalog`. See
[Patch notation](graph-patch-notation.md#discover-and-author) and the installed
consumer in `examples/graph-patch-consumer`.

## 1. Stereo gain chain (validates)

```text
# flow 1
input : pulp/audio_in
gain1 : pulp/gain
out : pulp/audio_out
input.audio >> gain1.audio
input.out1 >> gain1.in1
gain1.audio >> out.audio
gain1.out1 >> out.in1
gain1.gain = 0.5
```

```text
$ pulp graph validate first.pat
valid: true
format_version: 4
nodes: 3
connections: 4
```

Chaining through `gain1` without naming its port is refused, because a chain
hop infers a port only when the node has exactly one output:

```text
pulp graph: line 5: ambiguous port: node 'gain1' has 2 output ports and a chain hop infers a port only when the node has exactly one output; write the port explicitly, e.g. gain1.audio >> next.audio; use one of: audio, out1
```

## 2. Static panning (validates, with built-in gains)

Fixed left/right weights need only two `pulp/gain` nodes. This feeds the left
input channel to a 0.75 gain on the left output and to a 0.25 gain on the right
output:

```text
# flow 1
input : pulp/audio_in
left : pulp/gain
right : pulp/gain
out : pulp/audio_out
input.audio >> left.audio
input.audio >> right.in1
left.audio >> out.audio
right.out1 >> out.in1
left.gain = 0.75
right.gain = 0.25
```

`pulp graph validate pan.pat` prints `valid: true` with 4 nodes and 4
connections. Probing it with a 0.5-peak sine (the WAV from the first-steps page)
measures a peak of 0.375 on `left.audio >> out.audio` and 0.125 on
`right.out1 >> out.in1`, matching 0.5 x 0.75 and 0.5 x 0.25.

Azimuth, HRTF or a reusable mono-to-stereo stage need more than fixed gains.
The reusable form is a `SubgraphTemplate` (public C++ API, documented in
`examples/graph-spatial-cookbook/README.md`), which notation cannot spell. The
fixture's patch, spelling the template as a type, is refused:

```text
$ pulp graph validate examples/graph-spatial-cookbook/graph.pat --json
{"valid": false, "code": "UnsupportedContract", "line": 6, "node": "stage", "port": "", "rule": "unregistered type/version 'cookbook.static-stereo.v1'", "suggested_fix": "register the exact Processor or CustomNodeType from the catalog", ...}
```

## 3. Named buses (refused: needs a registered Processor)

Named host buses (for example a mono `Source` and stereo `Front` and `Rear`)
belong to a Processor's declared layout, not to the graph. Notation has no bus
construct, so the patch can only name the Processor and the validator refuses
the unregistered type:

```text
$ pulp graph validate examples/graph-named-bus-cookbook/graph.pat
pulp graph: line 6: unregistered type/version 'pulp.cookbook.NamedBusPlacement'; register the exact Processor or CustomNodeType from the catalog
```

You need: a `Processor` whose descriptor declares the named buses, registered
through `GraphPatchCatalog`. The full recipe is
`examples/graph-named-bus-cookbook/README.md`; it notes that SignalGraph nodes
map to main input/output channel counts rather than named host buses.

## 4. Live retune (refused: needs a registered Processor)

Two things fail in the default catalog.

Automating the built-in gain with `~>` is refused, because `pulp/gain` exposes
`gain` as a node setting (which is why the catalog lists it and
`gain1.gain = 0.5` works) and not as an automatable parameter:

```text
$ pulp graph validate modgain.pat      # input.audio ~> gain1.gain @ 0.5
pulp graph: line 4: built-in pulp/gain exposes 'gain' as a node setting, not an automatable parameter; set it with gain1.gain = <value>, or automate a registered Processor parameter
```

Retuning an instrument while it sounds needs the instrument itself, which is
application code:

```text
$ pulp graph validate examples/g11-live-retune-consumer/graph.pat
pulp graph: line 6: unregistered type/version 'pulp.g11.LiveRetuningInstrument'; register the exact Processor or CustomNodeType from the catalog
```

You need: a `Processor` whose `StateStore` declares the parameter (for example
`frequency`), registered through `GraphPatchCatalog`. Then
`source.audio ~> voice.frequency @ depth` is accepted, and the retune itself is
a control-thread write at a callback boundary. See
[the live retune cookbook](g11-live-retune-cookbook.md) and
`examples/g11-live-retune-consumer/README.md`.

## Summary

| Recipe | In the default catalog? | If not, register |
| --- | --- | --- |
| Stereo gain chain | Yes | nothing |
| Static panning | Yes (fixed gains) | `SubgraphTemplate` for a reusable stage |
| Named buses | No | a `Processor` declaring its bus layout |
| Live retune | No | a `Processor` with the parameter in its `StateStore` |
