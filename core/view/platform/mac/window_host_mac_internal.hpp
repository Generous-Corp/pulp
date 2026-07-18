// window_host_mac_internal.hpp — private forward declarations for the
// geometry / coordinate / event-translation helpers used by
// window_host_mac.mm.
//
// Split out of the oversized window_host_mac.mm translation unit. The
// implementations live in window_host_mac_geometry.mm. Only consumed by
// window_host_mac.mm — not part of the public SDK surface.
//
// Only free functions / file-local statics that do NOT touch PulpView ivars
// or any Obj-C instance state are extracted. The PulpView @implementation
// block, its ivars, and its category/delegate methods stay in window_host_mac.mm.

#pragma once

#include <pulp/view/geometry.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/view/view.hpp>  // View::CursorStyle (set_ns_cursor_for_style)

#include <cstdint>

namespace pulp::view {
class View;
class ModalOverlay;
struct WindowOptions;
}  // namespace pulp::view

#ifdef __OBJC__

#import <Cocoa/Cocoa.h>
#import <CoreVideo/CoreVideo.h>
#import <QuartzCore/QuartzCore.h>

// Per-binary-unique ObjC class names (renames PulpView when a shipped binary
// defines PULP_VIEW_OBJC_SUFFIX); keeps geometry helpers in sync with the
// PulpView @implementation in window_host_mac.mm.
#include "pulp_mac_objc_names.h"

namespace pulp::view::mac_geometry {

// ── Window lifecycle / configuration ─────────────────────────────────

// Request that the application close `window` (or the key/main window
// when `window` is nil), falling back to `[NSApp stop:]` when no window
// is available.
void request_app_close(NSWindow* window);

// Apply multi-window type configuration (palette / inspector / popup /
// dialog / main) and any parent-child relationship to a freshly created
// NSWindow.
void configure_window_type(NSWindow* window, const pulp::view::WindowOptions& options);

// ── Child-view (embedded native subview) geometry ────────────────────

// Convert a top-down (x, y, width, height) rect into the container's
// Cocoa coordinate space, honoring whether the container is flipped.
NSRect child_view_frame_in_host(NSView* container,
                                float x,
                                float y,
                                float width,
                                float height);

// Attach `child_view_handle` as a subview of `container`, positioning it
// per child_view_frame_in_host. Returns false on null inputs.
bool attach_child_view_to_host(NSView* container,
                               void* child_view_handle,
                               float x,
                               float y,
                               float width,
                               float height);

// Reposition an already-attached child view. Returns false if the child
// is not currently a subview of `container`.
bool set_child_view_bounds_in_host(NSView* container,
                                   void* child_view_handle,
                                   float x,
                                   float y,
                                   float width,
                                   float height);

// Detach a child view previously attached via attach_child_view_to_host.
void detach_child_view_from_host(NSView* container, void* child_view_handle);

// Mask an attached child view to a visible sub-rectangle expressed in the
// child's OWN top-left [0,0,frame_w,frame_h] box (Pulp convention), via a
// CALayer mask, so a native child inside a scroll region is clipped to its
// scroll ancestor's viewport without resizing (and reflowing) the child.
// has_clip=false removes any mask. Returns false unless the child is a subview
// of `container`.
bool clip_child_view_in_host(NSView* container,
                             void* child_view_handle,
                             bool has_clip,
                             float x,
                             float y,
                             float width,
                             float height);

// ── Coordinate / event translation ───────────────────────────────────

// Translate an NSEvent modifier-flags mask into Pulp's kMod* bitmask.
uint16_t modifiers_from_ns_flags(NSEventModifierFlags flags);

// Build a root-space mouse event and let the gesture arbiter consume it first.
bool dispatch_mac_gesture_pointer_event(pulp::view::View* root,
                                        pulp::view::Point pt,
                                        NSEvent* event,
                                        pulp::view::MousePhase phase,
                                        bool is_down);

// Convert window-space `pos` into `target`'s local-pre-scale coordinates,
// peeling off each ancestor's set_scale transform. `root` bounds the walk.
pulp::view::Point to_local(pulp::view::Point pos,
                           pulp::view::View* target,
                           pulp::view::View* root);

// Confirm a cached View* is still attached to the live tree rooted at
// `root` before any dereference. Pointer-compare only — safe to call
// with `needle` pointing into freed memory.
bool view_is_in_tree(pulp::view::View* needle, pulp::view::View* root);

// Translate a macOS virtual key code into Pulp's KeyCode enum.
pulp::view::KeyCode key_code_from_ns(unsigned short code);

// ── Cursor ───────────────────────────────────────────────────────────

// Hide or show the process cursor, keeping AppKit's reference-counted
// [NSCursor hide]/[NSCursor unhide] pair balanced: the hidden state is
// tracked here, so repeated calls in the same direction are no-ops and
// the count can never run away. Shared with set_ns_cursor_for_style —
// relative-mouse mode and an `invisible` cursor style hide the same
// cursor and must not each keep their own count.
void set_ns_cursor_hidden(bool hidden);

// Set the process cursor to the NSCursor backing `style`, unhiding first
// (via set_ns_cursor_hidden) for any style other than `invisible`.
// Styles with no native backing fall back to the arrow cursor.
void set_ns_cursor_for_style(pulp::view::View::CursorStyle style);

// Depth-first search for the topmost (last-painted) ModalOverlay in the
// subtree rooted at `root`. Returns nullptr when none is visible.
pulp::view::ModalOverlay* find_topmost_modal(pulp::view::View* root);

}  // namespace pulp::view::mac_geometry

