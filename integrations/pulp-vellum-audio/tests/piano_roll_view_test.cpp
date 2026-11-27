#include "piano_roll_fixtures.hpp"

#include <pulp/vellum_audio/piano_roll_pitch_ruler.hpp>
#include <vellum/canvas/recording_canvas.hpp>
#include <vellum/view/screenshot.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

using namespace piano_roll_test;
using ::vellum::canvas::DrawCommand;
using ::vellum::canvas::RecordingCanvas;

TEST_CASE("Piano roll culling admits one more note when the viewport scrolls onto it",
          "[timeline][piano-roll]") {
    const auto project = make_note_project();
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, project, host);

    // Two viewport states, opposite outcomes, the SAME note. A single static
    // count assertion passes both a renderer that draws nothing and one that
    // ignores the viewport entirely, so neither state is asserted alone.
    //
    // [0, 240) ends exactly where note B starts, so B is excluded.
    view.set_layout(layout_over(0, 240, 240.0f));
    RecordingCanvas before;
    view.paint(before);
    const auto drawn_before = view.painted_note_count();
    REQUIRE(drawn_before == 1);
    REQUIRE_FALSE(view.note_rect(kNoteB) == std::nullopt);

    // Scroll one tick so B's start falls inside. Nothing else changed.
    view.set_layout(layout_over(1, 240, 240.0f));
    RecordingCanvas after;
    view.paint(after);
    REQUIRE(view.painted_note_count() == drawn_before + 1);

    // Assert the ink, not just the tally: B's rectangle is absent from the
    // first recording and present in the second.
    const auto rect = view.note_rect(kNoteB);
    REQUIRE(rect);
    const auto has_rect_at = [](const RecordingCanvas& canvas, float x) {
        for (const auto& command : canvas.commands())
            if (command.type == DrawCommand::Type::fill_rect && command.f[0] == x)
                return true;
        return false;
    };
    CHECK_FALSE(has_rect_at(before, rect->x));
    CHECK(has_rect_at(after, rect->x));
}

