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
