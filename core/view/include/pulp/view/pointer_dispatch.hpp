#pragma once

// Platform-agnostic pointer routing shared by the window hosts.
//
// The coordinate walk and the context-menu lookup used to live inside the macOS
// hosts, which meant they could only be exercised by running a real NSView. They
// are pure view-tree logic, so they live here and each platform host calls in.

#include <pulp/view/view.hpp>

namespace pulp::view {

/// Convert `root_pos` (root-local, i.e. window space) into `target`'s
/// local-pre-scale coordinates, peeling off each ancestor's bounds offset,
/// `set_scale` transform, and ScrollView scroll offset along the way.
/// `target` must be `root` or a descendant of it.
Point point_to_local(Point root_pos, View* target, View* root);

/// Route a right-click at `root_pos` to the view under it. Returns true when a
/// view had an `on_context_menu` handler and it was invoked.
///
/// Only the hit view is consulted — the callback does not bubble to ancestors.
bool dispatch_context_menu(View& root, Point root_pos);

}  // namespace pulp::view
