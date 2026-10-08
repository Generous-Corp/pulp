# Native and web projection contract

DSPX-07 adapters advertise a graph feature only after a bounded baked `Processor`
descriptor exists. CLAP, VST3, LV2, WAM, WebCLAP, and Audio Units use the same
capability rule. A graph-only descriptor or an unbounded descriptor produces a
typed unsupported result and leaves the source state unchanged.

The AUv2 and AUv3 adapters use their existing bus negotiation, parameter, state,
latency, and render contracts. The `pulp-test-dspx07-au-baked-projection` fixture
drives one nonempty baked graph through both real adapter render paths and checks
the independent gain oracle, reset boundary, and zero-latency report. It does
not replace host packaging or commercial DAW receipts.

The projection decision is control-thread metadata. Audio rendering and browser
execution still require the adapter's ordinary descriptor, latency, parameter,
and state round-trip contracts. Dense modulation buffers remain transient and
must never be serialized into descriptor JSON or package metadata.

Forge, Spectr, and GPU-NAM consumer adoption is deferred. Their receipts require
separate owners, exact installed-SDK provenance, immutable artifact identity,
and product-facing runtime proof.
