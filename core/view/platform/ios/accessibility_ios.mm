// iOS VoiceOver accessibility provider
// Maps Pulp View accessibility properties to UIAccessibility.

#include <TargetConditionals.h>
#if TARGET_OS_IOS

#include <pulp/view/view.hpp>
#include <pulp/view/accessibility.hpp>
#import <UIKit/UIKit.h>

namespace pulp::view {

static UIAccessibilityTraits access_role_to_traits(View::AccessRole role) {
    switch (role) {
        case View::AccessRole::slider: return UIAccessibilityTraitAdjustable;
        case View::AccessRole::toggle: return UIAccessibilityTraitButton;
        case View::AccessRole::label:  return UIAccessibilityTraitStaticText;
        case View::AccessRole::meter:  return UIAccessibilityTraitUpdatesFrequently;
        case View::AccessRole::image:  return UIAccessibilityTraitImage;
        default: return UIAccessibilityTraitNone;
    }
}

}  // namespace pulp::view

/// Bridges a Pulp View to UIAccessibility with proper screen-space frame
/// and adjustable increment/decrement for slider roles.
@interface PulpAccessibilityElement : UIAccessibilityElement
@property (nonatomic, assign) pulp::view::View* pulpView;
@property (nonatomic, weak) UIView* hostView;
@end

@implementation PulpAccessibilityElement

- (NSString *)accessibilityLabel {
    if (!_pulpView) return nil;
    auto label = _pulpView->access_label();
    return label.empty() ? nil : [NSString stringWithUTF8String:label.c_str()];
}

- (NSString *)accessibilityValue {
    if (!_pulpView) return nil;
    auto value = _pulpView->access_value();
    return value.empty() ? nil : [NSString stringWithUTF8String:value.c_str()];
}

- (UIAccessibilityTraits)accessibilityTraits {
    if (!_pulpView) return UIAccessibilityTraitNone;
    return pulp::view::access_role_to_traits(_pulpView->access_role());
}

- (CGRect)accessibilityFrame {
    if (!_pulpView || !_hostView) return CGRectZero;

    // Convert view-local bounds to screen coordinates
    auto bounds = _pulpView->bounds();
    CGRect localRect = CGRectMake(bounds.x, bounds.y, bounds.width, bounds.height);

    // Walk up the Pulp view hierarchy to accumulate parent offsets
    auto* current = _pulpView->parent();
    while (current) {
        auto pb = current->bounds();
        localRect.origin.x += pb.x;
        localRect.origin.y += pb.y;
        current = current->parent();
    }

    // Convert from host UIView coordinates to screen coordinates
    CGRect screenRect = [_hostView convertRect:localRect toCoordinateSpace:_hostView.window.screen.coordinateSpace];
    return screenRect;
}

- (BOOL)isAccessibilityElement {
    return _pulpView && _pulpView->access_role() != pulp::view::View::AccessRole::none;
}

// ── Adjustable support (sliders, knobs) ─────────────────────────────────

- (void)accessibilityIncrement {
    if (!_pulpView) return;
    // Simulate an increment: post a synthetic drag-up event
    // The widget's on_mouse_drag handler will process the value change
    pulp::view::Point p = {0, -5};  // Up = increase for vertical sliders
    _pulpView->on_mouse_drag(p);
}

- (void)accessibilityDecrement {
    if (!_pulpView) return;
    pulp::view::Point p = {0, 5};   // Down = decrease
    _pulpView->on_mouse_drag(p);
}

@end

namespace pulp::view {

/// Create accessibility elements for all accessible views in the tree.
/// The hosting UIView should call this and implement UIAccessibilityContainer.
NSArray<UIAccessibilityElement *>* create_accessibility_elements(
    View& root, UIView* hostView) {

    NSMutableArray* elements = [NSMutableArray array];

    std::function<void(View&)> collect = [&](View& view) {
        if (view.access_role() != View::AccessRole::none) {
            PulpAccessibilityElement* element =
                [[PulpAccessibilityElement alloc] initWithAccessibilityContainer:hostView];
            element.pulpView = &view;
            element.hostView = hostView;
            [elements addObject:element];
        }
        for (size_t i = 0; i < view.child_count(); ++i)
            collect(*const_cast<View*>(view.child_at(i)));
    };

    collect(root);
    return elements;
}

}  // namespace pulp::view

#endif  // TARGET_OS_IOS
