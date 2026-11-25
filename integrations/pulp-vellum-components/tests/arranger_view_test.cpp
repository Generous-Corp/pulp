#include "render_helpers.hpp"
#include "timeline_command_test_helpers.hpp"

#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/timeline/transaction.hpp>
#include <pulp/timeline_editor/scripted_ui_host.hpp>
#include <pulp/vellum_components/arranger_view.hpp>
#include <vellum/canvas/recording_canvas.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace timebase = pulp::timebase;
namespace rh = pulp::vellum_components::testing;
using namespace pulp::timeline;
using namespace pulp::timeline_editor;
using namespace pulp::vellum_components;
using namespace timeline_test;
using ::vellum::canvas::DrawCommand;
using ::vellum::canvas::RecordingCanvas;
using ::vellum::view::HitMetrics;
using ::vellum::view::PointerType;

namespace {

/// One pixel per tick, with the lane column starting at x=120, so a pixel
/// delta and a tick delta are the same number and every expected value in
/// these tests is readable without arithmetic. The fixture clip is one quarter
/// (kTicksPerQuarter ticks) long, so it occupies x=[120, 120+kTicksPerQuarter)
/// and the view is given bounds wide enough to hold all of it -- otherwise
/// paint clips it at the right edge and empty-lane clicks land inside it.
ArrangerLayout unit_layout() {
    ArrangerLayout layout;
    layout.origin_tick = {0};
    layout.px_per_tick = 1.0;
    layout.track_height_px = 40.0f;
    layout.lane_left_px = 120.0f;
    layout.ruler_height_px = 0.0f;
    return layout;
}

/// The fixture project places one clip at tick 0 with a quarter-note duration
/// on track {4} of sequence {3}.
ArrangerView& configure(ArrangerView& view, const Project& project, EditIntentHost& host) {
    view.set_bounds({0, 0, 120.0f + kTicksPerQuarter + 800.0f, 200});
    view.set_project(&project, {3});
    view.set_layout(unit_layout());
    view.set_host(&host);
    view.set_hit_metrics(HitMetrics::for_pointer(PointerType::mouse));
    return view;
}

/// Plays the role a session plays: allocates identities and reduces whatever
/// the view emitted, in order, onto the project.
struct Applier {
    Project project;
    std::uint64_t next = 1;
    std::optional<UndoGroupId> group = UndoGroupId{WriterId{1}, 1};

    /// Reduces one intent and returns whether it applied. A conflict is
    /// reported rather than asserted so a test can prove a stream is rejected.
    bool apply(const EditIntent& intent) {
        EditIntentIdentity identity;
        identity.transaction_id = TransactionId{WriterId{1}, next};
        identity.command_id = CommandId{WriterId{1}, next};
        ++next;
        identity.expected_revision = {};
        identity.undo_group = group;

        auto lowered = lower_edit_intent(intent, identity);
        if (!lowered)
            return false;
        auto reduced = reduce_transaction(project, lowered.value());
        if (!reduced)
            return false;
        project = std::move(reduced).value().project;
        return true;
    }
};

/// The builtin schema registry, which every serialize path needs and which
/// cannot fail for the builtin set.
SchemaRegistry builtin_registry() {
    auto registry = make_builtin_timeline_registry();
    REQUIRE(registry);
    return std::move(registry).value();
}

/// The clip's authored start after a round trip through canonical JSON. This
/// is the acceptance vehicle: an edit that only exists in memory is not an
/// edit the document kept.
std::int64_t start_after_round_trip(const Project& project) {
    const auto registry = builtin_registry();
    auto encoded = serialize_project(project, registry);
    REQUIRE(encoded);
    auto decoded = deserialize_project(encoded.value().json, registry);
    REQUIRE(decoded);
    return clip(decoded.value()).start().value;
}

std::string canonical_project(const Project& project) {
    const auto registry = builtin_registry();
    auto encoded = serialize_project(project, registry);
    REQUIRE(encoded);
    return std::move(encoded).value().json;
}

Project make_three_track_project() {
    auto first = Track::create({4}, "first", {make_note_clip({5}, {6}, 0)});
    auto second = Track::create({7}, "second", {});
    auto third = Track::create({8}, "third", {});
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(third);
    std::vector<Track> tracks;
    tracks.push_back(std::move(first).value());
    tracks.push_back(std::move(second).value());
    tracks.push_back(std::move(third).value());
    auto sequence =
        Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter}, std::move(tracks));
    REQUIRE(sequence);
    ProjectInput input;
    input.id = {1};
    input.name = "project";
    input.next_item_id = 9;
    input.root_sequence_id = {3};
    input.sequences.push_back(std::move(sequence).value());
    auto project = Project::create(std::move(input));
    REQUIRE(project);
    return std::move(project).value();
}

