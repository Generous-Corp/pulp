#include "timeline_command_test_helpers.hpp"

#include <pulp/timeline_editor/edit_intent.hpp>
#include <pulp/timeline_editor/scripted_ui_host.hpp>
#include <pulp/timeline_editor/selection.hpp>
#include <pulp/view/hit_metrics.hpp>
#include <pulp/view/waveform_editor_primitives.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <type_traits>
#include <vector>

using namespace pulp::timeline;
using namespace pulp::timeline_editor;
using namespace timeline_test;
using pulp::view::HitMetrics;
using pulp::view::PointerType;
using pulp::view::WaveformHandleKind;
using pulp::view::WaveformHandleModel;
using pulp::view::WaveformHitResult;
using pulp::view::WaveformViewport;

namespace {

/// One sample per pixel with two handles 200px apart, so a touch-sized target
/// still resolves unambiguously and the two pointer types can be compared.
WaveformViewport parity_viewport() {
    WaveformViewport viewport;
    viewport.set_bounds({0, 0, 1000, 40});
    viewport.set_total_samples(1000);
    viewport.set_visible_range(0, 1000);
    return viewport;
}

WaveformHandleModel parity_model() {
    WaveformHandleModel model;
    model.set_total_samples(1000);
    model.set_selection(100, 300);
    return model;
}

/// The step a front-end owns: resolve device-dependent geometry into a
/// device-independent intent. Everything below this line is pointer-neutral.
EditIntent intent_from_hit(const WaveformHitResult& hit, GesturePhase phase) {
    EditIntent intent;
    intent.kind = EditIntentKind::Move;
    intent.phase = phase;
    intent.sequence_id = {3};
    intent.track_id = {4};
    intent.clip_id = {5};
    intent.expected_range = MusicalTimeRange{{0}, {kTicksPerQuarter}};
    intent.replacement_range = MusicalTimeRange{{hit.sample}, {kTicksPerQuarter}};
    return intent;
}

EditIntentIdentity fixed_identity() {
    EditIntentIdentity identity;
    identity.transaction_id = TransactionId{WriterId{1}, 1};
    identity.command_id = CommandId{WriterId{1}, 1};
    identity.expected_revision = {};
    return identity;
}

NoteEvent note(ItemId id = {20}, std::int64_t start = 0, std::int64_t duration = kTicksPerQuarter) {
    return NoteEvent{id, {start}, {duration}, 48'000, 64, 2};
}

NoteEditIntent note_intent(NoteEditIntentKind kind) {
    NoteEditIntent intent;
    intent.kind = kind;
    intent.sequence_id = {3};
    intent.track_id = {4};
    intent.clip_id = {5};
    return intent;
}

Clip note_clip_with_lane_value(std::uint32_t value) {
    MidiExpressionLane lane;
    lane.id = {30};
    lane.address = {0, 2, 0xb, 0, 1};
    lane.points.push_back({{31}, {0}, value});
    auto content = MidiContent::create({note()}, {}, 0, {std::move(lane)});
    return Clip::create({9}, {0}, {kTicksPerQuarter}, std::move(content).value()).value();
}

} // namespace

TEST_CASE("Selection set operations keep one canonical identity set") {
    Selection selection;
    const std::array initial{ItemId{7}, ItemId{5}, ItemId{5}, ItemId{3}, ItemId{}};
    selection.replace(initial);

    REQUIRE(std::vector(selection.items().begin(), selection.items().end()) ==
            std::vector<ItemId>{ItemId{3}, ItemId{5}, ItemId{7}});
    REQUIRE(selection.anchor() == ItemId{7});

    selection.add({6});
    selection.add({6});
    selection.toggle({5});
    selection.toggle({9});
    REQUIRE(std::vector(selection.items().begin(), selection.items().end()) ==
            std::vector<ItemId>{ItemId{3}, ItemId{6}, ItemId{7}, ItemId{9}});
    REQUIRE(selection.anchor() == ItemId{7});

    const std::array authored_order{ItemId{3}, ItemId{5}, ItemId{6}, ItemId{7}, ItemId{9}};
    REQUIRE(selection.extend_from_anchor({5}, authored_order));
    REQUIRE(std::vector(selection.items().begin(), selection.items().end()) ==
            std::vector<ItemId>{ItemId{5}, ItemId{6}, ItemId{7}});
    REQUIRE_FALSE(selection.extend_from_anchor({8}, authored_order));
    REQUIRE(std::vector(selection.items().begin(), selection.items().end()) ==
            std::vector<ItemId>{ItemId{5}, ItemId{6}, ItemId{7}});

    const std::array non_monotonic_order{ItemId{9}, ItemId{}, ItemId{2}, ItemId{7}, ItemId{5}};
    SECTION("Range canonicalization omits invalid authored identities") {
        REQUIRE(selection.extend_from_anchor({9}, non_monotonic_order));
        REQUIRE(std::vector(selection.items().begin(), selection.items().end()) ==
                std::vector<ItemId>{ItemId{2}, ItemId{7}, ItemId{9}});
        REQUIRE(selection.anchor() == ItemId{7});
    }

    SECTION("An invalid target leaves selection state unchanged") {
        const auto before_invalid_target =
            std::vector(selection.items().begin(), selection.items().end());
        REQUIRE_FALSE(selection.extend_from_anchor(ItemId{}, non_monotonic_order));
        REQUIRE(std::vector(selection.items().begin(), selection.items().end()) ==
                before_invalid_target);
        REQUIRE(selection.anchor() == ItemId{7});
    }
}

TEST_CASE("Selection pruning follows active project identities without joining undo history") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();

