#include <pulp/view/eq_curve_view.hpp>
#include <pulp/view/theme_contrast.hpp>
#include <cmath>
#include <algorithm>

namespace pulp::view {

EqCurveView::EqCurveView() {
    set_focusable(true);
}

void EqCurveView::set_bands(std::vector<Band> bands) {
    bands_ = std::move(bands);
}

void EqCurveView::set_band(size_t index, const Band& band) {
    if (index < bands_.size()) bands_[index] = band;
}

void EqCurveView::add_band(Band band) {
    bands_.push_back(std::move(band));
}

void EqCurveView::remove_band(size_t index) {
    if (index < bands_.size())
        bands_.erase(bands_.begin() + static_cast<ptrdiff_t>(index));
}

void EqCurveView::set_frequency_range(float min_hz, float max_hz) {
    min_freq_ = std::max(1.0f, min_hz);
    max_freq_ = std::max(min_freq_ + 1.0f, max_hz);
}

void EqCurveView::set_gain_range(float min_db, float max_db) {
    min_db_ = min_db;
    max_db_ = std::max(min_db_ + 1.0f, max_db);
}

void EqCurveView::set_spectrum(const float* magnitudes_db, size_t bin_count) {
    spectrum_.assign(magnitudes_db, magnitudes_db + bin_count);
}

void EqCurveView::clear_spectrum() {
    spectrum_.clear();
}

float EqCurveView::freq_to_x(float freq) const {
    auto b = local_bounds();
    float log_min = std::log10(min_freq_);
    float log_max = std::log10(max_freq_);
    float log_freq = std::log10(std::max(1.0f, freq));
    return b.x + b.width * (log_freq - log_min) / (log_max - log_min);
}

float EqCurveView::x_to_freq(float x) const {
    auto b = local_bounds();
    float log_min = std::log10(min_freq_);
    float log_max = std::log10(max_freq_);
    float t = (x - b.x) / b.width;
    return std::pow(10.0f, log_min + t * (log_max - log_min));
}

float EqCurveView::db_to_y(float db) const {
    auto b = local_bounds();
    float t = (db - max_db_) / (min_db_ - max_db_);
    return b.y + t * b.height;
}

float EqCurveView::y_to_db(float y) const {
    auto b = local_bounds();
    float t = (y - b.y) / b.height;
    return max_db_ + t * (min_db_ - max_db_);
}

int EqCurveView::hit_test_band(Point pos) const {
    constexpr float hit_radius = 10.0f;
    for (size_t i = 0; i < bands_.size(); ++i) {
        float bx = freq_to_x(bands_[i].frequency);
        float by = db_to_y(bands_[i].gain_db);
        float dx = pos.x - bx;
        float dy = pos.y - by;
        if (dx * dx + dy * dy <= hit_radius * hit_radius)
            return static_cast<int>(i);
    }
    return -1;
}

// Simplified biquad magnitude response for visualization
float EqCurveView::compute_magnitude_at(float freq) const {
    float total_db = 0.0f;
    for (auto& band : bands_) {
        if (!band.enabled) continue;
        // Simple bell/peak approximation for visualization
        float f_ratio = std::log2(freq / band.frequency);
        float bandwidth = 1.0f / band.q;
        float response = band.gain_db * std::exp(-0.5f * (f_ratio * f_ratio) / (bandwidth * bandwidth));
        total_db += response;
    }
    return total_db;
}

void EqCurveView::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    auto bg = resolve_color("bg.surface", Color::rgba8(30, 30, 40));
    auto grid_color = resolve_color("waveform.grid", Color::rgba8(60, 60, 80));
    auto curve_color = resolve_color("accent.primary", Color::rgba8(100, 180, 255));

    // Background
    canvas.set_fill_color(bg);
    canvas.fill_rect(b.x, b.y, b.width, b.height);

    // Grid lines
    if (show_grid_) {
        canvas.set_stroke_color(grid_color);
        canvas.set_line_width(0.5f);
        for (float f : {100.0f, 1000.0f, 10000.0f}) {
            float x = freq_to_x(f);
            canvas.stroke_line(x, b.y, x, b.y + b.height);
        }
        for (float db : {-24.0f, -12.0f, 0.0f, 12.0f, 24.0f}) {
            if (db >= min_db_ && db <= max_db_) {
                float y = db_to_y(db);
                canvas.set_line_width(db == 0.0f ? 1.0f : 0.5f);
                canvas.stroke_line(b.x, y, b.x + b.width, y);
            }
        }
    }

    // Spectrum analyzer overlay
    if (!spectrum_.empty()) {
        canvas.set_fill_color(with_alpha(curve_color, 40));
        float bin_width = (max_freq_ - min_freq_) / static_cast<float>(spectrum_.size());
        for (size_t i = 0; i < spectrum_.size(); ++i) {
            float freq = min_freq_ + static_cast<float>(i) * bin_width;
            float x = freq_to_x(freq);
            float y = db_to_y(spectrum_[i]);
            float baseline = db_to_y(min_db_);
            canvas.fill_rect(x, y, 2.0f, baseline - y);
        }
    }

    // Combined EQ curve
    if (!bands_.empty()) {
        canvas.set_stroke_color(curve_color);
        canvas.set_line_width(2.0f);
        float step = b.width / 200.0f;
        float prev_x = b.x;
        float prev_y = db_to_y(compute_magnitude_at(x_to_freq(b.x)));

        for (float px = b.x + step; px <= b.x + b.width; px += step) {
            float freq = x_to_freq(px);
            float db = compute_magnitude_at(freq);
            float y = db_to_y(db);
            canvas.stroke_line(prev_x, prev_y, px, y);
            prev_x = px;
            prev_y = y;
        }
    }

    // Band handles
    for (size_t i = 0; i < bands_.size(); ++i) {
        auto& band = bands_[i];
        if (!band.enabled) continue;

        float hx = freq_to_x(band.frequency);
        float hy = db_to_y(band.gain_db);
        float radius = (static_cast<int>(i) == selected_band_) ? 8.0f : 6.0f;

        auto handle_color = band.color.a > 0 ? band.color : curve_color;
        canvas.set_fill_color(handle_color);
        canvas.fill_circle(hx, hy, radius);
        canvas.set_stroke_color(Color::rgba8(255, 255, 255));
        canvas.set_line_width(1.5f);
        canvas.stroke_circle(hx, hy, radius);
    }
}

void EqCurveView::on_mouse_down(Point pos) {
    int hit = hit_test_band(pos);
    selected_band_ = hit;
    if (hit >= 0) {
        dragging_band_ = hit;
        drag_start_ = pos;
        if (on_band_selected) on_band_selected(static_cast<size_t>(hit));
    }
}

void EqCurveView::on_mouse_drag(Point pos) {
    if (dragging_band_ >= 0 && dragging_band_ < static_cast<int>(bands_.size())) {
        auto& band = bands_[static_cast<size_t>(dragging_band_)];
        band.frequency = std::clamp(x_to_freq(pos.x), min_freq_, max_freq_);
        band.gain_db = std::clamp(y_to_db(pos.y), min_db_, max_db_);
        if (on_band_changed) on_band_changed(static_cast<size_t>(dragging_band_), band);
    }
}

void EqCurveView::on_mouse_event(const MouseEvent& event) {
    View::on_mouse_event(event);
}

void EqCurveView::on_mouse_up(Point pos) {
    (void)pos;
    dragging_band_ = -1;
}

} // namespace pulp::view
