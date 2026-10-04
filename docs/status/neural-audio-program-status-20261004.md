# Neural audio program status (2026-10-04, refreshed)

This receipt records the current protected-main state and keeps every neural
product claim fail-closed. Synthetic, CPU-only, and metadata-only evidence is
labelled as such; no provider performance claim is inferred from it.

## Landed

- MLX/named-model disposition merged in [PR 9492](https://github.com/Generous-Corp/pulp/pull/9492), merge [0130b8b9709e8386f56e12d1b99807519a08f486](https://github.com/Generous-Corp/pulp/commit/0130b8b9709e8386f56e12d1b99807519a08f486). It proves MIT fixture provenance and CPU parity; the MLX harness remains tools-only and synthetic.
- Streaming benchmark baseline merged in [PR 9495](https://github.com/Generous-Corp/pulp/pull/9495), merge [f5bf5ce9f0550b8996717a2828149183fa398117](https://github.com/Generous-Corp/pulp/commit/f5bf5ce9f0550b8996717a2828149183fa398117). Its feasibility-only verdict and unavailable safety counters are preserved honestly.
- Neural model-package sidecars merged in [PR 9497](https://github.com/Generous-Corp/pulp/pull/9497), merge [2ee15848dd1905ea3182db10eca35dfdf8dde911](https://github.com/Generous-Corp/pulp/commit/2ee15848dd1905ea3182db10eca35dfdf8dde911). Focused private tests proved 77 assertions across 12 cases, including tamper rejection and fresh-process reload. The recurring ASan Editor-open failures were advisory and unrelated to the neural paths.
- CPU NAM/TCN bridge and portable streaming contract work landed earlier in [PR 9415](https://github.com/Generous-Corp/pulp/pull/9415) and [PR 9413](https://github.com/Generous-Corp/pulp/pull/9413).

## Active PRs and experiments

- [PR 9502](https://github.com/Generous-Corp/pulp/pull/9502), head `13575659ef58d0e47b14742bbe112a83607d6fff`: Dawn WaveNet depth audit. Synthetic proof passed 10/1, full synthetic realtime 463/18, slot-ledger 42/2, 112/6, 108/6, convolution/session 171/9 and 232/18, and WaveNet matrix 123/4. Exact Dawn-provider depth, ordered delivery, late/device-loss handling, and deadline margin remain open.
- [PR 9506](https://github.com/Generous-Corp/pulp/pull/9506), head `5e1f7432d48752558b630a00f0db94bab820f981`: this status receipt. It is queued while hosted checks reconcile.
- Benchmark follow-up: fresh successor base `2ee15848dd1905ea3182db10eca35dfdf8dde911`, hardened head `53f04a6780df204678c22013be5177d9b87f6408`. Focused test passed 167 assertions/1 case. JSON smoke receipt: `/Users/danielraffel/Code/pulp-c2-competitive-json-followup-20261004/build/streaming-receipt-followup.json`, 13,101 bytes, SHA-256 `7a78516638c039189c6fa49ccc0cc5296d526a97823b025dcc915d44732d7a85`; all verdicts are `feasibility_only`, `competitive_gates_complete=false`, and unavailable safety counters are `null`. Full gates are still required before its PR.
- Spectr/Forge/Magenta remains metadata-only. A named licensed model, paced producer receipt, and real consumer proof are still required before catalog registration.

## Open gates

1. Real MLX execution with a named model, CPU shadow parity, provider identity, fallback and reset receipts, and signed/notarized packaging.
2. Exact Dawn provider depth and timing proof on Apple Silicon.
3. A named, licensed Magenta/Forge consumer with sustained pacing.
4. Linux and Windows realtime execution receipts.
5. Competitive benchmark safety counters measured by the real provider rather than `null`.

The coordination goal remains active until each gate has landed with evidence or
has an explicit reviewed disposition. E126 capability-transaction ownership and
GPU-NAM/Dawn source ownership remain unchanged.
