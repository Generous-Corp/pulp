#include <pulp/vellum_audio/waveform_view.hpp>

#include <cctype>
#include <cmath>
#include <string>
#include <utility>

namespace pulp::vellum_audio {

size_t WaveformView::find_trigger_index(const float* samples, size_t count, TriggerMode mode) {
    if (mode == TriggerMode::free_run || count < 2)
        return 0;
    const bool want_rising = (mode == TriggerMode::rising_zero);
    for (size_t i = 1; i < count; ++i) {
        float prev = samples[i - 1];
        float curr = samples[i];
        if (want_rising) {
            if (prev <= 0.0f && curr > 0.0f)
                return i;
        } else {
            if (prev >= 0.0f && curr < 0.0f)
                return i;
        }
    }
    return 0;
}

void WaveformView::inspector_facts(std::vector<InspectorFacts::Fact>& out) const {
    out.push_back({"samples", "Samples", static_cast<double>(sample_count()), true, false});
}

void WaveformView::set_preview_shape(std::string_view shape) {
    std::string normalized(shape);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (normalized == "saw" || normalized == "sawtooth")
        preview_shape_ = PreviewShape::saw;
    else if (normalized == "sine" || normalized == "sin")
        preview_shape_ = PreviewShape::sine;
    else if (normalized == "square" || normalized == "squ")
        preview_shape_ = PreviewShape::square;
    else if (normalized == "tri" || normalized == "triangle")
        preview_shape_ = PreviewShape::triangle;
    else
        preview_shape_ = PreviewShape::none;
    request_repaint();
}

void WaveformView::apply_trigger() {
    if (trigger_mode_ == TriggerMode::free_run || samples_.size() < 2)
        return;
    size_t idx = find_trigger_index(samples_.data(), samples_.size(), trigger_mode_);
    if (idx == 0)
        return; // no crossing — leave as-is
    std::rotate(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(idx),
                samples_.end());
}

void WaveformView::set_data(const float* samples, size_t count) {
    samples_.assign(samples, samples + count);
    apply_trigger();
    thumbnail_owner_.reset();
    thumbnail_ = nullptr; // raw samples win over any cached thumbnail
    request_repaint();
}

void WaveformView::set_data(std::vector<float> samples) {
    samples_ = std::move(samples);
    apply_trigger();
    thumbnail_owner_.reset();
    thumbnail_ = nullptr;
    request_repaint();
}

void WaveformView::set_thumbnail(std::shared_ptr<const pulp::audio::WaveformOverview> thumb,
                                 std::uint32_t channel) {
    thumbnail_owner_ = std::move(thumb);
    thumbnail_ = thumbnail_owner_.get();
    thumbnail_channel_ = channel;
    // A live thumbnail shadows any prior raw sample buffer at paint time;
    // we drop the samples cache so memory doesn't double up.
    samples_.clear();
    request_repaint();
}

void WaveformView::set_thumbnail_borrowed(const pulp::audio::WaveformOverview* thumb,
                                          std::uint32_t channel) {
    thumbnail_owner_.reset();
    thumbnail_ = thumb;
    thumbnail_channel_ = channel;
    // A live thumbnail shadows any prior raw sample buffer at paint time;
    // we drop the samples cache so memory doesn't double up.
    samples_.clear();
    request_repaint();
}

void WaveformView::clear_thumbnail() {
    thumbnail_owner_.reset();
    thumbnail_ = nullptr;
    request_repaint();
}

void WaveformView::paint(::vellum::canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Background
    auto bg = resolve_color("bg.surface", ::vellum::canvas::Color::rgba8(30, 30, 40));
    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(0, 0, b.width, b.height, 2.0f);

    auto wave_color = resolve_color("waveform.line", ::vellum::canvas::Color::rgba8(100, 180, 250));
    auto fill_color =
        resolve_color("waveform.fill", ::vellum::canvas::Color::rgba(wave_color.r, wave_color.g, wave_color.b,
                                                           56.0f / 255.0f));

    // Thumbnail path: render decimated min/max peaks straight from the cached
    // thumbnail without re-decoding audio.
    if (thumbnail_ != nullptr && !thumbnail_->empty()) {
        // Center line
        auto center_color = resolve_color("waveform.grid", ::vellum::canvas::Color::rgba8(50, 50, 60));
        canvas.set_stroke_color(center_color);
        canvas.set_line_width(0.5f);
        float cy = b.height * 0.5f;
        canvas.stroke_line(0, cy, b.width, cy);

        // Render one (min, max) pair per pixel column. Scratch storage is
        // retained on the view so repainting a stable-size editor does not
        // allocate every frame; cap at 4096 px to stay bounded.
        const std::size_t target_peaks =
            std::min<std::size_t>(static_cast<std::size_t>(std::max(1.0f, b.width)), 4096u);
        thumbnail_min_max_.assign(target_peaks * 2, 0.0f);
        const std::size_t produced = thumbnail_->render_min_max(
            thumbnail_channel_, static_cast<std::uint32_t>(target_peaks), thumbnail_min_max_.data());
        if (produced == 0)
            return;

        // Drive the GPU waveform shader with the per-column max so the
        // line/fill remain consistent with the live-sample path. We render
        // the absolute envelope (max(|min|, |max|)) — perceptually closest
        // to traditional audio-editor waveform displays.
        thumbnail_envelope_.resize(produced);
        for (std::size_t i = 0; i < produced; ++i) {
            const float lo = thumbnail_min_max_[i * 2 + 0];
            const float hi = thumbnail_min_max_[i * 2 + 1];
            const float amp = std::max(std::abs(lo), std::abs(hi));
            // Signed envelope: top half positive, bottom half negative would
            // require two passes through the shader. The shader already
            // handles symmetric ±amp, so we feed it max amplitude per column.
            thumbnail_envelope_[i] = hi >= -lo ? amp : -amp;
        }

        ::vellum::canvas::Canvas::WaveformStyle style;
        style.line_color = wave_color;
        style.fill_color = fill_color;
        style.line_thickness = 2.0f;
        style.show_fill = true;
        style.fill_center = 0.5f;

        canvas.draw_waveform(thumbnail_envelope_.data(), thumbnail_envelope_.size(), 0, 0, b.width,
                             b.height, style);
        return;
    }

    if (samples_.empty() && preview_shape_ != PreviewShape::none) {
        auto center_color = resolve_color("waveform.grid", ::vellum::canvas::Color::rgba8(50, 50, 60));
        canvas.set_stroke_color(center_color);
        canvas.set_line_width(0.5f);
        const float cy = b.height * 0.5f;
        canvas.stroke_line(0, cy, b.width, cy);

        canvas.set_stroke_color(wave_color.with_alpha(wave_color.a * 0.85f));
        canvas.set_line_width(1.2f);
        canvas.begin_path();
        switch (preview_shape_) {
        case PreviewShape::saw:
            canvas.move_to(0.0f, b.height * 0.8f);
            canvas.line_to(b.width * 0.25f, b.height * 0.2f);
            canvas.line_to(b.width * 0.25f, b.height * 0.8f);
            canvas.line_to(b.width * 0.5f, b.height * 0.2f);
            canvas.line_to(b.width * 0.5f, b.height * 0.8f);
            canvas.line_to(b.width * 0.75f, b.height * 0.2f);
            canvas.line_to(b.width * 0.75f, b.height * 0.8f);
            canvas.line_to(b.width, b.height * 0.2f);
            break;
        case PreviewShape::sine:
            canvas.move_to(0.0f, b.height * 0.5f);
            canvas.quad_to(b.width * 0.12f, b.height * 0.1f, b.width * 0.25f, b.height * 0.5f);
            canvas.quad_to(b.width * 0.38f, b.height * 0.9f, b.width * 0.5f, b.height * 0.5f);
            canvas.quad_to(b.width * 0.62f, b.height * 0.1f, b.width * 0.75f, b.height * 0.5f);
            canvas.quad_to(b.width * 0.88f, b.height * 0.9f, b.width, b.height * 0.5f);
            break;
        case PreviewShape::square:
            canvas.move_to(0.0f, b.height * 0.2f);
            canvas.line_to(b.width * 0.25f, b.height * 0.2f);
            canvas.line_to(b.width * 0.25f, b.height * 0.8f);
            canvas.line_to(b.width * 0.5f, b.height * 0.8f);
            canvas.line_to(b.width * 0.5f, b.height * 0.2f);
            canvas.line_to(b.width * 0.75f, b.height * 0.2f);
            canvas.line_to(b.width * 0.75f, b.height * 0.8f);
            canvas.line_to(b.width, b.height * 0.8f);
            break;
        case PreviewShape::triangle:
            canvas.move_to(0.0f, b.height * 0.5f);
            canvas.line_to(b.width * 0.125f, b.height * 0.15f);
            canvas.line_to(b.width * 0.375f, b.height * 0.85f);
            canvas.line_to(b.width * 0.625f, b.height * 0.15f);
            canvas.line_to(b.width * 0.875f, b.height * 0.85f);
            canvas.line_to(b.width, b.height * 0.5f);
            break;
        case PreviewShape::none:
            break;
        }
        canvas.stroke_current_path();
        return;
    }

    if (samples_.empty())
        return;

    // Center line
    auto center_color = resolve_color("waveform.grid", ::vellum::canvas::Color::rgba8(50, 50, 60));
    canvas.set_stroke_color(center_color);
    canvas.set_line_width(0.5f);
    float cy = b.height * 0.5f;
    canvas.stroke_line(0, cy, b.width, cy);

    // Mirrored bar waveform — one vertical bar per column at the column's peak
    // amplitude, mirrored about the center line (the classic audio-editor
    // thumbnail). Drawn with fill_rect so it renders on the CPU raster path,
    // not only the GPU waveform shader.
    const int cols = std::max(1, std::min<int>(static_cast<int>(b.width), 512));
    const float colw = b.width / static_cast<float>(cols);
    const std::size_t n = samples_.size();
    const float half_h = b.height * 0.5f - 1.0f;
    canvas.set_fill_color(wave_color);
    for (int c = 0; c < cols; ++c) {
        std::size_t i0 = static_cast<std::size_t>(static_cast<float>(c) / cols * n);
        std::size_t i1 = static_cast<std::size_t>(static_cast<float>(c + 1) / cols * n);
        float amp = 0.0f;
        for (std::size_t i = i0; i < i1 && i < n; ++i)
            amp = std::max(amp, std::abs(samples_[i]));
        float half = std::clamp(amp, 0.0f, 1.0f) * half_h;
        float bw = std::max(1.0f, colw - 1.0f);
        canvas.fill_rect(static_cast<float>(c) * colw, cy - half, bw, half * 2.0f + 1.0f);
    }
}

// ── WaveformOverviewView ────────────────────────────────────────────────────

void WaveformOverviewView::set_overview(const pulp::audio::WaveformOverview* overview,
                                        std::uint32_t channel) {
    overview_ = overview;
    channel_ = channel;
    view_all();
}

void WaveformOverviewView::clear_overview() {
    overview_ = nullptr;
    vp_start_ = 0;
    vp_end_ = 0;
    request_repaint();
}

void WaveformOverviewView::set_viewport(std::uint64_t start_frame, std::uint64_t end_frame) {
    const std::uint64_t total = source_frames();
    vp_start_ = std::min(start_frame, total);
    vp_end_ = std::min(end_frame, total);
    if (vp_end_ < vp_start_)
        vp_end_ = vp_start_;
    request_repaint();
}

void WaveformOverviewView::view_all() {
    vp_start_ = 0;
    vp_end_ = source_frames();
    request_repaint();
}

bool WaveformOverviewView::column_peak(std::uint64_t f0, std::uint64_t f1, float& out_min,
                                       float& out_max) const {
    if (!has_overview() || f1 <= f0)
        return false;
    const auto& lvl = overview_->level(0);
    if (lvl.samples_per_peak == 0 || lvl.peaks.empty())
        return false;

    std::uint64_t p0 = f0 / lvl.samples_per_peak;
    std::uint64_t p1 = (f1 + lvl.samples_per_peak - 1) / lvl.samples_per_peak;
    if (p1 <= p0)
        p1 = p0 + 1;
    if (p0 >= lvl.peaks_per_channel)
        return false;
    p1 = std::min<std::uint64_t>(p1, lvl.peaks_per_channel);

    float lo = 1.0f, hi = -1.0f;
    const auto fold = [&](std::uint32_t ch) {
        if (ch >= lvl.peaks.size())
            return;
        const auto& col = lvl.peaks[ch];
        for (std::uint64_t i = p0; i < p1 && i < col.size(); ++i) {
            lo = std::min(lo, col[i].min());
            hi = std::max(hi, col[i].max());
        }
    };
    if (channel_ == pulp::audio::WaveformOverview::kAllChannels) {
        for (std::uint32_t ch = 0; ch < lvl.peaks.size(); ++ch)
            fold(ch);
    } else {
        fold(channel_);
    }
    if (hi < lo)
        return false; // no peaks landed in the window
    out_min = lo;
    out_max = hi;
    return true;
}

void WaveformOverviewView::paint(::vellum::canvas::Canvas& canvas) {
    const auto b = local_bounds();

    canvas.set_fill_color(bg_color_);
    canvas.fill_rect(0.0f, 0.0f, b.width, b.height);

    if (!has_overview() || vp_end_ <= vp_start_ || b.width < 1.0f)
        return;

    const float cy = b.height * 0.5f;
    const float half_h = b.height * 0.5f;
    canvas.set_stroke_color(line_color_);
    canvas.set_line_width(1.0f);

    const std::uint64_t span = vp_end_ - vp_start_;
    const int cols = static_cast<int>(b.width);
    for (int px = 0; px < cols; ++px) {
        const std::uint64_t cf0 =
            vp_start_ + span * static_cast<std::uint64_t>(px) / static_cast<std::uint64_t>(cols);
        std::uint64_t cf1 = vp_start_ + span * static_cast<std::uint64_t>(px + 1) /
                                            static_cast<std::uint64_t>(cols);
        if (cf1 <= cf0)
            cf1 = cf0 + 1;

        float lo = 0.0f, hi = 0.0f;
        if (!column_peak(cf0, cf1, lo, hi))
            continue;

        const float x = static_cast<float>(px) + 0.5f;
        const float y_top = cy - hi * half_h;
        float y_bot = cy - lo * half_h;
        if (y_bot - y_top < 1.0f)
            y_bot = y_top + 1.0f; // keep 1px minimum
        canvas.stroke_line(x, y_top, x, y_bot);
    }
}

} // namespace pulp::vellum_audio