std::vector<ItemId> track_order_after_round_trip(const Project& project) {
    const auto registry = builtin_registry();
    auto encoded = serialize_project(project, registry);
    REQUIRE(encoded);
    auto decoded = deserialize_project(encoded.value().json, registry);
    REQUIRE(decoded);
    const auto order = decoded.value().find_sequence({3})->track_order();
    return {order.begin(), order.end()};
}

struct TrackReorderHistory {
    std::string baseline;
    std::string edited;
    std::string undone;
    std::string redone;
    std::vector<ItemId> reopened_order;
    std::vector<TrackEditIntent> intents;
};

TrackReorderHistory exercise_track_header_drag(PointerType pointer_type,
                                               ::vellum::view::Point start,
                                               ::vellum::view::Point end,
                                               float ruler_height_px = 0.0f) {
    auto owned = DocumentSession::create(make_three_track_project());
    REQUIRE(owned);
    auto session = std::move(owned).value();
    auto registered = session->register_writer();
    REQUIRE(registered);
    auto writer = std::move(registered).value();

    const auto baseline = canonical_project(*session->snapshot());
    ScriptedUiHost<TrackEditIntent> host;
    ArrangerView view;
    ScriptedUiHost<EditIntent> clip_host;
    configure(view, *session->snapshot(), clip_host);
    auto layout = unit_layout();
    layout.ruler_height_px = ruler_height_px;
    view.set_layout(layout);
    view.set_track_edit_host(&host);
    view.set_hit_metrics(HitMetrics::for_pointer(pointer_type));

    ::vellum::view::View::SimulatedPointer pointer;
    pointer.type = pointer_type;
    view.simulate_drag(start, end, 4, pointer);

    const auto intents = host.intents();
    const auto undo_group = writer.allocate_undo_group_id();
    REQUIRE(undo_group.valid());
    for (const auto& intent : intents) {
        EditIntentIdentity identity;
        identity.transaction_id = writer.allocate_transaction_id();
        identity.command_id = writer.allocate_command_id();
        identity.expected_revision = session->revision();
        identity.undo_group = undo_group;
        auto lowered = lower_track_edit_intent(intent, identity);
        REQUIRE(lowered);
        REQUIRE(session->submit(writer, std::move(lowered).value()));
    }

    const auto edited = canonical_project(*session->snapshot());
    const auto reopened_order = track_order_after_round_trip(*session->snapshot());
    std::string undone = edited;
    std::string redone = edited;
    if (!intents.empty()) {
        REQUIRE(session->undo(writer));
        undone = canonical_project(*session->snapshot());
        REQUIRE(session->redo(writer));
        redone = canonical_project(*session->snapshot());
    }
    return {baseline, edited, undone, redone, reopened_order, intents};
}

void check_same_track_intent(const TrackEditIntent& left, const TrackEditIntent& right) {
    CHECK(left.kind == right.kind);
    CHECK(left.phase == right.phase);
    CHECK(left.sequence_id == right.sequence_id);
    CHECK(left.track_id == right.track_id);
    CHECK(left.expected_before_track_id == right.expected_before_track_id);
    CHECK(left.replacement_before_track_id == right.replacement_before_track_id);
}

