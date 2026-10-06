# P2 campaign provenance ownership claim

- Owner: `codex-p2-provenance-20261006`
- Protected base: `7b2d9f4fdceca1ae7d3eba421692be50fd25aa48`
- Worktree: `/Users/danielraffel/Code/pulp-p2-provenance-20261006`
- Bounded paths: `tools/scripts/gpu_audio_p2_campaign.py`, `tools/scripts/test_gpu_audio_p2_campaign.py`, and this ownership receipt.
- Status: implementation complete; private tooling packet; published on PR 9755 with refreshed protected-main lineage rooted at head `b7b5a96d6f15dcbd00c25d947b20fdea7d313e6e`; receipt-only metadata updates are carried on the current PR head while awaiting protected checks and merge-queue admission.
- Scope: require a clean tracked source tree before measurement and bind the exact driver SHA-256 and source revision into the campaign manifest with fail-closed mismatch checks. Preserve plan-only and matrix semantics.
- Validation: `PYTHONPATH=tools/scripts python3 -m unittest tools/scripts/test_gpu_audio_p2_campaign.py` (19 tests passed); `git diff --check`; Python bytecode compilation passed; full gates passed against the protected base. No campaign or hardware probe launched.
- Handoff: coordinator adversarial review before any publication.
