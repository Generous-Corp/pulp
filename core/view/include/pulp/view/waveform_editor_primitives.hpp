#pragma once

/// @file waveform_editor_primitives.hpp
/// Reusable waveform editor geometry, snapping, hit-testing, and transaction helpers.

#include <pulp/view/geometry.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::view {

/// Sample range in source-frame coordinates. End is exclusive by convention.
struct WaveformSampleRange {
    int64_t start = 0;
    int64_t end = 0;

    [[nodiscard]] int64_t length() const { return end - start; }
    [[nodiscard]] bool empty() const { return start == end; }
    [[nodiscard]] WaveformSampleRange normalized() const;
};

/// Immutable-for-paint viewport state and sample/pixel conversion rules.
struct WaveformViewport {
    int64_t total_samples = 0;
    int64_t visible_start = 0;
    int64_t visible_length = 0;
    int64_t min_visible_length = 16;
    Rect bounds{};

    void set_total_samples(int64_t samples);
    void set_bounds(Rect rect);
    void set_visible_range(int64_t start, int64_t length);
    void zoom_in(double factor = 2.0);
    void zoom_out(double factor = 2.0);
    void zoom_to_fit();
    void scroll(int64_t delta_samples);

    [[nodiscard]] bool empty() const { return total_samples <= 0 || visible_length <= 0; }
    [[nodiscard]] int64_t visible_end() const;
    [[nodiscard]] bool sample_visible(int64_t sample) const;
    [[nodiscard]] bool sample_point_visible(int64_t sample) const;
    [[nodiscard]] int64_t clamp_sample(int64_t sample) const;
    [[nodiscard]] double samples_per_pixel() const;
    [[nodiscard]] double pixels_per_sample() const;
    [[nodiscard]] float sample_to_x(int64_t sample) const;
    [[nodiscard]] int64_t x_to_sample(float x) const;
};

/// One pixel/column span for waveform min/max reduction or GPU upload planning.
struct WaveformRenderSpan {
    int64_t sample_start = 0;
    int64_t sample_end = 0;
    float x = 0.0f;
    float width = 0.0f;

    [[nodiscard]] int64_t sample_count() const { return sample_end - sample_start; }
};

/// Opt-in geometry plan. Callers decide when span allocation is acceptable.
struct WaveformRenderPlan {
    WaveformViewport viewport{};
    std::vector<WaveformRenderSpan> spans;
};

[[nodiscard]] WaveformRenderPlan build_waveform_render_plan(const WaveformViewport& viewport,
                                                            int max_spans = 0);

enum class WaveformHandleKind {
    none,
    selection_start,
    selection_end,
    trim_start,
    trim_end,
    fade_in,
    fade_out,
    loop_start,
    loop_end,
    slice_marker,
    playhead,
};

struct WaveformHandle {
    WaveformHandleKind kind = WaveformHandleKind::none;
    int id = -1;
    int64_t sample = 0;
    bool enabled = true;
};

/// Editor handle state without rendering, input events, or audio ownership.
struct WaveformHandleModel {
    int64_t total_samples = 0;

    bool has_selection = false;
    WaveformSampleRange selection{};

    bool has_trim = false;
    WaveformSampleRange trim{};

    bool has_loop = false;
    WaveformSampleRange loop{};

    bool has_fade_in = false;
    int64_t fade_in_end = 0;

    bool has_fade_out = false;
    int64_t fade_out_start = 0;

    bool has_playhead = false;
    int64_t playhead = 0;

    std::vector<int64_t> slice_markers;

    void set_total_samples(int64_t samples);
    void set_selection(int64_t start, int64_t end);
    void clear_selection();
    void set_trim(int64_t start, int64_t end);
    void clear_trim();
    void set_loop(int64_t start, int64_t end);
    void clear_loop();
    void set_fade_in(int64_t end_sample);
    void clear_fade_in();
    void set_fade_out(int64_t start_sample);
    void clear_fade_out();
    void set_playhead(int64_t sample);
    void clear_playhead();
    void set_slice_markers(std::vector<int64_t> markers);

