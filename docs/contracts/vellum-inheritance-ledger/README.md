# Vellum inheritance ledger entries

**Retired.** Vellum's export `20261102-c35c0f1dafef` carries everything these
entries promised, and Pulp's copy of the transferred slices is frozen. No event
owes an entry any more: whether Vellum inherits a change to an exported path is
recorded by `vellum_freeze_check.py --receipt-mode` (see
[`../vellum-extraction-freeze.md`](../vellum-extraction-freeze.md)). The entries
stay as history, still checked against their events and still append-only.

One JSON file per change event that landed generic work in Pulp under the
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

`tools/scripts/vellum_inheritance_ledger_check.py` enforces the schema and the
match with the event for every existing entry. It no longer requires an entry
for any event. A new event whose rationale mentions both Vellum and
inheritance must still set `vellum_inherits` explicitly. The checker also keeps the frozen ledger
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

## Relabelled events

An event that edits a file Vellum's mirror exports is inherited by the next
re-export whatever its rationale says. Seven change events filed after the
`20261025-46cee5e4e6e2` export call their change Pulp-only, but each edits
exported files, so each now has an entry here recording what Vellum takes. The
events themselves stay as written; the entry is the correction:

- `20261022-spectr-footprint-20261022`
- `20261022-lite-editor-seams-20261022`: Vellum inherits the source edits; the
  second `PULP_LITE_EDITOR` compile of the four lite-editor sources, and the
  macOS plug-in target that builds it, stay in Pulp for now.
- `20261024-attack-surface-daw-leftovers-20261023`
- `20261025-attack-surface-daw-leftovers-canvas-mask`
- `20261025-importer-synthetic-bold-20261025`
- `20261026-importer-final-review-20261026`
- `20261024-importer-anchor-collision-20261024`
