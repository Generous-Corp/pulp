#include "support/timeline_persistence_test_support.hpp"

#include <pulp/timeline/compile_context.hpp>
#include <pulp/timeline/transaction.hpp>

namespace {

constexpr std::int64_t kEighth = kTicksPerQuarter / 2;
constexpr std::int64_t kSixteenth = kTicksPerQuarter / 4;

GrooveTemplate groove_of(GrooveTemplateInput input) {
    return take(GrooveTemplate::create(std::move(input)));
}

GrooveTemplate straight_groove() {
    return groove_of({});
}

// A groove that states both halves of a feel: a triplet shuffle on eighths, and
// a four-step sixteenth table that pushes the second step late and the third
// early while accenting them in opposite directions. Both strengths are
// attenuated so the round trip has to preserve them rather than defaulting.
GrooveTemplateInput shuffle_input() {
    GrooveTemplateInput input;
    input.name = "shuffle";
    input.swing_grid = TickDuration{kEighth};
    input.swing = SwingRatio{2, 3};
    input.step = TickDuration{kSixteenth};
    input.steps = {
        GrooveStep{TickDuration{0}, 1000},
        GrooveStep{TickDuration{1000}, 1200},
        GrooveStep{TickDuration{-1000}, 800},
        GrooveStep{TickDuration{0}, 1000},
    };
    input.timing_strength = 750;
    input.velocity_strength = 500;
    return input;
}

constexpr std::string_view kShuffleJson =
    R"("groove":{"name":"shuffle","step":"176400","steps":[{"timing_offset":"0","velocity_scale":1000},{"timing_offset":"1000","velocity_scale":1200},{"timing_offset":"-1000","velocity_scale":800},{"timing_offset":"0","velocity_scale":1000}],"swing_denominator":"3","swing_grid":"352800","swing_numerator":"2","timing_strength":750,"velocity_strength":500})";

// Ids: project 1, sequence 2, track 3, clip 4.
Project project_with_groove(GrooveTemplate groove) {
    auto clip = take(Clip::create({4}, {0}, {100}, EmptyContent{}));
    auto track = take(Track::create({3}, "track", {clip}));
    auto sequence = take(Sequence::create({2}, "sequence", TickDuration{100}, std::nullopt, {track},
                                          take(ChordScaleLane::create({})), std::move(groove)));
    return take(Project::create(ProjectInput{{1}, "project", 5, {2}, {}, {sequence}}));
}

} // namespace

