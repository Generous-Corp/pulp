#include "timeline_command_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace timeline_test;

TEST_CASE("Timeline commands apply and invert without mutating the source") {
    const auto original = make_project();
    const auto inserted = make_note_clip({7}, {8}, 2 * kTicksPerQuarter, 2222);
    auto insert = transaction({1}, 1, 1, {}, {InsertClip{{3}, {4}, inserted}});
    auto after_insert = reduce_transaction(original, insert);
    REQUIRE(after_insert);
    REQUIRE(after_insert->project.find_sequence({3})->find_track({4})->find_clip({7}));
    REQUIRE(original.find_sequence({3})->find_track({4})->find_clip({7}) == nullptr);
    REQUIRE(after_insert->project.next_item_id() == 9);
    REQUIRE(after_insert->inverses.size() == 1);

    auto remove = transaction({1}, 2, 2, {}, after_insert->inverses);
    auto restored = reduce_transaction(after_insert->project, remove);
    REQUIRE(restored);
    REQUIRE(same_project(original, restored->project) == false);
    REQUIRE(restored->project.find_sequence({3})->find_track({4})->find_clip({7}) == nullptr);
    REQUIRE(restored->project.next_item_id() == 9);

    auto revive = transaction({1}, 3, 3, {}, restored->inverses);
    auto revived = reduce_transaction(restored->project, revive);
    REQUIRE_FALSE(revived);
    REQUIRE(revived.error().code == ConflictCode::IdentityNotAvailable);
}

TEST_CASE("Timeline move and note velocity commands enforce typed preconditions") {
    const auto original = make_project();
    const auto old_range = clip(original).time_range();
    ClipTimeRange new_range = MusicalTimeRange{{2 * kTicksPerQuarter}, {kTicksPerQuarter}};
    auto edit = transaction({1}, 1, 1, {},
                            {MoveClip{{3}, {4}, {5}, old_range, new_range},
                             SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 4096}});
    auto changed = reduce_transaction(original, edit);
    REQUIRE(changed);
    REQUIRE(clip(changed->project).start().value == 2 * kTicksPerQuarter);
    REQUIRE(velocity(changed->project) == 4096);
    REQUIRE(changed->dirty.items().size() == 2);

    auto inverse = transaction({1}, 2, 3, {}, changed->inverses);
    auto restored = reduce_transaction(changed->project, inverse);
    REQUIRE(restored);
    REQUIRE(same_project(original, restored->project));

    auto stale = edit;
    stale.id.sequence = 3;
    stale.commands[0].id.sequence = 5;
    stale.commands[1].id.sequence = 6;
    auto rejected = reduce_transaction(changed->project, stale);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::ExpectedValueMismatch);
}

TEST_CASE("Timeline move rejects sequence-boundary violations without touching the source") {
    const auto original = make_project();
    const auto old_range = clip(original).time_range();
    ClipTimeRange outside = MusicalTimeRange{{8 * kTicksPerQuarter}, {kTicksPerQuarter}};
    auto tx = transaction({1}, 1, 1, {}, {MoveClip{{3}, {4}, {5}, old_range, outside}});
    auto rejected = reduce_transaction(original, tx);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::ModelInvariant);
    REQUIRE(rejected.error().model_error);
    REQUIRE(clip(original).start().value == 0);
    REQUIRE(same_project(original, make_project()));
}

