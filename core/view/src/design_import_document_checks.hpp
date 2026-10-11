#pragma once

// Checks on an imported DesignIR document that the materializer would
// otherwise pass over silently: each one names a defect the render cannot
// show, as a structured diagnostic and one `[pulp:warn]` line.

#include <pulp/view/design_ir.hpp>

#include <string_view>
#include <vector>

namespace pulp::view {

/// The newest DesignIR envelope version this build was written against. The
/// reader keeps and round-trips a newer one; materializing it warns.
inline constexpr int kDesignIrNewestKnownVersion = 1;

/// One `document-version-unsupported` warning when @p ir declares an envelope
/// version newer than kDesignIrNewestKnownVersion. Its fields are read as if
/// they meant what they mean in the known version, which may be wrong.
void append_document_version_diagnostic(const DesignIR& ir,
                                        std::vector<ImportDiagnostic>& out);

/// One `node-type-unknown` warning, for the caller to raise on a node whose
/// `type` it does not recognize. The node still imports through whatever its
/// audio role or name resolves to, or as a plain View, so the render does not
/// show that the source asked for something else.
void append_unknown_node_type_diagnostic(const IRNode& node,
                                         std::string_view path,
                                         std::vector<ImportDiagnostic>& out);

/// One `identity-duplicate` warning per node whose `stable_anchor_id` an
/// earlier node (in document order) already carries. Bindings and re-import
/// tweaks address a node by that anchor, so only the first of them is
/// reachable.
void append_duplicate_anchor_diagnostics(const IRNode& root,
                                         std::vector<ImportDiagnostic>& out);

/// An `asset-undecodable` warning when the image file behind @p asset exists
/// but the renderer cannot decode it (corrupt, truncated, or not an image), so
/// the node would paint blank. Returns whether it warned. @p file_uri is the
/// resolved `file://` URI; any other URI, an SVG, or a build without a raster
/// decoder is not checked.
bool append_undecodable_image_diagnostic(const IRNode& node,
                                         std::string_view path,
                                         const IRAssetRef& asset,
                                         std::string_view file_uri,
                                         std::vector<ImportDiagnostic>& out);

}  // namespace pulp::view
