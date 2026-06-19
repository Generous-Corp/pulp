# Import Design Module Map

This is the Phase 1 ownership map for shrinking
`tools/import-design/pulp_import_design.cpp`. It documents the current source
regions, the intended module seams, and the invariants that must survive any
future extraction. It is not a claim that the split has already happened.

`tools/import-design/import_detect.{hpp,cpp}` is already a separate detection
module. It is distinct from `tools/cli/import_detect.{hpp,cpp}`, which serves the
integrated `pulp` CLI. Keep the import-design detector self-contained; this map
covers the remaining importer CLI surface and the helpers that still live in
`pulp_import_design.cpp`.

## Current Source Regions

| Region | Current lines | Ownership |
| --- | ---: | --- |
| PNG decode and opaque-core sampling | `pulp_import_design.cpp:61` | Move to an image helper used by asset enrichment. |
| CLI mode enums, string normalization, serialized-IR/envelope sniffing, element counts | `pulp_import_design.cpp:219` | Split between option parsing and lightweight input sniffing. |
| Pulp home/config/default selection | `pulp_import_design.cpp:356` | Defaults/config module. |
| Scoped temp dirs, URL/file input validation, curl fetch | `pulp_import_design.cpp:476` | Input I/O module. |
| Diagnostic formatting, argument parsers, asset options | `pulp_import_design.cpp:614` | Diagnostics plus option helpers. |
| C++/Swift output paths and token format dispatch | `pulp_import_design.cpp:768` | Output-path and token-export modules. |
| Usage text | `pulp_import_design.cpp:858` | CLI facade. |
| File read helper | `pulp_import_design.cpp:965` | Input I/O module. |
| `.pulp.zip` sidecar lifecycle | `pulp_import_design.cpp:996` | ZIP sidecar/transaction module. |
| Atomic text-file staging and rollback | `pulp_import_design.cpp:1269` | Atomic output module. |
| ZIP extraction and archive safety checks | `pulp_import_design.cpp:1451` | ZIP extraction module. |
| `main()` option state, argv parse, defaults, format/output validation | `pulp_import_design.cpp:1644` | CLI facade. |
| `--export-tokens` dispatch | `pulp_import_design.cpp:1975` | Token-export module. |
| `--detect-only` and `--report-new-format` dispatch | `pulp_import_design.cpp:2014` | Thin detection CLI wrapper around `import_detect`. |
| Source/input/mode validation, URL fetch, ZIP unpack, live JSX passthrough | `pulp_import_design.cpp:2098` | Input orchestration. |
| Source parser dispatch, ZIP commit, metadata, diagnostics | `pulp_import_design.cpp:2229` | Parse orchestration. |
| `--emit ir-json`, `cpp`, and `swiftui` paths | `pulp_import_design.cpp:2408` | Baked emit module. |
| JS codegen options and shortcut extraction/defaults | `pulp_import_design.cpp:2555` | JS emit module, with shortcut helpers kept separate. |
| Sprite-knob conversion, asset/font path resolution, image metadata, fader/meter skin derivation | `pulp_import_design.cpp:2618` | Asset enrichment module. |
| JS generation, strict-fidelity handling, dry-run, JS/meta/tokens sidecars | `pulp_import_design.cpp:2904` | JS emit module. |
| Claude bridge/classnames/defaults sidecars and native-react hint | `pulp_import_design.cpp:3014` | Claude/import-sidecar module. |
| Validation render/reference/diff | `pulp_import_design.cpp:3110` | Validation module. |
| Debug JSON report | `pulp_import_design.cpp:3189` | Debug-report module. |
| Final ZIP sidecar finalize and strict-fidelity exit | `pulp_import_design.cpp:3266` | CLI facade cleanup. |

## Target Modules

Extract in small, behavior-preserving moves. Prefer narrow headers with plain
data structs over pulling the entire CLI state into every module.

