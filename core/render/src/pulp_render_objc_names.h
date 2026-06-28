#pragma once

// Per-binary-unique Objective-C class names for the macOS render layer, the
// render-side companion to core/view/platform/mac/pulp_mac_objc_names.h.
//
// metal_surface_mac.mm defines an ObjC class (PulpMetalView, the CAMetalLayer-
// backed GPU surface view) compiled once into the shared pulp-render static
// library, so every plug-in linking pulp-render registers the SAME class name.
// Loading two Pulp plug-ins into one host then makes the objc runtime emit
// "Class X is implemented in both ..." and lets the first-loaded copy shadow the
// others — the same cross-plug-in collision the view layer fixes, here for the
// GPU surface view. (render_loop_apple.mm's display-link class is iOS-only —
// inside TARGET_OS_IPHONE — so it has no macOS copy to namespace.)
//
// The fix reuses the view layer's mechanism: each shipped binary compiles its
// own copy of these TUs with the per-binary token PULP_VIEW_OBJC_SUFFIX (set
// target-wide by _pulp_apply_view_mac_objc_suffix() in PulpUtils.cmake), so the
// class names become unique per binary and the shared pulp-render copies are not
// pulled in. The same token is shared so a single suffix identifies the binary
// across both the view and render clusters. No-op when the suffix is undefined.

#ifdef PULP_VIEW_OBJC_SUFFIX

#define PULP_RENDER_OBJC_PASTE2(a, b) a##b
#define PULP_RENDER_OBJC_PASTE(a, b) PULP_RENDER_OBJC_PASTE2(a, b)
#define PULP_RENDER_OBJC_NAME(base) PULP_RENDER_OBJC_PASTE(base, PULP_VIEW_OBJC_SUFFIX)

// GPU surface NSView (metal_surface_mac.mm). Renamed to a DISTINCT base
// (PulpMetalSurfaceView) — the view layer's window_host_mac.mm defines a separate
// `PulpMetalView`, so a shared suffix alone would still collide; the distinct
// base also resolves that pre-existing same-name ambiguity between the two.
#define PulpMetalView                  PULP_RENDER_OBJC_NAME(PulpMetalSurfaceView)

#endif  // PULP_VIEW_OBJC_SUFFIX
