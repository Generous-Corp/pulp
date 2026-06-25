# Content Commands Module Map

This map records the ownership contract for `tools/cli/content_commands.cpp`
and its extraction targets. The broader `CLI_COMMAND_SURFACE_MAP.md` owns
cross-command boundaries; this file owns the data-only `pulp content`
implementation seam.

The file currently owns the entire content-pack workflow in one 1,150-line
translation unit: content and plugin-runtime manifest parsing, archive hash
validation, safe extraction, data-root install/update/remove mutation, content
index generation, preview compatibility policy, JSON rendering, and command
dispatch.

Extraction changes should preserve the contracts below or update this file,
`CLI_COMMAND_SURFACE_MAP.md`, and the focused content tests in the same change.
If a change starts tracking `content_commands.cpp` in
`tools/scripts/hotspot_size_guard.json`, shrink that ceiling in the same PR that
shrinks the file.

## Current Source Map

| Region | Current lines | Owns today | Extraction owner |
| --- | --- | --- | --- |
| Public entry point | `content_commands.hpp` line 9 | `cmd_content` declaration for `pulp_cli.cpp` and direct tests. | Keep public until a command API review changes the surface. |
| Content value records and shared helpers | `content_commands.cpp` lines 36-224 | `ContentManifest`, `RuntimeManifest`, file read/write helpers, JSON escaping, vector helpers, JSON field extraction, safe path/component checks, data-root path helpers, timestamp formatting, and zip entry reads. | Split `ContentManifest` into `content_manifest.{hpp,cpp}`, `RuntimeManifest` into `content_runtime_manifest.{hpp,cpp}`, and data-root/install-root helpers into `content_paths.{hpp,cpp}` before multiple modules depend on them; keep remaining helpers private to the smallest module that uses them. |
| Content-pack manifest loading | `content_commands.cpp` lines 226-272 | Directory, bare-manifest, and `.pulpcontent`/`.zip` manifest loading, manifest hash calculation, exported-kind/path extraction, and content-pack metadata projection. | `content_manifest.{hpp,cpp}`. |
| Plugin runtime manifest loading | `content_commands.cpp` lines 274-316 | Runtime manifest schema validation, plugin id, accepted capabilities/kinds, hot-reload/manual-rescan policy extraction, and reload-kind consistency checks. | `content_runtime_manifest.{hpp,cpp}`. |
| Archive validation | `content_commands.cpp` lines 318-393 | Archive entry safety, `files.sha256.json` presence and shape checks, per-file digest verification, unlisted-payload rejection, and temporary validation root naming. | `content_archive.{hpp,cpp}`. |
| Content copy and extraction | `content_commands.cpp` lines 395-513 | Declared-path copy, symlink rejection, recursive export traversal, safe archive extraction, destination containment, and extraction error synthesis. | `content_archive.{hpp,cpp}` for archive extraction and `content_install.{hpp,cpp}` for declared-tree copy. |
| Installed-content index | `content_commands.cpp` lines 515-565 | Installed pack discovery, backup-directory filtering, stable listing order, `Content/index.json` generation, manifest hashes, plugin id projection, and content count reporting. | `content_index.{hpp,cpp}`. |
| Install/update shared helpers | `content_commands.cpp` lines 567-629 | Bare-manifest rejection for install/update, source copy/extract dispatch, content/runtime JSON rendering, reload-policy computation, and preview install-policy JSON. | `content_install.{hpp,cpp}` for install/update helpers, `content_manifest.{hpp,cpp}` for content manifest JSON, `content_runtime_manifest.{hpp,cpp}` for runtime manifest JSON, and `content_preview.{hpp,cpp}` for runtime policy rendering. |
| Validation contract | `content_commands.cpp` lines 631-681 | Content-pack validation, required fields, `kind=content-pack` enforcement, archive extraction-to-temp verification, and reuse of `pulp::cli::kit::validate_manifest_path` for manifest-shape checks. | `content_manifest_validation.{hpp,cpp}` after `content_archive.{hpp,cpp}` exposes archive validation/extraction helpers. |
| Usage and command dispatch | `content_commands.cpp` lines 683-1147 | Usage banner, argv parsing for validate/preview/install/update/list/rescan/remove/reveal, mutation guards, rollback, and the top-level subcommand dispatcher. | End-state thin `content_command_dispatch.{hpp,cpp}`; intermediate PRs may leave dispatch in `content_commands.cpp` while moving behavior helpers out. |

