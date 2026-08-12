#include <pulp/timeline_view/piano_roll_view.hpp>

#include <pulp/canvas/canvas.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <variant>

namespace pulp::timeline_view {

using namespace pulp::timeline;

namespace {

/// Lattice colours are constants rather than theme tokens for now: the roll
/// draws structure, and a host that wants its own palette overrides paint().
constexpr canvas::Color kLatticeFill{0.11f, 0.12f, 0.14f, 1.0f};
constexpr canvas::Color kAccidentalFill{0.08f, 0.09f, 0.11f, 1.0f};
constexpr canvas::Color kNoteFill{0.35f, 0.72f, 0.45f, 1.0f};

/// Whether a pitch class is a black key. Used only to shade the lattice, so it
/// carries no octave-numbering convention with it — this view labels nothing,
/// and the C4-versus-C3 disagreement between MidiKeyboard and the musical
/// typing keyboard is therefore not a question it has to answer.
constexpr bool is_accidental(std::uint8_t pitch) noexcept {
    switch (pitch % 12) {
    case 1:
    case 3:
    case 6:
    case 8:
    case 10:
        return true;
    default:
        return false;
    }
}

timebase::TickPosition clamp_tick(std::int64_t value) noexcept {
    return timebase::TickPosition{std::max<std::int64_t>(0, value)};
}

constexpr timebase::TickPosition note_end(const NoteEvent& note) noexcept {
    return timebase::TickPosition{note.start.value + note.duration.value};
}

} // namespace

PianoRollView::PianoRollView() = default;

void PianoRollView::set_clip(const Project* project, ItemId sequence_id, ItemId track_id,
                             ItemId clip_id) {
    project_ = project;
    sequence_id_ = sequence_id;
    track_id_ = track_id;
    clip_id_ = clip_id;
    const auto all = notes();
    note_interval_max_end_.assign(all.empty() ? 0 : all.size() * 4, {});
    const auto build_interval_index = [&](auto&& self, std::size_t node, std::size_t begin,
                                          std::size_t end) -> timebase::TickPosition {
        if (end - begin == 1)
            return note_interval_max_end_[node] = note_end(all[begin]);
        const auto middle = begin + (end - begin) / 2;
        const auto left_end = self(self, node * 2 + 1, begin, middle);
        const auto right_end = self(self, node * 2 + 2, middle, end);
        return note_interval_max_end_[node] =
                   timebase::TickPosition{std::max(left_end.value, right_end.value)};
    };
    if (!all.empty())
        build_interval_index(build_interval_index, 0, 0, all.size());
    // A rebind invalidates anything a live gesture believed about a note.
    drag_.reset();
}

void PianoRollView::set_layout(const PianoRollLayout& layout) {
    layout_ = layout;
}

void PianoRollView::set_host(timeline_editor::NoteEditIntentHost* host) {
    host_ = host;
}

void PianoRollView::set_hit_metrics(const view::HitMetrics& metrics) {
    metrics_ = metrics;
}

void PianoRollView::set_note_factory(NoteFactory factory) {
    note_factory_ = std::move(factory);
}

const MidiContent* PianoRollView::content() const {
    if (!project_)
        return nullptr;
    const auto* sequence = project_->find_sequence(sequence_id_);
    if (!sequence)
        return nullptr;
    const auto* track = sequence->find_track(track_id_);
    if (!track)
        return nullptr;
    const auto* clip = track->find_clip(clip_id_);
    if (!clip)
        return nullptr;
    // Audio and nested-sequence clips carry no note lattice. Returning null
    // leaves the roll inert rather than drawing an empty grid over content it
    // cannot represent.
    return std::get_if<MidiContent>(&clip->content());
}

std::span<const NoteEvent> PianoRollView::notes() const {
    const auto* midi = content();
    return midi ? midi->notes() : std::span<const NoteEvent>{};
}

std::optional<view::Rect> PianoRollView::note_rect(ItemId note_id) const {
    if (!layout_)
        return std::nullopt;
    const auto all = notes();
    const auto found = std::find_if(all.begin(), all.end(),
                                    [&](const NoteEvent& note) { return note.id == note_id; });
    if (found == all.end())
        return std::nullopt;
    const float left = layout_->time.x_at(found->start);
    const float right = layout_->time.x_at(note_end(*found));
    const float row_height = layout_->pitch.row_height();
    // y_at returns the row's centre, so the rectangle is built around it.
    const float centre = layout_->pitch.y_at(found->pitch);
    return view::Rect{left, centre - row_height * 0.5f, right - left, row_height};
}

const NoteEvent* PianoRollView::note_at(view::Point position) const {
    if (!layout_)
        return nullptr;
    const float tolerance = metrics_.tolerance_px();
    const auto row_pitch = layout_->pitch.pitch_at(position.y);
    const NoteEvent* best = nullptr;
    float best_distance = std::numeric_limits<float>::max();
    for (const auto& note : notes()) {
        if (note.pitch != row_pitch)
            continue;
        const float left = layout_->time.x_at(note.start);
        const float right = layout_->time.x_at(note_end(note));
        // A tolerance band around the rectangle, so a touch that lands just
        // outside a short note still grabs it.
        if (position.x < left - tolerance || position.x > right + tolerance)
            continue;
        const float distance = position.x < left    ? left - position.x
                               : position.x > right ? position.x - right
                                                    : 0.0f;
        if (distance < best_distance) {
            best_distance = distance;
            best = &note;
        }
    }
    return best;
}

bool PianoRollView::on_trailing_edge(const NoteEvent& note, view::Point position) const {
    if (!layout_)
        return false;
    const float right = layout_->time.x_at(note_end(note));
    return std::abs(position.x - right) <= metrics_.tolerance_px();
}

timebase::TickPosition PianoRollView::snapped(timebase::TickPosition tick) const {
    if (!layout_ || layout_->snap.value <= 0)
        return tick;
    const auto grid = layout_->snap.value;
    const auto steps = (tick.value + grid / 2) / grid;
    return clamp_tick(steps * grid);
}

bool PianoRollView::admissible(const NoteEvent& note) {
    // The overflow test comes first because note_end() below would perform the
    // very addition it guards. Everything after this line may add start and
    // duration freely.
    if (note.duration.value <= 0 || note.pitch > 127 || note.channel > 15 || !note.id.valid() ||
        note.start.value > std::numeric_limits<std::int64_t>::max() - note.duration.value) {
        refusals_.push_back(PianoRollRefusal::InvalidNote);
        return false;
    }
    // A note's time is authored relative to its own clip, so a gesture that
    // would push it past either edge is refused rather than clamped: clamping
    // silently produces a different musical result than the one dragged for.
    const auto* sequence = project_ ? project_->find_sequence(sequence_id_) : nullptr;
    const auto* track = sequence ? sequence->find_track(track_id_) : nullptr;
    const auto* clip = track ? track->find_clip(clip_id_) : nullptr;
    if (!clip)
        return false;
    if (note.start.value < 0 || note_end(note).value > clip->duration().value) {
        refusals_.push_back(PianoRollRefusal::OutsideClip);
        return false;
    }
    return true;
}

void PianoRollView::emit(timeline_editor::NoteEditIntentKind kind,
                         std::optional<NoteEvent> expected, std::optional<NoteEvent> replacement) {
    if (!host_)
        return;
    timeline_editor::NoteEditIntent intent;
    intent.kind = kind;
    // Commit-on-release: one closed edit, never an open bracket. See the class
    // comment for why this is the shape rather than a limitation.
    intent.phase = GesturePhase::Single;
    intent.sequence_id = sequence_id_;
    intent.track_id = track_id_;
    intent.clip_id = clip_id_;
    intent.expected = std::move(expected);
    intent.replacement = std::move(replacement);

    // The validated wrapper is the only way into the host, and its factory is
    // fallible, so this branch is mandatory error handling rather than a
    // refusal a gesture reaches: every path that gets here has already passed
    // `admissible`, which is a superset of the note-domain checks validation
    // repeats. Recording the refusal rather than dropping the result silently
    // is what keeps a future gesture that DOES reach it visible instead of mute.
    auto validated = timeline_editor::ValidatedNoteEditIntent::create(std::move(intent));
    if (!validated) {
        refusals_.push_back(PianoRollRefusal::InvalidNote);
        return;
    }
    host_->submit_intent(validated.value());
}

void PianoRollView::paint(canvas::Canvas& canvas) {
    painted_note_count_ = 0;
    visited_candidate_count_ = 0;
    if (!layout_)
        return;
    const auto local = bounds();
    const float row_height = layout_->pitch.row_height();

    for (int pitch = layout_->pitch.highest_pitch(); pitch >= layout_->pitch.lowest_pitch();
         --pitch) {
        const float centre = layout_->pitch.y_at(static_cast<std::uint8_t>(pitch));
        const float top = centre - row_height * 0.5f;
        if (top >= local.height)
            continue;
        canvas.set_fill_color(is_accidental(static_cast<std::uint8_t>(pitch)) ? kAccidentalFill
                                                                             : kLatticeFill);
        canvas.fill_rect(0.0f, top, local.width, row_height);
    }

    const auto visible_start = layout_->time.visible_start();
    const auto visible_end = layout_->time.visible_end();
    const auto all = notes();
    canvas.set_fill_color(kNoteFill);

    const auto paint_candidate = [&](const NoteEvent& note) {
        ++visited_candidate_count_;
        if (note.pitch < layout_->pitch.lowest_pitch() ||
            note.pitch > layout_->pitch.highest_pitch())
            return;

        const float left = layout_->time.x_at(note.start);
        const float right = layout_->time.x_at(note_end(note));
        const float centre = layout_->pitch.y_at(note.pitch);
        const float visible_left = std::max(left, 0.0f);
        const float visible_right = std::min(right, local.width);
        if (visible_right <= visible_left)
            return;
        canvas.fill_rect(visible_left, centre - row_height * 0.5f, visible_right - visible_left,
                         row_height);
        ++painted_note_count_;
    };

    const auto paint_overlapping_range = [&](auto&& self, std::size_t node, std::size_t begin,
                                              std::size_t end) -> void {
        if (note_interval_max_end_[node].value <= visible_start.value ||
            all[begin].start.value >= visible_end.value)
            return;
        if (end - begin == 1) {
            paint_candidate(all[begin]);
            return;
        }
        const auto middle = begin + (end - begin) / 2;
        self(self, node * 2 + 1, begin, middle);
        self(self, node * 2 + 2, middle, end);
    };
    if (!all.empty())
        paint_overlapping_range(paint_overlapping_range, 0, 0, all.size());
}

void PianoRollView::on_mouse_down(view::Point position) {
    drag_.reset();
    if (!layout_ || !content())
        return;

    Drag drag;
    drag.press_tick = layout_->time.tick_at(position.x);
    drag.press_pitch = layout_->pitch.pitch_at(position.y);

    if (const auto* note = note_at(position)) {
        drag.grabbed = *note;
        drag.kind = on_trailing_edge(*note, position) ? DragKind::Resize : DragKind::Move;
        drag.grab_offset_ticks = drag.press_tick.value - note->start.value;
        drag_ = drag;
        return;
    }

    // Empty lattice space: a create gesture, if the caller enabled one.
    if (!note_factory_)
        return;
    drag.kind = DragKind::Insert;
    drag_ = drag;
}

void PianoRollView::on_mouse_drag(view::Point) {
    // Nothing leaves mid-gesture. The drag is tracked so a release can read the
    // final position; emitting per frame is the shape this view does not take.
}

void PianoRollView::on_mouse_up(view::Point position) {
    if (!drag_ || !layout_)
        return;
    const auto drag = *drag_;
    drag_.reset();

    const auto release_tick = layout_->time.tick_at(position.x);
    const auto release_pitch = layout_->pitch.pitch_at(position.y);

    if (drag.kind == DragKind::Insert) {
        if (!note_factory_)
            return;
        auto created = note_factory_(snapped(drag.press_tick), drag.press_pitch);
        if (!created) {
            // A factory that declines is a read-only roll, not an error.
            return;
        }
        if (!admissible(*created))
            return;
        emit(timeline_editor::NoteEditIntentKind::Insert, std::nullopt, std::move(created));
        return;
    }

    NoteEvent replacement = *drag.grabbed;
    if (drag.kind == DragKind::Move) {
        replacement.start = snapped(clamp_tick(release_tick.value - drag.grab_offset_ticks));
        replacement.pitch = release_pitch;
        // A press that never moved is not an edit. The validated intent would
        // reject it anyway (Move requires a changed start or pitch), so
        // returning here keeps a plain click from being recorded as a refusal.
        if (replacement.start == drag.grabbed->start && replacement.pitch == drag.grabbed->pitch)
            return;
    } else {
        // A trailing edge dragged back past its own note start yields a
        // non-positive duration. That is not tested for here: `admissible`
        // below already rejects it, and a second check would be a branch no
        // gesture can be shown to reach independently of that one.
        const auto end = snapped(release_tick);
        replacement.duration = timebase::TickDuration{end.value - drag.grabbed->start.value};
        if (replacement.duration == drag.grabbed->duration)
            return;
    }

    if (!admissible(replacement))
        return;
    emit(drag.kind == DragKind::Move ? timeline_editor::NoteEditIntentKind::Move
                                     : timeline_editor::NoteEditIntentKind::Resize,
         drag.grabbed, std::move(replacement));
}

void PianoRollView::on_mouse_cancel(view::Point) {
    // Nothing was emitted, so a cancel has nothing to close or revert. This is
    // the one place commit-on-release is strictly simpler than a bracket.
    drag_.reset();
}

void PianoRollView::on_mouse_event(const view::MouseEvent& event) {
    view::View::on_mouse_event(event);
    if (!event.is_down || event.button != view::MouseButton::right)
        return;
    if (!layout_ || !content())
        return;
    if (const auto* note = note_at(event.position))
        emit(timeline_editor::NoteEditIntentKind::Erase, *note, std::nullopt);
}

} // namespace pulp::timeline_view
