# Vellum inheritance ledger entries

One JSON file per change event that lands generic work in Pulp under the
temporary inheritance exception (see
[`../vellum-extraction-freeze.md`](../vellum-extraction-freeze.md)). Rows
recorded before this directory existed stay in the frozen
[`../vellum-inheritance-ledger.md`](../vellum-inheritance-ledger.md).

The file is named `<event_id>.json`, after the change event under
`.github/vellum-change-events/`, and contains:

```json
{
  "schema": "pulp.vellum-inheritance-entry.v1",
  "event_id": "20261011-example-generic-change",
  "slices": ["retained-ui-kernel"],
  "vellum_obligation": "What Vellum must absorb when the target flips.",
  "evidence": ["pulp-test-example", "Vellum freeze"]
}
```

`slices` must equal the change event's `slices`. The landing commit is not
recorded: it is the first-parent `main` commit that added the event file, which
Git already proves. Entries are append-only: add a new file and never edit or
delete an existing one.

`tools/scripts/vellum_inheritance_ledger_check.py` enforces the schema, the
match with the change event, and that every change event carrying the
inheritance prefix has an entry here or a row in the frozen ledger. It also
keeps the frozen ledger frozen.
