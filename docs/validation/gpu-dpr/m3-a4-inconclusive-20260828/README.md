# A4 DPR M3 preflight — inconclusive publication

This directory preserves the compact, machine-readable result of the real M3
preflight without promoting it to measured A4 evidence. The exact outcome is
`INCONCLUSIVE`: 36 Pulp-native cells produced real PNG captures at their
expected physical sizes, but zero of the 84 cells satisfy the ratified
measured-cell contract. No A4 disposition exists, no render policy is
authorized, and B5 remains `waiting-trigger`.

The files have stable roles:

- `result.json` is the runner-projected incomplete result. It deliberately has
  no observations, no A2T or A3 receipts, and no disposition.
- `capture-index.json` binds all 36 cell keys to their real PNG SHA-256,
  physical size, observed DPR, exact Pulp revision, and capture binary without
  promoting those preflights to measured observations.
- `gap-report.json` is the exact gap and smallest-adapter handoff. It separates
  the 36 capture preflights from measured evidence and names the prerequisites
  for Pulp-native, Three.js, Forge/DAW, and web cells.
- `receipt.json` binds those two files by SHA-256 and records that the raw
  capture corpus is neither checked-in nor release evidence.

The 36 PNGs and per-cell preflight receipts remain producer-worktree-only,
non-release working evidence. They are intentionally omitted: this publication can prove only why
the experiment is incomplete, not replay independent fidelity, input,
same-process identity, metric, or correlated-trace oracles that were never
produced.

Verify the preserved bytes and fail-closed semantics with:

```bash
python3 tools/scripts/verify_gpu_dpr_inconclusive_receipt.py \
  docs/validation/gpu-dpr/m3-a4-inconclusive-20260828
python3 tools/scripts/test_verify_gpu_dpr_inconclusive_receipt.py
```

The self-test updates hashes after planting semantic mutations. Its rejection
therefore proves the verifier checks the incomplete boundary rather than only
detecting stale digests.
