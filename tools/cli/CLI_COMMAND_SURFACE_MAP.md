# CLI Command Surface Map

This map records ownership boundaries for the Pulp C++ CLI command surface
under `tools/cli/`. It complements the narrower `KIT_COMMANDS_MODULE_MAP.md`:
that file owns the `pulp kit` implementation split, while this file documents
how the top-level command families relate to each other so CLI changes do not
blur trust boundaries, docs ownership, or validation lanes.

The C++ delegate still builds as one `pulp-cli` target from
`tools/cli/CMakeLists.txt`, with the user-facing Rust CLI falling through to
this delegate for many command families. The largest remaining CLI hotspot is
`kit_commands.cpp` at 3,927 lines, but the surrounding ownership risks are
cross-command: `kit`, `content`, `package/add/search/audit`, `import`, and
`tool` all touch package-like concepts with different execution and mutation
rules.

CLI extraction and routing changes should preserve the contracts below or
update this file, command docs, tests, and skill-sync/versioning expectations in
the same change.

## Current Source Map

| Region | Current evidence | Owns today | Extraction owner |
| --- | --- | --- | --- |
| Top-level command table | `pulp_cli.cpp` lines 33-74 | Built-in command names, summaries, and direct C++ handlers including `kit`, `content`, `import`, `pr`, `project`, `macos`, and `overflow`. | Keep centralized until the Rust front owns the full command manifest; this table remains the C++ delegate's routing source. |
| Script and binary delegation | `pulp_cli.cpp` lines 85-106 | Python script commands (`ci-local`, `harness`) and build-binary commands (`design-debug`, `import-design`, `export-tokens`). | Keep in `pulp_cli.cpp` unless delegation grows policy; then extract a small `cli_delegate_commands` helper. |
| Legacy package aliases | `pulp_cli.cpp` lines 121-140 and 533-540 | `pulp audit`, `pulp add`, `pulp remove`, `pulp update`, `pulp search`, `pulp list`, `pulp suggest`, and `pulp target` compatibility routing into package helpers. | Keep routing thin in `pulp_cli.cpp`; package behavior belongs in `package_commands_*`. |
| Invocation/update wrapper | `pulp_cli.cpp` lines 493-560 | Process entry, update-banner suppression, and final dispatch across built-in, script, binary, package alias, audit, tool, legacy alias, help, and unknown-command paths. | Keep process-level concerns here; do not add package/content/kit/tool business rules. |
| Kit trust-boundary lane | `kit_commands.cpp` lines 2708-3920, `kit_commands.hpp` lines 12-39, `KIT_COMMANDS_MODULE_MAP.md` | Manifest validation, inspect/plan/apply/remove, archive pack/publish/init, validation profiles, JSON output, and safety review boundaries for source/UI/template kits. | Follow `KIT_COMMANDS_MODULE_MAP.md`: split manifest, policy, registry manifest, archive, apply, profile, and dispatch modules. |
| Content-pack lane | `content_commands.cpp`, `content_commands.hpp`, and `CONTENT_COMMANDS_MODULE_MAP.md` | Data-only content-pack validation, preview, install/update/list/rescan/remove/reveal, runtime content index writes, archive validation, and user-data filesystem mutation. | Follow `CONTENT_COMMANDS_MODULE_MAP.md`: split manifest, runtime-manifest, archive, path, install, index, preview, and dispatch modules. |
| Package lane | `package_commands_internal.hpp` lines 1-88, `package_commands_search.cpp` lines 106-396, `package_commands_add.cpp` lines 28-384, `package_commands.cpp` lines 25-145 | Third-party audio package search/list/suggest/target, add/remove/update mutation, CMake/dependency metadata edits, and package audit aliases. Already partially split by read-only, mutating, util, and audit concerns. | Preserve current split. If it grows, split target management out of `package_commands_search.cpp` before touching add/remove/update. |
| Framework import lane | `cmd_import.cpp` lines 56-115 and `import_run.cpp` lines 469-563 | `pulp import detect/inspect/emit` argument parsing, vendor-free framework detection, importer SPI orchestration, terms gate, clean-room emission, and scaffold write planning. | Keep `cmd_import.cpp` as parse/dispatch only. Put new verb orchestration in `import_run.cpp` or focused `import_*` helpers. |
| Tool/importer registry lane | `pulp_cli.cpp` line 541, `tool_registry.cpp`, and `importer_install.cpp` | Top-level `pulp tool` dispatch, add-on tool registry loading, importer install/locate helpers, and the `pulp add <importer>` alias used by package add. | Keep reusable registry mechanics separate from package/content/kit policy; package add may route to it but must not own tool installation. |
| CLI build and shellout tests | `tools/cli/CMakeLists.txt` lines 40-88 and 177-508, `test/CMakeLists.txt` CLI blocks | The `pulp-cli` source list, basic CLI shellout tests, and per-surface test target wiring. | Every moved implementation file must be added to the relevant target and focused test target in the same PR. |

## Ownership Rules

- `pulp_cli.cpp` owns routing, process-level update checks, and help table
  visibility. It must not grow package, kit, content, import, or tool business
  rules.
- Package commands operate on third-party code packages and project metadata:
  package lockfile, generated CMake include, `DEPENDENCIES.md`, `NOTICE.md`,
  target compatibility, and license policy.
- Kit commands operate on source/UI/template kit manifests and reviewable
  project mutation. They may inspect content-pack manifests, but plan/apply and
  publish must keep rejecting content-pack lanes and direct users to
  `pulp content`.