// ── Display-link frame timing ────────────────────────────────────────
//
// Shared by every macOS host that drives frames from a CVDisplayLink (the GPU
// window host, and both plugin-view hosts). Converts the display link's OWN
// presentation timestamp into monotonic seconds on the same timebase as
// CACurrentMediaTime, so the dt handed to the animation system is the interval
// the frame will actually be on screen for — not the callback's arrival time,
// and not a hardcoded 1/60 (which ran animations at 2x on a 120 Hz display).
//
// Falls back to CACurrentMediaTime() when the link reports no valid host time,
// which keeps the timebase identical either way.
namespace pulp::view::mac_frame_timing {

inline double display_link_seconds(const CVTimeStamp* ts) {
    if (ts && (ts->flags & kCVTimeStampHostTimeValid)) {
        const double freq = CVGetHostClockFrequency();
        if (freq > 0.0) return static_cast<double>(ts->hostTime) / freq;
    }
    return CACurrentMediaTime();
}

/// Nominal frame interval (seconds) of `link`, for HostFramePump::set_nominal_dt.
/// Returns 0 when the link can report no refresh period at all, in which case the
/// caller should leave the pump's default (1/60) in place.
///
/// NOMINAL, not ACTUAL, is the source. CVDisplayLinkGetActualOutputVideoRefreshPeriod
/// returns a MEASURED period and is documented to return 0 until the link has
/// computed one — which is exactly the state microseconds after CVDisplayLinkStart(),
/// where every host seeds its pump. Seeding from it therefore never fired: the
/// pump kept its 1/60 default forever, and the first / wake frame on a 120 Hz
/// ProMotion display was double the real interval. Nominal is valid the moment the
/// link is bound to a display (verified: 1/60 straight out of
/// CVDisplayLinkCreateWithCGDisplay, before Start).
///
/// Actual is kept as the FALLBACK for a variable-refresh-rate display, whose
/// nominal period can legitimately be kCVTimeIsIndefinite; on such a display the
/// measured period is the only answer, and it becomes valid once the link runs
/// (hosts re-seed on display re-bind).
inline float display_link_nominal_dt(CVDisplayLinkRef link) {
    if (!link) return 0.0f;
    const CVTime nominal = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(link);
    if ((nominal.flags & kCVTimeIsIndefinite) == 0 && nominal.timeScale != 0 &&
        nominal.timeValue > 0) {
        const double period = static_cast<double>(nominal.timeValue) /
                              static_cast<double>(nominal.timeScale);
        if (period > 0.0 && period < 1.0) return static_cast<float>(period);
    }
    const double measured = CVDisplayLinkGetActualOutputVideoRefreshPeriod(link);
    if (measured > 0.0 && measured < 1.0) return static_cast<float>(measured);
    return 0.0f;
}

/// Owns one CVDisplayLinkRef's handle lifecycle: create, route the output
/// callback, bind a display, start, stop, release.
///
/// Every macOS host that drives frames from a link (the standalone GPU window
/// host and both plugin-view hosts) hand-rolled this CoreVideo boilerplate, and
/// the release paths drifted. The verbs stay orthogonal — one per CoreVideo call
/// — so each host keeps ordering its own pump seeding / suspension around them
/// exactly as it needs to. The vsync gate, the liveness protocol that lets the
/// callback's main-queue block outlive the host, and the frame closure itself
/// deliberately stay with the host: those three protocols are not identical
/// across the hosts, so this owns the handle and nothing else.
///
/// Not thread-safe: every verb is called from the host's thread, never from the
/// link callback.
class MacDisplayLinkDriver {
public:
    MacDisplayLinkDriver() = default;
    ~MacDisplayLinkDriver() { stop(); }