Project make_authored_track_project() {
    auto first = Track::create({4}, "first", {});
    auto second = Track::create({7}, "second", {});
    auto third = Track::create({8}, "third", {});
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(third);

    SequenceInput sequence_input;
    sequence_input.id = {3};
    sequence_input.name = "sequence";
    sequence_input.musical_duration = TickDuration{8 * kTicksPerQuarter};
    sequence_input.tracks.push_back(std::move(first).value());
    sequence_input.tracks.push_back(std::move(second).value());
    sequence_input.tracks.push_back(std::move(third).value());
    sequence_input.track_order = {{8}, {4}, {7}};
    auto sequence = Sequence::create(std::move(sequence_input));
    REQUIRE(sequence);

    ProjectInput project_input;
    project_input.id = {1};
    project_input.name = "project";
    project_input.next_item_id = 9;
    project_input.root_sequence_id = {3};
    project_input.sequences.push_back(std::move(sequence).value());
    auto project = Project::create(std::move(project_input));
    REQUIRE(project);
    return std::move(project).value();
}

} // namespace

TEST_CASE("Arranger track-header drags reorder authored tracks for mouse and touch",
          "[timeline][arranger][parity]") {
    const ::vellum::view::Point header_third{60.0f, 100.0f};
    const ::vellum::view::Point header_front{60.0f, 0.0f};
    const auto mouse = exercise_track_header_drag(PointerType::mouse, header_third, header_front);
    const auto touch = exercise_track_header_drag(PointerType::touch, header_third, header_front);

    REQUIRE(mouse.intents.size() == 1);
    REQUIRE(touch.intents.size() == 1);
    check_same_track_intent(mouse.intents.front(), touch.intents.front());
    CHECK(mouse.intents.front().expected_before_track_id == std::nullopt);
    CHECK(mouse.intents.front().replacement_before_track_id == std::optional<ItemId>{{4}});
    CHECK(mouse.reopened_order == std::vector<ItemId>{{8}, {4}, {7}});
    CHECK(touch.reopened_order == mouse.reopened_order);
    CHECK(mouse.edited == touch.edited);
    CHECK(mouse.edited != mouse.baseline);
    CHECK(mouse.undone == mouse.baseline);
    CHECK(touch.undone == touch.baseline);
    CHECK(mouse.redone == mouse.edited);
    CHECK(touch.redone == touch.edited);
}

TEST_CASE("Arranger ruler offset preserves track-header reorder", "[timeline][arranger]") {
    constexpr float ruler_height = 24.0f;
    const auto history = exercise_track_header_drag(
        PointerType::mouse, {60.0f, ruler_height + 100.0f}, {60.0f, ruler_height}, ruler_height);

    REQUIRE(history.intents.size() == 1);
    CHECK(history.reopened_order == std::vector<ItemId>{{8}, {4}, {7}});
    CHECK(history.undone == history.baseline);
    CHECK(history.redone == history.edited);
}

TEST_CASE("Arranger track-header drop below the rows moves a track last", "[timeline][arranger]") {
    const auto history =
        exercise_track_header_drag(PointerType::mouse, {60.0f, 20.0f}, {60.0f, 160.0f});
    REQUIRE(history.intents.size() == 1);
    CHECK(history.intents.front().track_id == ItemId{4});
    CHECK(history.intents.front().expected_before_track_id == std::optional<ItemId>{{7}});
    CHECK(history.intents.front().replacement_before_track_id == std::nullopt);
    CHECK(history.reopened_order == std::vector<ItemId>{{7}, {8}, {4}});
    CHECK(history.undone == history.baseline);
    CHECK(history.redone == history.edited);
}

TEST_CASE("Arranger track-header drop at the authored position emits nothing",
          "[timeline][arranger]") {
    const auto click =
        exercise_track_header_drag(PointerType::mouse, {60.0f, 20.0f}, {60.0f, 20.0f});
    CHECK(click.intents.empty());
    CHECK(click.edited == click.baseline);

    const auto horizontal_jitter =
        exercise_track_header_drag(PointerType::mouse, {60.0f, 30.0f}, {61.0f, 30.0f});
    CHECK(horizontal_jitter.intents.empty());
    CHECK(horizontal_jitter.edited == horizontal_jitter.baseline);

    const auto history =
        exercise_track_header_drag(PointerType::mouse, {60.0f, 60.0f}, {60.0f, 40.0f});
    CHECK(history.intents.empty());
    CHECK(history.edited == history.baseline);
    CHECK(history.reopened_order == std::vector<ItemId>{{4}, {7}, {8}});
}

