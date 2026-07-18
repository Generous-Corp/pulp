#pragma once

/// @file design_import.hpp
/// Import design files from external tools (Figma, Stitch, v0, Pencil) into
/// Pulp's web-compat JS format. Supports a normalized intermediate
/// representation (IR) and W3C Design Tokens.
///
/// This is a back-compat umbrella header. The declarations are split across
/// focused headers (included below in dependency order):
///   * design_ir.hpp        — IR model + canonical JSON / asset entry points
///   * design_tokens.hpp    — W3C tokens + Figma/Stitch token sync
///   * design_sources.hpp   — source adapters (Figma/Stitch/v0/Pencil/Claude,
///                            DESIGN.md, Claude/React runtime bundles)
///   * design_shortcuts.hpp — keyboard-shortcut import + default shortcuts
///   * design_codegen.hpp   — JS / baked-C++ code generation
///
/// The native-binding materialization surface (build_native_view_tree and the
/// NativeImport* binding descriptors / context) still lives directly in this
/// umbrella; it has not been split out yet.

#include <pulp/view/design_ir.hpp>
#include <pulp/view/design_tokens.hpp>
#include <pulp/view/design_sources.hpp>
#include <pulp/view/design_shortcuts.hpp>
#include <pulp/view/design_codegen.hpp>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulp::view {

class View;
class Checkbox;
class Knob;
class Fader;
class Meter;
class ToggleButton;
class TextButton;
class TextEditor;
class XYPad;
class WaveformView;

// ── Import report ────────────────────────────────────────────────────────────
// Surfaces the per-control resolution provenance the IR carries so a
// low-confidence or conflicted control is SEEN at import time — in the CLI and
// as machine-readable JSON a CI gate can threshold — instead of discovered later.
struct ImportReportEntry {
    std::string source_node_id;       ///< Figma node id (empty if unknown)
    std::string kind;                 ///< resolved interactive-element kind
    int resolution_rung = 0;          ///< 0=unset..5=inert (see InteractiveElementKind)
    float confidence_score = 1.0f;    ///< 0..1
    std::vector<std::string> conflict_signals;  ///< cross-signal conflicts
    bool verification_pass = true;
};

struct ImportReport {
    std::vector<ImportReportEntry> controls;
    int conflicted = 0;       ///< controls with >=1 conflict signal
    int low_confidence = 0;   ///< controls with confidence below the threshold
    int unresolved = 0;       ///< controls resolved only at the inert rung (5)
    /// True when the import is clean enough to pass a CI gate at the given policy
    /// (no conflicts and nothing inert). low_confidence alone is advisory.
    bool ok() const { return conflicted == 0 && unresolved == 0; }
};

// Walk a parsed DesignIR root and collect the resolution report over every
// interactive element (recursively). `low_confidence_threshold` flags controls
// whose confidence is below it (default 0.6).
ImportReport collect_import_report(const IRNode& root,
                                   float low_confidence_threshold = 0.6f);

// Render an ImportReport as JSON (for a CI gate / tooling) or a human summary.
std::string import_report_to_json(const ImportReport& report);
std::string import_report_to_text(const ImportReport& report);

// Render-placement verification. Walks the IR and flags interactive overlays
// that cannot render where
// they claim to: a degenerate extent (no hit radius and a zero-area box), or a
// box that falls entirely outside the node's own render region [0,0,frame_w,
// frame_h] (when the frame size is known, >0). A flagged control gets
// verification_pass=false plus a recorded conflict, so collect_import_report
// surfaces it and --fail-on-unresolved can gate on it. Mutates `root` in place;
// returns the number of controls newly flagged. frame_w/h <= 0 means "unknown"
// (skip the bounds half, keep the degenerate-extent check). This is a
// geometry-level check, not a pixel diff.
int apply_placement_verification(IRNode& root, float frame_w = 0.0f, float frame_h = 0.0f);

