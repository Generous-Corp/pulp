// paint_probe.hpp — whether this build can rasterise at all.
//
// `render_to_rgba` returns an empty vector when there is no Skia raster
// backend, and `render_to_png` quietly falls back to CoreGraphics, which
// composites no filters. Both shapes come from one cause, so every suite that
// samples rendered pixels goes red on a lane built without Skia: the
// sanitizer lanes and the advisory x86_64 Rosetta lane. Those failures read as
// "the colour is wrong" rather than "there was no colour", which sends a
// reader hunting a paint bug that is not there and leaves the lane
// permanently red. A lane everyone has learned to ignore is worse than no
// lane, so a suite that cannot run says so instead of failing.
//
// A runtime probe rather than a build flag, because the same emptiness comes
// from a missing Skia AND from a Skia with no device to draw on. A
// `PULP_HAS_SKIA` test would claim paint works on a machine that still cannot
// draw.
//
// The probe asks only whether pixels arrive, never whether they are the RIGHT
// pixels. That distinction is what keeps a control case honest: a build that
// returns a correctly sized but blank buffer passes the probe and still fails
// its control, which is the failure a control exists to catch. Widening this
// to "the pixels look right" would skip exactly the runs worth seeing.

#pragma once

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/geometry.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>

#include <cstdint>

namespace pulp::test {

/// Whether `render_to_rgba` yields pixels in this build. Cached: the answer
/// cannot change within a run, and the probe itself allocates a surface.
inline bool paint_available() {
    static const bool ok = [] {
        pulp::view::View probe;
        probe.set_bounds(pulp::view::Rect{0.0f, 0.0f, 8.0f, 8.0f});
        probe.set_background_color(pulp::view::Color::rgba8(255, 0, 0));
        uint32_t w = 0, h = 0;
        const auto px = pulp::view::render_to_rgba(probe, 8, 8, 1.0f, &w, &h);
        return !px.empty() && w > 0 && h > 0;
    }();
    return ok;
}

}  // namespace pulp::test

/// Skip a case that cannot run without a rasteriser, naming what went
/// uncovered. A skip that reads like a pass recreates the original problem
/// from the other side, so this always states what is NOT covered.
#define PULP_SKIP_WITHOUT_PAINT(what)                                        \
    do {                                                                     \
        if (!::pulp::test::paint_available()) {                              \
            WARN("SKIPPED: this build has no Skia raster backend, so "       \
                 what " is NOT covered by this run");                        \
            return;                                                          \
        }                                                                    \
    } while (0)
