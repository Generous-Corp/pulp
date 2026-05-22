---
name: import-design
description: Import designs from Figma, Stitch, v0, Pencil, React Native, or Claude Design into Pulp web-compat JS with automated visual validation. Claude Design imports also scaffold a pulp::view::EditorBridge handler file (pulp #709). Versioned (parser-version / format-version / compat-schema-version) detection lives behind `--detect-only` and `--report-new-format` (pulp #1031).
---

# Import Design

Import a design from an external tool (Figma, Stitch, v0, Pencil, React Native, Claude Design, or the experimental JSX runtime lane) into this Pulp project.

## CRITICAL: pulp-design-tool requires the GPU host (PULP_HAS_SKIA)

Before debugging *any* runtime-import resize / sizing / layout issue in `pulp-design-tool` or `/tmp/<App>.app`, verify the binary is using `MacGpuWindowHost`, not the CPU `MacWindowHost`. The design viewport pin, aspect-lock, and uniform paint-scale all live in `MacGpuWindowHost` (gated by `#ifdef PULP_HAS_SKIA` in `core/view/platform/mac/window_host_mac.mm`). When Skia isn't linked, `WindowHost::create()` returns `MacWindowHost`, where `set_design_viewport()` and `set_fixed_aspect_ratio()` are base-class no-ops — the example still builds and runs, but resize behaves as if every fix you've shipped is missing.

**One-shot verification:**

```bash
# In the worktree
nm build/examples/design-tool/pulp-design-tool 2>/dev/null | grep -q MacGpuWindowHost \
  && echo "OK: GPU host present" || echo "FAIL: CPU-only build"

# In a packaged .app
strings /tmp/MyApp.app/Contents/MacOS/MyApp-Bin | grep -F "[gpu-host]" \
  && echo "OK: GPU host present" || echo "FAIL: CPU-only build"
```

**Recovery when Skia is missing** (e.g. fresh worktree with `external/skia-build/` containing only headers):

1. Reuse the primary checkout's populated cache:
   ```bash
   rm -rf external/skia-build
   ln -s /Users/<you>/Code/pulp/external/skia-build external/skia-build
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # should print "Skia: found at ..."
   ```
2. Or run `tools/build-skia.sh` to rebuild Skia binaries from scratch (~30 min).
3. Or pass `-DSKIA_DIR=/abs/path/to/external/skia-builder/build/<plat>-gpu`.

**Defenses already shipped** (don't bypass without reading the comments):

- `examples/design-tool/CMakeLists.txt` issues `FATAL_ERROR` at configure if `PULP_HAS_SKIA` is FALSE — design-tool is intentionally a GPU-only example.
- `examples/design-tool/main.cpp` checks `#ifndef PULP_HAS_SKIA` at startup and exits with EX_CONFIG (78) + a loud stderr message.
- `tools/cmake/PulpDependencies.cmake` already prints a screaming WARNING banner when `PULP_ENABLE_GPU=ON` but Skia isn't found; the release lane (`PULP_REQUIRE_GPU_FOR_SDK=ON` in `release-cli.yml`) escalates to FATAL_ERROR.

**If your runtime-import test produces a window where content doesn't scale, the dark fill is visible past the design surface, or aspect-lock doesn't engage during drag, the FIRST thing to check is `nm | grep MacGpuWindowHost`.** Don't tune `set_design_viewport` / `windowWillResize:` / `setContentAspectRatio:` until you've confirmed the GPU host is actually linked.

Detect which design source the user wants by checking:
1. If a Figma MCP server is available (com.figma.mcp), offer to read the current file/selection
2. If Stitch MCP is available (mcp__stitch__*), offer to list projects and get screens
3. If Pencil MCP is available (mcp__pencil__*), offer to read the current editor state
4. If the user mentions Claude Design or hands over a manually-exported HTML file from Anthropic Labs' Claude Design tool, treat as `--from claude` (no MCP — Anthropic has no public API; per pulp #468, manual file export is the supported path; Spectr's editor.html mapping is the precedent)
5. If the user provides a file path or URL, use that directly
6. If none of the above, ask the user for a source and file

## Workflow

### DesignIR v1 asset manifest lane

When a user asks for canonical IR or an import pipeline handoff, prefer
`pulp import-design --emit ir-json`. The output is a versioned DesignIR v1
envelope with a deterministic `assetManifest` sidecar. Local images, SVGs,
font URLs, CSS `url(...)` values, and data URIs are recorded by default.
HTTP(S) assets are resolve-only unless the user explicitly passes
`--allow-network-fetch`; fetched assets use `--asset-cache`, honor
`--asset-timeout-ms`, and can be pinned with repeated
`--asset-hash <uri=sha256>` flags.

For `--url` imports, relative asset references resolve against the original
source URL, not the temporary downloaded file. The manifest keeps the authored
value in `original_uri`, stores the resolved fetch target in `source_url`, and
nodes keep their raw URI attributes plus a stable companion such as
`srcAssetId` or `backgroundImageAssetId`.

DesignIR v1.5 also carries document-level provenance (`capture_method`,
`settle_rounds`, `fallback_reason`, `source_adapter`, `source_version`,
`imported_at`) and structured top-level diagnostics. Parse APIs return the
shared normalized form, including interactive-frame promotion from the Pulp
view library.

The shared native-resolution layer lives in
`core/view/src/design_import_native_common.{hpp,cpp}`. It consumes normalized
`DesignIR` plus `IRAssetManifest` and returns a deterministic
`ResolvedNativeNode` tree for later baked-native/baked-cpp materializers.
Mapping precedence is `audio_widget` first, then `IRNode.type`, then HTML
subtype attributes such as `input[type=range]`, with unsupported nodes and
properties degrading to diagnostics instead of throwing. When resolving frozen
DesignIR JSON, keep the embedded `assetManifest` fallback intact; do not
iterate `IRNode.attributes` directly when diagnostic order matters because it
is unordered. Use sorted attribute keys for stable output.

The `baked-native` materializer is the direct View-tree lane:
`build_native_view_tree(const DesignIR&, const IRAssetManifest&,
const NativeMaterializeOptions&)` is public in
`<pulp/view/design_import.hpp>`. It calls the shared native resolver, returns a
detached `std::unique_ptr<View>` subtree, and catches API-boundary failures into
diagnostics instead of throwing. Keep image assets routed through
`IRAssetManifest::resolve(asset_id)`; never interpolate raw filesystem paths
from IR attributes. The materialization call site itself should not run JS, but
do not market this as "no JS engine" globally because live React/parity lanes
still use the JS runtime. Baked/native consumers can link `pulp::view-core`;
live import, `ScriptEngine`, `WidgetBridge`, and scripted UI consumers should
link `pulp::view-script` or the full compatibility target, `pulp::view`.

The `baked-cpp` exporter emits native C++ source from the same resolved tree via
`generate_pulp_cpp(const DesignIR&, const IRAssetManifest&,
const CppExportOptions&)`. CLI usage is `pulp import-design --mode baked --emit
cpp --output imported_ui.cpp`; the tool writes the sibling `.hpp` by default.
Generated code should contain direct widget construction, stable anchor IDs,
token/asset constants, `bake_asset_manifest()`, and TODO comments for unresolved
audio parameter or meter bindings. Preserve duplicate token names even when
their values alias, and emit non-hex semantic color tokens as strings rather
than trying to parse them as colors.

The Phase 5/7/9 benchmark harness lives at `pulp-design-import-bench` and is driven
by `tools/scripts/design_import_benchmark.py`. Run it under no-launch env
(`PULP_DISABLE_PLUGIN_EDITOR=1 PULP_HEADLESS=1 PULP_TEST_MODE=1
PULP_INSPECTOR_NO_LAUNCH=1`). It compares `live`, `baked-native`, and
`baked-cpp` lanes, emits startup/idle/interactive metrics, and computes the
Phase 9 gate from linked text+data section size using `size`/`llvm-size`; after
the target split it tracks live-runtime objects under `pulp-view-script`. Do
not use Debug object-file byte counts as the gate input. A valid report must
record the explicit binary-size delta and whether JS evaluation churn is
actually the dominant bottleneck for the measured fixture. Keep the legacy
top-level `comparison` entry pointed at `baked-native`, and add per-lane
results under `comparisons` for both baked lanes.

### Step 1: Identify source and input

Ask the user or detect from context:
- **Source**: figma, stitch, v0, pencil, rn, claude, or designmd
- **Input**: file path, URL, or MCP live data (manual file only for claude; static file only for designmd)

### Step 2: Read the design data

**Figma (MCP available)**:
- Use `com.figma.mcp` to read the current file or selection
- Extract frames, auto-layout, fills, strokes, effects, text, components

**Figma Make runtime-import parser (Phase 6.6.3)**:
- The runtime-import lane accepts constrained, sanitized Figma Make React exports through `parse_figma_make_react()` and `source: 'figma'`. It normalizes the TSX file into the same bundle payload shape used by Claude and v0 runtime import.
- Accepted input is a single `.tsx`/`.jsx` React component with explicit Figma provenance, no `"use client"` directive, React hook imports from `react` only, and inline `style={{ ... }}` objects.
- Raw Figma Make defaults are intentionally rejected until a preprocessing step exists: unresolved `figma:asset/*` imports, versioned import paths like `<package>@<semver>`, Tailwind `className` utilities, Radix primitives, Code Connect glue files, Next.js wrappers, custom JSX components, non-range inputs, and network/storage/worker APIs.
- Representative fixtures live under `planning/fixtures/figma/`; the primary one is `level-meter-panel.tsx`. Run `tools/import-validation/figma-roundtrip.sh --parser-only` for the parser/dispatch gate, `tools/import-validation/figma-roundtrip.sh` for parser-emitted screenshot diff, and `tools/import-validation/figma-roundtrip.sh --coverage` before pushing parser PRs.

**Stitch (MCP available)**:
- Use `mcp__stitch__list_screens` to show available screens
- Use `mcp__stitch__get_screen` to read the selected screen
- Extract component tree, inline styles, design system tokens

**Pencil (MCP available)**:
- Use `mcp__pencil__get_editor_state` to check current file
- Use `mcp__pencil__batch_get` to read the node tree
- Use `mcp__pencil__get_variables` for design tokens
- Use `mcp__pencil__get_style_guide` for style references

**v0 (URL or TSX file)**:
- If URL provided, fetch the v0 generation
- If TSX file, read directly
- Extract JSX tree, Tailwind classes, shadcn/ui components

**DESIGN.md (Google design.md, Apache-2.0)**:
- DESIGN.md is a design *system* spec (YAML frontmatter + Markdown body), not a screen export. There is no screen tree to render; output is `tokens.json` only.
- Run `pulp import-design --from designmd --file path/to/DESIGN.md --tokens tokens.json`. **No `ui.js` is written** — the dispatch arm skips the codegen step. No bridge scaffold either.
- Detection is strict (all-of fingerprint, 95% min-confidence): filename `DESIGN.md` + `---` frontmatter fence + `name:` key + at least one of `colors`/`typography`/`rounded`/`spacing`/`components`. Generic Jekyll/Hugo blog posts will not match.
- Diagnostics: structured `[severity] code at path (line:col): message` on stderr. Exit codes — 0 OK, 1 usage/write, 2 detect-only no match, 3 parse error (malformed YAML, duplicate `##` section heading), 4 unsupported.
- See `docs/reference/imports/designmd.md` for the full reference (supported subset, reference-resolution rules, attribution).

**v0.dev runtime-import parser (Phase 6.6.2)**:
- The runtime-import lane accepts constrained v0 React exports through `parse_v0_dev_react()` and `source: 'v0'`. It normalizes the artifact into the same bundle payload shape used by Claude runtime import.
- Accepted inputs: a bare single-file `.tsx`/`.jsx` React component, or a v0 code-project envelope containing `[V0_FILE]tsx:file="..."` blocks. Prefer the explicit `source: 'v0'` route; standalone TSX has no reliable v0-only marker.
- C-1 deliberately rejects default v0 surfaces that are outside the supported matrix: Tailwind `className` output, shadcn/Radix components, Next.js wrappers/imports, custom JSX components, non-range inputs, and network/storage/worker APIs. Use inline React `style` objects plus the supported DOM/CSS/API subset.
- Representative fixtures live under `planning/fixtures/v0-dev/`; the primary one is `audio-control-panel.tsx` (audio control panel + canvas meter). Run `tools/import-validation/v0-roundtrip.sh --parser-only` for the parser/dispatch gate.

**Google Stitch runtime-import parser (Phase 6.6.4)**:
- The runtime-import lane accepts constrained Stitch React exports through `parse_stitch_react()` and `source: 'stitch'`. Stitch has no reliable standalone file marker, so do not auto-detect arbitrary TSX as Stitch.
- Accepted input is a single-component React TSX module from Stitch's vanilla/inline-style export path. The default Tailwind export is out of scope until the shared Tailwind-to-inline-style preprocessor exists.
- C-3 deliberately rejects Tailwind `className`, external CSS imports, Next.js wrappers or `"use client"`, Radix/shadcn components, React Native imports, Stitch MCP JSON node trees, custom JSX components, non-range inputs, and network/storage/worker APIs.
- Representative fixtures live under `planning/fixtures/stitch/`; the primary one is `transport-bar.tsx` (transport controls + range sliders + canvas VU meter). Run `tools/import-validation/stitch-roundtrip.sh --parser-only` for the parser/dispatch gate.

**JSX-instrument runtime-import (experiment slice, 2026-05-17, planning/2026-05-17-jsx-instrument-import.md)**:
- Unlike v0/figma/stitch, the `jsx` source is NOT a synthetic shape-counter — it executes the user's real React tree. `parse_jsx_react(bundle_js, component_name)` wraps a pre-compiled IIFE bundle (esbuild output of React + ReactDOM + user JSX + nav/document sandbox shims) as a synthetic `ClaudeBundle`, then the existing `parse_claude_html_with_runtime` harness materializes it into a `DesignIR` via the live React reconciler + web-compat DOM shim. **Per Codex/RepoPrompt review:** custom inline-defined components (knobs, faders, etc.) materialize as their underlying SVG primitives — DO NOT widget-promote them to native `<Knob>`/`<Fader>`, which would lose visual parity with the source JSX.
- The JSX→JS compile happens in Node, not in the C++ runtime. Run `tools/import-design/jsx-runtime/jsx-transform.mjs --in <file>.jsx --out <out>.js` to produce the IIFE bundle. The script ships its own `node_modules` (React 18.3.1 + ReactDOM 18.3.1 + esbuild 0.24.0) at `tools/import-design/jsx-runtime/node_modules/`. First-run `npm install` is required.
- Supported input today: single-file `.jsx` or `.tsx`, default-exported React function component, hooks from `react` only, inline `style={{...}}` objects, SVG primitives (`svg/path/circle/line`), `<input>`/`<button>` form elements (text-input editing is degraded — plain text inputs fall back to a non-editable View per Codex review), `setInterval`/`requestAnimationFrame`/`getBoundingClientRect`. The TypeScript path strips TSX through the Node/esbuild transform before the C++ runtime parser sees the bundle.
- Out of scope until follow-up PRs: window-level `mousemove`/`mouseup` global fan-out (the canonical 2-week gotcha; static render works, interactive drag does not), viewport resize signaling (`window.innerWidth/innerHeight` hard-coded), screenshot-similarity acceptance gate (timers/random would need freezing for determinism), Babel-standalone embedding (replaces the Node shell-out).
- End-to-end harness: `tools/import-validation/jsx-roundtrip.sh` runs the transform + builds + runs the smoke test (`pulp-test-design-import-jsx-runtime` — asserts >9 IR nodes + Chainer-shaped text materializes) + optionally renders a `pulp-screenshot` PNG. Primary fixtures: `planning/fixtures/jsx/chainer-instrument.jsx` (762-line Chainer instrument with 9 inline custom components, SVG, drag, setInterval) and `planning/fixtures/jsx/typed-control.tsx` for TSX stripping.
- The bundle's banner-installed shim is critical: ES module imports get hoisted to the top of esbuild's IIFE body, so the navigator/document/HTML*Element ctor shims MUST be emitted as an esbuild `banner` (which is literally prepended outside the IIFE) — not inline in the entry source. Without that, React-DOM's DevTools UA sniff (`navigator.userAgent.indexOf("Chrome")`) crashes during module init. Mirrors the pre-payload shim block in `run_claude_bundle_payload_pipeline` (`design_import.cpp:1494`).

**React Native runtime-import parser (Phase 6.6.5)**:
- The runtime-import lane accepts constrained single-file RN component exports through `parse_react_native_export()` and `source: 'rn'`. The import `from 'react-native'` is unambiguous, so runtime dispatch may auto-detect RN when the source label is omitted.
- Accepted input is a single TSX component with React imports from `react`, RN imports from `react-native`, RN element vocabulary (`View`, `Text`, `Pressable`/`Touchable*`, `ScrollView`, `TextInput`), and `StyleSheet.create({...})` styles. Numeric RN style values are treated as CSS pixels.
- C-4 deliberately rejects native/device APIs and wrappers outside the matrix: `Animated`, Reanimated, Linking, Alert, AsyncStorage, Dimensions/Platform branching, Modal, virtualized lists, navigation, Expo modules, `NativeModules`, `requireNativeComponent`, image sources, DOM tags, and array-form styles.
- RN defaults `flexDirection` to column; the parser-emitted bundle preserves that by injecting column flex semantics into the normalized DOM surface. Representative fixtures live under `planning/fixtures/rn/`; the primary one is `gain-stage.tsx`. Run `tools/import-validation/rn-roundtrip.sh --parser-only` for the parser/dispatch gate, `tools/import-validation/rn-roundtrip.sh` for parser-emitted screenshot diff, and `tools/import-validation/rn-roundtrip.sh --coverage` before pushing parser PRs.

**Pencil runtime-import parser (Phase 6.6.6)**:
- The runtime-import lane accepts constrained Pencil/OpenPencil React exports through `parse_pencil_react()` and `source: 'pencil'`. Pencil has no reliable standalone file marker, so do not auto-detect arbitrary TSX as Pencil.
- Accepted input is the sanitized post-preprocessor form of Pencil's Tailwind JSX export: a single React component with Tailwind classes expanded to inline `style` objects and `--pencil-*` tokens resolved to literals. The MCP JSON node-tree path stays with the offline Pencil adapter and is not a runtime-import source parser.
- C-5 deliberately rejects Tailwind `className`, unresolved `--pencil-*` token references, MCP JSON envelopes, `.pen`/`.fig` binary references, external CSS imports, Next.js wrappers or `"use client"`, Radix/shadcn components, React Native imports, custom JSX components, non-range inputs, and network/storage/worker APIs.
- Representative fixtures live under `planning/fixtures/pencil/`; the primary one is `gain-stage-card.tsx` (gain slider + canvas level meter + bypass). Run `tools/import-validation/pencil-roundtrip.sh --parser-only` for the parser/dispatch gate, `tools/import-validation/pencil-roundtrip.sh` for parser-emitted screenshot diff, and `tools/import-validation/pencil-roundtrip.sh --coverage` before pushing parser PRs.

**Claude Design (manual HTML export — pulp #468)**:
- Anthropic Labs has no MCP / public API. The user runs Claude Design, exports the canvas as Standalone HTML (or "Send to Local Coding Agent"), and hands you the resulting file.
- Run `pulp import-design --from claude --file <path>` — the parser delegates to the Stitch HTML pipeline and tags the IR as Claude. **This is the static path** — it sees only the loader-shell HTML wrapping the bundled React app (~9 elements: title, bundler placeholders, inline styles, the `<script>` blob).
- Add `--execute-bundle` to invoke the **native-runtime path**: Pulp parses the JSON envelope, decodes the gzip+base64 asset map, evaluates the React + React-DOM + app payloads in a headless `ScriptEngine`, then walks the materialized DOM into the `DesignIR`. Falls back to the static path on any harness failure (engine error, walker output below the 9-node loader-shell floor, JS payload too large). Use this when the user's Claude export is a real bundled-React app and they need the actual editor tree, not just the shell.
- The CLI also writes a `bridge_handlers.cpp` scaffold next to the generated JS (override path with `--bridge-output`, skip with `--no-bridge-scaffold`). The scaffold demonstrates registering `pulp::view::EditorBridge` handlers and attaching to a `WebViewPanel` (or future `JsRuntime`).

**Inline `<script>` evaluation in `--execute-bundle` (pulp #758)**: The harness now evaluates inline `<script type="text/javascript">` (and untyped `<script>`) blocks AFTER the src-loaded payloads, then compiles + evaluates inline `<script type="text/babel">` (and `text/jsx`) blocks via the bundle's own Babel-standalone (looked up as `globalThis.Babel.transform`). After both, the harness dispatches a `readystatechange` → `DOMContentLoaded` → `readystatechange(complete)` → `window.load` sequence and pumps four message-loop / frame-callback cycles for async settling. This is what makes a real Spectr-style Claude bundle (where the actual React app lives in inline `text/babel` blocks, not src-loaded payloads) materialize beyond the 9-element shell. Per-script soft-fail matches the existing src-loaded payload pattern. Inline `application/json` (and other `*/json`) blocks are intentionally skipped — they're config blobs, not executable code.

**Gotchas that bit pulp #758 implementation:**
- `core/view/js/web-compat.js`'s `document` is a plain object literal (not an `Element`), so it ships **without** `addEventListener` / `dispatchEvent`. The Step 3 dispatcher constructs events defensively — uses `new Event(t)` when available, falls back to `{type, target, bubbles:false, preventDefault, stop*}` literal otherwise — but bundles that call `document.addEventListener('DOMContentLoaded', ...)` only fire if the bundle (or some library it loads) installs `addEventListener`/`dispatchEvent` on `document`/`window` first. Real React-DOM does not do this — it attaches to the `root` element it controls — so the DCL dispatch is a best-effort safety net, not a guarantee. See test `DOMContentLoaded dispatch runs the queued handler when document supports it` in `test/test_design_import_inline_babel.cpp` for the exact shim shape that satisfies the contract.
- The harness's `error_out` is the **fallback reason** (or empty on success). Don't piggyback diagnostic warnings on it from inside the harness; on success the harness clears the slot. If you need to surface a warning that survives a successful run, push it through a different channel (e.g. add a `warnings` vector to `ClaudeRuntimeOptions`).
- Empty `<span>` (and other text-mapped tags: `p`, `label`, `h1`-`h6`, `a`, `strong`, `em`, `small`, `code`) get filtered out by the text-empty pruning in `json_to_ir_node`. If a fixture or test relies on observing an empty `<span>` with attributes round-tripping through the IR, use a `<div>` instead — divs map to `frame` and survive the prune.
- Babel-standalone's loaded test: probe `typeof globalThis.Babel.transform === 'function'` rather than `typeof Babel`. Some bundles install `Babel` as a sentinel object before the real implementation arrives, which would false-positive on the looser check.

**@pulp/react bundle dedup:** when emitting React+@pulp/react consumer bundles (Spectr et al.), the consumer's bundler MUST be able to dedup React across the @pulp/react boundary. This means @pulp/react's published `dist/index.mjs` must externalize `react`, `react-reconciler`, `react-reconciler/constants.js`, and `scheduler` — otherwise esbuild emits TWO independent React module instances (one for user code, one for the reconciler) and `ReactCurrentDispatcher.current` desyncs at first commit, manifesting as "cannot read property 'useState' of null" inside the user's `App()`. The fix is a 1-line addition to `packages/pulp-react/package.json`'s `build` script: `--external:react --external:react-reconciler --external:react-reconciler/constants.js --external:scheduler`. This must hold for any future package emitted from `pulp import-design` that pulls in @pulp/react.

**Spectr's `<svg><path>` doesn't auto-route to `<SvgPath>`:** Pulp v0.69.2+ ships an `<SvgPath>` JSX intrinsic that maps to the C++ `SvgPathWidget` shipped in v0.61.0 (#965/#991). However, plugin bundles emitted from Claude-Design exports (and similar) ship raw `<svg><path/></svg>` markup, not `<SvgPath>`. There's no automatic shim — the dom-adapter (or a future `pulp import-design` post-process) must rewrite `<svg>` → `<SvgPath>` for inline-icon use cases. Track plugin-side adoption when bumping SDK pin past v0.69.2. <!-- docs-noise-lint: skip — retained version provenance for SvgPath rollout -->

**v0.69.0 closes 4 v0.68.0 audit symptoms automatically:** segmented-control vertical stacking (was the most-visible Spectr UX gap) is closed by `display:flex` defaulting to `flex-direction:row` (#1167); FilterBank canvas was already auto-resolved at v0.68.1+; App-root layout-bottom-strip is closed by the same flex-direction default; click-bubble dispatch fully closed in v0.68.0 (#1008/#1073). When auditing a freshly-imported plugin against an older SDK reference, run the WebView↔Native side-by-side at idle FIRST — many "broken" rows resolve via SDK upgrade alone with zero plugin-side work. Pattern documented in `spectr/planning/audit-2026-05-03-webview-vs-native-v0.69.1.md`.

**JS string-literal escaping for emitted user text:** when `core/view/src/design_import.cpp` emits user-supplied text into single-quoted JS literals (`createLabel('...')`, `var.textContent = '...'`), the text MUST go through `js_single_quote_escape()` — newlines, single quotes, and backslashes leak through otherwise and `pulp-screenshot` crashes with "unexpected end of string" at JS eval time. The helper sits next to the existing `v0_html_attr_escape()` and covers the six emission sites that take arbitrary text: four `createLabel(text)` calls in the audio-widget column path, the generic text-node `createLabel(text_content)`, and the web-compat `var.textContent = '...'` line. Pinned by `[issue-81]` in `test/test_design_import.cpp`, which asserts both the absence of unescaped patterns AND the parity of unescaped single-quote counts on every emitted `createLabel` line. If you add a new emission site that takes user-supplied text, route it through `js_single_quote_escape()` AND extend the test.

**File-based fallback**:
- Read the file and parse based on --from source type

## Source-Contracts Registry

Pulp keeps a permissive, machine-checkable source-contract registry at
`tools/import-validation/source-contracts.json`. It records each provider's
upstream anchors, runtime/static parser symbols, fixture paths, roundtrip
script, test tags, MCP lane, and minimum runtime surface references. This file
does not replace `compat.json`: `compat.json` still owns detection fingerprints,
parser-version, format-version, and compat-schema-version.

Run the warn-only checker after changing import parsers, source fixtures, or
roundtrip scripts:

```bash
python3 tools/import-validation/check-source-contracts.py
python3 tools/import-validation/check-source-contracts.py --format markdown
python3 -m pytest tools/import-validation/test_source_contracts.py -v
```

The checker also enforces coverage symmetry between
`parse_design_source()` labels and the registry. When adding a source label
such as `jsx`, add a matching row to `source-contracts.json` and reference
its roundtrip script there; otherwise pre-push will fail strict
source-contract validation.

**`parser.runtime_file` — runtime parsers extracted out of `design_import.cpp`.**
Each contract's `parser` block has a single `file` plus `runtime`/`static`
symbol names. After the P6-A3 refactor the *runtime* parsers
(`parse_*_react`, `parse_claude_html_with_runtime`, `parse_react_native_export`)
moved into `core/view/src/claude_bundle.cpp` while the *static* parsers stayed
in `design_import.cpp`. A single `parser.file` can no longer locate both, so
contracts whose runtime parser lives elsewhere set the optional
`parser.runtime_file`. The Phase 7 follow-up split `claude_bundle.cpp` again:
the per-design-tool source-detection families and their five public
`parse_*_react` entry points (`parse_v0_dev_react`, `parse_figma_make_react`,
`parse_stitch_react`, `parse_react_native_export`, `parse_pencil_react`) now
live in `core/view/src/claude_bundle_sources.cpp`; only `parse_jsx_react` and
`parse_claude_html_with_runtime` stayed in `claude_bundle.cpp`. So a contract's
`runtime_file` is now `claude_bundle_sources.cpp` for those five and
`claude_bundle.cpp` for the JSX/Claude-HTML pair. The checker
resolves `parser.runtime` and the `explicit-runtime-parser` dispatch symbol
against `runtime_file` (falling back to `parser.file`); `parser.static` always
resolves against `parser.file`. **If a future refactor moves a parser symbol
to a new file, update `parser.file` / `parser.runtime_file` in the same PR** —
otherwise the `Source-contract registry check` step fails for every PR.

Provider MCP lanes are input-acquisition lanes only unless the source contract
explicitly says otherwise. Current runtime parsers reject raw Figma/Stitch/Pencil
MCP JSON and accept only their constrained exported artifacts.

**Where the `runtime-import-dispatch` tests live (2026-05-17 P5-1 split):**
the `WidgetBridge::install_runtime_import_handlers` tests for Figma /
Stitch / v0 / Pencil / RN were moved out of `test/test_widget_bridge.cpp`
into a sibling `test/test_widget_bridge_runtime_import.cpp` so the 14k-line
god-test file can shrink toward per-surface modules. Both files are
listed in `source-contracts.json`'s per-source `test_files` arrays, and
both targets (`pulp-test-widget-bridge` and
`pulp-test-widget-bridge-runtime-import`) are registered in
`test_targets`. When adding a new runtime-import dispatch test, place
it in the runtime-import sibling — keeping the parent file shrinking.

### Step 3: Translate to Pulp code

Use the appropriate mapping document as your translation reference:
- `planning/figma-to-pulp-mapping.md` for Figma designs
- `planning/stitch-to-pulp-mapping.md` for Stitch screens
- `planning/v0-to-pulp-mapping.md` for v0 generations
- `planning/pencil-to-pulp-mapping.md` for Pencil designs

Generate Pulp web-compat JavaScript:
- Layout: `document.createElement('div')` + `el.style.flexDirection`, etc.
- Typography: `el.style.fontSize`, `el.style.fontWeight`, etc.
- Colors: `el.style.backgroundColor`, `el.style.color`, etc.
- Audio widgets detected by name: knob/dial → `createKnob()`, fader/slider → `createFader()`, meter/level/vu → `createMeter()`, xypad → `createXYPad()`, waveform → `createWaveformView()`, spectrum/analyzer → `createSpectrumView()`
- Design tokens: `theme.colors["name"] = value`, `theme.dimensions["name"] = value`

### Step 4: Write output files

- Write the generated JS to `ui.js` (or user-specified output path)
- Extract design tokens to `tokens.json` in W3C Design Tokens format
- Report: number of elements, number of tokens, any warnings

### Step 5: Offer refinement

After generating, offer to:
- Adjust specific elements ("make the knob smaller")
- Add audio widgets ("add a meter next to the gain knob")
- Change theme tokens ("use darker background colors")
- Preview the UI if a preview tool is available

For interactive review of the current checkout, prefer:

```bash
pulp design
```

If the design tool lives in a nonstandard worktree/build setup, use:

```bash
pulp design --build-dir /path/to/build --script /path/to/design-tool.js
```

When run outside a Pulp checkout, automatic binding currently only works when the `pulp` binary
itself lives inside a Pulp build tree. In generic PATH-installed or split repo/SDK layouts, pass
`--build-dir` and `--script` explicitly.

## Mapping Quick Reference

### Figma → Pulp
| Figma | Pulp |
|-------|------|
| Frame (auto-layout) | `div` with flex |
| Text | `span` |
| Rectangle | `div` with background |
| Component | JS function |
| Fill (solid) | `backgroundColor` |
| Fill (gradient) | `background: linear-gradient(...)` |
| Stroke | `border` |
| Drop shadow | `boxShadow` |
| Corner radius | `borderRadius` |

### Stitch → Pulp
| Stitch | Pulp |
|--------|------|
| Container | `div` with flex |
| Text | `span` |
| Button | `button` |
| Input | `input` |
| Card | Panel |
| Design system colors | `theme.colors` |

### v0 → Pulp
| v0 (Tailwind) | Pulp |
|----------------|------|
| `flex flex-col` | `flexDirection: 'column'` |
| `gap-4` | `gap: '16px'` |
| `bg-slate-900` | `backgroundColor: '#0f172a'` |
| `rounded-lg` | `borderRadius: '8px'` |
| `<Button>` | `createButton()` |
| `<Slider>` | `createFader()` |

### DESIGN.md → Pulp
| DESIGN.md frontmatter | Pulp IR |
|------------------------|---------|
| `colors.<name>: "#hex"` | `IRTokens.colors[name] = "#hex"` |
| `typography.<level>.<field>: ...` | `IRTokens.strings["typography.<level>.<field>"] = ...` |
| `rounded.<size>: <Nx>` | `IRTokens.dimensions["rounded-<size>"] = N` |
| `spacing.<size>: <Nx>` | `IRTokens.dimensions["spacing-<size>"] = N` |
| `components.<name>.<prop>: <ref-or-value>` | `IRTokens.strings["components.<name>.<prop>"] = ...` (refs resolved in-place) |
| `{colors.primary}` | resolved to the primitive hex value at parse time |
| `{typography.<level>}` inside `components` | preserved verbatim (composite ref) |
| `{colors}` (group ref, outside components) | broken-ref warning |
| Markdown body `## Section` | retained in `DesignMdParseResult.sections` for Phase 2 lint |
| Unknown `## Section` (e.g. `## Iconography`) | preserved without error |
| Duplicate `## Section` | error-severity diagnostic → exit 3 |

### Pencil → Pulp
| Pencil | Pulp |
|--------|------|
| Frame (auto-layout) | `div` with flex |
| Text | `span` |
| Rectangle | `div` with background |
| Variables (COLOR) | `theme.colors` |
| Variables (FLOAT) | `theme.dimensions` |

## Stable-Anchor Identity (Phase 0a — inspector round-trip prerequisite)

Every tree-producing parser now stamps `IRNode::stable_anchor_id`,
`source_node_id` (when the source has a native ID), `provenance`
(adapter + version + source_uri), and `confidence` (PASS / DIVERGE /
NOT_IMPL). These fields key the tweaks layer (`pulp-tweaks.json`) so
inspector direct-manipulation edits survive re-import.

**If you add a new parser, you MUST:**

1. After the IR tree is built, stamp `ir.root.provenance` with the
   adapter name (e.g. `"figma"`, `"stitch-html"`, `"pencil"`), the
   adapter version, and the source URI.
2. Stamp `ir.root.confidence`:
   - `IRConfidence::pass` for a clean lowering
   - `IRConfidence::diverge` for lossy / regex / best-effort paths
   - `IRConfidence::not_impl` if the adapter can't lower the source yet
3. Call `assign_anchors(ir.root, strategy, adapter_name)` with the
   strategy that matches your source kind:
   - **adapter** strategy when the source has native stable IDs
     (Figma layer UUIDs, Pencil node IDs, Mitosis content-hash IDs).
     Requires `adapter_name` (becomes the `"<name>:"` prefix) AND
     `source_node_id` populated on each node.
   - **content-hash** strategy when there are no native IDs
     (Stitch HTML, v0 TSX, Claude Design HTML, raw JSX, generic HTML).
     The hash is FNV-1a 32-bit base-36 over (tag, role, normalized
     text, depth, sigIndex).
   - **path** strategy for RN-style file exports / hand-edited code
     where source position is the most stable identity.
4. The strategy default lives in `default_anchor_strategy()` in
   `core/view/include/pulp/view/anchor_strategy.hpp` — mirrors the TS
   `DEFAULT_ANCHOR_STRATEGY` map in `packages/pulp-import-ir/src/anchors.ts`.

**Why this matters:** Phase 1+ of the inspector roadmap writes user
inspector edits to a sidecar `pulp-tweaks.json` keyed by
`stable_anchor_id`. Without populated anchors the tweaks layer has
nothing to match against on re-import — defeating the "edit anywhere,
never lose work" principle. A parser that skips `assign_anchors` will
silently produce a tree where inspector edits get orphaned on the
next re-import.

**Codegen contract:** `generate_pulp_js` (both web-compat and
`bridge_native_js` modes) emits `// @pulp-anchor <id>` trail comments next to each
element when `opts.include_comments == true`. The runtime inspector
parses these to map generated elements back to their tweak-layer
identity. If you write a custom codegen path, preserve this pattern
so the inspector can still trace identity.

**Phase 0b — `setAnchor()` bridge wiring:** the web-compat codegen
path *also* emits a functional `setAnchor(<var>._id, '<anchor>')` call
after each createElement, AND the call is emitted unconditionally —
NOT gated on `opts.include_comments`. Rationale: the
`// @pulp-anchor` trail is cosmetic (for grep / debugging), but
`setAnchor()` is functional — the inspector cannot find a widget's
anchor without it, so dropping it in minified codegen would silently
break inspector tweaks in production. If you write a custom codegen
path, emit both: the comment (gated) for debuggability and the
setAnchor call (unconditional) for the runtime. The bridge side
(`WidgetBridge::setAnchor`) is a silent no-op on unknown widget IDs,
matching the rest of the bridge's tolerance for unmounted ids.

<!-- docs-noise-lint: skip — retained: pins guidance to the PR that introduced the _id contract -->
**Codex P1 follow-up (#2303):** The first argument to `setAnchor` <!-- docs-noise-lint: skip — retained: pins guidance to the PR that introduced the _id contract -->
MUST be the element's internal `_id` (the auto-generated `__el_N__`
that `document.createElement` assigns in `core/view/js/web-compat.js`),
NOT the generated JS variable name. The bridge keys `widget()` lookup
on `_id`; passing the var name silently no-ops and breaks the entire
anchor wiring chain for web-compat imports. Pre-fix codegen emitted
`setAnchor('var', ...)` (broken); post-fix emits `setAnchor(var._id, ...)`
(correct). If you write a new codegen variant or a non-web-compat
shim, ensure whatever id you pass matches the bridge's widget()
lookup key — not the JS-local variable name.

`bridge_native_js` codegen does NOT yet emit `setAnchor` (small
follow-up; the native-bridge JS codegen has many early-return branches
that need each to be wired). `bridge_native_js` is the default codegen
mode for imports; pass `--web-compat` only when the DOM-compatible JS
lane is required.

**Phase 5.1 — `setSource()` source-jump wiring:** alongside `setAnchor`,
the `@pulp/react` reconciler now forwards React's dev-mode `__source`
prop ({fileName, lineNumber, columnNumber}) through a `setSource(id,
file, line, col)` bridge call (`materializeUnder` →
`bindSourceLocation` in `packages/pulp-react/src/host-config.ts`). This
lands a `View::SourceLocation` on the live widget so the inspector's
`J` hotkey / `Inspector.jumpToSource` protocol method can open the
authoring JSX file:line in the user's editor. Gotchas:

- **`__source` is Babel-classic / automatic-dev-runtime only.** esbuild's
  `jsx: 'transform'` mode (current `jsx-transform.mjs` setting) does
  **not** inline `__source` props — only Babel's
  `@babel/plugin-transform-react-jsx-development` or esbuild's
  `jsx: 'automatic'` + `jsxDev: true` do. So today `bindSourceLocation`
  is a silent no-op for transform-mode bundles; it activates the moment
  the JSX runtime emits `__source`. Migrating `jsx-transform.mjs` to the
  automatic dev runtime is the remaining Phase 5.1 follow-up — do it as
  a deliberate, separately-validated change because it shifts
  `jsxFactory` semantics across every existing import fixture.
- **Source maps gate behind `PULP_JSX_SOURCEMAP=1`.** `jsx-transform.mjs`
  emits an inline source map only when that env var is set — off by
  default to keep production bundles lean (~10-30% size add). It is
  independent of the `__source` prop path.
- **`setSource` no-ops on an empty file path and unknown widget id** —
  same tolerance as `setAnchor`. A custom codegen / shim that wants
  source-jump must pass the same bridge-keyed `_id` as `setAnchor`.

### Phase 1 — Tweaks persistence (`pulp-tweaks.json`)

`TweakStore` (`inspect/include/pulp/inspect/tweak_store.hpp`) now reads
and writes a sidecar `pulp-tweaks.json` so inspector edits survive
process restart.

- **File location** — resolved by `TweakStore::default_tweaks_path()`:
  1. `$PULP_TWEAKS_FILE` env var if set (verbatim — useful for tests
     and headless CI runs).
  2. Otherwise walks up from `cwd` looking for a directory containing
     `package.json`; if found uses `<project_root>/pulp-tweaks.json`.
  3. Otherwise `<cwd>/pulp-tweaks.json`.
- **Schema** — `{ "$schema": "pulp-tweaks://v1", "version": 1,
  "tweaks": { anchor: { dottedPath: value } }, "bypassed": { anchor:
  true | string[] }, "sources"?: { anchor: { dottedPath: source } } }`.
  Mirrors `packages/pulp-import-ir/src/tweaks.ts` `TweaksFile` with
  the sibling `bypassed` overlay and `sources` sidecar Phase 1 adds.
  Files written by `@pulp/import-ir` (no integer `version`) load as
  v1 for back-compat; an explicit unknown `version` is a hard error
  so we never silently drop fields we don't understand.
- **Atomic write** — `save_to_disk()` writes `<path>.tmp` and renames
  over the target; no partial flush ever lands at the canonical path.
- **Auto-save** — opt-in via `TweakStore::set_auto_save(true)` or
  `Inspector.setAutoSave { enabled, path? }` over the protocol. OFF
  by default so unit tests don't touch disk by accident.
- **Protocol surface** — `Inspector.loadTweaks { path? }`,
  `Inspector.saveTweaks { path? }`, `Inspector.setAutoSave { enabled,
  path? }`. All three default `path` to `default_tweaks_path()`.

When wiring a new inspector client (web UI, CLI, MCP tool), prefer the
protocol methods over reaching into the C++ store directly — the
defaults + error reporting are the same and you get the resolved path
echoed back in the response for logging.

### Phase 4a — Lock-to-source, Path A (generated-TSX/JS rewrite)

`pulp/view/lock_to_source.hpp` (impl `core/view/src/lock_to_source.cpp`)
is the engine that **promotes a tweak back into the generated import
artifact** so the edit is permanent and survives a fresh re-import.
Path A only — the artifact `pulp import-design` lowers a design into.
Path B (live React-bundle AST patch, #1308) and Path C (DESIGN.md <!-- docs-noise-lint: skip — retained: pins Path B/C scope to their tracking issue -->
token export) are separate phases and are NOT in this engine.

- **How the element is found** — every web-compat element block carries
  the `// @pulp-anchor <id>` trail comment (emitted by `generate_pulp_js`
  when `include_comments` is on). `lock_tweak_into_source()` locates the
  block by that comment, then rewrites or inserts the matching
  `<var>.style.<prop>` assignment. The block ends at the next
  `// @pulp-anchor` comment or the first blank line (codegen emits
  exactly one blank line between elements).
- **Property-path mapping** — `lock_property_to_style_name()` collapses
  the dotted tweak paths (`paint.*`, `style.*`, `layout.*`, or a bare
  name) onto the camelCase `el.style.<name>` surface. Hyphen/snake
  fragments camelCase. The allow-list is exactly the set of properties
  `generate_node()` emits — an unknown / mistyped path reports
  `unsupported_property` instead of writing a bogus assignment.
- **Status semantics** — `rewritten` / `inserted` mutate the text;
  `already_current` is the idempotent re-lock no-op; `anchor_not_found`
  and `unsupported_property` are graceful failures that leave the
  source byte-identical so the caller keeps the tweak in the sidecar.
- **`@generated` boundary guard** — `is_generated_source()` is the
  cheap check for the roadmap's "only lock into files marked
  `@generated`" rule. It accepts both the Pulp codegen banner
  (`// Generated by Pulp import-design …`) and a conventional
  `// @generated` marker. The CLI / inspector layer owns the
  read-confirm-write loop; the engine is pure text-in / text-out.
- **Round-trip contract** — locking a tweak into the generated text
  produces exactly the text `generate_pulp_js` would emit had the IR
  carried the tweaked value all along. `test_lock_to_source.cpp`
  pins that byte-for-byte.

If you add a codegen property to `design_codegen.cpp`'s `generate_node`,
add it to the `kKnown` allow-list in `lock_property_to_style_name()` too
— otherwise that property can never be locked to source.

### Phase 4b — Lock-to-source, Path B (hand-authored JSX/TSX patch)

`pulp/view/jsx_lock.hpp` (impl `core/view/src/jsx_lock.cpp`) is the
Path B sibling of Path A; the roadmap tracks Path B under issue #1308. <!-- docs-noise-lint: skip — retained: pins Path B scope to its tracking issue -->
Where Path A rewrites the *generated* web-compat artifact, Path B
patches the user's **own hand-authored JSX/TSX** — the source behind a
live React bundle (`--from jsx`, `--execute-bundle`). There is no
generated file to rewrite, so the engine edits the authored source
directly.

- **How the element is found** — an element-instrumentation pass injects
  a `data-pulp-anchor="<stable_anchor_id>"` attribute onto each authored
  element (the JS-side instrumentation is a separate deliverable; the
  engine only *consumes* the marker). `jsx_lock_tweak_into_source()`
  scans for the matching attribute, walks left to the opening `<` and
  right to the tag-closing `>` (respecting strings and `{…}` braces so a
  `>` inside `{a > b}` does not end the tag early).
- **Surgical patch, not an AST re-emit** — mirrors 4a/4c. The engine
  rewrites exactly one literal span: a property inside an inline
  `style={{…}}` object, or a bare attribute (`width={80}`,
  `color="#888"`). Every other byte — comments, imports, formatting,
  sibling props — is preserved byte-for-byte. It is deliberately NOT a
  general JSX printer (Codex capped Path B from ballooning into a
  multi-quarter parser).
- **`too_dynamic` is the safety valve** — anything that is not a plain
  rewritable literal fails as `too_dynamic` with a specific reason, and
  the source is returned unchanged so the caller keeps the tweak in the
  sidecar: a `style={{ ...base }}` spread, a computed key, a non-literal
  value (`padding={gap * 2}`, `color={theme.fg}`), `style={someVar}`, or
  a prop the author simply never wrote (Phase 4b patches *existing*
  props only — it never inserts, which would risk a malformed tag).
- **Other statuses** — `patched` mutates; `already_current` is the
  idempotent no-op; `anchor_not_found` and `anchor_ambiguous` (the same
  `data-pulp-anchor` on two elements — refuse to guess) and
  `unsupported_property` are graceful failures that leave the source
  byte-identical.
- **Value rendering** — a quoted prop keeps its quote style (contents
  rewritten, single-quotes escaped for a JS string body); a bare numeric
  prop stays bare when the new value is numeric, but is promoted to a
  quoted string when the new value carries a unit (`width={80}` →
  `width={'120px'}`).
- **`jsx_lock_property_to_key()`** shares the same `kKnown` allow-list as
  Path A's `lock_property_to_style_name()` — keep the two in sync so a
  tweak can target the same set of properties on either path.

The engine is pure text-in / text-out — no filesystem I/O — so the
overlay / CLI layer owns the read-confirm-write loop and the
authored-vs-generated routing (`is_authored_jsx_source()` is the cheap
guard: a file carrying the codegen banner is Path A's, not Path B's).
`test_jsx_lock.cpp` pins the patch, the formatting-preservation
contract, and every failure path.

### Phase 4c — token lock-to-source (`DESIGN.md` rewrite)

`token_lock.hpp` / `token_lock.cpp` (`core/view/`) lock a *token-typed*
inspector tweak back into the project's `DESIGN.md` so a re-import picks
up the corrected token instead of re-introducing the stale one. This is
the token-level sibling of Phase 4a (generated-TSX rewrite) and 4b
(JSX/TSX AST patch).

- **Token vs element classification** — `classify_token_tweak(anchor,
  property_path)` returns a `TokenTarget` when the tweak addresses a
  DESIGN.md token, `std::nullopt` for element-only tweaks (`paint.*`,
  `layout.*`, `text.*` — those lock via 4a/4b). Two signals, in
  priority order: a dotted property path whose head is a canonical
  token group (`colors` / `spacing` / `rounded` / `typography`), or a
  `designtoken:<group>.<name>` anchor. Typography paths must be
  three-segment (`typography.<level>.<field>`). `components.*` is
  deliberately **not** lockable — a component entry is a reference
  bundle, not a primitive value.
- **Surgical text rewrite, not `export_designmd`** — the lock parses
  the YAML frontmatter only to *locate* the token (yaml-cpp `Mark()`
  gives the value line), then edits exactly that one value span in the
  original file text. Prose sections, YAML comments, key order,
  indentation, and the value's original quote style are all preserved.
  `export_designmd(Theme, ...)` re-emits the whole file and would lose
  every one of those — never use it for a single-token lock. (It also
  still throws, gated on #1307.)
- **Conservatism** — `lock_token_in_designmd` fails (and returns the
  input byte-identical) on: no frontmatter, missing group/token/field,
  a nested color palette (not a scalar), or a source line that does not
  match the expected `key: <value>` shape. A failed lock never mutates
  DESIGN.md. The locator keys off the *group*, so a leaf name that
  appears in two groups (e.g. `md` under both `spacing` and `rounded`)
  still resolves unambiguously.
- **Overlay wiring deferred** — the inspector overlay "lock token"
  affordance is intentionally not wired here (`inspector_overlay.cpp`
  has in-flight PRs). The engine is pure data-in/data-out so the
  overlay/protocol layer can adopt it without a rebuild.

Spec + design:
[`planning/2026-05-18-inspector-direct-manipulation-roadmap.md`](../../../planning/2026-05-18-inspector-direct-manipulation-roadmap.md)

## Automated Validation Loop

### Freshness check (MUST run first)

Before running any roundtrip harness against the framework, **verify your checkout is current with `origin/main`**. Lesson from pulp #2087: a roundtrip ran from a 175-commit-behind feature branch and produced "wrong UI variant" diff scores that reflected stale framework code, not main. We spent 15+ minutes drawing parser conclusions before noticing.

The `tools/import-validation/*-roundtrip.sh` scripts now refuse to run on a stale checkout. Bypass only when you specifically want to validate a feature branch:

```bash
# Default: refuse to run if HEAD is behind origin/main
tools/import-validation/spectr-roundtrip.sh

# Explicitly allow staleness (e.g., validating a feature branch's code)
PULP_FRESHNESS_BYPASS=1 tools/import-validation/spectr-roundtrip.sh

# Or accept up to N commits behind
tools/scripts/check_workspace_freshness.sh --max-behind 10 && tools/import-validation/spectr-roundtrip.sh
```

Also verify the **installed SDK** matches your expectations:
```bash
pulp sdk status              # what's installed
pulp doctor --versions       # CLI vs project vs installed
```
If you ran `pulp upgrade` recently, the CLI bumped but the SDK might not have. Use `pulp sdk install` to pull the latest SDK matching the CLI.

### Diff loop

After generating Pulp code, ALWAYS validate by comparing with the source design:

1. **Screenshot the source design** via MCP:
   - Pencil: `get_screenshot(nodeId)`
   - Save to a temp file as the reference

2. **Render the generated JS** headlessly:
   ```bash
   pulp-screenshot --script generated.js --output render.png --width W --height H
   ```

3. **Compare** reference vs render:
   ```bash
   pulp import-design --from X --file input --validate --reference source.png --diff diff.png
   ```

4. **Review the diff image** — red highlights show differences

5. **Iterate if needed** — adjust the generated code and re-render until similarity is acceptable (>85%)

### Keyboard shortcut extraction (UX best-practice default)

The library function `extract_keyboard_shortcuts(source, filename)` scans
imported React source for inline `onKeyDown={e => ...}`,
`window.addEventListener('keydown', ...)`, and `if (e.key === 'X')` patterns,
returning a `std::vector<DetectedShortcut>` for the design-import emitter
to register via Pulp's runtime `registerShortcut(key, modifiers, callback)`
surface. Modifier idioms (`metaKey || ctrlKey`) collapse to a single `meta`
entry per the cross-platform shortcut convention. Use
`serialize_detected_shortcuts()` to emit the stable JSON manifest.

The matcher is **lexical only** — handler bodies that reference React state
(e.g. `setActiveTab(2)`) can't be auto-wired yet; the manifest surfaces them
for human triage via a `handler_excerpt` field. V1 (this slice) only emits
the manifest; follow-up slices wire the CLI flag and emit `registerShortcut(...)`
into the generated JS. Default-on with a planned `--no-import-shortcuts` opt-out.

### Yoga Layout Rules (MUST follow)
- Every container needs explicit `height`, `min_height`, or `flex_grow`
- Labels need `min_height` (14px for normal text, 12px for small)
- Faders need `min_width >= 40px` for thumb rendering
- Meters need `min_width >= 20px` for bar visibility
- Knobs need `min_size >= 56px` for arc rendering
- Use `createCol`/`createRow` for containers (NOT `createPanel` which adds glass overlay)
- Row height = max child height; Column height = sum of child heights + gaps

### Proportional resize for fixed-design native-react imports (pulp #59/#63/#64/#65)

When you import a design that was authored at a known fixed size (Spectr's
editor.js at 1320×860, a Figma frame at 1440×900, etc.) and want the live
window to resize proportionally **without** re-layout, the right primitive
is `WindowHost::set_design_viewport(w, h)`:

```cpp
auto window = WindowHost::create(root, opts);
window->set_design_viewport(kDesignWidth, kDesignHeight);
window->set_fixed_aspect_ratio(kDesignWidth / kDesignHeight);
```

What it does: pins root.bounds at design size on every paint, applies an
aspect-correct scale + letterbox translate so the design fits inside the
current window, inverse-maps mouse coords before hit-test. The window can
change size; root never knows.

**Do not** try to solve proportional resize for fixed-design imports with:

1. **Per-child `set_scale()` on root children.** Scales chrome but
   `CanvasWidget` records its draw commands at the original size, so
   `<canvas>` content gets clipped on shrink. Tried and burned through 2026-05-13/14.
2. **Yoga `absolute + inset:0` propagation.** Chains of
   `position:absolute + inset:0` collapse to 0×0 in Pulp's runtime-import
   because Yoga only fills a containing block when the parent has a
   definite POINTS size — the cascade root→body→wrap→canvas never gets
   one. This is **architectural** (Yoga is flex+grid only), not a bug.
3. **JS-driven canvas refit via React refs.** Even with `getPublicInstance`
   correctly returning a DOM-shim Element so refs match the element that
   `getBoundingClientRect` queries, many native-react `resize()` functions
   (Spectr's included) bail on `wrapRef.current`/`canvasRef.current`
   existence checks and never run.

The design-viewport approach sidesteps all three by doing the resize at the
renderer (paint-time scale of the design surface), which is what a browser
webview effectively does at the layer level.

## CLI Alternative

The deterministic import tool is also available:
```bash
pulp import-design --from figma --file design.json
pulp import-design --from stitch --file screen.html
pulp import-design --from v0 --file component.tsx
pulp import-design --from pencil --file design.json
pulp import-design --from claude --file design.html   # writes ui.js + tokens.json + classnames.json + bridge_handlers.cpp (static parser — loader-shell only)
pulp import-design --from claude --file design.html --execute-bundle   # runs the bundled React app in QuickJS, walks the materialized DOM (#468)

# With validation
pulp import-design --from pencil --file design.json --validate --reference source.png --diff diff.png

# Override or skip the bridge scaffold (claude only)
pulp import-design --from claude --file design.html --bridge-output editor/handlers.cpp
pulp import-design --from claude --file design.html --no-bridge-scaffold

# Override or skip the classnames artifact (claude only — pulp #1035)
pulp import-design --from claude --file design.html --classnames editor/classnames.json
pulp import-design --from claude --file design.html --no-emit-classnames
```

Import artifact flag vocabulary:
- `--output <path>` is the destination for the primary artifact. JS defaults to `ui.js`; `--emit cpp` defaults to `imported_ui.cpp` and writes the sibling header.
- `--emit js`, `--emit ir-json`, and `--emit cpp` are implemented. `cpp` requires `--mode baked`. Legacy `--emit classnames` remains accepted for the Claude classnames sidecar.
- `--mode live` is the default. Static sources emit generated JS in live mode; `--from jsx --mode live --emit js` writes the precompiled JSX bundle verbatim. `--mode baked` emits canonical IR or baked C++ via `--emit ir-json|cpp`.
- `--snapshot-semantics fail|warn|accept` is honored for JSX baked IR snapshots. `fail` rejects dynamic APIs by default, `warn` proceeds with a structured diagnostic, and `accept` proceeds silently. The scan covers timers, animation frames, clock/random APIs, and fetch while ignoring comments and string literals.
- URL imports fetch through argv-safe `curl` into a unique temporary file; literal `--file` paths are read directly and may contain normal filesystem punctuation, while `--url` rejects shell metacharacters before fetching.

Use `--dry-run` to preview without writing files.

Detect-only directory inputs (`pulp import-design --detect-only --directory <dir>`) prefer
`code.html`, then `index.html`, then the first sorted `.html` / `.htm` payload. Keep fixture
tests on that deterministic order; raw `std::filesystem::directory_iterator` order differs
between macOS and Linux.

## Bridge Handler Scaffold (Claude Design only)

For `--from claude`, the CLI emits a starter C++ file demonstrating how to wire `pulp::view::EditorBridge` so the imported design's editor JS can `postMessage` into the C++ processor:

- Replace the `MyPluginEditor` placeholder with the editor class that owns the `WebViewPanel`.
- Register one `bridge_.add_handler("type", ...)` per message type your editor emits. Use `EditorBridge::get_float / get_uint / get_string` for safe payload reads, and `EditorBridge::ok_response() / ok_response(extras) / err_response(msg)` for replies.
- Call `bridge_.attach_webview(*panel_)` to route WebView messages through the dispatcher.
- For pulp #468's native-JS-runtime path, swap `attach_webview(...)` for `bridge_.attach_native_runtime(runtime, "<handler_name>")` once the runtime exposes its postMessage primitive.

See `docs/reference/editor-bridge.md` for the full API and the standard envelope-level error vocabulary (`malformed_json`, `unknown_type`, `missing_field`, `wrong_type`, `internal_error`).

## Classnames Artifact (Claude Design only — pulp #1035)

For `--from claude`, the CLI also emits `classnames.json` mapping
`classname → { cssProp(camelCase): cssValue, ... }` for every plain-classname `<style>` rule it finds in the export. Mirrors the output of Spectr's `tools/extract-html-bundle/extract.mjs` so downstream consumers (`@pulp/css-adapt`, `dom-adapter`) can merge class-based styles into inline before forwarding to bridge calls — no separate Node-side extraction script needed.

What the extractor honours:

- Bundler envelopes — when `<script type="__bundler/template">` is present, the extractor walks both the loader shell *and* the unwrapped template HTML's `<style>` blocks.
- Multiple `<style>` blocks cascade: later blocks override earlier ones per-property; unrelated declarations from earlier blocks are preserved.
- Comma-separated selector lists (`.btn-primary, .btn-secondary { ... }`) emit one entry per classname with identical declarations.
- Hyphenated CSS properties are camelCased (`font-family` → `fontFamily`).

What it skips:

- `:root`, `.scheme-*` rules — those are theme-mode token overrides handled upstream as `tokens.json` artifacts.
- Pseudo-classes (`.foo:hover`), attribute selectors (`.foo[data-x]`), descendant combinators (`.foo .bar`, `.foo > .bar`) — anything that isn't a plain `.classname { ... }` rule.
- `@media` / `@keyframes` / other at-rule wrappers.
- `<style>` blocks whose first 200 chars contain `@font-face` (those carry only font-face rules, no classnames).

This skill must stay aligned with the `view-bridge` skill — `view-bridge` covers editor lifecycle (create_view, open/notify_attached/resize/close), this skill covers message dispatch over that lifecycle.

## Versioned Detection (pulp #1031)

`pulp import-design` ships a three-layer version model so the CLI surface stays stable as external tools evolve their export formats:

- **`parser-version`** — Pulp's parser implementation for a given source.
- **`format-version`** — the export shape Pulp recognises.
- **`compat-schema-version`** — the schema of `compat.json` itself.

The matrix is declared in [`compat.json`](../../../compat.json) and consumed by `pulp import-design --detect-only`. See [`docs/reference/imports/index.md`](../../../docs/reference/imports/index.md) for the full vocabulary, recognized matrix, and "add a new format-version" workflow.

### Detect-only flow

When the user hands you an unknown export, run detection first before guessing the source:

```bash
# File or directory; --detect-only prints (source, format-version,
# parser-version, match-count, confidence) and exits.
pulp import-design --detect-only --file <path>
pulp import-design --detect-only --directory <path>
```

Exit codes: `0` = match, `1` = usage error, `2` = no match.

If confidence is below 80%, the CLI emits a warning and an invitation to run `--report-new-format`:

```bash
pulp import-design --file <path> --report-new-format > stitch-2026-XX.json
```

Hand-edit the resulting JSON into a new entry under `compat.json[imports/<source>/detected-formats]`. The `notes` field is mandatory — describe the upstream change in one line.

### Adding a fixture

Every new format-version needs a fixture so the detection gate covers it:

1. `mkdir -p test/fixtures/imports/<source>/<format-version>/`
2. Drop in the smallest representative export that triggers every fingerprint clause (synthetic is fine — clauses are content-addressed, not byte-addressed).
3. Add an `expected.json` sidecar with the assertion shape from existing fixtures (`source`, `format-version`, `parser-version`, `matched-clauses`, `total-clauses`, `min-confidence-pct`, `fingerprint-kinds`).
4. Run `ctest --test-dir build -R pulp-test-cli-import-detect` to confirm the fixture loop picks up the new row.

The detector module lives at `tools/import-design/import_detect.{hpp,cpp}` and is intentionally free of `pulp::view` / `pulp::state` link deps so the test target compiles fast and the unit tests don't drag the full design-import pipeline along.

## Canvas2D Bridge Gotchas (importer + shim authors MUST follow)

When translating browser `<canvas>` + Canvas2D code to Pulp's native bridge (`canvas*` globals), several spec-conforming browser idioms silently break against the bridge contract because the bridge surface is more limited and more direct than the HTML5 spec. The following rules were paid for in production debugging cycles on Spectr's analyzer port (pulp #1346/#1348/#1368/#1372 + Spectr `canvas2d-shim.ts`); the importer must emit code that respects them.

### 1. `ctx.arc()` does NOT add to a path on its own — synthesize as line segments

**Spec:** `ctx.arc()` adds an arc sub-path; subsequent `ctx.fill()` / `ctx.stroke()` operate on it.

**Bridge reality:** `canvasArc(id, x, y, r, sa, ea, fillColor)` strokes immediately and returns. It does not contribute to the active path, so `ctx.beginPath() → ctx.arc() → ctx.fill()` renders a stroked outline ring (just the arc's stroke), not a filled circle. Radial-gradient cap-emission patterns degenerate into hollow ellipse outlines.

**Importer rule:** when emitting `arc()` translation, emit a polyline approximation (~32 segments scaled by radius, 8..64) via `canvasLineTo`. This makes the sub-path closeable and `ctx.fill()` honors any active `fillStyle` / gradient.

```ts
arc(x, y, r, sa, ea, ccw) {
    const segs = Math.max(8, Math.min(64, Math.ceil(r * 1.2)));
    const sweep = ccw ? -((sa - ea + 2*Math.PI) % (2*Math.PI)) : ((ea - sa + 2*Math.PI) % (2*Math.PI));
    for (let i = 0; i <= segs; i++) {
        const a = sa + sweep * (i / segs);
        canvasLineTo(id, x + Math.cos(a) * r, y + Math.sin(a) * r);
    }
}
```

Same applies to `arcTo()` (rounded-rect corners), `ellipse()`, and `roundRect()`.

### 2. Gradient stops are individual `(color, pos)` args — NOT a JSON string

**Spec:** `grad.addColorStop(pos, color)` accumulates stops; the gradient is opaquely passed via `ctx.fillStyle = grad`.

**Bridge reality:** `canvasSetLinearGradient(id, x0, y0, x1, y1, color1, pos1, color2, pos2, ...)` and `canvasSetRadialGradient(id, cx, cy, radius, color1, pos1, ...)` read each pair via positional `args.get<>()`. Passing stops as a single JSON string makes `i+1 < args.numArgs` false on the first iteration → zero stops parsed → bridge dispatch skipped → `fillStyle = grad` falls through to `canvasSetFillColor(id, "[object Object]")` → parseColor returns default white → uniform white fill instead of the rainbow ramp.

**Importer rule:** when serializing gradient stops, spread as variadic args:

```ts
const stopArgs: (string|number)[] = [];
for (const s of grad.stops) stopArgs.push(s.color, s.offset);
canvasSetLinearGradient(id, x0, y0, x1, y1, ...stopArgs);
```

### 3. Radial gradient: bridge takes single circle (cx, cy, R), not two-circle (x0,y0,r0,x1,y1,r1)

**Spec:** `createRadialGradient(x0,y0,r0,x1,y1,r1)` — two circles, with the gradient interpolating in the cone between them.

**Bridge reality:** `canvasSetRadialGradient(id, cx, cy, radius, ...stops)` only takes the single outer circle. Inner-circle / off-axis ring patterns degrade.

**Importer rule:** map JS 6-numeric form to bridge's 3-numeric form using the OUTER circle (`x1, y1, r1`). For Spectr-style center-bloom (`r0=0`, same center) this is visually identical to the spec; for true two-point gradients, file a Pulp issue rather than emitting silently-wrong output.

### 4. `ctx.clearRect()` was a parent-surface eraser pre-#1372

**Reality (pre-pulp v0.74.1):** `ctx.clearRect()` used `SkBlendMode::kClear` / `CGContextClearRect` directly on the parent surface — NOT on a per-canvas backing layer. JS code that clears its own canvas would erase pixels another `<canvas>` sibling had just painted. Symptom: first sibling renders correctly, gets wiped by second sibling's clearRect at the start of its frame.

**Now (pulp v0.74.1+):** each `CanvasWidget::paint` is wrapped in `save_layer`, isolating clearRect / Porter-Duff to that canvas's own buffer. Importer can emit `<canvas>` siblings without worrying about cross-erasure. Pin SDK `>= 0.74.1`.

### 5. Other Canvas2D methods missing from the bridge

- `ctx.measureText()` — bridge has `canvasMeasureText` but the shim should fall back to a per-char approximation (~6.5px for 10px monospace, ~px*0.6 for proportional) when not available
- `ctx.strokeText()` — bridge has no stroke path; fall back to `canvasFillText` and accept the visual gap
- `ctx.createPattern()` — not implemented; emit a solid-color fallback from the pattern's first stop
- `ctx.createConicGradient()` — bridge has no `canvasSetConicGradient` registration even though `SkiaCanvas::set_fill_gradient_conic` exists; either file a Pulp follow-up or fall back to a flat solid

### 6. SDK version requirements for canvas2d parity

| Capability | Min SDK |
|------------|---------|
| `canvasSetLinearGradient` / `canvasSetRadialGradient` | v0.72.4 (pulp #1348) |
| Gradient stops actually applied to fills | v0.72.5 (pulp #1353) |
| `set_blend_mode` on Skia (GPU) honored | already wired; CG/CPU is silent no-op (pulp #1371) |
| Per-canvas `save_layer` isolation (no sibling clearRect erase) | v0.74.1 (pulp #1372) |
| Canvas paint instrumentation (`PULP_LOG_CANVAS_PAINT=1`) | v0.75.0 (pulp #1370) |

Reject importer output that targets earlier SDK versions for canvas-heavy designs — the visual gaps will be silent and look like Pulp bugs.

### 7. Validation discipline

Always pixel-sample after rendering — visual inspection misses uniform-fallback bugs. A spectrum that renders "uniform light gray" instead of "rainbow gradient" looks roughly right at thumbnail scale but is structurally broken (every color stop resolved to white by the parseColor fallback). Sample horizontal cross-sections at the expected gradient axis and assert color variance > some threshold.

### 8. Pointer events need explicit `registerPointer(id)` AND don't bubble

**Spec:** `addEventListener('pointerdown', fn)` plus React synthetic-event bubbling: a click on a child reaches the parent's handler unless `stopPropagation` is called.

**Bridge reality:** Pulp gates pointer dispatch behind an explicit `registerPointer(id)` call (parallel to `registerClick(id)` and `registerHover(id)`). `@pulp/react`'s prop-applier currently only wires `registerHover` for `mouseenter/leave`, so `onPointerDown/Move/Up` listeners are installed in the JS dispatch table but never fired by the native View — the JS handler appears registered (`on(id, 'pointerdown', fn)`) yet clicks never invoke it. Additionally, **pulp dispatches pointer events to the hit-test target only — there is no synthetic-event bubbling.** A handler on a parent `<div>` will not fire when the click lands on a child `<canvas>` that visually overlays it.

This was the root cause of Spectr's "FilterBank renders rainbow but band drag is dead" symptom (spectr #32 / commit `b7ba2b8`). Confirmed by `__spectrLog` probe at the top of `onPointerDown`: handler does NOT fire on `cliclick c:600,400` even though `on(pr_3, pointerdown, ...)` is registered.

**Importer rule:**

1. Whenever the importer emits an `onPointerDown / onPointerMove / onPointerUp / onPointerLeave / onWheel` handler, also emit a `registerPointer(id)` call against the same widget. Do this in the ref-mount callback (or its equivalent post-mount hook) so the bridge wires `on_pointer_event` into the View. Idempotent on the bridge side; safe to call on every remount.
2. **Do not assume bubbling.** If the design has a parent element with a pointer handler and child elements that visually cover it, mirror the same handler onto each direct child too. The handler can use the parent's `getBoundingClientRect()` for coord math so the same function works on every binding.

```ts
// Bind on parent + every interactive child:
<wrap onPointerDown={onPD} onPointerMove={onPM} onPointerUp={onPU}>
  <canvas onPointerDown={onPD} onPointerMove={onPM} onPointerUp={onPU} ... />
  <canvas onPointerDown={onPD} onPointerMove={onPM} onPointerUp={onPU} ... />
</wrap>
```

```ts
// In the ref-mount callback:
const id = inst.id;
if (typeof globalThis.registerPointer === 'function') globalThis.registerPointer(id);
```

The cleaner long-term fix is for `@pulp/react`'s prop-applier to call `registerPointer` automatically when it sees any pointer-event prop (parallel to its existing `registerHover` wiring) — track that as a follow-up Pulp issue rather than an importer-side workaround if you encounter it on a fresh import.

### 9. Shared widget promotion — `<div onClick>` → button

`pulp::view::promote_interactive_frames` walks the IR once during shared parse
normalization and re-types any `type == "frame"` carrying an interactive signal
to `type == "button"`. The CLI no longer owns a separate tool-local promotion
pass; `parse_design_ir_json()` and source adapters return the normalized form
for both library and CLI consumers. Adapters promote before assigning stable
anchors so content-hash anchors reflect the normalized type. Signal priority
(highest -> lowest):

1. `attributes["onclick"]` / `attributes["onClick"]` — strongest.
2. `attributes["role"] == "button"` — explicit ARIA semantic.
3. `style.cursor == "pointer"` — weakest; opt-out via `role="presentation"`.

Conservative on purpose: only frames are promoted; already-typed widgets
(`input`, `image`, `button`) are left alone, so a designer who wrote
`<input onClick={...}>` keeps the input.

**Gotcha**: the pass is source-agnostic, but it can only promote signals that
the adapter preserves in `IRNode::attributes` or `IRStyle::cursor`. Runtime
imports populate HTML attributes from the live DOM after React mount. Static
adapters that do not preserve `onclick` / `role` still cannot promote those
signals until their parser surfaces them.

When you re-import Spectr's `editor.html` via Claude Design + the runtime path, expect:

```
Promoted N interactive frame(s) to button widgets.
```

in the stdout summary. If you see `0 widgets` and no promotion line on a fixture you *know* contains `<div onClick>`, you're either (a) on a non-runtime parser path (#1823 territory) or (b) the React tree didn't mount during the harness eval and `attributes` is empty as a result.

## Live-host pump contract (pulp-internal #71)

Every live-host main loop that drives a `WidgetBridge` for a runtime-imported app **must call BOTH halves of the idle pump on every tick**:

```cpp
bridge->poll_async_results();      // async-exec results + queued frame callbacks
bridge->service_frame_callbacks(); // setTimeout / setInterval drain via __flushTimers__
```

`scripted_ui.cpp:67-81` documents the contract; `examples/design-tool/main.cpp` wires it through a GCD timer; `examples/ui-preview` does the equivalent. Skipping `service_frame_callbacks()` is a silent foot-gun — `setInterval(fn, N)` returns a valid id but `fn` never fires, so any imported app's polling-state-update path freezes. The chart/canvas (which paints from a separate ref-based store) keeps updating, but every React label that reads polled state stays at its initial value forever. Confirmed regression in design-tool when only the first half ran (Spectr's bands trigger frozen at "32" and zoom indicator frozen at "1.00×" — both drive their text from a 150ms `setInterval`).

Two safety nets enforce the contract going forward:

- **CI lint** at `tools/scripts/host_pump_lint.py` greps every host main.cpp listed in `HOST_FILES` for any `poll_async_results()` call not paired with `service_frame_callbacks()` within the same handler block. Fails CI on violation. Single-line bypass: append `// host-pump-lint: skip — <reason>` for genuine one-shot CLI tools.
- **Runtime smoke** at `tools/import-validation/live-host-pump-smoke.sh` launches each live-host binary against a tiny script that schedules `setInterval(fire, 50)`, runs for 2s, and asserts ≥5 fires landed. Generic — adding a new live host = one row in the `HOSTS=()` table; everything else is reused.

When adding a new live host (e.g. a future `examples/<thing>` binary), update **both**: append the new host to `HOST_FILES` in `host_pump_lint.py` AND to `HOSTS` in `live-host-pump-smoke.sh`. The lint catches source-level regressions at PR time; the smoke catches runtime breakage where the source pairing is correct but the run loop is misconfigured.

For unobtrusive smoke / CI runs, the design-tool exposes `--no-show-window` (uses `WindowOptions.initially_hidden` to skip Dock icon + window display while keeping the full bridge run loop active) and `--exit-after-ms <N>` (clean `request_close()` after N ms). Both flags compose, so `pulp-design-tool --script <probe.js> --no-show-window --exit-after-ms 2000` runs the full live-host code path with no GUI flash.

## Keyboard shortcut V2 wire-up

The import path detects `keydown`/`keyup` global-shortcut handlers in React-style source and emits two pieces of generated code in the host JS bundle:

1. **`registerShortcut(...)` calls** that bind each detected keycode + modifier combo to a synthetic-keydown re-dispatch handler. The native side intercepts the bare key (no DOM focus) and routes through this registration.
2. **Synthetic keydown re-dispatch** that builds a `KeyboardEvent`-shaped object with the right `key`, `code`, `keyCode`, and modifier flags (`ctrlKey`, `metaKey`, `shiftKey`, `altKey`), then dispatches it to the document so the original React handler's `if (e.ctrlKey && e.key === 's')` branch fires.

Two correctness gotchas the codegen MUST respect — both surfaced by real handler shapes:

- **`metaKey` and `ctrlKey` are separate axes — don't collapse**. The collector emits BOTH `"meta"` and `"ctrl"` when the source has the cross-platform `e.metaKey || e.ctrlKey` idiom, and emits separate `registerShortcut(...)` bindings per platform half. The synthetic event sets `ctrlKey`/`metaKey` according to which mask bits are present — a Ctrl-only source handler (`e.ctrlKey && e.key === 's'` on Win/Linux) gets a `ctrlKey: true, metaKey: false` synthetic event, not the flat "always Cmd" form.
- **`KeyboardEvent.code` letter/digit forms decode before keycode emission**. The extractor captures both `event.key` and `event.code`. `KeyS`, `KeyA`, …, `Digit1`, …, `Digit9` must be stripped to the letter/digit form before the keycode lookup, otherwise the table-miss returns `0` and `generate_pulp_js` drops the entire `registerShortcut(...)` line silently.

If you add a new shortcut shape detector to the extractor, mirror it in:
- The keycode table in `core/view/src/design_import.cpp::keycode_for(...)` (or its equivalent today)
- The modifier collector that walks the surrounding boolean expression
- The synthetic-event emitter that produces the `KeyboardEvent`-shaped JS object

Test coverage lives in `test/test_design_import.cpp` (E2E roundtrip — codegen → WidgetBridge → React-style handler). The roundtrip exercises both the registerShortcut emission and the synthetic-keydown re-dispatch; failing either half is a hard test failure.

## Default shortcuts (Phase A — source-matched, #2128)

On top of the V2 extractor, the import pass auto-binds platform-convention chords (`Cmd+,` Settings, `Cmd+?` Help, bare `?` cheatsheet, `Cmd+N/O/S/F`, plus Win/Linux `Ctrl`/`F1` variants) when the dev's React source has a recognizable component. Lives in `core/view/src/design_import.cpp::detect_default_shortcuts(...)` + `apply_default_shortcuts(...)`. Accepted defaults are lowered into `DetectedShortcut` form and ride the V2 codegen path with no fork.

Hard rule (encoded): **a wrong auto-binding is worse than no binding**. Detector requires ≥2 signals to fire and emits a `collision` entry (no bind) when multiple candidates compete.

Signals scored per (component, pattern):
1. Component name keyword match (`SettingsModal`, `HelpPanel`, etc.)
2. `role="dialog"` / `role="alertdialog"` / `role="menu"` / `role="listbox"` in body
3. `aria-label="..."` text in body containing pattern keyword
4. `<h1>`/`<h2>`/`<h3>` heading text matching pattern keyword
5. `<kbd>` tag presence (cheatsheet disambiguator — required for cheatsheet match)
6. **Canonical-name bonus**: exact `<Pattern>{Modal,Dialog,Panel,Popover,Sheet,Window,Drawer}` shape counts as a second signal on its own. Real apps (Spectr's `SettingsModal`, `HelpPopover`) use inline-styled divs without ARIA, so the strict ≥2 ARIA-shape gate would skip every one of them. Generic non-canonical names (`SettingsList`) still require a real second signal.

Body-window extraction stops at the next top-level component declaration — otherwise sibling components bleed into each other (a cheatsheet `<kbd>` in `ShortcutsModal` would wrongly count as a signal for an adjacent `SettingsModal` defined right above it).

Cross-platform emission: the CLI runs at import time, but the generated `ui.js` ships to multiple platforms. The import driver emits BOTH macOS and Win/Linux variants for any default with a platform delta (Help: `Cmd+?` + `F1`; Settings: `Cmd+,` + `Ctrl+,`). At runtime only the chord matching the physical key fires its `registerShortcut` entry (exact-mask match on the bridge side).

JS-literal escape: any key string interpolated into `key: '...'` MUST be backslash- and quote-escaped — `key_string_to_keycode` accepts all printable ASCII (incl. `'` and `\`), so a source with `e.key === "'"` would otherwise produce syntactically-invalid JS. The `emit_binding` lambda escapes at emission time.

CLI surface:
- `--no-default-shortcuts` opts out (default ON)
- `<name>.defaults.json` diagnostic written alongside `shortcuts.json` showing accepted candidates + collisions

What's NOT in Phase A: Pulp-framework defaults for the built-in `SettingsPanel` Audio/MIDI sub-tabs (`Cmd+Opt+A` / `Cmd+Opt+M`). Phase B follow-up — needs `TabPanel` select-tab JS API + standalone-only emission gate. Spec: `planning/2026-05-16-default-keyboard-shortcuts.md`.
