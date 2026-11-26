#include "piano_roll_fixtures.hpp"

#include <vellum/canvas/recording_canvas.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace piano_roll_test;
using ::vellum::canvas::DrawCommand;
using ::vellum::canvas::RecordingCanvas;

TEST_CASE("Piano roll refuses a move that would push a note out of its clip",
          "[timeline][piano-roll]") {
    auto session = Session::create(make_note_project());
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, session.project(), host);

    // Note A runs [0, 120) in a clip that ends at 960. Grab it 10px in and drag
    // to the right edge: the release tick clamps to 960, so the note would start
    // at 950 and end at 1070, past its own clip. That is refused rather than
    // clamped, because clamping produces a different musical result than the one
    // dragged for, with no signal that it happened.
    view.simulate_drag({10.0f, y_of(60)}, {960.0f, y_of(60)}, 4);

    REQUIRE(host.intents().size() == 3);
    CHECK(host.intents().front().value().phase == GesturePhase::Begin);
    CHECK(host.intents().back().value().phase == GesturePhase::Cancel);
    REQUIRE(view.refusals().size() == 1);
    CHECK(view.refusals().front() == PianoRollRefusal::OutsideClip);
    REQUIRE(session.apply_continuous(host.intents()));
    // The open group was restored before it closed.
    CHECK(find_note(session.notes(), kNoteA)->start.value == 0);
}

TEST_CASE("Piano roll resize stops at the clip end because the projection clamps",
          "[timeline][piano-roll]") {
    auto session = Session::create(make_note_project());
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, session.project(), host);

    // The counterpart to the case above, and the reason it uses a move: a
    // resize reads its new end straight from `tick_at`, which clamps to the
    // visible end, so dragging an edge past the right of the roll lands exactly
    // on the clip end rather than past it. A note ending where its clip ends is
    // legal, so this succeeds where the move was refused.
    view.simulate_drag({600.0f, y_of(64)}, {2000.0f, y_of(64)}, 4);
    REQUIRE(host.intents().size() == 2);
    CHECK(host.intents().front().value().phase == GesturePhase::Begin);
    CHECK(host.intents().back().value().phase == GesturePhase::End);
    REQUIRE(view.refusals().empty());
    REQUIRE(session.apply_continuous(host.intents()));

    const auto restored = notes_after_round_trip(session.project());
    const auto* resized = find_note(restored, kNoteC);
    REQUIRE(resized != nullptr);
    CHECK(resized->start.value == 480);
    CHECK(resized->duration.value == kClipTicks - 480);
}

TEST_CASE("A piano roll bound to a non-MIDI clip stays inert", "[timeline][piano-roll]") {
    // An audio or nested-sequence clip carries no note lattice. The roll draws
    // nothing and hit-tests nothing rather than inventing content for it.
    auto project = make_note_project();
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, project, host);
    view.set_note_factory(factory_for({100}, 120));

    // Rebind to a clip identity the project does not carry, which is the same
    // shape of miss as a clip whose content is not MidiContent.
    view.set_clip(&project, kSequence, kTrack, {999});
    RecordingCanvas canvas;
    view.paint(canvas);
    CHECK(view.painted_note_count() == 0);
    CHECK(view.notes().empty());

    view.simulate_click({700.0f, y_of(67)});
    CHECK(host.intents().empty());
}

TEST_CASE("Lowering emits a granular command for continuous and Single note gestures",
          "[timeline][piano-roll]") {
    auto session = Session::create(make_note_project());

    NoteEditIntent raw;
    raw.kind = NoteEditIntentKind::Move;
    raw.phase = GesturePhase::Begin;
    raw.sequence_id = kSequence;
    raw.track_id = kTrack;
    raw.clip_id = kClip;
    raw.expected = NoteEvent{kNoteB, {240}, {120}, 1000, 62, 0};
    raw.replacement = NoteEvent{kNoteB, {360}, {120}, 1000, 62, 0};
    auto validated = ValidatedNoteEditIntent::create(raw);
    REQUIRE(validated);

    EditIntentIdentity identity;
    identity.transaction_id = TransactionId{WriterId{1}, 1};
    identity.command_id = CommandId{WriterId{1}, 1};
    identity.undo_group = UndoGroupId{WriterId{1}, 1};

    auto lowered = lower_note_edit_intent(validated.value(), session.notes(), identity);
    REQUIRE(lowered);
    CHECK(lowered->gesture_phase == GesturePhase::Begin);
    REQUIRE(lowered->commands.size() == 1);
    CHECK(std::holds_alternative<SetNoteEvents>(lowered->commands[0].command));

    // Commit-on-release remains supported by the same granular command path.
    raw.phase = GesturePhase::Single;
    auto single = ValidatedNoteEditIntent::create(raw);
    REQUIRE(single);
    REQUIRE(session.apply(single.value()));
    CHECK(find_note(session.notes(), kNoteB)->start.value == 360);
}

