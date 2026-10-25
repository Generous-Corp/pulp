// Captured @font-face faces for the browser-capture lane.
//
// Chromium draws a page's text with the fonts the page itself serves. A bundle
// that loads its fonts from Blob URLs records each @font-face rule and the
// font bytes in the materialized-document sidecar, and the native import has
// to carry them, or every captured run is re-shaped with whatever same-named
// face the host happens to have: a different file, often a static face where
// the page used a variable one, so the weight is synthesized and the captured
// line layout is rejected for naming a face native never resolves.

#pragma once

#include <pulp/view/design_ir.hpp>
#include <pulp/view/design_source_types.hpp>

#include <filesystem>
#include <string>

namespace pulp::import_design {

/// Add `materialized`'s captured @font-face rules to `ir`, in-place.
///
/// Each referenced font asset is written once into `font_directory`, named by
/// content hash, and recorded in the asset manifest under its stable
/// materialized asset id. Each rule becomes one `font_family_assets` entry, in
/// the order the page defined them, with its family, weight (or weight
/// range), style and unicode-range descriptors and the font's path.
///
/// A document with no font bindings leaves `ir` unchanged and returns true.
/// Returns false and sets `error` when a font cannot be written, so an import
/// never silently loses the faces its capture was drawn with.
bool attach_captured_font_faces(pulp::view::DesignIR& ir,
                                const pulp::view::ClaudeBundle& materialized,
                                const std::filesystem::path& font_directory, std::string& error);

/// Add the @font-face faces a plain page loaded from url() sources, recorded
/// by the capture in `font_faces_json` (font-faces.json beside capture.json,
/// schema pulp-browser-capture-font-faces-v1), to `ir`, in-place.
///
/// Fonts are written and bound exactly as `attach_captured_font_faces` writes
/// and binds materialized ones, under the asset id
/// `pulp-page-font-<sha256>`. A missing file means the page loaded no url()
/// face and leaves `ir` unchanged. Returns false and sets `error` when the
/// record is malformed, a face names bytes it does not carry, or the bytes do
/// not match their recorded SHA-256.
bool attach_page_font_faces(pulp::view::DesignIR& ir, const std::filesystem::path& font_faces_json,
                            const std::filesystem::path& font_directory, std::string& error);

} // namespace pulp::import_design
