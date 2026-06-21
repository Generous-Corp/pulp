// cg_canvas_shadow.mm — CoreGraphics drop-shadow state slice.
//
// Pairs with core/canvas/src/skia_canvas_box_shadow.cpp on the Skia side.
// Covers the four Canvas2D `ctx.shadow*` setters plus the
// apply_shadow_to_context() rebuild helper.
//
// CG owns the sticky shadow state via its GState stack —
// CGContextSetShadowWithColor snapshots into the current GState, and a
// later CGContextRestoreGState restores whatever shadow was active in
// the parent frame. This matches Canvas2D save()/restore() semantics
// for `ctx.shadow*` exactly.

#include <pulp/canvas/cg_canvas.hpp>

#ifdef __APPLE__

#import <CoreGraphics/CoreGraphics.h>

namespace pulp::canvas {

void CoreGraphicsCanvas::apply_shadow_to_context() {
    const bool active = shadow_color_.a > 0.0f &&
        (shadow_blur_ > 0.0f || shadow_offset_x_ != 0.0f ||
         shadow_offset_y_ != 0.0f);
    if (!active) {
        CGContextSetShadowWithColor(ctx_, CGSizeZero, 0.0f, nullptr);
        return;
    }
    CGFloat components[4] = {shadow_color_.r, shadow_color_.g,
                             shadow_color_.b, shadow_color_.a};
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGColorRef color = CGColorCreate(cs, components);
    CGContextSetShadowWithColor(
        ctx_, CGSizeMake(shadow_offset_x_, shadow_offset_y_),
        shadow_blur_, color);
    CGColorRelease(color);
    CGColorSpaceRelease(cs);
}

void CoreGraphicsCanvas::set_shadow_color(Color color) {
    shadow_color_ = color;
    apply_shadow_to_context();
}
void CoreGraphicsCanvas::set_shadow_blur(float blur) {
    shadow_blur_ = (blur > 0.0f) ? blur : 0.0f;
    apply_shadow_to_context();
}
void CoreGraphicsCanvas::set_shadow_offset_x(float dx) {
    shadow_offset_x_ = dx;
    apply_shadow_to_context();
}
void CoreGraphicsCanvas::set_shadow_offset_y(float dy) {
    shadow_offset_y_ = dy;
    apply_shadow_to_context();
}

}  // namespace pulp::canvas

#endif  // __APPLE__
