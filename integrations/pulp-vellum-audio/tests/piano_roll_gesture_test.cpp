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

TEST_CASE("Piano roll mouse and touch drags share one exact undoable document edit",
          "[timeline][piano-roll][parity]") {
    const auto mouse = exercise_device_drag(PointerType::mouse);
    const auto touch = exercise_device_drag(PointerType::touch);

    REQUIRE(mouse.edited != mouse.baseline);
    CHECK(mouse.intents == touch.intents);
    CHECK(mouse.edited == touch.edited);
    CHECK(mouse.undone == mouse.baseline);
    CHECK(touch.undone == touch.baseline);
    CHECK(mouse.redone == mouse.edited);
    CHECK(touch.redone == touch.edited);
}

TEST_CASE("Piano roll touch metrics distinguish an edge resize from a mouse body move",
          "[timeline][piano-roll][pointer]") {
    const auto project = make_note_project();
    const auto exercise = [&](PointerType pointer_type) {
        ScriptedUiHost<ValidatedNoteEditIntent> host;
        PianoRollView view;
        configure(view, project, host);
        view.set_hit_metrics(HitMetrics::for_pointer(pointer_type));

        ::vellum::view::View::SimulatedPointer pointer;
        pointer.type = pointer_type;
        view.simulate_drag({350.0f, y_of(62)}, {470.0f, y_of(62)}, 4, pointer);
        return std::vector<ValidatedNoteEditIntent>(host.intents().begin(), host.intents().end());
    };

    const auto mouse = exercise(PointerType::mouse);
    const auto touch = exercise(PointerType::touch);
    REQUIRE(mouse.size() == 4);
    REQUIRE(touch.size() == 4);
    CHECK(std::all_of(mouse.begin(), mouse.end(), [](const auto& intent) {
        return intent.value().kind == NoteEditIntentKind::Move;
    }));
    CHECK(std::all_of(touch.begin(), touch.end(), [](const auto& intent) {
        return intent.value().kind == NoteEditIntentKind::Resize;
    }));
}

TEST_CASE("Piano roll click inserts a note that survives a serialize round trip",
          "[timeline][piano-roll]") {
    auto session = Session::create(make_note_project());
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, session.project(), host);
    view.set_note_factory(factory_for({100}, 120));

    // Empty lattice space: tick 700, pitch 67.
    view.simulate_click({700.0f, y_of(67)});

    REQUIRE(host.intents().size() == 1);
    REQUIRE(host.intents().front().value().kind == NoteEditIntentKind::Insert);
    REQUIRE(host.intents().front().value().phase == GesturePhase::Single);
    REQUIRE(session.apply(host.intents().front()));

    const auto restored = notes_after_round_trip(session.project());
    REQUIRE(restored.size() == 4);
    const auto* inserted = find_note(restored, {100});
    REQUIRE(inserted != nullptr);
    // The values, not the count: a count of four survives a document that put
    // the note anywhere at all.
    CHECK(inserted->start.value == 700);
    CHECK(inserted->pitch == 67);
    CHECK(inserted->duration.value == 120);
    CHECK(inserted->velocity == 900);
    // The notes that were already there crossed unchanged.
    REQUIRE(find_note(restored, kNoteA) != nullptr);
    CHECK(find_note(restored, kNoteA)->start.value == 0);
    CHECK(find_note(restored, kNoteC)->pitch == 64);
}

