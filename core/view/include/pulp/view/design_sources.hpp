#pragma once

/// @file design_sources.hpp
/// Source adapters that lower external design exports into the normalized
/// DesignIR: Figma / Stitch / v0 / Pencil / Claude HTML, Google DESIGN.md
/// (lint / diff / Tailwind export), and the Claude Design / React runtime
/// bundle envelope (unpack + headless runtime materialization).
///
/// The result and option value types these adapters exchange live in
/// design_source_types.hpp (`pulp-design-ir`), so generic view code can carry
/// them without reaching the importer.

#include <pulp/view/design_ir.hpp>
#include <pulp/view/design_ir_io.hpp>
#include <pulp/view/design_source_types.hpp>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <unordered_map>

namespace pulp::view {

// ── Design source helpers ───────────────────────────────────────────────
//
// The DesignSource enum itself lives in design_ir.hpp (DesignIR::source uses
// it by value). These string<->enum helpers live with the source adapters.

/// Convert string to DesignSource, returns nullopt for unknown sources.
std::optional<DesignSource> parse_design_source(const std::string& name);

/// Convert DesignSource to display name.
const char* design_source_name(DesignSource source);

/// Detect dynamic APIs that make a JSX baked snapshot non-deterministic unless
/// the caller explicitly accepts or warns on snapshot semantics.
SnapshotDynamicApiScan detect_jsx_snapshot_dynamic_apis(std::string_view source);

// ── Source adapters ─────────────────────────────────────────────────────

/// Parse a Figma export JSON into a DesignIR.
DesignIR parse_figma_json(const std::string& json);

/// Parse Pulp's "Design for Pulp" Figma-plugin export envelope.
DesignIR parse_figma_plugin_json(const std::string& json);

/// Parse Stitch HTML export into a DesignIR.
DesignIR parse_stitch_html(const std::string& html);

/// Parse v0-generated TSX/Tailwind into a DesignIR.
DesignIR parse_v0_tsx(const std::string& tsx);

/// Parse Pencil node tree JSON into a DesignIR.
DesignIR parse_pencil_json(const std::string& json);

/// Parse a Claude Design standalone HTML export into a DesignIR.
/// Claude Design exports the same HTML+CSS shape as other web tools,
/// so the parse path delegates to the Stitch HTML parser. Source-tagged
/// as DesignSource::claude so downstream code (CLI bridge scaffold,
/// SKILL guidance) can route on it.
DesignIR parse_claude_html(const std::string& html);

// ── DESIGN.md adapter (Google design.md, Apache-2.0) ─────────────────────
//
// DESIGN.md is a YAML-frontmatter + Markdown-body description of a design
// system. Unlike the other import sources, it does NOT describe a screen
// or a renderable artifact — it carries tokens and prose rationale. The
// parser populates IR token maps; the IR root node is left empty (no
// children, type "frame"). It deliberately does NOT scaffold a
// placeholder UI tree; that would imply scaffold quality Pulp cannot yet
// deliver. Spec: https://github.com/google-labs-code/design.md
//
// Diagnostics use a structured shape so the CLI can emit them as JSON
// with line/column info supplied by yaml-cpp.

/// Parse a DESIGN.md file (YAML frontmatter + Markdown body) into a DesignIR.
/// Errors are reported via diagnostics, not exceptions; the IR is partially
/// populated on parse error so callers can still surface what was readable.
DesignMdParseResult parse_designmd(const std::string& markdown);

/// Thin wrapper that returns just the IR. The CLI uses parse_designmd()
/// directly so it can emit structured diagnostics; this overload exists
/// for parity with the other source adapters' signatures.
DesignIR parse_designmd_yaml(const std::string& markdown);

// ── Lint, diff, and Tailwind export ─────────────────────────────────────
//
// These surfaces let DESIGN.md slot into CI gates the same way
// @google/design.md's TypeScript CLI does, without taking on its JS
// dependency surface. The lint rule set tracks the format rules Pulp consumes,
// including DESIGN.md 0.4 omitted-section and token-schema validation.

/// Run Pulp's Google design.md compatibility lint against a parsed DESIGN.md.
/// Findings carry severity (info/warning/error).
std::vector<DesignMdDiagnostic> lint_designmd(const DesignMdParseResult& parsed);

/// Compute the token diff between two parsed DESIGN.md files plus a
/// regression flag derived from their respective lint outputs.
DesignMdDiffResult diff_designmd(const DesignMdParseResult& before,
                                  const DesignMdParseResult& after);

/// Emit a Tailwind v3 `theme.extend`-shaped JSON object from the parsed
/// token tree. Byte-compatible with @google/design.md's
/// `--format json-tailwind` output for the common-case keys (colors,
/// fontFamily, fontSize, lineHeight, letterSpacing, fontWeight,
/// borderRadius, spacing).
std::string export_tailwind_v3_json(const DesignMdParseResult& parsed);

/// Emit a Tailwind v4 `@theme { ... }` CSS block from the parsed
/// token tree. Uses Tailwind v4's CSS-variable token namespaces
/// (`--color-*`, `--font-*`, `--text-*`, `--leading-*`,
/// `--tracking-*`, `--font-weight-*`, `--radius-*`, `--spacing-*`).
std::string export_tailwind_v4_css(const DesignMdParseResult& parsed);

// ── Claude Design bundle envelope (pulp #468) ───────────────────────────
//
// Claude Design's "standalone HTML" export is a small loader shell
// wrapping a JSON envelope of base64-gzip-compressed assets. The loader
// JS unpacks the envelope at runtime and replaces `document.documentElement.innerHTML`
// with the unwrapped template HTML, then injects the bundled scripts.
//
// For the native-runtime import lane (`--execute-bundle`), Pulp needs to
// reproduce that unpack step in C++ before handing the result to the JS
// engine. This function performs only the unpack — running the result is
// `parse_claude_html_with_runtime`'s job.

/// Decode the bundler envelope from a Claude Design standalone HTML file.
/// Returns nullopt if the bundler tags are missing or the envelope is malformed.
/// Bundle parsing failures degrade gracefully: callers fall back to
/// `parse_claude_html` for the static-HTML pipeline.
std::optional<ClaudeBundle> parse_claude_bundle(const std::string& html);

/// Decode the deterministic pre-mount document emitted by Chromium browser
/// capture. The sidecar records the exact HTML passed to DOMParser after an
/// adapter has patched the source, plus every Blob-backed asset it references.
/// Returned bundles use stable synthetic asset IDs and can be evaluated by the
/// same native runtime path as a regular Claude bundle.
std::optional<ClaudeBundle> parse_materialized_browser_document(
    const std::string& json);

/// `parse_materialized_browser_document` with process-wide reuse.
///
/// Decoding a captured document — JSON parse, base64 decode, and a SHA-256
/// check of every asset — costs on the order of 100 ms for a plug-in UI, and
/// an editor runs it on every open. The first call for a document does the
/// full decode and verification; every later call in the same process with
/// byte-identical input returns the same immutable bundle. Keyed by the full
/// input bytes (size, then an exact compare), so a hit is never a different
/// document, and only accepted documents are kept: a rejected input is
/// decoded again on every call and still returns nullptr. In-memory only and
/// bounded; `PULP_RUNTIME_IMPORT_CACHE=0` disables reuse.
std::shared_ptr<const ClaudeBundle> parse_materialized_browser_document_shared(
    const std::string& json);

MaterializedDocumentCacheStats materialized_document_cache_stats();
/// Drop every kept document (tests; a host that wants the memory back).
void clear_materialized_document_cache();

/// The whole scripts a live editor evaluates when it imports `bundle` at run
/// time (`__pulpRuntimeImport__`): each JavaScript asset payload and each
/// `text/javascript` inline script, byte-identical to what the payload
/// pipeline hands `evaluate_script()`. A prewarm compiles exactly these.
std::vector<std::string> runtime_import_whole_scripts(const ClaudeBundle& bundle);

/// Normalize a constrained v0.dev React TSX export into the runtime-import
/// bundle payload shape. Accepts either a bare single-file TSX component or
/// a v0 code-project envelope with `[V0_FILE]tsx:file="..."` blocks.
/// Returns nullopt when the artifact uses surfaces outside Pulp's supported
/// runtime-import DOM/CSS/API subset (Tailwind/shadcn/Next wrappers, network
/// APIs, storage APIs, unsupported JSX tags, etc.).
std::optional<ClaudeBundle> parse_v0_dev_react(const std::string& tsx_or_envelope);

/// Normalize a constrained Figma Make React TSX export into the runtime-import
/// bundle payload shape. C-2 accepts a sanitized single-file TSX component:
/// Figma Make provenance must be explicit, versions/assets must already be
/// stripped, and styling must use inline `style={{...}}` objects rather than
/// Tailwind/Radix/default Figma Make dependencies.
std::optional<ClaudeBundle> parse_figma_make_react(const std::string& tsx);

/// Normalize a constrained Google Stitch React export into the runtime-import
/// bundle payload shape. Stitch has no reliable standalone file marker, so
/// callers should route here only from an explicit `source: "stitch"` manifest
/// or runtime-import source label. Returns nullopt for Tailwind/default Stitch
/// exports, external CSS, Next/Radix wrappers, RN imports, MCP JSON, or any
/// surface outside Pulp's supported runtime-import DOM/CSS/API subset.
std::optional<ClaudeBundle> parse_stitch_react(const std::string& tsx);

/// Normalize a constrained React Native single-file component export into the
/// runtime-import bundle payload shape. RN has an unambiguous envelope
/// (`from "react-native"`), so the runtime dispatch may auto-detect it when
/// the source label is omitted. Returns nullopt for native/device APIs,
/// navigation/Expo wrappers, images, style arrays, DOM tags, or any surface
/// outside Pulp's supported runtime-import DOM/CSS/API subset.
std::optional<ClaudeBundle> parse_react_native_export(const std::string& tsx);

/// Normalize a constrained Pencil/OpenPencil sanitized Tailwind-JSX export
/// into the runtime-import bundle payload shape. The initial runtime-import
/// slice requires the caller to pass source="pencil" and expects Tailwind
/// utilities plus `--pencil-*` tokens to have already been resolved into
/// inline React styles. Returns nullopt for unresolved tokens, MCP JSON, binary
/// `.pen`/`.fig` references, external CSS, framework wrappers, or any surface
/// outside Pulp's supported runtime-import DOM/CSS/API subset.
std::optional<ClaudeBundle> parse_pencil_react(const std::string& tsx);

/// Wrap a pre-compiled JSX runtime bundle (esbuild IIFE of React plus either
/// ReactDOM or the @pulp/react native bridge and the user's JSX component,
/// produced by `tools/import-design/jsx-runtime/
/// jsx-transform.mjs`) as a synthetic `ClaudeBundle` so the existing
/// runtime-import harness (`parse_claude_html_with_runtime`) can materialize
/// it. The C++ side does not compile JSX itself — the Node-side esbuild
/// step is shelled out by the CLI before this is called.
///
/// `component_name` is purely cosmetic — it lands on the synthetic
/// `<div id="root" data-jsx-component="…">` template, useful for diagnostic
/// inspection.
///
/// Returns nullopt for sources that are obviously too small to be a real
/// bundle (< 100 bytes), so the CLI can surface a clean diagnostic when the
/// upstream Node transform produced an empty file.
std::optional<ClaudeBundle> parse_jsx_react(const std::string& bundle_js,
                                            const std::string& component_name = "JsxComponent");

/// Synthesize a Claude-style HTML envelope around an arbitrary ClaudeBundle
/// so the existing `parse_claude_html_with_runtime` harness can consume it
/// without a real Claude Design HTML wrapper on input. Used by `--from jsx`
/// (and future direct-bundle import sources) so they can ride the existing
/// runtime materialization path without each one re-implementing the
/// ScriptEngine + WidgetBridge + DOM-walker boot sequence.
///
/// The envelope uses `compressed:false` raw base64 for the manifest entries
/// — gzip+deflate is unnecessary overhead for in-process synthesis (the
/// existing test harness already exercises this code path). The template
/// HTML is JSON-escaped per the bundler/template script-tag contract.
std::string synthesize_runtime_envelope(const ClaudeBundle& bundle);

/// Run the Claude Design bundle in a headless JS engine and harvest the
/// materialized DOM into a `DesignIR`. The success bar is "more than 9
/// elements" (the loader-shell baseline that the static parser
/// produces). On any failure — missing bundle envelope, JS engine error,
/// empty walker output — returns the result of `parse_claude_html(html)`
/// so the caller's behavior never drops below the static-parse floor.
DesignIR parse_claude_html_with_runtime(const std::string& html,
                                        ClaudeRuntimeOptions opts = {});

/// Build the starter `bridge_handlers.cpp` text the CLI emits next to
/// the generated JS view when `--from claude` runs. Pure string-builder;
/// no I/O. The output references `pulp::view::EditorBridge` and shows
/// add_handler() registrations + an attach_webview() call so users have
/// a runnable shape to fill in (pulp #709 acceptance criterion).
std::string render_claude_bridge_scaffold(const std::string& generated_js_path);

// ── Claude Design classname extraction (pulp #1035) ─────────────────────
//
// Spectr's `tools/extract-html-bundle/extract.mjs` walks every `<style>`
// block in a Claude Design export and emits a `classnames.json` mapping
// of `classname → { cssProp: cssValue, ... }` (camelCase keys, raw
// values). The downstream `@pulp/css-adapt` layer consumes that map to
// merge class-based styles into inline before forwarding to bridge
// calls. Pulp's `pulp import-design --from claude` bakes that pass into
// the import pipeline so plugin authors get the same artifact without
// having to run a separate Node-side script.
//
// Result type (ClaudeClassNameRules, design_source_types.hpp): outer map keyed by classname (no leading dot), inner map
// keyed by CSS property in camelCase (e.g. `font-family` → `fontFamily`).
// Multiple `<style>` blocks in document order are merged; later blocks
// override earlier ones for the same classname (mirrors browser
// cascading order). `.scheme-*` rules are skipped — those are already
// handled as theme-mode token overrides upstream.

/// Extract classname → declaration map from a Claude Design HTML export.
/// Walks every `<style>...</style>` block in document order. When a
/// bundler `<script type="__bundler/template">` envelope is present, the
/// inner unwrapped HTML is also walked (that's where the real CSS lives
/// for self-bundled exports). Returns an empty map when no recognizable
/// CSS rules are found — never throws.
ClaudeClassNameRules extract_claude_classnames(const std::string& html);

/// Serialize a `ClaudeClassNameRules` map to the JSON shape Spectr's
/// `extract.mjs` produces. Keys are emitted in alphabetical order so the
/// output is stable across runs (deterministic CI fixture comparison).
/// CSS values are emitted verbatim — callers downstream of this artifact
/// (e.g. `@pulp/css-adapt`) handle lowering to bridge calls.
std::string serialize_claude_classnames(const ClaudeClassNameRules& rules);

} // namespace pulp::view
