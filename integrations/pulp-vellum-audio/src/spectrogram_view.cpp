#include <pulp/vellum_audio/spectrogram_view.hpp>

namespace pulp::vellum_audio {

void SpectrogramView::configure(int history_columns, int freq_rows, pulp::signal::ColorRamp ramp,
                                float min_db, float max_db) {
    buffer_.configure(history_columns, freq_rows);
    mapper_.set_ramp(ramp);
    min_db_ = min_db;
    max_db_ = max_db;
    configured_ = true;
    request_repaint();
}

void SpectrogramView::push_spectrum(const float* magnitudes_db, int num_bins) {
    if (!configured_) {
        // Auto-configure with sensible defaults
        configure(256, num_bins);
    }
    buffer_.push_column(magnitudes_db, num_bins, mapper_, min_db_, max_db_);
    request_repaint();
}

void SpectrogramView::paint(::vellum::canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Background
    auto bg = resolve_color("bg.surface", ::vellum::canvas::Color::rgba8(10, 10, 15));
    canvas.set_fill_color(bg);
    canvas.fill_rect(0, 0, b.width, b.height);

    if (!configured_ || buffer_.frames_written() == 0)
        return;

    int buf_w = buffer_.width();
    int buf_h = buffer_.height();
    float px_w = b.width / buf_w;
    float px_h = b.height / buf_h;

    int start_col = buffer_.write_column(); // oldest column

    for (int col = 0; col < buf_w; ++col) {
        int src_col = (start_col + col) % buf_w;
        float x = col * px_w;

        for (int row = 0; row < buf_h; ++row) {
            auto c = buffer_.pixels()[row * buf_w + src_col];
            // Flip vertically: low freq at bottom
            float y = b.height - (row + 1) * px_h;
            canvas.set_fill_color(::vellum::canvas::Color::rgba8(c.r, c.g, c.b, c.a));
            canvas.fill_rect(x, y, px_w + 0.5f, px_h + 0.5f);
        }
    }
}

} // namespace pulp::vellum_audio