TEST_CASE("Piano roll drag moves a note in time and pitch through the document",
          "[timeline][piano-roll]") {
    auto session = Session::create(make_note_project());
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, session.project(), host);

    // Grab note B (start 240, pitch 62) 10px in from its left edge, and drag
    // 120px right and two rows up, two semitones, since y decreases upward.
    view.simulate_drag({250.0f, y_of(62)}, {370.0f, y_of(64)}, 4);

    REQUIRE(host.intents().size() == 4);
    const std::array phases{GesturePhase::Begin, GesturePhase::Update, GesturePhase::Update,
                            GesturePhase::End};
    const std::array starts{270, 300, 330, 360};
    const std::array<std::uint8_t, 4> pitches{62, 63, 63, 64};
    NoteEvent expected{kNoteB, {240}, {120}, 1000, 62, 0};
    for (std::size_t index = 0; index < host.intents().size(); ++index) {
        const auto& intent = host.intents()[index].value();
        REQUIRE(intent.kind == NoteEditIntentKind::Move);
        REQUIRE(intent.phase == phases[index]);
        REQUIRE(intent.expected);
        REQUIRE(intent.replacement);
        CHECK(intent.expected->id == expected.id);
        CHECK(intent.expected->start == expected.start);
        CHECK(intent.expected->pitch == expected.pitch);
        CHECK(intent.replacement->id == kNoteB);
        CHECK(intent.replacement->start.value == starts[index]);
        CHECK(intent.replacement->pitch == pitches[index]);
        expected = *intent.replacement;
    }
    REQUIRE(session.apply_continuous(host.intents()));

    const auto restored = notes_after_round_trip(session.project());
    REQUIRE(restored.size() == 3);
    const auto* moved = find_note(restored, kNoteB);
    REQUIRE(moved != nullptr);
    CHECK(moved->start.value == 360);
    CHECK(moved->pitch == 64);
    // A move changes start and pitch and nothing else.
    CHECK(moved->duration.value == 120);
    CHECK(moved->velocity == 1000);
}

TEST_CASE("Piano roll edge drag resizes a note through the document", "[timeline][piano-roll]") {
    auto session = Session::create(make_note_project());
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, session.project(), host);

    // Note A runs [0, 120). Grab its trailing edge and drag out to tick 300.
    view.simulate_drag({120.0f, y_of(60)}, {300.0f, y_of(60)}, 4);

    REQUIRE(host.intents().size() == 4);
    const std::array phases{GesturePhase::Begin, GesturePhase::Update, GesturePhase::Update,
                            GesturePhase::End};
    const std::array durations{165, 210, 255, 300};
    NoteEvent expected{kNoteA, {0}, {120}, 1000, 60, 0};
    for (std::size_t index = 0; index < host.intents().size(); ++index) {
        const auto& intent = host.intents()[index].value();
        REQUIRE(intent.kind == NoteEditIntentKind::Resize);
        REQUIRE(intent.phase == phases[index]);
        REQUIRE(intent.expected);
        REQUIRE(intent.replacement);
        CHECK(intent.expected->duration == expected.duration);
        CHECK(intent.replacement->duration.value == durations[index]);
        expected = *intent.replacement;
    }
    REQUIRE(session.apply_continuous(host.intents()));

    const auto restored = notes_after_round_trip(session.project());
    const auto* resized = find_note(restored, kNoteA);
    REQUIRE(resized != nullptr);
    CHECK(resized->duration.value == 300);
    // A resize moves the trailing edge only.
    CHECK(resized->start.value == 0);
    CHECK(resized->pitch == 60);
}

TEST_CASE("Piano roll shift drag emits exact continuous velocity payloads",
          "[timeline][piano-roll][continuous]") {
    auto session = Session::create(make_note_project());
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, session.project(), host);

    ::vellum::view::View::SimulatedPointer pointer;
    pointer.modifiers = ::vellum::view::kModShift;
    view.simulate_drag({250.0f, y_of(62)}, {250.0f, y_of(62) - 40.0f}, 4, pointer);

    REQUIRE(host.intents().size() == 4);
    const std::array phases{GesturePhase::Begin, GesturePhase::Update, GesturePhase::Update,
                            GesturePhase::End};
    const std::array velocities{2280, 3560, 4840, 6120};
    NoteEvent expected{kNoteB, {240}, {120}, 1000, 62, 0};
    for (std::size_t index = 0; index < host.intents().size(); ++index) {
        const auto& intent = host.intents()[index].value();
        REQUIRE(intent.kind == NoteEditIntentKind::SetVelocity);
        REQUIRE(intent.phase == phases[index]);
        REQUIRE(intent.expected);
        REQUIRE(intent.replacement);
        CHECK(intent.expected->velocity == expected.velocity);
        CHECK(intent.replacement->velocity == velocities[index]);
        CHECK(intent.replacement->id == kNoteB);
        CHECK(intent.replacement->start.value == 240);
        CHECK(intent.replacement->pitch == 62);
        expected = *intent.replacement;
    }
    REQUIRE(session.apply_continuous(host.intents()));
    CHECK(find_note(session.notes(), kNoteB)->velocity == 6120);
}

