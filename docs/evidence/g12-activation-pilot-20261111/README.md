# G12 paced activation pilot — 2026-11-11

This packet records a fresh exact-head Release pilot for the existing G12
prepared gain activation fixture. It is **PARTIAL** evidence only and does not
qualify realtime deadlines, WCET, quiet-host behavior, cross-platform parity,
hosted plugins, or final G12 closure.

## Exact source and run

- Source commit: `0c1fc0d9ff0f0d2aa10e79a0a84179d0a1cc1e44`
- Fixture: `test/fixtures/g5_graph_event_rt_cases.inc`
- Executable SHA-256: `edfd79cee293ca56526dfdecd6da6fa00839ed6e2a93ac2de05d5a2dea1d3fc6` (33027920 bytes)
- Build profile: Release, sanitizer off
- Matrix: 3 rates × 4 block sizes × 2 existing backends = 24 cells
- Pilot callbacks: 64 per cell (1,536 callbacks; 184,320 samples)
- Canonical receipt digest: `4e6b7151343d52925840c57435f8337e64369cd9af34de31e8495efbb4198fd6`
- Receipt file SHA-256 (local retained artifact): `bc39465a12db83891092d2828479cc2b43e071e3e0e02f8737d39ad8edce082f`

The local receipt was validated with:

```text
python3 tools/graph_performance_qualification.py validate docs/evidence/g12-activation-pilot-20261111/receipt.json --source-root .
activation_receipt=pass
```

The retained receipt records every callback's fixed-origin schedule and
lossless float32 output RLE. The pilot observed **34**
deadline misses on this shared host, so `timing_qualified=false`; this is the
expected boundary for a pilot and is not a realtime pass.

## Controls

- Existing direct and canonical-routed fixture: positive (2), broken (4)
  (`omit_commit` and `wrong_gain`), restored (2); all broken runs exited 42
  and restored runs exited 0. Raw output hashes are in
  `real-fixture-controls.json`.
- Receipt validator: positive pass plus 10 independently planted semantic
  mutations rejected and restored green.
- The deterministic output oracle covers every sample in every callback; no
  allocator, lock, blocking, or I/O claim is promoted from this pilot.

Commands, raw callback logs, compile database, and binary archives are retained
locally for review but omitted from this source packet. The compact JSON files
retain exact source, executable, receipt, and raw-control hashes.

The next admissible step is a quiet-host, exact-head qualification run of the
same fixture with 100,000 paced callbacks per cell, plus independent resource
interposition and host/provider identity. This packet does not authorize that
claim or close G12.
