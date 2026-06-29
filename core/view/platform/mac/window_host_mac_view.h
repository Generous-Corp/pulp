#pragma once

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include <pulp/view/geometry.hpp>
#include <pulp/view/view_fwd.hpp>

#import <Cocoa/Cocoa.h>

// Per-binary-unique ObjC class names (renames PulpView and friends when a
// shipped binary defines PULP_VIEW_OBJC_SUFFIX). Must precede the @interface.
#include "pulp_mac_objc_names.h"

// Shared private interface for the macOS host's NSView implementation.
// Method bodies stay split across focused .mm files to keep the main host
// implementation below the hotspot-size ceiling.
@interface PulpView : NSView
@property (nonatomic, assign) pulp::view::View* rootView;
@property (nonatomic, assign) pulp::view::FrameClock* frameClock;
@property (nonatomic, strong) NSTimer* animationTimer;
@property (nonatomic, strong) NSTrackingArea* trackingArea;
// Inverse design-viewport transform applied to every window-space input
// point before hit_test. Set by WindowHost::set_design_viewport; nil
// when no design viewport is in effect (identity).
@property (nonatomic, copy) pulp::view::Point (^pointTransform)(pulp::view::Point);
// pulp #2502 — the host destructor MUST call this before the View tree /
// WidgetBridge / ScriptEngine the deferred-click blocks were built from can
// be freed. It invalidates every still-queued `mouseUp:` deferred-click block
// so none of them can run a `std::function` (`on_click` / `on_global_click`)
// whose closure references freed bridge/engine state.
- (void)prepareForTeardown;
- (void)setRelativeMouseMode:(BOOL)enabled;
@end

#endif
