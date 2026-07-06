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

#include <cstdint>

namespace pulp::view {
class View;
class ModalOverlay;
struct WindowOptions;
}  // namespace pulp::view

#ifdef __OBJC__

#import <Cocoa/Cocoa.h>

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

// Depth-first search for the topmost (last-painted) ModalOverlay in the
// subtree rooted at `root`. Returns nullptr when none is visible.
pulp::view::ModalOverlay* find_topmost_modal(pulp::view::View* root);

}  // namespace pulp::view::mac_geometry

#endif  // __OBJC__
