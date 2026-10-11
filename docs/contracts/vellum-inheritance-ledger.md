# Vellum inheritance ledger

This append-only ledger records the generic Pulp slices that Vellum must absorb
when the extraction is activated. The source remains Pulp-owned while the
projection is prepared. Every transferred-file change event gets one row with
its immutable event id, exact slice, and destination obligation.

| Event ID | Pulp commit | Transferred slice | Vellum obligation | Tests / evidence |
| --- | --- | --- | --- | --- |
| `none` (WP-11 scaffold, 2026-10-04) | this commit | none; package manifests and boundary tooling are Pulp-owned control plane | Carry the `pulp.ui.package.v1` manifest contract and boundary-lint rule when activating the extraction | `vellum-boundary-lint`; `vellum-boundary-lint-negative-contract`; `tools/scripts/gates.sh` |

## Recording rule

Append a row before rerunning the Vellum preflight whenever a change event is
owed. The event's `rationale` starts exactly with:

> design-import-refactor: generic change landed in Pulp per owner decision 2026-10-04; Vellum inherits (see inheritance ledger)

| `20261010-p5-content-registry-carve-pulp-only` | [eb5ea227991485005ff6fc9a2b12b8cb8d9d0731](https://github.com/Generous-Corp/pulp/commit/eb5ea227991485005ff6fc9a2b12b8cb8d9d0731) | `state/content-registry`, `audio/sample-bank-manifest` | Preserve the `pulp/state/content_registry.hpp` include path and content registry behavior while carrying the split target and narrow state closure. The current ownership map has no active transferred path for this Pulp-only carve, so no Vellum change event is emitted. | `pulp-state-link-closure`; `pulp-test-state`; `tools/scripts/confirm_failure.sh` |
| `20261005-design-import-refactor-20261004` | [8a003f54dd](https://github.com/Generous-Corp/pulp/commit/8a003f54dd) | `design-schema-compiler` | Preserve deterministic keyed DesignIR update application, trusted materialized-runtime canonicalization, fail-closed package-boundary scanning, and the captured clean-output corpus when activating extraction | `ctest --test-dir build -R keyed`; `node --test tools/import-design/jsx-runtime/materialized_runtime_canonicalization.test.mjs`; `python3 tools/scripts/test_vellum_boundary_lint.py`; `python3 tools/ui-build/lint/test_clean_output_lint.py` |
| `20261010-continuous-frame-registration-hook` | pending P3 branch commit | `retained-ui-kernel` | Preserve the allocation-free continuous-frame predicate and EqCurveView hover/analyzer liveness while extracting the audio widget dependency behind a runtime-kind registration hook; no Vellum source consumption | `pulp-test-continuous-frames`; `Vellum freeze`; `Vellum trusted freeze` |

## Appended rows

The rows above this heading were appended outside a table; rows from here on
use this table. Earlier rows are not edited.

| Event ID | Pulp commit | Transferred slice | Vellum obligation | Tests / evidence |
| --- | --- | --- | --- | --- |
| `20261005-design-import-refactor-20261004` (anchor correction) | [7b2d9f4fdc](https://github.com/Generous-Corp/pulp/commit/7b2d9f4fdc) | `design-schema-compiler` | Same obligation as the earlier row for this event. The earlier row's anchor `8a003f54dd` is not a commit on `main`; `7b2d9f4fdc` is the merge that landed the event file | `git merge-base --is-ancestor 7b2d9f4fdc origin/main` |
| all 86 change events from `20260725-gesture-claim-vs-candidacy` through `20261005-standalone-check-for-updates-menu` | per event: the first-parent `main` commit that added the event file | per event | Every pre-existing change event (any disposition) is mapped to the commit-anchored inheritance inventory kept with the extraction program's planning records (`vellum-move/ledger/inheritance-ledger.tsv`, mapping `vellum-move/evidence/A-governance/change-event-map.tsv`). Each event's commit set intersects at least one inventory item, and each item carries its Vellum disposition, owner, and acceptance. The authority-transition event `20260724-authority-activation-attempt-2` carries no capability change and has no inventory item | `map_change_events.py <pulp-checkout> <inheritance-ledger.tsv> <out.tsv>`: 86 covered, 1 authority transition |
