#pragma once

#include <TargetConditionals.h>
#if TARGET_OS_OSX

#include <pulp/view/geometry.hpp>
#include <pulp/view/view_fwd.hpp>

#import <Cocoa/Cocoa.h>
#ifdef PULP_HAS_SKIA
#import <QuartzCore/CAMetalLayer.h>
#endif

#include <cstdint>

// Per-binary-unique ObjC class names (renames PulpPluginView / PulpGpuPluginView
// when a shipped binary defines PULP_VIEW_OBJC_SUFFIX). Must precede the
// @interface.
#include "pulp_mac_objc_names.h"

// Shared private interfaces for the DAW-embedded plug-in NSViews, the plug-in
// counterpart of window_host_mac_view.h. plugin_view_host_mac.mm implements
// them; the text-input and drag-drop categories in their own .mm files extend
// them. Each class is declared exactly once: the per-binary ObjC unity file
// compiles all of those .mm files as ONE translation unit, where a second
// @interface for the same class is an error, so no .mm may redeclare a slice.

// ── PulpPluginView: NSView subclass for DAW embedding ────────────────────────

@interface PulpPluginView : NSView
@property(nonatomic, assign) pulp::view::View* rootView;
@property(nonatomic, copy) void (^onResize)(uint32_t, uint32_t);
// Fired from -viewDidMoveToWindow so the host can start/stop its CPU frame
// driver only while the view actually lives in a window.
@property(nonatomic, copy) void (^onWindowChange)(void);
// Fired from -setNeedsDisplay: (flag=YES) — the ONE funnel every "this editor
// is dirty" signal already passes through: the event handlers below, the host's
// repaint() override (and therefore View::request_repaint), resize, and AppKit
// itself. The host mirrors it into a link-thread-readable atomic so its
// CVDisplayLink callback can gate a vsync WITHOUT hopping to the main thread
// (see should_dispatch_host_frame). Without a dirty signal the gate could never
// re-open: a static editor would idle forever and never see the hover that woke
// it. Nil-ed in the host destructor before `this` is freed.
@property(nonatomic, copy) void (^onNeedsDisplay)(void);
// Inverse-design-viewport transform applied to every host-space input point
// before hit_test, mirroring the standalone PulpView. nil = identity. Set
// by MacPluginViewHost::set_design_viewport.
@property(nonatomic, copy) pulp::view::Point (^pointTransform)(pulp::view::Point);
// Design viewport size. (0, 0) = identity (paint at host bounds, no
// scale/letterbox). When set, drawRect pins root to (designW, designH),
// fills letterbox bars at host bounds, then translate+scale before
// paint_all so the rendered surface matches the standalone host.
@property(nonatomic, assign) float designW;
@property(nonatomic, assign) float designH;
@property(nonatomic, assign) BOOL designTopAlign;
// Reconcile first-responder with the pulp text-input focus slot. Declared here
// so the host's frame-tick block (below the @implementation) can call it every
// vsync — the event-independent cadence that hands the DAW keyboard back the
// instant focus clears without a following key/mouse event.
- (void)syncKeyFocus;
// Re-resolve the cursor at the last known pointer position and publish it only
// when it changed. Declared here so the host's frame-tick block (below the
// @implementation) can call it every vsync: AppKit re-asks which cursor to show
// when the pointer MOVES and never because content moved under a still pointer,
// so this is what makes a hovered region change update the cursor immediately.
- (void)refreshHoverCursor;

/// Opt IN to per-presented-frame drag coalescing, and out again.
///
/// Default NO, and that default is load bearing: a host that does not drive
/// -flushCoalescedPointerInput every frame would strand held motion forever,
/// so holding is enabled only by a host that has committed to flushing. Both
/// plug-in hosts run a CVDisplayLink, so both opt in while their link runs.
/// Turning it off flushes.
- (void)setCoalescePointerInput:(BOOL)enabled;