| Target module | Owns | Must not own |
| --- | --- | --- |
| `import_design_options` | Enums, normalized option values, argument parsing helpers, source/mode/emit compatibility checks. | Parser execution, filesystem writes. |
| `import_design_defaults` | `$HOME`/config lookup and default-source/mode resolution. | Import execution or diagnostics printing. |
| `import_design_io` | File reads, scoped temp dirs, URL/file safety checks, fetch-to-file. | ZIP archive semantics or output transactions. |
| `import_design_zip` | ZIP magic detection, archive extraction, path traversal guards, count/size caps. | Output sidecar commit/finalize. |
| `import_design_zip_sidecar` | `.pulp.zip` asset sidecar transaction, marker file, restore/replace policy. | Generic archive extraction. |
| `import_design_atomic_write` | Multi-file staging, rollback, and commit for generated text outputs. | ZIP sidecar lifecycle. |
| `import_design_output_paths` | JS/C++/Swift sidecar path resolution and no-clobber policy. | Parser or codegen logic. |
| `import_design_tokens` | `--export-tokens`, token format validation, token-file writes. | UI scaffold generation. |
| `import_design_detect_cli` | `--detect-only` / `--report-new-format` CLI wrapper and output. | Detection algorithms already in `import_detect`. |
| `import_design_parse` | Source parser dispatch and parser-result normalization. | Source detection internals or code emit. |
| `import_design_asset_enrichment` | Font/asset resolution, PNG metadata, sprite knobs, fader/meter skins, portable asset paths. | CLI argv parsing. |
| `import_design_emit_baked` | IR JSON, C++, and SwiftUI output flows. | JS emit sidecars. |
| `import_design_emit_js` | JS codegen options, strict-fidelity handling, dry-run, generated JS/meta/tokens writes. | Claude bridge handler file contents. |
| `import_design_claude_outputs` | Bridge scaffold, classnames/defaults sidecars, native-react hint. | Parser runtime. |
| `import_design_validate` | Generated-JS validation render, reference diff, screenshot backend plumbing. | Faithful SVG proofing. |
| `import_design_debug_report` | Debug JSON report assembly and write. | Parser or emit side effects. |
| `pulp_import_design.cpp` | `main()`, usage text, high-level orchestration, final exit code policy. | Helper implementations once extracted. |

When adding new `.cpp` files, update `tools/import-design/CMakeLists.txt` for
the `pulp-import-design` target. Add includes or target sources for tests only
when a helper becomes directly unit-testable.

## Extraction Order

1. Move pure option/default/output-path helpers first.
2. Move atomic file I/O and URL fetch helpers.
3. Move ZIP extraction and ZIP sidecar transaction helpers together with their
   existing failure/rollback coverage.
4. Move the detection CLI wrapper, leaving `import_detect.{hpp,cpp}` independent.
5. Move token export.
6. Move source parser dispatch.
7. Move asset enrichment and PNG helpers.
8. Move baked IR/C++/SwiftUI emit paths.
9. Move JS emit and sidecars.
10. Move validation and debug-report assembly.
11. Leave `pulp_import_design.cpp` as a thin CLI facade.

Do not invert dependency direction during extraction. For example, ZIP safety
checks must not depend on emit modules, and parser dispatch must not depend on
validation or debug reporting.

## Boundary Rules

- `import_detect.{hpp,cpp}` remains a bounded source/version detector. Do not
  pull `pulp::view` parser, materializer, or codegen dependencies into it.
- `.pulp.zip` safety and sidecar transaction invariants travel together:
  filename length guard, `..` rejection, absolute/drive/UNC path rejection,
  entry-count and uncompressed-size caps, marked sidecar replacement, and
  rollback on parse/write failure.
- Atomic writes and ZIP sidecar commit/finalize must preserve multi-file
  rollback across JS, metadata, C++/Swift sidecars, and asset sidecars.
- Paths baked into generated JS/Swift/C++ asset and font outputs must use
  generic `/` separators (`fs::path::generic_string()` style). Native
  backslashes in generated UI output are a portability regression.
- `--validate` renders the generated JS/native-widget path. It is not the
  faithful SVG proof lane; faithful-vector validation stays with
  `pulp-svg-probe`, screenshot backends, and fidelity diff tooling.
- `--from jsx --mode live --emit js` remains passthrough. Do not route it
  through validation, debug report, parser dispatch, or diff code.