TEST_CASE("Arranger empty-header drags and cancellations do not create tracks",
          "[timeline][arranger][pointer]") {
    const auto project = make_authored_track_project();
    ScriptedUiHost<EditIntent> clip_host;
    ScriptedUiHost<TrackArrangementIntent> arrangement_host;
    ArrangerView view;
    configure(view, project, clip_host);
    auto layout = unit_layout();
    layout.ruler_height_px = 24.0f;
    view.set_layout(layout);
    view.set_track_arrangement_host(&arrangement_host);
    view.set_track_factory([]() { return Track::create({9}, "created", {}).value(); });

    const float empty_header_y = layout.ruler_height_px + 3.0f * layout.track_height_px + 20.0f;
    ::vellum::view::View::SimulatedPointer touch;
    touch.type = PointerType::touch;
    view.simulate_drag({60.0f, empty_header_y}, {70.0f, empty_header_y + 10.0f}, 4, touch);
    CHECK(arrangement_host.intents().empty());

    view.on_mouse_down({60.0f, empty_header_y});
    REQUIRE(view.gesture_open());
    view.on_mouse_cancel({60.0f, empty_header_y});
    CHECK_FALSE(view.gesture_open());
    CHECK(arrangement_host.intents().empty());
}

TEST_CASE("Arranger empty-header release must match the press without drag callbacks",
          "[timeline][arranger][pointer]") {
    const auto project = make_authored_track_project();
    ScriptedUiHost<EditIntent> clip_host;
    ScriptedUiHost<TrackArrangementIntent> arrangement_host;
    ArrangerView view;
    configure(view, project, clip_host);
    auto layout = unit_layout();
    layout.ruler_height_px = 24.0f;
    view.set_layout(layout);
    view.set_track_arrangement_host(&arrangement_host);
    view.set_track_factory([]() { return Track::create({9}, "created", {}).value(); });

    const float empty_header_y = layout.ruler_height_px + 3.0f * layout.track_height_px + 20.0f;
    view.on_mouse_down({60.0f, empty_header_y});
    REQUIRE(view.gesture_open());
    view.on_mouse_up({61.0f, empty_header_y});

    CHECK_FALSE(view.gesture_open());
    CHECK(arrangement_host.intents().empty());
}

TEST_CASE("Arranger clip drag survives a serialize round trip with its authored start",
          "[timeline][arranger]") {
    const auto project = make_project();
    ScriptedUiHost<EditIntent> host;
    ArrangerView view;
    configure(view, project, host);

    // The fixture clip starts at tick 0 and is one quarter long. Grab it 10px
    // in from its left edge and drag 200px to the right.
    const float grab_x = unit_layout().lane_left_px + 10.0f;
    view.simulate_drag({grab_x, 20.0f}, {grab_x + 200.0f, 20.0f}, 4);

    REQUIRE_FALSE(host.intents().empty());
    // The bracket opens once and closes once, so a session allocates exactly
    // one undo group for the whole drag.
    REQUIRE(host.intents().front().phase == GesturePhase::Begin);
    REQUIRE(host.intents().back().phase == GesturePhase::End);
    for (std::size_t i = 1; i + 1 < host.intents().size(); ++i)
        REQUIRE(host.intents()[i].phase == GesturePhase::Update);

    Applier applier{project};
    for (const auto& intent : host.intents())
        REQUIRE(applier.apply(intent));

    // The value, not the shape: a 200px drag at one pixel per tick moves the
    // clip from tick 0 to tick 200, and the document keeps it.
    REQUIRE(clip(applier.project).start().value == 200);
    REQUIRE(clip(applier.project).duration().value == kTicksPerQuarter);
    REQUIRE(start_after_round_trip(applier.project) == 200);
}

