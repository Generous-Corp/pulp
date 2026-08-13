#include "timeline_command_test_helpers.hpp"

#include <pulp/canvas/recording_canvas.hpp>
#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/timeline_editor/scripted_ui_host.hpp>
#include <pulp/timeline_view/piano_roll_view.hpp>
#include <pulp/view/screenshot.hpp>
#include <pulp/view/screenshot_compare.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace pulp::timeline;
using namespace pulp::timeline_editor;
using namespace pulp::timeline_view;
using namespace timeline_test;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;
using pulp::view::HitMetrics;
using pulp::view::MouseButton;
using pulp::view::PointerType;

namespace {

// One pixel per tick horizontally and twenty pixels per semitone vertically, so
// every expected value below is readable without arithmetic. The clip runs
// [0, 960) and the roll shows pitches 48..71, highest at the top.
constexpr std::int64_t kClipTicks = 960;
constexpr std::uint8_t kLowPitch = 48;
constexpr std::uint8_t kHighPitch = 71;
constexpr float kRollHeight = 480.0f; // 24 rows of 20px
constexpr ItemId kSequence{3};
constexpr ItemId kTrack{4};
constexpr ItemId kClip{5};
constexpr ItemId kNoteA{10};
constexpr ItemId kNoteB{11};
constexpr ItemId kNoteC{12};
constexpr std::size_t kDenseNoteCount = 10'000;

std::uint64_t fnv1a64(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

PianoRollLayout layout_over(std::int64_t visible_start, std::int64_t visible_ticks,
                            float pixel_extent) {
    auto time = TickProjection::create({visible_start}, {visible_ticks},
                                       PixelSpan{0.0f, pixel_extent});
    REQUIRE(time);
    auto pitch = PitchProjection::create(kLowPitch, kHighPitch, PixelSpan{0.0f, kRollHeight});
    REQUIRE(pitch);
    return PianoRollLayout{std::move(time).value(), std::move(pitch).value(), {}};
}

/// The whole clip, one pixel per tick.
PianoRollLayout unit_layout() {
    return layout_over(0, kClipTicks, static_cast<float>(kClipTicks));
}

/// Three notes, canonically ordered by start: A at 0 (pitch 60), B at 240
/// (pitch 62), C at 480 (pitch 64). Each is 120 ticks long.
Project make_note_project() {
    auto content = MidiContent::create({
        NoteEvent{kNoteA, {0}, {120}, 1000, 60, 0},
        NoteEvent{kNoteB, {240}, {120}, 1000, 62, 0},
        NoteEvent{kNoteC, {480}, {120}, 1000, 64, 0},
    });
    REQUIRE(content);
    auto authored = Clip::create(kClip, {0}, {kClipTicks}, std::move(content).value());
    REQUIRE(authored);
    auto track = Track::create(kTrack, "track", {std::move(authored).value()});
    REQUIRE(track);
    auto sequence =
        Sequence::create(kSequence, "sequence", TickDuration{kClipTicks}, {std::move(track).value()});
    REQUIRE(sequence);
    auto project =
        Project::create({{1}, "project", 100, kSequence, {}, {std::move(sequence).value()}});
    REQUIRE(project);
    return std::move(project).value();
}

Project make_dense_note_project(bool include_long_note = false) {
    constexpr std::int64_t stride = 10;
    constexpr std::uint64_t first_note_id = 1'000;
    std::vector<NoteEvent> notes;
    notes.reserve(kDenseNoteCount + (include_long_note ? 1 : 0));
    constexpr auto clip_ticks = static_cast<std::int64_t>(kDenseNoteCount) * stride;
    if (include_long_note)
        notes.push_back({{first_note_id - 1}, {0}, {clip_ticks}, 1'000, 60, 0});
    for (std::size_t index = 0; index < kDenseNoteCount; ++index) {
        notes.push_back({{first_note_id + index},
                         {static_cast<std::int64_t>(index) * stride},
                         {4},
                         1'000,
                         60,
                         0});
    }

    auto content = MidiContent::create(std::move(notes));
    REQUIRE(content);
    auto authored = Clip::create(kClip, {0}, {clip_ticks}, std::move(content).value());
    REQUIRE(authored);
    auto track = Track::create(kTrack, "track", {std::move(authored).value()});
    REQUIRE(track);
    auto sequence = Sequence::create(kSequence, "sequence", TickDuration{clip_ticks},
                                     {std::move(track).value()});
    REQUIRE(sequence);
    auto project = Project::create(
        {{1}, "project", first_note_id + kDenseNoteCount, kSequence, {},
         {std::move(sequence).value()}});
    REQUIRE(project);
    return std::move(project).value();
}

Project make_negative_start_note_project() {
    auto content = MidiContent::create({
        NoteEvent{{20}, {-20}, {30}, 1'000, 60, 0},
    });
    REQUIRE(content);
    auto authored = Clip::create(kClip, {0}, {kClipTicks}, std::move(content).value());
    REQUIRE(authored);
    auto track = Track::create(kTrack, "track", {std::move(authored).value()});
    REQUIRE(track);
    auto sequence = Sequence::create(kSequence, "sequence", TickDuration{kClipTicks},
                                     {std::move(track).value()});
    REQUIRE(sequence);
    auto project = Project::create({{1}, "project", 21, kSequence, {},
                                    {std::move(sequence).value()}});
    REQUIRE(project);
    return std::move(project).value();
}

PianoRollView& configure(PianoRollView& view, const Project& project,
                         NoteEditIntentHost& host) {
    view.set_bounds({0, 0, static_cast<float>(kClipTicks), kRollHeight});
    view.set_clip(&project, kSequence, kTrack, kClip);
    view.set_layout(unit_layout());
    view.set_host(&host);
    view.set_hit_metrics(HitMetrics::for_pointer(PointerType::mouse));
    return view;
}

/// Lattice y of a pitch's row centre, which is where a gesture aims.
float y_of(std::uint8_t pitch) {
    auto projection = PitchProjection::create(kLowPitch, kHighPitch, PixelSpan{0.0f, kRollHeight});
    REQUIRE(projection);
    return projection->y_at(pitch);
}

/// Plays the role a product shell plays: owns the session, lowers whatever the
/// view emitted against the CURRENT note array, and commits it.
struct Session {
    std::unique_ptr<DocumentSession> session;
    WriterToken writer;

    static Session create(Project project) {
        auto owned = DocumentSession::create(std::move(project));
        REQUIRE(owned);
        auto session = std::move(owned).value();
        auto writer = session->register_writer();
        REQUIRE(writer);
        return Session{std::move(session), std::move(writer).value()};
    }

    const Project& project() const { return *session->snapshot(); }

    std::span<const NoteEvent> notes() const {
        const auto& content = project()
                                  .find_sequence(kSequence)
                                  ->find_track(kTrack)
                                  ->find_clip(kClip)
                                  ->content();
        return std::get<MidiContent>(content).notes();
    }

    /// Lowers and commits one intent. Returns whether the document took it.
    bool apply(const ValidatedNoteEditIntent& intent) {
        EditIntentIdentity identity;
        identity.transaction_id = writer.allocate_transaction_id();
        identity.command_id = writer.allocate_command_id();
        identity.expected_revision = session->revision();

        const auto current = notes();
        auto lowered = lower_note_edit_intent(intent, current, identity);
        if (!lowered)
            return false;
        return static_cast<bool>(session->submit(writer, std::move(lowered).value()));
    }
};

SchemaRegistry builtin_registry() {
    auto registry = make_builtin_timeline_registry();
    REQUIRE(registry);
    return std::move(registry).value();
}

/// The clip's notes after a round trip through canonical JSON. This is the
/// acceptance vehicle: an edit that only exists in memory is not an edit the
/// document kept.
std::vector<NoteEvent> notes_after_round_trip(const Project& project) {
    const auto registry = builtin_registry();
    auto encoded = serialize_project(project, registry);
    REQUIRE(encoded);
    auto decoded = deserialize_project(encoded.value().json, registry);
    REQUIRE(decoded);
    const auto& content = decoded.value()
                              .find_sequence(kSequence)
                              ->find_track(kTrack)
                              ->find_clip(kClip)
                              ->content();
    const auto restored = std::get<MidiContent>(content).notes();
    return std::vector<NoteEvent>(restored.begin(), restored.end());
}

std::string canonical_project(const Project& project) {
    const auto registry = builtin_registry();
    auto encoded = serialize_project(project, registry);
    REQUIRE(encoded);
    return std::move(encoded).value().json;
}

struct CanonicalHistory {
    std::string baseline;
    std::string edited;
    std::string undone;
    std::string redone;
    std::vector<ValidatedNoteEditIntent> intents;
};

CanonicalHistory exercise_device_drag(PointerType pointer_type) {
    auto session = Session::create(make_note_project());
    const auto baseline = canonical_project(session.project());
    std::vector<ValidatedNoteEditIntent> intents;
    {
        ScriptedUiHost<ValidatedNoteEditIntent> host;
        PianoRollView view;
        configure(view, session.project(), host);
        view.set_hit_metrics(HitMetrics::for_pointer(pointer_type));

        pulp::view::View::SimulatedPointer pointer;
        pointer.type = pointer_type;
        view.simulate_drag({250.0f, y_of(62)}, {370.0f, y_of(64)}, 4, pointer);

        REQUIRE(host.intents().size() == 1);
        intents.assign(host.intents().begin(), host.intents().end());
    }

    REQUIRE(intents.front().value().kind == NoteEditIntentKind::Move);
    REQUIRE(session.apply(intents.front()));
    const auto edited = canonical_project(session.project());

    REQUIRE(session.session->undo(session.writer));
    const auto undone = canonical_project(session.project());
    REQUIRE(session.session->redo(session.writer));
    const auto redone = canonical_project(session.project());
    return {baseline, edited, undone, redone, std::move(intents)};
}

const NoteEvent* find_note(std::span<const NoteEvent> notes, ItemId id) {
    const auto found =
        std::find_if(notes.begin(), notes.end(), [&](const NoteEvent& n) { return n.id == id; });
    return found == notes.end() ? nullptr : &*found;
}

/// A factory that mints one fixed identity, which is what a document's id
/// allocator does for a real shell. The id must be at or above the project's
/// next_item_id — `plan_identity_insert` rejects anything below it as
/// `IdentityNotAvailable`, which is precisely why a view cannot invent one.
PianoRollView::NoteFactory factory_for(ItemId id, std::int64_t duration) {
    return [id, duration](pulp::timebase::TickPosition start,
                          std::uint8_t pitch) -> std::optional<NoteEvent> {
        return NoteEvent{id, start, {duration}, 900, pitch, 0};
    };
}

} // namespace

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

        pulp::view::View::SimulatedPointer pointer;
        pointer.type = pointer_type;
        view.simulate_drag({350.0f, y_of(62)}, {470.0f, y_of(62)}, 4, pointer);
        return std::vector<ValidatedNoteEditIntent>(host.intents().begin(),
                                                    host.intents().end());
    };

    const auto mouse = exercise(PointerType::mouse);
    const auto touch = exercise(PointerType::touch);
    REQUIRE(mouse.size() == 1);
    REQUIRE(touch.size() == 1);
    CHECK(mouse.front().value().kind == NoteEditIntentKind::Move);
    CHECK(touch.front().value().kind == NoteEditIntentKind::Resize);
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
    // 120px right and two rows up — two semitones, since y decreases upward.
    view.simulate_drag({250.0f, y_of(62)}, {370.0f, y_of(64)}, 4);

    // Commit-on-release: exactly one intent for the whole drag, closed.
    REQUIRE(host.intents().size() == 1);
    const auto& intent = host.intents().front().value();
    REQUIRE(intent.kind == NoteEditIntentKind::Move);
    REQUIRE(intent.phase == GesturePhase::Single);
    REQUIRE(session.apply(host.intents().front()));

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

TEST_CASE("Piano roll edge drag resizes a note through the document",
          "[timeline][piano-roll]") {
    auto session = Session::create(make_note_project());
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, session.project(), host);

    // Note A runs [0, 120). Grab its trailing edge and drag out to tick 300.
    view.simulate_drag({120.0f, y_of(60)}, {300.0f, y_of(60)}, 4);

    REQUIRE(host.intents().size() == 1);
    REQUIRE(host.intents().front().value().kind == NoteEditIntentKind::Resize);
    REQUIRE(session.apply(host.intents().front()));

    const auto restored = notes_after_round_trip(session.project());
    const auto* resized = find_note(restored, kNoteA);
    REQUIRE(resized != nullptr);
    CHECK(resized->duration.value == 300);
    // A resize moves the trailing edge only.
    CHECK(resized->start.value == 0);
    CHECK(resized->pitch == 60);
}

TEST_CASE("Piano roll secondary click erases a note and leaves the rest authored",
          "[timeline][piano-roll]") {
    auto session = Session::create(make_note_project());
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, session.project(), host);

    pulp::view::View::SimulatedPointer secondary;
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
    auto projection =
        PitchProjection::create(kLowPitch, kHighPitch, PixelSpan{32.0f, kRollHeight});
    REQUIRE(projection);

    PianoRollPitchRuler ruler;
    ruler.set_bounds({7.0f, 0.0f, 64.0f, 10.0f});
    ruler.set_pitch_projection(projection.value());

    CHECK(ruler.orientation() ==
          pulp::view::MidiKeyboard::Orientation::vertical_chromatic_rows);
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
                        {12.0f, projection.value().y_at(62) -
                                    projection.value().pixels().origin},
                        1);

