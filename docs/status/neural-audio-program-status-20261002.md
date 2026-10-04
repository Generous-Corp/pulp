# Neural audio program status (2026-10-02; reconciled 2026-10-04)

This is the resumable status snapshot for the neural-audio workstream. The
normative plan, gate definitions, planted controls, and pass criteria remain in
[`planning/research/2026-10-01-neural-audio-mlx-first-program-plan.md`](../../planning/research/2026-10-01-neural-audio-mlx-first-program-plan.md).

There is no honest single percentage for this program: the packages have
different weights and several lanes have passing scaffolding without a product
gate. The current disposition is **two lanes substantially evidenced, four
lanes partial or open, and no full program sign-off**.

The performance standard is recorded in
[`neural-competitive-proof-plan-20261003.md`](../reports/neural-competitive-proof-plan-20261003.md).
It makes the user's outperformance objective measurable: matched models and
state semantics, quality parity, 100,000 paced blocks, p50/p95/p99 deadline
margin, safety counters, provider proof, and reproducible JSON receipts. Until
those conditions pass, this workstream must describe results as experiments or
capability evidence rather than broad superiority.

## Six-session disposition

| CMUX surface | Lane | Disposition | Evidence / remaining gate |
|---|---|---|---|
| `surface:25` | CPU oracle | Serialized NAM/TCN A1 fixture bridge and the focused CPU controls pass; broader matrix gated | [`neural-nam-tcn-artifact-bridge-receipt-20261003.md`](neural-nam-tcn-artifact-bridge-receipt-20261003.md), merged by [PR 9415](https://github.com/Generous-Corp/pulp/pull/9415); A2, ConvNet, Linear, broader TCN/Keras fixtures and independent-engine comparison remain open |
| `surface:26` | benchmark / RT | Streaming contract, planted callback/worker controls, and portable probe counters pass as test evidence | [PR 9413](https://github.com/Generous-Corp/pulp/pull/9413) watch event records 93 assertions in 19 cases; this is test-only instrumentation, not production-path coverage; Linux/Windows receipts and the competitive JSON campaign remain open |
| `surface:27` | planning | Competitive gate plan is merged and pinned in the source tree | `planning` points to `9986b6a383e981277f71b297be216fba750b4567`, the merge for planning [PR 534](https://github.com/danielraffel/pulp-planning/pull/534); Pulp [PR 9459](https://github.com/Generous-Corp/pulp/pull/9459) advanced the pointer to `51a15b22f25655e2d4b250765892a4d3480b7864`, and [PR 9466](https://github.com/Generous-Corp/pulp/pull/9466) advanced it to the PR 534 merge |
| `surface:28` | MLX | Watchdog and bounded-worker controls pass; product evidence open | Synthetic MLX N=2 pacing only (100,000 blocks, with synthetic deadline misses); no named model, CPU shadow, Pulp transport, provider identity, residency/thermal, or deadline receipt |
| `surface:29` | Dawn / shared I/O | Provider, private convolution, session correctness, and synthetic negative production-depth control pass; WaveNet performance gate open | [`2026-10-02-dawn-phase-e-direct-executables.md`](../receipts/2026-10-02-dawn-phase-e-direct-executables.md), [`2026-10-02-dawn-generalized-convolution-proof.md`](../receipts/2026-10-02-dawn-generalized-convolution-proof.md), and the audit-only synthetic [`2026-10-02-dawn-wavenet-depth-comparison.md`](../receipts/2026-10-02-dawn-wavenet-depth-comparison.md); no exact-provider or hardware depth claim; production WaveNet still admits one `inflight` block |
| `surface:30` | Spectr / Forge / Magenta | metadata-only closure | no named Forge consumer, installed Magenta model, or two-minute paced producer receipt |

## Merged-main evidence

These are the exact neural package merges present on Pulp `origin/main`
`e260a88701eb85d1a22e11952826f98a4a36671c`:

* [PR 9412](https://github.com/Generous-Corp/pulp/pull/9412), merge
  `593e48ebe46b66474382befe67b872c1654fc636`, adds private manifest admission helpers and focused installed-asset
  SHA-256/byte and fresh-process reload test controls. The focused receipt
  passes 57 assertions in 10 cases. It binds `MicroTcnModel`
  only to prove admission-to-prepare; it is not arbitrary-model or provider
  compatibility proof.
* [PR 9413](https://github.com/Generous-Corp/pulp/pull/9413), merge
  `e3afda1d5569f609fa32cbfd9fbee55b36884b31`, adds test-only portable
  allocation, lock, blocking, stale-result, and aliasing probes. Its watch
  event records 93 assertions in 19 cases and explicitly has no production
  authority or callback instrumentation effect.
* [PR 9415](https://github.com/Generous-Corp/pulp/pull/9415), merge
  `59f2aafdb99bcbfe37ca35eb58a2ee3434ee8b8c`, adds the private serialized
  NAM/TCN A1 bridge and fixture. Its focused Release receipt passes 29
  assertions in 5 cases, including 64/128-frame replay, reset, and zero
  callback allocations; broader architecture coverage remains open.
* [PR 9459](https://github.com/Generous-Corp/pulp/pull/9459) and [PR
  9466](https://github.com/Generous-Corp/pulp/pull/9466) are pointer-only Pulp
  merges. They advance the planning gitlink from `51a15b22f25655e2d4b250765892a4d3480b7864`
  to `9986b6a383e981277f71b297be216fba750b4567`, the merge of planning [PR
  534](https://github.com/danielraffel/pulp-planning/pull/534). That planning
  amendment hardens the matched-model, p50/p95/p99, percentile-bootstrap,
  provider-timestamp, pacing, and portability criteria; it does not close a
  product gate.

## Current open gates

1. **GPU pipeline:** compare production WaveNet depth 1 with depth 2 or more on
   the same host and block size. Require ordered delivery, bounded latency,
   late-result/device-loss controls, and a measured throughput or deadline
   margin improvement. The negative depth receipt records requested capacities
   2 and 8 both reaching high-water depth 1; `high_water_in_flight=2` in the
   separate session probe proves admission overlap only. Neither proves
   concurrent GPU execution.
2. **ModelStore compatibility:** PR 9412 closes focused helper/test
   installed-byte, digest, and fresh-process control cases, but installer
   integration and the product gate remain open for a complete manifest with
   tensor layout and for the install → new process → reload path to be
   revalidated as a product receipt
   (including license, runtime, sample rate, state schema, and all shipped
   assets, with the required repeated cold reloads).
3. **MLX product slice:** obtain a named model and run it through the
   model-neutral streaming contract with a CPU shadow, instance-2 isolation,
   provider identity, deadline/fallback, and Apple residency evidence. The
   existing synthetic receipt explicitly makes none of those product claims.
4. **Magenta pacing:** first establish fresh named-model provenance, then
   produce the receipt with time-to-first-audio, control latency, memory,
   cancellation, stream count, and classified underruns. Existing JSON/MD
   artifacts have divergent historical parent heads and are not a fresh
   two-minute pacing proof.
5. **Forge consumer:** register a named consumer only after validated model
   package loading, safe controls, reset/generation fencing, CPU fallback, and
   license/provenance-bearing timeline metadata.
6. **Portability:** obtain Linux and Windows real-time safety receipts for the
   same planted allocation/lock/blocking/aliasing controls.
7. **Competitive JSON receipt:** run the matched-model campaign with raw
   per-block JSON and report p50, p95, p99, max, deadline margin, quality/error,
   fallback, late-result, provider, allocation, lock, and blocking counters.
   The required 100,000 paced blocks, cold/steady repeats, and reproducible
   JSON are still absent; no broad outperformance claim is permitted.

## Resume anchors

* Source snapshot reconciled here: Pulp `origin/main`
  `e260a88701eb85d1a22e11952826f98a4a36671c`.
* Planning snapshot: `planning` gitlink
  `9986b6a383e981277f71b297be216fba750b4567` (planning PR 534 merge).
* The earlier parent worktree `/Users/danielraffel/Code/pulp-dsp-next-20261001`
  remains a historical resume anchor; do not edit it for this reconciliation.
* Do not mark the program complete while any open gate above remains without a
  receipt satisfying the corresponding plan criteria.
