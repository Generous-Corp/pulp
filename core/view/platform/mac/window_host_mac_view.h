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
/// Declare (YES) or withdraw (NO) a per-frame flush driver.
///
/// This is not a preference, it is a commitment: while it is YES the view
/// HOLDS drag motion and only `-flushPointerInputForFrame` releases it, so a
/// host that stops ticking without withdrawing strands the drag entirely.
/// Withdrawing flushes whatever is held, so the pair is safe to call around a
/// link that starts and stops with window attachment.
///
/// Call it from EVERY path that establishes a frame driver, not from one
/// chosen start function — a host with a display link and a timer fallback has
/// two such paths, and wiring only one is how this silently regressed the
/// first time (see host_pointer_input.hpp). Default NO, which is
/// dispatch-every-sample: the CPU window host has no frame driver and stays
/// there permanently.
- (void)setPointerFlushDriverActive:(BOOL)active;

/// Release drag motion held since the last presented frame. Call once per
/// frame tick, from the frame driver only.
///
/// Call it BEFORE the frame's render decision, not after: a frame that decides
/// not to paint must still release input, or latency grows without bound while
/// the UI is visually idle. Safe when nothing is held. This entry point also
/// reports a frame driver that never declared itself, so ordinary event paths
/// must use `-flushPointerInputNow` instead.
- (void)flushPointerInputForFrame;

/// Release held motion from an event path that must not defer — a terminal
/// event, or a handoff about to make the current capture unreachable.
- (void)flushPointerInputNow;
@end

#endif
