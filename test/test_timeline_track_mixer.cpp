#include "timeline_command_test_helpers.hpp"

#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>

using namespace timeline_test;

namespace {

template <typename T, typename E> T take_result(pulp::runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

AutomationLane mixer_lane(ItemId lane_id, ItemId point_id, TrackMixerParameter parameter,
                          float first, float second) {
    auto curve = AutomationCurve::create(
        {AutomationPoint{point_id, {0}, first, AutomationInterpolation::Continuous, 0.0f},
         AutomationPoint{{point_id.value + 1}, {kTicksPerQuarter}, second,
                         AutomationInterpolation::Continuous, 0.0f}});
    REQUIRE(curve);
    return take_result(
        AutomationLane::create(lane_id, TrackMixerTarget{parameter}, std::move(curve).value()));
}

Project mixer_project(TrackMixer mixer, std::vector<AutomationLane> lanes = {}) {
    auto track = take_result(Track::create(TrackInput{.id = {4},
                                                      .name = "track",
                                                      .clips = {make_note_clip({5}, {6}, 0)},
                                                      .automation_lanes = std::move(lanes),
                                                      .mixer = mixer}));
    auto sequence = take_result(Sequence::create({3}, "sequence",
                                                 TickDuration{8 * kTicksPerQuarter}, {track}));
    return take_result(
        Project::create({{1}, "project", 100, {3}, {}, {std::move(sequence)}}));
}

const Track& track_of(const Project& value) {
    return *value.find_sequence({3})->find_track({4});
}

SchemaRegistry registry() {
    return take_result(make_builtin_timeline_registry());
}

std::string serialized(const Project& project) {
    return take_result(serialize_project(project, registry())).json;
}

// The track envelope inside a serialized project, isolated so a migration can be
// driven against exactly what the encoder writes rather than a hand-typed shape.
std::string track_envelope(const std::string& document) {
    const auto tracks = document.find("\"tracks\":[");
    REQUIRE(tracks != std::string::npos);
    const auto begin = tracks + std::string("\"tracks\":[").size();
    std::size_t depth = 0;
    for (auto index = begin; index < document.size(); ++index) {
        if (document[index] == '{')
            ++depth;
        else if (document[index] == '}' && --depth == 0)
            return document.substr(begin, index + 1 - begin);
    }
    FAIL("no track envelope found");
    return {};
}

} // namespace

TEST_CASE("Track mixer round-trips and re-saves byte-identically", "[timeline][mixer]") {
    const auto authored = mixer_project(
        TrackMixer{0.5f, -0.25f},
        {mixer_lane({40}, {41}, TrackMixerParameter::Gain, 0.25f, 0.75f),
         mixer_lane({50}, {51}, TrackMixerParameter::Pan, -1.0f, 1.0f)});
    const auto document = serialized(authored);
    REQUIRE(document.find("\"mixer\":{\"gain_linear_bits\":") != std::string::npos);
    REQUIRE(document.find("pulp.timeline.automation_target.track_mixer") != std::string::npos);

    const auto restored = take_result(deserialize_project(document, registry()));
    REQUIRE(track_of(restored).mixer() == TrackMixer{0.5f, -0.25f});
    const auto lanes = track_of(restored).automation_lanes();
    REQUIRE(lanes.size() == 2);
    REQUIRE(std::get<TrackMixerTarget>(lanes[0].target()) ==
            TrackMixerTarget{TrackMixerParameter::Gain});
    REQUIRE(std::get<TrackMixerTarget>(lanes[1].target()) ==
            TrackMixerTarget{TrackMixerParameter::Pan});

    REQUIRE(serialized(restored) == document);
}

TEST_CASE("A default track mixer is written as absence", "[timeline][mixer]") {
    // Absence is what keeps a document that never touched a fader byte-identical
    // to its pre-mixer form, so this is a persistence contract, not a size tweak.
    const auto document = serialized(mixer_project(TrackMixer{}));
    REQUIRE(document.find("\"mixer\"") == std::string::npos);
    REQUIRE(track_of(take_result(deserialize_project(document, registry()))).mixer() ==
            TrackMixer{});
}

TEST_CASE("Track schema migrates v6 to v7 and back", "[timeline][mixer][migration]") {
    const auto reg = registry();
    const auto plain = track_envelope(serialized(mixer_project(TrackMixer{})));
    REQUIRE(plain.find("\"version\":7") != std::string::npos);

    const auto downgraded = take_result(
        reg.migrate(SchemaDomain::Document, "pulp.timeline.track", 7, 6, plain, {}));
    REQUIRE(downgraded.find("\"version\":6") != std::string::npos);
    REQUIRE(downgraded.find("\"mixer\"") == std::string::npos);

    const auto upgraded = take_result(
        reg.migrate(SchemaDomain::Document, "pulp.timeline.track", 6, 7, downgraded, {}));
    REQUIRE(upgraded == plain);
}

TEST_CASE("Downgrading a track that carries a mixer is refused", "[timeline][mixer][migration]") {
    // A v6 track has nowhere to put a gain. Writing one out silently would change
    // how the document sounds, so the migration refuses instead.
    const auto authored = track_envelope(serialized(mixer_project(TrackMixer{0.25f, 0.5f})));
    REQUIRE(authored.find("\"mixer\"") != std::string::npos);
    const auto downgraded =
        registry().migrate(SchemaDomain::Document, "pulp.timeline.track", 7, 6, authored, {});
    REQUIRE_FALSE(downgraded);
}

TEST_CASE("Downgrading a track with mixer automation is refused", "[timeline][mixer][migration]") {
    const auto authored = track_envelope(serialized(mixer_project(
        TrackMixer{}, {mixer_lane({40}, {41}, TrackMixerParameter::Gain, 0.25f, 0.75f)})));
    REQUIRE(authored.find("pulp.timeline.automation_target.track_mixer") != std::string::npos);
    const auto downgraded =
        registry().migrate(SchemaDomain::Document, "pulp.timeline.track", 7, 6, authored, {});
    REQUIRE_FALSE(downgraded);
}

TEST_CASE("Track mixer automation target rejects unknown enum values", "[timeline][mixer]") {
    const auto invalid = static_cast<TrackMixerParameter>(255);
    REQUIRE_FALSE(TrackMixerTarget{invalid}.valid());
    auto curve = AutomationCurve::create({AutomationPoint{{41}, {0}, 0.5f}});
    REQUIRE(curve);
    REQUIRE_FALSE(
        AutomationLane::create({40}, TrackMixerTarget{invalid}, std::move(curve).value()));
}

TEST_CASE("Track mixer refuses values outside its authored range", "[timeline][mixer]") {
    const auto rejected = [](TrackMixer mixer) {
        auto created = Track::create(TrackInput{.id = {4}, .name = "track", .mixer = mixer});
        REQUIRE_FALSE(created);
        REQUIRE(created.error().code == ModelErrorCode::InvalidTrackMixer);
    };
    rejected(TrackMixer{-0.001f, 0.0f});
    rejected(TrackMixer{kMaximumTrackGainLinear + 1.0f, 0.0f});
    rejected(TrackMixer{std::numeric_limits<float>::quiet_NaN(), 0.0f});
    rejected(TrackMixer{std::numeric_limits<float>::infinity(), 0.0f});
    rejected(TrackMixer{1.0f, -1.001f});
    rejected(TrackMixer{1.0f, 1.001f});
    rejected(TrackMixer{1.0f, std::numeric_limits<float>::quiet_NaN()});

    const auto valid = take_result(
        Track::create(TrackInput{.id = {4}, .name = "track", .mixer = TrackMixer{0.0f, 1.0f}}));
    REQUIRE(valid.mixer() == TrackMixer{0.0f, 1.0f});
    REQUIRE_FALSE(valid.with_mixer(TrackMixer{2.0f, 2.0f}));
    REQUIRE(take_result(valid.with_mixer(TrackMixer{2.0f, -1.0f})).mixer() ==
            TrackMixer{2.0f, -1.0f});
}

TEST_CASE("A decoded mixer outside the authored range is refused", "[timeline][mixer]") {
    // The encoder can only write values the model accepted, so this drives the
    // decoder with a hand-built document to prove a hostile file cannot install
    // a gain the model would have rejected.
    auto document = serialized(mixer_project(TrackMixer{0.5f, 0.0f}));
    // Scoped to the mixer member on purpose: a clip carries its own
    // gain_linear_bits and is written first, so an unscoped search would corrupt
    // the clip and prove nothing about the mixer.
    const auto key = std::string("\"mixer\":{\"gain_linear_bits\":\"");
    const auto gain = document.find(key);
    REQUIRE(gain != std::string::npos);
    const auto begin = gain + key.size();
    const auto end = document.find('"', begin);
    REQUIRE(end != std::string::npos);
    // 0x7fc00000 is a quiet NaN, which no comparison against the range accepts.
    document.replace(begin, end - begin, "2143289344");
    REQUIRE_FALSE(deserialize_project(document, registry()));
}

TEST_CASE("Set track mixer command reduces, inverts, and replays", "[timeline][mixer][command]") {
    const auto original = mixer_project(TrackMixer{});
    const TrackMixer replacement{0.5f, 0.25f};
    REQUIRE(equivalent(Command{SetTrackMixer{{3}, {4}, {}, replacement}},
                       Command{SetTrackMixer{{3}, {4}, {}, replacement}}));
    REQUIRE_FALSE(equivalent(Command{SetTrackMixer{{3}, {4}, {}, replacement}},
                             Command{SetTrackMixer{{3}, {4}, replacement, {}}}));

    auto reduced = reduce_transaction(
        original, transaction({1}, 1, 1, {}, {SetTrackMixer{{3}, {4}, {}, replacement}}));
    REQUIRE(reduced);
    REQUIRE(track_of(original).mixer() == TrackMixer{});
    REQUIRE(track_of(reduced->project).mixer() == replacement);
    REQUIRE(reduced->dirty.items().size() == 1);
    REQUIRE(reduced->dirty.items()[0] ==
            DirtyItem{{4}, {4}, {3}, DirtyFlags::Content | DirtyFlags::Mixer});
    REQUIRE(equivalent(reduced->inverses[0],
                       Command{SetTrackMixer{{3}, {4}, replacement, TrackMixer{}}}));

    auto session = take_result(DocumentSession::create(original));
    auto writer = take_result(session->register_writer());
    REQUIRE(session->submit(
        writer, session_transaction(writer, {}, {SetTrackMixer{{3}, {4}, {}, replacement}})));
    REQUIRE(track_of(*session->snapshot()).mixer() == replacement);
    REQUIRE(session->undo(writer));
    REQUIRE(track_of(*session->snapshot()).mixer() == TrackMixer{});
    REQUIRE(session->redo(writer));
    REQUIRE(track_of(take_result(session->journal().replay(original, {}))).mixer() == replacement);
}

TEST_CASE("Set track mixer refuses a stale gate and an unrepresentable value",
          "[timeline][mixer][command]") {
    const auto original = mixer_project(TrackMixer{0.5f, 0.0f});
    auto stale = reduce_transaction(
        original,
        transaction({1}, 1, 1, {}, {SetTrackMixer{{3}, {4}, TrackMixer{}, TrackMixer{0.25f, 0.0f}}}));
    REQUIRE_FALSE(stale);
    REQUIRE(stale.error().code == ConflictCode::ExpectedValueMismatch);

    auto invalid = reduce_transaction(
        original, transaction({1}, 2, 2, {},
                              {SetTrackMixer{{3}, {4}, TrackMixer{0.5f, 0.0f},
                                             TrackMixer{kMaximumTrackGainLinear * 2.0f, 0.0f}}}));
    REQUIRE_FALSE(invalid);
    REQUIRE(invalid.error().model_error->code == ModelErrorCode::InvalidTrackMixer);
}

TEST_CASE("A mixer automation lane needs no device placement but stays unique",
          "[timeline][mixer][automation]") {
    // A device lane must name a placement in the chain; a mixer lane names a
    // control the track always has, so the missing-target rule must not apply to
    // it while the one-lane-per-control rule still must.
    const auto authored =
        mixer_project(TrackMixer{}, {mixer_lane({40}, {41}, TrackMixerParameter::Gain, 0.0f, 1.0f)});
    REQUIRE(track_of(authored).automation_lanes().size() == 1);

    auto duplicate = Track::create(
        TrackInput{.id = {4},
                   .name = "track",
                   .automation_lanes = {mixer_lane({40}, {41}, TrackMixerParameter::Gain, 0.0f,
                                                   1.0f),
                                        mixer_lane({60}, {61}, TrackMixerParameter::Gain, 1.0f,
                                                   0.0f)}});
    REQUIRE_FALSE(duplicate);
    REQUIRE(duplicate.error().code == ModelErrorCode::DuplicateAutomationTarget);

    // Gain and pan are different controls and must not collide with each other.
    REQUIRE(Track::create(
        TrackInput{.id = {4},
                   .name = "track",
                   .automation_lanes = {mixer_lane({40}, {41}, TrackMixerParameter::Gain, 0.0f,
                                                   1.0f),
                                        mixer_lane({60}, {61}, TrackMixerParameter::Pan, -1.0f,
                                                   1.0f)}}));
}

TEST_CASE("A mixer lane insert command needs no device placement",
          "[timeline][mixer][automation]") {
    auto reduced = reduce_transaction(
        mixer_project(TrackMixer{}),
        transaction({1}, 1, 1, {},
                    {InsertAutomationLane{
                        // Identities at or above the project's next_item_id are
                        // the only ones a transaction may claim.
                        {3}, {4}, mixer_lane({100}, {101}, TrackMixerParameter::Gain, 0.0f,
                                             1.0f)}}));
    REQUIRE(reduced);
    REQUIRE(track_of(reduced->project).automation_lanes().size() == 1);
}

TEST_CASE("Remapping a track carries its authored mixer and arm intent",
          "[timeline][mixer][remap]") {
    // Copy, paste, and import all rebuild a track around fresh identities.
    // Authored value state is not identity, so it must survive the rewrite
    // unchanged rather than fall back to unity gain and centre pan.
    const TrackMixer authored{0.25f, -0.75f};
    const auto original = take_result(Track::create(TrackInput{.id = {4},
                                                               .name = "authored track",
                                                               .clips = {make_note_clip({5}, {6},
                                                                                        0)},
                                                               .record_armed = true,
                                                               .mixer = authored}));
    REQUIRE(original.mixer().gain_linear == 0.25f);
    REQUIRE(original.mixer().pan == -0.75f);

    ItemIdAllocator allocator(100);
    const auto remapped = take_result(remap_ids(original, allocator));
    REQUIRE(remapped.track.id() != original.id());
    REQUIRE(remapped.track.mixer().gain_linear == 0.25f);
    REQUIRE(remapped.track.mixer().pan == -0.75f);
    REQUIRE(remapped.track.record_armed());
    REQUIRE(remapped.track.name() == "authored track");

    const auto sequence =
        take_result(Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter},
                                     {original}));
    const auto project = take_result(Project::create(ProjectInput{.id = {1},
                                                                  .name = "project",
                                                                  .next_item_id = 100,
                                                                  .root_sequence_id = {3},
                                                                  .sequences = {sequence}}));
    const auto remapped_project = take_result(remap_ids(project, 200));
    const auto* rebuilt =
        remapped_project.project.sequences()[0].find_track(*remapped_project.ids.find({4}));
    REQUIRE(rebuilt != nullptr);
    REQUIRE(rebuilt->mixer().gain_linear == 0.25f);
    REQUIRE(rebuilt->mixer().pan == -0.75f);
    REQUIRE(rebuilt->record_armed());
    REQUIRE(rebuilt->name() == "authored track");
}
