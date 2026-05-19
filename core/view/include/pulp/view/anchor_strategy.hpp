#pragma once

/// @file anchor_strategy.hpp
/// Stable-anchor-id assignment for design-import IR nodes. Mirrors the
/// TS-side @pulp/import-ir/src/anchors.ts strategy so the C++ and TS
/// import pipelines produce compatible anchors and the tweaks layer
/// (pulp-tweaks.json) works for either path.
///
/// Phase 0a of the inspector direct-manipulation roadmap
/// (planning/2026-05-18-inspector-direct-manipulation-roadmap.md).
///
/// Three strategies, per the umbrella spec (issue #1307, sub-issue #1299):
///   • content-hash — used by stitch / v0 / claude / generic HTML.
///                    Hash over (tag, role, normalized text, depth, sigIndex).
///   • path         — used by RN file exports / hand-edited code.
///                    "Tag[idx]/Tag[idx]/..." from root.
///   • adapter      — used by Figma / Pencil / Mitosis (sources with
///                    native IDs). "<adapter>:<source_node_id>".
///
/// The walker is purely traversal-shaped — it mutates IRNode::stable_anchor_id
/// but otherwise leaves the tree alone. Adapters call assign_anchors() in
/// the final stage of their parse path.

#include <pulp/view/design_import.hpp>
#include <string>
#include <string_view>

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
void assign_anchors(IRNode& root,
                    AnchorStrategy strategy,
                    std::string_view adapter_name = "");

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