TEST_CASE("Piano roll paint bounds candidate work for ten thousand real notes",
          "[timeline][piano-roll][culling]") {
    const auto project = make_dense_note_project();
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    view.set_bounds({0, 0, 40.0f, kRollHeight});
    view.set_clip(&project, kSequence, kTrack, kClip);
    view.set_host(&host);

    REQUIRE(view.notes().size() == kDenseNoteCount);
    const auto has_rect_at = [](const RecordingCanvas& recording, float x) {
        return std::any_of(recording.commands().begin(), recording.commands().end(),
                           [x](const DrawCommand& command) {
                               return command.type == DrawCommand::Type::fill_rect &&
                                      command.f[0] == x;
                           });
    };

    const auto check_viewport = [&](std::int64_t visible_start) {
        view.set_layout(layout_over(visible_start, 40, 40.0f));
        RecordingCanvas canvas;
        view.paint(canvas);
        CHECK(view.painted_note_count() == 4);
        CHECK(view.visited_candidate_count() == 4);
        CHECK(has_rect_at(canvas, 30.0f));
        CHECK_FALSE(has_rect_at(canvas, 40.0f));
    };

    check_viewport(0);
    check_viewport(50'000);
    check_viewport(99'950);

    view.set_clip(&project, kSequence, kTrack, {999});
    RecordingCanvas rebound;
    view.paint(rebound);
    CHECK(view.painted_note_count() == 0);
    CHECK(view.visited_candidate_count() == 0);
}

TEST_CASE("Piano roll pitch ruler shares projection rows and emits audition requests",
          "[timeline][piano-roll][ruler]") {
    auto projection = PitchProjection::create(kLowPitch, kHighPitch, PixelSpan{32.0f, kRollHeight});
    REQUIRE(projection);

    PianoRollPitchRuler ruler;
    ruler.set_bounds({7.0f, 0.0f, 64.0f, 10.0f});
    ruler.set_pitch_projection(projection.value());

    CHECK(ruler.orientation() == MidiKeyboard::Orientation::vertical_chromatic_rows);
    CHECK(ruler.show_note_names());
    CHECK(ruler.first_note() == kLowPitch);
    CHECK(ruler.last_note() == kHighPitch);
    CHECK(ruler.bounds().x == 7.0f);
    CHECK(ruler.bounds().y == 32.0f);
    CHECK(ruler.bounds().width == 64.0f);
    CHECK(ruler.bounds().height == kRollHeight);

    std::vector<int> note_ons;
    std::vector<int> note_offs;
    float velocity = 0.0f;
    ruler.on_note_on = [&](int note, float requested_velocity) {
        note_ons.push_back(note);
        velocity = requested_velocity;
    };
    ruler.on_note_off = [&](int note) { note_offs.push_back(note); };

    const float local_c4_y = projection.value().y_at(60) - projection.value().pixels().origin;
    ruler.simulate_drag({12.0f, local_c4_y},
                        {12.0f, projection.value().y_at(62) - projection.value().pixels().origin},
                        1);

    REQUIRE(note_ons == std::vector<int>{60, 62});
    CHECK(note_offs == std::vector<int>{60, 62});
    CHECK(velocity == 0.8f);
}

TEST_CASE("Piano roll renders ten thousand notes offscreen and culls to the viewport",
          "[timeline][piano-roll][culling][render]") {
    const auto project = make_dense_note_project();
    PianoRollView view;
    view.set_bounds({0.0f, 0.0f, 40.0f, kRollHeight});
    view.set_clip(&project, kSequence, kTrack, kClip);
    view.set_layout(layout_over(50'000, 40, 40.0f));

    std::uint32_t rgba_width = 0;
    std::uint32_t rgba_height = 0;
    const auto rgba =
        ::vellum::view::render_to_rgba(view, 40, 480, 1.0f, &rgba_width, &rgba_height);
    REQUIRE_FALSE(rgba.empty());
    CHECK(rgba_width == 40);
    CHECK(rgba_height == 480);
    CHECK(view.painted_note_count() == 4);
    CHECK(view.visited_candidate_count() == 4);
    CHECK(fnv1a64(rgba) == 0x4e52bc808038c4a5ULL);
}

TEST_CASE("Piano roll culling stays bounded with one long note among ten thousand short notes",
          "[timeline][piano-roll][culling]") {
    const auto project = make_dense_note_project(true);
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    view.set_bounds({0, 0, 40.0f, kRollHeight});
    view.set_clip(&project, kSequence, kTrack, kClip);
    view.set_host(&host);
    view.set_layout(layout_over(50'000, 40, 40.0f));

    RecordingCanvas canvas;
    view.paint(canvas);

    CHECK(view.painted_note_count() == 5);
    CHECK(view.visited_candidate_count() == 5);
}

TEST_CASE("Piano roll paints a valid negative-start note that crosses tick zero",
          "[timeline][piano-roll][culling]") {
    const auto project = make_negative_start_note_project();
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    view.set_bounds({0, 0, 10.0f, kRollHeight});
    view.set_clip(&project, kSequence, kTrack, kClip);
    view.set_host(&host);
    view.set_layout(layout_over(0, 10, 10.0f));

    RecordingCanvas canvas;
    view.paint(canvas);

    CHECK(view.painted_note_count() == 1);
    CHECK(view.visited_candidate_count() == 1);
}

TEST_CASE("Piano roll geometry follows the projections it is handed", "[timeline][piano-roll]") {
    const auto project = make_note_project();
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, project, host);

    const auto full = view.note_rect(kNoteB);
    REQUIRE(full);
    CHECK(full->x == 240.0f);
    CHECK(full->width == 120.0f);
    CHECK(full->height == 20.0f);

    // Halving the pixel extent halves every width; the view owns no zoom policy
    // of its own, so this is a pure consequence of the projection handed in.
    view.set_layout(layout_over(0, kClipTicks, static_cast<float>(kClipTicks) / 2.0f));
    const auto halved = view.note_rect(kNoteB);
    REQUIRE(halved);
    CHECK(halved->x == 120.0f);
    CHECK(halved->width == 60.0f);
}

TEST_CASE("Piano roll fixture draws each note on its pitch row over the shaded lattice",
          "[timeline][piano-roll][render]") {
    const auto project = make_note_project();
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, project, host);

    constexpr std::uint32_t width = 960;
    constexpr std::uint32_t height = 480;
    std::uint32_t out_width = 0;
    std::uint32_t out_height = 0;
    const auto rgba =
        ::vellum::view::render_to_rgba(view, width, height, 1.0f, &out_width, &out_height);
    REQUIRE(out_width == width);
    REQUIRE(out_height == height);
    if (const char* dir = std::getenv("PULP_VELLUM_AUDIO_PNG_DIR"))
        CHECK(::vellum::view::render_to_file(
            view, width, height, std::string(dir) + "/piano_roll_three_notes.png", 2.0f));

    const auto near = [&](std::uint32_t x, std::uint32_t y, int r, int g, int b) {
        const auto o = (static_cast<std::size_t>(y) * width + x) * 4;
        INFO("pixel (" << x << ", " << y << ") = " << int(rgba[o]) << ", " << int(rgba[o + 1])
                       << ", " << int(rgba[o + 2]));
        return std::abs(int(rgba[o]) - r) <= 4 && std::abs(int(rgba[o + 1]) - g) <= 4 &&
               std::abs(int(rgba[o + 2]) - b) <= 4;
    };
    // Rows are 20px with pitch 71 on top, so pitch p spans y = (71 - p) * 20 to
    // +20. Each note is probed at both edges of its own row and one pixel
    // outside it, so a rectangle that is shifted or resized cannot pass.
    const auto note_rows = [&](std::uint32_t x, std::uint32_t top, int above_r, int above_g,
                               int above_b, int below_r, int below_g, int below_b) {
        CHECK(near(x, top + 2, 89, 184, 115));
        CHECK(near(x, top + 18, 89, 184, 115));
        CHECK(near(x, top - 2, above_r, above_g, above_b));
        CHECK(near(x, top + 22, below_r, below_g, below_b));
    };
    note_rows(60, 220, 20, 23, 28, 28, 31, 36);  // A, pitch 60: above 61 (black), below 59
    note_rows(300, 180, 20, 23, 28, 20, 23, 28); // B, pitch 62: above 63 and below 61 (black)
    note_rows(540, 140, 28, 31, 36, 20, 23, 28); // C, pitch 64: above 65, below 63 (black)
    CHECK(near(300, 230, 28, 31, 36));           // pitch 60 row where no note sounds
    CHECK(near(60, 190, 28, 31, 36));            // note B's row is clear at note A's time
    CHECK(near(130, 230, 28, 31, 36));           // note A ends at x = 120
}

namespace {

using pulp::timeline_editor::IntentStatus;
using pulp::timeline_editor::NoteEditIntentKind;
using ::vellum::view::HitMetrics;
using ::vellum::view::PointerType;

const NoteEvent& note_in(const std::vector<NoteEvent>& notes, ItemId id) {
    const auto* found = find_note(notes, id);
    REQUIRE(found != nullptr);
    return *found;
}

bool same_notes(const std::vector<NoteEvent>& left, const std::vector<NoteEvent>& right) {
    return std::equal(left.begin(), left.end(), right.begin(), right.end(),
                      [](const NoteEvent& a, const NoteEvent& b) {
                          return a.id == b.id && a.start == b.start && a.duration == b.duration &&
                                 a.velocity == b.velocity && a.pitch == b.pitch &&
                                 a.channel == b.channel;
                      });
}

std::vector<GesturePhase> phases_of(const GestureRun& run) {
    std::vector<GesturePhase> phases;
    for (const auto& intent : run.intents)
        phases.push_back(intent.value().phase);
    return phases;
}

::vellum::view::View::SimulatedPointer pointer_of(PointerType type) {
    ::vellum::view::View::SimulatedPointer pointer;
    pointer.type = type;
    return pointer;
}

} // namespace

TEST_CASE("Piano roll move drag commits one grouped edit that round-trips and undoes",
          "[timeline][piano-roll][gesture]") {
    for (const auto type : {PointerType::mouse, PointerType::touch}) {
        const auto run = run_gesture(make_note_project(), [&](PianoRollView& view) {
            view.set_hit_metrics(HitMetrics::for_pointer(type));
            view.simulate_drag({250.0f, y_of(62)}, {370.0f, y_of(64)}, 4, pointer_of(type));
        });

        REQUIRE(run.intents.size() == 4);
        CHECK(phases_of(run) == std::vector<GesturePhase>{GesturePhase::Begin, GesturePhase::Update,
                                                          GesturePhase::Update, GesturePhase::End});
        for (const auto& intent : run.intents)
            CHECK(intent.value().kind == NoteEditIntentKind::Move);

        // The press landed 10 ticks into the note, so a release at x=370 puts
        // its start at 360.
        const auto& before = note_in(run.baseline_notes, kNoteB);
        const auto& after = note_in(run.edited_notes, kNoteB);
        CHECK(before.start.value == 240);
        CHECK(before.pitch == 62);
        CHECK(after.start.value == 360);
        CHECK(after.pitch == 64);
        CHECK(after.duration.value == before.duration.value);
        CHECK(note_in(run.edited_notes, kNoteA).start.value == 0);

        CHECK(run.edited != run.baseline);
        CHECK(run.undone == run.baseline);
        CHECK(run.redone == run.edited);
    }
}

TEST_CASE("Piano roll resize drag changes only the duration", "[timeline][piano-roll][gesture]") {
    const auto run = run_gesture(make_note_project(), [](PianoRollView& view) {
        // Note A ends at x=120, so a press there grabs its trailing edge.
        view.simulate_drag({120.0f, y_of(60)}, {200.0f, y_of(60)}, 4);
    });

    REQUIRE_FALSE(run.intents.empty());
    for (const auto& intent : run.intents)
        CHECK(intent.value().kind == NoteEditIntentKind::Resize);
    CHECK(run.intents.back().value().phase == GesturePhase::End);

    const auto& before = note_in(run.baseline_notes, kNoteA);
    const auto& after = note_in(run.edited_notes, kNoteA);
    CHECK(before.duration.value == 120);
    CHECK(after.duration.value == 200);
    CHECK(after.start.value == before.start.value);
    CHECK(after.pitch == before.pitch);
    CHECK(run.undone == run.baseline);
    CHECK(run.redone == run.edited);
}

TEST_CASE("Piano roll shift drag sets velocity from the vertical distance",
          "[timeline][piano-roll][gesture]") {
    const auto run = run_gesture(make_note_project(), [](PianoRollView& view) {
        auto pointer = pointer_of(PointerType::mouse);
        pointer.modifiers = ::vellum::view::kModShift;
        // Two pixels up is 256 velocity units.
        view.simulate_drag({60.0f, y_of(60)}, {60.0f, y_of(60) - 2.0f}, 2, pointer);
    });

    REQUIRE_FALSE(run.intents.empty());
    for (const auto& intent : run.intents)
        CHECK(intent.value().kind == NoteEditIntentKind::SetVelocity);

    const auto& before = note_in(run.baseline_notes, kNoteA);
    const auto& after = note_in(run.edited_notes, kNoteA);
    CHECK(before.velocity == 1000);
    CHECK(after.velocity == 1256);
    CHECK(after.start.value == before.start.value);
    CHECK(after.pitch == before.pitch);
    CHECK(run.undone == run.baseline);
    CHECK(run.redone == run.edited);
}

TEST_CASE("Piano roll click on empty lattice inserts the factory's note",
          "[timeline][piano-roll][gesture]") {
    const auto run = run_gesture(
        make_note_project(), [](PianoRollView& view) { view.simulate_click({700.0f, y_of(66)}); },
        factory_for({100}, 60));

    REQUIRE(run.intents.size() == 1);
    CHECK(run.intents.front().value().kind == NoteEditIntentKind::Insert);
    CHECK(run.intents.front().value().phase == GesturePhase::Single);
    CHECK(run.baseline_notes.size() == 3);
    REQUIRE(run.edited_notes.size() == 4);
    const auto& created = note_in(run.edited_notes, {100});
    CHECK(created.start.value == 700);
    CHECK(created.pitch == 66);
    CHECK(created.duration.value == 60);
    // The document's id allocator does not step back, so undo is checked on
    // the notes, not on the whole document.
    CHECK(same_notes(run.undone_notes, run.baseline_notes));
    CHECK(run.redone == run.edited);
}

TEST_CASE("Piano roll refuses an inserted note that runs past the clip",
          "[timeline][piano-roll][gesture]") {
    const auto run = run_gesture(
        make_note_project(), [](PianoRollView& view) { view.simulate_click({900.0f, y_of(66)}); },
        factory_for({100}, 300));

    CHECK(run.intents.empty());
    CHECK(run.refusals == std::vector<PianoRollRefusal>{PianoRollRefusal::OutsideClip});
    CHECK(run.edited == run.baseline);
}

TEST_CASE("Piano roll right click erases the note under it", "[timeline][piano-roll][gesture]") {
    const auto run = run_gesture(make_note_project(), [](PianoRollView& view) {
        auto pointer = pointer_of(PointerType::mouse);
        pointer.button = ::vellum::view::MouseButton::right;
        view.simulate_click({500.0f, y_of(64)}, pointer);
    });

    REQUIRE(run.intents.size() == 1);
    CHECK(run.intents.front().value().kind == NoteEditIntentKind::Erase);
    CHECK(run.baseline_notes.size() == 3);
    REQUIRE(run.edited_notes.size() == 2);
    CHECK(find_note(run.edited_notes, kNoteC) == nullptr);
    CHECK(find_note(run.edited_notes, kNoteA) != nullptr);
    CHECK(find_note(run.edited_notes, kNoteB) != nullptr);
    CHECK(run.undone == run.baseline);
    CHECK(run.redone == run.edited);
}

TEST_CASE("Piano roll cancel restores the note an open drag had moved",
          "[timeline][piano-roll][gesture]") {
    const auto run = run_gesture(make_note_project(), [](PianoRollView& view) {
        view.on_mouse_down({250.0f, y_of(62)});
        view.on_mouse_drag({300.0f, y_of(62)});
        view.on_mouse_drag({350.0f, y_of(62)});
        view.on_mouse_cancel({350.0f, y_of(62)});
    });

    REQUIRE(run.intents.size() >= 2);
    CHECK(run.intents.front().value().phase == GesturePhase::Begin);
    CHECK(run.intents.back().value().phase == GesturePhase::Cancel);
    // The stream moved the note and then put it back.
    CHECK(same_notes(run.edited_notes, run.baseline_notes));
}

TEST_CASE("Piano roll refuses a move that would leave the clip on either side",
          "[timeline][piano-roll][gesture]") {
    // The drag passes through valid positions, so a stream opens; releasing
    // outside the clip must close it by restoring the note, never by ending on
    // a position the pointer did not ask for.
    const auto check_refused = [](const GestureRun& run) {
        REQUIRE_FALSE(run.refusals.empty());
        CHECK(run.refusals.back() == PianoRollRefusal::OutsideClip);
        const auto phases = phases_of(run);
        CHECK(std::count(phases.begin(), phases.end(), GesturePhase::End) == 0);
        CHECK(same_notes(run.edited_notes, run.baseline_notes));
        CHECK(note_in(run.edited_notes, kNoteB).start.value == 240);
    };

    // Past the right edge: note B is 120 ticks long, so a start of 930 ends at
    // 1050 in a 960-tick clip.
    check_refused(run_gesture(make_note_project(), [](PianoRollView& view) {
        view.simulate_drag({250.0f, y_of(62)}, {940.0f, y_of(62)}, 4);
    }));

    // Past the left edge: the press is 10 ticks into the note, so a release at
    // x=5 asks for a start of -5. It is refused, not parked at tick zero.
    check_refused(run_gesture(make_note_project(), [](PianoRollView& view) {
        view.simulate_drag({250.0f, y_of(62)}, {5.0f, y_of(62)}, 4);
    }));
}

TEST_CASE("Piano roll leaves the document alone when the host rejects a drag",
          "[timeline][piano-roll][gesture]") {
    auto session = Session::create(make_note_project());
    const auto baseline = canonical_project(session.project());
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    host.set_intent_status(IntentStatus::Rejected);
    {
        PianoRollView view;
        configure(view, session.project(), host);
        view.simulate_drag({250.0f, y_of(62)}, {370.0f, y_of(64)}, 4);
        CHECK_FALSE(view.gesture_open());
    }
    // Nothing the view emitted reached the document, because the host refused
    // it; the view neither throws nor leaves a stream half open.
    CHECK_FALSE(host.intents().empty());
    CHECK(canonical_project(session.project()) == baseline);
}
