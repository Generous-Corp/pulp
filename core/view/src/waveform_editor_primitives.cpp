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

bool scalar_handle_id_valid(int id) {
    return id == -1 || id == 0;
}

int normalized_surface_edit_id(WaveformHandleKind kind, int id) {
    return kind == WaveformHandleKind::slice_marker ? id : 0;
}

void set_selection_or_clear(WaveformHandleModel& model, int64_t start, int64_t end) {
    const auto range = clamp_range({start, end}, model.total_samples);
    if (range.empty()) {
        model.clear_selection();
    } else {
        model.set_selection(range.start, range.end);
    }
}

bool edits_selection(WaveformHandleKind kind) {
    return kind == WaveformHandleKind::none ||
           kind == WaveformHandleKind::selection_start ||
           kind == WaveformHandleKind::selection_end;
}

bool edits_trim(WaveformHandleKind kind) {
    return kind == WaveformHandleKind::trim_start || kind == WaveformHandleKind::trim_end;
}

bool edits_loop(WaveformHandleKind kind) {
    return kind == WaveformHandleKind::loop_start || kind == WaveformHandleKind::loop_end;
}

void merge_unrelated_live_handle_state(WaveformHandleModel& base,
                                       const WaveformHandleModel& live,
                                       WaveformHandleKind edit_kind) {
    base.total_samples = live.total_samples;
    if (!edits_selection(edit_kind)) {
        base.has_selection = live.has_selection;
        base.selection = live.selection;
    }
    if (!edits_trim(edit_kind)) {
        base.has_trim = live.has_trim;
        base.trim = live.trim;
    }
    if (edit_kind != WaveformHandleKind::fade_in) {
        base.has_fade_in = live.has_fade_in;
        base.fade_in_end = live.fade_in_end;
    }
    if (edit_kind != WaveformHandleKind::fade_out) {
        base.has_fade_out = live.has_fade_out;
        base.fade_out_start = live.fade_out_start;
    }
    if (!edits_loop(edit_kind)) {
        base.has_loop = live.has_loop;
        base.loop = live.loop;
    }
    if (edit_kind != WaveformHandleKind::slice_marker) {
        base.slice_markers = live.slice_markers;
    }
    if (edit_kind != WaveformHandleKind::playhead) {
        base.has_playhead = live.has_playhead;
        base.playhead = live.playhead;
    }
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

bool WaveformViewport::sample_point_visible(int64_t sample) const {
    return !empty() && sample >= visible_start && sample <= visible_end();
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
        if (!handle.enabled || !viewport.sample_point_visible(handle.sample)) {
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
        if (candidate < 0 || candidate > total_samples) {
            return;
        }
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
        const auto lower = (clamped_sample / grid) * grid;
        consider(lower, WaveformSnapSource::grid);
        if (lower <= total_samples - grid) {
            consider(lower + grid, WaveformSnapSource::grid);
        }
    }

    return best;
}

WaveformPlayheadOverlay build_waveform_playhead_overlay(const WaveformViewport& viewport,
                                                        int64_t playhead_sample) {
    WaveformPlayheadOverlay overlay;
    overlay.sample = viewport.clamp_sample(playhead_sample);
    overlay.visible = viewport.sample_point_visible(overlay.sample);
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

void WaveformEditorSurface::set_total_samples(int64_t samples) {
    viewport_.set_total_samples(samples);
    handles_.set_total_samples(viewport_.total_samples);
    if (handles_.has_selection && handles_.selection.empty()) {
        handles_.clear_selection();
    }
    if (edit_active_) {
        edit_initial_handles_.set_total_samples(viewport_.total_samples);
        if (edit_initial_handles_.has_selection && edit_initial_handles_.selection.empty()) {
            edit_initial_handles_.clear_selection();
        }
        edit_anchor_sample_ = viewport_.clamp_sample(edit_anchor_sample_);
    }
}

void WaveformEditorSurface::set_bounds(Rect bounds) {
    viewport_.set_bounds(bounds);
}

void WaveformEditorSurface::set_visible_range(int64_t start, int64_t length) {
    viewport_.set_visible_range(start, length);
}

void WaveformEditorSurface::zoom_to_fit() {
    viewport_.zoom_to_fit();
}

void WaveformEditorSurface::scroll(int64_t delta_samples) {
    viewport_.scroll(delta_samples);
}

void WaveformEditorSurface::set_snap_settings(WaveformSnapSettings settings) {
    snap_settings_ = std::move(settings);
}

void WaveformEditorSurface::set_selection(int64_t start, int64_t end) {
    set_selection_or_clear(handles_, start, end);
}

void WaveformEditorSurface::clear_selection() {
    handles_.clear_selection();
}

void WaveformEditorSurface::set_trim(int64_t start, int64_t end) {
    handles_.set_trim(start, end);
}

void WaveformEditorSurface::clear_trim() {
    handles_.clear_trim();
}

void WaveformEditorSurface::set_loop(int64_t start, int64_t end) {
    handles_.set_loop(start, end);
}

void WaveformEditorSurface::clear_loop() {
    handles_.clear_loop();
}

void WaveformEditorSurface::set_fade_in(int64_t end_sample) {
    handles_.set_fade_in(end_sample);
}

void WaveformEditorSurface::clear_fade_in() {
    handles_.clear_fade_in();
}

void WaveformEditorSurface::set_fade_out(int64_t start_sample) {
    handles_.set_fade_out(start_sample);
}

void WaveformEditorSurface::clear_fade_out() {
    handles_.clear_fade_out();
}

void WaveformEditorSurface::set_playhead(int64_t sample) {
    handles_.set_playhead(sample);
}

void WaveformEditorSurface::clear_playhead() {
    handles_.clear_playhead();
}

void WaveformEditorSurface::set_slice_markers(std::vector<int64_t> markers) {
    handles_.set_slice_markers(std::move(markers));
}

WaveformEditorSurfaceSnapshot WaveformEditorSurface::snapshot() const {
    WaveformEditorSurfaceSnapshot out;
    out.viewport = viewport_;
    out.handles = handles_;
    out.playhead = handles_.has_playhead
        ? build_waveform_playhead_overlay(viewport_, handles_.playhead)
        : WaveformPlayheadOverlay{};
    out.edit_active = edit_active_;
    out.edit_kind = edit_kind_;
    out.edit_id = edit_id_;
    return out;
}

WaveformRenderPlan WaveformEditorSurface::render_plan(int max_spans) const {
    return build_waveform_render_plan(viewport_, max_spans);
}

WaveformHitResult WaveformEditorSurface::hit_test(float x, float tolerance_px) const {
    return hit_test_waveform_handles(viewport_, handles_, x, tolerance_px);
}

bool WaveformEditorSurface::begin_selection_edit(int64_t anchor_sample) {
    if (viewport_.total_samples <= 0) return false;
    if (edit_active_) return false;
    edit_initial_handles_ = handles_;
    edit_active_ = true;
    edit_kind_ = WaveformHandleKind::none;
    edit_id_ = -1;
    edit_anchor_sample_ = viewport_.clamp_sample(anchor_sample);
    return true;
}

bool WaveformEditorSurface::begin_handle_edit(WaveformHandleKind kind, int id) {
    if (viewport_.total_samples <= 0) return false;
    if (edit_active_) return false;
    if (!can_edit(kind, id)) return false;
    const auto normalized_id = normalized_surface_edit_id(kind, id);
    edit_initial_handles_ = handles_;
    edit_active_ = true;
    edit_kind_ = kind;
    edit_id_ = normalized_id;

    auto anchor = int64_t{0};
    edit_initial_handles_.for_each_handle([&](const WaveformHandle& handle) {
        if (handle.kind == kind && handle.id == normalized_id) {
            anchor = handle.sample;
        }
    });
    edit_anchor_sample_ = viewport_.clamp_sample(anchor);
    return true;
}

bool WaveformEditorSurface::begin_handle_edit(const WaveformHitResult& hit) {
    if (!hit) return false;
    return begin_handle_edit(hit.kind, hit.id);
}

WaveformEditorSurfaceEditResult WaveformEditorSurface::update_edit(int64_t sample) {
    auto result = edit_result_for_sample(sample, false, false);
    if (result.active) handles_ = result.handles;
    return result;
}

WaveformEditorSurfaceEditResult WaveformEditorSurface::commit_edit(int64_t sample) {
    auto result = edit_result_for_sample(sample, true, false);
    if (result.active) handles_ = result.handles;
    edit_active_ = false;
    edit_kind_ = WaveformHandleKind::none;
    edit_id_ = -1;
    return result;
}

WaveformEditorSurfaceEditResult WaveformEditorSurface::cancel_edit() {
    auto result = edit_result_for_sample(edit_anchor_sample_, false, true);
    if (result.active) {
        handles_ = result.handles;
    }
    edit_active_ = false;
    edit_kind_ = WaveformHandleKind::none;
    edit_id_ = -1;
    return result;
}

bool WaveformEditorSurface::can_edit(WaveformHandleKind kind, int id) const {
    switch (kind) {
        case WaveformHandleKind::selection_start:
        case WaveformHandleKind::selection_end:
            return handles_.has_selection && scalar_handle_id_valid(id);
        case WaveformHandleKind::trim_start:
        case WaveformHandleKind::trim_end:
            return handles_.has_trim && scalar_handle_id_valid(id);
        case WaveformHandleKind::fade_in:
            return handles_.has_fade_in && scalar_handle_id_valid(id);
        case WaveformHandleKind::fade_out:
            return handles_.has_fade_out && scalar_handle_id_valid(id);
        case WaveformHandleKind::loop_start:
        case WaveformHandleKind::loop_end:
            return handles_.has_loop && scalar_handle_id_valid(id);
        case WaveformHandleKind::slice_marker:
            return id >= 0 && static_cast<std::size_t>(id) < handles_.slice_markers.size();
        case WaveformHandleKind::playhead:
            return handles_.has_playhead && scalar_handle_id_valid(id);
        case WaveformHandleKind::none:
            return false;
    }
    return false;
}

WaveformEditorSurfaceEditResult WaveformEditorSurface::edit_result_for_sample(int64_t sample,
                                                                               bool committed,
                                                                               bool cancelled) const {
    WaveformEditorSurfaceEditResult result;
    result.kind = edit_kind_;
    result.id = edit_id_;
    result.active = edit_active_;
    result.committed = committed && edit_active_;
    result.cancelled = cancelled && edit_active_;
    result.handles = handles_;

    if (!edit_active_) {
        result.snap = resolve_waveform_snap(sample, viewport_.total_samples, snap_settings_);
        return result;
    }

    result.snap = resolve_waveform_snap(sample, viewport_.total_samples, snap_settings_);
    result.handles = edit_initial_handles_;
    merge_unrelated_live_handle_state(result.handles, handles_, edit_kind_);
    if (!cancelled) {
        apply_edit(result.handles, edit_kind_, edit_id_, edit_anchor_sample_, result.snap.sample);
    }
    return result;
}

void WaveformEditorSurface::apply_edit(WaveformHandleModel& model,
                                       WaveformHandleKind kind,
                                       int id,
                                       int64_t anchor_sample,
                                       int64_t sample) {
    switch (kind) {
        case WaveformHandleKind::none:
            set_selection_or_clear(model, anchor_sample, sample);
            break;
        case WaveformHandleKind::selection_start:
            if (!model.has_selection) {
                model.clear_selection();
                break;
            }
            set_selection_or_clear(model, sample, model.selection.end);
            break;
        case WaveformHandleKind::selection_end:
            if (!model.has_selection) {
                model.clear_selection();
                break;
            }
            set_selection_or_clear(model, model.selection.start, sample);
            break;
        case WaveformHandleKind::trim_start:
            model.set_trim(std::min(sample, model.trim.end), model.trim.end);
            break;
        case WaveformHandleKind::trim_end:
            model.set_trim(model.trim.start, std::max(sample, model.trim.start));
            break;
        case WaveformHandleKind::fade_in:
            model.set_fade_in(sample);
            break;
        case WaveformHandleKind::fade_out:
            model.set_fade_out(sample);
            break;
        case WaveformHandleKind::loop_start:
            model.set_loop(std::min(sample, model.loop.end), model.loop.end);
            break;
        case WaveformHandleKind::loop_end:
            model.set_loop(model.loop.start, std::max(sample, model.loop.start));
            break;
        case WaveformHandleKind::slice_marker:
            if (id >= 0 && static_cast<std::size_t>(id) < model.slice_markers.size()) {
                auto markers = model.slice_markers;
                markers[static_cast<std::size_t>(id)] = sample;
                model.set_slice_markers(std::move(markers));
            }
            break;
        case WaveformHandleKind::playhead:
            model.set_playhead(sample);
            break;
    }
}

} // namespace pulp::view
