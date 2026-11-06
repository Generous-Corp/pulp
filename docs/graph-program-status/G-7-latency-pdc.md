# G-7 latency/PDC consumer fixture

This fixture is a bounded installed-SDK consumer for the prepared latency/PDC
snapshot API. It constructs a diamond graph with a custom four-sample delay,
checks deterministic per-node input/output latency, checks the delayed fan-in
branch (zero delay) and the dry fan-in branch (four samples of PDC), and checks
planted unknown-node and unknown-connection negatives. A copied snapshot stays
readable after a topology mutation while a fresh query fails closed until the
graph is prepared again. The follow-on also repeats the positive PDC and
planted-negative queries across eight paced 64-frame process blocks.

It does not implement CLI/MCP serialization, realtime qualification, or full
G-7 completion. The API is control-thread introspection sourced from the
published prepared snapshot; it is not an audio callback surface.

State: PARTIAL; exact-head installed-SDK and focused-host receipts are recorded
below. This does not claim CLI/MCP serialization, realtime qualification, or
full G-7 completion.

## Exact-head receipt

- **Source head:** `8ecb5db3c9c0323ed065d7b9055e217ba715b292`.
- **Receipt package head:** `da24589115dee2326152c92eb97087655ec44812`.
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
- **Paced introspection control:** eight 64-frame blocks were processed at a
  48 kHz wall-clock-shaped cadence. Each control-thread snapshot retained the
  four-sample PDC map, and the unknown node/edge negatives remained empty. This is continuity evidence for the diagnostic API only; it
  does not qualify realtime scheduling, callback safety, CLI/MCP serialization,
  or full G-7 completion.

## Follow-on paced receipt

- **Source base:** `571ea4f74e53f51cea37b506d81e4989a07103af` (the follow-on
  worktree adds only the consumer and documentation changes).
- **Installed SDK:** the exact existing SDK at
  `/Users/danielraffel/.pulp/sdk-dev/forge-v1/darwin-arm64/8ecb5db3c9c0323ed065d7b9055e217ba715b292/11a0d651ba9e`.
- **Command:** Release CMake configure with `Pulp_DIR`, governed build of
  `pulp-g7-latency-pdc-consumer`, CTest, then direct execution.
- **Result:** CTest `1/1` passed; direct output was
  `g7-latency-pdc PASS total_latency_samples=4 delayed_node_input_latency_samples=0 paced_blocks=8`.
- **Receipt SHA-256:**
  `c938042f957c02823896636446658e2f15b9ae7ab2479a08146c98bd2816f7ad`.
The paced run is diagnostic continuity evidence for control-thread
introspection. It does not qualify realtime scheduling or callback safety and
does not add a CLI/MCP surface.