// Swap-target verification. Walks the IR and flags every `swap` element whose
// target_frame does not name a frame that was actually captured on its own node
// — an unset target (-1), a negative index, an index past the captured frame
// count (1 + alternate_frames.size()), or a self-target that would make the
// button a no-op. Such a swap would render as a button that silently does
// nothing, so each one gets verification_pass=false plus a conflict signal,
// which makes collect_import_report count it as `conflicted` and
// --fail-on-unresolved exit nonzero. Mutates `root` in place; returns the number
// of swaps newly flagged. A design with no swap elements is untouched.
int apply_swap_target_verification(IRNode& root);

// One node that captured alternate frames nobody can render.
struct UnrenderableFrameSet {
    std::string node_name;      ///< the node's name, or "<unnamed>"
    std::size_t alternates;     ///< how many captured states would be dropped
    std::string reason;         ///< why they cannot render
};

// Alternate frames are consumed by exactly one lowering — the faithful_svg one
// (DesignFrameView::add_frame in both the C++ codegen and the native
// materializer). A node that carries alternate_frames but is NOT a renderable
// faithful node (wrong render_mode, or no svg_asset_id) therefore has its extra
// states silently dropped: the import "succeeds" and emits a single frame. This
// walk reports every such node so a caller can fail instead of dropping states
// without a word. An empty result means every captured state will render.
std::vector<UnrenderableFrameSet> find_unrenderable_alternate_frames(const IRNode& root);

struct NativeMaterializeOptions {
    bool apply_token_theme = true;
    bool preview_mode = false;
    std::vector<ImportDiagnostic>* diagnostics_out = nullptr;
};

struct NativeImportBindingOptions {
    std::vector<ImportDiagnostic>* diagnostics_out = nullptr;
};

struct NativeImportBindingDescriptor {
    std::string_view route_id;
    std::string_view param_key;
    std::string_view binding_module;
    std::string_view binding_param;
    std::string_view event_contract;
    std::string_view gesture_contract;
};

struct NativeImportXYPadBindingDescriptor {
    std::string_view route_id;
    std::string_view x_param_key;
    std::string_view y_param_key;
    std::string_view x_binding_module;
    std::string_view x_binding_param;
    std::string_view y_binding_module;
    std::string_view y_binding_param;
    std::string_view event_contract;
    std::string_view gesture_contract;
};

struct NativeImportMeterBindingDescriptor {
    std::string_view route_id;
    std::string_view meter_source;
    std::string_view channel;
    std::string_view value_key;
    std::string_view event_contract;
};

struct NativeImportChoiceBindingDescriptor {
    std::string_view route_id;
    std::string_view param_key;
    std::string_view choice_value;
    std::string_view choice_label;
    std::string_view event_contract;
    std::string_view gesture_contract;
};

struct NativeImportWaveformBindingDescriptor {
    std::string_view route_id;
    std::string_view param_key;
    std::string_view shape;
    std::string_view event_contract;
};

struct NativeImportTextBindingDescriptor {
    std::string_view route_id;
    std::string_view value_key;
    std::string_view initial_value;
    std::string_view placeholder;
    std::string_view event_contract;
    std::string_view focus_contract;
};

struct NativeImportHostActionDescriptor {
    std::string_view route_id;
    std::string_view action;
    std::string_view label;
    std::string_view payload_contract;
    std::string_view event_contract;
    std::string_view gesture_contract;
};

class NativeImportBindingContext {
public:
    /// Descriptor string_view fields passed to bind_* callbacks are borrowed
    /// and valid only for the duration of the callback. Binding contexts must
    /// copy any descriptor fields they retain.
    virtual ~NativeImportBindingContext();

    /// Claim a materialized view/route pair before installing callbacks.
    /// Returns false if this context already claimed the same view/route,
    /// allowing repeated binder calls on the same tree/context to fail closed.
    /// Rebuilt view trees can be bound with the same context because claims are
    /// keyed by a per-View lifetime id in addition to the View address. Call
    /// reset_import_binding_claims() before deliberately rebinding the same
    /// materialized tree.
    bool claim_import_binding(View& view, std::string_view route_id);
    void reset_import_binding_claims();