TEST_CASE("Arranger drag steps chain their optimistic gate so the stream applies in order",
          "[timeline][arranger]") {
    const auto project = make_project();
    ScriptedUiHost<EditIntent> host;
    ArrangerView view;
    configure(view, project, host);

    const float grab_x = unit_layout().lane_left_px + 10.0f;
    view.simulate_drag({grab_x, 20.0f}, {grab_x + 120.0f, 20.0f}, 4);
    REQUIRE(host.intents().size() >= 3);

    // Each step gates on the range the step before it produced. Were every
    // step to claim the range the drag started from, the second would be
    // rejected by MoveClip's exact optimistic gate.
    for (std::size_t i = 1; i < host.intents().size(); ++i) {
        const auto& previous = host.intents()[i - 1];
        const auto& current = host.intents()[i];
        REQUIRE(current.expected_range.has_value());
        REQUIRE(previous.replacement_range.has_value());
        REQUIRE(equivalent(*current.expected_range, *previous.replacement_range));
    }

    Applier applier{project};
    for (const auto& intent : host.intents())
        REQUIRE(applier.apply(intent));
    REQUIRE(clip(applier.project).start().value == 120);
}

TEST_CASE("Arranger click that never moves emits nothing", "[timeline][arranger]") {
    const auto project = make_project();
    ScriptedUiHost<EditIntent> host;
    ArrangerView view;
    configure(view, project, host);

    // A press and release on the clip with no movement between them. An undo
    // group opened here would put an empty entry in the user's history.
    view.simulate_click({unit_layout().lane_left_px + 10.0f, 20.0f});
    REQUIRE(host.intents().empty());
    REQUIRE_FALSE(view.gesture_open());
}

TEST_CASE("Arranger create gesture authors a clip that survives a round trip",
          "[timeline][arranger]") {
    const auto project = make_project();
    ScriptedUiHost<EditIntent> host;
    ArrangerView view;
    configure(view, project, host);

    // Identity comes from the caller: the document's id domain is not the
    // view's to allocate.
    view.set_clip_factory(
        [](timebase::TickPosition start) { return make_note_clip({20}, {21}, start.value); });

    // Empty lane space past the fixture clip's quarter-note extent.
    view.simulate_click({unit_layout().lane_left_px + kTicksPerQuarter + 200.0f, 20.0f});
    REQUIRE(host.intents().size() == 1);
    REQUIRE(host.intents().front().kind == EditIntentKind::Draw);

    Applier applier{project};
    REQUIRE(applier.apply(host.intents().front()));

    const auto registry = builtin_registry();
    auto encoded = serialize_project(applier.project, registry);
    REQUIRE(encoded);
    auto decoded = deserialize_project(encoded.value().json, registry);
    REQUIRE(decoded);
    const auto* created = decoded.value().find_sequence({3})->find_track({4})->find_clip({20});
    REQUIRE(created != nullptr);
    REQUIRE(created->start().value == kTicksPerQuarter + 200);
}

TEST_CASE("Arranger refuses to author a nested-sequence clip from a lane click",
          "[timeline][arranger]") {
    const auto project = make_project();
    ScriptedUiHost<EditIntent> host;
    ArrangerView view;
    configure(view, project, host);

    // A nested placement whose anchor or playback properties are anything but
    // the defaults is refused by the program compiler, which names the missing
    // construct (CompileErrorCode::NestedAbsoluteChildUnsupported and its
    // siblings), so an editing surface that could author one from a lane click
    // could author an unplayable document.
    view.set_clip_factory([](timebase::TickPosition start) -> std::optional<Clip> {
        auto nested =
            Clip::create({30}, {start.value}, {kTicksPerQuarter}, SequenceRef{{3}, {0}}, {});
        REQUIRE(nested);
        return std::move(nested).value();
    });

    view.simulate_click({unit_layout().lane_left_px + kTicksPerQuarter + 200.0f, 20.0f});
    REQUIRE(host.intents().empty());
    REQUIRE(view.refusals().size() == 1);
    REQUIRE(view.refusals().front() == ArrangerRefusal::NestedSequenceContent);
}

