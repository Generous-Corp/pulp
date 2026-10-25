// Captured @font-face faces for the browser-capture lane.
//
// Chromium draws a page's text with the fonts the page itself serves, from
// files beside it, inline data, or Blob URLs a bundle creates. The capture
// records each face it loaded, with the font bytes, in font-faces.json, and
// the native import has to carry them, or every captured run is re-shaped
// with whatever same-named face the host happens to have: a different file,
// often a static face where the page used a variable one, so the weight is
// synthesized and the captured line layout is rejected for naming a face
// native never resolves.

#pragma once

#include <pulp/view/design_ir.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace pulp::import_design {

/// Add the @font-face faces a page loaded from url() sources, recorded by the
/// capture in `font_faces_json` (font-faces.json beside capture.json, schema
/// pulp-browser-capture-font-faces-v1), to `ir`, in-place.
///
/// Each font is written once into `font_directory`, named by content hash,
/// and recorded in the asset manifest: a bundle's Blob font under
/// `pulp-materialized-asset-<sha256>`, the id its materialized runtime uses,
/// and any other font under `pulp-page-font-<sha256>`. Each rule becomes one
/// `font_family_assets` entry, in the order the page defined them, with its
/// family, weight (or weight range), style and unicode-range descriptors.
/// A missing file means the page loaded no url() face and leaves `ir`
/// unchanged. Each face the capture dropped (bytes that are not a font,
/// WOFF 1.0, a file Chromium could not decode) adds one line to `warnings`,
/// naming it and why. Returns false and sets `error` when the record is
/// malformed, a face names bytes it does not carry, the bytes do not match
/// their recorded SHA-256, or they are not a TrueType, OpenType or WOFF2 font.
bool attach_page_font_faces(pulp::view::DesignIR& ir, const std::filesystem::path& font_faces_json,
                            const std::filesystem::path& font_directory,
                            std::vector<std::string>& warnings, std::string& error);

} // namespace pulp::import_design