    virtual void bind_knob(Knob& knob, const NativeImportBindingDescriptor& descriptor) {
        (void)knob;
        (void)descriptor;
    }
    virtual void bind_fader(Fader& fader, const NativeImportBindingDescriptor& descriptor) {
        (void)fader;
        (void)descriptor;
    }
    virtual void bind_meter(Meter& meter, const NativeImportMeterBindingDescriptor& descriptor) {
        (void)meter;
        (void)descriptor;
    }
    virtual void bind_toggle_button(ToggleButton& button, const NativeImportBindingDescriptor& descriptor) {
        (void)button;
        (void)descriptor;
    }
    virtual void bind_choice_button(ToggleButton& button, const NativeImportChoiceBindingDescriptor& descriptor) {
        (void)button;
        (void)descriptor;
    }
    virtual void bind_xy_pad(XYPad& pad, const NativeImportXYPadBindingDescriptor& descriptor) {
        (void)pad;
        (void)descriptor;
    }
    virtual void bind_waveform_display(WaveformView& waveform,
                                       const NativeImportWaveformBindingDescriptor& descriptor) {
        (void)waveform;
        (void)descriptor;
    }
    virtual void bind_text_editor(TextEditor& editor, const NativeImportTextBindingDescriptor& descriptor) {
        (void)editor;
        (void)descriptor;
    }
    virtual void bind_host_action(TextButton& button, const NativeImportHostActionDescriptor& descriptor) {
        (void)button;
        (void)descriptor;
    }
    virtual void bind_checkbox(Checkbox& checkbox, const NativeImportBindingDescriptor& descriptor) {
        (void)checkbox;
        (void)descriptor;
    }
};

std::unique_ptr<View> build_native_view_tree(
    const DesignIR& ir,
    const IRAssetManifest& manifest,
    const NativeMaterializeOptions& options = {});

/// Apply explicit `pulp*` binding metadata from `ir` to an already materialized
/// native view tree. This is opt-in: building the tree does not install
/// callbacks or touch host/parameter state unless a caller invokes this helper
/// with a binding context.
void bind_native_view_tree(View& root,
                           const DesignIR& ir,
                           NativeImportBindingContext& ctx,
                           const NativeImportBindingOptions& options = {});

/// Resolve imported image `asset_ref` nodes against an asset manifest and stamp
/// source-derived metadata onto the nodes: absolute `asset_path`, PNG natural
/// dimensions, opaque-core bounds for render-bounds sprites, and `asset_bleed`
/// when the PNG is much larger than the logical box. This is a preprocessing
/// step for baked native/codegen paths; it does not modify the manifest.
void enrich_imported_image_asset_metadata(DesignIR& ir,
                                          const IRAssetManifest& manifest,
                                          std::string_view base_directory = {});

/// Captured-art knob promotion. The figma-plugin "Export to Pulp" envelope
/// captures a skeuomorphic knob's body as an asset-backed image child (a disc
/// PNG) plus, often, a small separate indicator/pointer layer. Without this
/// pass the native materializer synthesizes a default Knob and discards the
/// captured art (the knob looks wrong — a generic value-arc instead of the
/// design's disc). For each name/metadata-detected knob node:
///   - exactly one substantial captured layer (+ only small pointer layers the
///     native rotating notch replaces): HOIST the body disc's `asset_ref` +
///     `render_bounds` onto the knob node and drop the captured children. The
///     materializer then skins the knob with the disc and overlays the native
///     notch — the knob stays INTERACTIVE and looks like the design.
///   - two or more SUBSTANTIAL captured layers (body + highlight + logo …):
///     demote to a plain container (`audio_widget = none`) so every layer
///     renders as an image — faithful but not turnable; no silent layer loss.
///   - zero captured layers: left as a default synthesized knob.
/// Run this BEFORE enrich_imported_image_asset_metadata so the hoisted
/// `asset_ref` receives its absolute `asset_path` + opaque-core metadata.
void hoist_captured_art_knobs(DesignIR& ir);

