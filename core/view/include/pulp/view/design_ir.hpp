#pragma once

/// @file design_ir.hpp
/// Normalized intermediate representation (IR) for imported designs. Defines
/// the IR model (style, layout, nodes, tokens, asset manifest, diagnostics)
/// plus the canonical JSON serialize/parse and asset-collection entry points
/// shared by every design-import source adapter.

#include <pulp/view/theme.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <map>
#include <unordered_map>
#include <variant>

namespace pulp::view {

// ── Design source types ─────────────────────────────────────────────────
//
// DesignSource is part of the IR model: DesignIR::source is a DesignSource
// by value, so the enum must be defined here (foundational) to keep the
// header dependency graph acyclic. The string<->enum helper functions live
// in design_sources.hpp alongside the source adapters.

enum class DesignSource {
    figma,
    stitch,
    v0,
    pencil,
    claude,   // Anthropic Claude Design — manual HTML/zip export, no Anthropic API
    designmd, // Google DESIGN.md (Apache-2.0) — YAML frontmatter + Markdown body
    jsx,           // Single-file React JSX instrument — compiled via esbuild + bundled React/ReactDOM
    figma_plugin   // Pulp's "Design for Pulp" Figma plugin export envelope
};

// ── Normalized Intermediate Representation ──────────────────────────────

/// Layout direction for flex containers.
enum class LayoutDirection { row, column };

/// Alignment values for flex containers.
enum class LayoutAlign {
    flex_start, flex_end, center, stretch, space_between, space_around
};

/// Sizing mode for an IR node dimension.
enum class SizingMode { fixed, hug, fill };

/// A single CSS `box-shadow` layer. CSS `box-shadow` is a comma-separated
/// list of these layers (painted first-to-last, so the first sits on top);
/// `IRStyle::box_shadow` keeps them in author order. Before pulp #41 the IR
/// stored the whole declaration as one opaque string, which silently dropped
/// every layer past the first — multi-shadow designs lost their stack.
struct IRBoxShadow {
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float blur = 0.0f;
    float spread = 0.0f;
    std::string color;            ///< raw CSS color token ("#rrggbbaa" or "rgba(...)")
    bool inset = false;
    std::string raw;              ///< original layer text, for lossless round-trip
};

/// Style properties for an IR node.
struct IRStyle {
    std::optional<std::string> background_color;
    std::optional<std::string> background_gradient;   // linear-gradient(...)
    std::optional<std::string> background_image;      // url(...), data:..., or none
    std::optional<std::string> background_repeat;
    std::optional<std::string> color;                  // text color
    std::optional<float> opacity;
    std::optional<float> border_radius;
    std::optional<std::string> border;                 // e.g. "1px solid #333"
    std::optional<std::string> border_color;
    std::optional<float> border_width;
    std::optional<std::string> border_style;
    std::optional<std::string> border_top_color;
    std::optional<std::string> border_right_color;
    std::optional<std::string> border_bottom_color;
    std::optional<std::string> border_left_color;
    std::optional<float> border_top_width;
    std::optional<float> border_right_width;
    std::optional<float> border_bottom_width;
    std::optional<float> border_left_width;
    std::optional<float> border_top_left_radius;
    std::optional<float> border_top_right_radius;
    std::optional<float> border_bottom_right_radius;
    std::optional<float> border_bottom_left_radius;
    std::vector<IRBoxShadow> box_shadow;               // ordered CSS shadow layers
    std::optional<std::string> filter;                 // e.g. "blur(4px)"
    std::optional<std::string> backdrop_filter;
    std::optional<std::string> font_family;
    std::optional<float> font_size;
    std::optional<int> font_weight;
    std::optional<std::string> font_style;             // normal, italic
    std::optional<std::string> text_align;
    std::optional<float> letter_spacing;
    std::optional<float> line_height;
    std::optional<std::string> text_transform;
    std::optional<std::string> text_decoration;
    std::optional<std::string> white_space;
    std::optional<std::string> text_overflow;
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
    std::optional<std::string> display;  // flex, grid, none
    LayoutDirection direction = LayoutDirection::column;
    float gap = 0.0f;
    std::optional<float> row_gap;
    std::optional<float> column_gap;
    float padding_top = 0.0f;
    float padding_right = 0.0f;
    float padding_bottom = 0.0f;
    float padding_left = 0.0f;
    std::optional<float> margin_top;
    std::optional<float> margin_right;
    std::optional<float> margin_bottom;
    std::optional<float> margin_left;
    LayoutAlign justify = LayoutAlign::flex_start;
    LayoutAlign align = LayoutAlign::stretch;
    std::optional<std::string> align_self;
    std::optional<std::string> align_content;
    bool wrap = false;
    std::optional<float> flex_grow;
    std::optional<float> flex_shrink;
    std::optional<std::string> flex_basis;
    std::optional<int> order;
    std::optional<float> aspect_ratio;
    std::optional<std::string> overflow_x;
    std::optional<std::string> overflow_y;
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

struct IRNode;

/// Heuristic signals that a parsed frame is interactive enough to promote
/// from a static frame to a button widget.
enum class WidgetPromotionSignal {
    none,
    onclick_attribute,
    aria_role_button,
    cursor_pointer
};

/// Inspect one IR node for interactive-frame promotion signals.
WidgetPromotionSignal classify_interactive_signal(const IRNode& node);

/// Promote parsed `frame` nodes carrying click/ARIA/cursor signals to
/// `button`. Runs automatically from the parse APIs; exposed for tests and
/// callers that construct IR manually.
std::size_t promote_interactive_frames(IRNode& root);

/// Parse a CSS `box-shadow` value into ordered layers. Splits on top-level
/// commas only (commas inside `rgb()`/`rgba()` are preserved), then parses
/// each layer's offsets/blur/spread/color/inset. Numeric lengths accept an
/// optional `px` suffix. Empty / "none" input yields an empty vector. Each
/// layer keeps its trimmed original text in `raw` for lossless round-trip.
std::vector<IRBoxShadow> parse_css_box_shadow(const std::string& css);

/// Serialize ordered box-shadow layers back to a CSS `box-shadow` string,
/// preferring each layer's `raw` text when present (lossless) and otherwise
/// reconstructing from the parsed fields. Empty input yields an empty string.
std::string box_shadow_to_css(const std::vector<IRBoxShadow>& shadows);

// ── Phase 0a (planning/2026-05-18-inspector-direct-manipulation-roadmap.md):
// additive identity fields on IRNode. The TS-side @pulp/import-ir package
// already defines the canonical shape; the C++ IR lags. Phase 0a is
// deliberately additive — IRStyle/IRLayout migration to TS-style
// paint/text/layout is a separate ~1-month effort tracked elsewhere.
//
// stable_anchor_id is the key the tweaks layer (pulp-tweaks.json) uses to
// match user edits back to nodes across re-imports. Computed by the
// anchor strategies in <pulp/view/anchor_strategy.hpp> (mirror of
// packages/pulp-import-ir/src/anchors.ts).
//
// provenance + confidence let the inspector (and downstream tooling) know
// where a node came from and how confident the adapter was — critical
// for surfacing "this was sampled from an image, not a literal" in the
// future Lock-to-source UI.

/// Confidence the adapter has that a node was lowered correctly. Maps to
/// the TS Confidence enum (`PASS` | `DIVERGE` | `NOT_IMPL`). Drives the
/// `pulp ui validate` exit code in CI.
enum class IRConfidence {
    pass,        // Adapter fully understood the source node
    diverge,     // Adapter produced something close-but-not-equivalent
    not_impl     // Adapter does not know how to lower this node yet
};

/// Adapter-side provenance: who lowered this node, from what source.
struct IRProvenance {
    std::string adapter;     // e.g. "figma", "stitch-html", "claude-design-html"
    std::string version;     // adapter version string ("1.0.0" or schema rev)
    std::string source_uri;  // source identifier (file path, URL, MCP handle)
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
    // True when the source explicitly carried a min/max range (vs the 0..1
    // defaults). Lets the codegen reconstruct the value/range display stack only
    // for widgets that actually declared a range. pulp #3192 follow-up.
    bool has_audio_range = false;
    std::vector<IRNode> children;
    std::unordered_map<std::string, std::string> attributes;  // Extra metadata

