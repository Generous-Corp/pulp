# Native and web projection contract

DSPX-07 adapters advertise a graph feature only after a bounded baked `Processor`
descriptor exists. CLAP, VST3, LV2, WAM, and WebCLAP use the same capability
rule. A graph-only descriptor, an unbounded descriptor, or an AU surface outside
this packet produces a typed unsupported result and leaves the source state
unchanged.

The projection decision is control-thread metadata. Audio rendering and browser
execution still require the adapter's ordinary descriptor, latency, parameter,
and state round-trip contracts. Dense modulation buffers remain transient and
must never be serialized into descriptor JSON or package metadata.

Forge, Spectr, and GPU-NAM consumer adoption is deferred. Their receipts require
separate owners, exact installed-SDK provenance, immutable artifact identity,
and product-facing runtime proof.
