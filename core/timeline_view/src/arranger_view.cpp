#include <pulp/timeline_view/arranger_view.hpp>

#include <pulp/canvas/canvas.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <variant>

namespace pulp::timeline_view {

using namespace pulp::timeline;

namespace {

/// Row colours are constants rather than theme tokens for now: the arranger
/// draws structure, and a host that wants its own palette overrides paint().
constexpr canvas::Color kHeaderFill{0.16f, 0.17f, 0.20f, 1.0f};
constexpr canvas::Color kLaneFill{0.11f, 0.12f, 0.14f, 1.0f};
constexpr canvas::Color kClipFill{0.30f, 0.55f, 0.85f, 1.0f};

/// Clamps to the tick domain rather than wrapping: a drag that would push a
/// clip before the origin parks it at zero, which is what every arranger does
/// at the left edge.
timebase::TickPosition clamp_tick(std::int64_t value) noexcept {
    return timebase::TickPosition{std::max<std::int64_t>(0, value)};
}

bool is_nested_sequence(const Clip& clip) noexcept {
    return std::holds_alternative<SequenceRef>(clip.content());
}

} // namespace

float ArrangerLayout::x_for_tick(timebase::TickPosition tick) const noexcept {
    const auto delta = static_cast<double>(tick.value) - static_cast<double>(origin_tick.value);
    return lane_left_px + static_cast<float>(delta * px_per_tick);
}

timebase::TickPosition ArrangerLayout::tick_for_x(float x) const noexcept {
    if (px_per_tick == 0.0)
        return origin_tick;
    const auto ticks = static_cast<double>(x - lane_left_px) / px_per_tick;
    return clamp_tick(origin_tick.value + static_cast<std::int64_t>(std::llround(ticks)));
}

float ArrangerLayout::y_for_track_index(std::size_t index) const noexcept {
    return static_cast<float>(index) * track_height_px;
}

ArrangerView::ArrangerView() = default;

void ArrangerView::set_project(const Project* project, ItemId sequence_id) {
    project_ = project;
    sequence_id_ = sequence_id;
    // A project swap invalidates anything a live drag believed about a clip.
    drag_.reset();
}

void ArrangerView::set_layout(const ArrangerLayout& layout) {
    layout_ = layout;
}

void ArrangerView::set_host(timeline_editor::EditIntentHost* host) {
    host_ = host;
}

void ArrangerView::set_hit_metrics(const view::HitMetrics& metrics) {
    metrics_ = metrics;
}

void ArrangerView::set_clip_factory(ClipFactory factory) {
    clip_factory_ = std::move(factory);
}

const Sequence* ArrangerView::sequence() const {
    if (!project_)
        return nullptr;
    return project_->find_sequence(sequence_id_);
}

const Track* ArrangerView::track_at(view::Point position, std::size_t& index_out) const {
    const auto* seq = sequence();
    if (!seq || layout_.track_height_px <= 0.0f || position.y < 0.0f)
        return nullptr;
    const auto order = seq->track_order();
    const auto row = static_cast<std::size_t>(position.y / layout_.track_height_px);
    if (row >= order.size())
        return nullptr;
    index_out = row;
    return seq->find_track(order[row]);
}

const Clip* ArrangerView::clip_at(const Track& track, view::Point position) const {
    // The pointer type is resolved to a scalar here, before anything the
    // document sees: below this line the gesture is device-neutral.
    const float tolerance = metrics_.tolerance_px();
    const Clip* best = nullptr;
    float best_distance = std::numeric_limits<float>::max();
    for (const auto& clip : track.clips()) {
        if (clip.time_anchor() != ClipTimeAnchor::Musical)
            continue;
        const float left = layout_.x_for_tick(clip.start());
        const float right = layout_.x_for_tick(clip.end());
        // A tolerance band around the rectangle, so a touch that lands just
        // outside a narrow clip still grabs it.
        if (position.x < left - tolerance || position.x > right + tolerance)
            continue;
        const float distance = position.x < left    ? left - position.x
                               : position.x > right ? position.x - right
                                                    : 0.0f;
        if (distance < best_distance) {
            best_distance = distance;
            best = &clip;
        }
    }
    return best;
}

std::optional<view::Rect> ArrangerView::clip_rect(ItemId track_id, ItemId clip_id) const {
    const auto* seq = sequence();
    if (!seq)
        return std::nullopt;
    const auto* track = seq->find_track(track_id);
    if (!track)
        return std::nullopt;
    const auto* clip = track->find_clip(clip_id);
    if (!clip || clip->time_anchor() != ClipTimeAnchor::Musical)
        return std::nullopt;

    const auto order = seq->track_order();
    const auto found = std::find(order.begin(), order.end(), track_id);
    if (found == order.end())
        return std::nullopt;
    const auto row = static_cast<std::size_t>(std::distance(order.begin(), found));

    const float left = layout_.x_for_tick(clip->start());
    const float right = layout_.x_for_tick(clip->end());
    return view::Rect{left, layout_.y_for_track_index(row), right - left,
                      layout_.track_height_px};
}

void ArrangerView::paint(canvas::Canvas& canvas) {
    const auto* seq = sequence();
    if (!seq)
        return;
    const auto local = bounds();
    const auto order = seq->track_order();

    for (std::size_t row = 0; row < order.size(); ++row) {
        const float top = layout_.y_for_track_index(row);
        if (top >= local.height)
            break;

        canvas.set_fill_color(kHeaderFill);
        canvas.fill_rect(0.0f, top, layout_.lane_left_px, layout_.track_height_px);

        canvas.set_fill_color(kLaneFill);
        canvas.fill_rect(layout_.lane_left_px, top, local.width - layout_.lane_left_px,
                         layout_.track_height_px);

        const auto* track = seq->find_track(order[row]);
        if (!track)
            continue;
        canvas.set_fill_color(kClipFill);
        for (const auto& clip : track->clips()) {
            if (clip.time_anchor() != ClipTimeAnchor::Musical)
                continue;
            const float left = layout_.x_for_tick(clip.start());
            const float right = layout_.x_for_tick(clip.end());
            // Clip against the lane column so a scrolled-out clip cannot paint
            // over the header, which is a separate hit surface.
            const float visible_left = std::max(left, layout_.lane_left_px);
            const float visible_right = std::min(right, local.width);
            if (visible_right <= visible_left)
                continue;
            canvas.fill_rect(visible_left, top, visible_right - visible_left,
                             layout_.track_height_px);
        }
    }
}

void ArrangerView::on_mouse_down(view::Point position) {
    drag_.reset();
    std::size_t row = 0;
    const auto* track = track_at(position, row);
    if (!track || position.x < layout_.lane_left_px)
        return;

    if (const auto* clip = clip_at(*track, position)) {
        Drag drag;
        drag.track_id = track->id();
        drag.clip_id = clip->id();
        drag.duration = clip->duration();
        drag.believed = MusicalTimeRange{clip->start(), clip->duration()};
        drag.grab_offset_ticks =
            layout_.tick_for_x(position.x).value - clip->start().value;
        drag_ = drag;
        return;
    }

    // Empty lane space: a create gesture, if the caller enabled one.
    if (!clip_factory_)
        return;
    auto created = clip_factory_(layout_.tick_for_x(position.x));
    if (!created)
        return;
    // An arranger will not author a nested-sequence placement. The program
    // compiler refuses to lower one whose anchor or playback properties are
    // anything but the defaults (`NestedSequenceUnsupported`), so a lane click
    // that produced one could author a document that cannot play. Nesting is a
    // deliberate structural operation, not a side effect of clicking a lane.
    if (is_nested_sequence(*created)) {
        refusals_.push_back(ArrangerRefusal::NestedSequenceContent);
        return;
    }

    timeline_editor::EditIntent intent;
    intent.kind = timeline_editor::EditIntentKind::Draw;
    intent.phase = GesturePhase::Single;
    intent.sequence_id = sequence_id_;
    intent.track_id = track->id();
    intent.clip = std::move(created);
    submit(intent);
}

void ArrangerView::on_mouse_drag(view::Point position) {
    if (!drag_)
        return;
    const auto target =
        clamp_tick(layout_.tick_for_x(position.x).value - drag_->grab_offset_ticks);
    if (target.value == drag_->believed.start.value)
        return;

    const MusicalTimeRange replacement{target, drag_->duration};
    // The first step that actually moves opens the gesture; a press that never
    // moves leaves no undo group behind.
    const auto phase = drag_->opened ? GesturePhase::Update : GesturePhase::Begin;
    emit_move(*drag_, phase, replacement);
    drag_->believed = replacement;
    drag_->opened = true;
}

void ArrangerView::on_mouse_up(view::Point position) {
    if (!drag_)
        return;
    const auto target =
        clamp_tick(layout_.tick_for_x(position.x).value - drag_->grab_offset_ticks);
    const MusicalTimeRange replacement{target, drag_->duration};

    if (!drag_->opened && target.value == drag_->believed.start.value) {
        // A click that never moved. Nothing was emitted, so there is no group
        // to close and no edit to stand.
        drag_.reset();
        return;
    }
    emit_move(*drag_, drag_->opened ? GesturePhase::End : GesturePhase::Single, replacement);
    drag_.reset();
}

void ArrangerView::on_mouse_cancel(view::Point) {
    if (!drag_)
        return;
    if (drag_->opened) {
        // Cancel closes the group over edits that were already applied; the
        // revert is the session's own undo, not a second reduction here.
        emit_move(*drag_, GesturePhase::Cancel, drag_->believed);
    }
    drag_.reset();
}

void ArrangerView::emit_move(const Drag& drag, GesturePhase phase,
                             MusicalTimeRange replacement) {
    timeline_editor::EditIntent intent;
    intent.kind = timeline_editor::EditIntentKind::Move;
    intent.phase = phase;
    intent.sequence_id = sequence_id_;
    intent.track_id = drag.track_id;
    intent.clip_id = drag.clip_id;
    intent.expected_range = ClipTimeRange{drag.believed};
    intent.replacement_range = ClipTimeRange{replacement};
    submit(intent);
}

void ArrangerView::submit(const timeline_editor::EditIntent& intent) {
    if (host_)
        host_->submit_intent(intent);
}

} // namespace pulp::timeline_view
