#pragma once

/// @file piano_roll_view.hpp
/// The piano roll: one MIDI clip's notes as rectangles, and the gestures that
/// edit them.

#include <pulp/timeline/model.hpp>
#include <pulp/timeline_editor/edit_intent.hpp>
#include <pulp/timeline_editor/viewport_projection.hpp>
#include <pulp/view/hit_metrics.hpp>
#include <pulp/view/midi_keyboard.hpp>
#include <pulp/view/view.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace pulp::timeline_view {

/** @addtogroup timeline_view
 * @{
 */

/// A resolved note lattice, supplied by whoever owns the viewport.
///
/// Both members are the editor kernel's own projections rather than a second
/// set of scalars minted here. They qualify as resolved values on the same test
/// the arranger's scalars pass: neither owns zoom, scroll or device-pixel
/// policy, and neither has to be kept in sync with anything — a `TickProjection`
/// is a visible tick range plus a pixel span, and a `PitchProjection` is an
/// inclusive pitch range plus a pixel span. Whoever resolves zoom and scroll
/// builds them once per frame and hands them over finished.
struct PianoRollLayout {
    timeline_editor::TickProjection time;
    timeline_editor::PitchProjection pitch;
    /// Grid the authored start of an inserted or moved note lands on. Zero
    /// snaps nothing, which is what a free-time edit wants.
    timebase::TickDuration snap{};
};

/// Pitch ruler for a piano roll, using the shared MIDI keyboard interaction and
/// C4 naming convention over the editor kernel's resolved pitch projection.
///
/// Assign `on_note_on` and `on_note_off` to receive audition requests. The
/// ruler deliberately does not own an audio engine or decide how those requests
/// are rendered.
class PianoRollPitchRuler final : public view::MidiKeyboard {
  public:
    /// Creates a named vertical chromatic ruler ready for pitch projection.
    PianoRollPitchRuler();

    /// Aligns this ruler's frame and inclusive note range to a piano-roll
    /// projection while preserving its current horizontal frame.
    void set_pitch_projection(const timeline_editor::PitchProjection& projection);
};

/// Why the piano roll declined to emit an intent a gesture would otherwise
/// have produced. Recorded rather than thrown: a refused gesture is an ordinary
/// outcome a view stays usable after.
enum class PianoRollRefusal : std::uint8_t {
    /// A note factory returned a note the note domain rejects, or the gesture
    /// would have produced one. See `set_note_factory`.
    InvalidNote,
    /// A move or resize would have left the note's own clip. A note's time is
    /// authored relative to its clip, so a gesture that runs past either edge
    /// is refused rather than silently clamped into a different musical result.
    OutsideClip,
};

/// Piano roll over one MIDI clip of a `timeline::Project`.
///
/// Holds nothing durable: the project is borrowed for painting and hit testing,
/// and every edit leaves as a validated `timeline_editor::NoteEditIntent`
/// submitted to a host. The view never builds a `timeline::Command`, never owns
/// a revision, and never learns what applies its intents.
///
/// When continuous gestures are enabled, note transforms emit a `Begin` /
/// `Update` / `End` stream whose adjacent expected and replacement notes form
/// one optimistic chain. A host lowers the stream with one undo group, so the
/// document applies continuously while one undo restores the pre-gesture
/// bytes. A short drag with only one changed sample remains a closed `Single`
/// edit. Insert and erase are always `Single`.
///
/// Shift-dragging a note body edits velocity instead of position. The gesture
/// changes velocity by 128 authored units per vertical pixel, increasing as the
/// pointer moves upward and clamping to the note domain.
class PianoRollView : public view::View {
  public:
    /// Builds the payload for a create-note gesture at a lattice position.
    ///
    /// Supplied by the caller because a note's identity comes from the
    /// document's id domain, which a view does not own, and because default
    /// duration and velocity are authoring policy. Returning `nullopt` declines
    /// the gesture, which is what a read-only piano roll does.
    using NoteFactory = std::function<std::optional<timeline::NoteEvent>(
        timebase::TickPosition start, std::uint8_t pitch)>;

    PianoRollView();

    /// Borrows the project to paint and hit-test, and names the MIDI clip whose
    /// notes this roll edits. Nothing is retained past the next call; the
    /// caller re-points the view after every commit.
    ///
    /// A clip whose content is not `MidiContent` leaves the roll bound to
    /// nothing: audio and nested-sequence clips have no note lattice, and a
    /// roll that drew something for them would be inventing content.
    void set_clip(const timeline::Project* project, timeline::ItemId sequence_id,
                  timeline::ItemId track_id, timeline::ItemId clip_id);
    void set_layout(const PianoRollLayout& layout);
    /// Where emitted intents go. Null leaves the view fully interactive and
    /// silent, which is what a detached or read-only roll is.
    void set_host(timeline_editor::NoteEditIntentHost* host);
    /// Pointer geometry, resolved once per gesture rather than per hit test.
    void set_hit_metrics(const view::HitMetrics& metrics);
    /// Enables the create-note gesture. Unset, a click on empty lattice space
    /// does nothing.
    void set_note_factory(NoteFactory factory);
    /// Opts into continuous transform phases. The host must allocate and retain
    /// one undo group across Begin/Update/End/Cancel. Disabled by default so
    /// release-only hosts keep receiving the historical Single intent.
    void set_continuous_gestures(bool enabled) noexcept { continuous_gestures_ = enabled; }

    /// The bound clip's notes, or empty when nothing is bound.
    std::span<const timeline::NoteEvent> notes() const;

