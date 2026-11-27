#include "arranger_fixtures.hpp"

#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/transaction.hpp>
#include <pulp/timeline_editor/scripted_ui_host.hpp>
#include <pulp/vellum_components/arranger_view.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace timeline_view_test;

namespace {

struct CanonicalHistory {
    std::string baseline;
    std::string edited;
    std::string undone;
    std::string redone;
    std::vector<EditIntent> intents;
};

CanonicalHistory exercise_device_drag(PointerType pointer_type) {
    auto owned = DocumentSession::create(make_project());
    REQUIRE(owned);
    auto session = std::move(owned).value();
    auto registered = session->register_writer();
    REQUIRE(registered);
    auto writer = std::move(registered).value();

    const auto baseline = canonical_project(*session->snapshot());
    std::vector<EditIntent> intents;
    {
        ScriptedUiHost<EditIntent> host;
        ArrangerView view;
        configure(view, *session->snapshot(), host);
        view.set_hit_metrics(HitMetrics::for_pointer(pointer_type));

        ::vellum::view::View::SimulatedPointer pointer;
        pointer.type = pointer_type;
        const float grab_x = unit_layout().lane_left_px + 10.0f;
        view.simulate_drag({grab_x, 20.0f}, {grab_x + 200.0f, 20.0f}, 4, pointer);

        REQUIRE(host.intents().size() >= 3);
        intents.assign(host.intents().begin(), host.intents().end());
    }

    const auto undo_group = writer.allocate_undo_group_id();
    REQUIRE(undo_group.valid());
    for (const auto& intent : intents) {
        EditIntentIdentity identity;
        identity.transaction_id = writer.allocate_transaction_id();
        identity.command_id = writer.allocate_command_id();
        identity.expected_revision = session->revision();
        identity.undo_group = undo_group;
        auto lowered = lower_edit_intent(intent, identity);
        REQUIRE(lowered);
        REQUIRE(session->submit(writer, std::move(lowered).value()));
    }

    const auto edited = canonical_project(*session->snapshot());
    REQUIRE(session->undo(writer));
    const auto undone = canonical_project(*session->snapshot());
    REQUIRE(session->redo(writer));
    const auto redone = canonical_project(*session->snapshot());
    return {baseline, edited, undone, redone, std::move(intents)};
}

class SessionArrangementHost final : public TrackArrangementIntentHost {
  public:
    SessionArrangementHost(DocumentSession& session, WriterToken& writer)
        : session_(session), writer_(writer), pinned_(session.snapshot()) {}

    void bind(ArrangerView& view) {
        view_ = &view;
        view_->set_project(pinned_.get(), {3});
    }

    UiPlayhead playhead() const noexcept override {
        return {};
    }
    AuditionResult begin_audition(const AuditionRequest&) noexcept override {
        return {};
    }
    void end_audition(AuditionHandle) noexcept override {}

    IntentResult submit_intent(const TrackArrangementIntent& intent) noexcept override {
        intents_.push_back(intent);
        EditIntentIdentity identity;
        identity.transaction_id = writer_.allocate_transaction_id();
        identity.command_id = writer_.allocate_command_id();
        identity.expected_revision = session_.revision();
        auto lowered = lower_track_arrangement_intent(intent, identity);
        if (!lowered || !session_.submit(writer_, std::move(lowered).value()))
            return {IntentStatus::Rejected, 0};

        pinned_ = session_.snapshot();
        if (view_)
            view_->set_project(pinned_.get(), {3});
        return {IntentStatus::Accepted, ++accepted_sequence_};
    }

    const std::vector<TrackArrangementIntent>& intents() const noexcept {
        return intents_;
    }

