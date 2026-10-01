# DSPX-01 descriptor and registry contract

DSPX-01 is the admission contract for graph custom-node and bounded
sample-region descriptors. It is a design-time SDK surface. A descriptor is
accepted only when its identity, version, port shape, callbacks, state
lifecycle, and optional projection claims are internally coherent.

`CustomNodeType::is_valid_registration()` is the single shape gate used by
direct graph registration, prepared topology edits, and baked registration.
The graph owns one descriptor per `(type_id, version)` key and exposes a
value-owned, deterministically sorted metadata snapshot. Registering an exact
key replaces its metadata; an invalid registration leaves the prior registry
unchanged. A resolved descriptor can then be reached by
`SignalGraph::add_custom_node`.

Sample-region descriptors use the sibling `SampleKernelDescriptor` contract
in `pulp/host/custom_node_type.hpp`. `SampleRegionProof` reports typed refusal
reasons and bounded resource values. Parser shape admission runs before any
record allocation and refuses malformed, truncated, overflowing, or over-limit
counts through `SampleRegionParserProof`.

The executable contract proof is
`test/test_dspx01_descriptor_registry.cpp`. It covers successful registration
and graph reachability, invalid identity and bake combinations that preserve
registry state, valid scalar descriptor identity, and hostile parser input.
The broader sample-region matrix remains in
`test/test_sample_region_proof.cpp`; DSPX-01 does not add another runtime or
reopen the sample-region kernels.

This packet has no timeline CLI/MCP operation and no live-product control
route. Those surfaces remain typed unsupported until a later packet owns an
operation and a host executor.
