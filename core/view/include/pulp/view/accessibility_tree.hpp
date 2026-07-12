#pragma once

// Accessibility tree snapshot.
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

    // True when the platform bridges put this View in the accessibility
    // tree — pulp::view::is_accessibility_element(view), the same gate the
    // macOS / iOS / UIA / AT-SPI / TalkBack collectors call. A role alone is
    // not enough: a name-only role (button, link, tab, label ...) with an
    // empty label is excluded rather than announced as a nameless control.
    bool exposed = false;

    // Numeric range information when the view exposes
    // AccessibilityValueInterface. Present fields are filled; otherwise
    // has_value is false and callers should ignore min/max/current.
    bool has_value = false;
    double min_value = 0.0;
    double max_value = 0.0;
    double current_value = 0.0;
    std::string value_string;

    // ARIA state attributes. Tri-state per ARIA 1.2:
    // `true` / `false` / `mixed` / unset (empty string). Platform AT
    // bridges read these to expose the toggle/checkbox state.
    // Stored on View::access_pressed_/_checked_/_disabled_/_hidden_;
    // surfaced through this snapshot for the cross-platform tree path.
    std::string pressed;   // aria-pressed
    std::string checked;   // aria-checked
    std::string disabled;  // aria-disabled
    std::string hidden;    // aria-hidden

    // Nesting depth under the root; the root itself is depth 0.
    int depth = 0;
};

/// Depth-first snapshot of `root` and every descendant. Views with
/// AccessRole::none are still included — callers can filter if they
/// want only announceable nodes. Tests commonly assert on (role, label)
/// pairs; the `value` fields support slider / progress checks.
std::vector<AccessibilityNodeSnapshot> snapshot_accessibility_tree(const View& root);

/// Count nodes the platform bridges actually expose (snapshot node `exposed`
/// — role != none AND named when the role requires a name). Handy when a test
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
