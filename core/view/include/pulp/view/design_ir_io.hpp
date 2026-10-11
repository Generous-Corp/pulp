#pragma once

/// @file design_ir_io.hpp
/// Importer operations over the DesignIR value model: canonical JSON
/// serialize/parse, asset-manifest collection, CSS box-shadow round-trip, and
/// interactive-frame promotion. Part of `pulp-design-import`; the value types
/// they operate on live in design_ir.hpp (`pulp-design-ir`).

#include <pulp/view/design_ir.hpp>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace pulp::view {

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

/// Serialize a DesignIR as a deterministic, versioned canonical JSON
/// envelope. The reader remains permissive; this writer emits only the
/// canonical nested form.
std::string serialize_design_ir(const DesignIR& ir, const DesignIrJsonOptions& options = {});

/// Thrown by parse_design_ir_json for input that is not well-formed JSON:
/// truncated documents, unterminated strings or escapes, unbalanced or
/// over-deep nesting, embedded NUL bytes, and anything the JSON parser itself
/// refuses. `offset` is the byte offset of the problem, or `std::string::npos`
/// when the parser reports only a line and column (included in the message).
struct DesignIrParseError : std::runtime_error {
    DesignIrParseError(const std::string& message, std::size_t byte_offset)
        : std::runtime_error(message), offset(byte_offset) {}
    std::size_t offset;
};

/// Deepest object/array nesting parse_design_ir_json accepts. The JSON
/// parser recurses once per level, so unbounded nesting would exhaust the
/// stack instead of failing.
inline constexpr std::size_t kMaxDesignIrJsonDepth = 512;

/// Parse either the v1 canonical envelope or a legacy bare-node IR JSON
/// document. Legacy source adapters keep their JSON-first compatibility by
/// routing through this reader. Malformed input throws DesignIrParseError;
/// the bytes are bounds-checked before the JSON parser sees them, so a
/// truncated or hostile document cannot read past the end of `json`.
DesignIR parse_design_ir_json(const std::string& json);

/// Collect all external resources referenced by the IR into a manifest.
/// Network fetches require explicit opt-in through options.
///
/// A local file outside options.base_directory is recorded by its absolute
/// path. A loader resolves assets only inside the saved document's directory
/// and refuses that path, so a caller that serializes the IR must first copy
/// such files beside the document and record relative paths, as
/// `pulp import-design` does for every lane that writes a document.
IRAssetManifest collect_design_ir_assets(const DesignIR& ir,
                                         const DesignIrAssetOptions& options = {});

/// Refresh ir.asset_manifest from the current tree while preserving embedded
/// manifest entries whose stable asset_id may already be referenced by nodes.
void refresh_design_ir_asset_manifest(DesignIR& ir, const DesignIrAssetOptions& options = {});

} // namespace pulp::view
