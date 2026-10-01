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