TEST_CASE("Piano roll one-sample drag preserves the closed Single path",
          "[timeline][piano-roll][continuous]") {
    const auto project = make_note_project();
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, project, host);

    view.simulate_drag({250.0f, y_of(62)}, {370.0f, y_of(64)}, 1);

    REQUIRE(host.intents().size() == 1);
    CHECK(host.intents().front().value().kind == NoteEditIntentKind::Move);
    CHECK(host.intents().front().value().phase == GesturePhase::Single);
    REQUIRE(host.intents().front().value().replacement);
    CHECK(host.intents().front().value().replacement->start.value == 360);
    CHECK(host.intents().front().value().replacement->pitch == 64);
}

TEST_CASE("Continuous note edits preserve identity modifiers seed and byte-exact history",
          "[timeline][piano-roll][continuous]") {
    auto session = Session::create(make_modified_note_project());
    const auto baseline = canonical_project(session.project());
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, session.project(), host);

    view.simulate_drag({250.0f, y_of(62)}, {370.0f, y_of(64)}, 4);
    REQUIRE(session.apply_continuous(host.intents()));
    const auto edited = canonical_project(session.project());
    REQUIRE(edited != baseline);

    const auto& edited_content = std::get<MidiContent>(session.project()
                                                           .find_sequence(kSequence)
                                                           ->find_track(kTrack)
                                                           ->find_clip(kClip)
                                                           ->content());
    REQUIRE(edited_content.modifier_for(kNoteB));
    CHECK(edited_content.modifier_for(kNoteB)->probability == 4096);
    CHECK(edited_content.modifier_for(kNoteB)->ratchet_count == 2);
    CHECK(edited_content.modifier_seed() == 0xBEEF);
    CHECK(find_note(edited_content.notes(), kNoteB)->id == kNoteB);

    REQUIRE(session.session->undo(session.writer));
    CHECK(canonical_project(session.project()) == baseline);
    REQUIRE(session.session->redo(session.writer));
    CHECK(canonical_project(session.project()) == edited);
}

TEST_CASE("Cancelling an open piano roll transform restores canonical bytes",
          "[timeline][piano-roll][continuous]") {
    auto session = Session::create(make_note_project());
    const auto baseline = canonical_project(session.project());
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, session.project(), host);

    view.on_mouse_down({250.0f, y_of(62)});
    view.on_mouse_drag({280.0f, y_of(62)});
    view.on_mouse_drag({310.0f, y_of(62)});
    view.on_mouse_cancel({310.0f, y_of(62)});

    REQUIRE(host.intents().size() == 2);
    CHECK(host.intents()[0].value().phase == GesturePhase::Begin);
    CHECK(host.intents()[1].value().phase == GesturePhase::Cancel);
    REQUIRE(session.apply_continuous(host.intents()));
    CHECK(canonical_project(session.project()) == baseline);
}

