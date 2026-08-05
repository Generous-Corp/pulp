// iOS VoiceOver accessibility provider
// Maps Pulp View accessibility properties to UIAccessibility protocol.
//
// Each View with an AccessRole is exposed as a UIAccessibilityElement.
// The hosting UIView implements UIAccessibilityContainer to provide
// a flat list of accessible elements to VoiceOver.

#include <TargetConditionals.h>
#if TARGET_OS_IOS

#include <pulp/view/view.hpp>
#include <pulp/view/accessibility.hpp>
#import <UIKit/UIKit.h>

#include <vector>

// ── Role mapping ────────────────────────────────────────────────────────
//
// `@interface` / `@implementation` declarations may only appear at file
// scope (clang error: "Objective-C declarations may only appear in
// global scope"). Keep the C++ helpers and the Obj-C bridge class at
// the top level here and re-enter `namespace pulp::view` further down
// for the C++ entry point that the rest of pulp-view calls.

namespace {

// UIKit exposes no role string — a VoiceOver element is described by its
// TRAITS. The trait vocabulary is much narrower than NSAccessibility's role
// list, so several Pulp roles collapse onto Button (checkbox, radio, combo
// box, menu item, tab) — VoiceOver then announces "button" and the element's
// accessibilityValue carries the state. That is the correct UIKit idiom, not a
// gap: UIKit's own UISwitch/UISegmentedControl do the same. Structural roles
// (list / table / row / cell / group / dialog) carry no trait; the element is
// still reachable, it just isn't announced as a control.
//
// No `default:` — adding an AccessRole must be a compile error here, not a
// silent fall through to TraitNone.
UIAccessibilityTraits access_role_to_traits(pulp::view::View::AccessRole role) {
    using AccessRole = pulp::view::View::AccessRole;
    switch (role) {
        case AccessRole::slider:
        case AccessRole::scroll_bar:
            return UIAccessibilityTraitAdjustable;
        case AccessRole::toggle:
        case AccessRole::button:
        case AccessRole::checkbox:
        case AccessRole::radio:
        case AccessRole::combo_box:
        case AccessRole::menu_item:
        case AccessRole::tab:
            return UIAccessibilityTraitButton;
        case AccessRole::link:
            return UIAccessibilityTraitLink;
        case AccessRole::label:
            return UIAccessibilityTraitStaticText;
        case AccessRole::heading:
            return UIAccessibilityTraitHeader;
        case AccessRole::image:
            return UIAccessibilityTraitImage;
        case AccessRole::meter:
        case AccessRole::progress_bar:
            return UIAccessibilityTraitUpdatesFrequently;
        // Text entry: UIKit has no public "text field" trait; VoiceOver derives
        // it from the element responding to UITextInput. Pulp's element is a
        // plain UIAccessibilityElement, so it announces as untyped text-entry.
        // Tracked as a known iOS gap (see the ios skill).
        case AccessRole::text_field:
        case AccessRole::group:
        case AccessRole::list:
        case AccessRole::list_item:
        case AccessRole::table:
        case AccessRole::row:
        case AccessRole::cell:
        case AccessRole::tab_list:
        case AccessRole::menu:
        case AccessRole::dialog:
        case AccessRole::none:
            return UIAccessibilityTraitNone;
    }
    return UIAccessibilityTraitNone;
}

}  // namespace

// ── PulpAccessibilityElement ────────────────────────────────────────────

/// Bridges a Pulp View to UIAccessibility.
@interface PulpAccessibilityElement : UIAccessibilityElement {
@private
    pulp::view::ViewCapture _viewCapture;
    pulp::view::View* _rootView;
    std::weak_ptr<const std::uint64_t> _rootLifetime;
}
- (void)captureView:(pulp::view::View*)view root:(pulp::view::View*)root;
- (pulp::view::View*)liveView;
@end

@implementation PulpAccessibilityElement

