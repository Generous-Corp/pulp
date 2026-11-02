# Pinned Vellum export receipt

`vellum_freeze_check.py --receipt-mode` (implemented in
`tools/scripts/vellum_receipt_drift.py`) reads this directory to decide which
Pulp paths Vellum's newest mirror export copied.

- `<export_id>.json` is a byte-for-byte copy of Vellum's
  `provenance/mirror/receipts/<export_id>.json`. Its git blob id equals the one
  in the Vellum repository, so `git hash-object` on this file and
  `ghapp api repos/Generous-Corp/vellum/contents/provenance/mirror/receipts/<export_id>.json --jq .sha`
  must print the same value.
- `pin.json` names that copy, its SHA-256, and `frozen_paths`: exact paths, or
  directories ending in `/`, whose Pulp copy is frozen. A frozen path owes no
  inheritance decision, because it is not edited in Pulp: a change to one
  fails unless a change event it adds covers the path's transferred slice with
  disposition `framework-backport` or `emergency-exception`. `frozen_paths`
  lists exactly the paths of the transferred slices in
  `.github/vellum-ownership.json`; `test_vellum_receipt_drift.py` holds the
  two equal.

The check reads both files at the base commit of a change, so editing them
takes effect for the changes after the one that edits them. That makes an edit
here an ownership decision for every later change: review it as one. The check
only proves the copy is self-consistent and that its `pulp_sha` is real Pulp
history; it does not yet compare the copy with Vellum's blob. Re-pin by copying
the new receipt, updating `export_id`, `receipt_path`, `receipt_source` and
`receipt_sha256` together, and removing the old copy. The pin cannot be deleted
while the guard runs.

A changed exported path needs an added event that states `vellum_inherits`:
a change event or expansion-watch event that covers it, or an export event under
`.github/vellum-export-events/`:

```json
{
  "schema_version": 1,
  "event_id": "20261101-example-exported-change",
  "kind": "exported-path-change",
  "created_at": "2026-11-01T12:00:00Z",
  "receipt_export_id": "20261102-c35c0f1dafef",
  "paths": ["core/state/src/store.cpp"],
  "vellum_inherits": true,
  "rationale": "What changed, and why Vellum does or does not take it."
}
```
