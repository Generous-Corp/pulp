// view_fwd.hpp — slim forward declarations for the pulp::view core types.
//
// Created in the 2026-05 Phase 4 R2-2 header-diet slice (Codex-consulted
// alternative to a full PIMPL refactor; see
// planning/2026-05-17-refactor-roadmap-year.md for the rationale —
// PIMPL would break SDK ABI for 64 derived classes, so we take the
// safer header-diet path).
//
// Use this header when a file only needs to:
//
//   - hold a pointer or reference to a View / WindowHost /
//     PluginViewHost / FrameClock
//   - take one of those by pointer/reference parameter or return type
//   - declare a `std::function<void(View&)>` or similar signature type
//
// In any of those cases, including this slim header is strictly
// cheaper than `<pulp/view/view.hpp>`, which transitively pulls in
// `<pulp/canvas/canvas.hpp>` (~1,500 lines), css_animation.hpp,
// theme.hpp, geometry.hpp, and input_events.hpp.
//
// Include `<pulp/view/view.hpp>` only when you need to:
//
//   - call methods on a View (deref the pointer)
//   - subclass View
//   - construct or destroy a View locally (calls inline destructor)
//   - access any of the nested enums (Position, Overflow, BlendMode
//     proxy, etc.)
//
// The forward-declaration set is conservative on purpose — only the
// load-bearing public types appear here, not internal helpers /
// FlexStyle / OverlayRequest / FilterOp, which would couple this
// header back to view.hpp's evolution.

#pragma once

namespace pulp::view {

class View;
class WindowHost;
class PluginViewHost;
class FrameClock;

}  // namespace pulp::view
