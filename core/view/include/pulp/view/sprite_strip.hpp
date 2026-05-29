#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pulp::view {

/// A filmstrip image containing N frames of a control at different values.
/// Used for designer-created knob/fader skins. The control's current value
/// (0.0–1.0) selects which frame to display.
///
/// Typical usage: a 128x128px knob rendered at 128 rotation angles produces
/// a 128x16384 vertical filmstrip. The widget renders the frame corresponding
/// to its current parameter value.
class SpriteStrip {
public:
    enum class Orientation { vertical, horizontal };
    /// Where the strip's pixel data lives. `raw_pixels` is the legacy
    /// in-memory RGBA8 path; `image_file` defers decoding to Skia via the
    /// file path stored in path_ (added for Track A1 figma-plugin imports —
    /// avoids round-tripping the decoded bytes through the JS bridge).
    enum class Source { raw_pixels, image_file };

    SpriteStrip() = default;

    /// Load from raw pixel data (RGBA8, row-major).
    void load(const uint8_t* data, size_t data_size,
              int total_width, int total_height,
              int frame_count, Orientation orientation = Orientation::vertical) {
        data_.assign(data, data + data_size);
        path_.clear();
        source_ = Source::raw_pixels;
        total_width_ = total_width;
        total_height_ = total_height;
        frame_count_ = frame_count;
        orientation_ = orientation;
        compute_frame_dimensions();
    }

    /// Load from an encoded image file (PNG/JPEG/etc.). The strip caches
    /// the path; Skia decodes lazily inside the canvas. Caller MUST pass
    /// the strip's total pixel width/height (Skia will not be re-queried).
    void load_from_file(std::string file_path,
                        int total_width, int total_height,
                        int frame_count,
                        Orientation orientation = Orientation::vertical) {
        path_ = std::move(file_path);
        data_.clear();
        source_ = Source::image_file;
        total_width_ = total_width;
        total_height_ = total_height;
        frame_count_ = frame_count;
        orientation_ = orientation;
        compute_frame_dimensions();
    }

    /// Get the frame index for a normalized value [0.0–1.0].
    int frame_for_value(float value) const {
        if (frame_count_ <= 0) return 0;
        int idx = static_cast<int>(value * static_cast<float>(frame_count_ - 1) + 0.5f);
        return std::clamp(idx, 0, frame_count_ - 1);
    }

    /// Get the pixel offset (x, y) for a given frame index.
    void frame_offset(int frame, int& x, int& y) const {
        if (orientation_ == Orientation::vertical) {
            x = 0;
            y = frame * frame_height_;
        } else {
            x = frame * frame_width_;
            y = 0;
        }
    }

    bool loaded() const {
        if (frame_count_ <= 0) return false;
        return source_ == Source::raw_pixels ? !data_.empty() : !path_.empty();
    }
    Source source() const { return source_; }
    const std::string& path() const { return path_; }
    int frame_count() const { return frame_count_; }
    int frame_width() const { return frame_width_; }
    int frame_height() const { return frame_height_; }
    int total_width() const { return total_width_; }
    int total_height() const { return total_height_; }
    Orientation orientation() const { return orientation_; }
    const uint8_t* data() const { return data_.data(); }
    size_t data_size() const { return data_.size(); }

    /// Whether to interpolate between adjacent frames for smooth sub-frame animation.
    void set_interpolate(bool v) { interpolate_ = v; }
    bool interpolate() const { return interpolate_; }

private:
    void compute_frame_dimensions() {
        if (orientation_ == Orientation::vertical) {
            frame_width_ = total_width_;
            frame_height_ = (frame_count_ > 0) ? total_height_ / frame_count_ : 0;
        } else {
            frame_width_ = (frame_count_ > 0) ? total_width_ / frame_count_ : 0;
            frame_height_ = total_height_;
        }
    }

    std::vector<uint8_t> data_;
    std::string path_;
    Source source_ = Source::raw_pixels;
    int total_width_ = 0;
    int total_height_ = 0;
    int frame_width_ = 0;
    int frame_height_ = 0;
    int frame_count_ = 0;
    Orientation orientation_ = Orientation::vertical;
    bool interpolate_ = false;
};

} // namespace pulp::view
