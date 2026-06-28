# Project-import substrate (framework importers)

Framework-**agnostic** SDK substrate for *project importers*: add-on tools,
installed on demand, that read an existing audio-plugin project **read-only**
and propose a **buildable Pulp migration scaffold** + honest residual report.
The SDK owns the contract and emission; each importer (its own repo) owns
framework-specific parsing and ships zero framework source.

> **Not the design-import lane.** `tools/import-validation/` handles importing
> *designs* (Figma/Stitch/v0/Pencil/RN) and its source-contract is
> design-shaped. This `tools/import/` lane handles importing existing *plugin
> projects* and its IR is project-shaped. The two are deliberately separate and
> must not be conflated or merged.

## Vendor-agnostic rule (firm)

No vendor names in the SDK, mainline, CI, tests, or these schemas/fixtures.
A source framework and vendor are runtime **DATA** (`source.framework`,
`source.vendor_id` strings), never enumerated SDK values. The conformance test
(`test_project_import_ir_schema.py`) enforces this with a tripwire that fails if
a vendor token leaks into the schema or fixtures.

## What's here

| Path | Purpose |
|---|---|
| `schemas/project-import-ir-v0.schema.json` | The framework-agnostic project-import IR contract (versioned). Maps onto `pulp::format::PluginDescriptor` + `pulp::state::ParamInfo`; records facts, unresolved `constructs`, extraction context, and residual evidence. |
| `fixtures/project-import-ir-v0/` | Vendor-neutral example IR docs the test validates. |
| `test_project_import_ir_schema.py` | stdlib-only conformance test (schema well-formedness, fixture validation, negative cases, vendor-agnostic guard). |

Run the test: `python3 -m unittest tools.import.test_project_import_ir_schema -v`

## Design notes

The IR is shaped to be **honest by construction**: a fact the importer can't
statically resolve becomes a low-confidence value plus a first-class `constructs`
entry plus a diagnostic — never a silent drop or a guess. Key choices:

- **Parameter curves** are recorded in `source_curve` separately from the linear
  `pulp_range`, because Pulp's registered `ParamInfo.range` is linear today;
  skewed params emit linear + flagged PARTIAL until SDK shaped-range lands.
- **Param identity** keeps the source string id + version hint AND a stable
  `proposed_pulp_id` (Pulp ids are `uint32`), so preset/state migration survives.
- **`extraction_context`** records the parse environment (compile-db source,
  includes, parser version) so format-conditional or build-conditional
  extractions are explainable.
- **`constructs[]`** represents loop/factory/computed parameter sites with
  `unknown_cardinality` so emission reports "N unresolved constructs", not a
  fabricated parameter count.
- **`dsp.reachability_scope`** is explicit so portable-core confidence is never
  read from a shallow scan.

## Roadmap

- [x] `ProjectImportIR` schema + conformance test.
- [x] SPI **contract** (`detect`/`analyze`/`plan`/`emit`, JSON-over-stdio,
      version negotiation) + `PulpImportPlan` / `EmissionManifest` /
      `CompatMatrix` schemas + fixtures. (The C++ SPI *runner* is still pending.)
- [ ] `pulp import` built-in command (detects framework from the target dir even
      with no importer installed; prints the install-hint path).
- [ ] tool-registry importer fields (`category:importer`, `frameworks[]`,
      `spi_min/max`, `sdk_min/max`, `capabilities`, `health_check`, `sha256`).
- [ ] SDK materializer / report writer / clean-room-output check.
