#include "timeline_command_test_helpers.hpp"

#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>
#include <pulp/timeline/transaction.hpp>
#include <pulp/timeline_editor/scripted_ui_host.hpp>
#include <pulp/timeline_view/arranger_view.hpp>
#include <pulp/canvas/recording_canvas.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace pulp::timeline;
using namespace pulp::timeline_editor;
using namespace pulp::timeline_view;
using namespace timeline_test;
using pulp::canvas::DrawCommand;
using pulp::canvas::RecordingCanvas;
using pulp::view::HitMetrics;
using pulp::view::PointerType;

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
    return layout;
}

/// The fixture project places one clip at tick 0 with a quarter-note duration
/// on track {4} of sequence {3}.
ArrangerView& configure(ArrangerView& view, const Project& project,
                        EditIntentHost& host) {
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

} // namespace

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
    view.set_clip_factory([](pulp::timebase::TickPosition start) {
        return make_note_clip({20}, {21}, start.value);
    });

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
    // the defaults is refused by the program compiler
    // (CompileErrorCode::NestedSequenceUnsupported), so an editing surface that
    // could author one from a lane click could author an unplayable document.
    view.set_clip_factory([](pulp::timebase::TickPosition start) -> std::optional<Clip> {
        auto nested = Clip::create({30}, {start.value}, {kTicksPerQuarter},
                                   SequenceRef{{3}, {0}}, {});
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

    // The clip rectangle is drawn, at the geometry the projection puts it at —
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

TEST_CASE("Arranger geometry follows the projection it is handed", "[timeline][arranger]") {
    const auto project = make_project();
    ScriptedUiHost<EditIntent> host;
    ArrangerView view;
    configure(view, project, host);

    // Halving the scale halves the width and scrolling the origin shifts the
    // rectangle left — the view owns no zoom or scroll policy of its own, so
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