TEST_CASE("Timeline command diagnostics preserve target and media failure kinds") {
    const auto original = make_project();
    auto wrong_kind = transaction({1}, 1, 1, {}, {SetNoteVelocity{{3}, {4}, {5}, {5}, 1000, 2000}});
    auto wrong_kind_result = reduce_transaction(original, wrong_kind);
    REQUIRE_FALSE(wrong_kind_result);
    REQUIRE(wrong_kind_result.error().code == ConflictCode::WrongTargetKind);

    auto remove = transaction({1}, 2, 2, {}, {RemoveClip{{3}, {4}, {5}}});
    auto removed = reduce_transaction(original, remove);
    REQUIRE(removed);
    auto inactive = transaction(
        {1}, 3, 3, {},
        {MoveClip{{3}, {4}, {5}, clip(original).time_range(), clip(original).time_range()}});
    auto inactive_result = reduce_transaction(removed->project, inactive);
    REQUIRE_FALSE(inactive_result);
    REQUIRE(inactive_result.error().code == ConflictCode::InactiveTarget);

    auto missing_asset_clip =
        Clip::create({7}, {2 * kTicksPerQuarter}, {kTicksPerQuarter}, MediaRef{{99}, {0}, 1});
    REQUIRE(missing_asset_clip);
    auto missing_asset =
        transaction({1}, 2, 2, {}, {InsertClip{{3}, {4}, std::move(missing_asset_clip).value()}});
    auto missing_asset_result = reduce_transaction(original, missing_asset);
    REQUIRE_FALSE(missing_asset_result);
    REQUIRE(missing_asset_result.error().model_error);
    REQUIRE(missing_asset_result.error().model_error->code == ModelErrorCode::MissingAsset);
    REQUIRE(missing_asset_result.error().related_item == ItemId{99});

    auto track = Track::create({4}, "track", {make_note_clip({5}, {6}, 0)});
    REQUIRE(track);
    auto sequence = Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter},
                                     {std::move(track).value()});
    REQUIRE(sequence);
    MediaAsset asset{{9}, "asset", 10, {48'000, 1}};
    auto with_asset =
        Project::create({{1}, "project", 10, {3}, {asset}, {std::move(sequence).value()}});
    REQUIRE(with_asset);
    auto invalid_clip =
        Clip::create({10}, {2 * kTicksPerQuarter}, {kTicksPerQuarter}, MediaRef{{9}, {8}, 4});
    REQUIRE(invalid_clip);
    auto invalid_range =
        transaction({1}, 1, 1, {}, {InsertClip{{3}, {4}, std::move(invalid_clip).value()}});
    auto invalid_result = reduce_transaction(with_asset.value(), invalid_range);
    REQUIRE_FALSE(invalid_result);
    REQUIRE(invalid_result.error().model_error);
    REQUIRE(invalid_result.error().model_error->code == ModelErrorCode::InvalidMediaRange);
    REQUIRE(invalid_result.error().related_item == ItemId{9});
}

TEST_CASE("Timeline identity and clip indexes path copy at logarithmic scale") {
    std::vector<Clip> clips;
    clips.reserve(10000);
    for (std::uint64_t i = 0; i < 10000; ++i) {
        auto value =
            Clip::create({5 + i}, {static_cast<std::int64_t>(i * 16)}, {8}, EmptyContent{});
        REQUIRE(value);
        clips.push_back(std::move(value).value());
    }
    auto track = Track::create({4}, "large", std::move(clips));
    REQUIRE(track);
    auto sequence =
        Sequence::create({3}, "sequence", TickDuration{200000}, {std::move(track).value()});
    REQUIRE(sequence);
    auto project = Project::create({{1}, "large", 10005, {3}, {}, {std::move(sequence).value()}});
    REQUIRE(project);
    const auto before_nodes = Project::identity_stats().nodes_created;
    auto added = Clip::create({10005}, {160000}, {8}, EmptyContent{});
    REQUIRE(added);
    auto tx = transaction({1}, 1, 1, {}, {InsertClip{{3}, {4}, std::move(added).value()}});
    auto moved = reduce_transaction(project.value(), tx);
    REQUIRE(moved);
    REQUIRE(Project::identity_stats().nodes_created - before_nodes < 128);
    REQUIRE(project->shared_identity_nodes_with(moved->project) > 9900);
    REQUIRE(project->find_sequence({3})->find_track({4})->shared_index_nodes_with(
                *moved->project.find_sequence({3})->find_track({4})) > 19000);
}
