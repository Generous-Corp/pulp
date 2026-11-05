# Vellum extraction freeze

This repository is preparing an independent Vellum framework extraction into
the public `Generous-Corp/vellum` repository. Pulp continues to build and ship from its existing implementation;
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
from the actual proposed merge (including renames). Its privileged companion,
`Vellum trusted freeze`, checks out literal protected `main` controls, fetches
the API-bound PR head, and constructs the candidate merge locally from those
two exact commits. It does not consume GitHub's asynchronous
`refs/pull/N/merge`; a stale synthetic ref therefore cannot block a merely
behind PR, while a source-head mismatch or merge conflict still fails closed.
A separate post-merge
workflow recomputes the compact outbox record without secrets and dispatches it
to the Vellum observatory from a one-step credential-bearing job. The
event remains permanently replayable from Pulp history even if dispatch is
temporarily unavailable. Neither workflow copies or applies source changes.
Generic features and fixes originate in Vellum after authority transfers,
except under the temporary inheritance exception described below.

If Vellum is abandoned, authority does not drift back silently. A reviewed
ownership-reversal commit must update both repositories and state whether Pulp
becomes an explicitly independent fork or the mapped implementation remains
frozen for a defined support period.

## Design-import refactor inheritance decision (2026-10-04)

Per the owner decision recorded in the design-import refactor goal, generic
design-import, UI compiler, bridge-contract, and SDK improvements land in Pulp;
Vellum inherits them later through the package manifests and inheritance ledger.
This decision does not transfer source authority.

This was a temporary exception to "generic features and fixes originate in
Vellum". While it held, a Pulp change to a transferred-slice file that carried
generic behavior used disposition `pulp-only` with a rationale that starts
exactly with:

> design-import-refactor: generic change landed in Pulp per owner decision 2026-10-04; Vellum inherits (see inheritance ledger)

and added the entry `vellum-inheritance-ledger/<event_id>.json` (see
[`vellum-inheritance-ledger/README.md`](vellum-inheritance-ledger/README.md)).
There `pulp-only` meant "landed in Pulp", not "Pulp-specific": the prefix and the
ledger entry distinguished an inherited generic change from Pulp-specific
integration work.

The exception has ended for the transferred slices. Vellum's export
`20261102-0f616a440cec` carries everything Pulp landed under it, and the change
that pinned that receipt froze Pulp's copy of every transferred-slice path (the
pin's `frozen_paths`, below). Generic work on those paths originates in Vellum.
The inheritance ledger is retired with it: its entries stay as history, and no
event owes a new one.

## Receipt mode: decisions for exported paths

Vellum's mirror is a copy of named Pulp files at one Pulp commit, recorded in an
export receipt. Until Pulp's copy of a file is frozen, a Pulp change to it is
either carried by the next re-export or deliberately left behind, and the diff
alone does not say which. `vellum_freeze_check.py --receipt-mode`, run by the
required "Vellum freeze" check from the change's own scripts and by "Vellum
trusted freeze" from the protected base's scripts, makes that a recorded
decision.

It reads the receipt copy pinned under `.github/vellum-export-receipt/` at the
change's base commit and requires an added event stating `vellum_inherits` for
every changed path the receipt exports, every path that produces a generated
export input, and every file added under a directory that holds exported
source. A change event covers its transferred slices' paths, an expansion-watch
event covers its capability families' selections, and an export event under
`.github/vellum-export-events/` covers the exact paths it lists. The pin's
README describes the event schema and how to re-pin a newer receipt.

Paths the pin lists as frozen owe no decision, because they are not edited in
Pulp: the pin freezes exactly the transferred slices' paths. A change to a
frozen path fails unless a change event it adds covers the path's slice with
disposition `framework-backport` (bringing a named Vellum commit back) or
`emergency-exception`; a `pulp-only` event does not unfreeze it. Like the rest
of the pin, `frozen_paths` is read at the base commit, so a freeze applies to
the changes after the one that adds it.
