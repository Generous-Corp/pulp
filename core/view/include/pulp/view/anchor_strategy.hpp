#pragma once

/// @file anchor_strategy.hpp
/// Stable-anchor-id assignment for design-import IR nodes. The rendered
/// `children` axis mirrors the TS-side @pulp/import-ir/src/anchors.ts strategy
/// so the C++ and TS import pipelines produce compatible anchors and the
/// tweaks layer (pulp-tweaks.json) works for either path. Native C++ IR also
/// carries an ordered `alternate_frames` axis; that axis is scoped to this
/// native importer until the TS IR and its consumers grow the same field.
///
/// Three strategies are supported:
///   • content-hash — used by stitch / v0 / claude / generic HTML.
///                    Hash over (tag, role, normalized text, depth, sigIndex).
///   • path         — used by RN file exports / hand-edited code.
///                    "Tag[idx]/Tag[idx]/..." from root.
///   • adapter      — used by Figma / Pencil / Mitosis (sources with
///                    native IDs). "<adapter>:<source_node_id>".
///
/// The walker is purely traversal-shaped — it mutates IRNode::stable_anchor_id
/// but otherwise leaves the tree alone. It visits both the rendered `children`
/// axis and the ordered `alternate_frames` axis. Alternate roots use a reserved
/// `@alternate[index]` path namespace and an `@alternate/<type>` content-hash
/// namespace. Content-hash descendants under an alternate root carry the same
/// marker without the frame index, so equivalent controls across captured
/// states retain one identity while ordinary children cannot collide. Adapter
/// identities continue to use their source node IDs.
/// Adapters call assign_anchors() in the final stage of their parse path.
/// The `@alternate` namespace is reserved for this walker. Adapter strategies
/// assume the source supplies unique node IDs; this low-level API preserves
/// duplicate authored IDs rather than silently rewriting source identity.

#include <pulp/view/design_import.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::view {

/// Strategy for computing stable_anchor_id values. See header comment.
enum class AnchorStrategy {
    content_hash,
    path,
    adapter
};

/// Walk `root` and populate `stable_anchor_id` on every node according to
/// `strategy`. For the `adapter` strategy, `adapter_name` is the prefix
/// before `:<source_node_id>` (e.g. "figma", "pencil") — required for
/// `adapter`, ignored otherwise.
///
/// For nodes that already have a non-empty `stable_anchor_id` (e.g. set
/// explicitly by an authored override), the existing value is preserved.
///
/// A content hash does not include the parent, so two identical subtrees under
/// different parents (two cards each holding an unlabelled slider) hash alike.
/// After the walk, every anchor this call derived from content (the
/// content-hash strategy, and the adapter strategy's fallback for a node with
/// no source id) that repeats an earlier anchor is disambiguated as
/// resolve_anchor_collisions() describes. Explicit identities (source ids,
/// authored overrides) are never rewritten.
void assign_anchors(IRNode& root,
                    AnchorStrategy strategy,
                    std::string_view adapter_name = "");

/// One derived anchor a collision pass replaced.
struct AnchorDisambiguation {
    std::string path;       ///< `$/children[i]/...` of the node that changed
    std::string previous;   ///< the anchor it shared
    std::string anchor;     ///< its new anchor
    std::string first_path; ///< the earlier node that keeps `previous`
};

/// Makes every derived anchor in `root` unique, in document order over
/// `children`. The first node holding an anchor keeps it, and so does every
/// node whose anchor is already unique, so re-import identity is preserved
/// wherever it already worked. Each later node with a derived anchor gets the
/// hash of its anchor folded with its parent's anchor, then with each further
/// ancestor's until the result is held by no other node in the tree.
///
/// "Derived" means the node's `anchor_strategy` says the anchor was computed
/// from content: `content-hash`, or `adapter` on a node with no
/// `source_node_id`. A repeated explicit identity is left as it is: that is a
/// source defect for the caller to refuse (find_duplicate_anchors()).
std::vector<AnchorDisambiguation> resolve_anchor_collisions(IRNode& root);

/// Compute a single anchor without walking. Public for tests and for
/// adapter code that wants to assign anchors incrementally.
///
/// @param node The node to anchor (read-only).
/// @param parent_anchor For `path` strategy: the parent's already-computed
///                      anchor; otherwise ignored.
/// @param sibling_tag_index_for_path For `path` strategy: how many earlier
///                                   siblings share `node.type` as their tag.
/// @param depth Tree depth (root = 0).
/// @param sig_index_for_content_hash For `content-hash` strategy: how many
///                                   earlier siblings share the same
///                                   {tag, role, text} signature.
/// @param strategy Strategy selector.
/// @param adapter_name For `adapter` strategy only.
std::string compute_anchor_id(const IRNode& node,
                              std::string_view parent_anchor,
                              std::size_t sibling_tag_index_for_path,
                              std::size_t depth,
                              std::size_t sig_index_for_content_hash,
                              AnchorStrategy strategy,
                              std::string_view adapter_name = "");

/// Map a DesignSource to its default anchor strategy. Mirrors the
/// DEFAULT_ANCHOR_STRATEGY map in @pulp/import-ir/src/anchors.ts.
AnchorStrategy default_anchor_strategy(DesignSource source);

}  // namespace pulp::view