TEST_CASE("Arranger paints a lane and a clip rectangle at their projected geometry",
          "[timeline][arranger]") {
    const auto project = make_project();
    ScriptedUiHost<EditIntent> host;
    ArrangerView view;
    configure(view, project, host);

    RecordingCanvas canvas;
    view.paint(canvas);

    const auto expected = view.clip_rect({4}, {5});
    REQUIRE(expected);
    REQUIRE(expected->x == 120.0f);
    REQUIRE(expected->width == static_cast<float>(kTicksPerQuarter));

    // The clip rectangle is drawn, at the geometry the projection puts it at -
    // asserting the ink, not that painting merely ran.
    bool found = false;
    for (const auto& command : canvas.commands()) {
        if (command.type != DrawCommand::Type::fill_rect)
            continue;
        if (command.f[0] == expected->x && command.f[1] == expected->y &&
            command.f[2] == expected->width && command.f[3] == expected->height)
            found = true;
    }
    REQUIRE(found);
}

TEST_CASE("Arranger offscreen pixels distinguish the ruler, grid, lane and clips",
          "[timeline][arranger][render]") {
    constexpr std::uint32_t width = 750;
    constexpr std::uint32_t height = 80;
    const auto project = make_project();
    ScriptedUiHost<EditIntent> host;
    ArrangerView view;
    configure(view, project, host);

    auto layout = unit_layout();
    layout.px_per_tick = 1.0 / 5040.0;
    layout.ruler_height_px = 24.0f;
    view.set_layout(layout);
    view.set_bounds({0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)});

    const auto frame = rh::render(view, width, height);
    REQUIRE(frame.width == width);
    rh::maybe_write_png(view, width, height, "arranger_basic.png");
    const auto require_pixel = [&](std::uint32_t x, std::uint32_t y, int r, int g, int b) {
        const auto actual = frame.at(x, y);
        INFO("pixel (" << x << ", " << y << ") = " << actual.r << ", " << actual.g << ", "
                       << actual.b << ", " << actual.a);
        REQUIRE(rh::near(actual, r, g, b, 4));
    };

    require_pixel(200, 10, 20, 23, 28);    // ruler fill
    require_pixel(120, 10, 122, 128, 140); // bar line over the ruler
    require_pixel(260, 10, 41, 43, 50);    // beat line over the ruler
    require_pixel(400, 44, 45, 47, 54);    // beat line over the lane
    require_pixel(180, 44, 77, 140, 217);  // clip fill
}
TEST_CASE("Arranger fixture draws each track's clips on its own row",
          "[timeline][arranger][render]") {
    // Three tracks, with clips at different starts, so a row mix-up or a clip
    // drawn on the wrong lane shows as a pixel difference.
    std::vector<Track> tracks;
    const std::int64_t starts[3][2] = {{0, 2 * kTicksPerQuarter},
                                       {kTicksPerQuarter, -1},
                                       {3 * kTicksPerQuarter, 5 * kTicksPerQuarter}};
    std::uint64_t next = 10;
    for (std::uint64_t t = 0; t < 3; ++t) {
        std::vector<Clip> clips;
        for (const auto start : starts[t]) {
            if (start >= 0) {
                clips.push_back(make_note_clip({next}, {next + 1}, start));
                next += 2;
            }
        }
        auto track = Track::create({4 + t}, "track", std::move(clips));
        REQUIRE(track);
        tracks.push_back(std::move(track).value());
    }
    auto sequence =
        Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter}, std::move(tracks));
    REQUIRE(sequence);
    ProjectInput input;
    input.id = {1};
    input.name = "project";
    input.next_item_id = next;
    input.root_sequence_id = {3};
    input.sequences.push_back(std::move(sequence).value());
    auto project = Project::create(std::move(input));
    REQUIRE(project);

    ArrangerView view;
    view.set_project(&project.value(), {3});
    ArrangerLayout layout;
    layout.px_per_tick = 80.0 / static_cast<double>(kTicksPerQuarter);
    layout.track_height_px = 36.0f;
    layout.lane_left_px = 96.0f;
    layout.ruler_height_px = 24.0f;
    view.set_layout(layout);
    constexpr std::uint32_t width = 800;
    constexpr std::uint32_t height = 24 + 3 * 36 + 12;
    view.set_bounds({0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)});

    const auto frame = rh::render(view, width, height);
    rh::maybe_write_png(view, width, height, "arranger_three_tracks.png");

    const auto clip_x = [&](std::int64_t tick) {
        return static_cast<std::uint32_t>(layout.x_for_tick({tick}) + 40.0f);
    };
    const auto row_y = [&](std::size_t row) {
        return static_cast<std::uint32_t>(layout.y_for_track_index(row) + 18.0f);
    };
    const auto is_clip = [&](std::uint32_t x, std::uint32_t y) {
        return rh::near(frame.at(x, y), 77, 140, 217, 4);
    };
    // The fixture clip fill is distinct from the lane fill (28, 31, 36).
    CHECK(is_clip(clip_x(0), row_y(0)));
    CHECK(is_clip(clip_x(2 * kTicksPerQuarter), row_y(0)));
    CHECK(is_clip(clip_x(kTicksPerQuarter), row_y(1)));
    CHECK(is_clip(clip_x(3 * kTicksPerQuarter), row_y(2)));
    CHECK(is_clip(clip_x(5 * kTicksPerQuarter), row_y(2)));
    // Absent clips leave the lane fill, and a clip does not leak into the
    // header column.
    CHECK_FALSE(is_clip(clip_x(2 * kTicksPerQuarter), row_y(1)));
    CHECK_FALSE(is_clip(clip_x(0), row_y(2)));
    CHECK_FALSE(is_clip(20, row_y(0)));
    CHECK(rh::near(frame.at(20, row_y(0)), 41, 43, 51, 4));
}

