# G-7 latency/PDC consumer fixture

This fixture is a bounded installed-SDK consumer for the prepared latency/PDC
snapshot API. It constructs a diamond graph with a custom four-sample delay,
checks deterministic per-node input/output latency, checks the delayed fan-in
branch (zero delay) and the dry fan-in branch (four samples of PDC), and checks
planted unknown-node and unknown-connection negatives. A copied snapshot stays
readable after a topology mutation while a fresh query fails closed until the
graph is prepared again.

It does not implement CLI/MCP serialization, realtime qualification, or full
G-7 completion. The API is control-thread introspection sourced from the
published prepared snapshot; it is not an audio callback surface.

State: PARTIAL; exact-head installed-SDK and focused-host receipts are recorded
below. This does not claim CLI/MCP serialization, realtime qualification, or
full G-7 completion.

## Exact-head receipt

- **Source head:** `8ecb5db3c9c0323ed065d7b9055e217ba715b292`.
- **Installed SDK:** `/Users/danielraffel/.pulp/sdk-dev/forge-v1/darwin-arm64/8ecb5db3c9c0323ed065d7b9055e217ba715b292/11a0d651ba9e`.
- **SDK provenance:** SHA-256
  `4335a1fb583b56590a34253bcbe6564189942b9f59ef27cbc46429f43012cb4f`.
- **Consumer receipt:** SHA-256
  `8a24976fa451f4d1637220cce6b46bd82ead17f8002e0d39a2a7301bc4f47544`.
- **Consumer binary:** SHA-256
  `f62e930aee3b31d77d6e00249f3dcf68cbd1dc2655bb3806906fa139f268beed`.
- **Installed-SDK consumer:** Release configure/build and CTest `1/1` passed;
  direct output was
  `g7-latency-pdc PASS total_latency_samples=4 delayed_node_input_latency_samples=0`.
- **Focused host result:** 144 cases and 12,416 assertions passed on the
  focused host lane.
- **Negative and mutation controls:** unknown node and unknown connection
  queries returned no value (no fabricated zero-delay/latency entry); a copied
  prepared snapshot remained readable after topology mutation, while a fresh
  query stayed unavailable until re-prepare. These controls cover the fixture's
  fail-closed and snapshot-lifetime behavior only.