/// Deliver drag motion held since the last presented frame. The host MUST call
/// this once per presented frame, BEFORE the frame's render decision: a frame
/// that decides not to paint must still release input, or latency grows without
/// bound while the editor is visually idle. Safe when nothing is held.
- (void)flushCoalescedPointerInput;

/// Drop held motion WITHOUT delivering it. For a host tearing down mid-gesture,
/// where the drag target may already be gone.
- (void)discardCoalescedPointerInput;

/// Whether the opt-in above is currently set. A host's frame path asserts this
/// out loud: the fail-safe makes a silently-lost opt-in look correct and merely
/// slow, which is the hardest failure of this mechanism to notice.
- (BOOL)coalescingPointerInput;
@end

#ifdef PULP_HAS_SKIA
// CAMetalLayer-backed NSView for DAW-embedded GPU rendering.
//
// Unlike the standalone window host (which owns its NSWindow and starts the
// CVDisplayLink in its constructor), an embedded plugin view is handed a
// parent view by the host and only becomes live once it joins a window. So
// this view exposes window-attach / backing-change callbacks the host wires
// up to start/stop the display link and reconfigure surfaces at the right
// moments. The wrapper paths (AU returns the NSView directly; VST3/CLAP call
// attach_to_parent) never drive `attach_to_parent`-time rendering — they all
// funnel through `-viewDidMoveToWindow`.
@interface PulpGpuPluginView : NSView
@property(nonatomic, readonly) CAMetalLayer* metalLayer;
@property(nonatomic, assign) pulp::view::View* rootView;
@property(nonatomic, copy) void (^onWindowChange)(void);
@property(nonatomic, copy) void (^onBackingChange)(void);
@property(nonatomic, copy) void (^onResize)(uint32_t, uint32_t);
// Inverse-design-viewport transform applied to every host-space input point
// before hit_test. Mirrors PulpPluginView + the standalone host. nil =
// identity. Set by MacGpuPluginViewHost::set_design_viewport.
@property(nonatomic, copy) pulp::view::Point (^pointTransform)(pulp::view::Point);
@property(nonatomic, assign) float designW;
@property(nonatomic, assign) float designH;
@property(nonatomic, assign) BOOL designTopAlign;
// See PulpPluginView::syncKeyFocus — declared so the GPU host's display-link
// frame-tick block can reconcile first-responder every vsync.
- (void)syncKeyFocus;
// Re-resolve the cursor at the last known pointer position and publish it only
// when it changed. Declared here so the host's frame-tick block (below the
// @implementation) can call it every vsync: AppKit re-asks which cursor to show
// when the pointer MOVES and never because content moved under a still pointer,
// so this is what makes a hovered region change update the cursor immediately.
- (void)refreshHoverCursor;

/// Opt IN to per-presented-frame drag coalescing, and out again.
///
/// Default NO, and that default is load bearing: a host that does not drive
/// -flushCoalescedPointerInput every frame would strand held motion forever,
/// so holding is enabled only by a host that has committed to flushing. Both
/// plug-in hosts run a CVDisplayLink, so both opt in while their link runs.
/// Turning it off flushes.
- (void)setCoalescePointerInput:(BOOL)enabled;

/// Deliver drag motion held since the last presented frame. The host MUST call
/// this once per presented frame, BEFORE the frame's render decision: a frame
/// that decides not to paint must still release input, or latency grows without
/// bound while the editor is visually idle. Safe when nothing is held.
- (void)flushCoalescedPointerInput;

/// Drop held motion WITHOUT delivering it. For a host tearing down mid-gesture,
/// where the drag target may already be gone.
- (void)discardCoalescedPointerInput;

/// Whether the opt-in above is currently set. A host's frame path asserts this
/// out loud: the fail-safe makes a silently-lost opt-in look correct and merely
/// slow, which is the hardest failure of this mechanism to notice.
- (BOOL)coalescingPointerInput;
@end
#endif // PULP_HAS_SKIA

#endif // TARGET_OS_OSX
