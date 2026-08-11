# Creative Timeline Engine session examples

These examples cover a clip-launching session, a project that carries every
document concept at once, and a durable package/session shell. Each builds a real
immutable `Project`, edits it through typed commands in a `DocumentSession`, and
asserts against the model rather than a mock. They take no `pulp::host`
dependency, so they build wherever the document model does. The durable shell
additionally requires the project-package module.

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

## Durable project session

`pulp-timeline-project-session` is a small application-owned shell around
`PackageWriter`, `open_package()`, `FileJournal`, and `DocumentSession`:

```bash
cmake --build build --target pulp-timeline-project-session
./build/examples/timeline-session/pulp-timeline-project-session
```

`ProjectSessionShell::create()` publishes a revision-zero package, creates
`journal/session.ptlj`, restores a session against that exact journal state, and
registers a writer. Create is create-new: while holding the package writer lock,
it refuses an existing `project.json` or shell journal rather than replacing the
durable generation. Acquiring `PackageWriter` may still restore required
package-owned directories or reclaim its private interrupted-write staging, so
this is a generation-preservation guarantee rather than a promise that every
directory entry is untouched.

`submit()` allocates the writer-scoped transaction and command identities and
uses the current revision as its optimistic gate. `DocumentSession` publishes
the resulting immutable snapshot only after `FileJournal` acknowledges the
complete revision. A close does not implicitly save, but it also does not throw
away acknowledged edits: reopening validates `project.json`, opens the package's
journal, and restores the journal's newer snapshot and revision over the older
package generation.

`save()` observes one matching snapshot/revision pair, checkpoints that revision
in the journal, and then publishes the same snapshot as `project.json` through
the still-held `PackageWriter`. It reports success only for
`PublishedDurably`. If package publication fails after the checkpoint, the
checkpointed journal remains the durable recovery source; a failed save is never
reported as success. The worked executable proves both an identity-bearing edit
recovered after close without save and a later identity-targeted edit recovered
at the exact canonical bytes, revision, and allocator frontier before each saved
generation is published.

The shell is deliberately a control-thread example, not a real-time API. Its
operations perform synchronous filesystem I/O, and all member calls belong on
one control thread. Keep it alive to retain both exclusive locks, make every
other writer honor the package lock, and do not rename or replace package entries
out of band. `open()` requires a valid published package generation; because it
acquires the open-or-create `PackageWriter` first, an absent destination may gain
the package layout before open fails with `InvalidGeneration`. Package, journal,
transaction, and non-durable-publication failures remain distinct in
`ProjectSessionError`, with the underlying typed error attached when available.

This worked shell covers document-only and inline MIDI edits. It intentionally
does not expose `PackageWriter::stage_blob()`, and `submit()` rejects
`CreateAsset` before journaling. An application that admits edits
introducing media, state, artifact, or receipt references must add a controlled
blob-staging path and publish the hash-verified bytes before submitting those
references; otherwise the journal can durably hold a model revision that package
publication correctly refuses as an invalid generation.

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
