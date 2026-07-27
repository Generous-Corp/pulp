# Creative Timeline Engine session examples

Two examples covering the non-linear half of the Creative Timeline Engine: a
clip-launching session, and a project that carries every document concept at
once. Both build a real immutable `Project`, edit it through typed commands in a
`DocumentSession`, and assert against the model rather than a mock. Unlike the
[Phase 1 examples](example-timeline-phase1.html) they take no `pulp::host`
dependency, so they build wherever the document model does.

## Clip-launching session

`pulp-timeline-launch-session` is a launcher, not an arrangement. Three tracks
are the rows and their arrangement lanes start **empty**; two scenes group the
slots into columns. The clips a slot names live on a dedicated pool track —
`Slot::clip_id` describes exactly this shape, and a sequence refuses to remove a
clip a slot still references.

```bash
cmake --build build --target pulp-timeline-launch-session
./build/examples/timeline-session/pulp-timeline-launch-session
```

Three behaviours are worth reading the source for:

**Launch quantization is a boundary set, not a delay.** A slot's grid defines
`{ phase + k * grid }`. Arming *on* a boundary fires there rather than waiting a
whole period; arming mid-bar waits for the next one. The example asserts both,
because only having the second would let an off-by-one period pass.

**Arbitration is per track.** A session is not globally in "launcher mode": each
track selects `ProviderKind::Launcher` or `ProviderKind::Arrangement` through the
compiler's `TrackCompilePolicy`. The example starts hybrid — two launcher rows
and one still playing its arrangement.

**Capturing a performance is a flatten, not a subsystem hop.** Each recorded
launch becomes an ordinary `InsertClip` against the row it played on, committed
through the same transaction path a human edit uses. The resulting arrangement is
indistinguishable from one authored by hand.

Both ends of a captured span are exact musical boundaries. A launch resolves to a
monotonic beat and `LaunchHandle` retains the beat a stop resolved to, so a
capture never inherits block granularity at either end. Captures that would
exceed the history's reserved capacity are counted by `dropped_capture_count()`
rather than dropped silently — the reservation is also what keeps the
audio-thread `push_back` allocation-free.

## Full DAW-style project

`pulp-timeline-daw-project` puts every document concept on one document:

```bash
cmake --build build --target pulp-timeline-daw-project
./build/examples/timeline-session/pulp-timeline-daw-project
```

- **A linear arrangement and a launcher on the same track.** Both the arrangement
  clip and the slot's source clip are present; arbitration decides which sounds.
- **A reusable chorus referenced three times, one diverged.**
  `build_diverge_transaction` emits the `CloneSequence` + `SetClipSequenceRef`
  pair as one transaction and mints every cloned identity from the project's own
  allocator — so the copy's id is discovered, never chosen. The other two
  references keep sharing the original.
- **A take lane with a comp** across two takes. Both takes are alternates of the
  same passage and therefore share a placement; a comp segment must lie inside
  its take's placed span.
- **Journal-backed autosave** behind a real `JournalSink`. Note that
  `validate_restore` is not optional in practice: attaching a sink calls
  `checkpoint()` and then asks the sink to prove its durable state matches the
  document. The base class refuses by default, so a sink that cannot answer
  cannot be attached — which is the contract working as intended.
- **An agent driving batch edits** through the ordinary command API, including
  the refusal case: a batch naming a nonexistent sequence is rejected and leaves
  the document revision untouched.
