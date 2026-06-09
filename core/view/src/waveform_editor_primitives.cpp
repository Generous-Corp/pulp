#include <pulp/view/waveform_editor_primitives.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

namespace pulp::view {
namespace {

int64_t clamp_to_total(int64_t sample, int64_t total_samples) {
    return std::clamp(sample, int64_t{0}, std::max<int64_t>(0, total_samples));
}

WaveformSampleRange clamp_range(WaveformSampleRange range, int64_t total_samples) {
    range = range.normalized();
    range.start = clamp_to_total(range.start, total_samples);
    range.end = clamp_to_total(range.end, total_samples);
    if (range.end < range.start) {
        range.end = range.start;
    }
    return range;
}

void sort_unique_clamped(std::vector<int64_t>& values, int64_t total_samples) {
    for (auto& value : values) {
        value = clamp_to_total(value, total_samples);
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

int source_priority(WaveformSnapSource source) {
    switch (source) {
        case WaveformSnapSource::bounds: return 0;
        case WaveformSnapSource::candidate: return 1;
        case WaveformSnapSource::grid: return 2;
        case WaveformSnapSource::none: return 3;
    }
    return 3;
}

} // namespace

WaveformSampleRange WaveformSampleRange::normalized() const {
    if (end < start) {
        return {end, start};
    }
    return *this;
}

void WaveformViewport::set_total_samples(int64_t samples) {
    total_samples = std::max<int64_t>(0, samples);
    if (total_samples == 0) {
        visible_start = 0;
        visible_length = 0;
        return;
    }

    const auto requested_length = visible_length > 0 ? visible_length : total_samples;
    set_visible_range(visible_start, requested_length);
}

void WaveformViewport::set_bounds(Rect rect) {
    bounds = rect;
}

void WaveformViewport::set_visible_range(int64_t start, int64_t length) {
    total_samples = std::max<int64_t>(0, total_samples);
    if (total_samples == 0) {
        visible_start = 0;
        visible_length = 0;
        return;
    }

    const auto min_length = std::clamp(std::max<int64_t>(1, min_visible_length),
                                       int64_t{1},
                                       total_samples);
    const auto clamped_length = std::clamp(length, min_length, total_samples);
    const auto max_start = std::max<int64_t>(0, total_samples - clamped_length);

    visible_length = clamped_length;
    visible_start = std::clamp(start, int64_t{0}, max_start);
}

void WaveformViewport::zoom_in(double factor) {
    if (empty()) return;
    if (factor <= 0.0) factor = 1.0;

    const auto min_length = std::clamp(std::max<int64_t>(1, min_visible_length),
                                       int64_t{1},
                                       total_samples);
    const auto proposed = static_cast<int64_t>(std::floor(static_cast<double>(visible_length) / factor));
    const auto new_length = std::max(min_length, proposed);
    const auto center = visible_start + visible_length / 2;
    set_visible_range(center - new_length / 2, new_length);
}

void WaveformViewport::zoom_out(double factor) {
    if (empty()) return;
    if (factor <= 0.0) factor = 1.0;

    const auto proposed = static_cast<int64_t>(std::ceil(static_cast<double>(visible_length) * factor));
    const auto new_length = std::min(total_samples, proposed);
    const auto center = visible_start + visible_length / 2;
    set_visible_range(center - new_length / 2, new_length);
}

void WaveformViewport::zoom_to_fit() {
    set_visible_range(0, total_samples);
}

void WaveformViewport::scroll(int64_t delta_samples) {
    set_visible_range(visible_start + delta_samples, visible_length);
}

int64_t WaveformViewport::visible_end() const {
    return std::min(total_samples, visible_start + visible_length);
}

bool WaveformViewport::sample_visible(int64_t sample) const {
    return !empty() && sample >= visible_start && sample < visible_end();
}

int64_t WaveformViewport::clamp_sample(int64_t sample) const {
    return clamp_to_total(sample, total_samples);
}

double WaveformViewport::samples_per_pixel() const {
    if (visible_length <= 0 || bounds.width <= 0.0f) return 0.0;
    return static_cast<double>(visible_length) / static_cast<double>(bounds.width);
}

double WaveformViewport::pixels_per_sample() const {
    if (visible_length <= 0) return 0.0;
    return static_cast<double>(bounds.width) / static_cast<double>(visible_length);
}

float WaveformViewport::sample_to_x(int64_t sample) const {
    if (visible_length <= 0 || bounds.width <= 0.0f) return bounds.x;
    const auto fraction = static_cast<double>(sample - visible_start) /
                          static_cast<double>(visible_length);
    return bounds.x + static_cast<float>(fraction * static_cast<double>(bounds.width));
}

int64_t WaveformViewport::x_to_sample(float x) const {
    if (empty() || bounds.width <= 0.0f) return clamp_sample(visible_start);

    const auto fraction = std::clamp(static_cast<double>(x - bounds.x) /
                                     static_cast<double>(bounds.width),
                                     0.0,
                                     1.0);
    const auto sample = visible_start + static_cast<int64_t>(fraction * static_cast<double>(visible_length));
    return clamp_sample(sample);
}

WaveformRenderPlan build_waveform_render_plan(const WaveformViewport& viewport, int max_spans) {
    WaveformRenderPlan plan;
    plan.viewport = viewport;

    if (viewport.empty() || viewport.bounds.width <= 0.0f) {
        return plan;
    }

    auto span_count = static_cast<int>(std::ceil(viewport.bounds.width));
    if (max_spans > 0) {
        span_count = std::min(span_count, max_spans);
    }
    if (span_count <= 0) {
        return plan;
    }

    plan.spans.reserve(static_cast<size_t>(span_count));
    const auto start = viewport.visible_start;
    const auto length = viewport.visible_length;
    const auto samples_per_span = static_cast<double>(length) / static_cast<double>(span_count);
    const auto pixels_per_span = static_cast<double>(viewport.bounds.width) / static_cast<double>(span_count);

    for (int i = 0; i < span_count; ++i) {
        auto sample_start = start + static_cast<int64_t>(std::floor(static_cast<double>(i) * samples_per_span));
        auto sample_end = start + static_cast<int64_t>(std::floor(static_cast<double>(i + 1) * samples_per_span));
        if (i == span_count - 1) {
            sample_end = viewport.visible_end();
        }
        if (sample_end <= sample_start) {
            sample_end = std::min<int64_t>(viewport.visible_end(), sample_start + 1);
        }

        const auto x = viewport.bounds.x + static_cast<float>(static_cast<double>(i) * pixels_per_span);
        const auto next_x = viewport.bounds.x + static_cast<float>(static_cast<double>(i + 1) * pixels_per_span);
        plan.spans.push_back({viewport.clamp_sample(sample_start),
                              viewport.clamp_sample(sample_end),
                              x,
                              std::max(0.0f, next_x - x)});
    }

    return plan;
}

void WaveformHandleModel::set_total_samples(int64_t samples) {
    total_samples = std::max<int64_t>(0, samples);
    selection = clamp_range(selection, total_samples);
    trim = clamp_range(trim, total_samples);
    loop = clamp_range(loop, total_samples);
    fade_in_end = clamp_to_total(fade_in_end, total_samples);
    fade_out_start = clamp_to_total(fade_out_start, total_samples);
    playhead = clamp_to_total(playhead, total_samples);
    sort_unique_clamped(slice_markers, total_samples);
}

void WaveformHandleModel::set_selection(int64_t start, int64_t end) {
    selection = clamp_range({start, end}, total_samples);
    has_selection = true;
}

void WaveformHandleModel::clear_selection() {
    has_selection = false;
    selection = {};
}

void WaveformHandleModel::set_trim(int64_t start, int64_t end) {
    trim = clamp_range({start, end}, total_samples);
    has_trim = true;
}

void WaveformHandleModel::clear_trim() {
    has_trim = false;
    trim = {};
}

void WaveformHandleModel::set_loop(int64_t start, int64_t end) {
    loop = clamp_range({start, end}, total_samples);
    has_loop = true;
}

void WaveformHandleModel::clear_loop() {
    has_loop = false;
    loop = {};
}

void WaveformHandleModel::set_fade_in(int64_t end_sample) {
    fade_in_end = clamp_to_total(end_sample, total_samples);
    has_fade_in = true;
}

void WaveformHandleModel::clear_fade_in() {
    has_fade_in = false;
    fade_in_end = 0;
}

void WaveformHandleModel::set_fade_out(int64_t start_sample) {
    fade_out_start = clamp_to_total(start_sample, total_samples);
    has_fade_out = true;
}

void WaveformHandleModel::clear_fade_out() {
    has_fade_out = false;
    fade_out_start = 0;
}

void WaveformHandleModel::set_playhead(int64_t sample) {
    playhead = clamp_to_total(sample, total_samples);
    has_playhead = true;
}

void WaveformHandleModel::clear_playhead() {
    has_playhead = false;
    playhead = 0;
}

void WaveformHandleModel::set_slice_markers(std::vector<int64_t> markers) {
    slice_markers = std::move(markers);
    sort_unique_clamped(slice_markers, total_samples);
}

std::vector<WaveformHandle> WaveformHandleModel::handles() const {
    std::vector<WaveformHandle> out;
    out.reserve((has_selection ? 2u : 0u) +
                (has_trim ? 2u : 0u) +
                (has_loop ? 2u : 0u) +
                (has_fade_in ? 1u : 0u) +
                (has_fade_out ? 1u : 0u) +
                (has_playhead ? 1u : 0u) +
                slice_markers.size());
    for_each_handle([&](const WaveformHandle& handle) {
        out.push_back(handle);
    });
    return out;
}

WaveformHitResult hit_test_waveform_handles(const WaveformViewport& viewport,
                                            const WaveformHandleModel& model,
                                            float x,
                                            float tolerance_px) {
    WaveformHitResult result;
    result.sample = viewport.x_to_sample(x);

    const auto tolerance = std::max(0.0f, tolerance_px);
    auto best_distance = std::numeric_limits<float>::max();

    model.for_each_handle([&](const WaveformHandle& handle) {
        if (!handle.enabled || !viewport.sample_visible(handle.sample)) {
            return;
        }

        const auto handle_x = viewport.sample_to_x(handle.sample);
        const auto distance = std::abs(handle_x - x);
        if (distance <= tolerance && distance < best_distance) {
            best_distance = distance;
            result.kind = handle.kind;
            result.id = handle.id;
            result.sample = handle.sample;
            result.distance_px = distance;
        }
    });

    return result;
}

WaveformSnapResult resolve_waveform_snap(int64_t sample,
                                         int64_t total_samples,
                                         const WaveformSnapSettings& settings) {
    total_samples = std::max<int64_t>(0, total_samples);
    const auto clamped_sample = clamp_to_total(sample, total_samples);
    if (total_samples == 0) {
        return {0, false, WaveformSnapSource::none};
    }

    const auto tolerance = std::max<int64_t>(0, settings.tolerance_samples);
    WaveformSnapResult best{clamped_sample, false, WaveformSnapSource::none};
    auto best_distance = std::numeric_limits<int64_t>::max();

    auto consider = [&](int64_t candidate, WaveformSnapSource source) {
        candidate = clamp_to_total(candidate, total_samples);
        const auto distance = std::llabs(candidate - clamped_sample);
        if (distance > tolerance) {
            return;
        }
        if (!best.snapped || distance < best_distance ||
            (distance == best_distance && source_priority(source) < source_priority(best.source))) {
            best = {candidate, true, source};
            best_distance = distance;
        }
    };

    if (settings.snap_to_bounds) {
        consider(0, WaveformSnapSource::bounds);
        consider(total_samples, WaveformSnapSource::bounds);
    }

    for (auto candidate : settings.candidates) {
        consider(candidate, WaveformSnapSource::candidate);
    }

    if (settings.grid_interval_samples > 0) {
        const auto grid = settings.grid_interval_samples;
        const auto quotient = (clamped_sample + grid / 2) / grid;
        consider(quotient * grid, WaveformSnapSource::grid);
    }

    return best;
}

WaveformPlayheadOverlay build_waveform_playhead_overlay(const WaveformViewport& viewport,
                                                        int64_t playhead_sample) {
    WaveformPlayheadOverlay overlay;
    overlay.sample = viewport.clamp_sample(playhead_sample);
    overlay.visible = viewport.sample_visible(overlay.sample);
    overlay.x = overlay.visible ? viewport.sample_to_x(overlay.sample) : viewport.bounds.x;
    return overlay;
}

void WaveformEditorTransaction::begin(WaveformEditorOperationKind operation,
                                      int64_t anchor_sample,
                                      WaveformSampleRange initial_selection,
                                      int64_t initial_playhead_sample) {
    active_ = operation != WaveformEditorOperationKind::none;
    operation_ = active_ ? operation : WaveformEditorOperationKind::none;
    anchor_sample_ = anchor_sample;
    initial_selection_ = initial_selection.normalized();
    initial_playhead_sample_ = initial_playhead_sample;
}

WaveformEditorTransactionResult WaveformEditorTransaction::update(int64_t sample) const {
    return result_for_sample(sample, false, false);
}

WaveformEditorTransactionResult WaveformEditorTransaction::commit(int64_t sample) {
    auto result = result_for_sample(sample, true, false);
    active_ = false;
    operation_ = WaveformEditorOperationKind::none;
    return result;
}

WaveformEditorTransactionResult WaveformEditorTransaction::cancel() {
    auto result = result_for_sample(anchor_sample_, false, true);
    result.selection = initial_selection_;
    result.playhead_sample = initial_playhead_sample_;
    active_ = false;
    operation_ = WaveformEditorOperationKind::none;
    return result;
}

WaveformEditorTransactionResult WaveformEditorTransaction::result_for_sample(int64_t sample,
                                                                             bool committed,
                                                                             bool cancelled) const {
    WaveformEditorTransactionResult result;
    result.operation = operation_;
    result.active = active_;
    result.committed = committed && active_;
    result.cancelled = cancelled && active_;
    result.selection = initial_selection_;
    result.playhead_sample = initial_playhead_sample_;

    if (!active_) {
        return result;
    }

    switch (operation_) {
        case WaveformEditorOperationKind::create_selection:
            result.selection = WaveformSampleRange{anchor_sample_, sample}.normalized();
            break;
        case WaveformEditorOperationKind::extend_selection:
            result.selection = WaveformSampleRange{initial_selection_.start, sample}.normalized();
            break;
        case WaveformEditorOperationKind::drag_selection_start:
            result.selection = WaveformSampleRange{sample, initial_selection_.end}.normalized();
            break;
        case WaveformEditorOperationKind::drag_selection_end:
            result.selection = WaveformSampleRange{initial_selection_.start, sample}.normalized();
            break;
        case WaveformEditorOperationKind::move_playhead:
            result.playhead_sample = sample;
            break;
        case WaveformEditorOperationKind::none:
            break;
    }

    return result;
}

} // namespace pulp::view
