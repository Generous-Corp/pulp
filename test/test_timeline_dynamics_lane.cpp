#include "support/timeline_persistence_test_support.hpp"

#include <pulp/timeline/compile_context.hpp>
#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/transaction.hpp>

#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <bit>
#include <cmath>

using Catch::Matchers::WithinAbs;

namespace {

DynamicsLane lane_of(std::vector<DynamicsEvent> events) {
    return take(DynamicsLane::create(std::move(events)));
}

// A ramp from quiet to loud across one bar, then a held tail. Two segments so a
// test can tell an interpolated read from a held one.
DynamicsLane ramp_then_hold() {
    return lane_of({
        DynamicsEvent{{0}, 0.25f, AutomationInterpolation::Continuous},
        DynamicsEvent{{1920}, 0.75f, AutomationInterpolation::Hold},
        DynamicsEvent{{3840}, 1.0f, AutomationInterpolation::Continuous},
    });
}

Project project_with_dynamics(DynamicsLane lane) {
    auto clip = take(Clip::create({4}, {0}, {100}, EmptyContent{}));
    auto track = take(Track::create({3}, "track", {clip}));
    auto sequence = take(Sequence::create(SequenceInput{.id = {2},
                                                        .name = "sequence",
                                                        .musical_duration = TickDuration{4000},
                                                        .tracks = {track},
                                                        .dynamics_lane = std::move(lane)}));
    return take(Project::create(ProjectInput{{1}, "project", 5, {2}, {}, {sequence}}));
}

} // namespace

TEST_CASE("dynamics lane rejects malformed events and preserves authored order",
          "[timeline][dynamics-lane]") {
    // Intensity is a closed normalized range, so out-of-range is rejected rather
    // than clamped: a clamp would hand back a lane the caller never described.
    REQUIRE_FALSE(DynamicsLane::create({DynamicsEvent{{0}, 1.5f}}));
    REQUIRE_FALSE(DynamicsLane::create({DynamicsEvent{{0}, -0.1f}}));
    REQUIRE_FALSE(DynamicsLane::create({DynamicsEvent{{0}, std::nanf("")}}));
    REQUIRE_FALSE(DynamicsLane::create({DynamicsEvent{{-1}, 0.5f}}));

    // Authored order is the document's order: out-of-order and duplicate
    // positions are rejections, never a silent sort.
    REQUIRE_FALSE(
        DynamicsLane::create({DynamicsEvent{{960}, 0.5f}, DynamicsEvent{{0}, 0.5f}}));
    REQUIRE_FALSE(
        DynamicsLane::create({DynamicsEvent{{960}, 0.5f}, DynamicsEvent{{960}, 0.6f}}));

    // The boundary values themselves are legal; only outside the range is not.
    REQUIRE(DynamicsLane::create({DynamicsEvent{{0}, 0.0f}, DynamicsEvent{{1}, 1.0f}}));

    const auto lane = ramp_then_hold();
    REQUIRE_FALSE(lane.empty());
    REQUIRE(lane.events().size() == 3);
    REQUIRE(DynamicsLane::create({}).value().empty());
}