## Target Modules

| Module | Owns | Must not own |
| --- | --- | --- |
| `content_manifest.{hpp,cpp}` | Content-pack manifest loading, `ContentManifest`, exported-kind/path projection, content-pack metadata JSON, and manifest hash calculation. | User data root mutation, archive extraction, plugin runtime compatibility policy, command argv parsing, or project package/kit rules beyond calling a validator. |
| `content_manifest_validation.{hpp,cpp}` | Content-pack validation, required-field and `kind=content-pack` checks, archive validate/extract-to-temp orchestration, and reuse of the kit manifest-shape validator. | Archive entry traversal internals, install/update mutation, command argv parsing, or kit plan/apply semantics. |
| `content_runtime_manifest.{hpp,cpp}` | Plugin runtime manifest loading, `RuntimeManifest`, runtime content capability/kind validation, hot-reload/manual-rescan extraction, and runtime JSON rendering. | Content pack install/remove mutation, archive hash checks, package registry lookup, or importer/tool behavior. |
| `content_archive.{hpp,cpp}` | `.pulpcontent` archive detection support, safe relative-entry validation, `files.sha256.json` verification, digest mismatch reporting, unlisted payload rejection, and safe extraction into a caller-provided root. | User data index generation, plugin compatibility policy, command dispatch, or accepting source-tree symlinks. |
| `content_paths.{hpp,cpp}` | Shared content data-root, `Content/` root, install-root calculation, path containment, and unsafe component/path checks used by index/install/dispatch modules. | Archive hash validation, manifest schema parsing, command argv parsing, or filesystem mutation beyond pure path calculation. |
| `content_install.{hpp,cpp}` | Declared content tree copy, install/update/remove/reveal helpers, rollback, backup cleanup, and post-mutation index refresh calls. | Manifest schema parsing, archive digest policy, runtime reload policy, package/kit project mutation, or CMake/dependency docs edits. |
| `content_index.{hpp,cpp}` | Installed content discovery, backup filtering, stable ordering, `Content/index.json` rendering, rescan behavior, and list JSON helpers. | Archive extraction, runtime compatibility policy, command argv parsing, or install/update rollback. |
| `content_preview.{hpp,cpp}` | Compatibility preview between a validated content pack and plugin runtime manifest, reload-policy selection, restart requirement synthesis, and preview JSON/human output helpers. | Filesystem mutation, archive extraction, user data index writes, or package/kit install policy. |
| `content_command_dispatch.{hpp,cpp}` | `pulp content` usage text, argv parsing, `--help`, subcommand routing, and conversion from CLI args into focused helper calls. | Business rules that can be tested without argv, archive traversal, rollback details, content index internals, or runtime manifest parsing. |
| `content_commands.cpp` | Public `cmd_content` facade while extraction is in progress. | New behavior that belongs in the focused modules above. |

## Boundary Rules

- The content lane is data-only. It must not execute package code, run CMake,
  install source/UI/template kits, mutate project source files, or update
  package lockfiles/dependency docs.
- Install/update/remove are the only mutating content subcommands. Validate,
  preview, list, rescan, and reveal must not copy payload files into a plugin
  install tree.
- Archive helpers must reject absolute paths, `..` traversal, unsafe
  extraction destinations, missing `files.sha256.json`, digest mismatches, and
  unlisted payload entries before install/update trusts archive contents.