    template <typename Visitor>
    void for_each_handle(Visitor&& visitor) const {
        if (has_selection) {
            visitor(WaveformHandle{WaveformHandleKind::selection_start, 0, selection.start, true});
            visitor(WaveformHandle{WaveformHandleKind::selection_end, 0, selection.end, true});
        }
        if (has_trim) {
            visitor(WaveformHandle{WaveformHandleKind::trim_start, 0, trim.start, true});
            visitor(WaveformHandle{WaveformHandleKind::trim_end, 0, trim.end, true});
        }
        if (has_fade_in) {
            visitor(WaveformHandle{WaveformHandleKind::fade_in, 0, fade_in_end, true});
        }
        if (has_fade_out) {
            visitor(WaveformHandle{WaveformHandleKind::fade_out, 0, fade_out_start, true});
        }
        if (has_loop) {
            visitor(WaveformHandle{WaveformHandleKind::loop_start, 0, loop.start, true});
            visitor(WaveformHandle{WaveformHandleKind::loop_end, 0, loop.end, true});
        }
        for (std::size_t i = 0; i < slice_markers.size(); ++i) {
            visitor(WaveformHandle{WaveformHandleKind::slice_marker,
                                   static_cast<int>(i),
                                   slice_markers[i],
                                   true});
        }
        if (has_playhead) {
            visitor(WaveformHandle{WaveformHandleKind::playhead, 0, playhead, true});
        }
    }

    [[nodiscard]] std::vector<WaveformHandle> handles() const;
};

struct WaveformHitResult {
    WaveformHandleKind kind = WaveformHandleKind::none;
    int id = -1;
    int64_t sample = 0;
    float distance_px = 0.0f;

    [[nodiscard]] bool hit() const { return kind != WaveformHandleKind::none; }
    explicit operator bool() const { return hit(); }
};

[[nodiscard]] WaveformHitResult hit_test_waveform_handles(const WaveformViewport& viewport,
                                                          const WaveformHandleModel& model,
                                                          float x,
                                                          float tolerance_px);

enum class WaveformSnapSource {
    none,
    bounds,
    candidate,
    grid,
};

struct WaveformSnapSettings {
    int64_t grid_interval_samples = 0;
    int64_t tolerance_samples = 0;
    std::vector<int64_t> candidates;
    bool snap_to_bounds = true;
};

struct WaveformSnapResult {
    int64_t sample = 0;
    bool snapped = false;
    WaveformSnapSource source = WaveformSnapSource::none;
};

[[nodiscard]] WaveformSnapResult resolve_waveform_snap(int64_t sample,
                                                       int64_t total_samples,
                                                       const WaveformSnapSettings& settings);

struct WaveformPlayheadOverlay {
    int64_t sample = 0;
    bool visible = false;
    float x = 0.0f;
};

[[nodiscard]] WaveformPlayheadOverlay build_waveform_playhead_overlay(const WaveformViewport& viewport,
                                                                      int64_t playhead_sample);

enum class WaveformEditorOperationKind {
    none,
    create_selection,
    extend_selection,
    drag_selection_start,
    drag_selection_end,
    move_playhead,
};

struct WaveformEditorTransactionResult {
    WaveformEditorOperationKind operation = WaveformEditorOperationKind::none;
    bool active = false;
    bool committed = false;
    bool cancelled = false;
    WaveformSampleRange selection{};
    int64_t playhead_sample = 0;
};

/// Semantic edit transaction state. Platform input translation stays in widgets/views.
class WaveformEditorTransaction {
public:
    void begin(WaveformEditorOperationKind operation,
               int64_t anchor_sample,
               WaveformSampleRange initial_selection = {},
               int64_t initial_playhead_sample = 0);

    [[nodiscard]] WaveformEditorTransactionResult update(int64_t sample) const;
    [[nodiscard]] WaveformEditorTransactionResult commit(int64_t sample);
    [[nodiscard]] WaveformEditorTransactionResult cancel();

    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] WaveformEditorOperationKind operation() const { return operation_; }