- `DESIGN.md` via `--export-tokens` stays tokens-only; that path does not
  scaffold UI output.
- Keep provider-source runtime parsers and the source-contract registry in sync
  when parser symbols move. MCP lanes acquire input; they are not accepted as raw
  runtime parser payloads unless the source contract says so.
- Asset enrichment may sample pixels and stamp metadata, but it must remain
  data-driven. Do not add per-fixture constants for knobs, faders, meters, fonts,
  or illustration fills.
- Generated bridge/source identity (`setAnchor`, source locations, tweak/lock
  contracts) belongs to codegen or inspector modules, not to the CLI facade.

## Test Anchors

Keep these tests green after each extraction step. When a helper becomes
unit-testable, add focused coverage near the existing behavioral tests instead
of only asserting through `main()`.

| Surface | Current coverage |
| --- | --- |
| Import-design binary wiring | `tools/import-design/CMakeLists.txt:1`, `test/CMakeLists.txt:493`, `test/CMakeLists.txt:5522`, `test/CMakeLists.txt:5544` |
| Fidelity diff Python harness | `test/CMakeLists.txt:207`, `test/test_import_fidelity_diff.py` |
| Detector self-containment and fixtures | `test/CMakeLists.txt:833`, `test/test_cli_import_detect.cpp` |
| DESIGN.md import/export | `test/CMakeLists.txt:5413`, `test/test_design_import_designmd.cpp:63` |
| CLI help/import vocabulary | `test/test_import_design_tool.cpp:173`, `test/test_import_design_tool.cpp:213` |
| Token export and nested output paths | `test/test_import_design_tool.cpp:878`, `test/test_import_design_tool.cpp:892`, `test/test_import_design_tool.cpp:2013` |
| File/URL safety and fetch behavior | `test/test_import_design_tool.cpp:937`, `test/test_import_design_tool.cpp:1001` |
| Debug report | `test/test_import_design_tool.cpp:1043` |
| ZIP happy paths and baked emit metadata | `test/test_import_design_tool.cpp:1199`, `test/test_import_design_tool.cpp:1281`, `test/test_import_design_tool.cpp:1448`, `test/test_import_design_tool.cpp:1513`, `test/test_import_design_tool.cpp:1572` |
| ZIP failure, rollback, and archive hardening | `test/test_import_design_tool.cpp:1639`, `test/test_import_design_tool.cpp:1668`, `test/test_import_design_tool.cpp:1699`, `test/test_import_design_tool.cpp:1724`, `test/test_import_design_tool.cpp:1763`, `test/test_import_design_tool.cpp:1805`, `test/test_import_design_tool.cpp:1904`, `test/test_import_design_tool.cpp:1930`, `test/test_import_design_tool.cpp:1949`, `test/test_import_design_tool.cpp:1977` |
| Token format validation, screenshot backend validation, SwiftUI no-clobber | `test/test_import_design_tool.cpp:2037`, `test/test_import_design_tool.cpp:2052`, `test/test_import_design_tool.cpp:2067`, `test/test_import_design_tool.cpp:2087`, `test/test_import_design_tool.cpp:2105`, `test/test_import_design_tool.cpp:2129`, `test/test_import_design_tool.cpp:2160`, `test/test_import_design_tool.cpp:2183` |
| CLI source routing, Claude sidecars, classnames, strict fidelity, native-react hints, sprite knobs | `test/test_cli_import_design.cpp:118`, `test/test_cli_import_design.cpp:234`, `test/test_cli_import_design.cpp:267`, `test/test_cli_import_design.cpp:360`, `test/test_cli_import_design.cpp:458`, `test/test_cli_import_design.cpp:534` |
| Figma-plugin IR and visual-feature route tests | `test/test_cli_import_design.cpp:571`, `test/test_cli_import_design.cpp:595`, `test/test_cli_import_design.cpp:634`, `test/test_cli_import_design.cpp:669`, `test/test_cli_import_design.cpp:728` |
| Source-contract registry | `tools/import-validation/source-contracts.json`, `tools/import-validation/check-source-contracts.py`, `tools/import-validation/test_source_contracts.py` |
