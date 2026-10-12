#pragma once

#include <cstdint>
#include <string>

// Native-view helpers for the AAX editor shell. AAX hands the plug-in a host
// owned parent view (NSView* on macOS) and sizes it from GetViewSize; the
// plug-in's child view has to be placed inside that parent explicitly. These
// helpers keep that placement, and the frame diagnostics that prove what the
// host actually did, out of the shared PluginViewHost so no other format's
// attach path changes. Every function is a no-op on platforms without a
// native AAX parent, so the adapter can call them unconditionally.

namespace pulp::format::aax {

/// A native rectangle in the parent's own (logical point) coordinate space.
struct AaxNativeRect {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
};

/// Everything the host-frame receipt needs in one read: the parent the host
/// handed down, the child Pulp attached, and the window both sit in.
struct AaxNativeGeometry {
    bool valid = false;          ///< both views were non-null
    bool has_window = false;     ///< parent sits in a window
    bool parent_flipped = false; ///< parent's y axis grows downward
    AaxNativeRect parent_frame;  ///< parent frame in its superview
    AaxNativeRect parent_bounds; ///< parent's own coordinate space
    AaxNativeRect child_frame;   ///< child frame inside the parent
    AaxNativeRect window_frame;  ///< window frame on screen (logical points)
    float backing_scale = 1.0f;  ///< window (or main screen) backing scale
};

/// The y origin that puts a child of `child_height` at the TOP of a parent of
/// `parent_height`. AppKit's default view space grows upward, so origin (0,0)
/// is the bottom-left corner; a host parent larger than the child then shows
/// the editor in its lower-left corner. A flipped parent already has (0,0) at
/// the top-left.
constexpr float aax_child_top_left_y(float parent_height, float child_height, bool parent_flipped) {
    if (parent_flipped)
        return 0.0f;
    const float y = parent_height - child_height;
    return y > 0.0f ? y : 0.0f;
}

/// Return the backing scale for the native AAX parent (NSView* on macOS).
/// A null parent is valid because AAX may query GetViewSize before attaching
/// the view; the platform implementation uses the main display then.
#if defined(__APPLE__)
float aax_display_scale(void* native_parent) noexcept;
#else
inline float aax_display_scale(void*) noexcept {
    return 1.0f;
}
#endif

#if defined(__APPLE__)
/// Read the parent/child/window frames. Never throws; `valid` is false when a
/// view is missing.
AaxNativeGeometry aax_native_geometry(void* native_parent, void* native_child) noexcept;

/// Move the child to the parent's top-left corner, keeping the child's size.
/// A no-op when the sizes already match (origin stays at 0,0), which is the
/// VST3/CLAP case where the host parent is created at the requested size.
void aax_place_child_top_left(void* native_parent, void* native_child) noexcept;

/// Resize the child to the parent's bounds. The child's flexible autoresizing
/// mask then tracks the parent 1:1, and the design viewport letterboxes the
/// content, so a host that keeps an off-size parent still shows a whole,
/// proportional editor rather than a clipped corner.
void aax_fill_parent(void* native_parent, void* native_child) noexcept;

/// Whether the child follows the parent's size changes (its flexible
/// autoresizing mask). AppKit autoresizing applies the parent's size DELTA to
/// the child: a 990x645 child added to a parent the host has not laid out yet
/// (0x0) becomes 1980x1290 the moment the host grows the parent to 990x645,
/// and the editor then publishes that doubled frame back to the host. Turn
/// tracking off around the host's own sizing transaction and back on once the
/// parent and child agree.
void aax_set_child_tracks_parent(void* native_child, bool tracks) noexcept;
#else
inline AaxNativeGeometry aax_native_geometry(void*, void*) noexcept {
    return {};
}
inline void aax_place_child_top_left(void*, void*) noexcept {}
inline void aax_fill_parent(void*, void*) noexcept {}
inline void aax_set_child_tracks_parent(void*, bool) noexcept {}
#endif

/// One-line rendering of a geometry read for the host receipt.
inline std::string aax_describe_geometry(const AaxNativeGeometry& g) {
    auto rect = [](const AaxNativeRect& r) {
        return std::to_string(static_cast<int>(r.width)) + "x" +
               std::to_string(static_cast<int>(r.height)) + "@" +
               std::to_string(static_cast<int>(r.x)) + "," + std::to_string(static_cast<int>(r.y));
    };
    if (!g.valid)
        return "invalid";
    return "parent=" + rect(g.parent_frame) + " parent_bounds=" + rect(g.parent_bounds) +
           " child=" + rect(g.child_frame) +
           " window=" + (g.has_window ? rect(g.window_frame) : std::string("none")) +
           " scale=" + std::to_string(g.backing_scale) + (g.parent_flipped ? " flipped" : "");
}

} // namespace pulp::format::aax
