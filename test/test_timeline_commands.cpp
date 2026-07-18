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
    auto revived = reduce_history_transaction(restored->project, revive);
    REQUIRE(revived);
    REQUIRE(equivalent(*revived->project.find_sequence({3})->find_track({4})->find_clip({7}),
                       inserted));
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
