# A4 Pulp-owned and maintained Three.js DPR corpus — 2026-08-28

This is the corrected compact corpus for the 48 Pulp-owned A4 cells captured
against these independent authorities:

- canonical planning revision:
  `641649b7e7fece6baae34380b6e719904506af22`
- Pulp implementation head:
  `9770c166338fc03a14c7724a9ffd220ec89735df`
- Forge baseline revision:
  `0750a88dea3af7fca927a8c02887e071109407ae`

The earlier `m3-a4-threejs-e011ed4-20260828` corpus is superseded because it
incorrectly used its Pulp SHA for all three authorities. None of those cells
count toward A4.

## Result

- Terminal cells: **48 / 48** across four scenarios, three modes, and four
  requested DPRs.
- Pulp native fixtures: **36 pass, 0 fail**.
- Maintained `threejs-audio-reactive` canary: **0 pass, 12 fail**.
- Inconclusive/unavailable cells: **0**.
- Producer failures: **0**.
- GPU timing: **1,440 / 1,440** samples are finite and strictly positive;
  per-cell medians range from 0.065536 ms to 0.131072 ms.

The 12 Three.js failures are the independent Pulp capture fidelity verdict:
the animated native Three.js canvas did not meet the content/similarity oracle.
The same cells passed hardware identity, same-process metric, logical-input,
and correlated Perfetto checks. The scenario therefore contributes a terminal
Three.js **NO-GO** unless a changed immutable implementation is recaptured.

The machine identity was `Daniels-Mac-Studio.local:arm64`. Every receipt binds
`Native Dawn Adapter (Metal)`, backend `Dawn/WebGPU`, the exact Pulp head,
scenario content digest, attempt nonce/process, and producer digest.

## Corpus

- `plan.json`, `run-state.json`, and `result.json` preserve the exact runner
  documents. The result intentionally contains 48 observations and 36 pending
  Forge/web cells, so the combined result remains `incomplete`.
- `frozen-evidence/` preserves all accepted receipts, raw sample ledgers,
  input receipts, PNG captures, and correlated Perfetto traces.
- `artifact-sha256.txt` records SHA-256 for the copied corpus.

The 44,920,096-byte measurement executable is not duplicated in Git. Every
receipt binds its SHA-256 as
`4bbaa64a24fc2a5c1e686270a6e39958581e7758c17d3d7298e4450af953e689`.
The pinned trace analyzer SHA-256 is
`f5c8e5bf1d0d9add82d02f32b2f3df099a4372e1e4e637b9937ceddb4b2d68a9`.
The complete local run, including immutable producer copies, remains at
`/tmp/pulp-a4-owned-9770c1-correct.wLyrJp/run` for landing-session replay.

The final 84-cell A4 disposition is still dependency-gated until the 24 Forge
and 12 web cells are ingested into the same authority tuple and the required
A2T/A3 dependency receipts are attached.