    MacDisplayLinkDriver(const MacDisplayLinkDriver&) = delete;
    MacDisplayLinkDriver& operator=(const MacDisplayLinkDriver&) = delete;

    /// Does a link handle exist? True between a successful open() and stop().
    /// Independent of whether the link is currently ticking.
    bool is_open() const { return link_ != nullptr; }

    /// Create a link over the active displays and route its output callback to
    /// (`callback`, `context`). Returns false — leaving the driver closed —
    /// when a link is already open or CoreVideo declines to create one.
    bool open(CVDisplayLinkOutputCallback callback, void* context) {
        if (link_) return false;
        CVDisplayLinkCreateWithActiveCGDisplays(&link_);
        if (!link_) return false;
        CVDisplayLinkSetOutputCallback(link_, callback, context);
        return true;
    }

    /// Point the link at `display`, so frames are paced by the vsync of the
    /// screen the host is actually on. No-op while closed.
    void bind_to_display(CGDirectDisplayID display) {
        if (link_) CVDisplayLinkSetCurrentCGDisplay(link_, display);
    }

    /// Start ticking. No-op while closed.
    void resume() {
        if (link_) CVDisplayLinkStart(link_);
    }

    /// This link's nominal frame interval, or 0 while closed / unknown.
    float nominal_dt() const { return display_link_nominal_dt(link_); }

    /// Stop ticking and release the handle. Idempotent. CVDisplayLinkStop is
    /// synchronous, so no output callback runs after this returns — but a block
    /// an earlier callback already dispatched to the main queue can still be
    /// pending, which is what each host's own liveness token covers.
    void stop() {
        if (!link_) return;
        CVDisplayLinkStop(link_);
        CVDisplayLinkRelease(link_);
        link_ = nullptr;
    }

private:
    CVDisplayLinkRef link_ = nullptr;
};

/// Should the CPU plugin-view host drive a per-vsync render link?
///
/// Yes whenever the editor is in a window. It used to additionally require a
/// scripted idle callback, which ONLY a JS/scripted editor installs
/// (`make_scripted_idle_pump`, wired in clap_entry / vst3_plug_view /
/// au_v2_cocoa_view / au_view_controller_mac — all guarded on a script bridge).
/// A native C++ editor installs none, so it got no render link, no FrameClock
/// tick, no widget or CSS animation, and a caret that never blinked.
///
/// The GPU plugin host has always started its link on window attach alone; this
/// is what makes the CPU host match. Running the link is not the same as running
/// a frame: the link callback gates every vsync on the link thread
/// (`should_dispatch_host_frame`), so an editor whose tree is static costs one
/// atomic read per vsync and never reaches the main thread.
inline bool plugin_view_wants_render_link(bool has_view, bool in_window) {
    return has_view && in_window;
}

}  // namespace pulp::view::mac_frame_timing

// ── Host clear / background color ─────────────────────────────────────
//
// Every macOS host (the standalone window host and both plugin-view hosts)
// seeds an opaque dark background (RGB 30,30,46 = 0x1E1E2E) so no
// clear/undefined composite flashes before the first Metal frame lands.
// Defined once here so the NSColor, CGColor, and canvas rgba8 spellings can
// never drift apart.
namespace pulp::view::mac_host {

inline constexpr uint8_t kHostClearR = 30;
inline constexpr uint8_t kHostClearG = 30;
inline constexpr uint8_t kHostClearB = 46;

// AppKit spelling for NSWindow.backgroundColor.
inline NSColor* ns_host_clear_color() {
    return [NSColor colorWithCalibratedRed:kHostClearR / 255.0
                                     green:kHostClearG / 255.0
                                      blue:kHostClearB / 255.0
                                     alpha:1.0];
}

// Core Graphics spelling for CALayer.backgroundColor. Returns a +1-owned
// CGColorRef (CG "Create" rule) in sRGB; assign it to a retaining property.
inline CGColorRef cg_host_clear_color() {
    const CGFloat comps[4] = {kHostClearR / 255.0, kHostClearG / 255.0,
                              kHostClearB / 255.0, 1.0};
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGColorRef color = CGColorCreate(cs, comps);
    CGColorSpaceRelease(cs);
    return color;
}

}  // namespace pulp::view::mac_host

#endif  // __OBJC__