    /// Rectangle a note occupies, in this view's local coordinates, or
    /// `nullopt` when the identity is not in the bound clip.
    std::optional<view::Rect> note_rect(timeline::ItemId note_id) const;

    /// How many notes the last `paint()` actually drew.
    ///
    /// Exposed because culling correctness is the property worth asserting
    /// about this renderer, and a count is the only way to see the difference
    /// between a viewport that excluded a note and a renderer that drew nothing.
    std::size_t painted_note_count() const noexcept { return painted_note_count_; }

    /// How many time-overlapping note candidates the last `paint()` examined.
    ///
    /// The interval index excludes both notes ending before the viewport and
    /// notes starting at or after it. This makes the renderer's culling budget
    /// deterministic without using a wall-clock threshold.
    std::size_t visited_candidate_count() const noexcept { return visited_candidate_count_; }

    /// Refusals recorded since the last `clear_refusals()`, in order.
    const std::vector<PianoRollRefusal>& refusals() const noexcept { return refusals_; }
    void clear_refusals() { refusals_.clear(); }

    /// Whether a pointer gesture is mid-flight. A continuous transform may
    /// already have emitted an open bracket while this remains true.
    bool gesture_open() const noexcept { return drag_.has_value(); }

    // ── View ─────────────────────────────────────────────────────────────

    void paint(canvas::Canvas& canvas) override;
    bool wants_mouse_input() const override { return true; }
    void on_mouse_down(view::Point position) override;
    void on_mouse_drag(view::Point position) override;
    void on_mouse_up(view::Point position) override;
    void on_mouse_cancel(view::Point position) override;
    /// Erase lives here rather than on the button-less channel because it needs
    /// to know which button pressed. A secondary-button press over a note
    /// erases it; the button-less verbs never see one.
    void on_mouse_event(const view::MouseEvent& event) override;

  private:
    /// What a press grabbed and how its continuous intent stream is shaped.
    enum class DragKind : std::uint8_t {
        Insert, ///< Empty lattice space: a note is created on release.
        Move,   ///< A note body: start and pitch change.
        Resize, ///< A note's trailing edge: duration changes.
        SetVelocity, ///< Shift plus note body: velocity changes vertically.
    };

    /// One gesture in flight. `pending` deliberately trails the newest pointer
    /// sample by one event, leaving a changed value for End even when a
    /// synthetic drag reports its final point before mouse-up.
    struct Drag {
        DragKind kind = DragKind::Move;
        /// The note as the document carried it at press time. Empty for Insert.
        std::optional<timeline::NoteEvent> grabbed;
        /// Distance from the note's start to where the pointer grabbed it, so a
        /// move repositions the note rather than snapping its start to the
        /// cursor.
        std::int64_t grab_offset_ticks = 0;
        /// Where the press landed, for the Insert case and for deciding whether
        /// a gesture moved at all.
        timebase::TickPosition press_tick{};
        std::uint8_t press_pitch = 60;
        float press_y = 0.0f;
        /// Newest changed sample not yet emitted.
        std::optional<timeline::NoteEvent> pending;
        /// Replacement from the most recently emitted Begin or Update.
        std::optional<timeline::NoteEvent> last_emitted;
        /// Latest rejected sample, retained so release does not record the same
        /// refusal twice after the final drag callback already reported it.
        std::optional<timeline::NoteEvent> last_refused;
        /// The host declined Begin, so this gesture preserves compatibility by
        /// submitting one closed Single at release instead.
        bool release_only = false;
    };

    const timeline::MidiContent* content() const;
    /// Note under a point, resolved against the pointer tolerance. Returns the
    /// topmost match, which for a lattice is the one whose row the point is in.
    const timeline::NoteEvent* note_at(view::Point position) const;
    /// Whether a point is within the pointer tolerance of a note's trailing
    /// edge, which is what distinguishes a resize from a move.
    bool on_trailing_edge(const timeline::NoteEvent& note, view::Point position) const;

    /// Applies the layout's snap grid to an authored start.
    timebase::TickPosition snapped(timebase::TickPosition tick) const;
    /// Rejects a note the note domain or the bound clip would not accept, so a
    /// refusal is recorded here instead of a malformed intent crossing the seam.
    bool admissible(const timeline::NoteEvent& note);
    /// Resolves one pointer sample into the authored note for a transform.
    std::optional<timeline::NoteEvent> replacement_at(const Drag& drag,
                                                       view::Point position) const;

    bool emit(timeline_editor::NoteEditIntentKind kind, timeline::GesturePhase phase,
              std::optional<timeline::NoteEvent> expected,
              std::optional<timeline::NoteEvent> replacement);

    const timeline::Project* project_ = nullptr;
    timeline::ItemId sequence_id_{};
    timeline::ItemId track_id_{};
    timeline::ItemId clip_id_{};
    std::optional<PianoRollLayout> layout_;
    timeline_editor::NoteEditIntentHost* host_ = nullptr;
    view::HitMetrics metrics_ = view::HitMetrics::for_pointer(view::PointerType::mouse);
    NoteFactory note_factory_;
    std::optional<Drag> drag_;
    std::uint16_t current_modifiers_ = view::kModNone;
    bool continuous_gestures_ = false;
    std::vector<PianoRollRefusal> refusals_;
    /// Segment-tree maximum note end for each canonical note range. It is
    /// rebuilt with the borrowed content in `set_clip()` so a viewport query
    /// can prune an entire range even when some other range holds a very long
    /// note.
    std::vector<timebase::TickPosition> note_interval_max_end_;
    std::size_t painted_note_count_ = 0;
    std::size_t visited_candidate_count_ = 0;
};

/// @}

} // namespace pulp::timeline_view