TEST_CASE("dynamics value at position interpolates the declared curve",
          "[timeline][dynamics-lane]") {
    const auto lane = ramp_then_hold();

    // Before the first event the lane says nothing. Empty is not zero: an
    // unstated intensity is a different claim from an authored silence.
    REQUIRE(lane.at({-1}) == nullptr);
    REQUIRE_FALSE(lane.value_at({-1}).has_value());

    // Exactly on an event reads that event's own value, on either curve kind.
    REQUIRE_THAT(lane.value_at({0}).value(), WithinAbs(0.25f, 1e-6));
    REQUIRE_THAT(lane.value_at({1920}).value(), WithinAbs(0.75f, 1e-6));

    // A Continuous segment ramps linearly to its successor. Halfway between
    // 0.25 and 0.75 is 0.5, and a quarter of the way is 0.375.
    REQUIRE_THAT(lane.value_at({960}).value(), WithinAbs(0.5f, 1e-6));
    REQUIRE_THAT(lane.value_at({480}).value(), WithinAbs(0.375f, 1e-6));

    // A Hold segment keeps the earlier value for its whole span, right up to
    // the instant the next event takes over. This is the assertion that fails
    // if Hold were ever treated as Continuous.
    REQUIRE_THAT(lane.value_at({2880}).value(), WithinAbs(0.75f, 1e-6));
    REQUIRE_THAT(lane.value_at({3839}).value(), WithinAbs(0.75f, 1e-6));
    REQUIRE_THAT(lane.value_at({3840}).value(), WithinAbs(1.0f, 1e-6));

    // From the last event onward the final value holds; there is no successor
    // to ramp toward, whatever that event's interpolation says.
    REQUIRE_THAT(lane.value_at({100000}).value(), WithinAbs(1.0f, 1e-6));

    // A single-event lane has no segment at all and simply holds.
    const auto single = lane_of({DynamicsEvent{{100}, 0.4f}});
    REQUIRE_FALSE(single.value_at({99}).has_value());
    REQUIRE_THAT(single.value_at({100}).value(), WithinAbs(0.4f, 1e-6));
    REQUIRE_THAT(single.value_at({99999}).value(), WithinAbs(0.4f, 1e-6));

    // An empty lane answers nothing anywhere rather than defaulting.
    REQUIRE_FALSE(lane_of({}).value_at({0}).has_value());
}

TEST_CASE("dynamics context resolves only for a renderer that declared it",
          "[timeline][dynamics-lane]") {
    const auto project = project_with_dynamics(ramp_then_hold());

    // The declaration is load-bearing at the point of the read, not paperwork
    // filed at registration: an undeclared kind reads as absent even though the
    // lane is right there in the snapshot.
    const CompileContextView undeclared(project, {2}, CompileContextSubscriptions::none());
    REQUIRE(undeclared.dynamics_lane() == nullptr);
    REQUIRE_FALSE(undeclared.dynamics_at({960}).has_value());

    // Declaring a *different* kind does not open this one; the bits are
    // independent, which is what keeps invalidation narrow.
    const CompileContextView other_kind(
        project, {2}, CompileContextSubscriptions().subscribe(CompileContextKind::ChordScale));
    REQUIRE(other_kind.dynamics_lane() == nullptr);
    REQUIRE(other_kind.chord_scale_lane() != nullptr);

    const CompileContextView declared(
        project, {2}, CompileContextSubscriptions().subscribe(CompileContextKind::Dynamics));
    REQUIRE(declared.dynamics_lane() != nullptr);
    REQUIRE_THAT(declared.dynamics_at({960}).value(), WithinAbs(0.5f, 1e-6));

    // A sequence outside the snapshot resolves to absent rather than crashing.
    const CompileContextView missing(
        project, {999}, CompileContextSubscriptions().subscribe(CompileContextKind::Dynamics));
    REQUIRE(missing.dynamics_lane() == nullptr);
    REQUIRE_FALSE(missing.dynamics_at({0}).has_value());
}

TEST_CASE("dynamics lane survives identity remapping and shares storage on copy",
          "[timeline][dynamics-lane]") {
    // Rebuilding a sequence under new ids must carry the lane. A rebuild path
    // that forgets it would drop authored dynamics silently, with nothing in
    // the type system to notice.
    const auto project = project_with_dynamics(ramp_then_hold());
    const auto* sequence = project.find_sequence({2});
    REQUIRE(sequence != nullptr);
    REQUIRE(sequence->dynamics_lane().events().size() == 3);

    ItemIdAllocator allocator{100};
    auto remapped = remap_ids(*sequence, allocator);
    REQUIRE(remapped);
    REQUIRE(remapped.value().sequence.dynamics_lane() == sequence->dynamics_lane());
    REQUIRE(remapped.value().sequence.dynamics_lane().events().size() == 3);

    // The lane is immutable and shared: a snapshot that replaces something else
    // must not deep-copy the events.
    const auto replaced = sequence->with_groove(sequence->groove());
    REQUIRE(replaced.dynamics_lane().shares_storage_with(sequence->dynamics_lane()));

    // Replacing the lane itself yields a different lane but leaves the original
    // snapshot untouched, which is what makes the model immutable.
    const auto quiet = lane_of({DynamicsEvent{{0}, 0.1f}});
    const auto swapped = sequence->with_dynamics_lane(quiet);
    REQUIRE(swapped.dynamics_lane() == quiet);
    REQUIRE(sequence->dynamics_lane().events().size() == 3);
}

