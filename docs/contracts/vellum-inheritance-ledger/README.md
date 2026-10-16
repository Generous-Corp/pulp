# Vellum inheritance ledger entries

One JSON file per change event that lands generic work in Pulp under the
temporary inheritance exception (see
[`../vellum-extraction-freeze.md`](../vellum-extraction-freeze.md)). Rows
recorded before this directory existed stay in the frozen
[`../vellum-inheritance-ledger.md`](../vellum-inheritance-ledger.md).

The file is named `<event_id>.json`, after the change event under
`.github/vellum-change-events/` or `.github/vellum-expansion-watch-events/`, and contains:

```json
{
  "schema": "pulp.vellum-inheritance-entry.v1",
  "event_id": "20261011-example-generic-change",
  "slices": ["retained-ui-kernel"],
  "vellum_obligation": "What Vellum must absorb when the target flips.",
  "evidence": ["pulp-test-example", "Vellum freeze"]
}
```

`slices` must equal the event's `slices` (or its `capability_families` for an
expansion-watch event). The landing commit is not
recorded: it is the first-parent `main` commit that added the event file, which
Git already proves. Entries are append-only: add a new file and never edit or
delete an existing one.

`tools/scripts/vellum_inheritance_ledger_check.py` enforces the schema, the
match with the event, and that every event claiming Vellum inherits it has an
entry here or a row in the frozen ledger. An event claims inheritance when it
sets `"vellum_inherits": true`, or, without that field, when its rationale
starts with the design-import-refactor inheritance prefix or says "Vellum
inherits" or "inherited by Vellum". `"vellum_inherits": false` records an
explicit non-claim. Advisory wording such as "Vellum should inherit" is not a
claim. A new event whose rationale mentions both Vellum and inheritance must
set `vellum_inherits` explicitly. The checker also keeps the frozen ledger
frozen, and with `--base <ref>` it rejects any edit to or deletion of an entry
that exists at that ref. The `vellum-inheritance-ledger` ctest runs it with
`--base auto`: in GitHub Actions that is the base commit the event names
(`merge_group.base_sha` or `pull_request.base.sha`, fetched when the checkout
lacks it), and elsewhere it is `origin/main`.

## Corrections and non-entries

- `20261010-continuous-frame-registration-hook`: the frozen ledger row says the
  commit is "pending P3 branch commit". The landing commit is the first-parent
  `main` commit that added the event file, `e49c5a5390` (#10032). `c4505fe7f2`
  is the branch commit that created it. No entry file is needed, because the
  event already has its row in the frozen ledger.
- `20261010-p5-content-registry-carve-pulp-only` has no entry file on purpose.
  It carves the Pulp-kept `content` unit, touches no transferred slice, and
  emits no change event, so Vellum inherits nothing from it. The extraction
  program's planning ledger records the carve as item `C1-ui-kernel-30`,
  citing `eb5ea227991485005ff6fc9a2b12b8cb8d9d0731` and the #10036 merge
  `ecf122d1ff`.