    // ── Phase 0a additive identity fields ────────────────────────────────
    /// Stable anchor for the tweaks layer. Empty until an anchor strategy
    /// has populated it (call assign_anchors() in anchor_strategy.hpp).
    std::optional<std::string> stable_anchor_id;
    /// Strategy used to produce stable_anchor_id:
    /// "adapter", "content-hash", or "path".
    std::optional<std::string> anchor_strategy;
    /// Source-side native ID (e.g. Figma layer UUID, Pencil node ID).
    /// Only populated for sources that have native IDs.
    std::optional<std::string> source_node_id;
    /// Canonical adapter provenance fields. Kept alongside IRProvenance
    /// for compatibility with older call sites.
    std::optional<std::string> source_adapter;
    std::optional<std::string> source_version;
    /// Adapter provenance — typically set on the root node by each parser.
    std::optional<IRProvenance> provenance;
    /// Adapter's confidence in this node's lowering.
    std::optional<IRConfidence> confidence;
    /// Verbatim source slice for this node (debugging + AST-patch fallback).
    /// Optional; adapters may leave empty when the source is binary or large.
    std::optional<std::string> raw_source;
};

enum class ImportDiagnosticSeverity {
    info,
    warning,
    error
};

enum class ImportDiagnosticKind {
    unknown,
    unsupported_property,
    unresolved_asset,
    snapshot_semantics_warning,
    legacy_field_shortcut,
    capture_partial,
    fallback_used
};

struct ImportDiagnostic {
    ImportDiagnosticSeverity severity = ImportDiagnosticSeverity::warning;
    std::string code;
    std::string path;
    std::string message;
    ImportDiagnosticKind kind = ImportDiagnosticKind::unknown;
    std::optional<std::string> anchor_id;
    std::optional<std::string> property;
};

struct IRAssetRef {
    std::string asset_id;       // stable handle used by nodes
    std::string original_uri;   // as authored
    std::vector<std::string> original_uri_aliases; // equivalent authored refs
    std::optional<std::string> local_path;
    std::string content_hash;   // sha256 of resolved bytes; empty if unresolved
    std::string mime;
    std::optional<int> width;
    std::optional<int> height;
    std::optional<std::string> font_family;
    std::optional<std::string> license;
    std::optional<std::string> source_url;
    std::vector<ImportDiagnostic> diagnostics;
};

struct IRAssetManifest {
    std::vector<IRAssetRef> assets;
    int version = 1;

