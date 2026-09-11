#include "support/timeline_persistence_test_support.hpp"

#include <pulp/timeline/compile_context.hpp>

#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>

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

TEST_CASE("dynamics lane survives a save and reload byte for byte",
          "[timeline][dynamics-lane][persistence]") {
    const auto registry = builtins();
    // Intensities that are not exactly representable in decimal, so a writer
    // that rounded through text instead of carrying the bit pattern would come
    // back different.
    const auto authored = lane_of({
        DynamicsEvent{{0}, 0.1f, AutomationInterpolation::Continuous},
        DynamicsEvent{{1920}, 0.7f, AutomationInterpolation::Hold},
        DynamicsEvent{{3840}, 1.0f, AutomationInterpolation::Continuous},
    });
    const auto original = project_with_dynamics(authored);

    const auto first = take(serialize_project(original, registry));
    // The lane is written under its own member, between the chord lane and the
    // groove in canonical order, as the intensity's bit pattern plus the
    // interpolation that leaves each event.
    REQUIRE(first.json.find(R"("chord_scale_lane":[],"dynamics_lane":[{"intensity_bits":")") !=
            std::string::npos);
    REQUIRE(first.json.find(R"("interpolation":"hold","position":"1920"})") !=
            std::string::npos);
    REQUIRE(first.json.find(R"(}],"groove":{)") != std::string::npos);

    const auto restored = take(deserialize_project(first.json, registry));
    const auto* sequence = restored.find_sequence({2});
    REQUIRE(sequence != nullptr);
    // Equality compares every authored value, including the float bits, so a
    // lossy intensity or a dropped interpolation fails here rather than being
    // rounded past.
    REQUIRE(sequence->dynamics_lane() == authored);
    REQUIRE(sequence->dynamics_lane().events().size() == 3);
    REQUIRE(sequence->dynamics_lane().events()[1].interpolation == AutomationInterpolation::Hold);

    // A re-save reproduces the first save exactly.
    REQUIRE(take(serialize_project(restored, registry)).json == first.json);

    // A sequence that states no dynamics writes an empty lane rather than
    // omitting the member, so the version gate stays a presence test.
    const auto empty_json = take(serialize_project(project_with_dynamics(lane_of({})), registry));
    REQUIRE(empty_json.json.find(R"("chord_scale_lane":[],"dynamics_lane":[],"groove":{)") !=
            std::string::npos);
    REQUIRE(take(deserialize_project(empty_json.json, registry))
                .find_sequence({2})
                ->dynamics_lane()
                .empty());
}

TEST_CASE("a document whose dynamics lane is malformed is rejected on load",
          "[timeline][dynamics-lane][persistence]") {
    const auto registry = builtins();
    const auto json = take(serialize_project(project_with_dynamics(ramp_then_hold()), registry)).json;
    const auto bits_of = [](float value) {
        return std::to_string(std::bit_cast<std::uint32_t>(value));
    };
    const std::string first_event = R"("intensity_bits":")" + bits_of(0.25f) + R"(")";
    const auto first_at = json.find(first_event);
    REQUIRE(first_at != std::string::npos);

    // An intensity outside [0, 1] is a model rejection on the way in, not a
    // clamp: the bit pattern decodes, and the factory refuses the value.
    auto too_loud = json;
    too_loud.replace(first_at, first_event.size(),
                     R"("intensity_bits":")" + bits_of(1.5f) + R"(")");
    auto rejected = deserialize_project(too_loud, registry);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().model_error.has_value());

    // A NaN has a bit pattern too, and it is refused for the same reason.
    auto not_a_number = json;
    not_a_number.replace(first_at, first_event.size(),
                         R"("intensity_bits":")" + bits_of(std::nanf("")) + R"(")");
    REQUIRE_FALSE(deserialize_project(not_a_number, registry));

    // A pattern wider than a float is a number the writer could never have
    // produced, so it is refused before the bits are ever reinterpreted.
    auto too_wide = json;
    too_wide.replace(first_at, first_event.size(), R"("intensity_bits":"4294967296")");
    REQUIRE_FALSE(deserialize_project(too_wide, registry));

    // An interpolation name outside the shared vocabulary is not coerced.
    auto unknown_curve = json;
    const auto curve_at = unknown_curve.find(R"("interpolation":"hold")");
    REQUIRE(curve_at != std::string::npos);
    unknown_curve.replace(curve_at, std::string_view(R"("interpolation":"hold")").size(),
                          R"("interpolation":"step")");
    REQUIRE_FALSE(deserialize_project(unknown_curve, registry));

    // Descending positions would change which intensity is in force where, so
    // the loader refuses rather than sorting the document into a new
    // performance.
    auto descending = json;
    const auto position_at = descending.find(R"("position":"1920"})");
    REQUIRE(position_at != std::string::npos);
    descending.replace(position_at, std::string_view(R"("position":"1920"})").size(),
                       R"("position":"-960"})");
    REQUIRE_FALSE(deserialize_project(descending, registry));

    // An event missing a member is a missing field, not a defaulted one.
    auto partial = json;
    const auto interpolation_at = partial.find(R"(,"interpolation":"continuous")");
    REQUIRE(interpolation_at != std::string::npos);
    partial.erase(interpolation_at, std::string_view(R"(,"interpolation":"continuous")").size());
    REQUIRE_FALSE(deserialize_project(partial, registry));
}

