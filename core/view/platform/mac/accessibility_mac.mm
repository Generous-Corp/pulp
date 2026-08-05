// macOS VoiceOver accessibility provider
// Maps Pulp View accessibility properties to NSAccessibility protocol.
//
// Each View with an AccessRole is exposed as an NSAccessibilityElement.
// The PulpView (NSView) implements NSAccessibility to provide a tree of
// accessible elements to VoiceOver.

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include <pulp/view/view.hpp>
#include <pulp/view/accessibility.hpp>
#include <pulp/view/platform/ns_role_mapping.hpp>
#include <pulp/runtime/log.hpp>
#import <Cocoa/Cocoa.h>

// Per-binary-unique ObjC class names (renames PulpWindowAccessibilityElement
// when a shipped binary defines PULP_VIEW_OBJC_SUFFIX). Must precede the first
// reference to the class.
#include "pulp_mac_objc_names.h"
#include "accessibility_mac_host_lifetime.hpp"

namespace pulp::view {

// Map Pulp AccessRole to NSAccessibilityRole. The table lives in
// platform/ns_role_mapping.hpp so the standalone window host and the plug-in
// editor host (plugin_view_host_mac.mm) cannot drift apart.
static NSAccessibilityRole access_role_to_ns(View::AccessRole role) {
    return ns_role_for_access_role(role);
}

// Collect accessible children. is_accessibility_element() is the shared gate
// (view.hpp): a role alone is not enough — a role that has no content beyond
// its name (button, link, tab, label ...) stays OUT of the tree until it has an
// accessible name, or VoiceOver announces a nameless "button".
static void collect_accessible(View& root, std::vector<View*>& out) {
    for (size_t i = 0; i < root.child_count(); ++i) {
        auto* child = root.child_at(i);
        if (is_accessibility_element(*child))
            out.push_back(const_cast<View*>(child));
        collect_accessible(*const_cast<View*>(child), out);
    }
}

} // namespace pulp::view

// ── NSAccessibilityElement wrapper for each accessible View ─────────────────
//
// This is the standalone window host's accessibility element. The plug-in
// editor host (plugin_view_host_mac.mm) defines its own PulpAccessibilityElement
// with a different shape; the two intentionally carry distinct class names so a
// binary that links both never registers the same ObjC class twice.

@interface PulpWindowAccessibilityElement : NSAccessibilityElement {
@private
    pulp::view::ViewCapture _viewCapture;
    pulp::view::View* _rootView;
    std::weak_ptr<const std::uint64_t> _rootLifetime;
    std::weak_ptr<const std::uint8_t> _hostLifetime;
    NSView* _hostView;  // non-owning; guarded by _hostLifetime
}
- (void)captureView:(pulp::view::View*)view
               root:(pulp::view::View*)root
               host:(NSView*)host;
- (pulp::view::View*)liveView;
@end

@implementation PulpWindowAccessibilityElement

- (void)captureView:(pulp::view::View*)view
               root:(pulp::view::View*)root
               host:(NSView*)host {
    _viewCapture.set(view);
    _rootView = root;
    _rootLifetime = root ? root->import_binding_lifetime_token()
                         : std::weak_ptr<const std::uint64_t>{};
    _hostLifetime = pulp::view::capture_accessibility_host_lifetime(host);
    _hostView = host;
}

- (pulp::view::View*)liveView {
    // AppKit may retain an accessibility element across a scripted realm
    // replacement. The lifetime token protects the root pointer; live_in()
    // then rejects a removed child and allocator-address reuse without ever
    // dereferencing the cached child pointer.
    if (!_rootView || _rootLifetime.expired() || _hostLifetime.expired())
        return nullptr;
    return _viewCapture.live_in(*_rootView);
}

- (NSAccessibilityRole)accessibilityRole {
    auto* view = [self liveView];
    if (!view) return NSAccessibilityUnknownRole;
    return pulp::view::access_role_to_ns(view->access_role());
}

- (NSString*)accessibilityLabel {
    auto* view = [self liveView];
    if (!view || view->access_label().empty()) return nil;
    return [NSString stringWithUTF8String:view->access_label().c_str()];
}