/// Source-agnostic IR normalization for vector SHAPE PRIMITIVES. Walks the tree
/// and, for each rect/rectangle/line/ellipse/circle/polygon/star node that
/// would otherwise be dropped to an empty frame — no `path_data`, no children,
/// no visible fill, no rasterized asset, not an audio widget — synthesizes an
/// SVG path `d` (plus `svg_viewbox` and, when the node carries a border, an
/// `svg_stroke`/`svg_stroke_width`; `svg_fill` is forced to "none" so the
/// SvgPathWidget's default opaque-black fill never paints a phantom box) from
/// the node's geometry. Codegen then lowers it to a native SvgPath via
/// createSvgPath+setSvgPath instead of silently dropping the shape. Idempotent:
/// nodes that already carry `path_data`, render some other way, or are not
/// synthesizable primitives are left untouched. Geometry is derived from IR
/// fields only (width/height, corner radii, and optional pointCount/innerRadius
/// attributes) — never a layer name or source quirk.
void synthesize_primitive_paths(IRNode& root);

/// Split the CSS `border` shorthand — "1px solid #333" — into the discrete
/// border_color / border_width / border_style fields, for every node in the
/// tree that set the shorthand but not the parts.
///
/// IRStyle carries both spellings, and the producers all write the shorthand:
/// the .fig decoder, the Claude bundle reader, and the v0 TSX reader. Every
/// native consumer reads only the parts — codegen's setBorder requires
/// border_color AND border_width, and synthesize_node moves border_color onto a
/// synthesized path. So a stroke declared in a design reached the IR and then
/// went nowhere: six declared strokes lowered to zero setBorder + zero
/// setSvgStroke calls in a real 1115-node import.
///
/// Only fills gaps — a producer that already set the parts wins, since it said
/// what it meant more precisely than the shorthand can.
void normalize_border_shorthand(IRNode& root);

/// Reconnect a slider's progress fill to its thumb.
///
/// A Figma slider component stores three absolutely-positioned children — a
/// full-width track, a short progress "fill", and a round thumb — and the fill's
/// stored x/width is a per-instance, value-driven position. Figma's *live*
/// component render recomputes the fill from the current value so it always
/// touches the thumb, but the stored geometry in a `.fig` (or a REST/plugin
/// export of one) does not: an instance can persist a fill that floats in a gap
/// away from the thumb. Rendering that stored geometry faithfully reproduces a
/// visually broken "detached red bar" that no design ever intended to show.
///
/// This pass detects the track+fill+thumb triplet structurally (never by layer
/// name) and, ONLY when the fill and thumb are horizontally disjoint, extends
/// the fill across the gap so it meets the thumb — the minimal edit that removes
/// the detached-bar artifact without inventing a value. A fill that already
/// touches or overlaps its thumb is left exactly as stored. Detection is tight
/// (short wide container, near-full-width thin track, thin colored fill, round
/// thumb) so non-slider rows are never rewritten.
void reconnect_slider_fill(IRNode& root);

/// Bind geometry-detected controls to host parameters from an out-of-band
/// manifest that maps a Figma node id (`IRInteractiveElement::source_node_id`,
/// stamped by the producer) to a host-param key. For each interactive element
/// whose `source_node_id` is present in `node_id_to_param_key` with a non-empty
/// value AND whose `param_key` is still empty, sets `param_key` from the manifest.
/// Recurses into children. Returns the number of controls bound.
///
/// This is the binding path for DESCRIPTIVELY-named controls — the common Figma
/// case (a knob named "Cutoff", not `param:filter.cutoff`), where the producer
/// stamps provenance but no key. An explicit producer binding (a layer-name
/// sigil) always WINS: the manifest never overwrites a non-empty `param_key`, so
/// a sigil and a manifest entry for the same node resolve to the sigil. A node id
/// absent from the manifest, or mapped to an empty string, is left untouched.
int apply_param_binding_manifest(
    IRNode& root,
    const std::unordered_map<std::string, std::string>& node_id_to_param_key);

/// Parse a `--param-binding-manifest` document — a flat JSON object mapping a
/// Figma node id to a host-param key, e.g. `{"10:42": "filter.cutoff"}` — into a
/// map suitable for apply_param_binding_manifest. Entries with an empty key, a
/// non-string value, or an empty value are skipped (lenient, like the IR parser).
/// Returns std::nullopt only on malformed JSON or a non-object root, setting
/// `*error` (when non-null).
std::optional<std::unordered_map<std::string, std::string>>
parse_param_binding_manifest_json(const std::string& json, std::string* error);

} // namespace pulp::view
