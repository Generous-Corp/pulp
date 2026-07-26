#include "support/timeline_persistence_test_support.hpp"

#include <pulp/timeline/compile_context.hpp>
#include <pulp/timeline/transaction.hpp>

namespace {

ChordScaleLane lane_of(std::vector<ChordScaleEvent> events) {
    return take(ChordScaleLane::create(std::move(events)));
}

// Ids: project 1, sequence 2, track 3, clip 4. Two harmonic statements so the
// round trip must preserve order, both roots, and both enum spellings.
Project project_with_chord_lane(ChordScaleLane lane) {
    auto clip = take(Clip::create({4}, {0}, {100}, EmptyContent{}));
    auto track = take(Track::create({3}, "track", {clip}));
    auto sequence = take(Sequence::create({2}, "sequence", TickDuration{100}, std::nullopt, {track},
                                          {}, {}, std::move(lane)));
    return take(Project::create(ProjectInput{{1}, "project", 5, {2}, {}, {sequence}}));
}

ChordScaleLane two_bar_lane() {
    return lane_of({
        ChordScaleEvent{{0}, ChordQuality::Minor7, 9, ScaleMode::Dorian, 9},
        ChordScaleEvent{{1920}, ChordQuality::Dominant7, 2, ScaleMode::Mixolydian, 7},
    });
}

} // namespace

