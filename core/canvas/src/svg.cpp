// Define implementation in this translation unit
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION

#include <pulp/canvas/svg.hpp>

// nanosvg headers
#include <nanosvg.h>
#include <nanosvgrast.h>

#include <cstring>
#include <fstream>
#include <sstream>

namespace pulp::canvas {

SvgImage::~SvgImage() {
    if (image_) nsvgDelete(static_cast<NSVGimage*>(image_));
}

SvgImage::SvgImage(SvgImage&& other) noexcept : image_(other.image_) {
    other.image_ = nullptr;
}

SvgImage& SvgImage::operator=(SvgImage&& other) noexcept {
    if (this != &other) {
        if (image_) nsvgDelete(static_cast<NSVGimage*>(image_));
        image_ = other.image_;
        other.image_ = nullptr;
    }
    return *this;
}

SvgImage SvgImage::from_string(const std::string& svg_data) {
    SvgImage img;
    // nsvgParse modifies the input, so we need a mutable copy
    std::string copy = svg_data;
    img.image_ = nsvgParse(copy.data(), "px", 96.0f);
    return img;
}

SvgImage SvgImage::from_file(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return {};
    std::ostringstream ss;
    ss << file.rdbuf();
    return from_string(ss.str());
}

float SvgImage::width() const {
    if (!image_) return 0;
    return static_cast<NSVGimage*>(image_)->width;
}

float SvgImage::height() const {
    if (!image_) return 0;
    return static_cast<NSVGimage*>(image_)->height;
}

std::vector<uint8_t> SvgImage::rasterize(int w, int h) const {
    if (!image_ || w <= 0 || h <= 0) return {};
    if (width() <= 0 || height() <= 0) return {};

    auto* rast = nsvgCreateRasterizer();
    if (!rast) return {};

    std::vector<uint8_t> pixels(w * h * 4);

    float sx = static_cast<float>(w) / width();
    float sy = static_cast<float>(h) / height();
    float scale = std::min(sx, sy);

    nsvgRasterize(rast, static_cast<NSVGimage*>(image_),
                  0, 0, scale, pixels.data(), w, h, w * 4);

    nsvgDeleteRasterizer(rast);
    return pixels;
}

void SvgImage::render(Canvas& canvas, float x, float y, float w, float h) const {
    if (!image_) return;

    auto* img = static_cast<NSVGimage*>(image_);
    if (img->width <= 0 || img->height <= 0) return;
    float sx = w / img->width;
    float sy = h / img->height;
    float scale = std::min(sx, sy);

    canvas.save();
    canvas.translate(x, y);
    canvas.scale(scale, scale);

    // Render fills and cubic Bezier curves via Canvas's path API instead
    // of approximating each path as straight lines between control handles.
    // The previous implementation:
    //   for (i = 0; i < npts - 1; i += 3)
    //       stroke_line(pts[i*2], pts[i*2+1], pts[(i+3)*2], pts[(i+3)*2+1])
    // had two latent bugs that surfaced as "preset preview blank":
    //   1. Fills were never emitted — no fill_current_path() call. A path
    //      with `fill="#abc"` and no stroke would render NOTHING.
    //   2. Strokes stepped through Bezier control points as if they were
    //      line endpoints, producing jagged polylines connecting handles
    //      rather than the smooth curves the SVG actually defined.
    //
    // The nanosvg point layout for one cubic-Bezier subpath of N segments
    // is (3N+1) flat (x,y) pairs:
    //   pts[0..1]    = subpath start
    //   pts[2..7]    = (cp1, cp2, end) of segment 0
    //   pts[8..13]   = (cp1, cp2, end) of segment 1
    //   ...
    // Starting from index 1 we step in groups of 3 control points, each
    // group encoding one cubic_to(cp1x, cp1y, cp2x, cp2y, x, y).
    for (auto* shape = img->shapes; shape; shape = shape->next) {
        if (!(shape->flags & NSVG_FLAGS_VISIBLE)) continue;

        bool has_fill   = (shape->fill.type   == NSVG_PAINT_COLOR);
        bool has_stroke = (shape->stroke.type == NSVG_PAINT_COLOR);

        if (has_fill) {
            uint32_t c = shape->fill.color;
            canvas.set_fill_color(Color::rgba8(
                c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF,
                static_cast<uint8_t>(shape->opacity * 255)));
        }
        if (has_stroke) {
            uint32_t c = shape->stroke.color;
            canvas.set_stroke_color(Color::rgba8(
                c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF,
                static_cast<uint8_t>(shape->opacity * 255)));
            canvas.set_line_width(shape->strokeWidth);
        }

        if (!has_fill && !has_stroke) continue;

        // Build a single path covering all subpaths of this shape so the
        // fill operates on the union (matches SVG winding semantics; the
        // Canvas API defaults to FillRule::nonzero).
        canvas.begin_path();
        for (auto* path = shape->paths; path; path = path->next) {
            if (path->npts < 1) continue;
            const float* pts = path->pts;
            canvas.move_to(pts[0], pts[1]);
            // Each cubic segment consumes 3 control points (cp1, cp2, end).
            // i steps through the start-index of each group.
            for (int i = 1; i + 2 < path->npts; i += 3) {
                const float* g = &pts[i * 2];
                canvas.cubic_to(g[0], g[1], g[2], g[3], g[4], g[5]);
            }
            if (path->closed) canvas.close_path();
        }

        if (has_fill) {
            // Honor `fill-rule="evenodd"` so SVGs with cutouts or holes
            // (a common preset-thumbnail pattern) render correctly. nanosvg
            // exposes the rule on the shape; default in SVG is nonzero.
            FillRule rule = (shape->fillRule == NSVG_FILLRULE_EVENODD)
                ? FillRule::evenodd
                : FillRule::nonzero;
            canvas.fill_current_path(rule);
        }
        if (has_stroke) canvas.stroke_current_path();
    }

    canvas.restore();
}

} // namespace pulp::canvas