TEST_CASE("Lowering refuses a stale expected note instead of overwriting it",
          "[timeline][piano-roll]") {
    auto session = Session::create(make_note_project());

    NoteEditIntent raw;
    raw.kind = NoteEditIntentKind::Move;
    raw.phase = GesturePhase::Begin;
    raw.sequence_id = kSequence;
    raw.track_id = kTrack;
    raw.clip_id = kClip;
    // A pitch the document never carried for this identity: the view that built
    // this was looking at a project revision that has moved on.
    raw.expected = NoteEvent{kNoteB, {240}, {120}, 1000, 71, 0};
    raw.replacement = NoteEvent{kNoteB, {360}, {120}, 1000, 71, 0};
    auto validated = ValidatedNoteEditIntent::create(raw);
    REQUIRE(validated);

    EditIntentIdentity identity;
    identity.transaction_id = TransactionId{WriterId{1}, 1};
    identity.command_id = CommandId{WriterId{1}, 1};
    identity.undo_group = UndoGroupId{WriterId{1}, 1};

    auto lowered = lower_note_edit_intent(validated.value(), session.notes(), identity);
    REQUIRE_FALSE(lowered);
    CHECK(lowered.error() == NoteLoweringError::ExpectedNoteMismatch);
    // The document is untouched, a refusal is not a partial edit.
    CHECK(find_note(session.notes(), kNoteB)->pitch == 62);
}

// Every refusal below names the gesture that produces it. A refusal no gesture
// can reach is dead code that reads as rigour, and the OutsideClip case above
// was exactly that until it was rewritten around a move, so the whole set is
// enumerated here rather than sampled.
//
// SCOPE, stated because it is easy to read these as stronger than they are: a
// view-side case asserts THAT the gesture is refused, not WHICH check refused
// it. Note-domain validity is checked in `admissible` and again by
// `ValidatedNoteEditIntent::create` at the seam, both reporting `InvalidNote`,
// so no assertion here can separate the layers, removing either one leaves
// these cases green. That is the correct behavioural claim (a bad gesture never
// reaches the document) and deliberately not a claim about layering. The
// lowering cases below ARE layer-specific, because each returns its own error.

TEST_CASE("Piano roll refuses a factory note the note domain rejects", "[timeline][piano-roll]") {
    const auto project = make_note_project();
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, project, host);
    // A zero-duration note is not a note. A factory is caller code, so this is
    // the gesture that reaches the note-domain refusal.
    view.set_note_factory(factory_for({100}, 0));

    view.simulate_click({700.0f, y_of(67)});
    CHECK(host.intents().empty());
    REQUIRE(view.refusals().size() == 1);
    CHECK(view.refusals().front() == PianoRollRefusal::InvalidNote);
}

TEST_CASE("Piano roll refuses a resize dragged back past its own note start",
          "[timeline][piano-roll]") {
    const auto project = make_note_project();
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, project, host);

    // Note A runs [0, 120). Drag its trailing edge left to the roll origin: the
    // new end lands on its own start, so the duration would be zero.
    view.simulate_drag({120.0f, y_of(60)}, {0.0f, y_of(60)}, 4);
    REQUIRE(host.intents().size() == 3);
    CHECK(host.intents().front().value().phase == GesturePhase::Begin);
    CHECK(host.intents().back().value().phase == GesturePhase::Cancel);
    REQUIRE(view.refusals().size() == 1);
    CHECK(view.refusals().front() == PianoRollRefusal::InvalidNote);
}