TEST_CASE("chord/scale lane resolves the harmony in force and rejects malformed events",
          "[timeline][context-lane]") {
    const auto lane = two_bar_lane();
    REQUIRE_FALSE(lane.empty());
    REQUIRE(lane.events().size() == 2);

    // Before the first statement the lane says nothing; it never invents a key.
    REQUIRE(lane.at({-1}) == nullptr);
    REQUIRE(lane.at({0})->chord_quality == ChordQuality::Minor7);
    REQUIRE(lane.at({1919})->chord_root == 9);
    REQUIRE(lane.at({1920})->chord_quality == ChordQuality::Dominant7);
    REQUIRE(lane.at({999'999})->scale_mode == ScaleMode::Mixolydian);
    REQUIRE(lane.at({999'999})->scale_root == 7);

    // Defensive cases: a pitch class outside an octave, a negative position,
    // and an out-of-order or duplicated position are all refusals, not repairs.
    REQUIRE_FALSE(ChordScaleLane::create({ChordScaleEvent{{0}, ChordQuality::Major, 12,
                                                          ScaleMode::Major, 0}}));
    REQUIRE_FALSE(ChordScaleLane::create({ChordScaleEvent{{0}, ChordQuality::Major, 0,
                                                          ScaleMode::Major, 12}}));
    REQUIRE_FALSE(ChordScaleLane::create({ChordScaleEvent{{-1}, ChordQuality::Major, 0,
                                                          ScaleMode::Major, 0}}));
    const auto unordered = ChordScaleLane::create({
        ChordScaleEvent{{480}, ChordQuality::Major, 0, ScaleMode::Major, 0},
        ChordScaleEvent{{0}, ChordQuality::Minor, 0, ScaleMode::Major, 0},
    });
    REQUIRE_FALSE(unordered);
    REQUIRE(unordered.error().code == ModelErrorCode::UnorderedChordScaleLane);
    REQUIRE_FALSE(ChordScaleLane::create({
        ChordScaleEvent{{0}, ChordQuality::Major, 0, ScaleMode::Major, 0},
        ChordScaleEvent{{0}, ChordQuality::Minor, 0, ScaleMode::Major, 0},
    }));
}

TEST_CASE("a context view reads only the kinds its owner declared",
          "[timeline][context-lane][subscription]") {
    const auto project = project_with_chord_lane(two_bar_lane());

    auto declared = CompileContextSubscriptions::none();
    declared.subscribe(CompileContextKind::ChordScale);
    REQUIRE(declared.any());
    REQUIRE(declared.reads(CompileContextKind::ChordScale));

    const CompileContextView subscriber(project, {2}, declared);
    REQUIRE(subscriber.chord_scale_lane() != nullptr);
    REQUIRE(subscriber.chord_scale_lane()->events().size() == 2);
    REQUIRE(subscriber.chord_scale_at({1920})->chord_root == 2);
    REQUIRE(subscriber.chord_scale_at({1920})->scale_root == 7);

    // The declaration is what grants the read. Undeclared context is absent,
    // so a compile hook cannot depend on context the compiler does not know to
    // invalidate it for.
    const CompileContextView undeclared(project, {2}, CompileContextSubscriptions::none());
    REQUIRE_FALSE(undeclared.subscriptions().any());
    REQUIRE(undeclared.chord_scale_lane() == nullptr);
    REQUIRE(undeclared.chord_scale_at({1920}) == nullptr);

    // An unknown sequence reads as absent rather than as an empty lane.
    const CompileContextView missing(project, {99}, declared);
    REQUIRE(missing.chord_scale_lane() == nullptr);
}

TEST_CASE("setting the chord/scale lane dirties the context kind and nothing else",
          "[timeline][context-lane][dirty-set]") {
    const auto project = project_with_chord_lane(lane_of({}));
    const auto replacement = two_bar_lane();

    Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.commands.push_back(
        {{{1}, 1}, SetChordScaleLane{{2}, lane_of({}), replacement}});
    auto reduced = take(reduce_transaction(project, transaction));

    REQUIRE(reduced.project.find_sequence({2})->chord_scale_lane().events().size() == 2);
    REQUIRE(reduced.dirty.contexts().size() == 1);
    REQUIRE(reduced.dirty.contexts()[0].owner_sequence == ItemId{2});
    REQUIRE(reduced.dirty.contexts()[0].kind == CompileContextKind::ChordScale);

    // The companion item names the sequence and no track: its readers are not
    // its children, so it cannot name them.
    REQUIRE(reduced.dirty.items().size() == 1);
    REQUIRE(reduced.dirty.items()[0].owner_sequence == ItemId{2});
    REQUIRE_FALSE(reduced.dirty.items()[0].owner_track.valid());
    REQUIRE(reduced.dirty.items()[0].flags == DirtyFlags::Context);

    // The inverse restores the previous lane exactly.
    REQUIRE(reduced.inverses.size() == 1);
    Transaction undo;
    undo.id = {{1}, 2};
    undo.commands.push_back({{{1}, 2}, reduced.inverses[0]});
    auto restored = take(reduce_transaction(reduced.project, undo));
    REQUIRE(restored.project.find_sequence({2})->chord_scale_lane().empty());

    // The optimistic gate refuses a stale expected value rather than clobbering.
    Transaction stale;
    stale.id = {{1}, 3};
    stale.commands.push_back({{{1}, 3}, SetChordScaleLane{{2}, lane_of({}), replacement}});
    auto rejected = reduce_transaction(reduced.project, stale);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::ExpectedValueMismatch);

    // A command naming a sequence that does not exist is refused, not applied
    // to whatever sequence happens to be first.
    Transaction missing;
    missing.id = {{1}, 4};
    missing.commands.push_back({{{1}, 4}, SetChordScaleLane{{99}, lane_of({}), replacement}});
    REQUIRE_FALSE(reduce_transaction(project, missing));
}

TEST_CASE("chord/scale lane round trips and re-saves byte-identically",
          "[timeline][context-lane][persistence]") {
    const auto registry = builtins();
    const auto original = project_with_chord_lane(two_bar_lane());

    auto first = serialize_project(original, registry);
    REQUIRE(first.has_value());
    REQUIRE(first.value().json.find("\"type_name\":\"pulp.timeline.sequence\",\"version\":4") !=
            std::string::npos);
    REQUIRE(first.value().json.find(
                R"("chord_scale_lane":[{"chord_quality":"minor7","chord_root":9,"position":"0","scale_mode":"dorian","scale_root":9})") !=
            std::string::npos);

    const auto decoded = take(deserialize_project(first.value().json, registry));
    const auto& lane = decoded.find_sequence({2})->chord_scale_lane();
    REQUIRE(lane.events().size() == 2);
    REQUIRE(lane.events()[0] ==
            ChordScaleEvent{{0}, ChordQuality::Minor7, 9, ScaleMode::Dorian, 9});
    REQUIRE(lane.events()[1] ==
            ChordScaleEvent{{1920}, ChordQuality::Dominant7, 2, ScaleMode::Mixolydian, 7});

    auto second = serialize_project(decoded, registry);
    REQUIRE(second.has_value());
    REQUIRE(second.value().json == first.value().json);
}

TEST_CASE("every chord quality and scale mode survives the wire spelling",
          "[timeline][context-lane][persistence]") {
    const auto registry = builtins();
    std::vector<ChordScaleEvent> events;
    constexpr std::array qualities{ChordQuality::Major,           ChordQuality::Minor,
                                   ChordQuality::Diminished,     ChordQuality::Augmented,
                                   ChordQuality::Dominant7,      ChordQuality::Major7,
                                   ChordQuality::Minor7,         ChordQuality::HalfDiminished7,
                                   ChordQuality::Suspended2,     ChordQuality::Suspended4};
    constexpr std::array modes{ScaleMode::Major,         ScaleMode::NaturalMinor,
                               ScaleMode::HarmonicMinor, ScaleMode::MelodicMinor,
                               ScaleMode::Dorian,        ScaleMode::Phrygian,
                               ScaleMode::Lydian,        ScaleMode::Mixolydian,
                               ScaleMode::Locrian,       ScaleMode::Chromatic};
    static_assert(qualities.size() == modes.size());
    for (std::size_t index = 0; index < qualities.size(); ++index)
        events.push_back(ChordScaleEvent{{static_cast<std::int64_t>(index)}, qualities[index],
                                         static_cast<std::uint8_t>(index % 12), modes[index],
                                         static_cast<std::uint8_t>((index + 5) % 12)});

    const auto original = project_with_chord_lane(lane_of(events));
    const auto json = take(serialize_project(original, registry)).json;
    const auto decoded = take(deserialize_project(json, registry));
    const auto& lane = decoded.find_sequence({2})->chord_scale_lane();
    REQUIRE(lane.events().size() == events.size());
    for (std::size_t index = 0; index < events.size(); ++index)
        REQUIRE(lane.events()[index] == events[index]);
}

TEST_CASE("a document whose chord lane is malformed is rejected on load",
          "[timeline][context-lane][persistence]") {
    const auto registry = builtins();
    const auto json = take(serialize_project(project_with_chord_lane(two_bar_lane()), registry)).json;

    // An unknown quality name is not silently coerced to a default chord.
    auto unknown_quality = json;
    const auto quality_at = unknown_quality.find("\"minor7\"");
    REQUIRE(quality_at != std::string::npos);
    unknown_quality.replace(quality_at, 8, "\"minor8\"");
    REQUIRE_FALSE(deserialize_project(unknown_quality, registry));

    // A pitch class outside an octave is a model rejection, not a modulo.
    auto bad_root = json;
    const auto root_at = bad_root.find("\"chord_root\":9");
    REQUIRE(root_at != std::string::npos);
    bad_root.replace(root_at, 14, "\"chord_root\":99");
    REQUIRE_FALSE(deserialize_project(bad_root, registry));

    // 256 truncates to 0 in the model's byte-wide pitch class, so narrowing
    // must not be what turns an out-of-range root into a valid C.
    auto wrapping_root = json;
    const auto wrap_at = wrapping_root.find("\"scale_root\":7");
    REQUIRE(wrap_at != std::string::npos);
    wrapping_root.replace(wrap_at, 14, "\"scale_root\":256");
    REQUIRE_FALSE(deserialize_project(wrapping_root, registry));

    // Descending positions would change which harmony is in force where, so
    // the loader refuses rather than sorting the document into a new tune.
    auto descending = json;
    const auto position_at = descending.find("\"position\":\"1920\"");
    REQUIRE(position_at != std::string::npos);
    descending.replace(position_at, 17, "\"position\":\"-960\"");
    REQUIRE_FALSE(deserialize_project(descending, registry));
}

TEST_CASE("sequence v2 upgrades to an empty chord lane and downgrades only when empty",
          "[timeline][context-lane][migration]") {
    const auto registry = builtins();
    // v2 is the annotations version; the chord lane arrives one step later, so
    // the chain a v1 document walks is 1 -> 2 -> 3.
    const std::string v2 =
        R"({"data":{"absolute_duration":null,"id":"2","markers":[],"musical_duration":"100","name":"sequence","regions":[],"tracks":[]},"type_name":"pulp.timeline.sequence","version":2})";
    const auto v3 =
        take(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 2, 3, v2));
    REQUIRE(v3 ==
            R"({"data":{"absolute_duration":null,"chord_scale_lane":[],"id":"2","markers":[],"musical_duration":"100","name":"sequence","regions":[],"tracks":[]},"type_name":"pulp.timeline.sequence","version":3})");
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 3, 2, v3)) ==
            v2);

    // The whole chain composes, so a v1 document reaches v3 and back unchanged.
    const std::string v1 =
        R"({"data":{"absolute_duration":null,"id":"2","musical_duration":"100","name":"sequence","tracks":[]},"type_name":"pulp.timeline.sequence","version":1})";
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 1, 3, v1)) ==
            v3);
    REQUIRE(take(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 3, 1, v3)) ==
            v1);

    // The splice trusts canonical member order, so a reordered payload is
    // refused rather than spliced into the wrong slot.
    const std::string reordered_v2 =
        R"({"data":{"id":"2","absolute_duration":null,"markers":[],"musical_duration":"100","name":"sequence","regions":[],"tracks":[]},"type_name":"pulp.timeline.sequence","version":2})";
    REQUIRE_FALSE(
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 2, 3, reordered_v2));

    // A v2 reader has nowhere to put authored harmony. Dropping it would change
    // what the document sounds like while reporting success, so this refuses.
    const std::string populated =
        R"({"data":{"absolute_duration":null,"chord_scale_lane":[{"chord_quality":"minor7","chord_root":9,"position":"0","scale_mode":"dorian","scale_root":9}],"id":"2","markers":[],"musical_duration":"100","name":"sequence","regions":[],"tracks":[]},"type_name":"pulp.timeline.sequence","version":3})";
    REQUIRE_FALSE(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 3, 2,
                                   populated));
}

