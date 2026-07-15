#pragma once

// Annotated-capture import lane.
//
// The faithful-vector Figma lane (make_catalog_component.py + the Figma plugin's
// scene export) works because the SOURCE carries semantics: typed nodes, node ids,
// component names. A bare SVG capture — a screenshot vectorized, a hand-authored
// asset, or a UI capture run through an extractor — has NONE of that: it
// is just paths and rects. This lane is the no-metadata analog: accept a bare
// captured SVG plus a SIDECAR MANIFEST that supplies the missing semantics
// (per-element selector/heuristic, kind, geometry, needle path, host-param key),
// and generate the same artifacts the Figma path emits — a populated
// DesignFrameElement table, a DesignFrameView subclass stub, and the wiring lines.
//
// The sidecar schema is deliberately generic, so that ANY external UI extractor
// — whatever it extracted FROM — can emit a manifest this lane consumes directly:
// a widget kind, a bounding box, and a parameter key are all it needs to supply.
// Fields:
//
//   {
//     "name":  "Reverb Panel",           // catalog display name
//     "class": "ReverbPanelView",         // C++ class to generate
//     "elements": [
//       { "selector": "#mix",             // source id / heuristic -> source_node_id
//         "kind": "knob",                 // DesignFrameElement::Kind name
//         "param_key": "mix",             // host-parameter binding key
//         "geometry": { "cx":120, "cy":90, "hit_radius":34,
//                       "x":0,"y":0,"w":0,"h":0,
//                       "value":0.5, "value_y":0.5 },
//         "needle": "M120 90 L120 60",    // -> needle_d (knob/fader/xy_pad/switch)
//         "options": ["Hall","Room"],     // dropdown / tab_group
//         "selected_index": 0,
//         "text": "C2", "placeholder": "", "bg_color": "#101418",
//         "note": 60, "view_group": -1, "target_frame": -1,
//         "action": "octave_up",
//         "factory_id": "", "custom_props": "" }
//     ]
//   }
//
// The manifest parse (bare SVG bytes + JSON) → std::vector<DesignFrameElement> is
// the testable core; the two generate_* functions turn a parsed manifest into
// compile-shaped C++ source text.

#include <string>
#include <vector>

#include <pulp/view/design_frame_view.hpp>

namespace pulp::import_design {

struct AnnotatedCaptureManifest {
    std::string name;         ///< catalog display name
    std::string class_name;   ///< C++ class to generate (e.g. ReverbPanelView)
    std::vector<view::DesignFrameElement> elements;

    /// True when any element declares a host-parameter key — the generated view
    /// then enables route_changes_to_host_params(true).
    bool has_param_bindings() const;
};

/// Parse a sidecar manifest JSON into a typed manifest. Returns false and fills
/// `error` on invalid JSON, a missing/unknown `kind`, or a non-array `elements`.
/// Unknown geometry / attribute fields are ignored (forward-compatible); missing
/// ones keep the DesignFrameElement default.
bool parse_annotated_manifest(const std::string& json,
                              AnnotatedCaptureManifest& out, std::string& error);

/// snake_case a ClassName (ReverbPanelView -> reverb_panel_view). Matches the
/// convention make_catalog_component.py uses for generated file names.
std::string snake_case(const std::string& class_name);

/// The DesignFrameView subclass header (`class X : public DesignFrameView`).
std::string generate_view_header(const AnnotatedCaptureManifest& m);

/// The subclass implementation: a `build_<snake>_elements()` helper that
/// materializes the typed element table, and a ctor that decodes the embedded SVG
/// (via the `<svg_b64_symbol>()` accessor the caller also generates, mirroring the
/// Figma path's <snake>_svg.cpp) and enables host-param routing when the manifest
/// declares any param_key. `svg_b64_symbol` is the fully-qualified accessor name,
/// e.g. "pulp::view::detail::reverb_panel_view_svg_b64".
std::string generate_view_source(const AnnotatedCaptureManifest& m,
                                 const std::string& svg_b64_symbol);

}  // namespace pulp::import_design