TEST_CASE("Lowering refuses an insert whose identity the clip already carries",
          "[timeline][piano-roll]") {
    auto session = Session::create(make_note_project());
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, session.project(), host);
    // A factory that re-mints a live identity, the shape of an id allocator
    // that was rewound, or a paste that forgot to remap.
    view.set_note_factory(factory_for(kNoteA, 120));

    view.simulate_click({700.0f, y_of(67)});
    REQUIRE(host.intents().size() == 1);

    EditIntentIdentity identity;
    identity.transaction_id = TransactionId{WriterId{1}, 1};
    identity.command_id = CommandId{WriterId{1}, 1};
    auto lowered = lower_note_edit_intent(host.intents().front(), session.notes(), identity);
    REQUIRE_FALSE(lowered);
    CHECK(lowered.error() == NoteLoweringError::DuplicateNoteIdentity);
}

TEST_CASE("Lowering refuses an edit naming a note the clip does not carry",
          "[timeline][piano-roll]") {
    auto session = Session::create(make_note_project());

    // The gesture: a roll still pointed at a project revision in which this note
    // existed, after another writer erased it.
    NoteEditIntent raw;
    raw.kind = NoteEditIntentKind::Move;
    raw.phase = GesturePhase::Single;
    raw.sequence_id = kSequence;
    raw.track_id = kTrack;
    raw.clip_id = kClip;
    raw.expected = NoteEvent{{60}, {240}, {120}, 1000, 62, 0};
    raw.replacement = NoteEvent{{60}, {360}, {120}, 1000, 62, 0};
    auto validated = ValidatedNoteEditIntent::create(raw);
    REQUIRE(validated);

    EditIntentIdentity identity;
    identity.transaction_id = TransactionId{WriterId{1}, 1};
    identity.command_id = CommandId{WriterId{1}, 1};
    auto lowered = lower_note_edit_intent(validated.value(), session.notes(), identity);
    REQUIRE_FALSE(lowered);
    CHECK(lowered.error() == NoteLoweringError::NoteNotInClip);
}

TEST_CASE("Lowering refuses a malformed transaction identity", "[timeline][piano-roll]") {
    auto session = Session::create(make_note_project());

    NoteEditIntent raw;
    raw.kind = NoteEditIntentKind::Erase;
    raw.phase = GesturePhase::Single;
    raw.sequence_id = kSequence;
    raw.track_id = kTrack;
    raw.clip_id = kClip;
    raw.expected = NoteEvent{kNoteB, {240}, {120}, 1000, 62, 0};
    auto validated = ValidatedNoteEditIntent::create(raw);
    REQUIRE(validated);

    // The gesture: a caller that submitted before allocating, or one whose
    // transaction and command came from different writers.
    EditIntentIdentity mismatched;
    mismatched.transaction_id = TransactionId{WriterId{1}, 1};
    mismatched.command_id = CommandId{WriterId{2}, 1};
    auto lowered = lower_note_edit_intent(validated.value(), session.notes(), mismatched);
    REQUIRE_FALSE(lowered);
    CHECK(lowered.error() == NoteLoweringError::InvalidIdentity);

    // Same edit, well-formed identity: the refusal is about the identity.
    EditIntentIdentity valid;
    valid.transaction_id = TransactionId{WriterId{1}, 1};
    valid.command_id = CommandId{WriterId{1}, 1};
    REQUIRE(lower_note_edit_intent(validated.value(), session.notes(), valid));
}

// Honest scope: this asserts the gesture is REFUSED, which it is with or without
// the ordering guard in `admissible`, the downstream note-domain validation
// catches the same note. What the guard buys is that the bounds check does not
// perform the signed overflow it exists to prevent, and the only instrument that
// separates those two is a sanitizer build, not this case. Kept because an
// extreme note reaching the seam is worth pinning either way; NOT evidence for
// the guard.

TEST_CASE("Piano roll refuses a factory note at the far edge of the tick domain",
          "[timeline][piano-roll]") {
    const auto project = make_note_project();
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, project, host);
    // start + duration must be computed to bounds-check a note against its clip,
    // so the overflow test has to come first or the check performs the overflow
    // it exists to prevent.
    view.set_note_factory(
        [](pulp::timebase::TickPosition, std::uint8_t pitch) -> std::optional<NoteEvent> {
            return NoteEvent{{100},
                             {std::numeric_limits<std::int64_t>::max() - 1},
                             {std::numeric_limits<std::int64_t>::max() - 1},
                             900,
                             pitch,
                             0};
        });

    view.simulate_click({700.0f, y_of(67)});
    CHECK(host.intents().empty());
    REQUIRE(view.refusals().size() == 1);
    CHECK(view.refusals().front() == PianoRollRefusal::InvalidNote);
}
