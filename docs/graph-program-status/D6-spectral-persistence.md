# D6 spectral persistence on the saved-identity v7 envelope

Status: the JSON persistence slice is implemented. The notation grammar half
of D6 (`# flow 2`) is a separate change and is not part of this one.

## What changed

- `GraphSerializer::to_saved_document` and `to_saved_file` write a spectral
  graph as the saved-identity v7 document (format 7). Before this change they
  returned an empty document for any spectral graph.
- Each node record carries its `authored_id`; `identity_schema`,
  `logical_identities` and `bindings` are the same envelope ordinary v7
  documents use, so `inspect_document`, `from_saved_document`,
  `diff_graph_documents` and `pulp graph diff` read spectral documents with
  no spectral special case. `find_unrestored_connection` compares the
  spectral lane flag like the other flags instead of refusing it.
- Every spectral node records a readable `identity` (`fft_size`, `hop`,
  `channels`, `sample_rate_bits` as 16 lowercase hex digits) that must match
  its `contract_hex` exactly.
- `spectral_analysis` nodes are saved as their declaration only. No captured
  input audio or live FFT history is written or admitted.
- `spectral_asset_source` (the single-stream source) is saved with one
  embedded, hash-checked asset and `finite_policy: 2`; asset morph nodes
  record `finite_policy: 1`. A recorded policy that is not the factory's is
  refused.
- Edges write every lane flag, including `control_signal`.
- Format 6 stays the `to_json` spectral document and is read with its
  original rules. A format 6 load assigns authored IDs by the D1 migration
  rule (digest of the whole document, keyed by persisted node id), so the
  same file always restores the same IDs; saving it as v7 is its migration.
- `add_spectral_asset_source_v1` now goes through `push_node_locked_`, so a
  source node gets an authored ID like every other node. It previously
  bypassed the mint, contrary to the D1 design note that every add path
  allocates one; the v7 writer refuses a node without a valid authored ID.
- `to_saved_subgraph_document` refuses subgraph records on a spectral graph,
  so the v7 writer does not open subgraph persistence to spectral graphs.

## Refusal discipline

The spectral reader builds an isolated candidate, and every refusal goes
through `detail::refuse_load` before the destination is touched. Authored IDs
are planned (`plan_authored_ids`) and assigned on the candidate, inside a
try block, before the destination mutation lock is taken. Injected-failure
hooks cover both points (`fail_next_spectral_candidate_build`,
`fail_next_authored_id_plan`).

## Tests

- `test/test_spectral_graph_saved_identity.cpp`: v7 round trip (byte-identical
  re-save, authored IDs, morph audio oracle); format 6 migration with
  deterministic IDs; analysis to mask to synthesis audio through a restored
  graph against the independent DFT/OLA oracle at FFT 256, hop 100, two
  channels and a rate one ulp above 44100 (with a negative control proving
  the mask is what the oracle checks); single-stream source at hop 96 against
  the oracle; malformed fields, unknown versions, identity and binding
  corruption, hash corruption, substituted bytes, invalid typed edges, mixed
  families, policy versions, oversized tables, destination limits, the bank
  bound at the admission block and injected allocation failures, each leaving
  a prepared bystander graph unchanged and audible; inspect and diff.
- `test/test_cli_graph_spectral_saved_identity.cpp`: `pulp graph save
  --saved-identity` on a format 6 spectral file.

## Remainder

- Spectral edit history is implemented: `GraphEditHistory` checkpoints
  spectral graphs and bounds its total retained bytes (see
  `docs/guides/spectral-asset-replay.md`, "Diff and edit history").
- The installed A3 replay consumer still records a format 6 fixture; an
  installed v7 consumer (analysis and single-stream source) is not built yet.
- Notation for spectral constructors (`# flow 2`) is the separate D6a change.
