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
// Measured-dt source for the animation timer. Owned by the host; set alongside
// frameClock. Without it the timer would have to invent a dt (it used to
// hardcode 1/60), which desynchronises every animation from wall time.
@property (nonatomic, assign) pulp::view::HostFramePump* framePump;
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
/// Opt IN to per-frame drag coalescing. Default NO, and that default is
/// deliberate: a host that does not drive `flushCoalescedPointerInput` every
/// frame would strand held motion forever, so holding is enabled only by a
/// host that has committed to flushing. The GPU host sets this when it starts
/// its display link; the CPU host (no display link) leaves it NO and keeps
/// dispatching each sample immediately, exactly as before.
@property (nonatomic, assign) BOOL coalescePointerInput;

/// Deliver drag motion held since the last presented frame.
///
/// `mouseDragged:` holds motion rather than dispatching it, so the host MUST
/// call this once per presented frame — otherwise held input is never
/// delivered. Call it before the frame's render decision, not after: a frame
/// that decides not to paint must still release input, or latency grows
/// without bound while the UI is visually idle. Safe to call when nothing is
/// held.
- (void)flushCoalescedPointerInput;
@end

#endif
