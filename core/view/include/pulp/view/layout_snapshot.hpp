#pragma once

#include <pulp/view/view.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace pulp::view {

struct LayoutTreeSnapshotOptions {
    std::string surface = "view";
    std::string fixture;
    float viewport_width = 0.0f;
    float viewport_height = 0.0f;
};

struct LayoutTreeTolerance {
    float numeric_bounds_px = 0.0f;
    float text_box_px = 0.5f;
};

struct LayoutTreeDiff {
    std::vector<std::string> messages;

    bool empty() const { return messages.empty(); }
};

/// Dump the current, already-laid-out View tree as deterministic JSON.
///
/// Callers own layout timing: set root bounds and call layout_children() before
/// dumping when they need post-Yoga coordinates. The snapshot is semantic rather
/// than pixel-based, so it is portable across CI platforms.
std::string dump_layout_tree(
    const View& root,
    const LayoutTreeSnapshotOptions& options = {});

/// Compare two dump_layout_tree() JSON strings with the Phase 2 oracle rules:
/// node kind/id/visibility/z-order/overflow must match exactly, numeric layout
/// bounds must match exactly by default, and text-box dimensions allow a small
/// tolerance for text measurement. Golden-file callers that compare snapshots
/// produced on different platforms should pass an explicit numeric bounds
/// tolerance instead of relying on the in-process exact-match default.
bool layout_tree_snapshots_equivalent(
    std::string_view actual_json,
    std::string_view expected_json,
    const LayoutTreeTolerance& tolerance = {},
    LayoutTreeDiff* diff = nullptr);

} // namespace pulp::view
