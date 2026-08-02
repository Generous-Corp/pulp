#pragma once

#include <cstdint>
#include <memory>

#ifdef __OBJC__
@class NSView;

namespace pulp::view {

// A token owned by the native NSView through an associated Objective-C object.
// Accessibility elements keep only the weak half, so they can reject AppKit
// callbacks after the host view is destroyed without messaging a stale pointer.
std::weak_ptr<const std::uint8_t>
capture_accessibility_host_lifetime(NSView* host);

} // namespace pulp::view
#endif
