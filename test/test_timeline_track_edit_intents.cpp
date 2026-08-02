#include "timeline_command_test_helpers.hpp"

#include <pulp/timeline/serialize.hpp>
#include <pulp/timeline_editor/track_edit_intent.hpp>

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

using namespace pulp::timeline;
using namespace pulp::timeline_editor;
using namespace timeline_test;

namespace {

constexpr ItemId kSequence{3};
constexpr ItemId kFirst{4};
constexpr ItemId kSecond{7};
constexpr ItemId kThird{9};

/// Three tracks whose authored order is deliberately NOT their identity order,
/// so a reorder that was dropped — or an order derived from identity instead of
/// read from the document — is visible rather than coincidentally correct.
Project make_ordered_project() {
    auto first = Track::create(kFirst, "first", {});
    REQUIRE(first);
    auto second = Track::create(kSecond, "second", {});
    REQUIRE(second);
    auto third = Track::create(kThird, "third", {});
    REQUIRE(third);
    auto sequence = Sequence::create(
        SequenceInput{.id = kSequence,
                      .name = "sequence",
                      .musical_duration = TickDuration{8 * kTicksPerQuarter},
                      .tracks = {std::move(first).value(), std::move(second).value(),
                                 std::move(third).value()},
                      .track_order = {kThird, kFirst, kSecond}});
    REQUIRE(sequence);
    auto project = Project::create(
        ProjectInput{.id = {1},
                     .name = "project",
                     .next_item_id = 10,
                     .root_sequence_id = kSequence,
                     .sequences = {std::move(sequence).value()}});
    REQUIRE(project);
    return std::move(project).value();
}

std::vector<ItemId> authored_order(const Project& project) {
    const auto order = project.find_sequence(kSequence)->track_order();
    return {order.begin(), order.end()};
}

EditIntentIdentity identity_for(WriterToken& writer, DocumentRevision revision,
                                std::optional<UndoGroupId> group = std::nullopt) {
    EditIntentIdentity identity;
    identity.transaction_id = writer.allocate_transaction_id();
    identity.command_id = writer.allocate_command_id();
    identity.expected_revision = revision;
    identity.undo_group = group;
    return identity;
}

TrackEditIntent reorder(ItemId track, std::optional<ItemId> expected_before,
                        std::optional<ItemId> replacement_before) {
    TrackEditIntent intent;
    intent.kind = TrackEditIntentKind::Reorder;
    intent.sequence_id = kSequence;
    intent.track_id = track;
    intent.expected_before_track_id = expected_before;
    intent.replacement_before_track_id = replacement_before;
    return intent;
}

} // namespace

// The whole point of the channel: an intent a front-end builds from a drop
// position reaches the document as an authored reorder. Asserted as the order
// itself rather than as a successful commit, and across a save/reopen, because a
// reorder that committed but did not persist is the failure this slice exists to
// prevent.
TEST_CASE("A track reorder intent lowers and persists the authored order") {
    auto session = std::move(DocumentSession::create(make_ordered_project())).value();
    auto writer = std::move(session->register_writer()).value();
    REQUIRE(authored_order(*session->snapshot()) == std::vector<ItemId>{kThird, kFirst, kSecond});

    // Drag `second` to the front: it should precede `third`, which currently leads.
    auto lowered = lower_track_edit_intent(reorder(kSecond, std::nullopt, kThird),
                                           identity_for(writer, session->revision()));
    REQUIRE(lowered);
    REQUIRE(session->submit(writer, std::move(lowered).value()));

    const auto expected = std::vector<ItemId>{kSecond, kThird, kFirst};
    REQUIRE(authored_order(*session->snapshot()) == expected);

    auto registry = make_builtin_timeline_registry();
    REQUIRE(registry);
    auto written = serialize_project(*session->snapshot(), registry.value());
    REQUIRE(written);
    auto reopened = deserialize_project(written->json, registry.value());
    REQUIRE(reopened);
    REQUIRE(authored_order(reopened.value()) == expected);
}

// A destination of nullopt means last position, which is a request rather than
// an omission. Without this the natural reading of the optional is "unset", and
// a front-end dropping a track at the end of the list would have no way to say so.
TEST_CASE("A track reorder intent with no destination moves the track last") {
    auto session = std::move(DocumentSession::create(make_ordered_project())).value();
    auto writer = std::move(session->register_writer()).value();

    auto lowered = lower_track_edit_intent(reorder(kThird, kFirst, std::nullopt),
                                           identity_for(writer, session->revision()));
    REQUIRE(lowered);
    REQUIRE(session->submit(writer, std::move(lowered).value()));
    REQUIRE(authored_order(*session->snapshot()) ==
            std::vector<ItemId>{kFirst, kSecond, kThird});
}

// The bracket rules are the clip channel's, and they have to hold here too or a
// track drag could open a gesture the session then refuses mid-stream. Asserted
// as a pair: the same intent lowers with a valid group and is refused without one.
TEST_CASE("A track reorder intent requires an undo group once its phase is not Single") {
    auto session = std::move(DocumentSession::create(make_ordered_project())).value();
    auto writer = std::move(session->register_writer()).value();
    const auto group = writer.allocate_undo_group_id();

    auto intent = reorder(kSecond, std::nullopt, kThird);
    intent.phase = GesturePhase::Begin;

    REQUIRE_FALSE(lower_track_edit_intent(intent, identity_for(writer, session->revision())));
    REQUIRE(lower_track_edit_intent(intent, identity_for(writer, session->revision(), group)));
}

// A destination the front-end never resolved is a malformed gesture, and it is
// worth failing here rather than reaching the reducer as a missing item — the two
// are different diagnoses. An ABSENT destination stays legal, which is the case a
// blanket "destination must be valid" check would break.
TEST_CASE("A track reorder intent rejects a supplied neighbour that is not an identity") {
    auto session = std::move(DocumentSession::create(make_ordered_project())).value();
    auto writer = std::move(session->register_writer()).value();

    // Both neighbours are checked, and both are asserted. One case covering only
    // the destination leaves the gate's half of the same condition unexercised —
    // which a control that disables one disjunct passes without noticing.
    REQUIRE_FALSE(lower_track_edit_intent(reorder(kSecond, std::nullopt, ItemId{}),
                                          identity_for(writer, session->revision())));
    REQUIRE_FALSE(lower_track_edit_intent(reorder(kSecond, ItemId{}, kThird),
                                          identity_for(writer, session->revision())));
    // Absent stays legal, which is the case a blanket "must be valid" would break.
    REQUIRE(lower_track_edit_intent(reorder(kSecond, std::nullopt, std::nullopt),
                                    identity_for(writer, session->revision())));
}

// Naming the moved track as its own destination describes no position. The model
// refuses it with a stated reason, and the lowerer deliberately does not, so this
// pins WHERE the refusal comes from: a second copy of the rule up here could
// drift from the one the editing paths enforce.
TEST_CASE("A track reorder onto itself is refused by the model rather than the lowerer") {
    auto session = std::move(DocumentSession::create(make_ordered_project())).value();
    auto writer = std::move(session->register_writer()).value();

    auto lowered = lower_track_edit_intent(reorder(kSecond, std::nullopt, kSecond),
                                           identity_for(writer, session->revision()));
    REQUIRE(lowered);
    const auto refused = session->submit(writer, std::move(lowered).value());
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error().code == ConflictCode::ModelInvariant);
    REQUIRE(authored_order(*session->snapshot()) ==
            std::vector<ItemId>{kThird, kFirst, kSecond});
}
