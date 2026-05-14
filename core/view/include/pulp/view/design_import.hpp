#pragma once

/// @file design_import.hpp
/// Import design files from external tools (Figma, Stitch, v0, Pencil) into
/// Pulp's web-compat JS format. Supports a normalized intermediate
/// representation (IR) and W3C Design Tokens.

#include <pulp/view/theme.hpp>
#include <string>
#include <vector>
#include <optional>
#include <map>
#include <unordered_map>
#include <variant>

namespace pulp::view {

// ── Design source types ─────────────────────────────────────────────────

enum class DesignSource {
    figma,
    stitch,
    v0,
    pencil,
    claude   // Anthropic Claude Design — manual HTML/zip export, no Anthropic API
};

/// Convert string to DesignSource, returns nullopt for unknown sources.
std::optional<DesignSource> parse_design_source(const std::string& name);

/// Convert DesignSource to display name.
const char* design_source_name(DesignSource source);

// ── Normalized Intermediate Representation ──────────────────────────────

/// Layout direction for flex containers.
enum class LayoutDirection { row, column };

/// Alignment values for flex containers.
enum class LayoutAlign {
    flex_start, flex_end, center, stretch, space_between, space_around
};

/// Sizing mode for an IR node dimension.
enum class SizingMode { fixed, hug, fill };

/// Style properties for an IR node.
struct IRStyle {
    std::optional<std::string> background_color;
    std::optional<std::string> background_gradient;   // linear-gradient(...)
    std::optional<std::string> color;                  // text color
    std::optional<float> opacity;
    std::optional<float> border_radius;
    std::optional<std::string> border;                 // e.g. "1px solid #333"
    std::optional<std::string> box_shadow;
    std::optional<std::string> filter;                 // e.g. "blur(4px)"
    std::optional<std::string> font_family;
    std::optional<float> font_size;
    std::optional<int> font_weight;
    std::optional<std::string> font_style;             // normal, italic
    std::optional<std::string> text_align;
    std::optional<float> letter_spacing;
    std::optional<float> line_height;
    std::optional<std::string> text_transform;
    std::optional<std::string> overflow;               // hidden, scroll, auto
    std::optional<std::string> cursor;
    std::optional<std::string> position;               // absolute, relative
    std::optional<float> top, left, right, bottom;
    std::optional<int> z_index;
    std::optional<std::string> transform;              // rotate, scale, etc.
    std::optional<float> width, height;
    std::optional<float> min_width, min_height;
    std::optional<float> max_width, max_height;
};

/// Layout properties for an IR container node.
struct IRLayout {
    LayoutDirection direction = LayoutDirection::column;
    float gap = 0.0f;
    float padding_top = 0.0f;
    float padding_right = 0.0f;
    float padding_bottom = 0.0f;
    float padding_left = 0.0f;
    LayoutAlign justify = LayoutAlign::flex_start;
    LayoutAlign align = LayoutAlign::stretch;
    bool wrap = false;
    SizingMode width_mode = SizingMode::fixed;
    SizingMode height_mode = SizingMode::fixed;
};

/// Audio widget type detected from naming conventions or annotations.
enum class AudioWidgetType {
    none,       // Not an audio widget
    knob,
    fader,
    meter,
    xy_pad,
    waveform,
    spectrum
};

/// A single node in the normalized design IR.
struct IRNode {
    std::string type;   // "frame", "text", "image", "button", "input", "slider"
    std::string name;
    std::string text_content;            // For text nodes
    IRStyle style;
    IRLayout layout;
    AudioWidgetType audio_widget = AudioWidgetType::none;
    std::string audio_label;             // Label for audio widgets (e.g. "Gain")
    float audio_min = 0.0f;
    float audio_max = 1.0f;
    float audio_default = 0.5f;
    std::vector<IRNode> children;
    std::unordered_map<std::string, std::string> attributes;  // Extra metadata
};

/// Design token collection (W3C-compatible).
struct IRTokens {
    std::unordered_map<std::string, std::string> colors;       // name → "#hex"
    std::unordered_map<std::string, float> dimensions;         // name → px value
    std::unordered_map<std::string, std::string> strings;      // name → string
};

/// Complete design IR document.
struct DesignIR {
    IRNode root;
    IRTokens tokens;
    DesignSource source = DesignSource::figma;
    std::string source_file;
};

// ── Source adapters ─────────────────────────────────────────────────────

/// Parse a Figma export JSON into a DesignIR.
DesignIR parse_figma_json(const std::string& json);

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

/// One asset entry from a Claude Design bundle.
struct ClaudeBundleAsset {
    std::string uuid;          ///< envelope key (also referenced as <script src="<uuid>">)
    std::string mime;          ///< e.g. "text/javascript", "font/woff2"
    std::vector<uint8_t> data; ///< already base64-decoded + gunzipped if compressed
};

/// Result of unpacking a Claude Design bundle's `<script type="__bundler/manifest">`
/// + `<script type="__bundler/template">` pair.
struct ClaudeBundle {
    std::vector<ClaudeBundleAsset> assets;  ///< all assets (JS, fonts, etc.) in manifest order
    std::vector<size_t> javascript_indices; ///< indices into assets[] of MIME `text/javascript`,
                                            ///< in the order the template's <script src> tags reference them
    std::string template_html;              ///< the unwrapped HTML template (with `<div id="root">` etc.)
};

/// Decode the bundler envelope from a Claude Design standalone HTML file.
/// Returns nullopt if the bundler tags are missing or the envelope is malformed.
/// Bundle parsing failures degrade gracefully: callers fall back to
/// `parse_claude_html` for the static-HTML pipeline.
std::optional<ClaudeBundle> parse_claude_bundle(const std::string& html);

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

/// Options for the `--execute-bundle` import lane.
struct ClaudeRuntimeOptions {
    /// Hard cap on bytes of bundled JS to evaluate. Bundles larger than
    /// this short-circuit back to the static parser so we don't crash
    /// the QuickJS parser stack on a 10 MB minified blob. Default ~6 MB
    /// covers a typical react.development + react-dom.development +
    /// app.bundle combo (~4.3 MB) with headroom.
    size_t max_total_js_bytes = 6 * 1024 * 1024;

