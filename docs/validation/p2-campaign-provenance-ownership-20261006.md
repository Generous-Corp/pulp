# P2 campaign provenance ownership claim

- Owner: `codex-p2-provenance-20261006`
- Protected base: `e582bd551becdab3bff3c061a70ef5f4fcfff56d`
- Worktree: `/Users/danielraffel/Code/pulp-p2-provenance-20261006`
- Bounded paths: `tools/scripts/gpu_audio_p2_campaign.py`, `tools/scripts/test_gpu_audio_p2_campaign.py`, and this ownership receipt.
- Status: implementation complete; private tooling packet; local commit only; no push or PR.
- Scope: require a clean tracked source tree before measurement and bind the exact driver SHA-256 and source revision into the campaign manifest with fail-closed mismatch checks. Preserve plan-only and matrix semantics.
- Validation: `PYTHONPATH=tools/scripts python3 -m unittest tools/scripts/test_gpu_audio_p2_campaign.py` (19 tests passed); `git diff --check`; Python bytecode compilation passed. No campaign or hardware probe launched.
- Handoff: coordinator adversarial review before any publication.
