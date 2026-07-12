// view_fwd.hpp — slim forward declarations for the pulp::view core types.
//
// Header-diet alternative to a full PIMPL refactor: PIMPL would break SDK
// ABI for public derived classes, so files that only need pointers or
// references should include this lighter declaration surface instead of
// <pulp/view/view.hpp>.
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
class HostFramePump;

}  // namespace pulp::view