    const IRAssetRef* resolve(std::string_view asset_id) const;
};

struct IRTokenIdentity {
    std::string source_id;
    std::string source_collection;
    std::string source_mode;
    std::string source_adapter;
};

/// Design token collection (W3C-compatible).
struct IRTokens {
    std::unordered_map<std::string, std::string> colors;       // name → "#hex"
    std::unordered_map<std::string, float> dimensions;         // name → px value
    std::unordered_map<std::string, std::string> strings;      // name → string
    // Keyed by canonical token path (for example "colors.primary").
    std::unordered_map<std::string, IRTokenIdentity> source_identity;
};

/// Complete design IR document.
struct DesignIR {
    int version = 1;
    IRNode root;
    IRTokens tokens;
    IRAssetManifest asset_manifest;
    std::vector<ImportDiagnostic> diagnostics;
    DesignSource source = DesignSource::figma;
    std::string source_file;
    std::string capture_method;
    int settle_rounds = 0;
    std::string fallback_reason;
    std::string source_adapter;
    std::string source_version;
    std::string imported_at;
};

struct DesignIrJsonOptions {
    int version = 0; // 0 preserves DesignIR::version
    bool include_tokens = true;
    bool include_source_metadata = true;
    bool include_asset_manifest = true;
};

struct DesignIrAssetOptions {
    bool allow_network_fetch = false;
    int network_timeout_ms = 30000;
    std::filesystem::path cache_directory;
    std::filesystem::path base_directory;
    std::string base_url;
    std::unordered_map<std::string, std::string> expected_hash_by_uri;
};

/// Serialize a DesignIR as a deterministic, versioned canonical JSON
/// envelope. The reader remains permissive; this writer emits only the
/// canonical nested form.
std::string serialize_design_ir(const DesignIR& ir,
                                const DesignIrJsonOptions& options = {});

/// Parse either the v1 canonical envelope or a legacy bare-node IR JSON
/// document. Legacy source adapters keep their JSON-first compatibility by
/// routing through this reader.
DesignIR parse_design_ir_json(const std::string& json);

/// Collect all external resources referenced by the IR into a manifest.
/// Network fetches require explicit opt-in through options.
IRAssetManifest collect_design_ir_assets(const DesignIR& ir,
                                         const DesignIrAssetOptions& options = {});

/// Rebuild and store ir.asset_manifest from the current tree.
void refresh_design_ir_asset_manifest(DesignIR& ir,
                                      const DesignIrAssetOptions& options = {});

} // namespace pulp::view