TEST_CASE("a pre-lane sequence document loads as a sequence with no harmony",
          "[timeline][context-lane][migration]") {
    const auto registry = builtins();
    const auto current = take(serialize_project(project_with_chord_lane(lane_of({})), registry)).json;
    auto legacy = current;
    const auto lane_at = legacy.find(R"("chord_scale_lane":[],)");
    REQUIRE(lane_at != std::string::npos);
    legacy.erase(lane_at, std::string_view(R"("chord_scale_lane":[],)").size());
    constexpr std::string_view straight_groove =
        R"("groove":{"name":"","step":"0","steps":[],"swing_denominator":"2","swing_grid":"0","swing_numerator":"1","timing_strength":1000,"velocity_strength":1000},)";
    const auto groove_at = legacy.find(straight_groove);
    REQUIRE(groove_at != std::string::npos);
    legacy.erase(groove_at, straight_groove.size());
    const auto version_at = legacy.find(R"("type_name":"pulp.timeline.sequence","version":4)");
    REQUIRE(version_at != std::string::npos);
    legacy.replace(version_at, std::string_view(
                                   R"("type_name":"pulp.timeline.sequence","version":4)").size(),
                   R"("type_name":"pulp.timeline.sequence","version":2)");

    const auto decoded = take(deserialize_project(legacy, registry));
    REQUIRE(decoded.find_sequence({2})->chord_scale_lane().empty());
    REQUIRE(decoded.find_sequence({2})->groove().is_canonical_default());
    // Re-saving lands on the current version with both later context fields
    // materialized at their canonical defaults.
    REQUIRE(take(serialize_project(decoded, registry)).json == current);
}

