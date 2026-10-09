# Forge product-neutral boundary review

Date: 2026-10-07

This is an architectural review and migration proposal. It does not rename a
public header, change a stable node ID, alter the installed catalog, or change
Forge Modular/Sequencer behavior.

## Finding

The current Pulp surface combines two different layers:

1. **Reusable Pulp capability machinery**
   - `CustomNodeType` factories and stable `type_id` values.
   - Node-local parameter IDs and baked range/default contracts.
   - Generic node input/output arity, lifecycle callbacks, and sample-region
     runtime contracts, independent of Forge graph authoring policy.
   - Realization construction (a finite set of construction-time variants).
   - Descriptor data structures for parameter kind/curve, choices, units,
     descriptions, axes, and realizations.
   - Validation and serialization primitives that can describe any host-facing
     node catalog.

2. **Forge's product catalog and projection policy**
   - The `forge_*_catalog.hpp` family list and the joined Forge catalog.
   - Forge-facing labels/descriptions and the curated family membership.
   - The `pulp.forge-catalog.v1` JSON envelope and `forge-catalog.json` install
     artifact.
   - The `pulp forge catalog export` CLI and `pulp_install_forge_catalog()`.
   - Forge's interpretation of rows: named/logical graph ports and wiring,
     mappings to SDK factories, macro eligibility, gain bounds, prompt
     vocabulary, and product selection.

The first layer is product-neutral. The second is Forge policy. The present
names make the boundary look more product-specific than it should be, and the
current Pulp headers expose both layers together.

## Evidence from consumers

The Forge repository consumes Pulp family headers for stable IDs and factories
in generated effect rows and graph loading, including:

- `/Users/danielraffel/Code/forge/include/forge/gen/effect_node_registry_core_rows.hpp`
- `/Users/danielraffel/Code/forge/include/forge/gen/effect_node_registry_round2_rows.hpp`
- `/Users/danielraffel/Code/forge/include/forge/gen/effect_node_registry_spectral_dynamics_rows.hpp`
- `/Users/danielraffel/Code/forge/src/gen/graph_loader.cpp`
- `/Users/danielraffel/Code/forge/src/gen/live_graph_processor.cpp`

Forge also loads the installed snapshot at
`share/pulp/forge-catalog.json` and validates its schema, node keys, realization
IDs, ranges, defaults, and choices. Forge-specific policy remains in
`forge::gen`, including ports, factory routing, macro policy, conservative gain
bounds, and product registries.

The Pulp side currently exposes the mixed layer through:

- `core/host/include/pulp/host/forge_param_descriptor.hpp`
- `core/host/include/pulp/host/forge_catalog_export.hpp`
- `core/host/include/pulp/host/forge_catalog_json.hpp`
- `core/host/include/pulp/host/forge_catalog_index.hpp`
- the `core/host/include/pulp/host/forge_*_catalog.hpp` family headers
- `tools/cli/cmd_forge.cpp`
- `tools/cmake/PulpForgeCatalogInstall.cmake`

## Recommended boundary

Keep the DSP implementations and generic host registration primitives in Pulp,
but introduce product-neutral names for the descriptor/export layer. A clean
future Pulp vocabulary is:

- `pulp::host::ParamKind`, `ParamCurve`, `ParamChoice`, and
  `ParamDescriptor`;
- `pulp::host::AxisValue`, `RealizationAxis`, `RealizationSetting`, and
  `Realization`;
- `pulp::host::NodeDescriptor`;
- `pulp::host::CatalogNode`, `CatalogRealization`, and a generic catalog
  validator/serializer interface.

Forge-specific composition should live in Forge:

- the Forge node-family catalog and its membership/index;
- Forge labels and prompt/product descriptions;
- Forge logical port/wiring mappings, SDK factory selection, macro eligibility,
  gain bounds, and product selection;
- the Forge JSON schema/version and the `pulp forge catalog` compatibility
  command while existing consumers migrate.

Stable `type_id`, parameter IDs, parameter keys, realization mode tokens, and
existing JSON keys remain compatibility contracts. They must not be renamed as
part of a namespace cleanup.

## Machinery versus catalog content

Classify each symbol and data field by its semantics, not its current filename:

| Surface | Product-neutral Pulp responsibility | Forge responsibility |
| --- | --- | --- |
| Descriptors | Types for IDs, parameters, choices, units, axes, and realizations; intrinsic DSP metadata | Forge presentation overrides and prompt vocabulary |
| Factories | Reusable DSP-to-`CustomNodeType` adapters and their physical parameter/lifecycle contracts | Selecting factories for Forge rows and binding product assets/settings |
| Ports | Actual node arity and reusable port descriptions, when defined | Forge logical stereo/CV mapping, graph syntax, and wiring policy |
| Discovery | Enumerating an explicitly supplied catalog and looking up its identities | Curating the Forge catalog's membership and product visibility |
| Validation/export | Validating supplied descriptors against constructed nodes; reusable encoding primitives | Expected Forge membership, Forge schema envelope, compatibility CLI and artifact |
| Sample regions | Runtime identities and exact construction/placement constraints | Forge parser/lowering mappings and authoring exposure |

The implementation currently mixes these responsibilities. For example,
`forge_catalog_export_nodes()` constructs a particular catalog, whereas
`ForgeCatalogExportNode` describes a transferable data shape. The catalog index
is a fixed list of Forge-facing packs, not evidence of a generic discovery API.
Likewise, a `forge_*_catalog.hpp` factory may wrap generic Pulp DSP while sharing
its file with product-oriented labels or selection policy.

Keep intrinsic DSP descriptions and useful reusable adapters in Pulp. Move only
Forge-specific selection and interpretation to Forge; do not require other
plugins to adopt Forge's complete catalog to use the machinery. A neutral API
must accept caller-supplied entries and validate/export a non-Forge catalog
without including the Forge family index, requiring Forge membership, or
embedding a Forge schema identifier.

The proposed generic names above are provisional: inventory existing Pulp names
before choosing them. Start with compatibility additions; do not immediately
emit deprecation warnings that would break consumers compiling with `-Werror`.
Changing the underlying C++ type name can also change mangled symbols, so an
alias alone is not proof of binary compatibility. Preserve existing definitions
and exported entry points during the first migration step and test both old
objects and new source consumers wherever binary compatibility is promised.

## Migration sequence

1. Add the generic Pulp types and validator behind the existing implementation.
2. Add neutral aliases/wrappers while preserving the existing concrete types,
   old include paths, and exported symbols. Defer warning-emitting deprecation
   until downstream consumers have migrated; prove promised source/binary
   compatibility rather than assuming aliases preserve it.
3. Add generic Pulp catalog APIs without changing the existing Forge exporter,
   installed path, schema string, or CLI output.
4. Move Forge catalog composition and product policy into Forge. During this
   phase Forge can consume the generic API while retaining the old compatibility
   include paths.
5. Add a Forge-side compatibility reader for both old and new generic catalog
   projections. Only after a released Forge Modular and Forge Sequencer have
   consumed the generic projection should the old names be deprecated for
   removal.
6. Remove compatibility names only in a separately versioned breaking release,
   with a migration note and an explicit downstream inventory.

Do not move a DSP implementation merely because its current header begins with
`forge_`. Move only the catalog composition/policy; preserve Pulp-owned DSP
factories and stable IDs where they are useful to any host.

## Required compatibility tests before any rename

The migration is accepted only when all of the following pass against the same
SDK prefix:

- Pulp generic-header compile tests and old-header alias compile tests.
- Byte identity of the existing `share/pulp/forge-catalog.json` during the
  compatibility phase, plus semantic identity of every node, realization,
  parameter, choice, range, default, and stable ID.
- Forge parser tests for old and generic schema projections, including malformed
  and missing-field negative controls.
- Forge Modular graph-load, factory/type-ID, parameter-clamp, realization, and
  round-trip project tests.
- Forge Sequencer generation/import/export tests, including sample-region rows,
  stable IDs, parameter contracts, and playback/render smoke tests.
- Installed SDK consumer tests that configure against a copied SDK prefix rather
  than source headers, proving that installation is the contract.
- A small non-Forge catalog fixture that constructs, validates, discovers, and
  serializes its own entries using only generic Pulp headers. It must not depend
  on Forge family membership, schema, logical wiring, macro eligibility, gain
  policy, or product selection. Generic port arity remains part of the node
  contract. Pair this with malformed descriptor and factory-mismatch controls.
- Old-object/new-library linkage checks for every binary-compatibility promise;
  old/new source builds with warnings treated as errors.
- Full Pulp and Forge test suites, generated-file checks, and adversarial review
  of the migration diff.

The acceptance rule is fail-closed: a missing or changed stable ID, type ID,
parameter key, realization token, numeric contract, or case/tag count blocks the
rename even if compilation succeeds.

## Decision

Do not rename the live Forge surface in the Space extraction. Land the Space
behavior only after its current compatibility receipt is green. Treat this
review as the prerequisite for a separate generic-layer migration packet, with
Forge Modular and Forge Sequencer test owners identified before implementation.