TEST_CASE("a pre-dynamics sequence document loads as a sequence with no dynamics",
          "[timeline][dynamics-lane][migration]") {
    const auto registry = builtins();
    const auto current =
        take(serialize_project(project_with_dynamics(lane_of({})), registry)).json;
    // A v7 document is the current shape minus the lane member, so strip
    // exactly that and stamp the version it was written at.
    auto legacy = current;
    constexpr std::string_view empty_member = R"("dynamics_lane":[],)";
    const auto member_at = legacy.find(empty_member);
    REQUIRE(member_at != std::string::npos);
    legacy.erase(member_at, empty_member.size());
    constexpr std::string_view stamp = R"("type_name":"pulp.timeline.sequence","version":8)";
    const auto stamp_at = legacy.find(stamp);
    REQUIRE(stamp_at != std::string::npos);
    legacy.replace(stamp_at, stamp.size(),
                   R"("type_name":"pulp.timeline.sequence","version":7)");

    const auto decoded = take(deserialize_project(legacy, registry));
    REQUIRE(decoded.find_sequence({2})->dynamics_lane().empty());
    // Re-saving lands on the current version with the lane materialized empty.
    REQUIRE(take(serialize_project(decoded, registry)).json == current);

    // A v7 document that carries the lane is a contradiction rather than a
    // hint, and a v8 document that omits it is the same contradiction the
    // other way round. Both the structural preflight and the decoder refuse.
    auto declared_too_early = current;
    const auto early_at = declared_too_early.find(stamp);
    REQUIRE(early_at != std::string::npos);
    declared_too_early.replace(early_at, stamp.size(),
                               R"("type_name":"pulp.timeline.sequence","version":7)");
    auto rejected_early = deserialize_project(declared_too_early, registry);
    REQUIRE_FALSE(rejected_early);
    REQUIRE(rejected_early.error().code == PersistenceErrorCode::InvalidSchema);

    auto omitted = current;
    const auto omitted_at = omitted.find(empty_member);
    REQUIRE(omitted_at != std::string::npos);
    omitted.erase(omitted_at, empty_member.size());
    auto rejected_omitted = deserialize_project(omitted, registry);
    REQUIRE_FALSE(rejected_omitted);
    REQUIRE(rejected_omitted.error().code == PersistenceErrorCode::InvalidSchema);
}