TEST_CASE("Arranger parks a clip at tick zero when a drag runs past the origin",
          "[timeline][arranger]") {
    auto project = make_project();
    ScriptedUiHost<EditIntent> host;
    ArrangerView view;
    configure(view, project, host);
    auto layout = unit_layout();
    layout.origin_tick = {100};
    view.set_layout(layout);

    // The clip starts at tick 0, which scrolls to x = lane_left - 100. Grab it
    // at its visible body and pull 400px left: the target is far below zero, so
    // it parks at the origin the clip already occupies and nothing is emitted.
    view.simulate_drag({layout.lane_left_px + 20.0f, 20.0f}, {layout.lane_left_px - 380.0f, 20.0f},
                       4);
    for (const auto& intent : host.intents()) {
        REQUIRE(intent.replacement_range.has_value());
        CHECK(std::get<MusicalTimeRange>(*intent.replacement_range).start.value >= 0);
    }
    CHECK(host.intents().empty());
}

TEST_CASE("Arranger clips scrolled past the lane edge do not paint over the header",
          "[timeline][arranger][render]") {
    const auto project = make_project();
    ScriptedUiHost<EditIntent> host;
    ArrangerView view;
    configure(view, project, host);
    auto layout = unit_layout();
    layout.origin_tick = {100};
    layout.ruler_height_px = 0.0f;
    view.set_layout(layout);
    constexpr std::uint32_t width = 700;
    view.set_bounds({0.0f, 0.0f, static_cast<float>(width), 80.0f});

    const auto frame = rh::render(view, width, 80);
    // The header column keeps its fill; the visible part of the clip starts
    // at the lane edge.
    CHECK(rh::near(frame.at(60, 20), 41, 43, 51, 4));
    CHECK(rh::near(frame.at(130, 20), 77, 140, 217, 4));
}