TEST_CASE("a set_chord_scale_lane command round trips through its schema envelope",
          "[timeline][context-lane][persistence]") {
    const auto registry = builtins();
    const std::string encoded =
        R"({"data":{"expected":[],"replacement":[{"chord_quality":"suspended4","chord_root":7,"position":"480","scale_mode":"lydian","scale_root":5}],"sequence_id":"2"},"type_name":"pulp.timeline.command.set_chord_scale_lane","version":1})";
    auto decoded = deserialize_commands("[" + encoded + "]", registry);
    REQUIRE(decoded.has_value());
    REQUIRE(decoded.value().size() == 1);
    const auto* command = std::get_if<SetChordScaleLane>(&decoded.value()[0]);
    REQUIRE(command != nullptr);
    REQUIRE(command->sequence_id == ItemId{2});
    REQUIRE(command->expected.empty());
    REQUIRE(command->replacement.events().size() == 1);
    REQUIRE(command->replacement.events()[0] ==
            ChordScaleEvent{{480}, ChordQuality::Suspended4, 7, ScaleMode::Lydian, 5});

    // A command carrying an unorderable lane is refused at decode.
    const std::string unordered =
        R"({"data":{"expected":[],"replacement":[{"chord_quality":"major","chord_root":0,"position":"480","scale_mode":"major","scale_root":0},{"chord_quality":"major","chord_root":0,"position":"0","scale_mode":"major","scale_root":0}],"sequence_id":"2"},"type_name":"pulp.timeline.command.set_chord_scale_lane","version":1})";
    REQUIRE_FALSE(deserialize_commands("[" + unordered + "]", registry));
}
