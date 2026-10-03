# Sample-region compatibility baseline (C0)

This packet freezes behavior from Pulp
`e922ba8e7e03742cdb0b748c1bc872ff18ecab7c` before the sample-region graph
model is introduced. The fixtures are committed values, not values regenerated
by the verifier:

- `.pulpgraph` v1 and v2 inputs plus exact renders;
- exact bytes from serializing a graph with no region data;
- fixed and varying callback schedules loaded from the legacy graph plus a
  separately constructed graph that calls `connect_feedback()` directly;
- a signed `.pulpbake` v1 envelope and its exact render;
- `Processor` size/alignment and its complete preexisting virtual-signature
  prefix, stable enum values, and the complete node/reload C ABI layout and
  function signatures;
- a source consumer written against the old installed SDK surface, including a
  positional `CustomNodeType` initializer and existing C ABI entry point.

The frozen `CustomNodeType` size is 608 bytes in this generation. The increases
over the original 512-byte C0 receipt are intentional and additive. 512 to 544
added the block-aware `latency_samples_for_block` callback used by downstream GPU
audio consumers such as Forge. 544 to 608 added the two event-aware process
callbacks, `process_events` and `process_instance_events`, which let a Custom
node read the inbound MIDI the graph already gathers for every node.

Both increases append members after the historical callbacks, so positional
aggregate initializers written against an earlier generation still compile — a
full-tree build is what proves that, not this receipt. The compatibility gate
records each as a reviewed source-layout change while continuing to protect the
C ABI and the virtual-method prefix, both of which are unchanged.

## Adding a member to `CustomNodeType`

Size is not the axis. The audio thread never reads `CustomNodeType` — callbacks
are copied into the compiled graph at prepare, and the registry is only read at
register/prepare/compile — so struct size and cache behaviour do not decide
anything, and there is no byte ceiling worth stating. Append-last is free and is
proved by a real consumer (`old_installed_sdk_consumer.cpp`), so positional-init
compatibility does not decide it either.

The one question that decides it, first yes wins:

1. **Must this contract be stable bytes — hashed, proven, serialized, compared
   for identity, or handed across a bake or the C ABI?** Then it is a **sibling
   descriptor**, registered beside the type the way `SampleKernelDescriptor` is
   (`register_custom_node_type(type, sample_kernel)`). That is why that one is a
   sibling: a `std::function` cannot be hashed, proven or serialized, so it could
   never have lived there. Never make something a sibling merely to avoid adding
   a line to this receipt.
2. **Otherwise — a `std::function` or metadata consumed only through the registry
   at register/prepare/compile?** Then it is a **field, appended last**, plus one
   sentence in the receipt above.

A field that opens a new execution lane (as transport, baked-param and events
each did) costs four edits, and this is the real cost — not the bytes:

- the callback member itself, appended after the historical callbacks
- a rule in `CustomNodeType::is_valid_registration()` for the combinations it
  must refuse
- the `same_function` equality block in `signal_graph.cpp`, which compares every
  callback; omit one and a re-registration that changed it silently no-ops
- the compile-time capture into the compiled graph, and the matching dispatch in
  BOTH the routed binding and the reference walk

Miss any one and the failure is silent rather than loud.

**When this guidance stops being true.** (a) If any executor path starts reading
`CustomNodeType` on the audio thread instead of capturing at compile, size
becomes a real axis — today `git grep custom_node_types_ core/host/src` shows
only register/prepare/compile reads. (b) If `CustomNodeType` itself is ever
hashed or serialized, question 1 flips for every member and the whole struct
becomes a bytes contract. (c) If `old_installed_sdk_consumer.cpp` is retired,
append-last stops being required, though it stays harmless. (d) If
`same_function` is replaced by generated or reflective equality, drop that
checklist item.

Run the complete positive and deliberate-perturbation matrix with:

```sh
python3 tools/scripts/sample_region_compat_baseline.py --negative-controls
```

To prove the frozen consumer through the real installed-package surface, add
`--installed-sdk /absolute/path/to/sdk`. The baseline creation receipt used
the retained `~/.pulp/sdk/0.837.0` installation. Its provenance JSON and every
SDK header, CMake input, library, and dylib consumed by the old-source build are
SHA-256 pinned in `installed-sdk-0.837.0.json`; the verifier refuses a different
or incomplete SDK before accepting the compile. The exported Pulp CMake
directory is also closed: any unreceipted file there refuses verification, so
configure cannot silently consume an added module outside the frozen set.
Without that option the same consumer compiles and links against the exact
source head, which keeps the gate self-contained on clean CI machines.

The negative matrix independently perturbs the legacy inputs and outputs, the
installed-SDK receipt and consumer output, the Processor layout and complete
virtual prefix, and the signed bake envelope, public key, and render.

The verifier returns 1 for compatibility drift and 2 when configure, compile,
link, fixture access, or execution prevents it from reaching a compatibility
verdict. This distinction is part of `PKT-C0-01`.

`compat_runner --emit` exists only to create the initial exact-head baseline.
It is never called by the verifier. Updating the committed expected values is a
reviewed compatibility-baseline change, not a response to a failing gate.
