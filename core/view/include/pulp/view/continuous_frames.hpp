#pragma once

/// @file continuous_frames.hpp
/// Predicate that decides whether a view tree still needs per-vsync frames.
///
/// A host render loop (native window, plugin-view host, or a foreign-host
/// embed tick) should only invalidate/repaint when something is actually
/// moving. This predicate walks the tree and reports whether any node is
/// mid-animation — an opted-in continuous-repaint view, a widget with a
/// running hover/thumb/scroll animation or a time-driven shader, or a live
/// CSS animation. When it returns false and the FrameClock has no active
/// subscribers, the loop can idle at 0 fps until the next input or state
/// change, instead of compositing a static surface every tick.
///
/// The macOS window and plugin-view hosts' display-link repaint gate uses this
/// predicate (together with `FrameClock::has_active_subscribers()`); it is
/// public so a foreign-host embed layer can gate its own tick the same way
/// rather than repainting unconditionally.
///
/// Thread affinity: call on the view-owning UI thread. The tree is walked
/// without synchronization, so it must not run concurrently with mutation of
/// the view hierarchy or its animation state.

namespace pulp::view {

class View;

/// True if `view` (or any descendant) needs continuous per-frame repaints.
/// Null-safe: a null pointer returns false. Read-only; does not advance any
/// animation. Pair with `FrameClock::has_active_subscribers()` for the full
/// "keep the render loop alive" decision.
bool needs_continuous_frames(const View* view);

} // namespace pulp::view
