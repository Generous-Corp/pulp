#pragma once

// AT-SPI Component geometry: pure arithmetic with no View or runtime
// dependency, so the Linux provider and its tests share one definition and the
// tests need not pull in the view headers.

#include <cmath>
#include <cstdint>

namespace pulp::view::atspi {

// ── Component geometry ───────────────────────────────────────────────────────
// org.a11y.atspi.Component answers in one of three coordinate spaces. A View's
// bounds are in the root view's space, and a window host may show that root
// scaled and letterboxed (a design viewport), so the window position of a root
// point is `root * scale + offset`, not the root point itself. The screen
// position adds where the window sits on the desktop.

/// CoordType values on the wire.
inline constexpr uint32_t kCoordScreen = 0;
inline constexpr uint32_t kCoordWindow = 1;

/// Where the root view sits in its window and on screen.
struct WindowGeometry {
    /// Whether the window's screen origin has been reported. Screen coordinates
    /// are answered only after it has.
    bool screen_origin_known = false;
    int screen_x = 0;
    int screen_y = 0;
    /// window position = root position * scale + (offset_x, offset_y).
    float scale = 1.0f;
    float offset_x = 0.0f;
    float offset_y = 0.0f;
};

/// A rectangle in whole pixels.
struct PixelRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    friend constexpr bool operator==(const PixelRect&, const PixelRect&) = default;
};

/// Whether this geometry can answer a request in `coord_type`. Window
/// coordinates always can; screen coordinates need the window's screen origin;
/// parent coordinates are not offered.
constexpr bool coord_type_available(uint32_t coord_type, const WindowGeometry& g) {
    if (coord_type == kCoordWindow)
        return true;
    return coord_type == kCoordScreen && g.screen_origin_known;
}

/// A rectangle given in root-view space, expressed in `coord_type`. The caller
/// checks coord_type_available() first. Positions round to the nearest pixel and
/// sizes scale with the viewport.
inline PixelRect root_rect_in(float x, float y, float width, float height, uint32_t coord_type,
                              const WindowGeometry& g) {
    const float scale = g.scale > 0.0f ? g.scale : 1.0f;
    const bool screen = coord_type == kCoordScreen;
    PixelRect out;
    out.x = static_cast<int>(std::lround(x * scale + g.offset_x)) + (screen ? g.screen_x : 0);
    out.y = static_cast<int>(std::lround(y * scale + g.offset_y)) + (screen ? g.screen_y : 0);
    out.width = static_cast<int>(std::lround(width * scale));
    out.height = static_cast<int>(std::lround(height * scale));
    return out;
}

} // namespace pulp::view::atspi
