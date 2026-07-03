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
/// `IRStyle::box_shadow` keeps them in author order; collapsing the declaration
/// into one opaque string would drop every layer past the first, losing the
/// multi-shadow stack.
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
    // CSS mix-blend-mode keyword (e.g. "multiply", "screen", "color-dodge").
    // Normalized to the lowercase-hyphen CSS spelling at parse time; the
    // engine's View::set_mix_blend_mode + setMixBlendMode bridge consume it.
    std::optional<std::string> mix_blend_mode;
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
    // CSS clip-path / mask. The engine (View::set_clip_path / set_mask /
    // set_mask_image / set_mask_size) + the setClipPath/setMask* bridge
    // consume these; sources that carry them — and Figma mask
    // layers once the extractor emits a clip-path — survive the IR.
    std::optional<std::string> clip_path;
    std::optional<std::string> mask;                   // `mask` shorthand
    std::optional<std::string> mask_image;
    std::optional<std::string> mask_size;
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
    // The asset's true visual extent in logical px when it bleeds past the
    // layout box (figma-plugin `render_bounds`: drop shadows, glow, oversized
    // chrome like the silver-knob graphic). w/h = rendered size, dx/dy = offset
    // of the render origin relative to the layout box (negative = extends
    // left/up). When present + larger than width/height, the importer sizes the
    // image to render_bounds and offsets by dx/dy instead of squashing it.
    struct RenderBounds { float w = 0, h = 0, dx = 0, dy = 0; };
    std::optional<RenderBounds> render_bounds;
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
    // Figma-style resize CONSTRAINTS, normalized to a small token set and mapped
    // onto flex/position at codegen (margin:auto / flex-grow / align-self), never
    // a new layout primitive. Horizontal: left | right | center | scale |
    // stretch (pin both edges). Vertical: top | bottom | center | scale |
    // stretch. Empty = unconstrained (the flex default, anchored start).
    std::optional<std::string> h_constraint;
    std::optional<std::string> v_constraint;
    // CSS Grid. Pulp's engine has a native grid layout (LayoutMode::grid via the
    // createGrid/setGrid bridge); design-import lowers a grid container here
    // instead of flattening it to flex. Container tracks are raw CSS track lists
    // ("1fr 1fr", "100px auto"); auto_flow is "row"/"column"(+ " dense").
    // grid_column/grid_row are per-item placements ("1 / 3", "2"); span/named
    // lines are deferred (left to the flex fallback). display=="grid" OR a
    // template present is the grid signal.
    std::optional<std::string> grid_template_columns;
    std::optional<std::string> grid_template_rows;
    std::optional<std::string> grid_auto_flow;
    std::optional<std::string> grid_column;   // item placement (column line/span)
    std::optional<std::string> grid_row;      // item placement (row line/span)
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

// ── Additive identity fields on IRNode ────────────────────────────────────
// The TS-side @pulp/import-ir package already defines the canonical shape.
// These fields are deliberately additive; IRStyle/IRLayout migration to
// TS-style paint/text/layout is tracked separately.
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

/// One styled character range inside a text node. Offsets are [start, end)
/// UTF-8 BYTE offsets into `IRNode::text_content` (the Figma exporter converts
/// its per-character indices to byte offsets; codegen slices by byte and snaps
/// to codepoint boundaries defensively). Only the fields a run actually overrides
/// are set; everything else inherits the node's dominant style. Mixed-style text
/// (a bold word, a colored span, a different size mid-string) becomes an ordered
/// list of these so codegen can emit per-range <span>s instead of collapsing to
/// the first-char dominant style.
struct IRTextRun {
    int start = 0;
    int end = 0;
    std::optional<float> font_size;
    std::optional<int> font_weight;
    std::optional<std::string> font_style;       // "italic" / "normal"
    std::optional<std::string> color;
    std::optional<float> letter_spacing;
    std::optional<std::string> text_decoration;  // "underline" / "line-through"
};