- Content commands operate on data-only content packs installed under the user
  data root. They must not execute package code, run CMake, mutate project
  source files, or install source/UI/template kits.
- Import commands operate on existing external projects read-only and emit a
  migration scaffold. Generic SDK code must remain vendor-free; framework
  markers belong in data files or add-on importer tools, not in `tools/cli`.
- Tool registry helpers own add-on tool lookup and installation mechanics.
  Package add may delegate importer aliases to them, but package install logic
  must not duplicate registry location or SPI-version policy.
- Shared JSON/path/print helpers should be local to a command family unless at
  least two command families need the exact same invariant. Do not create a
  broad `cli_utils` dumping ground for unrelated helpers.

## Trust And Mutation Boundaries

| Lane | May read | May mutate | Must not do |
| --- | --- | --- | --- |
| Package search/list/suggest/target | Package registry, project targets, package lockfile | `target add/remove` may update `pulp.toml`; search/list/suggest are read-only. | Install data content, apply kit files, execute importers, or write user content indexes. |
| Package add/remove/update | Package registry, package lockfile, project metadata | `packages.lock.json`, generated package CMake, dependency/notice docs. | Trust a kit/content archive, install user content, or run validation profiles. |
| Kit inspect/validate/search | Local kit trees or archives, manifest-declared files | Nothing except explicit output/report paths. | Execute code, run CMake, install content, or mutate a project. |
| Kit plan/apply/remove | Reviewed kit manifests, archives, project lock records | Project files declared by the kit, generated CMake include, kit lock/ownership records, rollback backups. | Install data-only content packs or execute package/importer binaries during review. |
| Kit pack/publish/init | Kit source trees, hash manifests, registry manifests | Archive output, initialized kit skeletons, publish dry-run output. | Apply to a project or install content. |
| Content validate/preview | Content-pack manifests, archives, runtime capability manifests | Nothing except explicit preview output. | Run package code, mutate a project, or apply source/UI/template kits. |
| Content install/update/remove/rescan | Content-pack manifests, archives, user data root, runtime content registry | User data content tree and `Content/index.json`. | Touch project source, generated CMake, package lockfiles, or dependency docs. |
| Import detect/inspect/emit | External project tree, known-frameworks data, importer tool registry | Only the requested scaffold output during `emit`. | Embed vendor markers in SDK code, mutate the source project, or let importers write files directly. |

## Current Validation Anchors

- `test/test_cli_kit_commands.cpp` covers `pulp kit` manifest validation,
  inspect/plan/apply/remove, archive pack/publish/init, JSON output, archive
  safety, rollback, and error behavior.
- `test/test_cli_content_commands.cpp` covers `pulp content` validate,
  preview, install/update/list/rescan/remove/reveal, archive safety, runtime
  registry visibility, unsafe path rejection, and kit/content archive
  interop.
- `test/test_cli_import.cpp`, `test/test_cli_import_emit.cpp`,
  `test/test_cli_import_terms.cpp`, `test/test_cli_import_detect.cpp`, and
  `test/test_cli_importer_install.cpp` cover import detection, SPI emit,
  terms gating, vendor-free SDK constraints, and importer install behavior.
- `test/test_cli_shellout.cpp` covers broad CLI process contracts that require
  the built binary rather than direct function calls.
- `tools/scripts/cli_sync_check.py` and
  `tools/scripts/check_cli_mcp_parity.py --mode=report` are the sync checks to
  run when a command surface changes. Docs-only ownership maps do not change
  command behavior, but command additions/removals must update
  `docs/status/cli-commands.yaml`, `docs/reference/cli.md`, relevant slash
  commands, skills, and MCP parity baselines/tools.

## Extraction Order

1. Keep using `KIT_COMMANDS_MODULE_MAP.md` for `kit_commands.cpp`; it is the
   highest-LOC command hotspot and already has a focused split plan.
2. Split `content_commands.cpp` next by side-effect boundary: manifest parse
   and content capability validation first, archive safety second,
   install/update/remove/rescan/index writes third, and command dispatch last.
3. Preserve the existing package split. If package grows again, extract target
   management from `package_commands_search.cpp` before changing add/remove,
   because target mutation does not share the same docs/license side effects as
   package installation.
4. Keep `cmd_import.cpp` thin. Add new import behavior to `import_run.cpp` or
   narrower `import_*` helpers, and keep the vendor-token guard tests updated.
5. Do not combine kit and content extraction in the same PR. They share
   archive/hash concepts but enforce different trust and mutation rules.
6. Do not combine package and tool-registry extraction in the same PR unless
   the change is specifically about the importer alias path
   (`pulp add <importer>`). Otherwise the review surface mixes project package
   mutation with add-on tool installation.
7. After every extraction that shrinks a tracked hotspot, lower the exact LOC
   ceiling in `tools/scripts/hotspot_size_guard.json` in the same commit.

## Non-Goals

- Do not introduce a new public CLI API layer just to move code. The Rust
  front, C++ delegate, docs manifest, slash commands, and MCP parity checks
  already form the public synchronization boundary.
- Do not merge `kit`, `content`, and `package` manifest models into one
  generic package model until their trust boundaries are proved equivalent.
  Today they are not equivalent.
- Do not move framework-specific import knowledge into SDK C++ code. Importer
  specificity remains data/add-on owned.
- Do not treat all large CLI files as equally urgent. `tool_registry.cpp` and
  `cli_common.cpp` are broad shared infrastructure; split them only around
  stable ownership seams, not line count alone.
