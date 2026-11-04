# Routed spectral ports

Status: **PARTIAL — executable routed runtime and focused controls; full qualification remains open.**

Authority: [admission](https://github.com/danielraffel/pulp-planning/blob/3920bf5bfa24f1ec9cd030a43dac13c212ca7a76/graph-program-status/G-10.md)
and [reviewed lowering contract](https://github.com/danielraffel/pulp-planning/blob/148602040cf0dce7f38d9353e1c3229778468bae/graph-program-status/g10-routed-lowering-design-20261103.md).

The implementation preserves one SignalGraph execution plan and the existing
STFT/WOLA engine. No callback codec, second execution runtime, unrestricted
native stage registration, saved graph restoration, or full G-10/A3 completion
is authorized by this work.

## Delivery sequence

1. Guarded split analysis/synthesis in the existing engine: transactional
   inverse/OLA commit, finite output, bounded storage and checked clocks.
2. Named analysis, closed identity/mask stages, synthesis and spectral edges:
   typed bindings in the existing serial plan, graph-owned bounded batches,
   identity/cardinality/reachability validation, N+H PDC and branch-local errors.
3. Render-owned epoch transitions and two-phase candidate handoff: stopped
   entry, access routing/refusals, mailbox preservation, reader retirement,
   exact reset/seek behavior and namespace exhaustion.
4. Installed public-consumer execution, adversarial controls, production
   mutations, callback-resource proof, governed builds and repository gates.

The guarded engine is a prerequisite, not proof of routed ports. It must not
be reported as the admitted first slice or as an independently connected
SignalGraph consumer. A safe PR boundary may separate it from graph integration;
the remaining obligations below remain open until independently evidenced.

## Admitted contract checklist

- Published typed named handles with stale/foreign rejection and repeated
  validation through generic and prepared-edit entry points.
- Closed immutable catalog factories; invalid configuration and unknown IDs
  reject; constant-output production mutations fail qualification and zero guard.
- One serial node-major walk, no anticipation/workers or legacy fallback.
- F=ceil(B/H), B<=4096, disjoint fan-out storage, actual retained spectral
  allocation <=64 MiB including engine/provider/scratch/diagnostic overhead.
- Spectral latency propagation without audio delay storage; dry/wet alignment,
  upstream delay, nesting and fan-out proof.
- Frame chronology, single replacement for invalid scheduled slots, duplicate
  rejection, independent fatal markers and exact branch-local cutoff/prefix.
- Coherent fixed telemetry records even with timing disabled; sticky errors,
  publication drops/age and epoch reset semantics.
- Single render transition writer, process-lifetime namespaces, fresh epochs,
  genuine exhaustion behavior and checked chronology at synthetic limits.
- Three-slot offer/acknowledgment/finalization with no audio allocation,
  destruction, lifecycle callback, lock, retry or authoring mutation.
- Complete live-slot/raw-dispatch inventory; all snapshot-bound operations
  controller-routed or explicitly refused; no live pinned spectral execution.
- TransitionPending/Busy/PendingMailboxData refusal and exactly-once mailbox
  preservation, including exact parameters and retained metadata readers.
- Failed/cancelled/stale candidate versus active-only reset oracle using
  production barriers; successful offer plus reset advances once.
- Tail drain L+N-1 and nested composition; runtime-only save/load/bake refusal.
- Clean installed SDK consumer with <=200 glue lines, independently connected
  identity/mask stages, sibling synthesis and dry mixing; ctest, documentation
  and notation coverage (missing notation remains PARTIAL).
- Every acceptance row: passing control, production mutation detected by
  confirm_failure, restored green; callback matrix qualification is separate.

## Focused evidence and remaining qualification

The routed focused suite passed 32,836 assertions across 21 cases after the
review fixes and deadline-wait cleanup. The guarded engine suite passed
28,120 assertions across three cases. The existing single-node adapter passed
118 assertions across four cases, and 416 related CTests passed. Final rerun
results are recorded with the delivery head in the PR.

The standalone installed consumer links only `Pulp::host`, independently connects
two stages, a sibling synthesis branch and dry mixing, and checks fixed/irregular
partitions, reconstruction below -100 dB, exact prefix/tail silence, and invalid
joins/cycles/masks/hops/numeric frame edges. Its installed-package CTest passed after a fresh SDK installation. The 92-line
consumer executable is 2,573,088 bytes with SHA-256
`64e26472fa54f0329eee94d8e42f8ed7daff683a1be8b0c50315a092e70dc286`.
The local source-install receipt is development-only, not distribution eligible;
its source SHA is `be527a11b3d5863ba7923bb96bc7ec50b159ed48` and its dirty flag
reflects this status document being updated.
This is public runtime evidence; it does not establish notation authoring.

Production mutations were detected with passing baselines and restored green:
origin stride, frame capacity, N+H prefix, fatal cutoff, constant-output identity,
epoch increment, late offer recapture, sample-rate validation, mask validation,
spectral lowering, chronology bound, mailbox refusal, dry-history reset, and
trailing duplicate rejection. The initial capacity mutation was NOT CONFIRMED;
adding a three-sample prime followed by B=4096/H=63 exposed all 66 required
slots, and the rerun detected the mutation. These are targeted controls, not a
claim that every admission row has its own mutation receipt. The guarded-engine
transaction mutation and installed-consumer lowering mutation are also confirmed
with restored green at the final runtime revision. Independent audio feedback,
active-controller load refusal, and live custom-state refusal each have
confirmed production mutations.

The independent advisory review identified a live processor-provider setter
mutation and a stale prepared-block-size query after activation. Both were fixed
with regression tests. Subsequent review found an overbroad audio-feedback
refusal, mutation eligibility checked before locking, and silent load success
after spectral mutation refusal, and a custom-state setter bypass; these were
also corrected and tested.
The final independent branch review exited clean with no accepted/actionable
findings. The governed full `pulp build --all` passed. The routed delivery is stacked on inventory prerequisite
[PR 11498](https://github.com/Generous-Corp/pulp/pull/11498), exact head
`b995a005a46f62d2b597939d94cb60a47a43cab5`. It fixes the normal inventory writer
when a still-registered executable loses its last dependency. It is unmerged
and lacks counted approval; this branch never approves or merges that work.
The inventory was regenerated without deleting valid entries. Gate receipts and
any host-policy exclusions are reported in the delivery PR; they do not upgrade
the qualification scope described below.

### Exact remaining obligations

- Complete the admission-row evidence matrix, including any controls absent
  from the focused tests; the checklist above remains the authority for scope.
- Qualify the 44.1/48/96 kHz × 32/64/128/256 callback matrix and 100,000 paced
  calls where realtime qualification is claimed. No such claim is made here.
- Extend callback-resource evidence beyond the C++ `new` probe (activation,
  reset and 300 callbacks): it is not a malloc interposer, lock/I/O audit or
  timing receipt.
- Add notation/catalog authoring coverage. Runtime infrastructure registry rows
  deliberately do not claim a generator-facing spectral capability.
- Preserve runtime-only save/load/bake refusal; offline codec work belongs to
  its separate admitted slice and never executes in the callback.

G-10 and A3 remain PARTIAL. These qualification gaps are not implemented features
or passing evidence, and require a follow-up contract/evidence packet.
