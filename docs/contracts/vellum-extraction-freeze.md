# Vellum extraction freeze

This repository is preparing an independent, private Vellum framework
extraction. Pulp continues to build and ship from its existing implementation;
Pulp does not consume Vellum during this experiment.

The remaining operational gates are tracked in
[`vellum-activation-readiness.md`](vellum-activation-readiness.md). Landing this
prepared control plane is not source-authority activation.

`.github/vellum-ownership.json` is Pulp's pinned projection of the source
authority map. While its activation state is `prepared`, it transfers no
authority. The transfer becomes operational only when a reviewed change sets
the state to `active`, references an immutable schema-v2 Vellum authority
record, and lists the exact transferred slices. The active projection is
generated from the one append-only `authority-transition` event:

```sh
python3 tools/scripts/generate_vellum_ownership_projection.py \
  --activation-event \
  .github/vellum-change-events/20260723-authority-activation.json
```

The generator copies the event ID, record commit and path, approver, and
timestamp into the activation metadata and each transferred slice. The freeze
check independently binds those values to the Vellum record, a signed annotated
`refs/tags/authority/*` tag targeting that exact commit, its published immutable
GitHub Release, exact App-bound checks, the prepared Pulp candidate blob, and
the unchanged selected source paths. The event and generated projection must
land in the same Pulp commit; neither one is activation evidence on its own.

After activation, a pull request that touches a transferred path must add one
or more immutable records under `.github/vellum-change-events/`. The union of
their declared slices must exactly match the slices affected by the actual
merge result:

```json
{
  "schema_version": 1,
  "event_id": "20260722-audio-adapter-only",
  "kind": "change",
  "created_at": "2026-07-22T20:00:00Z",
  "slices": ["retained-ui-kernel"],
  "rationale": "Audio-specific integration only.",
  "tests": ["test_name"],
  "disposition": "pulp-only"
}
```

Allowed dispositions are:

- `pulp-only`: Pulp-specific audio, plug-in, host, or legacy integration work;
- `framework-backport`: a deliberate backport of a named immutable Vellum
  commit, with destination tests;
- `emergency-exception`: a time-bounded exception with an owner, expiry, and
  follow-up. It must still be live when the event is committed and may last no
  more than fourteen days from its declared creation time.

The required `Vellum freeze` check verifies the append-only declaration,
validates old-to-new ownership-map transitions, and determines affected slices
from the actual proposed merge (including renames). A separate post-merge
workflow recomputes the compact outbox record without secrets and dispatches it
to the private Vellum observatory from a one-step credential-bearing job. The
event remains permanently replayable from Pulp history even if dispatch is
temporarily unavailable. Neither workflow copies or applies source changes.
Generic features and fixes originate in Vellum after authority transfers.

If Vellum is abandoned, authority does not drift back silently. A reviewed
ownership-reversal commit must update both repositories and state whether Pulp
becomes an explicitly independent fork or the mapped implementation remains
frozen for a defined support period.
