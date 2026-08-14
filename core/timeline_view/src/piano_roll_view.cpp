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

constexpr bool same_note(const NoteEvent& left, const NoteEvent& right) noexcept {
    return left.id == right.id && left.start == right.start && left.duration == right.duration &&
           left.velocity == right.velocity && left.pitch == right.pitch &&
           left.channel == right.channel;
}

} // namespace

PianoRollView::PianoRollView() = default;

PianoRollPitchRuler::PianoRollPitchRuler() {
    set_orientation(view::MidiKeyboard::Orientation::vertical_chromatic_rows);
    set_show_note_names(true);
}

void PianoRollPitchRuler::set_pitch_projection(
    const timeline_editor::PitchProjection& projection) {
    set_range(projection.lowest_pitch(), projection.highest_pitch());
    const auto frame = bounds();
    const auto pixels = projection.pixels();
    set_bounds({frame.x, pixels.origin, frame.width, pixels.extent});
    request_repaint();
}

void PianoRollView::set_clip(const Project* project, ItemId sequence_id, ItemId track_id,
                             ItemId clip_id) {
    const bool same_binding = sequence_id_ == sequence_id && track_id_ == track_id &&
                              clip_id_ == clip_id;
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
    // A host may synchronously apply an accepted continuous phase and rebind
    // this view before submit_intent() returns. Preserve only the pre-staged
    // optimistic value that the rebound snapshot now actually carries; every
    // other rebind invalidates the gesture as before.
    if (drag_) {
        bool preserves_staged_phase = false;
        if (same_binding && drag_->last_emitted) {
            const auto rebound = notes();
            const auto found = std::find_if(rebound.begin(), rebound.end(), [&](const auto& note) {
                return note.id == drag_->last_emitted->id;
            });
            preserves_staged_phase =
                found != rebound.end() && same_note(*found, *drag_->last_emitted);
        }
        if (!preserves_staged_phase)
            drag_.reset();
    }
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

std::optional<NoteEvent> PianoRollView::replacement_at(const Drag& drag,
                                                       view::Point position) const {
    if (!layout_ || !drag.grabbed || drag.kind == DragKind::Insert)
        return std::nullopt;

    NoteEvent replacement = *drag.grabbed;
    if (drag.kind == DragKind::Move) {
        const auto tick = layout_->time.tick_at(position.x);
        replacement.start = snapped(clamp_tick(tick.value - drag.grab_offset_ticks));
        replacement.pitch = layout_->pitch.pitch_at(position.y);
    } else if (drag.kind == DragKind::Resize) {
        const auto end = snapped(layout_->time.tick_at(position.x));
        replacement.duration = timebase::TickDuration{end.value - replacement.start.value};
    } else {
        constexpr double kVelocityUnitsPerPixel = 128.0;
        const auto delta = static_cast<std::int64_t>(
            std::llround(static_cast<double>(drag.press_y - position.y) *
                         kVelocityUnitsPerPixel));
        const auto authored = std::clamp<std::int64_t>(
            static_cast<std::int64_t>(drag.grabbed->velocity) + delta, 0,
            std::numeric_limits<std::uint16_t>::max());
        replacement.velocity = static_cast<std::uint16_t>(authored);
    }
    return replacement;
}

bool PianoRollView::emit(timeline_editor::NoteEditIntentKind kind, GesturePhase phase,
                         std::optional<NoteEvent> expected,
                         std::optional<NoteEvent> replacement) {
    if (!host_)
        return false;
    timeline_editor::NoteEditIntent intent;
    intent.kind = kind;
    intent.phase = phase;
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
        return false;
    }
    const auto result = host_->submit_intent(validated.value());
    return result.status == timeline_editor::IntentStatus::Accepted;
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
    drag.press_y = position.y;

    if (const auto* note = note_at(position)) {
        drag.grabbed = *note;
        drag.kind = (current_modifiers_ & view::kModShift) != 0
                        ? DragKind::SetVelocity
                        : on_trailing_edge(*note, position) ? DragKind::Resize : DragKind::Move;
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

void PianoRollView::on_mouse_drag(view::Point position) {
    if (!drag_ || drag_->kind == DragKind::Insert)
        return;
    if (!continuous_gestures_)
        return;
    if (drag_->release_only)
        return;

    auto replacement = replacement_at(*drag_, position);
    if (!replacement || same_note(*replacement, *drag_->grabbed)) {
        if (replacement)
            drag_->pending = std::move(replacement);
        return;
    }
    if (!admissible(*replacement)) {
        drag_->last_refused = std::move(replacement);
        return;
    }
    drag_->last_refused.reset();
    if (!drag_->pending) {
        drag_->pending = std::move(replacement);
        return;
    }
    if (same_note(*drag_->pending, *replacement))
        return;

    const auto kind = drag_->kind == DragKind::Move
                          ? timeline_editor::NoteEditIntentKind::Move
                      : drag_->kind == DragKind::Resize
                          ? timeline_editor::NoteEditIntentKind::Resize
                          : timeline_editor::NoteEditIntentKind::SetVelocity;
    const auto phase = drag_->last_emitted ? GesturePhase::Update : GesturePhase::Begin;
    const auto expected = drag_->last_emitted ? *drag_->last_emitted : *drag_->grabbed;
    // Returning to the press value is held for End/Cancel. Emitting it as an
    // Update would leave no changed payload with which to close the bracket.
    if (same_note(*drag_->pending, *drag_->grabbed)) {
        drag_->pending = std::move(replacement);
        return;
    }

    const auto grabbed = *drag_->grabbed;
    const auto staged = *drag_->pending;
    const auto previously_emitted = drag_->last_emitted;
    const auto before_submission = *drag_;
    // Stage before crossing the synchronous host seam. set_clip() can then
    // prove that a reentrant rebind contains exactly the accepted phase.
    drag_->last_emitted = staged;
    drag_->pending = replacement;
    const bool accepted = emit(kind, phase, expected, staged);
    if (accepted && drag_ && drag_->last_emitted &&
        same_note(*drag_->last_emitted, staged))
        return;

    // Begin rejection opened nothing. Keep the legacy release-only path when
    // the rebound snapshot still carries the grabbed note; this lets hosts
    // adopt continuous grouping independently without losing existing edits.
    if (!accepted && !previously_emitted) {
        const auto current = notes();
        const auto found = std::find_if(current.begin(), current.end(),
                                        [&](const auto& note) { return note.id == grabbed.id; });
        if (found != current.end() && same_note(*found, grabbed)) {
            drag_ = before_submission;
            drag_->release_only = true;
            return;
        }
    }

    // A rejected later phase leaves the last accepted group open, so make one
    // best-effort restoring close before abandoning local gesture state.
    drag_.reset();
    if (previously_emitted)
        emit(kind, GesturePhase::Cancel, *previously_emitted, grabbed);
    else if (accepted)
        emit(kind, GesturePhase::Cancel, staged, grabbed);
}

void PianoRollView::on_mouse_up(view::Point position) {
    if (!drag_ || !layout_)
        return;
    const auto drag = *drag_;
    drag_.reset();

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
        emit(timeline_editor::NoteEditIntentKind::Insert, GesturePhase::Single, std::nullopt,
             std::move(created));
        return;
    }

    auto replacement = replacement_at(drag, position);
    if (!replacement)
        return;
    const auto kind = drag.kind == DragKind::Move
                          ? timeline_editor::NoteEditIntentKind::Move
                      : drag.kind == DragKind::Resize
                          ? timeline_editor::NoteEditIntentKind::Resize
                          : timeline_editor::NoteEditIntentKind::SetVelocity;

    const auto cancel_open = [&]() {
        if (drag.last_emitted && !same_note(*drag.last_emitted, *drag.grabbed))
            emit(kind, GesturePhase::Cancel, *drag.last_emitted, *drag.grabbed);
    };
    if (same_note(*replacement, *drag.grabbed)) {
        cancel_open();
        return;
    }
    if (!drag.last_refused || !same_note(*drag.last_refused, *replacement)) {
        if (!admissible(*replacement)) {
            cancel_open();
            return;
        }
    } else {
        cancel_open();
        return;
    }

    if (!drag.last_emitted) {
        emit(kind, GesturePhase::Single, *drag.grabbed, std::move(replacement));
        return;
    }
    if (!same_note(*drag.last_emitted, *replacement)) {
        if (!emit(kind, GesturePhase::End, *drag.last_emitted, *replacement))
            emit(kind, GesturePhase::Cancel, *drag.last_emitted, *drag.grabbed);
        return;
    }
    // A reversal can release exactly on the last emitted value. If a distinct
    // pending sample exists, use it as the final Update so End still carries a
    // valid transform. Otherwise restore rather than strand an open group.
    if (drag.pending && !same_note(*drag.pending, *drag.last_emitted) &&
        !same_note(*drag.pending, *drag.grabbed) && admissible(*drag.pending) &&
        emit(kind, GesturePhase::Update, *drag.last_emitted, *drag.pending)) {
        if (!emit(kind, GesturePhase::End, *drag.pending, *replacement))
            emit(kind, GesturePhase::Cancel, *drag.pending, *drag.grabbed);
    } else {
        cancel_open();
    }
}

void PianoRollView::on_mouse_cancel(view::Point) {
    if (drag_ && drag_->last_emitted && drag_->grabbed &&
        !same_note(*drag_->last_emitted, *drag_->grabbed)) {
        const auto kind = drag_->kind == DragKind::Move
                              ? timeline_editor::NoteEditIntentKind::Move
                          : drag_->kind == DragKind::Resize
                              ? timeline_editor::NoteEditIntentKind::Resize
                              : timeline_editor::NoteEditIntentKind::SetVelocity;
        emit(kind, GesturePhase::Cancel, *drag_->last_emitted, *drag_->grabbed);
    }
    drag_.reset();
}

void PianoRollView::on_mouse_event(const view::MouseEvent& event) {
    view::View::on_mouse_event(event);
    if (event.isPress())
        current_modifiers_ = event.modifiers;
    if (!event.is_down || event.button != view::MouseButton::right)
        return;
    if (!layout_ || !content())
        return;
    if (const auto* note = note_at(event.position))
        emit(timeline_editor::NoteEditIntentKind::Erase, GesturePhase::Single, *note,
             std::nullopt);
}

} // namespace pulp::timeline_view
