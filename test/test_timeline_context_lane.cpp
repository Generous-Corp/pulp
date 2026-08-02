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
    REQUIRE_FALSE(ChordScaleLane::create(
        {ChordScaleEvent{{0}, ChordQuality::Major, 12, ScaleMode::Major, 0}}));
    REQUIRE_FALSE(ChordScaleLane::create(
        {ChordScaleEvent{{0}, ChordQuality::Major, 0, ScaleMode::Major, 12}}));
    REQUIRE_FALSE(ChordScaleLane::create(
        {ChordScaleEvent{{-1}, ChordQuality::Major, 0, ScaleMode::Major, 0}}));
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
    transaction.commands.push_back({{{1}, 1}, SetChordScaleLane{{2}, lane_of({}), replacement}});
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
    REQUIRE(first.value().json.find("\"type_name\":\"pulp.timeline.sequence\",\"version\":7") !=
            std::string::npos);
    // Canonical order is alphabetical, so the bass and the extension mask sort
    // before the quality and the voicing hint sorts last. An event that states
    // none of them writes them as null and zero rather than omitting them.
    REQUIRE(
        first.value().json.find(
            R"("chord_scale_lane":[{"chord_bass":null,"chord_extensions":0,"chord_quality":"minor7","chord_root":9,"position":"0","scale_mode":"dorian","scale_root":9,"voicing":null})") !=
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
    constexpr std::array qualities{ChordQuality::Major,      ChordQuality::Minor,
                                   ChordQuality::Diminished, ChordQuality::Augmented,
                                   ChordQuality::Dominant7,  ChordQuality::Major7,
                                   ChordQuality::Minor7,     ChordQuality::HalfDiminished7,
                                   ChordQuality::Suspended2, ChordQuality::Suspended4};
    constexpr std::array modes{ScaleMode::Major,         ScaleMode::NaturalMinor,
                               ScaleMode::HarmonicMinor, ScaleMode::MelodicMinor,
                               ScaleMode::Dorian,        ScaleMode::Phrygian,
                               ScaleMode::Lydian,        ScaleMode::Mixolydian,
                               ScaleMode::Locrian,       ScaleMode::Chromatic};
    static_assert(qualities.size() == modes.size());
    for (std::size_t index = 0; index < qualities.size(); ++index)
        events.push_back(ChordScaleEvent{{static_cast<std::int64_t>(index)},
                                         qualities[index],
                                         static_cast<std::uint8_t>(index % 12),
                                         modes[index],
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
    const auto json =
        take(serialize_project(project_with_chord_lane(two_bar_lane()), registry)).json;

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
    REQUIRE(
        v3 ==
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
    REQUIRE_FALSE(
        registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 3, 2, populated));
}

TEST_CASE("a pre-lane sequence document loads as a sequence with no harmony",
          "[timeline][context-lane][migration]") {
    const auto registry = builtins();
    const auto current =
        take(serialize_project(project_with_chord_lane(lane_of({})), registry)).json;
    auto legacy = current;
    const auto lane_at = legacy.find(R"("chord_scale_lane":[],)");
    REQUIRE(lane_at != std::string::npos);
    legacy.erase(lane_at, std::string_view(R"("chord_scale_lane":[],)").size());
    constexpr std::string_view straight_groove =
        R"("groove":{"name":"","step":"0","steps":[],"swing_denominator":"2","swing_grid":"0","swing_numerator":"1","timing_strength":1000,"velocity_strength":1000},)";
    const auto groove_at = legacy.find(straight_groove);
    REQUIRE(groove_at != std::string::npos);
    legacy.erase(groove_at, straight_groove.size());
    const auto scenes_at = legacy.find(R"("scenes":[],)");
    REQUIRE(scenes_at != std::string::npos);
    legacy.erase(scenes_at, std::string_view(R"("scenes":[],)").size());
    // A pre-order document carries no authored track order. Its contents vary
    // with the track ids, so erase the member by span rather than by literal.
    const auto order_at = legacy.find(R"("track_order":[)");
    REQUIRE(order_at != std::string::npos);
    const auto order_end = legacy.find("],", order_at);
    REQUIRE(order_end != std::string::npos);
    legacy.erase(order_at, order_end + 2 - order_at);
    const auto version_at = legacy.find(R"("type_name":"pulp.timeline.sequence","version":7)");
    REQUIRE(version_at != std::string::npos);
    legacy.replace(version_at,
                   std::string_view(R"("type_name":"pulp.timeline.sequence","version":7)").size(),
                   R"("type_name":"pulp.timeline.sequence","version":2)");

    const auto decoded = take(deserialize_project(legacy, registry));
    REQUIRE(decoded.find_sequence({2})->chord_scale_lane().empty());
    REQUIRE(decoded.find_sequence({2})->groove().is_canonical_default());
    // Re-saving lands on the current version with all later context/session
    // fields materialized at their canonical defaults.
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

TEST_CASE("a widened chord event round trips and orders as before",
          "[timeline][context-lane][persistence]") {
    const auto registry = builtins();
    ChordScaleEvent slash{{0}, ChordQuality::Major7, 5, ScaleMode::Lydian, 0};
    slash.chord_bass = 9;
    slash.chord_extensions = kChordExtensionNinth | kChordExtensionSharpEleventh;
    slash.voicing = ChordVoicing::Rootless;
    ChordScaleEvent plain{{50}, ChordQuality::Minor, 2, ScaleMode::Dorian, 2};

    const auto project = project_with_chord_lane(lane_of({slash, plain}));
    const auto encoded = take(serialize_project(project, registry)).json;
    const auto decoded = take(deserialize_project(encoded, registry));
    const auto& events = decoded.find_sequence({2})->chord_scale_lane().events();
    REQUIRE(events.size() == 2);
    // Order is still position order: the added members sort after position in
    // the struct and so cannot reorder a lane that only orders by position.
    REQUIRE(events[0] == slash);
    REQUIRE(events[1] == plain);
    REQUIRE(take(serialize_project(decoded, registry)).json == encoded);

    // An event that states none of the detail encodes it as absent rather than
    // as some invented default, and comes back the same way.
    REQUIRE_FALSE(events[1].chord_bass.has_value());
    REQUIRE(events[1].chord_extensions == 0);
    REQUIRE_FALSE(events[1].voicing.has_value());
}

TEST_CASE("chord detail outside its declared vocabulary is refused",
          "[timeline][context-lane]") {
    ChordScaleEvent out_of_octave{{0}, ChordQuality::Major, 0, ScaleMode::Major, 0};
    out_of_octave.chord_bass = 12;
    REQUIRE_FALSE(ChordScaleLane::create({out_of_octave}));

    // An undefined extension bit would survive a round trip and mean something
    // different to the next reader that defines it.
    ChordScaleEvent undefined_extension{{0}, ChordQuality::Major, 0, ScaleMode::Major, 0};
    undefined_extension.chord_extensions = static_cast<std::uint16_t>(kChordExtensionMask + 1);
    const auto rejected = ChordScaleLane::create({undefined_extension});
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ModelErrorCode::InvalidChordScaleEvent);

    ChordScaleEvent unknown_voicing{{0}, ChordQuality::Major, 0, ScaleMode::Major, 0};
    unknown_voicing.voicing = static_cast<ChordVoicing>(99);
    REQUIRE_FALSE(ChordScaleLane::create({unknown_voicing}));

    // Every declared bit together is admitted, so the mask is a ceiling rather
    // than an accidental single-bit test.
    ChordScaleEvent every_extension{{0}, ChordQuality::Major, 0, ScaleMode::Major, 0};
    every_extension.chord_extensions = kChordExtensionMask;
    REQUIRE(ChordScaleLane::create({every_extension}));
}

TEST_CASE("the sequence chord-detail migration defaults old events and refuses to drop new ones",
          "[timeline][context-lane][migration]") {
    const auto registry = builtins();
    DecodeLimits limits;

    const auto plain = take(serialize_project(project_with_chord_lane(two_bar_lane()), registry));
    const auto lowered = take(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 7,
                                               6, sequence_envelope(plain.json), limits));
    // A v6 event has no detail members at all.
    REQUIRE(lowered.find(R"("chord_bass")") == std::string::npos);
    REQUIRE(lowered.find(R"("voicing")") == std::string::npos);
    REQUIRE(lowered.find(R"("type_name":"pulp.timeline.sequence","version":6)") !=
            std::string::npos);

    // The upgrade puts back exactly what the downgrade removed, so a v6
    // document states the same harmony a v7 one does.
    const auto raised = take(registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 6,
                                              7, lowered, limits));
    REQUIRE(raised == sequence_envelope(plain.json));

    // A bass, an extension, or a voicing hint has no v6 spelling, and dropping
    // one would leave a v6 reader stating a different chord.
    ChordScaleEvent rich{{0}, ChordQuality::Dominant7, 7, ScaleMode::Mixolydian, 0};
    rich.chord_bass = 11;
    const auto authored =
        take(serialize_project(project_with_chord_lane(lane_of({rich})), registry));
    auto refused = registry.migrate(SchemaDomain::Document, "pulp.timeline.sequence", 7, 6,
                                    sequence_envelope(authored.json), limits);
    REQUIRE_FALSE(refused);
    REQUIRE(refused.error().code == PersistenceErrorCode::MigrationFailed);
}

