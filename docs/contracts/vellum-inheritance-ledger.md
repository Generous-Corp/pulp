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
