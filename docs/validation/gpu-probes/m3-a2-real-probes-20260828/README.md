# A2 real GPU-probe acceptance — M3 Ultra

This directory is the durable raw receipt for the four A2 recipes exercised on
2026-08-28. The installed CLI/MCP runs used a fresh temporary working directory
outside every checkout and a system-only `PATH`. Each group contains two
positive runs and one seeded negative control. The raw v1 results are preserved
rather than rewritten into a summary;
`receipt.json` binds their bytes, source heads, executable roles, and executable
hashes.

The result was:

- `gpu-compute.magnitude.v1`: authentic Apple M3 Ultra / Metal, 256 numeric
  samples, repeat-stable evidence apart from the bounded evidence ID, and the
  seeded WGSL mutation failed at the CPU oracle.
- `gpu-audio.stft.v1`: authentic Apple M3 Ultra / Metal, 1,024 numeric samples,
  repeat-stable evidence, and the seeded FFT mutation failed at the CPU oracle.
  This recipe is an offline CLI/MCP child-process operation; it does not run in
  an audio callback.
- `renderer3d.hardcoded-cube.v1`: Dawn/WebGPU over Metal rendered, submitted,
  read back, and matched its Metal fingerprint. Its current adapter contract is
  truthfully `unverified` / `unknown`, so this receipt does not promote it to an
  authenticated hardware-class claim.
- `threejs.multi-pass.v1`: V8 plus the pinned Three.js runtime used authentic
  Apple M3 Ultra / Metal, completed all named passes, sampled five oracle
  regions, and detected the seeded final-swatch mutation.
- The clean installed Rust CLI ran the compute and STFT recipes through its
  installed C++ sibling. The installed MCP transcript preserves exit 0 for a
  positive compute probe and `isError: true` plus exit 1 for the completed
  seeded failure.

The binary artifacts are intentionally not checked in: every raw result records
their names, byte bounds, and SHA-256 digests. This keeps the repository small
while retaining the causal and reproducibility contract. The verifier checks
the immutable raw bytes, v1 semantics, repeat determinism, work-completed
markers, bounded artifact declarations, adapter truthfulness, and MCP failure
projection:

```bash
python3 tools/scripts/verify_gpu_probe_acceptance.py \
  docs/validation/gpu-probes/m3-a2-real-probes-20260828
python3 tools/scripts/test_verify_gpu_probe_acceptance.py
```

This evidence closes the A2 real-offscreen and agent-front acceptance on this
host. It makes no Windows/Linux backend claim and does not replace A3's real
visible-host/DAW acceptance.