    Selection selection;
    const std::array selected{ItemId{5}, ItemId{6}, ItemId{999}};
    selection.replace(selected);
    selection.prune(*session->snapshot());
    REQUIRE(std::vector(selection.items().begin(), selection.items().end()) ==
            std::vector<ItemId>{ItemId{5}, ItemId{6}});

    auto remove = session_transaction(writer, session->revision(),
                                      {RemoveClip{ItemId{3}, ItemId{4}, ItemId{5}}});
    REQUIRE(session->submit(writer, std::move(remove)));
    REQUIRE_FALSE(session->snapshot()->locate({5})->active);
    REQUIRE_FALSE(session->snapshot()->locate({6})->active);
    REQUIRE(std::vector(selection.items().begin(), selection.items().end()) ==
            std::vector<ItemId>{ItemId{5}, ItemId{6}});

    REQUIRE(session->undo(writer));
    REQUIRE(session->snapshot()->locate({5})->active);
    REQUIRE(session->snapshot()->locate({6})->active);
    selection.prune(*session->snapshot());
    REQUIRE(std::vector(selection.items().begin(), selection.items().end()) ==
            std::vector<ItemId>{ItemId{5}, ItemId{6}});

    REQUIRE(session->redo(writer));
    selection.prune(*session->snapshot());
    REQUIRE(selection.empty());
    REQUIRE_FALSE(selection.anchor());

    REQUIRE(session->undo(writer));
    REQUIRE(session->snapshot()->locate({5})->active);
    REQUIRE(selection.empty());
}

TEST_CASE("Edit intents from a mouse and a touch pointer lower to identical transactions") {
    const auto viewport = parity_viewport();
    const auto model = parity_model();

    // 2px from the selection-start handle: inside both the 4px mouse tolerance
    // and the 22px touch tolerance, so both devices resolve the same handle.
    const auto mouse_hit = hit_test_waveform_handles(viewport, model, 102.0f,
                                                     HitMetrics::for_pointer(PointerType::mouse));
    const auto touch_hit = hit_test_waveform_handles(viewport, model, 102.0f,
                                                     HitMetrics::for_pointer(PointerType::touch));
    REQUIRE(mouse_hit.kind == WaveformHandleKind::selection_start);
    REQUIRE(touch_hit.kind == WaveformHandleKind::selection_start);
    REQUIRE(mouse_hit.sample == touch_hit.sample);

    auto from_mouse =
        lower_edit_intent(intent_from_hit(mouse_hit, GesturePhase::Single), fixed_identity());
    auto from_touch =
        lower_edit_intent(intent_from_hit(touch_hit, GesturePhase::Single), fixed_identity());
    REQUIRE(from_mouse);
    REQUIRE(from_touch);
    REQUIRE(equivalent(from_mouse.value(), from_touch.value()));
    REQUIRE(from_mouse.value().commands.size() == 1);
    REQUIRE(std::holds_alternative<MoveClip>(from_mouse.value().commands[0].command));
}