TEST_CASE("sequence v7 upgrades to an empty dynamics lane and downgrades only when empty",
          "[timeline][dynamics-lane][migration]") {
    const auto registry = builtins();
    DecodeLimits limits;
    const std::string v7 =
        R"({"data":{"absolute_duration":null,"chord_scale_lane":[],"groove":{"name":"","step":"0","steps":[],"swing_denominator":"2","swing_grid":"0","swing_numerator":"1","timing_strength":1000,"velocity_strength":1000},"id":"2","markers":[],"musical_duration":"100","name":"sequence","regions":[],"scenes":[],"track_order":[],"tracks":[]},"type_name":"pulp.timeline.sequence","version":7})";
    const auto v8 = take(
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 7, 8, v7, limits));
    REQUIRE(
        v8 ==
        R"({"data":{"absolute_duration":null,"chord_scale_lane":[],"dynamics_lane":[],"groove":{"name":"","step":"0","steps":[],"swing_denominator":"2","swing_grid":"0","swing_numerator":"1","timing_strength":1000,"velocity_strength":1000},"id":"2","markers":[],"musical_duration":"100","name":"sequence","regions":[],"scenes":[],"track_order":[],"tracks":[]},"type_name":"pulp.timeline.sequence","version":8})");
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 8, 7, v8,
                                  limits)) == v7);

    // The whole chain composes, so a v1 document reaches v8 and back unchanged.
    const std::string v1 =
        R"({"data":{"absolute_duration":null,"id":"2","musical_duration":"100","name":"sequence","tracks":[]},"type_name":"pulp.timeline.sequence","version":1})";
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 1, 8, v1,
                                  limits)) == v8);
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 8, 1, v8,
                                  limits)) == v1);

    // The splice trusts canonical member order, so a reordered payload is
    // refused rather than spliced into the wrong slot.
    const std::string reordered_v7 =
        R"({"data":{"chord_scale_lane":[],"absolute_duration":null,"groove":{"name":"","step":"0","steps":[],"swing_denominator":"2","swing_grid":"0","swing_numerator":"1","timing_strength":1000,"velocity_strength":1000},"id":"2","markers":[],"musical_duration":"100","name":"sequence","regions":[],"scenes":[],"track_order":[],"tracks":[]},"type_name":"pulp.timeline.sequence","version":7})";
    REQUIRE_FALSE(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 7, 8,
                                   reordered_v7, limits));
}

TEST_CASE("downgrading a sequence with authored dynamics is refused",
          "[timeline][dynamics-lane][migration]") {
    const auto registry = builtins();
    DecodeLimits limits;

    // An empty lane is exactly what a v7 reader already understood the document
    // to say, so it drops cleanly and the upgrade puts back what the downgrade
    // removed.
    const auto silent = sequence_envelope(
        take(serialize_project(project_with_dynamics(lane_of({})), registry)).json);
    const auto lowered = take(
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 8, 7, silent, limits));
    REQUIRE(lowered.find(R"("dynamics_lane")") == std::string::npos);
    REQUIRE(lowered.find(R"("type_name":"pulp.timeline.sequence","version":7)") !=
            std::string::npos);
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 7, 8, lowered,
                                  limits)) == silent);

    // One authored event is a performance instruction with no v7 spelling. A
    // v7 reader has nowhere to put it, so writing the document without it
    // would change how the sequence performs while reporting success. This
    // refuses even for a single, inaudible-looking event: the doctrine is that
    // authored data is never dropped quietly, not that loud data is not.
    const auto whisper = sequence_envelope(
        take(serialize_project(project_with_dynamics(lane_of({DynamicsEvent{{0}, 0.5f}})),
                               registry))
            .json);
    auto refused =
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 8, 7, whisper, limits);
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error().code == PersistenceErrorCode::MigrationFailed);

    // The refusal holds across the whole chain: a document that cannot reach
    // v7 cannot reach anything below it either.
    const auto authored = sequence_envelope(
        take(serialize_project(project_with_dynamics(ramp_then_hold()), registry)).json);
    auto refused_chain =
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 8, 1, authored, limits);
    REQUIRE_FALSE(refused_chain);
    REQUIRE(refused_chain.error().code == PersistenceErrorCode::MigrationFailed);
}