  private:
    DocumentSession& session_;
    WriterToken& writer_;
    std::shared_ptr<const Project> pinned_;
    ArrangerView* view_ = nullptr;
    std::uint64_t accepted_sequence_ = 0;
    std::vector<TrackArrangementIntent> intents_;
};

struct TrackCreateHistory {
    std::string baseline;
    std::string edited;
    std::string undone;
    std::string redone;
    std::vector<ItemId> baseline_order;
    std::vector<ItemId> reopened_order;
    std::vector<ItemId> undone_order;
    std::vector<ItemId> redone_order;
    bool undone_contains_created_track = false;
    std::vector<TrackArrangementIntent> intents;
};

TrackCreateHistory exercise_track_creation(PointerType pointer_type) {
    auto owned = DocumentSession::create(make_authored_track_project());
    REQUIRE(owned);
    auto session = std::move(owned).value();
    auto registered = session->register_writer();
    REQUIRE(registered);
    auto writer = std::move(registered).value();

    const auto baseline = canonical_project(*session->snapshot());
    const auto baseline_order = track_order_after_round_trip(*session->snapshot());
    ScriptedUiHost<EditIntent> clip_host;
    ArrangerView view;
    configure(view, *session->snapshot(), clip_host);
    auto layout = unit_layout();
    layout.ruler_height_px = 24.0f;
    view.set_layout(layout);
    view.set_hit_metrics(HitMetrics::for_pointer(pointer_type));
    view.set_track_factory([]() { return Track::create({9}, "created", {}).value(); });

    SessionArrangementHost host(*session, writer);
    host.bind(view);
    view.set_track_arrangement_host(&host);

    ::vellum::view::View::SimulatedPointer pointer;
    pointer.type = pointer_type;
    const float empty_header_y =
        layout.ruler_height_px + 3.0f * layout.track_height_px + layout.track_height_px * 0.5f;
    view.simulate_click({layout.lane_left_px * 0.5f, empty_header_y}, pointer);

    const auto edited = canonical_project(*session->snapshot());
    const auto reopened_order = track_order_after_round_trip(*session->snapshot());
    std::string undone = edited;
    std::string redone = edited;
    auto undone_order = reopened_order;
    auto redone_order = reopened_order;
    bool undone_contains_created_track = true;
    if (!host.intents().empty()) {
        REQUIRE(session->undo(writer));
        undone = canonical_project(*session->snapshot());
        undone_order = track_order_after_round_trip(*session->snapshot());
        const auto* undone_sequence = session->snapshot()->find_sequence({3});
        REQUIRE(undone_sequence);
        undone_contains_created_track = undone_sequence->find_track({9}) != nullptr;
        REQUIRE(session->redo(writer));
        redone = canonical_project(*session->snapshot());
        redone_order = track_order_after_round_trip(*session->snapshot());
    }
    return {baseline,      edited,         undone,
            redone,        baseline_order, reopened_order,
            undone_order,  redone_order,   undone_contains_created_track,
            host.intents()};
}

} // namespace

TEST_CASE("Arranger mouse and touch drags share one exact undoable document edit",
          "[timeline][arranger][parity]") {
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

TEST_CASE("Arranger touch metrics extend the clip boundary beyond mouse tolerance",
          "[timeline][arranger][pointer]") {
    const auto project = make_project();
    const auto exercise = [&](PointerType pointer_type) {
        ScriptedUiHost<EditIntent> host;
        ArrangerView view;
        configure(view, project, host);
        view.set_hit_metrics(HitMetrics::for_pointer(pointer_type));

        ::vellum::view::View::SimulatedPointer pointer;
        pointer.type = pointer_type;
        const float start = unit_layout().lane_left_px + kTicksPerQuarter + 10.0f;
        view.simulate_drag({start, 20.0f}, {start + 200.0f, 20.0f}, 4, pointer);
        return std::vector<EditIntent>(host.intents().begin(), host.intents().end());
    };

    const auto mouse = exercise(PointerType::mouse);
    const auto touch = exercise(PointerType::touch);
    CHECK(mouse.empty());
    REQUIRE(touch.size() >= 3);
    REQUIRE(touch.back().replacement_range.has_value());
    CHECK(std::get<MusicalTimeRange>(*touch.back().replacement_range).start.value == 200);
}

TEST_CASE("Arranger empty-header clicks create one durable track for mouse and touch",
          "[timeline][arranger][parity]") {
    const auto mouse = exercise_track_creation(PointerType::mouse);
    const auto touch = exercise_track_creation(PointerType::touch);

    REQUIRE(mouse.intents.size() == 1);
    REQUIRE(touch.intents.size() == 1);
    const auto* mouse_create = std::get_if<TrackCreateIntent>(&mouse.intents.front());
    const auto* touch_create = std::get_if<TrackCreateIntent>(&touch.intents.front());
    REQUIRE(mouse_create);
    REQUIRE(touch_create);
    CHECK(mouse_create->sequence_id == ItemId{3});
    CHECK(mouse_create->track.id() == ItemId{9});
    CHECK(mouse_create->track.name() == "created");
    CHECK(mouse_create->before_track_id == std::nullopt);
    CHECK(touch_create->sequence_id == mouse_create->sequence_id);
    CHECK(touch_create->track.id() == mouse_create->track.id());
    CHECK(touch_create->track.name() == mouse_create->track.name());
    CHECK(touch_create->before_track_id == mouse_create->before_track_id);

    const std::vector<ItemId> expected_order{{8}, {4}, {7}, {9}};
    CHECK(mouse.reopened_order == expected_order);
    CHECK(touch.reopened_order == expected_order);
    CHECK(mouse.edited == touch.edited);
    CHECK(mouse.edited != mouse.baseline);
    CHECK(mouse.undone_order == mouse.baseline_order);
    CHECK(touch.undone_order == touch.baseline_order);
    CHECK_FALSE(mouse.undone_contains_created_track);
    CHECK_FALSE(touch.undone_contains_created_track);
    CHECK(mouse.redone == mouse.edited);
    CHECK(touch.redone == touch.edited);
    CHECK(mouse.redone_order == expected_order);
    CHECK(touch.redone_order == expected_order);
}