TEST_CASE("Hit metrics widen the target for touch without a second hit test") {
    const auto viewport = parity_viewport();
    const auto model = parity_model();

    // 10px away: outside the mouse target, inside the touch target. The two
    // results differ here precisely because the metrics differ, which is what
    // makes the identical-result case above meaningful rather than vacuous.
    const auto mouse_miss = hit_test_waveform_handles(viewport, model, 110.0f,
                                                      HitMetrics::for_pointer(PointerType::mouse));
    const auto touch_hit = hit_test_waveform_handles(viewport, model, 110.0f,
                                                     HitMetrics::for_pointer(PointerType::touch));
    REQUIRE_FALSE(mouse_miss.hit());
    REQUIRE(touch_hit.kind == WaveformHandleKind::selection_start);
    REQUIRE(touch_hit.sample == 100);
}

TEST_CASE("Hit metrics project onto the tolerance the raw hit test already took") {
    REQUIRE(HitMetrics::for_pointer(PointerType::mouse).tolerance_px() == 4.0f);
    REQUIRE(HitMetrics::for_pointer(PointerType::touch).tolerance_px() == 22.0f);
    REQUIRE(HitMetrics::for_pointer(PointerType::pen).tolerance_px() == 6.0f);

    // A zero extent means "use this pointer type's default", not "no target".
    REQUIRE(HitMetrics{PointerType::touch, 0.0f}.effective_target_pt() == 44.0f);
    REQUIRE(HitMetrics{PointerType::mouse, 20.0f}.tolerance_px() == 10.0f);
    REQUIRE(HitMetrics{PointerType::mouse, 20.0f}.tolerance_px(2.0f) == 20.0f);

    const auto viewport = parity_viewport();
    const auto model = parity_model();
    const auto metrics = HitMetrics::for_pointer(PointerType::touch);
    const auto via_metrics = hit_test_waveform_handles(viewport, model, 110.0f, metrics);
    const auto via_tolerance =
        hit_test_waveform_handles(viewport, model, 110.0f, metrics.tolerance_px());
    REQUIRE(via_metrics.kind == via_tolerance.kind);
    REQUIRE(via_metrics.sample == via_tolerance.sample);
}

TEST_CASE("Edit intent verbs lower onto the commands that already exist") {
    auto identity = fixed_identity();

    EditIntent erase;
    erase.kind = EditIntentKind::Erase;
    erase.sequence_id = {3};
    erase.track_id = {4};
    erase.clip_id = {5};
    auto lowered_erase = lower_edit_intent(erase, identity);
    REQUIRE(lowered_erase);
    REQUIRE(std::holds_alternative<RemoveClip>(lowered_erase.value().commands[0].command));

    EditIntent draw;
    draw.kind = EditIntentKind::Draw;
    draw.sequence_id = {3};
    draw.track_id = {4};
    draw.clip = make_note_clip({9}, {10}, 0);
    auto lowered_draw = lower_edit_intent(draw, identity);
    REQUIRE(lowered_draw);
    REQUIRE(std::holds_alternative<InsertClip>(lowered_draw.value().commands[0].command));

    // A resize is a move whose replacement range changes extent, not a command
    // of its own; the two verbs are distinguished by the front-end, not the model.
    EditIntent resize;
    resize.kind = EditIntentKind::Resize;
    resize.sequence_id = {3};
    resize.track_id = {4};
    resize.clip_id = {5};
    resize.expected_range = MusicalTimeRange{{0}, {kTicksPerQuarter}};
    resize.replacement_range = MusicalTimeRange{{0}, {2 * kTicksPerQuarter}};
    auto lowered_resize = lower_edit_intent(resize, identity);
    REQUIRE(lowered_resize);
    const auto& move = std::get<MoveClip>(lowered_resize.value().commands[0].command);
    REQUIRE(move.clip_id == ItemId{5});
    REQUIRE(equivalent(move.replacement_range,
                       ClipTimeRange{MusicalTimeRange{{0}, {2 * kTicksPerQuarter}}}));
}

