// macOS VoiceOver accessibility provider
// Maps Pulp View accessibility properties to NSAccessibility protocol.
//
// Each View with an AccessRole is exposed as an NSAccessibilityElement.
// The PulpView (NSView) implements NSAccessibility to provide a tree of
// accessible elements to VoiceOver.

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include <pulp/view/view.hpp>
#include <pulp/runtime/log.hpp>
#import <Cocoa/Cocoa.h>

namespace pulp::view {

// Map Pulp AccessRole to NSAccessibilityRole
static NSAccessibilityRole access_role_to_ns(View::AccessRole role) {
    switch (role) {
        case View::AccessRole::slider: return NSAccessibilitySliderRole;
        case View::AccessRole::toggle: return NSAccessibilityCheckBoxRole;
        case View::AccessRole::label:  return NSAccessibilityStaticTextRole;
        case View::AccessRole::group:  return NSAccessibilityGroupRole;
        case View::AccessRole::meter:  return NSAccessibilityProgressIndicatorRole;
        case View::AccessRole::image:  return NSAccessibilityImageRole;
        default: return NSAccessibilityUnknownRole;
    }
}

// Collect accessible children (views with a non-none AccessRole)
static void collect_accessible(View& root, std::vector<View*>& out) {
    for (size_t i = 0; i < root.child_count(); ++i) {
        auto* child = root.child_at(i);
        if (child->access_role() != View::AccessRole::none)
            out.push_back(const_cast<View*>(child));
        collect_accessible(*const_cast<View*>(child), out);
    }
}

} // namespace pulp::view

// ── NSAccessibilityElement wrapper for each accessible View ─────────────────

@interface PulpAccessibilityElement : NSAccessibilityElement
@property (nonatomic, assign) pulp::view::View* view;
@property (nonatomic, unsafe_unretained) NSView* hostView;
@end

@implementation PulpAccessibilityElement

- (NSAccessibilityRole)accessibilityRole {
    if (!_view) return NSAccessibilityUnknownRole;
    return pulp::view::access_role_to_ns(_view->access_role());
}

- (NSString*)accessibilityLabel {
    if (!_view || _view->access_label().empty()) return nil;
    return [NSString stringWithUTF8String:_view->access_label().c_str()];
}

- (id)accessibilityValue {
    if (!_view || _view->access_value().empty()) return nil;
    return [NSString stringWithUTF8String:_view->access_value().c_str()];
}

- (BOOL)isAccessibilityFocused {
    return _view ? _view->has_focus() : NO;
}

- (NSRect)accessibilityFrame {
    if (!_view || !_hostView) return NSZeroRect;

    // Compute root-relative position
    float rx = 0, ry = 0;
    auto* v = _view;
    while (v) {
        rx += v->bounds().x;
        ry += v->bounds().y;
        v = v->parent();
    }
    auto b = _view->bounds();

    // Convert from top-down view coordinates to screen coordinates
    NSRect viewRect = NSMakeRect(rx, NSHeight(_hostView.bounds) - ry - b.height, b.width, b.height);
    NSRect windowRect = [_hostView convertRect:viewRect toView:nil];
    NSRect screenRect = [_hostView.window convertRectToScreen:windowRect];
    return screenRect;
}

- (BOOL)isAccessibilityElement {
    return _view && _view->access_role() != pulp::view::View::AccessRole::none;
}

@end

// ── Public API ──────────────────────────────────────────────────────────────

namespace pulp::view {

// Build accessibility elements for an NSView hosting a Pulp view tree.
// Call this after layout changes to refresh the accessibility tree.
// Returns an array of PulpAccessibilityElement* suitable for -accessibilityChildren.
NSArray* build_accessibility_elements(View& root, NSView* host) {
    std::vector<View*> accessible;
    collect_accessible(root, accessible);

    NSMutableArray* elements = [NSMutableArray arrayWithCapacity:accessible.size()];
    for (auto* v : accessible) {
        PulpAccessibilityElement* el = [[PulpAccessibilityElement alloc] init];
        el.view = v;
        el.hostView = host;
        [elements addObject:el];
    }
    return elements;
}

void init_mac_accessibility(View& root) {
    runtime::log_info("macOS Accessibility: VoiceOver support initialized ({} accessible views)",
        [&]{ std::vector<View*> a; collect_accessible(root, a); return a.size(); }());
}

} // namespace pulp::view

#endif // TARGET_OS_OSX