private:
    [[nodiscard]] WaveformEditorTransactionResult result_for_sample(int64_t sample,
                                                                    bool committed,
                                                                    bool cancelled) const;

    bool active_ = false;
    WaveformEditorOperationKind operation_ = WaveformEditorOperationKind::none;
    int64_t anchor_sample_ = 0;
    WaveformSampleRange initial_selection_{};
    int64_t initial_playhead_sample_ = 0;
};

struct WaveformEditorSurfaceSnapshot {
    WaveformViewport viewport{};
    WaveformHandleModel handles{};
    WaveformPlayheadOverlay playhead{};
    bool edit_active = false;
    WaveformHandleKind edit_kind = WaveformHandleKind::none;
    int edit_id = -1;
};

struct WaveformEditorSurfaceEditResult {
    WaveformHandleKind kind = WaveformHandleKind::none;
    int id = -1;
    bool active = false;
    bool committed = false;
    bool cancelled = false;
    WaveformSnapResult snap{};
    WaveformHandleModel handles{};
};

/// View-layer editing surface that composes waveform primitives without owning audio or painting.
class WaveformEditorSurface {
public:
    void set_total_samples(int64_t samples);
    void set_bounds(Rect bounds);
    void set_visible_range(int64_t start, int64_t length);
    void zoom_to_fit();
    void scroll(int64_t delta_samples);

    [[nodiscard]] const WaveformViewport& viewport() const { return viewport_; }
    [[nodiscard]] const WaveformHandleModel& handles() const { return handles_; }

    void set_snap_settings(WaveformSnapSettings settings);
    [[nodiscard]] const WaveformSnapSettings& snap_settings() const { return snap_settings_; }

    void set_selection(int64_t start, int64_t end);
    void clear_selection();
    void set_trim(int64_t start, int64_t end);
    void clear_trim();
    void set_loop(int64_t start, int64_t end);
    void clear_loop();
    void set_fade_in(int64_t end_sample);
    void clear_fade_in();
    void set_fade_out(int64_t start_sample);
    void clear_fade_out();
    void set_playhead(int64_t sample);
    void clear_playhead();
    void set_slice_markers(std::vector<int64_t> markers);

    [[nodiscard]] WaveformEditorSurfaceSnapshot snapshot() const;
    [[nodiscard]] WaveformRenderPlan render_plan(int max_spans = 0) const;
    [[nodiscard]] WaveformHitResult hit_test(float x, float tolerance_px) const;

    [[nodiscard]] bool begin_selection_edit(int64_t anchor_sample);
    [[nodiscard]] bool begin_handle_edit(WaveformHandleKind kind, int id = -1);
    [[nodiscard]] bool begin_handle_edit(const WaveformHitResult& hit);
    [[nodiscard]] WaveformEditorSurfaceEditResult update_edit(int64_t sample);
    [[nodiscard]] WaveformEditorSurfaceEditResult commit_edit(int64_t sample);
    [[nodiscard]] WaveformEditorSurfaceEditResult cancel_edit();

    [[nodiscard]] bool edit_active() const { return edit_active_; }
    [[nodiscard]] WaveformHandleKind edit_kind() const { return edit_kind_; }
    [[nodiscard]] int edit_id() const { return edit_id_; }

private:
    [[nodiscard]] bool can_edit(WaveformHandleKind kind, int id) const;
    [[nodiscard]] WaveformEditorSurfaceEditResult edit_result_for_sample(int64_t sample,
                                                                         bool committed,
                                                                         bool cancelled) const;
    static void apply_edit(WaveformHandleModel& model,
                           WaveformHandleKind kind,
                           int id,
                           int64_t anchor_sample,
                           int64_t sample);

    WaveformViewport viewport_{};
    WaveformHandleModel handles_{};
    WaveformSnapSettings snap_settings_{};
    WaveformHandleModel edit_initial_handles_{};
    bool edit_active_ = false;
    WaveformHandleKind edit_kind_ = WaveformHandleKind::none;
    int edit_id_ = -1;
    int64_t edit_anchor_sample_ = 0;
};

} // namespace pulp::view