TEST_CASE("setting the dynamics lane dirties the context kind and nothing else",
          "[timeline][dynamics-lane][dirty-set]") {
    const auto project = project_with_dynamics(lane_of({}));
    const auto replacement = ramp_then_hold();

    Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.commands.push_back({{{1}, 1}, SetDynamicsLane{{2}, lane_of({}), replacement}});
    auto reduced = take(reduce_transaction(project, transaction));

    REQUIRE(reduced.project.find_sequence({2})->dynamics_lane().events().size() == 3);
    REQUIRE(reduced.dirty.contexts().size() == 1);
    REQUIRE(reduced.dirty.contexts()[0].owner_sequence == ItemId{2});
    REQUIRE(reduced.dirty.contexts()[0].kind == CompileContextKind::Dynamics);

    // The companion item names the sequence and no track: a lane's readers are
    // not its children, so it cannot name them.
    REQUIRE(reduced.dirty.items().size() == 1);
    REQUIRE(reduced.dirty.items()[0].owner_sequence == ItemId{2});
    REQUIRE_FALSE(reduced.dirty.items()[0].owner_track.valid());
    REQUIRE(reduced.dirty.items()[0].flags == DirtyFlags::Context);

    // The inverse is the same command with the two lanes swapped, so undo
    // restores the previous lane exactly rather than approximating it.
    REQUIRE(reduced.inverses.size() == 1);
    Transaction undo;
    undo.id = {{1}, 2};
    undo.commands.push_back({{{1}, 2}, reduced.inverses[0]});
    auto restored = take(reduce_transaction(reduced.project, undo));
    REQUIRE(restored.project.find_sequence({2})->dynamics_lane().empty());
}

TEST_CASE("the dynamics lane gate refuses a stale expectation and a missing sequence",
          "[timeline][dynamics-lane][dirty-set]") {
    const auto project = project_with_dynamics(ramp_then_hold());
    const auto replacement = lane_of({DynamicsEvent{{0}, 0.5f}});

    // A caller that read the lane before someone else changed it is refused
    // rather than allowed to clobber the version it never saw.
    Transaction stale;
    stale.id = {{1}, 1};
    stale.commands.push_back({{{1}, 1}, SetDynamicsLane{{2}, lane_of({}), replacement}});
    auto rejected = reduce_transaction(project, stale);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::ExpectedValueMismatch);
    // The refusal leaves the document exactly as it was: a rejected gate is not
    // a partial write.
    REQUIRE(project.find_sequence({2})->dynamics_lane() == ramp_then_hold());

    // A command naming a sequence that does not exist is refused, not applied
    // to whichever sequence happens to be first.
    Transaction missing;
    missing.id = {{1}, 2};
    missing.commands.push_back(
        {{{1}, 2}, SetDynamicsLane{{99}, ramp_then_hold(), replacement}});
    auto absent = reduce_transaction(project, missing);
    REQUIRE_FALSE(absent);
    REQUIRE(absent.error().code == ConflictCode::TargetMissing);
}

