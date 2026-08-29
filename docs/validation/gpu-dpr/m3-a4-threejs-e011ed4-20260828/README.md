# A4 maintained Three.js DPR tranche — 2026-08-28

This directory is the compact, checked-in corpus for the 12
`threejs-audio-reactive` cells captured against immutable Pulp implementation
head `e011ed4d9f56eb02c9f67404a9b67da94d6d5438`.

## Result

- Terminal cells: **12 / 12** (three modes times four requested DPRs).
- Cell outcomes: **0 pass, 12 fail, 0 inconclusive, 0 unavailable**.
- Producer failures: **0**.
- Every cell contains 30 finite, strictly positive GPU elapsed-time samples.
  GPU medians range from 0.065536 ms to 0.131072 ms.
- Every failure is the recorded fidelity disposition: the independent Pulp
  capture did not meet the content/similarity oracle for the animated native
  Three.js canvas. Metrics, logical input, authentic hardware identity, and
  correlated Perfetto evidence were still accepted.
- This is a terminal scenario tranche, not the final A4 disposition. The
  combined 84-cell result remains `incomplete` until the other six scenarios
  are ingested. These 12 failures contribute a Three.js **NO-GO** to that final
  disposition unless a new immutable implementation is measured in a new run.

The machine identity was `Daniels-Mac-Studio.local:arm64`. All receipts bind
the authentic hardware adapter `Native Dawn Adapter (Metal)` through
`Dawn/WebGPU`, the exact Pulp head above, the maintained demo content digest,
attempt nonce, process, and the producer digest.

## Corpus

- `plan.json`, `run-state.json`, and `result.json` preserve the exact runner
  documents. The result intentionally contains 12 observations and 72 pending
  cells.
- `frozen-evidence/` preserves each accepted receipt, raw sample ledger,
  logical-input receipt, PNG capture, and correlated Perfetto trace.
- `artifact-sha256.txt` records SHA-256 for the copied corpus.

The 44,920,096-byte measurement executable is intentionally not committed 12
times. Every receipt binds its SHA-256 as
`4bbaa64a24fc2a5c1e686270a6e39958581e7758c17d3d7298e4450af953e689`.
The pinned trace analyzer SHA-256 is
`f5c8e5bf1d0d9add82d02f32b2f3df099a4372e1e4e637b9937ceddb4b2d68a9`.
The full local run, including immutable producer copies, remains at
`/tmp/pulp-a4-threejs-e011ed4.YkcyDp/run` for immediate replay/audit during
this landing session.

## Validation performed

The producer target was built in Release with `PULP_BENCHMARK=ON`,
`PULP_TRACING=ON`, and V8. The focused C++ suite passed 28 assertions across
three cases. The Python runner suite passed 41 planted bad-evidence cases,
including rejection of any zero GPU timing sample, and the native adapter
self-test passed its measured-producer and provenance controls.

