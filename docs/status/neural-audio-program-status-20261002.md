# Neural audio program status (2026-10-02)

This is the resumable status snapshot for the neural-audio workstream. The
normative plan, gate definitions, planted controls, and pass criteria remain in
[`planning/research/2026-10-01-neural-audio-mlx-first-program-plan.md`](../../planning/research/2026-10-01-neural-audio-mlx-first-program-plan.md).

There is no honest single percentage for this program: the packages have
different weights and several lanes have passing scaffolding without a product
gate. The current disposition is **two lanes substantially evidenced, four
lanes partial or open, and no full program sign-off**.

## Six-session disposition

| CMUX surface | Lane | Disposition | Evidence / remaining gate |
|---|---|---|---|
| `surface:25` | CPU oracle | A1, A1-Standard, and LSTM fixture parity pass; broader matrix gated | [`neural-nam-cpu-oracle-parity-receipt-20261002.md`](neural-nam-cpu-oracle-parity-receipt-20261002.md); A2, ConvNet, Linear, broader TCN/Keras fixtures are absent or unverified |
| `surface:26` | benchmark / RT | benchmark harness and planted Unix controls pass | provider-neutral CPU matrix, streaming contract, and CTest discovery evidence; Linux/Windows receipts remain open |
| `surface:27` | planning | ledger reconciled and committed | plan submodule currently pinned by the parent worktree; update the gitlink after any planning commit |
| `surface:28` | MLX | watchdog and bounded-worker controls pass; product evidence open | synthetic MLX N=2 pacing only (100,000 blocks, with synthetic deadline misses); the current processor facade still selects CPU for non-CPU capability flags; named model, CPU shadow, transport, residency/thermal, and deadline evidence remain open |
| `surface:29` | Dawn / shared I/O | provider, private convolution, session correctness, and synthetic negative production-depth control pass; WaveNet performance gate open | [`2026-10-02-dawn-provider-configure-and-direct-probe.md`](../receipts/2026-10-02-dawn-provider-configure-and-direct-probe.md), [`2026-10-02-dawn-session-baseline-diagnosis.md`](../receipts/2026-10-02-dawn-session-baseline-diagnosis.md), and the audit-only synthetic [`2026-10-02-dawn-wavenet-depth-comparison.md`](../receipts/2026-10-02-dawn-wavenet-depth-comparison.md); no exact-provider or hardware depth claim; production WaveNet still admits one `inflight` block |
| `surface:30` | Spectr / Forge / Magenta | metadata-only closure | no named Forge consumer, installed Magenta model, or two-minute paced producer receipt |

## Current open gates

1. **GPU pipeline:** compare production WaveNet depth 1 with depth 2 or more on
   the same host and block size. Require ordered delivery, bounded latency,
   late-result/device-loss controls, and a measured throughput or deadline
   margin improvement. The negative depth receipt records requested capacities
   2 and 8 both reaching high-water depth 1; `high_water_in_flight=2` in the
   separate session probe proves admission overlap only. Neither proves
   concurrent GPU execution.
2. **ModelStore compatibility:** the private versioned sidecar write/read
   primitive now passes its same-process round-trip and schema controls. The
   product gate still requires an installer-supplied complete manifest with
   tensor layout, installed-byte verification (the current check validates hash
   syntax only), and an install → new process → reload test that revalidates
   license, runtime, sample rate, and state schema.
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

## Resume anchors

- Parent worktree: `/Users/danielraffel/Code/pulp-dsp-next-20261001`
- Branch: `codex/dsp-next-20261001`
- Parent status commits include `49ba136fdc` (session probe correction),
  `63b9409953` (stale Dawn receipt reconciliation), and the current planning
  pointer commit after those changes.
- Before editing, run `~/.local/bin/pulp-worktree-lineage-session --plain`,
  `tools/scripts/worktree_lineage.sh show --path .`, and read `CLAUDE.md`.
- Focused validation already passing: shared-I/O session (232 assertions),
  shared-I/O pipeline (171 assertions), docs consistency, and `git diff --check`.
- Do not mark the program complete while any open gate above remains without a
  receipt that satisfies the corresponding pass criteria in the plan.