/// A single node in the normalized design IR.
// ── Faithful-vector import (Plan B) ──────────────────────────────────────────
// How a node should be rendered. `normal` is the existing widget/sprite
// materialization. `faithful_svg` means: render this node's own SVG export
// (svg_asset_id) pixel-faithfully via DesignFrameView, and overlay native
// interaction from `interactive_elements` (source-side semantics — NOT guessed
// from the SVG). Per-node so faithful-vector and sprite nodes coexist.
enum class NodeRenderMode {
    normal,
    faithful_svg,
};

// The kind of an interactive overlay on a faithful_svg node. Deliberately
// separate from AudioWidgetType (which is audio-parameter specific); this models
// design-interaction semantics.
//
// Two materialization mechanisms (see DesignFrameView):
//   - `knob` is SVG-PATCH: its needle path is rotated in the SVG and re-rendered
//     (pixel-perfect, uses cx/cy/hit_radius/svg_patch_d/default_value).
//   - `fader` is SVG-PATCH like `knob` but TRANSLATES its thumb (svg_patch_d) by
//     value along the track box [x,y,w,h] (orientation follows the track shape).
//   - `toggle` is a click-to-flip control over the box [x,y,w,h]; with
//     svg_patch_d set it is a SWITCH whose moving dot is that path.
//   - `dropdown` / `text_field` / `tab_group` / `stepper` are NATIVE-OVERLAY: an
//     opaque child widget (ComboBox / TextEditor / tab group / < > stepper) is
//     positioned over the element's `rect` and replaces that baked SVG region
//     with a live control.
// Every value here maps 1:1 to a DesignFrameElement::Kind in to_frame_elements()
// (design_import_native_common.cpp); the runtime already backs all of them.
enum class InteractiveElementKind {
    knob,
    fader,
    toggle,
    dropdown,
    text_field,
    tab_group,
    // A `< >` stepper: a header value cycled by prev/next chevrons (the design's
    // section-header preset selectors — named "Dropdown" but carrying a `< >`
    // pair, not a down-chevron). Uses `options` + `selected_index` like a
    // dropdown, but steps through them in place instead of opening a popup.
    stepper,
    // `swap` is a swap-link button: clicking its rect activates `target_frame`
    // (a mode/page switch the design wires up).
    swap,
    // `action` is a command button: clicking its rect fires the named `action`
    // (e.g. "octave_up") — the consumer maps the id to its own state. It does not
    // light, emit notes, or swap frames.
    action,
    // `xy_pad` is SVG-patch like `fader` but 2D: dragging in its rect [x,y,w,h]
    // moves the puck (svg_patch_d). `default_value` is the X (0→left, 1→right),
    // `default_value_y` the Y (0→top, 1→bottom).
    xy_pad,
    // `value_label` is a live read-only text overlay painted over its rect (the
    // design's baked readout). `text` is the initial string; `value_left_align`
    // left-aligns it (for a "LABEL <value>" readout that grows rightward).
    value_label,
    // `custom` is a registered native control: the materializer maps
    // it to DesignFrameElement::Kind::custom, whose overlay is built by the
    // factory registered under `factory_id` (+ opaque `custom_props`). Unmapped
    // factory → inert render + an import diagnostic (never a silent knob).
    custom,
};

// One source-identified interactive element overlaid on a faithful_svg render.
// Coordinates are in the SVG's own space. The importer fills this from the
// source (Figma node ids/names/properties), so behavior is real, not inferred.
struct IRInteractiveElement {
    InteractiveElementKind kind = InteractiveElementKind::knob;

    // ── knob (SVG-patch) ────────────────────────────────────────────────
    float cx = 0.0f;                 ///< pivot / hit center, SVG coords
    float cy = 0.0f;
    float hit_radius = 0.0f;         ///< click-target radius, SVG coords
    /// Knob: the `d` of its needle path in the SVG (the patch target a drag
    /// rotates around (cx, cy)). fader/switch reuse this as the translated thumb.
    std::string svg_patch_d;
    float default_value = 0.5f;      ///< 0..1