    REQUIRE(note_ons == std::vector<int>{60, 62});
    CHECK(note_offs == std::vector<int>{60, 62});
    CHECK(velocity == 0.8f);
}

TEST_CASE("Piano roll renders ten thousand notes through Skia within its wall-clock budget",
          "[timeline][piano-roll][culling][screenshot]") {
    if (!pulp::view::raw_rgba_render_available()) {
        SKIP("Skia raster rendering is not available in this build");
    }

    const auto project = make_dense_note_project();
    PianoRollView view;
    view.set_bounds({0.0f, 0.0f, 40.0f, kRollHeight});
    view.set_clip(&project, kSequence, kTrack, kClip);
    view.set_layout(layout_over(50'000, 40, 40.0f));

    const auto started = std::chrono::steady_clock::now();
    const auto png = pulp::view::render_to_png(view, 40, 480, 1.0f,
                                               pulp::view::ScreenshotBackend::skia);
    const auto elapsed = std::chrono::steady_clock::now() - started;

    REQUIRE_FALSE(png.empty());
    const auto metadata = pulp::view::inspect_png_metadata(png);
    REQUIRE(metadata.valid);
    CHECK(metadata.width == 40);
    CHECK(metadata.height == 480);
    CHECK(view.painted_note_count() == 4);
    CHECK(view.visited_candidate_count() == 4);
#if defined(NDEBUG)
    // Keep the Release deadline well above normal raster time so it catches
    // unbounded work without turning shared-runner scheduling into a failure.
    CHECK(elapsed < std::chrono::seconds{30});
#endif

    const auto content = pulp::view::analyze_screenshot_content(png);
    REQUIRE(content.valid);
    CHECK(content.unique_colors >= 3);
    CHECK(content.non_background_coverage > 0.01);

    std::uint32_t rgba_width = 0;
    std::uint32_t rgba_height = 0;
    const auto rgba = pulp::view::render_to_rgba(view, 40, 480, 1.0f, &rgba_width,
                                                  &rgba_height);
    REQUIRE_FALSE(rgba.empty());
    CHECK(rgba_width == 40);
    CHECK(rgba_height == 480);
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

TEST_CASE("Piano roll geometry follows the projections it is handed",
          "[timeline][piano-roll]") {
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

TEST_CASE("Piano roll refuses a move that would push a note out of its clip",
          "[timeline][piano-roll]") {
    auto session = Session::create(make_note_project());
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, session.project(), host);

    // Note A runs [0, 120) in a clip that ends at 960. Grab it 10px in and drag
    // to the right edge: the release tick clamps to 960, so the note would start
    // at 950 and end at 1070 — past its own clip. That is refused rather than
    // clamped, because clamping produces a different musical result than the one
    // dragged for, with no signal that it happened.
    view.simulate_drag({10.0f, y_of(60)}, {960.0f, y_of(60)}, 4);

    CHECK(host.intents().empty());
    REQUIRE(view.refusals().size() == 1);
    CHECK(view.refusals().front() == PianoRollRefusal::OutsideClip);
    // The document never saw it.
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
    REQUIRE(host.intents().size() == 1);
    REQUIRE(view.refusals().empty());
    REQUIRE(session.apply(host.intents().front()));

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
    raw.phase = GesturePhase::Single;
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

    auto lowered = lower_note_edit_intent(validated.value(), session.notes(), identity);
    REQUIRE_FALSE(lowered);
    CHECK(lowered.error() == NoteLoweringError::ExpectedNoteMismatch);
    // The document is untouched — a refusal is not a partial edit.
    CHECK(find_note(session.notes(), kNoteB)->pitch == 62);
}

// Every refusal below names the gesture that produces it. A refusal no gesture
// can reach is dead code that reads as rigour, and the OutsideClip case above
// was exactly that until it was rewritten around a move — so the whole set is
// enumerated here rather than sampled.
//
// SCOPE, stated because it is easy to read these as stronger than they are: a
// view-side case asserts THAT the gesture is refused, not WHICH check refused
// it. Note-domain validity is checked in `admissible` and again by
// `ValidatedNoteEditIntent::create` at the seam, both reporting `InvalidNote`,
// so no assertion here can separate the layers — removing either one leaves
// these cases green. That is the correct behavioural claim (a bad gesture never
// reaches the document) and deliberately not a claim about layering. The
// lowering cases below ARE layer-specific, because each returns its own error.

TEST_CASE("Piano roll refuses a factory note the note domain rejects",
          "[timeline][piano-roll]") {
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
    CHECK(host.intents().empty());
    REQUIRE(view.refusals().size() == 1);
    CHECK(view.refusals().front() == PianoRollRefusal::InvalidNote);
}

TEST_CASE("Lowering refuses an insert whose identity the clip already carries",
          "[timeline][piano-roll]") {
    auto session = Session::create(make_note_project());
    ScriptedUiHost<ValidatedNoteEditIntent> host;
    PianoRollView view;
    configure(view, session.project(), host);
    // A factory that re-mints a live identity — the shape of an id allocator
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
// the ordering guard in `admissible` — the downstream note-domain validation
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
    view.set_note_factory([](pulp::timebase::TickPosition,
                             std::uint8_t pitch) -> std::optional<NoteEvent> {
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
