# DSPX-05 serialization and bake

Pulp graph serialization uses canonical ordering and bounded records. Graph records are sorted by stable node identity before emission; duplicate or out-of-range references are refused. The baked payload codec enforces format and byte caps while streaming, rejects unknown versions, malformed lengths, unsupported nodes, and trailing bytes, and migrates only the registered v1 to v2 to v3 sequence.

The signed `.pulpbake` envelope verifies the manifest, trusted signer, plan hash, and signature before parsing. A failed verification or migration returns a typed refusal and performs no graph loading. Positive round trips and negative malformed, reordered, stale-version, and unsupported-node controls are covered by the existing `pulp-test-baked-codec` and baked graph parity suites.

The `dspx05.serialization-bake@1` capability is namespaced and describes these bounds and refusal behavior. CLI/MCP/live-product surfaces remain deferred until their executable lifecycle receipts exist.