- (void)captureView:(pulp::view::View*)view root:(pulp::view::View*)root {
    _viewCapture.set(view);
    _rootView = root;
    _rootLifetime = root ? root->import_binding_lifetime_token()
                         : std::weak_ptr<const std::uint64_t>{};
}

- (pulp::view::View*)liveView {
    if (!_rootView || _rootLifetime.expired()) return nullptr;
    return _viewCapture.live_in(*_rootView);
}

- (NSString *)accessibilityLabel {
    auto* view = [self liveView];
    if (!view) return nil;
    auto label = view->access_label();
    return label.empty() ? nil : [NSString stringWithUTF8String:label.c_str()];
}

- (NSString *)accessibilityValue {
    auto* view = [self liveView];
    if (!view) return nil;
    // Shared resolver (accessibility.hpp): value interface → text interface →
    // access_value slot → check/press state ("checked" / "unchecked" /
    // "mixed"). Reading only the slot left every slider / meter / text field
    // with an empty VoiceOver value; and because UIKit collapses checkbox /
    // toggle onto TraitButton, the state slot is the ONLY thing that makes a
    // checkbox announce more than "button" here (macOS has a native @YES/@NO
    // accessibilityValue for that; UIKit does not).
    const std::string value = pulp::view::accessibility_value_string(*view);
    return value.empty() ? nil : [NSString stringWithUTF8String:value.c_str()];
}

- (UIAccessibilityTraits)accessibilityTraits {
    auto* view = [self liveView];
    if (!view) return UIAccessibilityTraitNone;
    return access_role_to_traits(view->access_role());
}

- (CGRect)accessibilityFrame {
    auto* view = [self liveView];
    if (!view) return CGRectZero;
    auto bounds = view->bounds();
    // Convert view-local bounds to screen coordinates by walking up the hierarchy
    float ox = bounds.x, oy = bounds.y;
    auto* parent = view->parent();
    while (parent) {
        ox += parent->bounds().x;
        oy += parent->bounds().y;
        parent = parent->parent();
    }
    CGRect localRect = CGRectMake(ox, oy, bounds.width, bounds.height);
    // Convert from container view coordinates to screen coordinates
    UIView* container = (UIView*)self.accessibilityContainer;
    if (container) {
        return UIAccessibilityConvertFrameToScreenCoordinates(localRect, container);
    }
    return localRect;
}

- (BOOL)isAccessibilityElement {
    auto* view = [self liveView];
    return view && pulp::view::is_accessibility_element(*view);
}

// ── Adjustable support (sliders, knobs) ─────────────────────────────────

- (void)accessibilityIncrement {
    auto* view = [self liveView];
    if (!view) return;
    view->on_accessibility_adjust(0.05f);  // +5% step
}

- (void)accessibilityDecrement {
    auto* view = [self liveView];
    if (!view) return;
    view->on_accessibility_adjust(-0.05f);  // -5% step
}

@end

namespace pulp::view {

// ── Helper: collect accessible views ────────────────────────────────────

namespace {

void collect_accessible_views(View& root, std::vector<View*>& out) {
    for (size_t i = 0; i < root.child_count(); ++i) {
        auto* child = root.child_at(i);
        if (is_accessibility_element(*child))
            out.push_back(const_cast<View*>(child));
        collect_accessible_views(*const_cast<View*>(child), out);
    }
}

}  // namespace

/// Create accessibility elements for all accessible views in the tree.
/// Called by the hosting UIView to populate its accessibility container.
NSArray<UIAccessibilityElement *>* create_accessibility_elements(
    View& root, UIView* container) {

    std::vector<View*> accessible;
    collect_accessible_views(root, accessible);

    NSMutableArray* elements = [NSMutableArray arrayWithCapacity:accessible.size()];
    for (auto* view : accessible) {
        PulpAccessibilityElement* element =
            [[PulpAccessibilityElement alloc] initWithAccessibilityContainer:container];
        [element captureView:view root:&root];
        [elements addObject:element];
    }
    return elements;
}

}  // namespace pulp::view

#endif  // TARGET_OS_IOS
