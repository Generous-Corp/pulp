#pragma once

#include <string>

// SVG fragment handles.
//
// A faithful design port renders one big SVG document (the frame) via
// Canvas::draw_svg, then needs to restyle or move ONE sub-tree of it on demand:
// brighten the hovered control, dim a bypassed one, drop a reorder ghost, spin a
// meter needle. Re-exporting a fresh SVG per state is wasteful; mutating the live
// document (the wrap_needle_rotation model in design_frame_view.cpp) works for
// the one dragged control but doesn't compose for overlays that must paint ON TOP
// of the already-drawn frame with their own transform/opacity/recolor.
//
// The fragment-handle primitive is the composable answer: extract a named
// sub-tree once (by a unique marker substring — typically a path `d`, the same
// handle wrap_needle_rotation keys on — reusing the SVG-as-string model the
// suppress_svg_* helpers already establish), then build a STANDALONE mini-document
// that shares the source's `<svg …>` header (so coordinates line up 1:1 with the
// full render) containing only that fragment, wrapped in an optional
// transform + opacity and recolored. Drawing that mini-document through the SAME
// draw box the full frame used composites the restyled fragment exactly over its
// original position.
//
// These are pure string/geometry helpers with no Canvas or GPU dependency, so the
// extraction + document-build + transform math is fully unit-testable in a
// headless, Skia-free build; DesignFrameView layers the draw_svg call on top.
namespace pulp::view {

// An affine restyle applied to a fragment: a translate, then a rotate (degrees,
// about a pivot), then a uniform scale (about the same pivot). Emitted as an SVG
// `transform` attribute in SOURCE (SVG) coordinates — the fragment mini-document
// is drawn through the same panel→view fit as the frame, so the transform reads in
// the design's own coordinate space, matching wrap_needle_rotation's convention.
struct FragmentTransform {
    float dx = 0.0f, dy = 0.0f;        ///< translate, SVG coords
    float rotate_deg = 0.0f;           ///< rotation about (pivot_x, pivot_y)
    float scale = 1.0f;                ///< uniform scale about (pivot_x, pivot_y)
    float pivot_x = 0.0f, pivot_y = 0.0f;

    /// True when this is the do-nothing transform (no attribute is emitted).
    bool is_identity() const;

    /// The `transform="…"` attribute VALUE (without the `transform=` / quotes),
    /// composed outermost-first as SVG applies them: `translate(dx dy)
    /// rotate(deg cx cy) scale(s)`. Empty when is_identity().
    std::string to_svg_transform() const;
};

// Extract the single SVG element that CONTAINS `marker` (a unique substring, e.g.
// a path's `d` value). Returns the element's full source text — a self-closing
// `<… />` element verbatim, or a `<tag …>…</tag>` container with its matching
// (depth-balanced) close tag. Returns "" if the marker isn't found or the element
// is malformed. Mirrors the rfind('<', …) element-start model wrap_needle_rotation
// uses, extended to depth-match a container close.
std::string extract_svg_fragment(const std::string& svg, const std::string& marker);

// Return the source document's opening `<svg …>` tag verbatim (through its '>'),
// which carries width/height/viewBox/xmlns. "" if there is no `<svg` header. The
// fragment mini-document reuses this so its coordinate space is identical to the
// source's.
std::string svg_open_tag(const std::string& svg);

// Recolor a fragment: replace the VALUE of every `fill="…"` (and, when
// `include_stroke`, `stroke="…"`) attribute whose current value is not "none"
// with `hex` (a "#RRGGBB" string). Leaves `fill="none"` alone so an outline-only
// element stays an outline. Returns the recolored fragment. A no-op when `hex` is
// empty. Used for hover-brighten / disabled-desaturate tinting.
std::string recolor_svg_fragment(std::string fragment, const std::string& hex,
                                 bool include_stroke = false);

// Build a standalone `<svg>` document that renders ONLY `fragment`, transformed by
// `xform`, composited at `opacity` (0..1), and optionally recolored to `recolor_hex`.
// `source_svg` supplies the shared `<svg …>` header so the fragment lands at the
// same coordinates as the full frame. When the transform is identity, opacity is
// >= 1, and no recolor is requested, the fragment is emitted unwrapped. Returns ""
// if `source_svg` has no `<svg` header or `fragment` is empty.
std::string build_svg_fragment_document(const std::string& source_svg,
                                        const std::string& fragment,
                                        const FragmentTransform& xform,
                                        float opacity = 1.0f,
                                        const std::string& recolor_hex = {});

}  // namespace pulp::view
