#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

    SpriteStrip() = default;

    /// Load from raw pixel data (RGBA8, row-major).
    void load(const uint8_t* data, size_t data_size,
              int total_width, int total_height,
              int frame_count, Orientation orientation = Orientation::vertical) {
        data_.assign(data, data + data_size);
        total_width_ = total_width;
        total_height_ = total_height;
        frame_count_ = frame_count;
        orientation_ = orientation;

        if (orientation == Orientation::vertical) {
            frame_width_ = total_width;
            frame_height_ = total_height / frame_count;
        } else {
            frame_width_ = total_width / frame_count;
            frame_height_ = total_height;
        }
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

    bool loaded() const { return !data_.empty() && frame_count_ > 0; }
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
    std::vector<uint8_t> data_;
    int total_width_ = 0;
    int total_height_ = 0;
    int frame_width_ = 0;
    int frame_height_ = 0;
    int frame_count_ = 0;
    Orientation orientation_ = Orientation::vertical;
    bool interpolate_ = false;
};

} // namespace pulp::view