TEST_CASE("a groove that states no feel is the identity on timing and velocity",
          "[timeline][groove]") {
    const auto groove = straight_groove();
    REQUIRE(groove.states_no_feel());
    REQUIRE(groove.is_canonical_default());
    REQUIRE(groove.name().empty());
    REQUIRE(groove.steps().empty());
    REQUIRE(groove.swing_grid() == TickDuration{0});
    REQUIRE(groove.swing() == kStraightSwing);

    for (const std::int64_t tick : {-kTicksPerQuarter, std::int64_t{-1'000}, std::int64_t{-1},
                                    std::int64_t{0}, std::int64_t{1}, std::int64_t{1'000},
                                    kTicksPerQuarter, std::int64_t{123'456'789}}) {
        REQUIRE(groove.apply_timing({tick}) == TickPosition{tick});
        REQUIRE(groove.velocity_scale_at({tick}) == kGrooveUnitScale);
    }
}

TEST_CASE("swing and the step table displace an authored position independently",
          "[timeline][groove]") {
    GrooveTemplateInput swing_only;
    swing_only.swing_grid = TickDuration{kEighth};
    swing_only.swing = kTripletSwing;
    const auto swung = groove_of(swing_only);
    REQUIRE_FALSE(swung.states_no_feel());

    // The pair boundary stays put and the off-beat lands on the last triplet.
    REQUIRE(swung.apply_timing({0}) == TickPosition{0});
    REQUIRE(swung.apply_timing({kEighth}) == TickPosition{(2 * kEighth * 2) / 3});
    REQUIRE(swung.apply_timing({2 * kEighth}) == TickPosition{2 * kEighth});
    // Swing alone never touches velocity: a feel's two halves are separate.
    REQUIRE(swung.velocity_scale_at({kEighth}) == kGrooveUnitScale);

    GrooveTemplateInput table_only;
    table_only.step = TickDuration{kSixteenth};
    table_only.steps = {GrooveStep{TickDuration{0}, 1000}, GrooveStep{TickDuration{500}, 1200}};
    const auto table = groove_of(table_only);
    REQUIRE(table.apply_timing({0}) == TickPosition{0});
    REQUIRE(table.apply_timing({kSixteenth}) == TickPosition{kSixteenth + 500});
    // The table repeats, and it repeats backwards too rather than clamping at
    // step zero: a position before the origin is still inside some step.
    REQUIRE(table.apply_timing({2 * kSixteenth}) == TickPosition{2 * kSixteenth});
    REQUIRE(table.apply_timing({3 * kSixteenth}) == TickPosition{3 * kSixteenth + 500});
    REQUIRE(table.apply_timing({-kSixteenth}) == TickPosition{-kSixteenth + 500});
    REQUIRE(table.apply_timing({-2 * kSixteenth}) == TickPosition{-2 * kSixteenth});
    REQUIRE(table.velocity_scale_at({kSixteenth}) == 1200);
    REQUIRE(table.velocity_scale_at({-kSixteenth}) == 1200);

    // Together: the step a note belongs to is decided by where it was written,
    // not by where swing moved it, so the two displacements compose without
    // either one re-assigning the other's input.
    GrooveTemplateInput both = table_only;
    both.swing_grid = TickDuration{kEighth};
    both.swing = kTripletSwing;
    const auto combined = groove_of(both);

    // 300000 is chosen because swing carries it across a step boundary: it is
    // authored in step 1 but lands in step 0. Reading the table at the swung
    // position would therefore apply a different step's offset, so this is the
    // assertion that distinguishes the two orders rather than agreeing with
    // both. (176400 does not: it stays in step 1 either way.)
    constexpr std::int64_t kCrossesAStep = 300'000;
    REQUIRE((kCrossesAStep / kSixteenth) % 2 == 1);
    REQUIRE((swung.apply_timing({kCrossesAStep}).value / kSixteenth) % 2 == 0);
    REQUIRE(combined.apply_timing({kCrossesAStep}) ==
            TickPosition{swung.apply_timing({kCrossesAStep}).value + 500});
    REQUIRE(combined.velocity_scale_at({kCrossesAStep}) == 1200);
}

TEST_CASE("groove strength attenuates timing and accent symmetrically", "[timeline][groove]") {
    GrooveTemplateInput input;
    input.step = TickDuration{kSixteenth};
    input.steps = {GrooveStep{TickDuration{1000}, 1500}, GrooveStep{TickDuration{-1000}, 500}};
    input.timing_strength = 500;
    input.velocity_strength = 500;
    const auto half = groove_of(input);

    // A late step and an early step of the same size are pulled in by the same
    // amount; truncation toward zero would bias the groove flat.
    REQUIRE(half.apply_timing({0}) == TickPosition{500});
    REQUIRE(half.apply_timing({kSixteenth}) == TickPosition{kSixteenth - 500});
    REQUIRE(half.velocity_scale_at({0}) == 1250);
    REQUIRE(half.velocity_scale_at({kSixteenth}) == 750);

    // Rounding is away from zero on both signs, so a one-tick offset survives
    // attenuation as one tick rather than collapsing to none on one side only.
    input.steps = {GrooveStep{TickDuration{1}, 1001}, GrooveStep{TickDuration{-1}, 999}};
    const auto tiny = groove_of(input);
    REQUIRE(tiny.apply_timing({0}) == TickPosition{1});
    REQUIRE(tiny.apply_timing({kSixteenth}) == TickPosition{kSixteenth - 1});
    REQUIRE(tiny.velocity_scale_at({0}) == 1001);
    REQUIRE(tiny.velocity_scale_at({kSixteenth}) == 999);

    // Zero strength is the identity while the authored table is preserved.
    input.swing_grid = TickDuration{kEighth};
    input.swing = kTripletSwing;
    input.timing_strength = 0;
    input.velocity_strength = 0;
    const auto silent = groove_of(input);
    REQUIRE_FALSE(silent.states_no_feel());
    REQUIRE_FALSE(silent.is_canonical_default());
    REQUIRE(silent.apply_timing({0}) == TickPosition{0});
    REQUIRE(silent.apply_timing({kEighth}) == TickPosition{kEighth});
    REQUIRE(silent.velocity_scale_at({0}) == kGrooveUnitScale);
    REQUIRE(silent.steps().size() == 2);
}

TEST_CASE("groove combines opposing timing deltas before signed-domain saturation",
          "[timeline][groove]") {
    GrooveTemplateInput input;
    input.swing_grid = TickDuration{5};
    input.swing = kTripletSwing;
    input.step = TickDuration{2};
    input.steps = {GrooveStep{TickDuration{-1}, kGrooveUnitScale}};
    const auto cancelling = groove_of(input);
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();

    REQUIRE(swing_displacement({maximum}, input.swing_grid, input.swing) == TickDuration{1});
    REQUIRE(cancelling.apply_timing({maximum}) == TickPosition{maximum});
}

TEST_CASE("a groove template refuses ranges it cannot mean", "[timeline][groove]") {
    auto refused = [](GrooveTemplateInput input) {
        auto result = GrooveTemplate::create(std::move(input));
        REQUIRE_FALSE(result);
        REQUIRE(result.error().code == ModelErrorCode::InvalidGrooveTemplate);
    };

    // A step width and a table imply each other.
    GrooveTemplateInput width_without_table;
    width_without_table.step = TickDuration{kSixteenth};
    refused(width_without_table);
    GrooveTemplateInput table_without_width;
    table_without_width.steps = {GrooveStep{}};
    refused(table_without_width);

    // An offset of a whole step or more pushes material past the step beyond
    // its neighbour, which is a table written wrong rather than a strong feel.
    GrooveTemplateInput overrun;
    overrun.step = TickDuration{100};
    overrun.steps = {GrooveStep{TickDuration{100}, 1000}};
    refused(overrun);
    overrun.steps = {GrooveStep{TickDuration{-100}, 1000}};
    refused(overrun);
    // One tick inside the step is accepted, so the bound is exclusive and not
    // an off-by-one rejection of legitimate tables.
    overrun.steps = {GrooveStep{TickDuration{99}, 1000}};
    REQUIRE(GrooveTemplate::create(overrun));
    overrun.steps = {GrooveStep{TickDuration{-99}, 1000}};
    REQUIRE(GrooveTemplate::create(overrun));

    GrooveTemplateInput loud;
    loud.step = TickDuration{100};
    loud.steps = {GrooveStep{TickDuration{0}, kMaxGrooveVelocityScale + 1}};
    refused(loud);
    loud.steps = {GrooveStep{TickDuration{0}, -1}};
    refused(loud);

    GrooveTemplateInput strong;
    strong.timing_strength = kGrooveUnitScale + 1;
    refused(strong);
    strong = {};
    strong.velocity_strength = -1;
    refused(strong);

    // A ratio of zero or one collapses half of every pair onto one instant.
    GrooveTemplateInput degenerate;
    degenerate.swing_grid = TickDuration{kEighth};
    degenerate.swing = SwingRatio{0, 2};
    refused(degenerate);
    degenerate.swing = SwingRatio{2, 2};
    refused(degenerate);
    degenerate.swing = SwingRatio{1, 0};
    refused(degenerate);

    GrooveTemplateInput vast;
    vast.swing_grid = TickDuration{kMaxSwingGridTicks + 1};
    refused(vast);
    vast.swing_grid = TickDuration{-1};
    refused(vast);

    GrooveTemplateInput crowded;
    crowded.step = TickDuration{100};
    crowded.steps.assign(kMaxGrooveSteps + 1, GrooveStep{TickDuration{0}, 1000});
    refused(crowded);
}

TEST_CASE("a context view reads a groove only when its owner declared it",
          "[timeline][groove][subscription]") {
    const auto project = project_with_groove(groove_of(shuffle_input()));

    auto declared = CompileContextSubscriptions::none();
    declared.subscribe(CompileContextKind::Groove);
    const CompileContextView subscriber(project, {2}, declared);
    REQUIRE(subscriber.groove() != nullptr);
    REQUIRE(subscriber.groove()->name() == "shuffle");
    // Declaring one kind does not grant the other.
    REQUIRE(subscriber.chord_scale_lane() == nullptr);

    auto harmony_only = CompileContextSubscriptions::none();
    harmony_only.subscribe(CompileContextKind::ChordScale);
    const CompileContextView other_kind(project, {2}, harmony_only);
    REQUIRE(other_kind.groove() == nullptr);
    REQUIRE(other_kind.chord_scale_lane() != nullptr);

    const CompileContextView undeclared(project, {2}, CompileContextSubscriptions::none());
    REQUIRE(undeclared.groove() == nullptr);

    // An unknown sequence reads as absent rather than as a straight groove.
    const CompileContextView missing(project, {99}, declared);
    REQUIRE(missing.groove() == nullptr);

    // A sequence that plays straight still answers: "no feel" is a statement,
    // not an absence, so a hook never has to guess what silence meant.
    const auto plain = project_with_groove(straight_groove());
    const CompileContextView plain_view(plain, {2}, declared);
    REQUIRE(plain_view.groove() != nullptr);
    REQUIRE(plain_view.groove()->states_no_feel());
}

TEST_CASE("setting the groove dirties the groove context kind and nothing else",
          "[timeline][groove][dirty-set]") {
    const auto project = project_with_groove(straight_groove());
    const auto replacement = groove_of(shuffle_input());

    Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.commands.push_back({{{1}, 1}, SetGroove{{2}, straight_groove(), replacement}});
    auto reduced = take(reduce_transaction(project, transaction));

    REQUIRE(reduced.project.find_sequence({2})->groove().name() == "shuffle");
    REQUIRE(reduced.dirty.contexts().size() == 1);
    REQUIRE(reduced.dirty.contexts()[0].owner_sequence == ItemId{2});
    REQUIRE(reduced.dirty.contexts()[0].kind == CompileContextKind::Groove);

    // The companion item names the sequence and no track: a groove's readers
    // are not its children, so it cannot name them.
    REQUIRE(reduced.dirty.items().size() == 1);
    REQUIRE(reduced.dirty.items()[0].owner_sequence == ItemId{2});
    REQUIRE_FALSE(reduced.dirty.items()[0].owner_track.valid());
    REQUIRE(reduced.dirty.items()[0].flags == DirtyFlags::Context);

    REQUIRE(reduced.inverses.size() == 1);
    Transaction undo;
    undo.id = {{1}, 2};
    undo.commands.push_back({{{1}, 2}, reduced.inverses[0]});
    auto restored = take(reduce_transaction(reduced.project, undo));
    REQUIRE(restored.project.find_sequence({2})->groove().states_no_feel());

    // The optimistic gate refuses a stale expected value rather than clobbering.
    Transaction stale;
    stale.id = {{1}, 3};
    stale.commands.push_back({{{1}, 3}, SetGroove{{2}, straight_groove(), replacement}});
    auto rejected = reduce_transaction(reduced.project, stale);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::ExpectedValueMismatch);

    Transaction missing;
    missing.id = {{1}, 4};
    missing.commands.push_back({{{1}, 4}, SetGroove{{99}, straight_groove(), replacement}});
    REQUIRE_FALSE(reduce_transaction(project, missing));
}

TEST_CASE("a groove round trips and re-saves byte-identically",
          "[timeline][groove][persistence]") {
    const auto registry = builtins();
    const auto original = project_with_groove(groove_of(shuffle_input()));

    auto first = serialize_project(original, registry);
    REQUIRE(first.has_value());
    REQUIRE(first.value().json.find("\"type_name\":\"pulp.timeline.sequence\",\"version\":3") !=
            std::string::npos);
    REQUIRE(first.value().json.find(kShuffleJson) != std::string::npos);

    const auto decoded = take(deserialize_project(first.value().json, registry));
    const auto& groove = decoded.find_sequence({2})->groove();
    REQUIRE(groove.name() == "shuffle");
    REQUIRE(groove.swing_grid() == TickDuration{kEighth});
    REQUIRE(groove.swing() == SwingRatio{2, 3});
    REQUIRE(groove.step() == TickDuration{kSixteenth});
    REQUIRE(groove.timing_strength() == 750);
    REQUIRE(groove.velocity_strength() == 500);
    REQUIRE(groove.steps().size() == 4);
    REQUIRE(groove.steps()[1] == GrooveStep{TickDuration{1000}, 1200});
    REQUIRE(groove.steps()[2] == GrooveStep{TickDuration{-1000}, 800});
    REQUIRE(groove == groove_of(shuffle_input()));

    auto second = serialize_project(decoded, registry);
    REQUIRE(second.has_value());
    REQUIRE(second.value().json == first.value().json);
}

TEST_CASE("a document whose groove is malformed is rejected on load",
          "[timeline][groove][persistence]") {
    const auto registry = builtins();
    const auto json = take(serialize_project(project_with_groove(groove_of(shuffle_input())),
                                             registry))
                          .json;
    auto mutated = [&](std::string_view from, std::string_view to) {
        auto copy = json;
        const auto at = copy.find(from);
        REQUIRE(at != std::string::npos);
        copy.replace(at, from.size(), to);
        return copy;
    };

    // An offset at least a whole step wide is a model rejection, not a clamp.
    REQUIRE_FALSE(deserialize_project(mutated(R"("timing_offset":"1000")",
                                              R"("timing_offset":"176400")"),
                                      registry));
    // A ratio outside the open unit interval would collapse half of every pair.
    REQUIRE_FALSE(
        deserialize_project(mutated(R"("swing_numerator":"2")", R"("swing_numerator":"3")"),
                            registry));
    // A strength above unity would amplify rather than attenuate. The model
    // owns that range; what matters here is that a document carrying it is
    // refused rather than clamped on the way in.
    REQUIRE_FALSE(deserialize_project(mutated(R"("timing_strength":750)",
                                              R"("timing_strength":1001)"),
                                      registry));
    REQUIRE_FALSE(deserialize_project(mutated(R"("velocity_scale":1200)",
                                              R"("velocity_scale":4001)"),
                                      registry));
    // Values the 32-bit field cannot hold are refused at the parse and at the
    // narrowing guard respectively, so neither reaches the model wearing a
    // different value than the document wrote.
    REQUIRE_FALSE(deserialize_project(mutated(R"("velocity_strength":500)",
                                              R"("velocity_strength":4294967296)"),
                                      registry));
    REQUIRE_FALSE(deserialize_project(mutated(R"("velocity_strength":500)",
                                              R"("velocity_strength":2147483648)"),
                                      registry));
    // A missing member is a refusal, not a defaulted field.
    REQUIRE_FALSE(deserialize_project(mutated(R"("timing_strength":750,)", ""), registry));
}

TEST_CASE("sequence v2 upgrades to a straight groove and downgrades only when straight",
          "[timeline][groove][migration]") {
    const auto registry = builtins();
    const std::string v2 =
        R"({"data":{"absolute_duration":null,"chord_scale_lane":[],"id":"2","musical_duration":"100","name":"sequence","tracks":[]},"type_name":"pulp.timeline.sequence","version":2})";
    const auto v3 =
        take(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 2, 3, v2));
    REQUIRE(v3 ==
            R"({"data":{"absolute_duration":null,"chord_scale_lane":[],"groove":{"name":"","step":"0","steps":[],"swing_denominator":"2","swing_grid":"0","swing_numerator":"1","timing_strength":1000,"velocity_strength":1000},"id":"2","musical_duration":"100","name":"sequence","tracks":[]},"type_name":"pulp.timeline.sequence","version":3})");
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 3, 2, v3)) ==
            v2);

    // The migration is textual, so it must survive a non-canonical member order
    // in both directions.
    const std::string reordered_v2 =
        R"({"version":2,"type_name":"pulp.timeline.sequence","data":{"chord_scale_lane":[],"absolute_duration":null,"id":"2","musical_duration":"100","name":"sequence","tracks":[]}})";
    const auto reordered_v3 =
        take(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 2, 3, reordered_v2));
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 3, 2,
                                  reordered_v3)) == reordered_v2);

    // The erase must pick the comma that actually separates the removed member,
    // including when the groove is written first or last.
    const std::string groove_first =
        R"({"data":{"groove":{"name":"","step":"0","steps":[],"swing_denominator":"2","swing_grid":"0","swing_numerator":"1","timing_strength":1000,"velocity_strength":1000},"absolute_duration":null,"chord_scale_lane":[],"id":"2","musical_duration":"100","name":"sequence","tracks":[]},"type_name":"pulp.timeline.sequence","version":3})";
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 3, 2,
                                  groove_first)) ==
            R"({"data":{"absolute_duration":null,"chord_scale_lane":[],"id":"2","musical_duration":"100","name":"sequence","tracks":[]},"type_name":"pulp.timeline.sequence","version":2})");
    const std::string groove_last =
        R"({"data":{"absolute_duration":null,"chord_scale_lane":[],"id":"2","musical_duration":"100","name":"sequence","tracks":[],"groove":{"name":"","step":"0","steps":[],"swing_denominator":"2","swing_grid":"0","swing_numerator":"1","timing_strength":1000,"velocity_strength":1000}},"type_name":"pulp.timeline.sequence","version":3})";
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 3, 2,
                                  groove_last)) ==
            R"({"data":{"absolute_duration":null,"chord_scale_lane":[],"id":"2","musical_duration":"100","name":"sequence","tracks":[]},"type_name":"pulp.timeline.sequence","version":2})");
}