TEST_CASE("a set_dynamics_lane command round trips through its schema envelope",
          "[timeline][dynamics-lane][persistence]") {
    const auto registry = builtins();
    // Intensity travels as its float bit pattern, so the value a caller read
    // back is the value the gate compares — 0.25f is 0x3e800000.
    const auto bits = [](float value) {
        return std::to_string(static_cast<std::uint64_t>(std::bit_cast<std::uint32_t>(value)));
    };
    const std::string encoded =
        R"({"data":{"expected":[],"replacement":[{"intensity_bits":")" + bits(0.25f) +
        R"(","interpolation":"continuous","position":"0"},{"intensity_bits":")" + bits(1.0f) +
        R"(","interpolation":"hold","position":"1920"}],"sequence_id":"2"},)"
        R"("type_name":"pulp.timeline.command.set_dynamics_lane","version":1})";
    auto decoded = deserialize_commands("[" + encoded + "]", registry);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded.value().size() == 1);
    const auto* command = std::get_if<SetDynamicsLane>(&decoded.value()[0]);
    REQUIRE(command != nullptr);
    REQUIRE(command->sequence_id == ItemId{2});
    REQUIRE(command->expected.empty());
    REQUIRE(command->replacement.events().size() == 2);
    REQUIRE(command->replacement.events()[0] ==
            DynamicsEvent{{0}, 0.25f, AutomationInterpolation::Continuous});
    REQUIRE(command->replacement.events()[1] ==
            DynamicsEvent{{1920}, 1.0f, AutomationInterpolation::Hold});

    // The model's rules are enforced at decode, so a command carrying a lane
    // the model would reject cannot be constructed at all.
    const std::string unordered =
        R"({"data":{"expected":[],"replacement":[{"intensity_bits":")" + bits(0.5f) +
        R"(","interpolation":"hold","position":"1920"},{"intensity_bits":")" + bits(0.5f) +
        R"(","interpolation":"hold","position":"0"}],"sequence_id":"2"},)"
        R"("type_name":"pulp.timeline.command.set_dynamics_lane","version":1})";
    REQUIRE_FALSE(deserialize_commands("[" + unordered + "]", registry));

    const std::string too_loud =
        R"({"data":{"expected":[],"replacement":[{"intensity_bits":")" + bits(1.5f) +
        R"(","interpolation":"hold","position":"0"}],"sequence_id":"2"},)"
        R"("type_name":"pulp.timeline.command.set_dynamics_lane","version":1})";
    REQUIRE_FALSE(deserialize_commands("[" + too_loud + "]", registry));

    // An unknown interpolation spelling is a refusal rather than a fallback to
    // whichever member happens to be first in the enum.
    const std::string bad_curve =
        R"({"data":{"expected":[],"replacement":[{"intensity_bits":")" + bits(0.5f) +
        R"(","interpolation":"stepped","position":"0"}],"sequence_id":"2"},)"
        R"("type_name":"pulp.timeline.command.set_dynamics_lane","version":1})";
    REQUIRE_FALSE(deserialize_commands("[" + bad_curve + "]", registry));
}

TEST_CASE("the dynamics lane command carries annotation authority a writer profile can gate",
          "[timeline][dynamics-lane][capabilities]") {
    constexpr auto authority = command_authority_of<SetDynamicsLane>();
    STATIC_REQUIRE(authority.command_class == CommandClass::Annotation);
    STATIC_REQUIRE(authority.intent == CommandIntent::Modify);

    // Both directions matter: a profile that denies nothing must admit the
    // command, or the deny case below would pass against a mask that refuses
    // everything.
    REQUIRE(allows(unrestricted_capabilities(), authority));
    const auto denied =
        deny(unrestricted_capabilities(), CommandClass::Annotation, CommandIntent::Modify);
    REQUIRE_FALSE(allows(denied, authority));

    // Denying a neighbouring class leaves this command reachable, so the gate
    // is the authority pair rather than a blanket annotation ban.
    const auto elsewhere =
        deny(unrestricted_capabilities(), CommandClass::Note, CommandIntent::Modify);
    REQUIRE(allows(elsewhere, authority));
}
