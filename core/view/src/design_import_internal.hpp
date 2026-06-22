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
#include <string>
#include <string_view>
#include <vector>

#include <choc/text/choc_JSON.h>

#include <pulp/view/design_import.hpp>

namespace pulp::view {

// Clears the baked indicator "antenna" that ELYSIUM-style captured knob discs
// paint standing straight up ABOVE the disc body at 12 o'clock (we draw our own
// rotating pointer, so the baked one is a stuck second line). Operates on a raw
// RGBA8 buffer: from the TOP of the [core_x, core_y, core_w, core_h] disc bbox,
// it clears the narrow opaque antenna span row-by-row and STOPS at the first
// wide (disc-body) row — so the ring outline, face, and bottom min/max ticks are
// never touched (no notch/gap). The antenna is found by its actual opaque span
// per row, NOT assumed centered (the min/max ticks skew the bbox center).
// Pure + testable; defined in design_import.cpp. `[knob][antenna]`.
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

// ── DesignIR JSON split (2026-05-29 frontend-IR refactor, PR-1) ──────────
// These five symbols cross the design_import.cpp / design_ir_json.cpp
// boundary. The DesignIR JSON serialize/deserialize band moved into
// design_ir_json.cpp; the asset pipeline and per-source parsers stayed in
// design_import.cpp. The four parsers below are defined in
// design_ir_json.cpp (promoted from static to external linkage) and called
// from design_import.cpp; promote_interactive_frames is the reverse —
// defined in design_import.cpp and called from the JSON parsers.

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