TEST_CASE("Arranger sizes grid output for every visible bar", "[timeline][arranger][render]") {
    constexpr float width = 240.0f;
    constexpr float lane_width = 120.0f;
    constexpr std::int64_t visible_bars = 9000;
    const auto project = make_project();
    ScriptedUiHost<EditIntent> host;
    ArrangerView view;
    configure(view, project, host);

    auto layout = unit_layout();
    layout.px_per_tick = lane_width / static_cast<double>(visible_bars * 4 * kTicksPerQuarter);
    layout.ruler_height_px = 24.0f;
    view.set_layout(layout);
    view.set_bounds({0.0f, 0.0f, width, 80.0f});

    RecordingCanvas canvas;
    view.paint(canvas);
    std::vector<DrawCommand> grid_lines;
    for (const auto& command : canvas.commands()) {
        if (command.type == DrawCommand::Type::fill_rect && command.f[1] == 0.0f &&
            command.f[2] == 2.0f && command.f[3] == 80.0f)
            grid_lines.push_back(command);
    }
    REQUIRE(grid_lines.size() == static_cast<std::size_t>(visible_bars + 1));
    CHECK(grid_lines.front().f[0] + grid_lines.front().f[2] * 0.5f == layout.lane_left_px);
    CHECK(std::fabs(grid_lines.back().f[0] + grid_lines.back().f[2] * 0.5f - width) < 0.001f);
}

TEST_CASE("Arranger grid uses the clip scale for non-integral visible spans",
          "[timeline][arranger][render]") {
    constexpr auto next_bar_tick = 4 * kTicksPerQuarter;
    constexpr float lane_width = 1.5f;
    const auto project = make_project();
    ScriptedUiHost<EditIntent> host;
    ArrangerView view;
    configure(view, project, host);

    auto layout = unit_layout();
    layout.px_per_tick = 1.0;
    layout.origin_tick = {next_bar_tick - 1};
    view.set_layout(layout);
    view.set_bounds({0.0f, 0.0f, layout.lane_left_px + lane_width, 80.0f});

    RecordingCanvas canvas;
    view.paint(canvas);
    const float expected_x = layout.x_for_tick({next_bar_tick});
    bool found_aligned_bar = false;
    for (const auto& command : canvas.commands()) {
        if (command.type != DrawCommand::Type::fill_rect || command.f[1] != 0.0f ||
            command.f[2] != 2.0f || command.f[3] != 80.0f)
            continue;
        const float center_x = command.f[0] + command.f[2] * 0.5f;
        if (std::fabs(center_x - expected_x) < 0.001f)
            found_aligned_bar = true;
    }
    CHECK(found_aligned_bar);
}

TEST_CASE("Arranger rejects the first out-of-range visible tick duration",
          "[timeline][arranger][render]") {
    const auto project = make_project();
    ScriptedUiHost<EditIntent> host;
    ArrangerView view;
    configure(view, project, host);

    auto layout = unit_layout();
    layout.px_per_tick = 1.0 / 0x1p63;
    view.set_layout(layout);
    view.set_bounds({0.0f, 0.0f, layout.lane_left_px + 1.0f, 80.0f});

    RecordingCanvas canvas;
    view.paint(canvas);
    for (const auto& command : canvas.commands()) {
        const bool full_height_grid =
            command.type == DrawCommand::Type::fill_rect && command.f[1] == 0.0f &&
            (command.f[2] == 1.0f || command.f[2] == 2.0f) && command.f[3] == 80.0f;
        CHECK_FALSE(full_height_grid);
    }
}

TEST_CASE("Arranger geometry follows the projection it is handed", "[timeline][arranger]") {
    const auto project = make_project();
    ScriptedUiHost<EditIntent> host;
    ArrangerView view;
    configure(view, project, host);

    // Halving the scale halves the width and scrolling the origin shifts the
    // rectangle left, the view owns no zoom or scroll policy of its own, so
    // both are pure consequences of the scalars it was given.
    ArrangerLayout scrolled = unit_layout();
    scrolled.px_per_tick = 0.5;
    scrolled.origin_tick = {100};
    view.set_layout(scrolled);

    const auto rect = view.clip_rect({4}, {5});
    REQUIRE(rect);
    REQUIRE(rect->x == 120.0f - 50.0f);
    REQUIRE(rect->width == static_cast<float>(kTicksPerQuarter) * 0.5f);
}