TEST_CASE("a groove downgrade refuses every field it would have to drop",
          "[timeline][groove][migration]") {
    const auto registry = builtins();
    auto with_groove = [](std::string_view groove) {
        return std::string(R"({"data":{"absolute_duration":null,"chord_scale_lane":[],"groove":)") +
               std::string(groove) +
               R"(,"id":"2","musical_duration":"100","name":"sequence","tracks":[]},"type_name":"pulp.timeline.sequence","version":3})";
    };
    auto refuses = [&](std::string_view groove) {
        REQUIRE_FALSE(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 3, 2,
                                       with_groove(groove)));
    };

    // Each departure from the straight feel is a separate way to lose data, and
    // each one refuses on its own rather than only the audible ones.
    refuses(
        R"({"name":"shuffle","step":"0","steps":[],"swing_denominator":"2","swing_grid":"0","swing_numerator":"1","timing_strength":1000,"velocity_strength":1000})");
    refuses(
        R"({"name":"","step":"0","steps":[],"swing_denominator":"3","swing_grid":"352800","swing_numerator":"2","timing_strength":1000,"velocity_strength":1000})");
    refuses(
        R"({"name":"","step":"176400","steps":[{"timing_offset":"1000","velocity_scale":1000}],"swing_denominator":"2","swing_grid":"0","swing_numerator":"1","timing_strength":1000,"velocity_strength":1000})");
    refuses(
        R"({"name":"","step":"0","steps":[],"swing_denominator":"3","swing_grid":"0","swing_numerator":"1","timing_strength":1000,"velocity_strength":1000})");
    refuses(
        R"({"name":"","step":"0","steps":[],"swing_denominator":"2","swing_grid":"0","swing_numerator":"1","timing_strength":500,"velocity_strength":1000})");
    refuses(
        R"({"name":"","step":"0","steps":[],"swing_denominator":"2","swing_grid":"0","swing_numerator":"1","timing_strength":1000,"velocity_strength":0})");

    // The straight groove itself still downgrades, so the refusals above are
    // discriminating rather than a blanket rejection.
    REQUIRE(registry.migrate(
        SchemaDomain::Document, "pulp.timeline.sequence", 3, 2,
        with_groove(
            R"({"name":"","step":"0","steps":[],"swing_denominator":"2","swing_grid":"0","swing_numerator":"1","timing_strength":1000,"velocity_strength":1000})")));
}

