# Neural audio program status (2026-10-04, refreshed)

This receipt is audited against the current protected `origin/main` head
[`8e845a48503e4abd066011300f01255d437bce6b`](https://github.com/Generous-Corp/pulp/commit/8e845a48503e4abd066011300f01255d437bce6b), which includes merged PR9493 and PR9501. The older `3dd81fc3fc64a77559229dc87e5836f8c92ee570` and `a72faee66b762cf408e0c8dc09225d6401fb7855` anchors are retained only as historical provenance; neither is treated as current. Claims remain fail-closed: synthetic, CPU-only, metadata-only, and unavailable-counter evidence does not establish provider execution or product performance.

## Landed

- MLX/named-model disposition landed in [PR 9492](https://github.com/Generous-Corp/pulp/pull/9492), merge [`0130b8b9709e8386f56e12d1b99807519a08f486`](https://github.com/Generous-Corp/pulp/commit/0130b8b9709e8386f56e12d1b99807519a08f486). It records MIT fixture provenance and CPU parity; the MLX harness remains tools-only and synthetic.
- Shared-I/O receipt contract landed in [PR 9494](https://github.com/Generous-Corp/pulp/pull/9494), merge [`7f5aedef12a5ea284614c88ceb843d98812d4efe`](https://github.com/Generous-Corp/pulp/commit/7f5aedef12a5ea284614c88ceb843d98812d4efe). The lifecycle and terminal receipts remain contract evidence, not a named-provider performance result.
- Versioned streaming benchmark receipts landed in [PR 9495](https://github.com/Generous-Corp/pulp/pull/9495), merge [`f5bf5ce9f0550b8996717a2828149183fa398117`](https://github.com/Generous-Corp/pulp/commit/f5bf5ce9f0550b8996717a2828149183fa398117). Its feasibility-only disposition and unavailable safety counters remain explicit.
- Neural model-package sidecars landed in [PR 9497](https://github.com/Generous-Corp/pulp/pull/9497), merge [`2ee15848dd1905ea3182db10eca35dfdf8dde911`](https://github.com/Generous-Corp/pulp/commit/2ee15848dd1905ea3182db10eca35dfdf8dde911). Focused private tests proved tamper rejection and fresh-process reload; this does not prove a product model or provider.
- Changed-surface accounting landed in [PR 9500](https://github.com/Generous-Corp/pulp/pull/9500), merge [`0d6b64e2b192b70ea86bc540e9af876ebac7bc44`](https://github.com/Generous-Corp/pulp/commit/0d6b64e2b192b70ea86bc540e9af876ebac7bc44).
- Audio glitch tracing landed in [PR 9501](https://github.com/Generous-Corp/pulp/pull/9501), merge [`a72faee66b762cf408e0c8dc09225d6401fb7855`](https://github.com/Generous-Corp/pulp/commit/a72faee66b762cf408e0c8dc09225d6401fb7855). It is included in protected-main history; the protected head has since advanced, and this does not close the neural provider gates.
- Dawn WaveNet depth audit landed in [PR 9502](https://github.com/Generous-Corp/pulp/pull/9502), merge [`3c04dcff568c4315207e3e0cbca469a749451f31`](https://github.com/Generous-Corp/pulp/commit/3c04dcff568c4315207e3e0cbca469a749451f31). The audit remains **FAIL / gate open**: synthetic channel and slot-ledger tests do not prove authenticated Dawn depth, ordered delivery, late retirement, device-loss handling, or deadline margin.

## Current coordination state

- [PR 9514](https://github.com/Generous-Corp/pulp/pull/9514) is the active benchmark follow-up at head [`ad330a92e0000670adb524f7d323e174ef6949e8`](https://github.com/Generous-Corp/pulp/commit/ad330a92e0000670adb524f7d323e174ef6949e8). It is disjoint from this status-only receipt. Its focused test reports 167 assertions in 1 case. The checked receipt at `/Users/danielraffel/Code/pulp-c2-competitive-json-followup-20261004/build/streaming-receipt-followup.json` is 13,280 bytes with SHA-256 `fc759480bd67aacec13c7ad6e33d1edc98aee6850b718d840dc99d2581b2118d`; it is a synthetic unpaced CPU feasibility campaign with `competitive_gates_complete=false` and null explicit safety counters.
- [PR 9522](https://github.com/Generous-Corp/pulp/pull/9522) is the active Spectr/Forge metadata refresh at head [`08f0de25b87678e79844e6210787ddce0374d9d4`](https://github.com/Generous-Corp/pulp/commit/08f0de25b87678e79844e6210787ddce0374d9d4). Its shared `drift-fast` failure is not provider or product evidence. It remains metadata-only and does not register a Magenta/Forge consumer or claim sustained generation.
- [PR 9506](https://github.com/Generous-Corp/pulp/pull/9506) was closed on 2026-10-04 and is not queued or active. [PR 9512](https://github.com/Generous-Corp/pulp/pull/9512) and [PR 9513](https://github.com/Generous-Corp/pulp/pull/9513) were also closed; none is current evidence.
- [PR 9526](https://github.com/Generous-Corp/pulp/pull/9526) is the previous status successor at head [`6e2d2e40dec4894dcc692b4fe3c99b3e603d9298`](https://github.com/Generous-Corp/pulp/commit/6e2d2e40dec4894dcc692b4fe3c99b3e603d9298), based on the older `a72faee6` protected head. This receipt is a fresh successor from `8e845a48`; it does not force-push or alter PR9526.

## Open product gates

1. Real MLX execution with a named licensed model, CPU shadow parity, provider identity, fallback/reset receipts, and signed/notarized packaging.
2. Authenticated Dawn provider depth and timing on Apple Silicon, including ordered delivery, late/device-loss recovery, and deadline margin.
3. A named licensed Magenta/Forge consumer with sustained pacing and a real consumer receipt.
4. Linux and Windows realtime execution receipts.
5. Competitive benchmark safety counters measured by the real provider, with paced delivery, independent-process cold starts, named serialized model/corpus hashes, and the remaining campaign controls.

The coordination goal remains active until these gates have landed with evidence or an explicit reviewed disposition. E126 capability-transaction ownership and GPU-NAM/Dawn source ownership remain unchanged.