TEST_CASE("Edit intent lowering carries the gesture phase and rejects a groupless bracket") {
    auto identity = fixed_identity();
    EditIntent intent;
    intent.kind = EditIntentKind::Erase;
    intent.phase = GesturePhase::Begin;
    intent.sequence_id = {3};
    intent.track_id = {4};
    intent.clip_id = {5};

    auto missing_group = lower_edit_intent(intent, identity);
    REQUIRE_FALSE(missing_group);

    identity.undo_group = UndoGroupId{WriterId{1}, 1};
    auto grouped = lower_edit_intent(intent, identity);
    REQUIRE(grouped);
    REQUIRE(grouped.value().gesture_phase == GesturePhase::Begin);

    intent.phase = GesturePhase::Cancel;
    auto cancelled = lower_edit_intent(intent, identity);
    REQUIRE(cancelled);
    REQUIRE(cancelled.value().gesture_phase == GesturePhase::Cancel);
}

TEST_CASE("A gesture reaches a host as an intent and the host as a transaction") {
    // The seam's parameter is bound to a real vocabulary, not only to a
    // stand-in: a front-end that holds EditIntentHost submits EditIntent and
    // nothing else compiles into that call.
    static_assert(std::is_same_v<EditIntentHost::IntentType, EditIntent>);

    ScriptedUiHost<EditIntent> concrete;
    EditIntentHost& host = concrete;

    const auto viewport = parity_viewport();
    const auto model = parity_model();
    const auto hit = hit_test_waveform_handles(viewport, model, 102.0f,
                                               HitMetrics::for_pointer(PointerType::touch));
    REQUIRE(hit.kind == WaveformHandleKind::selection_start);

    const EditIntent submitted = intent_from_hit(hit, GesturePhase::Single);
    REQUIRE(host.submit_intent(submitted).status == IntentStatus::Accepted);
    REQUIRE(concrete.intents().size() == 1);

    // What the host received lowers to the same transaction the front-end would
    // have produced, so the value that crossed the seam is the whole edit.
    auto direct = lower_edit_intent(submitted, fixed_identity());
    auto via_host = lower_edit_intent(concrete.intents().front(), fixed_identity());
    REQUIRE(direct);
    REQUIRE(via_host);
    REQUIRE(equivalent(direct.value(), via_host.value()));
}

TEST_CASE("Edit intents compare the complete pointer-neutral value") {
    EditIntent lhs;
    lhs.kind = EditIntentKind::Move;
    lhs.sequence_id = {3};
    lhs.track_id = {4};
    lhs.clip_id = {5};
    lhs.expected_range = MusicalTimeRange{{0}, {kTicksPerQuarter}};
    lhs.replacement_range = MusicalTimeRange{{100}, {kTicksPerQuarter}};
    auto rhs = lhs;
    REQUIRE(lhs == rhs);

    rhs.replacement_range = MusicalTimeRange{{101}, {kTicksPerQuarter}};
    REQUIRE_FALSE(lhs == rhs);

    EditIntent draw;
    draw.kind = EditIntentKind::Draw;
    draw.sequence_id = {3};
    draw.track_id = {4};
    draw.clip = make_note_clip({9}, {10}, 0);
    auto same_draw = draw;
    REQUIRE(draw == same_draw);
    same_draw.clip = make_note_clip({11}, {12}, 0);
    REQUIRE_FALSE(draw == same_draw);

    draw.clip = note_clip_with_lane_value(1);
    same_draw = draw;
    same_draw.clip = note_clip_with_lane_value(2);
    REQUIRE_FALSE(draw == same_draw);
}

TEST_CASE("Note edit intents validate each payload shape") {
    auto insert = note_intent(NoteEditIntentKind::Insert);
    insert.replacement = note();
    REQUIRE_FALSE(validate_note_edit_intent(insert));

    auto erase = note_intent(NoteEditIntentKind::Erase);
    erase.expected = note();
    REQUIRE_FALSE(validate_note_edit_intent(erase));

    auto move = note_intent(NoteEditIntentKind::Move);
    move.expected = note();
    move.replacement = note();
    move.replacement->start = {kTicksPerQuarter};
    move.replacement->pitch = 65;
    REQUIRE_FALSE(validate_note_edit_intent(move));

    auto resize = note_intent(NoteEditIntentKind::Resize);
    resize.expected = note();
    resize.replacement = note();
    resize.replacement->duration = {2 * kTicksPerQuarter};
    REQUIRE_FALSE(validate_note_edit_intent(resize));

    auto velocity = note_intent(NoteEditIntentKind::SetVelocity);
    velocity.expected = note();
    velocity.replacement = note();
    velocity.replacement->velocity = 32'000;
    REQUIRE_FALSE(validate_note_edit_intent(velocity));
}

