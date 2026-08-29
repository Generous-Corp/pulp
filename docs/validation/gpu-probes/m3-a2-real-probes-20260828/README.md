# Historical A2 GPU-probe evidence — M3 Ultra

This directory is the durable historical raw receipt for the four A2 recipes exercised on
2026-08-28. The installed CLI/MCP runs used a fresh temporary working directory
outside every checkout and a system-only `PATH`. Each group contains two
positive runs and one seeded negative control. The raw v1 results are preserved
rather than rewritten into a summary;
`receipt.json` binds their bytes, every relevant recipe/runtime source blob at
the declared integration commit, executable roles, and executable hashes.

The result was:

- `gpu-compute.magnitude.v1`: authentic Apple M3 Ultra / Metal, 256 numeric
  samples, repeat-stable evidence apart from the bounded evidence ID, and the
  seeded WGSL mutation failed at the CPU oracle.
- `gpu-audio.stft.v1`: authentic Apple M3 Ultra / Metal, 1,024 numeric samples,
  repeat-stable evidence, and the seeded FFT mutation failed at the CPU oracle.
  This recipe is an offline CLI/MCP child-process operation; it does not run in
  an audio callback.
- `renderer3d.hardcoded-cube.v1`: Dawn/WebGPU over Metal rendered, submitted,
  read back, and matched its Metal fingerprint. The planted negative is applied
  before submission as the bounded `pre-submit-framebuffer-downscale` mutation:
  a 32 x 32 framebuffer input whose GPU-produced readback cannot satisfy the
  recipe's 1,500-pixel foreground floor. Its typed execution identity is
  therefore 32 x 32 / 1,024 work items (rather than the positive recipe's
  128 x 128 / 16,384), and validation binds `observed.rgba8` to exactly four
  bytes per declared work item. Its current adapter contract is
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

The 2026-08-28 GPU-side negative-control refresh was rebuilt and replayed from
exact source commit `146716041f20e9cd5e599f11f4697369d3218519` after the final source rebase.
`receipt.json` binds that commit, the installed Rust/C++/MCP binary digests, all
13 newly replayed raw results, recipe blob
`ecdcf9505e02ba4134ded2bb6b8bbdb21de933eb`, and model blob
`22c6eb4b7aa29f5190fbd4798c0c712ab476e28f`. The two commands above pass from
the receipt-rebind commit; metadata-only rebinding or suppression of source
drift is not an acceptable landing path.

This v1 evidence no longer closes final A2 acceptance. It used direct C++
Renderer3D/Three.js roles, exercised installed MCP only for compute, predates
exact installed build/plan provenance and current source, and carries only the
older Forge proof. The v2 recorder and terminal contract are documented in the
parent `docs/validation/gpu-probes/README.md`; preserve this directory without
rewriting it.

The historical evidence makes no
Windows/Linux backend claim and does not replace A3's real visible-host/DAW
acceptance.
