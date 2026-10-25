#pragma once

// A host's Skia surface follows the text raster policy its view tree asks for.
// Internal to the platform view hosts.
//
// The policy is a property of the surface (pulp/canvas/text_raster_policy.hpp):
// imported text authored `-webkit-font-smoothing: antialiased` reproduces
// Chromium's glyphs only on a `chromium` surface. A tree requests it on its
// root (View::request_text_raster_policy), which the bridge and the native
// materializer do, and the host applies it here at the start of each frame,
// before damage is decided: a changed policy recreates the surface's targets,
// so the frame that follows repaints everything.

#include <pulp/render/skia_surface.hpp>
#include <pulp/view/view.hpp>

namespace pulp::view {

inline void apply_text_raster_policy(render::SkiaSurface& surface, View& root) {
    const auto wanted = root.text_raster_policy();
    if (surface.text_raster_policy() == wanted)
        return;
    surface.set_text_raster_policy(wanted);
    root.request_repaint();
}

} // namespace pulp::view