    /// Override the JS engine backend. nullopt -> platform default
    /// (auto: JSC on Apple, QuickJS elsewhere). Useful for tests that
    /// want deterministic engine choice.
    std::optional<int> engine_override;  // pulp::view::JsEngineType opaque to header

    /// If non-null, populated with a human-readable explanation when the
    /// runtime path bails out and the caller should fall back to the
    /// static parser. Empty when `parse_claude_html_with_runtime` returns
    /// a non-empty IR.
    std::string* error_out = nullptr;
};

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
// Result type: outer map keyed by classname (no leading dot), inner map
// keyed by CSS property in camelCase (e.g. `font-family` → `fontFamily`).
// Multiple `<style>` blocks in document order are merged; later blocks
// override earlier ones for the same classname (mirrors browser
// cascading order). `.scheme-*` rules are skipped — those are already
// handled as theme-mode token overrides upstream.
using ClaudeClassNameRules =
    std::map<std::string, std::map<std::string, std::string>>;

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

/// Detect audio widget type from a node name.
AudioWidgetType detect_audio_widget(const std::string& name);

// ── Code generator ──────────────────────────────────────────────────────

/// Code generation output mode.
enum class CodeGenMode {
    web_compat,   // document.createElement + el.style (web-compat layer)
    native        // createCol/createRow/createKnob + setFlex (native Pulp API)
};

/// Options for code generation.
struct CodeGenOptions {
    CodeGenMode mode = CodeGenMode::native;  // Native by default (better Yoga compat)
    bool include_tokens = true;       // Generate token assignments
    bool include_comments = true;     // Generate inline comments
    bool preview_mode = false;        // Use minimal widget style (design preview)
    std::string root_variable = "root";
    int indent_spaces = 2;
};

/// Generate Pulp JS code from a DesignIR.
/// Native mode (default) uses createCol/createRow/createKnob + setFlex.
/// Web-compat mode uses document.createElement + el.style.
std::string generate_pulp_js(const DesignIR& ir, const CodeGenOptions& opts = {});

// ── W3C Design Tokens ───────────────────────────────────────────────────

/// Parse W3C Design Tokens JSON into a Pulp Theme.
///
/// W3C format:
/// @code
/// {
///   "color": {
///     "primary": { "$value": "#89B4FA", "$type": "color" },
///     "bg": { "$value": "#1E1E2E", "$type": "color" }
///   },
///   "dimension": {
///     "spacing-md": { "$value": "8px", "$type": "dimension" }
///   }
/// }
/// @endcode
Theme parse_w3c_tokens(const std::string& json);

/// Export a Pulp Theme to W3C Design Tokens JSON format.
std::string export_w3c_tokens(const Theme& theme);

/// Convert IR tokens to a Pulp Theme.
Theme ir_tokens_to_theme(const IRTokens& tokens);

/// Convert a Pulp Theme to W3C-compatible IR tokens.
IRTokens theme_to_ir_tokens(const Theme& theme);

// ── External tool token sync ────────────────────────────────────────────

/// Parse Figma Variables JSON (from MCP get_variable_defs) into a Pulp Theme.
/// Figma variables are organized into collections with modes.
/// Each variable has resolvedValue for the default mode.
Theme parse_figma_variables(const std::string& json);

/// Export a Pulp Theme as Figma Variables-compatible JSON.
/// Produces the structure expected by Figma's variable creation APIs.
std::string export_figma_variables(const Theme& theme);

/// Parse a Stitch Design System JSON (from MCP list_design_systems/get_screen)
/// into a Pulp Theme. Maps Stitch colors, fonts, and roundness to tokens.
Theme parse_stitch_design_system(const std::string& json);

/// Export a Pulp Theme as Stitch Design System-compatible JSON.
/// Produces the structure expected by Stitch's create/update_design_system APIs.
std::string export_stitch_design_system(const Theme& theme);

} // namespace pulp::view
