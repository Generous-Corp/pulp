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
    float sx = w / img->width;
    float sy = h / img->height;
    float scale = std::min(sx, sy);

    canvas.save();
    canvas.translate(x, y);
    canvas.scale(scale, scale);

    // Walk SVG shapes and render using Canvas primitives
    for (auto* shape = img->shapes; shape; shape = shape->next) {
        if (!(shape->flags & NSVG_FLAGS_VISIBLE)) continue;

        // Set fill color
        if (shape->fill.type == NSVG_PAINT_COLOR) {
            uint32_t c = shape->fill.color;
            canvas.set_fill_color(Color::rgba8(
                c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF,
                static_cast<uint8_t>(shape->opacity * 255)));
        }

        // Set stroke
        if (shape->stroke.type == NSVG_PAINT_COLOR) {
            uint32_t c = shape->stroke.color;
            canvas.set_stroke_color(Color::rgba8(
                c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF,
                static_cast<uint8_t>(shape->opacity * 255)));
            canvas.set_line_width(shape->strokeWidth);
        }

        // Render paths as line segments (simplified — full path rendering
        // would require Canvas path API which we don't have yet)
        for (auto* path = shape->paths; path; path = path->next) {
            if (path->npts < 2) continue;

            // Draw as connected lines between control points
            for (int i = 0; i < path->npts - 1; i += 3) {
                float* p = &path->pts[i * 2];
                float* q = &path->pts[(i + 3 < path->npts ? i + 3 : 0) * 2];
                if (shape->stroke.type != NSVG_PAINT_NONE)
                    canvas.stroke_line(p[0], p[1], q[0], q[1]);
            }
        }
    }

    canvas.restore();
}

} // namespace pulp::canvas
