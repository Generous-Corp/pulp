#include "timeline_command_test_helpers.hpp"

#include <pulp/timeline/serialize.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace timeline_test;

namespace {

template <typename T, typename E> T take_result(pulp::runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

ContentHash hash_of(char digit) {
    return *ContentHash::from_hex(std::string(64, digit));
}

MediaAsset source_asset() {
    return {{20}, "source.wav", 256, {48'000, 1}, hash_of('a'), AssetStoragePolicy::External, {},
            {}};
}

MediaAsset frozen_asset(ItemId id = {21}, std::uint64_t frames = 128) {
    return {id,           "track-freeze.wav",           frames, {48'000, 1},
            hash_of('b'), AssetStoragePolicy::External, {},     {}};
}

TrackFreeze freeze(ItemId asset_id = {21}, std::uint64_t frames = 128) {
    return {MediaRef{asset_id, {0}, frames}, {64}, {48'000, 1}, hash_of('c')};
}

Project project(bool include_frozen_asset = false,
                std::optional<TrackFreeze> selected = std::nullopt) {
    auto clip =
        take_result(Clip::create_absolute({11}, {0}, 256, {48'000, 1}, MediaRef{{20}, {0}, 256}));
    auto track = take_result(Track::create(
        TrackInput{.id = {10}, .name = "track", .clips = {clip}, .freeze = selected}));
    auto sequence =
        take_result(Sequence::create({3}, "sequence", std::nullopt, std::nullopt, {track}));
    std::vector<MediaAsset> assets{source_asset()};
    if (include_frozen_asset)
        assets.push_back(frozen_asset());
    return take_result(Project::create(ProjectInput{
        {1}, "project", include_frozen_asset ? 22u : 21u, {3}, std::move(assets), {sequence}}));
}

const Track& track(const Project& value) {
    return *value.find_sequence({3})->find_track({10});
}

} // namespace

TEST_CASE("Track freeze command publishes a sealed artifact without changing authored state") {
    const auto original = project();
    const auto frozen = freeze();
    REQUIRE(equivalent(Command{SetTrackFreeze{{3}, {10}, std::nullopt, frozen}},
                       Command{SetTrackFreeze{{3}, {10}, std::nullopt, frozen}}));
    REQUIRE_FALSE(equivalent(Command{SetTrackFreeze{{3}, {10}, std::nullopt, frozen}},
                             Command{SetTrackFreeze{{3}, {10}, frozen, std::nullopt}}));
    REQUIRE(retained_size(Command{SetTrackFreeze{{3}, {10}, std::nullopt, frozen}}) ==
            sizeof(SetTrackFreeze));
    auto reduced = reduce_transaction(
        original, transaction({1}, 1, 1, {},
                              {CreateAsset{frozen_asset()},
                               SetTrackFreeze{{3}, {10}, std::nullopt, frozen}}));
    REQUIRE(reduced);
    REQUIRE_FALSE(track(original).freeze());
    REQUIRE(track(reduced->project).freeze() == frozen);
    REQUIRE(track(reduced->project).clips().size() == 1);
    REQUIRE(track(reduced->project).clips()[0].id() == ItemId{11});
    REQUIRE(reduced->dirty.items().size() == 2);
    REQUIRE(reduced->dirty.items()[1] ==
            DirtyItem{{10}, {10}, {3}, DirtyFlags::Content | DirtyFlags::Freeze});
    REQUIRE(std::holds_alternative<SetTrackFreeze>(reduced->inverses[0]));
    REQUIRE(std::holds_alternative<RemoveAsset>(reduced->inverses[1]));
}

TEST_CASE("Track freeze is undoable redoable and journal replay deterministic") {
    const auto initial = project();
    auto session = take_result(DocumentSession::create(initial));
    auto writer = take_result(session->register_writer());
    REQUIRE(session->submit(
        writer, session_transaction(writer, {},
                                    {CreateAsset{frozen_asset()},
                                     SetTrackFreeze{{3}, {10}, std::nullopt, freeze()}})));
    REQUIRE(track(*session->snapshot()).freeze());

    REQUIRE(session->undo(writer));
    REQUIRE_FALSE(track(*session->snapshot()).freeze());
    REQUIRE(session->snapshot()->find_asset({21}) == nullptr);
    REQUIRE(session->redo(writer));
    REQUIRE(track(*session->snapshot()).freeze() == freeze());
    REQUIRE(session->snapshot()->find_asset({21}) != nullptr);

    const auto replayed = take_result(session->journal().replay(initial, {}));
    REQUIRE(track(replayed).freeze() == freeze());
    REQUIRE(replayed.find_asset({21})->content_hash == frozen_asset().content_hash);
}

TEST_CASE("Track freeze rejects stale gates invalid media and referenced asset removal") {
    auto noncanonical_rate = freeze();
    noncanonical_rate.sample_rate = {96'000, 2};
    auto invalid_track =
        Track::create(TrackInput{.id = {10}, .name = "track", .freeze = noncanonical_rate});
    REQUIRE_FALSE(invalid_track);
    REQUIRE(invalid_track.error().code == ModelErrorCode::InvalidSampleRate);

    const auto selected = project(true, freeze());
    auto stale = reduce_transaction(
        selected,
        transaction({1}, 1, 1, {}, {SetTrackFreeze{{3}, {10}, std::nullopt, freeze({21}, 64)}}));
    REQUIRE_FALSE(stale);
    REQUIRE(stale.error().code == ConflictCode::ExpectedValueMismatch);

    auto missing = reduce_transaction(
        project(),
        transaction({1}, 2, 2, {}, {SetTrackFreeze{{3}, {10}, std::nullopt, freeze({99})}}));
    REQUIRE_FALSE(missing);
    REQUIRE(missing.error().model_error->code == ModelErrorCode::MissingAsset);

    auto out_of_range = freeze();
    out_of_range.media.source_start = {127};
    out_of_range.media.frame_count = 2;
    auto invalid_range = reduce_transaction(
        project(true),
        transaction({1}, 3, 3, {}, {SetTrackFreeze{{3}, {10}, std::nullopt, out_of_range}}));
    REQUIRE_FALSE(invalid_range);
    REQUIRE(invalid_range.error().model_error->code == ModelErrorCode::InvalidMediaRange);

    auto wrong_rate = freeze();
    wrong_rate.sample_rate = {44'100, 1};
    auto incompatible = reduce_transaction(
        project(true),
        transaction({1}, 4, 4, {}, {SetTrackFreeze{{3}, {10}, std::nullopt, wrong_rate}}));
    REQUIRE_FALSE(incompatible);
    REQUIRE(incompatible.error().model_error->code == ModelErrorCode::IncompatibleSampleRate);

    auto remove = reduce_transaction(selected, transaction({1}, 5, 5, {}, {RemoveAsset{{21}}}));
    REQUIRE_FALSE(remove);
    REQUIRE(remove.error().model_error->code == ModelErrorCode::MissingAsset);
}

TEST_CASE("Track freeze persists canonically and older schema downgrade fails closed") {
    auto registry = take_result(make_builtin_timeline_registry());
    const auto original = project(true, freeze());
    auto encoded_result = serialize_project(original, registry);
    REQUIRE(encoded_result);
    const auto encoded = std::move(encoded_result).value();
    REQUIRE(encoded.json.find("\"freeze\":{\"asset_id\":\"21\"") != std::string::npos);
    REQUIRE(encoded.json.find("\"render_plan_hash\":\"" + std::string(64, 'c') + "\"") !=
            std::string::npos);
    REQUIRE(encoded.json.find("\"type_name\":\"pulp.timeline.track\",\"version\":6") !=
            std::string::npos);
    const auto decoded = take_result(deserialize_project(encoded.json, registry));
    REQUIRE(track(decoded).freeze() == freeze());
    REQUIRE(take_result(serialize_project(decoded, registry)).json == encoded.json);

    const std::string v5 =
        R"({"data":{"active_take_lane_id":"0","automation_lanes":[],"clips":[],)"
        R"("device_chain":[],"id":"3","name":"track","record_armed":false,"take_lanes":[]},)"
        R"("type_name":"pulp.timeline.track","version":5})";
    const auto v6 =
        take_result(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 5, 6, v5));
    REQUIRE(v6.find("\"version\":6") != std::string::npos);
    REQUIRE(take_result(
                registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 6, 5, v6)) == v5);

    const std::string frozen_v6 =
        R"({"data":{"active_take_lane_id":"0","automation_lanes":[],"clips":[],)"
        R"("device_chain":[],"freeze":{},"id":"3","name":"track","record_armed":false,)"
        R"("take_lanes":[]},"type_name":"pulp.timeline.track","version":6})";
    REQUIRE_FALSE(registry.migrate(SchemaDomain::Document, "pulp.timeline.track", 6, 5, frozen_v6));
}
