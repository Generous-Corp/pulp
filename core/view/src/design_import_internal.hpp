// design_import_internal.hpp — PRIVATE shared declarations for the
// design-import translation units.
//
// Shared by the split design-import translation units. design_codegen needs
// the motion-provenance vendor key, which is defined (with external linkage,
// at namespace scope) in design_import.cpp; this header gives it a single
// declaration point instead of an ad-hoc extern decl.
//
// PRIVATE: lives under core/view/src/, not the public include tree.
// Not part of the installed SDK surface — do not reference from headers
// outside core/view/src/.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <choc/text/choc_JSON.h>

#include <pulp/view/design_import.hpp>

namespace pulp::view {

// ── PNG codec / pixel surgery boundary ───────────────────────────────────
// Defined in design_import_png.cpp; called from the asset pipeline in
// design_import.cpp. The two structs are the currency the codec and the
// pixel passes exchange, so they live here rather than in either TU.

// A decoded 8-bit RGBA image: tightly packed, four bytes per pixel, row-major.
struct ImportDecodedPng {
    std::vector<uint8_t> rgba;
    int width = 0;
    int height = 0;
    bool valid() const { return !rgba.empty() && width > 0 && height > 0; }
};

// The bounding box of an image's opaque art within its PNG, plus the PNG's own
// dimensions. A captured sprite's art rarely fills its file: the drop shadow /
// glow bleeds past it, so the layout box is fitted to this core, not the file.
struct ImportOpaqueCore {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    int png_w = 0;
    int png_h = 0;
};

// Reads width/height out of a PNG's IHDR. Returns {0, 0} for anything that is
// not a PNG or that declares a non-positive extent.
std::pair<int, int> png_dimensions_from_bytes(const std::vector<uint8_t>& bytes);

// Decodes an 8-bit, non-interlaced PNG (gray / gray+alpha / RGB / RGBA) to
// RGBA8. Returns nullopt for any other PNG shape or a malformed stream.
std::optional<ImportDecodedPng> decode_png_rgba_for_import(const std::vector<uint8_t>& bytes);

// Re-encodes a decoded RGBA buffer as an 8-bit RGBA PNG. Lossless against
// decode_png_rgba_for_import, so a decode → edit-pixels → encode round-trip
// leaves the untouched pixels byte-identical.
std::optional<std::vector<uint8_t>> encode_rgba_png_for_import(const ImportDecodedPng& img);

// The opaque-art bounding box of a PNG's pixels, at the given alpha cutoff.
// Returns nullopt when the image does not decode or is fully transparent.
std::optional<ImportOpaqueCore> compute_import_opaque_core(const std::vector<uint8_t>& bytes,
                                                           float min_alpha = 0.5f);

// Erases the indicator baked into a captured knob disc, in place. Wraps
// clear_baked_knob_antenna with the decoded image's own dimensions.
void clean_baked_knob_indicator(ImportDecodedPng& img, const ImportOpaqueCore& core);

// Samples a shape illustration's own vertical color gradient, bottom→top, as
// up to `n` comma-joined "#rrggbb" stops. Returns "" when the art is too close
// to grey to read as a gradient fill (a logo / icon rather than a fillable
// shape).
std::string sample_shape_fill_gradient(const ImportDecodedPng& img,
                                       const ImportOpaqueCore& core, int n = 5);

// Clears the baked indicator "antenna" that ELYSIUM-style captured knob discs
// paint standing straight up ABOVE the disc body at 12 o'clock (we draw our own
// rotating pointer, so the baked one is a stuck second line). Operates on a raw
// RGBA8 buffer: from the TOP of the [core_x, core_y, core_w, core_h] disc bbox,
// it clears the narrow opaque antenna span row-by-row and STOPS at the first
// wide (disc-body) row — so the ring outline, face, and bottom min/max ticks are
// never touched (no notch/gap). The antenna is found by its actual opaque span
// per row, NOT assumed centered (the min/max ticks skew the bbox center).
// Pure + testable; defined in design_import_png.cpp. `[knob][antenna]`.
void clear_baked_knob_antenna(std::vector<uint8_t>& rgba, int img_w, int img_h,
                              int core_x, int core_y, int core_w, int core_h);

// Motion-provenance vendor key — lowercased, slash-friendly token matching the
// import CLI's `source` argument. Stable across releases (fixtures depend on
// these strings). Defined in design_import.cpp.
const char* design_source_vendor_key(DesignSource source);

// JSON-encode an arbitrary string for safe embedding inside a JS string
// literal (escapes quotes, control chars, and `<` to dodge a premature
// `</script>` close). Defined in claude_bundle.cpp; shared with the
// extracted claude_bundle_sources.cpp source-detection cluster.
std::string json_string_literal(const std::string& s);

// HTML-attribute escaper (&, ", <, >) used by the design-tool runtime-
// import entry points to embed `data-pulp-source` / `data-*-root`
// attributes. Defined in claude_bundle_sources.cpp; shared with the
// runtime harness (parse_jsx_react) in claude_bundle.cpp.
std::string v0_html_attr_escape(const std::string& s);

// ── DesignIR JSON split boundary ─────────────────────────────────────────
// These five symbols cross the design_import.cpp / design_ir_json.cpp
// boundary. DesignIR JSON serialization/deserialization lives in
// design_ir_json.cpp; the asset pipeline and per-source parsers live in
// design_import.cpp. The JSON parsers below are defined in design_ir_json.cpp
// and called from design_import.cpp; promote_interactive_frames is the reverse
// direction, defined in design_import.cpp and called from the JSON parsers.

// Recursively promotes interactive-frame nodes; defined in design_import.cpp.
std::size_t promote_interactive_frames(IRNode& root);

// Maps a serialized audio-widget id back to its enum; defined in
// design_import.cpp (external linkage, no prior declaration) and called by
// parse_ir_node in design_ir_json.cpp.
AudioWidgetType audio_widget_from_id(const std::string& id);

// True when `key` names an asset-reference field; defined in design_ir_json.cpp.
bool is_asset_reference_key(std::string_view key);

// Parse a single DesignIR node tree; defined in design_ir_json.cpp.
IRNode parse_ir_node(const choc::value::ValueView& obj);

// True when `kw` (CSS lowercase-hyphen spelling) is in the supported-blend
// table every lane shares; defined in design_ir_json.cpp.
bool is_supported_blend_keyword(const std::string& kw);

// Clear any mix_blend_mode outside the supported-blend table, pushing a
// `blend-unsupported` diagnostic per cleared node. Call after parse_ir_node on
// every adapter path so an unmappable keyword never reaches codegen as invalid
// CSS (web) or a silent normal-fallback (native). Defined in design_ir_json.cpp.
void validate_blend_modes(IRNode& node, std::vector<ImportDiagnostic>& diagnostics);

// Minimal JSON string escaper; defined in design_ir_json.cpp. Shared with
// design_ir_report.cpp (the import-report JSON emitter) so the report analysis
// pass can live in its own TU without duplicating the escaper.
std::string json_escape(std::string_view text);

// InteractiveElementKind → stable wire id (the recognition kind table); defined
// in design_ir_json.cpp. Shared with design_ir_report.cpp. (The kind↔id table is
// a recognition concern slated to move to the recognition module; this
// declaration keeps the report pass decoupled in the meantime.)
const char* interactive_kind_id(InteractiveElementKind k);

// Parse the DesignIR token table; defined in design_ir_json.cpp.
IRTokens parse_ir_tokens(const choc::value::ValueView& obj);

// Parse a DesignIR asset manifest; defined in design_ir_json.cpp.
IRAssetManifest parse_asset_manifest(const choc::value::ValueView& obj);

// Construct an ImportDiagnostic; defined in design_ir_json.cpp. The default
// `kind` argument lives here only — the definition omits it.
ImportDiagnostic make_import_diagnostic(ImportDiagnosticSeverity severity,
                                        std::string code,
                                        std::string path,
                                        std::string message,
                                        ImportDiagnosticKind kind = ImportDiagnosticKind::unknown);

}  // namespace pulp::view