- Source-tree content copy must keep rejecting symlinks before payload files
  are copied.
- `content_manifest_validation` may call
  `pulp::cli::kit::validate_manifest_path` for shared package-manifest shape
  rules, but content plan/apply semantics must remain separate from `pulp kit`.
- `content_preview` reads a trusted plugin runtime manifest and reports
  compatibility/reload policy only. It must not mutate the user data root.
- `content_index` owns generated `Content/index.json`; install/update/remove
  call it after mutation, and rescan calls it without changing pack payloads.
- Command dispatch remains a thin adapter. New content behavior should land
  behind a focused module API first, then be wired into `cmd_*` parsing.

## Current Test Anchors

- `test/test_cli_content_commands.cpp` lines 131-171 cover validate,
  install/list/reveal/remove, index generation, manifest hashes, and preserving
  user presets during remove.
- `test/test_cli_content_commands.cpp` lines 174-225 cover install refusal,
  `ContentRegistry` visibility, and factory preset discovery.
- `test/test_cli_content_commands.cpp` lines 228-320 cover unsafe component
  rejection, symlink rejection, and bare-manifest install/update rejection.
- `test/test_cli_content_commands.cpp` lines 323-431 cover preview runtime
  compatibility/reload policy, successful update replacement, backup cleanup,
  and invalid update rejection before installed content is replaced.
- `test/test_cli_content_commands.cpp` lines 433-459 cover rescan index
  rebuilds without touching installed pack payloads.
- `test/test_cli_content_commands.cpp` lines 461-555 cover `.pulpcontent`
  archive validation/install, missing hash manifests, unlisted payload
  rejection, and missing exported sample/wavetable validation.
- `test/cmake/content_workflow_tests.cmake` lines 34-45 wires
  `pulp-test-cli-content-commands`; moved implementation files must be added
  to that target in the same PR.
- `tools/cli/CMakeLists.txt` line 77 wires `content_commands.cpp` into the
  `pulp-cli` target; every extracted content implementation file must be added
  there too.

## Extraction Order

1. Move shared content path helpers into `content_paths.{hpp,cpp}` before
   extracting modules that need `content_root`, `install_root`, containment, or
   unsafe-component checks.
2. Move `ContentManifest` loading and content manifest JSON into
   `content_manifest.{hpp,cpp}`; keep the existing JSON/error corpus unchanged.
3. Move archive hash validation and safe extraction into
   `content_archive.{hpp,cpp}` with the existing missing-hash, unlisted-payload,
   unsafe-path, and `.pulpcontent` install fixtures.
4. Move content-pack validation into `content_manifest_validation.{hpp,cpp}`
   once archive validation/extraction APIs are available.
5. Move installed-content discovery and index generation into
   `content_index.{hpp,cpp}` so rescan/list behavior can be tested without
   install/update command parsing.
6. Move install/update/remove/reveal filesystem mutation into
   `content_install.{hpp,cpp}` with rollback, backup cleanup, symlink, and
   unsafe-component tests.
7. Move plugin runtime manifest parsing and preview policy into
   `content_runtime_manifest.{hpp,cpp}` and `content_preview.{hpp,cpp}`.
8. Leave `content_command_dispatch` as the final adapter after behavior helpers
   are stable and covered.
9. After each extraction that shrinks `content_commands.cpp`, lower the exact
   LOC ceiling in `tools/scripts/hotspot_size_guard.json` in the same commit if
   the file is tracked there.

## Non-Goals

- Do not merge content and kit archive helpers unless their trust contracts
  become identical. Today kit archives are reviewable project mutation inputs;
  content archives are data-only user-content inputs.
- Do not introduce a package/content super-manifest model in this split. The
  shared shape validation call is a dependency, not an ownership transfer.
- Do not add CMake/project mutation to `pulp content`; the content lane owns
  user data, not source projects.
