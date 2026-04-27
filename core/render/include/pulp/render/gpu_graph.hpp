#pragma once

#include <cstddef>
#include <limits>
#include <vector>

namespace pulp::render {

/// GPU-accelerated 1D data renderer for waveforms, spectra, and EQ curves.
/// Packs float data into a format suitable for GPU texture upload and
/// SkSL shader sampling.
class GpuGraphRenderer {
public:
    /// Update the data buffer. Thread-safe when used with TripleBuffer.
    void set_data(const float* data, size_t count) {
        if (data == nullptr || count == 0) {
            data_.clear();
            return;
        }
        data_.assign(data, data + count);
    }

    void set_data(std::vector<float> data) {
        data_ = std::move(data);
    }

    const std::vector<float>& data() const { return data_; }
    size_t count() const { return data_.size(); }
    bool empty() const { return data_.empty(); }

    /// Line thickness for the graph (pixels)
    void set_line_thickness(float t) { line_thickness_ = t; }
    float line_thickness() const { return line_thickness_; }

    /// Whether to show filled area under the curve
    void set_show_fill(bool v) { show_fill_ = v; }
    bool show_fill() const { return show_fill_; }

    /// Fill center position (0=top, 0.5=center, 1=bottom)
    void set_fill_center(float v) { fill_center_ = v; }
    float fill_center() const { return fill_center_; }

private:
    std::vector<float> data_;
    float line_thickness_ = 1.5f;
    bool show_fill_ = true;
    float fill_center_ = 0.5f;
};

/// GPU-accelerated 2D data renderer for spectrograms and heatmaps.
/// Stores a 2D grid of float values with color ramp mapping.
class GpuHeatMapRenderer {
public:
    /// Set the data grid (row-major, width x height).
    void set_data(const float* data, size_t width, size_t height) {
        if (data == nullptr || width == 0 || height == 0 ||
            height > std::numeric_limits<size_t>::max() / width) {
            width_ = 0;
            height_ = 0;
            data_.clear();
            return;
        }
        width_ = width;
        height_ = height;
        data_.assign(data, data + width * height);
    }

    const std::vector<float>& data() const { return data_; }
    size_t width() const { return width_; }
    size_t height() const { return height_; }
    bool empty() const { return data_.empty(); }

    /// Min/max values for color ramp mapping
    void set_range(float min_val, float max_val) {
        min_val_ = min_val;
        max_val_ = max_val;
    }
    float min_val() const { return min_val_; }
    float max_val() const { return max_val_; }

private:
    std::vector<float> data_;
    size_t width_ = 0;
    size_t height_ = 0;
    float min_val_ = 0.0f;
    float max_val_ = 1.0f;
};

/// GPU-accelerated instanced bar renderer for spectrum displays.
class GpuBarRenderer {
public:
    void set_data(const float* data, size_t count) {
        if (data == nullptr || count == 0) {
            data_.clear();
            return;
        }
        data_.assign(data, data + count);
    }

    const std::vector<float>& data() const { return data_; }
    size_t count() const { return data_.size(); }

    void set_bar_width(float w) { bar_width_ = w; }
    float bar_width() const { return bar_width_; }

    void set_gap(float g) { gap_ = g; }
    float gap() const { return gap_; }

private:
    std::vector<float> data_;
    float bar_width_ = 4.0f;
    float gap_ = 1.0f;
};

} // namespace pulp::render
