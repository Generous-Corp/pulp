// SceneImage — retained image / bitmap node.
//
// Item 6.1 / Pulp-native names. The actual pixel source is provided by
// the caller as either a decoded RGBA byte buffer (CPU-side) or a path
// to a file the underlying Canvas backend will decode on demand. The
// node carries a destination rect; the walker emits the matching
// `Canvas::draw_image_from_*` call.
#pragma once

#include <pulp/canvas/scene/scene_node.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pulp::canvas {

class SceneImage : public SceneNode {
public:
    SceneImage() : SceneNode(SceneNodeKind::image) {}

    void set_rect(float x, float y, float w, float h) {
        if (x == x_ && y == y_ && w == w_ && h == h_) return;
        x_ = x; y_ = y; w_ = w; h_ = h;
        mark_dirty();
    }

    float x() const { return x_; }
    float y() const { return y_; }
    float w() const { return w_; }
    float h() const { return h_; }

    /// Attach a decoded RGBA byte buffer. The caller-supplied bytes are
    /// stored by value so the node remains valid after the caller's
    /// buffer goes out of scope.
    void set_rgba_pixels(std::vector<uint8_t> rgba, int pixel_width,
                          int pixel_height) {
        rgba_pixels_ = std::move(rgba);
        pixel_width_ = pixel_width;
        pixel_height_ = pixel_height;
        encoded_bytes_.clear();
        file_path_.clear();
        mark_dirty();
    }

    /// Attach an encoded image blob (PNG/JPEG bytes). Backends call
    /// `Canvas::draw_image_from_data` at paint time.
    void set_encoded_bytes(std::vector<uint8_t> bytes) {
        encoded_bytes_ = std::move(bytes);
        rgba_pixels_.clear();
        file_path_.clear();
        pixel_width_ = 0;
        pixel_height_ = 0;
        mark_dirty();
    }

    /// Attach a path to an encoded image on disk.
    void set_file_path(std::string path) {
        file_path_ = std::move(path);
        rgba_pixels_.clear();
        encoded_bytes_.clear();
        pixel_width_ = 0;
        pixel_height_ = 0;
        mark_dirty();
    }

    const std::vector<uint8_t>& rgba_pixels() const { return rgba_pixels_; }
    const std::vector<uint8_t>& encoded_bytes() const { return encoded_bytes_; }
    const std::string& file_path() const { return file_path_; }
    int pixel_width() const { return pixel_width_; }
    int pixel_height() const { return pixel_height_; }

    // ── SceneNode overrides ──────────────────────────────────────────────
    SceneRect local_bounds() const override { return {x_, y_, w_, h_}; }
    void paint_geometry(Canvas& canvas) const override;

private:
    float x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    std::vector<uint8_t> rgba_pixels_;
    std::vector<uint8_t> encoded_bytes_;
    std::string file_path_;
    int pixel_width_ = 0;
    int pixel_height_ = 0;
};

}  // namespace pulp::canvas
