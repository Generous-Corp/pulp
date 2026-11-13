# G-10 routed spectral acceptance rows: qualification pass, 2026-11-13

Evidence for the routed spectral acceptance audit
(`docs/graph-program-status/G-10-routed-acceptance.md`, unchanged here). It
covers the rows that audit still listed as open (5, 7, 9, 13 and 16), the
host `live_slot_` access inventory, and notation coverage for spectral ports.
Base: `origin/main` `cf7f9ec18f3c`. Build: Release, Ninja, examples OFF.

Every production mutation below was run through
`tools/scripts/confirm_failure.sh --file <source> --break "perl -0pi <script>"
--build-dir build --target <target> --test "<binary> <tag>"`. "CONFIRMED" means
the tag passed on clean source, failed once the witnessed recompile was in,
and passed again after the source was restored through git. Each mutation
changes exactly one predicate or statement.

## Per-row outcome

| Row | Obligation still open before this pass | Outcome |
|---|---|---|
| 5 | Stale handles, joins, cycles, automation and candidate revalidation, each mutated independently | Stale-handle, automation, dense-modulation and preparation-time join predicates are now CONFIRMED (5 mutations). Authoring join and cycle were already confirmed. A forged stage-to-stage cycle seen only at preparation has no confirmable mutation, see below. |
| 7 | Destructive active reprepare and shared-engine mutations | Both CONFIRMED with a new reprepare control and the existing candidate-isolation control. |
| 9 (and row 4's remaining item) | Full controller tuple oracle through seek and replacement | New oracle; two production mutations CONFIRMED. |
| 13 | Replace for every duplicate; overwrite accepted payload | New control; both mutations CONFIRMED. |
| 16 | Bounded allocation-failure injection with unchanged active output | **STOP**: needs a design decision, see below. |

### Row 5: independent graph predicates

New tests:

- `Spectral handles from a cleared authoring incarnation are refused`
  (`[spectral-stale-handle]`, `test/test_routed_spectral_ports.cpp`). It clears
  the graph so node ids restart, advances the authoring generation and
  recreates same-named ports, so only the incarnation separates the retained
  handles from the current ones. Both stale-source and stale-destination are
  checked through generic connect and prepared-edit connect. The current
  handles still connect.
- `Candidate preparation revalidates spectral joins independently of authoring`
  (`[spectral-join-revalidation]`, `test/test_routed_spectral_faults.cpp`). The
  authoring layer refuses the join, then the forged connection list goes
  straight to `SpectralRuntimeBank::prepare`, the validator every candidate
  goes through. An orphan analysis is also refused, and the single-edge graph
  is accepted.
- The existing `Spectral frame edges reject automation and dense modulation`
  (`[automation]`) now has mutation receipts.

| Mutation | Source | Verdict |
|---|---|---|
| Drop source-handle incarnation comparison | `core/host/src/signal_graph_authoring.cpp:962` | CONFIRMED |
| Drop destination-handle incarnation comparison | `core/host/src/signal_graph_authoring.cpp:963` | CONFIRMED |
| Drop `c.automation` from the spectral-edge predicate | `core/host/src/spectral_graph.cpp:327` | CONFIRMED |
| Drop `c.audio_rate_modulation` from the spectral-edge predicate | `core/host/src/spectral_graph.cpp:328` | CONFIRMED |
| Drop `destination->input` (join) from the preparation edge predicate | `core/host/src/spectral_graph.cpp:326` | CONFIRMED |

Remaining in row 5: a forged stage cycle (stage A to B to A) reaches
preparation only as an ancestor walk with no analysis root. Its guard
(`++steps > nodes.size()`, `core/host/src/spectral_graph.cpp:368`) turns a
mutation into a non-terminating walk rather than a wrong answer, so
`confirm_failure.sh` can only time out on it. The authoring-layer cycle
refusal is the confirmed control. Qualifying the preparation-only cycle needs
a bounded-walk design, for example a visited set that fails closed, before a
mutation can produce an observable result.

### Row 7: candidate isolation and reader lifetime

New test: `An active spectral controller refuses reprepare without disturbing
rendering` (`[spectral-reprepare-refusal]`). `prepare()` on a live controller is
refused at the original and at a different block size, the transition serial
does not move, and twelve following blocks match an untouched control
bit-for-bit.

| Mutation | Source | Control | Verdict |
|---|---|---|---|
| Remove the live-controller refusal at the top of `prepare()` | `core/host/src/signal_graph_prepare.cpp:1599-1602` | `[spectral-reprepare-refusal]` | CONFIRMED |
| Candidate compilation also resets the active bank's engines and frame state (shared engine) | inserted after `core/host/src/signal_graph_prepare.cpp:444` | existing `[spectral-candidate-isolation]` | CONFIRMED |

The shared-engine mutation keeps the active epoch, so only shared engine state
is disturbed. The existing prepared-then-cancelled disposition detects it
through the waveform comparison.

### Rows 9 and 4: controller tuple oracle

New test: `Controller frame tuples restart exactly at seek and replacement
boundaries` (`[spectral-controller-tuples]`). It renders a real `SignalGraph`
controller across these steps: seek, steady blocks, a rejected offer (no
restart), an accepted replacement, a reset request, and an offer together with
a seek in the same callback (exactly one restart). After every callback it
reads the active analysis batch through the controller's own reader. Each
published stamp must equal independently computed
`{epoch, k, first + k*H, first + k*H + N}`. The epoch's low half must equal the
transition serial, which the test advances itself and checks against the
controller. The high half must stay one namespace. Each callback must publish
exactly the frames whose completion its end reached, no earlier and no later.
26 frames in total.

| Mutation | Source | Verdict |
|---|---|---|
| Seek no longer restarts the bank (drop `reset` from the restart predicate) | `core/host/src/spectral_graph.cpp:178` | CONFIRMED |
| Bank reset keeps the old sequence counter | `core/host/src/spectral_graph_internal.hpp:289` | CONFIRMED |

Not attempted: dropping `activate` from the same predicate is an equivalent
mutant. A fresh candidate bank always differs from `spectral_render_target_`,
so activation restarts anyway.

### Row 13: duplicate chronology

New test: `A duplicated record neither overwrites the accepted payload nor adds
a replacement` (`[spectral-duplicate-payload]`). It uses non-periodic input, so
every frame payload is distinct. Record 3 repeats record 2's descriptor and
carries record 5's valid payload. The stage must publish exactly 8 frames.
Frame 2 must keep the accepted payload, frame 3 must be one zero replacement,
and the other frames must be unchanged. The error count must be exactly 2, with
InvalidFrame first.

| Mutation | Source | Verdict |
|---|---|---|
| Consume every record for the current slot, last valid payload wins (overwrite accepted payload) | `core/host/src/spectral_graph_internal.hpp:193-194` | CONFIRMED |
| Each stale duplicate publishes its own zero replacement | `core/host/src/spectral_graph_internal.hpp:180` | CONFIRMED |

The "omit zero replacement" mutation was already confirmed in the prior batch.

### Row 16: STOP, allocation-failure injection

Candidate preparation already turns `std::bad_alloc` into
`Result::PreflightFailed` (`core/host/src/signal_graph_prepared_topology_edit.cpp:3209`).
The routed test binary cannot inject a failure, though. Its global
`operator new` belongs to the exported `Pulp::rt-allocation-probe`
(`tools/testing/rt_allocation_probe/src/rt_allocation_probe.cpp:124`), which
records allocations but cannot fail them, and a second replacement in the same
binary is an ODR violation. A bounded sweep (fail the k-th allocation of
`candidate->prepare()` for every k, then compare the active output against a
control) needs one of two things:

- a fail-after-N knob added to that SDK-exported probe, or
- a separate test executable with its own allocator.

There is also a question to settle first. Any allocation reached inside a
`noexcept` frame during preparation would terminate rather than fail, so the
sweep can expose a terminate-class defect, and the probe API change needs an
owner decision. Row 16 stays open.

## Defect found and fixed: graph event binding under a live controller

`SignalGraph::connect_graph_event` (`core/host/src/trigger_graph.cpp:358`) was the
one authoring entry that reaches `invalidate_live_locked_()` without refusing a
live spectral controller. With two event-capable custom nodes in a live
spectral graph, the binding was **accepted** (`0 == Accepted`). The edge was
recorded, and `latency_samples()` fell from 320 to 0 while the controller kept
rendering. A pre-fix run showed all three failures.

- Fix: `refuse_spectral_mutation_()` under the mutation lock, returning
  `GraphEventBindingResult::Mode`.
- Control: `A live spectral graph refuses graph event binding without changing
  its controller` (`[spectral-graph-event-refusal]`). It checks the refusal
  code, that no edge was recorded, `UnsupportedOperation`, unchanged latency,
  and bit-identical output against a control for twelve blocks.
- Mutation (remove the new refusal): CONFIRMED.

## `live_slot_` access inventory

Scope: every `live_slot_` occurrence under `core/host` (85 lines, from
`git grep -n live_slot_ -- core/host`), plus every `process_snapshot_impl`
caller. Comment-only lines are omitted.

The invariant that makes most sites safe: **the two live representations are
mutually exclusive.**

- Spectral entry requires `!live_slot_.has_value()`
  (`signal_graph_prepare.cpp:1606`).
- Spectral publication goes to `spectral_slots_`, never to `live_slot_`
  (`signal_graph_prepare.cpp:1797-1805`).
- Every legacy publisher refuses spectral mode first.

So while a spectral controller is live, `live_slot_` is empty, and a raw
`live()` read yields null.

The thread column uses three values:

- **ctl**: control thread, normally under `GraphMutationLock`.
- **ctl-any**: control-thread API that does not take the lock.
- **audio**: realtime callback.

| Site | Thread | Classification and why it is safe |
|---|---|---|
| `signal_graph_runtime.hpp:720` `is_prepared` | ctl-any | Spectral-aware: `spectral_mode_` OR legacy slot presence. |
| `signal_graph.cpp:1139-1200` six raw `live_*` getters (gain atomic, plugin slot, custom, transport, event and param processors) | ctl-any | Each calls `refuse_spectral_mutation_()` first. Exclusivity means a racing spectral entry still leaves `live()` null. These are legacy-only pointers, never spectral handles. |
| `signal_graph.cpp:1208` `live_snapshot_handle_locked_` | ctl | Asserts the lock, then refuses spectral. |
| `signal_graph.cpp:1397-1398` `release` unpublish and wait | ctl | Under the lock, after `refuse_spectral_mutation_()` (`:1391`). |
| `signal_graph_authoring.cpp:34` `has_isolated_plugins_locked_` | ctl | Read-only under the lock. In spectral mode it reads an empty slot, and its callers (`begin_swap_edit`, isolated `prepare`) refuse spectral separately. |
| `signal_graph_authoring.cpp:292` processor input-provider setter | ctl | Refuses for either live representation under the lock. |
| `signal_graph_live_swap.cpp:93-195` `stage_plugin_replacement` | ctl | Under the lock, after `refuse_spectral_mutation_()` (`:80`). |
| `signal_graph_live_swap.cpp:276-286` `warm_staged_slot_locked_` | ctl | Only reached from `stage_plugin_replacement` (`:196`), which refused already. |
| `signal_graph_live_swap.cpp:502-774` `prepare_swap` reads and publication | ctl | Under the lock, after refusal (`:487`). `begin_swap_edit` (`:445`) and `abort_swap_edit` (`:796`) also refuse. Spectral swap refusal is mutation-confirmed (prior batch). |
| `signal_graph_prepare.cpp:126` `invalidate_live_locked_` unpublish | ctl | Every public caller refuses spectral first; see the caller sweep below. |
| `signal_graph_prepare.cpp:631, 656, 821` mailbox and equaliser reuse in `compile_` | ctl | Only meaningful with a legacy live slot. A spectral candidate compiles in its own graph, whose `live_slot_` is private and empty; an active spectral owner has an empty slot. |
| `signal_graph_prepare.cpp:1588` isolated-plugin precondition | ctl | Read only, refuses when a slot is live. |
| `signal_graph_prepare.cpp:1606, 1616, 1807` `prepare` | ctl | Live spectral refuses at `:1599` (now mutation-confirmed, row 7). Spectral entry requires an empty legacy slot. A spectral compile publishes to `spectral_slots_[0]`; only a non-spectral compile or a prepared-edit candidate publishes to `live_slot_`. |
| `signal_graph_prepared_topology_edit.cpp:865, 2265` base and old keepalive capture | ctl | Spectral mode captures null instead of the legacy slot. |
| `signal_graph_prepared_topology_edit.cpp:1359` `base_is_current_locked_` | ctl | Spectral branch uses the access gate; the legacy branch compares the slot. |
| `signal_graph_prepared_topology_edit.cpp:1473-1476` lifecycle rollback | ctl | Only when `quiesced_lifecycles_dirty_`. Spectral refuses plugin and custom replacement before lifecycle preparation (`:2247-2257`), so the spectral path never dirties lifecycles. |
| `signal_graph_prepared_topology_edit.cpp:2130` `ExecutionSnapshot::export_prepared_description` | ctl | Under the owner lock. A spectral owner's slot is empty, so it refuses with `SnapshotChanged`. |
| `signal_graph_prepared_topology_edit.cpp:3345` prepared snapshot extraction | ctl | Reads the candidate's private slot, not the owner's. |
| `signal_graph_prepared_topology_edit.cpp:3421, 3472` reserve and publish | ctl | `prepare_publish` is skipped in spectral mode (`:3420`). Spectral commit offers the candidate and returns `Staged` at `:3446-3454`, before legacy publication. |
| `spectral_graph.cpp:23` `read_snapshot_` | any | The controller-routed gateway. Legacy `Slot::read` is used only outside spectral mode; spectral readers are counted, re-check reclamation, and close under `TransitionPending` for access reads. |
| `spectral_graph.cpp:442, 471, 485, 501` the four spectral factories | ctl | Refuse live spectral mode or a populated legacy slot before mutating, under the lock. |
| `trigger_graph.cpp:619-679` trigger plan prepare and install | ctl | `prepare_trigger_plan` refuses spectral mode and spectral nodes (`:475-477`). Install requires `trigger_controller_`, which `ensure_trigger_controller_` (`:246`) never creates for a spectral graph. |
| `trigger_graph.cpp:724-1062` trigger reset, host-event ingress, counters, trace, process | ctl / audio | Each returns early without `trigger_controller_` (absent in spectral graphs) or pins `live_slot_.read()` and null-checks it. Spectral prepare refuses existing trigger state (`signal_graph_prepare.cpp:1595-1598`), so the modes cannot coexist. |
| `trigger_graph.cpp:1071, 1086, 1090` trigger test seams | ctl (tests) | Private test hooks used only by trigger tests. They dereference the pin unchecked, which is unreachable from a spectral graph. |
| `signal_graph_process.cpp:49, 55` `process_impl` | audio | Spectral mode dispatches to `process_spectral_` before any slot read; the legacy path uses a guarded pin. |
| `signal_graph_process.cpp:582, 593` pinned `ExecutionSnapshot::process` | audio | Explicitly clears and refuses a spectral snapshot. |
| `spectral_graph.cpp:193` render dispatch | audio | Controller-chosen target at the serialized boundary. One executor (`process_snapshot_impl`); never reads `live_slot_`. |
| `trigger_graph.cpp:1028` trigger process dispatch | audio | Only with a trigger controller and pinned trigger snapshot; never spectral. |

**Caller sweep for `invalidate_live_locked_()`** (51 call lines under `core/host/src`):

- **Graph and authoring callers.** `graph_serializer.cpp`, `signal_graph.cpp`,
  `signal_graph_authoring.cpp`, `signal_graph_live_swap.cpp`, `set_limits`,
  `clear`, `set_custom_node_state` and the spectral factories all refuse spectral
  mode under the lock before invalidating.
- **Prepared-edit callers.** The `PreparedTopologyEdit::*` callers act on the
  candidate graph, not the owner.
- **Trigger callers.** The trigger `add_*` calls go through
  `ensure_trigger_controller_` (refuses). `connect_trigger` needs a trigger
  producer, which cannot exist in a spectral graph.
- **The exception was `connect_graph_event`.** It had no refusal; that is the
  defect fixed above.

**Raw snapshot readers.** The 39 `read_snapshot_` callers (prepared stats,
latency, edge probes, telemetry, mailboxes, MIDI extraction, `set_node_gain`,
anticipation pump) all reach a snapshot through the gateway. This routing is
source inspection; individual mailbox and probe semantics under transition
remain as stated in the acceptance audit and are not re-qualified here.

**Inventory result:** one unsafe site, `connect_graph_event`, now fixed with
its own test and mutation. No other `live_slot_` access can observe or mutate
a live spectral controller.

## Notation coverage for spectral ports

Notation deliberately has no spectral spelling. Spectral nodes and frame edges
are explicit refusals in both directions:

- `format_graph_patch` refuses at `core/host/src/graph_patch.cpp:682` (nodes)
  and `:717` (edges).
- Strict JSON loading refuses at `:823` (`spectral*` node types) and `:873`
  (the spectral edge flag).

Before this pass no notation test referenced spectral. New control:
`Spectral nodes and frame edges are explicit notation refusals`
(`[spectral-notation]`, `test/test_graph_patch.cpp`).

| Mutation | Source | Verdict |
|---|---|---|
| Drop spectral-node refusal in formatting | `graph_patch.cpp:682` | see receipts table |
| Drop `spectral*` node-type refusal in strict load | `graph_patch.cpp:823` | see receipts table |
| Drop spectral edge-flag refusal in strict load | `graph_patch.cpp:873` | see receipts table |

The formatter's edge predicate (`:717`) is unreachable on its own: any spectral
edge implies spectral endpoints, which are refused first.

Remaining gap: a real notation spelling for spectral identity, stages and
frame edges is a catalog and notation contract change, not a small slice.
Notation authoring stays PARTIAL, as the admission permits.

## Receipts for the follow-up mutations

| Mutation | Verdict |
|---|---|
| Remove the `connect_graph_event` spectral refusal (`trigger_graph.cpp`) | CONFIRMED |
| Notation: spectral-node format refusal (`graph_patch.cpp:682`) | CONFIRMED |
| Notation: `spectral*` type load refusal (`graph_patch.cpp:823`) | NOT CONFIRMED: the legacy loader independently rejects any `spectral*` type, because the serializer has no spectral type spelling (`graph_serializer.cpp:359-369`). The control still sees `UnsupportedContract`, so this predicate is defense in depth and the mutation is equivalent. |
| Notation: spectral edge-flag load refusal (`graph_patch.cpp:873`) | CONFIRMED |

## Suite results

See the closing section, written after the full runs.