TEST_CASE("Continuous piano roll gestures survive accepted synchronous snapshot rebinds",
          "[timeline][piano-roll][continuous]") {
    auto session = Session::create(make_note_project());
    const auto baseline = canonical_project(session.project());
    CallbackNoteHost host;
    PianoRollView view;
    configure(view, session.project(), host);
    const auto group = session.writer.allocate_undo_group_id();
    std::uint64_t accepted_sequence = 0;
    host.callback = [&](const ValidatedNoteEditIntent& intent) {
        EditIntentIdentity identity;
        identity.transaction_id = session.writer.allocate_transaction_id();
        identity.command_id = session.writer.allocate_command_id();
        identity.expected_revision = session.session->revision();
        identity.undo_group = group;
        auto lowered = lower_note_edit_intent(intent, session.notes(), identity);
        if (!lowered || !session.session->submit(session.writer, std::move(lowered).value()))
            return IntentResult{IntentStatus::Rejected, 0};
        view.set_clip(&session.project(), kSequence, kTrack, kClip);
        return IntentResult{IntentStatus::Accepted, ++accepted_sequence};
    };

    view.simulate_drag({250.0f, y_of(62)}, {370.0f, y_of(64)}, 4);

    REQUIRE(host.intents.size() == 4);
    CHECK(host.intents.front().value().phase == GesturePhase::Begin);
    CHECK(host.intents.back().value().phase == GesturePhase::End);
    CHECK_FALSE(view.gesture_open());
    CHECK(find_note(session.notes(), kNoteB)->start.value == 360);
    CHECK(find_note(session.notes(), kNoteB)->pitch == 64);
    const auto edited = canonical_project(session.project());
    REQUIRE(session.session->undo(session.writer));
    CHECK(canonical_project(session.project()) == baseline);
    REQUIRE(session.session->redo(session.writer));
    CHECK(canonical_project(session.project()) == edited);
}

TEST_CASE("Piano roll closes through Single when the host does not accept Begin",
          "[timeline][piano-roll][continuous]") {
    for (const auto status : {IntentStatus::Deferred, IntentStatus::Rejected}) {
        const auto project = make_note_project();
        ScriptedUiHost<ValidatedNoteEditIntent> host;
        host.set_intent_status(status);
        PianoRollView view;
        configure(view, project, host);

        view.simulate_drag({250.0f, y_of(62)}, {370.0f, y_of(64)}, 4);

        REQUIRE(host.intents().size() == 2);
        CHECK(host.intents().front().value().phase == GesturePhase::Begin);
        CHECK(host.intents().back().value().phase == GesturePhase::Single);
        CHECK_FALSE(view.gesture_open());
    }
}

TEST_CASE("A release-only host retains existing piano roll drag behavior",
          "[timeline][piano-roll][continuous]") {
    auto session = Session::create(make_note_project());
    CallbackNoteHost host;
    PianoRollView view;
    configure(view, session.project(), host);
    host.callback = [&](const ValidatedNoteEditIntent& intent) {
        if (intent.value().phase != GesturePhase::Single)
            return IntentResult{IntentStatus::Rejected, 0};
        if (!session.apply(intent))
            return IntentResult{IntentStatus::Rejected, 0};
        view.set_clip(&session.project(), kSequence, kTrack, kClip);
        return IntentResult{IntentStatus::Accepted, 1};
    };

    view.simulate_drag({250.0f, y_of(62)}, {370.0f, y_of(64)}, 4);

    REQUIRE(host.intents.size() == 2);
    CHECK(host.intents.front().value().phase == GesturePhase::Begin);
    CHECK(host.intents.back().value().phase == GesturePhase::Single);
    CHECK(find_note(session.notes(), kNoteB)->start.value == 360);
    CHECK(find_note(session.notes(), kNoteB)->pitch == 64);
}

TEST_CASE("Piano roll secondary click erases a note and leaves the rest authored",
          "[timeline][piano-roll]") {
    auto session = Session::create(make_note_project());
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, session.project(), host);

    ::vellum::view::View::SimulatedPointer secondary;
    secondary.button = MouseButton::right;
    view.simulate_click({500.0f, y_of(64)}, secondary);

    REQUIRE(host.intents().size() == 1);
    REQUIRE(host.intents().front().value().kind == NoteEditIntentKind::Erase);
    REQUIRE(session.apply(host.intents().front()));

    const auto restored = notes_after_round_trip(session.project());
    REQUIRE(restored.size() == 2);
    CHECK(find_note(restored, kNoteC) == nullptr);
    // The surviving notes kept their authored values, which a size check alone
    // would not see.
    REQUIRE(find_note(restored, kNoteA) != nullptr);
    CHECK(find_note(restored, kNoteA)->start.value == 0);
    CHECK(find_note(restored, kNoteA)->pitch == 60);
    REQUIRE(find_note(restored, kNoteB) != nullptr);
    CHECK(find_note(restored, kNoteB)->start.value == 240);
    CHECK(find_note(restored, kNoteB)->pitch == 62);
}