TEST_CASE("a set_chord_scale_lane command may omit or carry the chord detail",
          "[timeline][context-lane][persistence]") {
    const auto registry = builtins();
    // Commands are authored input with no version-gated migration path of their
    // own, so an omitted member keeps meaning what it meant before the field
    // existed rather than making the command undecodable.
    const std::string without =
        R"({"data":{"expected":[],"replacement":[{"chord_quality":"major","chord_root":0,"position":"0","scale_mode":"major","scale_root":0}],"sequence_id":"2"},"type_name":"pulp.timeline.command.set_chord_scale_lane","version":1})";
    auto omitted = deserialize_commands("[" + without + "]", registry);
    REQUIRE(omitted.has_value());
    const auto* plain = std::get_if<SetChordScaleLane>(&omitted.value()[0]);
    REQUIRE(plain != nullptr);
    REQUIRE_FALSE(plain->replacement.events()[0].chord_bass.has_value());
    REQUIRE(plain->replacement.events()[0].chord_extensions == 0);

    const std::string with =
        R"({"data":{"expected":[],"replacement":[{"chord_bass":4,"chord_extensions":32,"chord_quality":"major","chord_root":0,"position":"0","scale_mode":"major","scale_root":0,"voicing":"shell"}],"sequence_id":"2"},"type_name":"pulp.timeline.command.set_chord_scale_lane","version":1})";
    auto carried = deserialize_commands("[" + with + "]", registry);
    REQUIRE(carried.has_value());
    const auto* rich = std::get_if<SetChordScaleLane>(&carried.value()[0]);
    REQUIRE(rich != nullptr);
    REQUIRE(rich->replacement.events()[0].chord_bass == std::uint8_t{4});
    REQUIRE(rich->replacement.events()[0].chord_extensions == kChordExtensionThirteenth);
    REQUIRE(rich->replacement.events()[0].voicing == ChordVoicing::Shell);

    // Half the detail is neither spelling, and decoding it would invent the
    // rest rather than read it.
    const std::string partial =
        R"({"data":{"expected":[],"replacement":[{"chord_bass":4,"chord_quality":"major","chord_root":0,"position":"0","scale_mode":"major","scale_root":0}],"sequence_id":"2"},"type_name":"pulp.timeline.command.set_chord_scale_lane","version":1})";
    REQUIRE_FALSE(deserialize_commands("[" + partial + "]", registry));
}

TEST_CASE("a sequence older than the chord detail may not carry it",
          "[timeline][context-lane][persistence]") {
    const auto registry = builtins();
    const auto current = take(serialize_project(project_with_chord_lane(two_bar_lane()), registry));
    // Stamping an older version onto a payload that carries the detail is a
    // contradiction, not a document to read the detail out of. Both the
    // structural preflight and the decoder are asked, so neither can be the
    // only thing standing between a mislabelled document and a decode.
    auto mislabelled = current.json;
    constexpr std::string_view stamp = R"("type_name":"pulp.timeline.sequence","version":7)";
    const auto at = mislabelled.find(stamp);
    REQUIRE(at != std::string::npos);
    mislabelled.replace(at, stamp.size(),
                        R"("type_name":"pulp.timeline.sequence","version":6)");
    auto rejected = deserialize_project(mislabelled, registry);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == PersistenceErrorCode::InvalidSchema);
}