TEST_CASE("a v2 sequence document loads as a sequence with no feel",
          "[timeline][groove][migration]") {
    const auto registry = builtins();
    const auto current = take(serialize_project(project_with_groove(straight_groove()), registry))
                             .json;
    auto legacy = current;
    constexpr std::string_view straight_member =
        R"("groove":{"name":"","step":"0","steps":[],"swing_denominator":"2","swing_grid":"0","swing_numerator":"1","timing_strength":1000,"velocity_strength":1000},)";
    const auto groove_at = legacy.find(straight_member);
    REQUIRE(groove_at != std::string::npos);
    legacy.erase(groove_at, straight_member.size());
    constexpr std::string_view current_version =
        R"("type_name":"pulp.timeline.sequence","version":3)";
    const auto version_at = legacy.find(current_version);
    REQUIRE(version_at != std::string::npos);
    legacy.replace(version_at, current_version.size(),
                   R"("type_name":"pulp.timeline.sequence","version":2)");

    const auto decoded = take(deserialize_project(legacy, registry));
    REQUIRE(decoded.find_sequence({2})->groove().states_no_feel());
    // Re-saving lands on the current version with the groove materialized.
    REQUIRE(take(serialize_project(decoded, registry)).json == current);
}

TEST_CASE("a set_groove command round trips through its schema envelope",
          "[timeline][groove][persistence]") {
    const auto registry = builtins();
    constexpr std::string_view straight =
        R"({"name":"","step":"0","steps":[],"swing_denominator":"2","swing_grid":"0","swing_numerator":"1","timing_strength":1000,"velocity_strength":1000})";
    const std::string encoded =
        std::string(R"({"data":{"expected":)") + std::string(straight) +
        R"(,"replacement":{"name":"push","step":"176400","steps":[{"timing_offset":"640","velocity_scale":1100}],"swing_denominator":"3","swing_grid":"352800","swing_numerator":"2","timing_strength":1000,"velocity_strength":1000},"sequence_id":"2"},"type_name":"pulp.timeline.command.set_groove","version":1})";
    auto decoded = deserialize_commands("[" + encoded + "]", registry);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded.value().size() == 1);
    const auto* command = std::get_if<SetGroove>(&decoded.value()[0]);
    REQUIRE(command != nullptr);
    REQUIRE(command->sequence_id == ItemId{2});
    REQUIRE(command->expected.states_no_feel());
    REQUIRE(command->replacement.name() == "push");
    REQUIRE(command->replacement.swing() == SwingRatio{2, 3});
    REQUIRE(command->replacement.steps().size() == 1);
    REQUIRE(command->replacement.steps()[0] == GrooveStep{TickDuration{640}, 1100});

    // A command carrying a groove the model would refuse is refused at decode
    // rather than reaching reduce_transaction.
    const std::string invalid =
        std::string(R"({"data":{"expected":)") + std::string(straight) +
        R"(,"replacement":{"name":"","step":"100","steps":[{"timing_offset":"100","velocity_scale":1000}],"swing_denominator":"2","swing_grid":"0","swing_numerator":"1","timing_strength":1000,"velocity_strength":1000},"sequence_id":"2"},"type_name":"pulp.timeline.command.set_groove","version":1})";
    REQUIRE_FALSE(deserialize_commands("[" + invalid + "]", registry));
}
