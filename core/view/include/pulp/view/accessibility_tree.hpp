#pragma once

// Accessibility tree snapshot (workstream 04 slice 4.4).
//
// Walks a pulp::view::View subtree and produces a flat inventory of
// (role, label, value, actions) tuples. Used by cross-platform a11y
// tests to verify that view metadata is set correctly without needing
// a real screen reader attached. Platform bridges (macOS / iOS / Android
// / Windows UIA / Linux AT-SPI) serve the same data to their respective
// accessibility clients, so an offline mismatch here will surface as a
// bridge regression later.

#include <pulp/view/view.hpp>

#include <string>
#include <vector>

namespace pulp::view {

struct AccessibilityNodeSnapshot {
    const View* view = nullptr;
    View::AccessRole role = View::AccessRole::none;
    std::string label;
    std::string value;

    // Numeric range information when the view exposes
    // AccessibilityValueInterface. Present fields are filled; otherwise
    // has_value is false and callers should ignore min/max/current.
    bool has_value = false;
    double min_value = 0.0;
    double max_value = 0.0;
    double current_value = 0.0;
    std::string value_string;

    // Nesting depth under the root; the root itself is depth 0.
    int depth = 0;
};

/// Depth-first snapshot of `root` and every descendant. Views with
/// AccessRole::none are still included — callers can filter if they
/// want only announceable nodes. Tests commonly assert on (role, label)
/// pairs; the `value` fields support slider / progress checks.
std::vector<AccessibilityNodeSnapshot> snapshot_accessibility_tree(const View& root);

/// Count nodes in the tree that carry a non-none role. Handy when a test
/// only cares that *some* number of controls are exposed (e.g. preset-
/// browser row widgets).
std::size_t count_announceable(const View& root);

/// Find the first descendant whose access_label exactly matches `label`
/// and whose role equals `role`. Returns nullptr if no match. Useful for
/// test assertions that target a specific widget.
const View* find_by_role_and_label(const View& root,
                                   View::AccessRole role,
                                   std::string_view label);

}  // namespace pulp::view
