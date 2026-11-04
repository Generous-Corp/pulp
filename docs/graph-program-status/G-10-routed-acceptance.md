# Routed spectral acceptance audit

Status: **PARTIAL — acceptance is not established by the existing PR.**

Authority: [admission](https://github.com/danielraffel/pulp-planning/blob/3920bf5bfa24f1ec9cd030a43dac13c212ca7a76/graph-program-status/G-10.md) and [lowering design](https://github.com/danielraffel/pulp-planning/blob/148602040cf0dce7f38d9353e1c3229778468bae/graph-program-status/g10-routed-lowering-design-20261103.md).

This audit retains the design's eighteen acceptance rows in their original order.
A passing sample test or one mutation does not close a row containing several
independent requirements. The prior delivery head is
`0404350f416ed400e003eb299e7ab161304a94ba`; its receipts remain historical evidence,
not automatically proof for future source revisions. No row below is declared
fully qualified until its positive and independent production-negative controls
are paired with restored-green receipts.

Primary executable: `pulp-test-routed-spectral-ports`, from
`test/test_routed_spectral_ports.cpp` and `test/test_routed_spectral_faults.cpp`.
Installed fixture: `examples/g10-routed-consumer`. Guarded engine owner:
`pulp-test-spectral-guarded-split`.

| Row | Existing evidence | Exact evidence still needed |
|---|---|---|
| 1. Installed edges and explicit unsupported modes | Installed consumer passed; removing lowering breaks it while the old adapter passes. | Independently mutate pinned-mode and unsupported-operation refusals; retain public error assertions. |
| 2. Partition reconstruction, impulses and N+H | Fixed/irregular and maximum blocks reconstruct; prefix mutation detected. | Separate analysis-double-latency and underrun-read mutations; explicit peak location/tolerance and zero-frame callback controls. |
| 3. Dry/wet, upstream U, nested/fan-out PDC | Nested dry/fan-out waveform control. | Independently remove U propagation, allocate frame-edge audio delay, and sum alternative latency; confirm each fails. |
| 4. Frame tuples, startup and epochs | Full tuple/partition equality, startup grid, drop/reorder and old-bank-epoch mutations confirmed; prior origin-stride and transition increment controls. | Extend full tuple oracle through controller seek/replacement, rather than bank reset alone. |
| 5. Independent validation predicates | All thirteen factory geometry/config predicates independently mutation-confirmed; named generic/prepared-edit handle-kind bypass confirmed. | Remaining graph predicates: stale handles, joins, cycles, automation and candidate revalidation independently. |
| 6. Borrow lifetime, fan-out ownership and capacity | Sibling preservation and maximal 66-frame batch; F-1 mutation detected. | Writable alias and retained borrowed-pointer production mutations independently detected. |
| 7. Candidate isolation and reader lifetime | Rejected candidate/reset comparison and reader Busy backpressure. | Reprepare-active, shared-engine and premature-retirement mutations; assert continued rendering from retained state. |
| 8. Failed/cancelled candidate versus concurrent reset | Boundary-barrier and failed-offer controls; late-offer mutation detected. | Before/after-validation failure, cancellation and stale-generation schedules; shared-allocator mutation with sample/tuple/topology oracle. |
| 9. Successful offer/reset and failure without reset | Offer+seek/exhaustion controls and epoch mutation. | Candidate-side reservation, retired-object reset and double-increment mutations independently. |
| 10. Namespace uniqueness and exhaustion | Distinct controller namespaces and namespace-reuse mutation confirmed; synthetic maximum transition serial control. | Transition wrap and failed-candidate-serial-consumption mutations independently. |
| 11. Closed catalog and config revalidation | Public opaque factories; invalid gain controls. | Public construction/registration escape control, candidate revalidation bypasses and unknown internal catalog ID refusal. |
| 12. Zero-preservation qualification and runtime guard | Constant identity mutation detected by zero-input control. | Independently remove runtime zero guard and qualification oracle, retaining an outer control that detects escape. |
| 13. Bad-slot replacement and duplicate chronology | Missing-frame and unsolicited duplicate controls; duplicate guard mutation detected. | Independently omit zero replacement, replace for every duplicate and overwrite accepted payload. |
| 14. Fatal prefix, siblings and partition-invariant onset | Fatal-prefix and capacity-marker controls; cutoff mutation detected. | Executor-false/full-block-clear, lost full-edge marker and partitioned onset mutations. |
| 15. Finite guarded synthesis and transactional history | Guarded transaction suite and combined nonfinite-return mutation detected. | Inverse, OLA, quotient and partial-commit guards removed independently; retain earlier history and finite-output oracle. |
| 16. Arithmetic/resource limits | Origin bound, u64 sequence wrap prevention and 64 MiB cap each mutation-confirmed; oversized test allocation stays below 100 MiB if cap is bypassed. | Output-clock bound, allocation-failure injection and unchanged active output independently. |
| 17. Tail/drain composition | Installed prefix/tail silence and nested impulse control. | Nontrivial mask/DC/interior-bin finite tails and early-stop mutations. Codec EOF-source integration itself remains outside R admission. |
| 18. Serial dispatch and callback resource audit | Toggle controls, no fallback, C++ new probe over reset/activation/callbacks. | Parallel/anticipation eligibility mutations; independent allocation/lock/I/O planted controls. Timing claims require the separate roadmap callback matrix. |

## Additional mandatory inventories

- Enumerate every host `live_slot_` access/publication and raw snapshot dispatch;
  classify it as audio-only, controller-routed or explicitly refused. A handful
  of setter tests is not that inventory.
- Exercise exact parameter ingress and retained handles across offer, rejection,
  activation and finalization; independently plant old-slot routing, early gate
  reopen and skipped mailbox preservation. Existing MIDI preservation does not
  prove the exact-parameter path.
- Prove coherent telemetry while timing is disabled, sticky errors, publication
  drop/age, and reset versus already-published old-epoch records.
- Preserve explicit save/load/bake refusal and source separation from the offline
  codec. The installed fixture must remain at most 200 non-generated glue lines.

Notation/catalog authoring remains PARTIAL as explicitly permitted by the design.
Offline arithmetic does not establish paced realtime timing, malloc/lock/I/O
freedom, or full G-10/A3 completion. Missing proof stays open; it is not waived by
an unarmed PR, a clean advisory review, or a test-count total.

## Snapshot-access source inventory

Inspection at `0404350f416ed400e003eb299e7ab161304a94ba`, before the qualification
fixes. This is source classification, not concurrency mutation proof. The search
scope is every `live_slot_` occurrence under `core/host`, plus every
`process_snapshot_impl` caller; declarations and comments are not accesses.

| Source / access family | Classification and controlling condition |
|---|---|
| `signal_graph_runtime.hpp`: prepared-state query | Controller-aware: spectral mode OR legacy slot presence. |
| `spectral_graph.cpp`: `read_snapshot_` | Controller-routed: spectral reader/reclamation/access gates and active slot; legacy `Slot::read` only outside spectral mode. |
| `spectral_graph.cpp`: three spectral add factories | Explicitly refuse live spectral mode or a populated legacy slot before authoring mutation. |
| `signal_graph_authoring.cpp`: processor input-provider setter | Explicit refusal under mutation lock for either live representation. |
| `signal_graph.cpp`: raw gain/plugin/custom processor getters and opaque snapshot handle | Explicit spectral refusal; opaque handle acquisition holds mutation lock. Legacy pointers are not spectral handles. |
| `signal_graph.cpp`: release/unpublish/wait | Explicit spectral refusal under mutation lock. |
| `signal_graph_prepare.cpp`: invalidation | Legacy unpublish; public callers require mutation refusal. Caller inventory/mutation coverage remains required. |
| `signal_graph_prepare.cpp`: MIDI/parameter mailbox reuse during compile | Candidate-local or audio-only legacy slot. Active spectral candidate preparation has a separate graph and no active legacy slot. |
| `signal_graph_prepare.cpp`: prepare prologue/publication | Active spectral reprepare refuses. Initial spectral entry requires stopped legacy controller and no retained processing handles. Candidate spectral snapshot publishes only into its private legacy slot; initial controller publishes into spectral slot zero. |
| `signal_graph_prepared_topology_edit.cpp`: constructor, base-current check, old keepalive | Spectral branches avoid legacy live slot; candidate generation and access gate govern current-base eligibility. |
| `signal_graph_prepared_topology_edit.cpp`: rollback | Legacy lifecycle rollback only when `quiesced_lifecycles_dirty_`; spectral plugin/custom replacement refuses before lifecycle preparation. |
| `signal_graph_prepared_topology_edit.cpp`: prepared snapshot extraction | Private candidate legacy slot, not active controller slot. |
| `signal_graph_prepared_topology_edit.cpp`: reserve/publish | Spectral commit offers candidate and returns Staged before legacy adoption/publication. |
| `signal_graph_live_swap.cpp`: stage/prepare/abort | Explicit spectral refusal under mutation lock; internal preparation helpers are reached through these entries. |
| `signal_graph_live_swap.cpp`: begin swap | Qualification fix moves spectral refusal under the mutation lock. Ordinary-entry positive control and spectral refusal/output/candidate-generation control pass (28 assertions). The exact concurrent-entry schedule remains to be mutation-qualified. |
| `signal_graph_process.cpp`: ordinary dispatch | Routes spectral mode through `process_spectral_`; audio-only dispatch uses guarded snapshot. |
| `signal_graph_process.cpp`: both pinned snapshot process overloads | Explicitly clear/refuse a spectral snapshot. Retained legacy processing handles prevent initial spectral entry. |
| `spectral_graph.cpp`: render dispatch | Controller chooses/resets target at serialized boundary, then calls the existing `process_snapshot_impl`; no second executor. |

The forged-kind regression exposed a defect: generic named connect
compares supplied kinds to each other and catalog kinds to each other, but does
not compare the supplied kinds with the named catalog ports. The shared validator
now does both comparisons. The controlled pre-fix run passed two mismatched-kind
control assertions then accepted the forged pair; the repaired generic and
prepared-edit cases pass 42 assertions. Independent removal/restoration proof is
confirmed. Geometry/mask refusal controls pass 46 assertions. The following
fifteen production mutations each passed baseline, failed after a witnessed
recompile, and passed after exact source restoration at qualification commit
`cc86da1c7f453c4fedb5523fa709c07d47dd5083`:

- Named-handle kind versus catalog check; legacy swap-entry spectral refusal.
- FFT size minimum, maximum and power-of-two predicates independently.
- Hop minimum and maximum independently.
- Channel minimum and maximum independently.
- Sample-rate finite and positive predicates independently.
- Mask length, finite gain, minimum gain and maximum gain independently.

These used `tools/scripts/confirm_failure.sh --file <production source> --break
<single-predicate removal> --build-dir build --target
pulp-test-routed-spectral-ports --object <affected translation unit> --test
'./build/test/pulp-test-routed-spectral-ports <tag>' --keep-logs`. Tags were
`[spectral-handle-validation]`, `[spectral-swap-refusal]` and `[spectral-refusal]`.
The identity/mask source is `core/graph/include/pulp/graph/spectral_frame.hpp`;
each listed predicate was replaced by `false` separately. This closes those
specific factory predicates, not every graph/catalog validation obligation in
row 5 or row 11. A separate advisory review of the same committed fixes reported
no actionable finding; it is not a counted PR verdict.

The next batch, at `ac5d11c27964c0c6acb02c1024528aa0b1a11112`, independently
confirmed ten more production mutations with the same baseline/broken/restored
workflow: reuse a controller namespace; permit u64 sequence wrap; bypass retained
storage charging; drop a published frame; reorder published stamps; retain an
old bank epoch on reset; overwrite the sticky first-error record; retain its
error across reset; admit conversion while a legacy processing handle survives;
and omit the named pinned-execution diagnostic. All ten compiled, failed their
specific controls, and restored green. The complete routed executable then
passed **33,604 assertions in 30 cases**. Independent advisory review of the new
diagnostic and controls reported no actionable finding.

The untimed telemetry control also demonstrates a full publication ring retains
explicitly old-epoch records across reset, reports drops and age, and publishes
cleared error state with the fresh epoch after draining. It does not relabel old
records as current. Exact parameter ingress and early access-gate reopening are
still separate outstanding obligations.