- (id)accessibilityValue {
    // pulp #1737 — surface ARIA state attributes (aria-pressed,
    // aria-checked) through accessibilityValue. NSAccessibility's
    // toggle/checkbox VoiceOver output reads accessibilityValue and
    // expects @(YES) / @(NO) / @"mixed" / nil. Tri-state mapping per
    // ARIA 1.2:
    //   "true"  → @YES (announced as "checked" / "on" / "pressed")
    //   "false" → @NO  (announced as "unchecked" / "off" / "not pressed")
    //   "mixed" → @"mixed" (announced as "mixed")
    //   unset / other → fall through to legacy access_value()
    // aria-checked takes priority over aria-pressed when both set
    // because checkbox/radio states are more semantic-load-bearing
    // than toggle-button pressed state.
    auto* view = [self liveView];
    if (view) {
        const std::string& checked = view->access_checked();
        if (!checked.empty()) {
            if (checked == "true")  return @YES;
            if (checked == "false") return @NO;
            if (checked == "mixed") return @"mixed";
        }
        const std::string& pressed = view->access_pressed();
        if (!pressed.empty()) {
            if (pressed == "true")  return @YES;
            if (pressed == "false") return @NO;
            if (pressed == "mixed") return @"mixed";
        }
    }
    // Resolve through the shared value resolver: a value interface (slider,
    // meter, progress bar), else a text interface (TextEditor's content), else
    // the manually-set access_value slot. Reading only the last one is why a
    // TextEditor holding "hello" returned nil.
    if (!view) return nil;
    const std::string value = pulp::view::accessibility_value_string(*view);
    if (value.empty()) return nil;
    return [NSString stringWithUTF8String:value.c_str()];
}

// pulp #1737 — surface aria-disabled. NSAccessibility's
// isAccessibilityEnabled returns YES by default; we flip to NO when
// aria-disabled is "true". `false` and unset both leave the view
// enabled. Note: this does NOT change actual interaction handling —
// View::set_enabled() is the C++-side gate for that. This only affects
// what VoiceOver announces.
- (BOOL)isAccessibilityEnabled {
    auto* view = [self liveView];
    if (!view) return YES;
    return view->access_disabled() == "true" ? NO : YES;
}

// pulp #1737 — surface aria-hidden. NSAccessibility's accessibility
// element flag should return NO when aria-hidden="true" so VoiceOver
// skips the element. The legacy isAccessibilityElement check
// (role != AccessRole::none) still applies — aria-hidden is an
// additional gate.
- (BOOL)isAccessibilityFocused {
    auto* view = [self liveView];
    return view ? view->has_focus() : NO;
}

- (NSRect)accessibilityFrame {
    auto* view = [self liveView];
    NSView* host = _hostView;
    if (!view || !host) return NSZeroRect;

    // Compute root-relative position
    float rx = 0, ry = 0;
    auto* v = view;
    while (v) {
        rx += v->bounds().x;
        ry += v->bounds().y;
        v = v->parent();
    }
    auto b = view->bounds();

    // Convert from top-down view coordinates to screen coordinates
    NSRect viewRect = NSMakeRect(rx, NSHeight(host.bounds) - ry - b.height, b.width, b.height);
    NSRect windowRect = [host convertRect:viewRect toView:nil];
    NSRect screenRect = [host.window convertRectToScreen:windowRect];
    return screenRect;
}

- (BOOL)isAccessibilityElement {
    auto* view = [self liveView];
    if (!view) return NO;
    // pulp #1737 — aria-hidden="true" suppresses the element regardless
    // of role. Other aria-hidden values (false, unset) keep the legacy
    // role-based gate.
    if (view->access_hidden() == "true") return NO;
    return pulp::view::is_accessibility_element(*view);
}

@end

// ── Public API ──────────────────────────────────────────────────────────────

namespace pulp::view {

// Build accessibility elements for an NSView hosting a Pulp view tree.
// Call this after layout changes to refresh the accessibility tree.
// Returns an array of PulpWindowAccessibilityElement* suitable for
// -accessibilityChildren.
NSArray* build_accessibility_elements(View& root, NSView* host) {
    std::vector<View*> accessible;
    collect_accessible(root, accessible);

    NSMutableArray* elements = [NSMutableArray arrayWithCapacity:accessible.size()];
    for (auto* v : accessible) {
        PulpWindowAccessibilityElement* el = [[PulpWindowAccessibilityElement alloc] init];
        [el captureView:v root:&root host:host];
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