    /// toggle only: press-flash command button (sample next/prev/random, dice) —
    /// lights on press, clears on release, instead of the sticky on/off flip.
    /// Maps 1:1 to DesignFrameElement::flash. Defaults false (sticky toggle).
    bool flash = false;

    // ── overlay controls (dropdown / text_field / tab_group) ─────────────
    /// Element bounding box in SVG coords — where the native overlay widget is
    /// positioned (via DesignFrameView's panel transform). Zero for knobs.
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    /// dropdown: the selectable options. tab_group: the tab labels. The first
    /// entry is also the design's currently-shown value when selected_index is 0.
    std::vector<std::string> options;
    /// dropdown / tab_group: which option/tab is selected initially.
    int selected_index = 0;
    /// text_field: placeholder text shown until focused/typed.
    std::string placeholder;
    /// text_field: the design's own field background ("#RRGGBB"), so the overlay
    /// matches it exactly and the inset-past-the-icon edge is seamless. Empty →
    /// the default dark field color.
    std::string bg_color;

    // ── swap / action / xy_pad / value_label ─────────────────────────────
    /// swap only: the frame index to activate when clicked (-1 = unset).
    /// Maps 1:1 to DesignFrameElement::target_frame.
    int target_frame = -1;
    /// action only: the command id fired on click (e.g. "octave_up"). Empty =
    /// unset. Maps 1:1 to DesignFrameElement::action.
    std::string action;
    /// value_label only: the initial readout string painted over the rect.
    /// Maps 1:1 to DesignFrameElement::text.
    std::string text;
    /// value_label only: left-align the readout (for a "LABEL <value>" overlay
    /// whose value grows rightward). Maps 1:1 to DesignFrameElement::value_left_align.
    bool value_left_align = false;
    /// xy_pad only: initial normalized Y (0=top, 1=bottom). The X axis reuses
    /// `default_value`. Maps to DesignFrameElement::value_y.
    float default_value_y = 0.5f;

    /// Human-readable name for the control, taken from the design's own caption
    /// text (e.g. the "DEPTH" label under a knob). Empty when the importer found
    /// no confident caption — consumers then fall back to the binding key. This is
    /// the name a host surfaces for the generated parameter (embed ABI v5
    /// PulpEmbedParamInfo.name); unit/range are not carried here yet.
    std::string label;

    /// Host-parameter binding key for this control, e.g. "filter.cutoff". Empty
    /// when the importer found no binding hint (a geometry-detected knob with a
    /// bare layer name) — the control still renders and drags, but drives nothing
    /// until a key is supplied. Maps 1:1 to DesignFrameElement::param_key, which
    /// DesignFrameView resolves against the framework-agnostic HostParamSurface
    /// (JUCE APVTS / iPlug2 / StateStore) when route_changes_to_host_params is on.
    /// This is the binding channel for geometry-detected interactive elements —
    /// the recognized-widget path lowers its binding through IRNode instead.
    std::string param_key;

    // ── Import report (resolution provenance) ────────────────────────────
    // Carried from the importer (where all three signals — geometry/affordance,
    // name/token, component identity — exist) THROUGH to the host materialize
    // boundary, so a low-confidence or conflicted control is SEEN ("this might be
    // wrong") instead of discovered in the DAW. The resolution LOGIC that fills
    // these lives in the importer; this struct carries it to materialization.
    /// Which ladder rung resolved this control's interaction. 0 = not run through
    /// the resolver ladder (legacy/unset); 1 = explicit identity (component/Code-Connect);
    /// 2 = Tier-1 affordance-inferred primitive; 3 = name/token (whole-word);
    /// 4 = registered custom factory; 5 = inert render + diagnostic (the warn).
    int resolution_rung = 0;
    /// Confidence the resolved kind is correct, 0..1. 1.0 = unset/legacy path.
    float confidence_score = 1.0f;
    /// Cross-signal conflicts detected at import (e.g. "name=knob but geometry is
    /// a wide track+thumb"). Empty = no conflict. A non-empty list means the
    /// control still materializes with the best candidate but is flagged for review.
    std::vector<std::string> conflict_signals;
    /// Whether render-level verification (overlay covers its node region, type
    /// doesn't visually contradict the skin) passed. true = passed/unchecked.
    bool verification_pass = true;