TEST_CASE("Note edit intent validation rejects ambiguous or malformed payloads") {
    auto missing = note_intent(NoteEditIntentKind::Insert);
    auto error = validate_note_edit_intent(missing);
    REQUIRE(error);
    REQUIRE(error->code == ModelErrorCode::MissingItem);

    auto ambiguous = note_intent(NoteEditIntentKind::Insert);
    ambiguous.expected = note();
    ambiguous.replacement = note();
    error = validate_note_edit_intent(ambiguous);
    REQUIRE(error);
    REQUIRE(error->code == ModelErrorCode::InvalidNote);
    auto rejected_for_host = ValidatedNoteEditIntent::create(ambiguous);
    REQUIRE_FALSE(rejected_for_host);
    REQUIRE(rejected_for_host.error().code == ModelErrorCode::InvalidNote);

    auto invalid_phase = note_intent(NoteEditIntentKind::Insert);
    invalid_phase.phase = static_cast<GesturePhase>(255);
    invalid_phase.replacement = note();
    error = validate_note_edit_intent(invalid_phase);
    REQUIRE(error);
    REQUIRE(error->code == ModelErrorCode::InvalidNote);
    auto invalid_phase_for_host = ValidatedNoteEditIntent::create(invalid_phase);
    REQUIRE_FALSE(invalid_phase_for_host);
    REQUIRE(invalid_phase_for_host.error().code == ModelErrorCode::InvalidNote);

    auto mismatched = note_intent(NoteEditIntentKind::Move);
    mismatched.expected = note({20});
    mismatched.replacement = note({21}, kTicksPerQuarter);
    error = validate_note_edit_intent(mismatched);
    REQUIRE(error);
    REQUIRE(error->code == ModelErrorCode::IdentityConflict);
    REQUIRE(error->item == ItemId{20});
    REQUIRE(error->related_item == ItemId{21});

    auto malformed = note_intent(NoteEditIntentKind::Erase);
    malformed.expected = note({20}, 0, 0);
    error = validate_note_edit_intent(malformed);
    REQUIRE(error);
    REQUIRE(error->code == ModelErrorCode::InvalidNote);
    REQUIRE(error->item == ItemId{20});

    auto contradictory = note_intent(NoteEditIntentKind::SetVelocity);
    contradictory.expected = note();
    contradictory.replacement = note();
    contradictory.replacement->start = {kTicksPerQuarter};
    contradictory.replacement->velocity = 32'000;
    error = validate_note_edit_intent(contradictory);
    REQUIRE(error);
    REQUIRE(error->code == ModelErrorCode::InvalidNote);
    REQUIRE(error->item == ItemId{20});
}

TEST_CASE("A note gesture crosses the host seam as one comparable value") {
    static_assert(std::is_same_v<NoteEditIntentHost::IntentType, ValidatedNoteEditIntent>);

    auto submitted = note_intent(NoteEditIntentKind::Move);
    submitted.phase = GesturePhase::Update;
    submitted.expected = note();
    submitted.replacement = note();
    submitted.replacement->start = {kTicksPerQuarter};
    REQUIRE_FALSE(validate_note_edit_intent(submitted));
    auto validated = ValidatedNoteEditIntent::create(submitted);
    REQUIRE(validated);

    ScriptedUiHost<ValidatedNoteEditIntent> concrete;
    NoteEditIntentHost& host = concrete;
    REQUIRE(host.submit_intent(validated.value()).status == IntentStatus::Accepted);
    REQUIRE(concrete.intents().size() == 1);
    REQUIRE(concrete.intents().front().value() == submitted);

    auto different = submitted;
    different.replacement->pitch = 65;
    auto different_validated = ValidatedNoteEditIntent::create(different);
    REQUIRE(different_validated);
    REQUIRE_FALSE(different_validated.value() == validated.value());
}