    // ── custom (registered control) ──────────────────────────────────────
    /// kind==custom only: the id the native overlay factory is registered under
    /// (register_design_control_factory). Maps 1:1 to DesignFrameElement::factory_id.
    std::string factory_id;
    /// kind==custom only: opaque props handed to the factory (typically JSON).
    /// Maps 1:1 to DesignFrameElement::custom_props.
    std::string custom_props;

    /// Figma node id — provenance for the inspector's Wiring lens (maps a live
    /// control back to its source node). NOT the host-param key; that is param_key.
    std::optional<std::string> source_node_id;
};

struct IRNode {
    std::string type;   // "frame", "text", "image", "button", "input", "slider"
    std::string name;
    std::string text_content;            // For text nodes
    std::vector<IRTextRun> text_runs;    // Per-range style overrides (mixed text)
    IRStyle style;
    IRLayout layout;
    AudioWidgetType audio_widget = AudioWidgetType::none;
    std::string audio_label;             // Label for audio widgets (e.g. "Gain")
    float audio_min = 0.0f;
    float audio_max = 1.0f;
    float audio_default = 0.5f;
    // True when the source explicitly carried a min/max range (vs the 0..1
    // defaults). Lets the codegen reconstruct the value/range display stack only
    // for widgets that actually declared a range.
    bool has_audio_range = false;
    std::vector<IRNode> children;
    std::unordered_map<std::string, std::string> attributes;  // Extra metadata

    // ── Additive identity fields ─────────────────────────────────────────
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

    // ── Faithful-vector import ───────────────────────────────────────────
    /// How this node materializes. `faithful_svg` renders `svg_asset_id` via
    /// DesignFrameView and overlays `interactive_elements`; `normal` keeps the
    /// existing widget/sprite path. Per-node so the two lanes coexist.
    NodeRenderMode render_mode = NodeRenderMode::normal;
    /// Asset id (into IRAssetManifest) of this node's SVG export, when
    /// render_mode == faithful_svg. The asset's mime is image/svg+xml.
    std::optional<std::string> svg_asset_id;
    /// Source-identified interactive overlays for a faithful_svg render.
    /// Empty for `normal` nodes.
    std::vector<IRInteractiveElement> interactive_elements;
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

/// A bundled (non-system) font shipped with the design (figma-plugin #43a:
/// envelope `font_family_assets[]`). The importer registers each one with the
/// text renderer (#43b) so `setFontFamily(family)` resolves to the bundled
/// face instead of silently falling back to a system font of the same name.
struct IRFontAsset {
    std::string family;          // CSS family name nodes reference via font_family
    std::string style;           // "Regular", "Italic", … (informational)
    int weight = 400;            // 100–900
    std::string asset_id;        // → asset_manifest entry holding the .ttf/.otf
    std::string resolved_path;   // absolute fs path, stamped by the CLI asset-resolution pass
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
    std::vector<IRFontAsset> font_family_assets;   // bundled fonts (#43a/#43b)
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

/// Refresh ir.asset_manifest from the current tree while preserving embedded
/// manifest entries whose stable asset_id may already be referenced by nodes.
void refresh_design_ir_asset_manifest(DesignIR& ir,
                                      const DesignIrAssetOptions& options = {});

} // namespace pulp::view
