#include "timeline_command_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace timeline_test;

namespace {

SequenceMarker marker(ItemId id, std::int64_t tick, std::string name) {
    return {id, MarkerTypeId::cue(), std::move(name), MusicalSequencePoint{{tick}}};
}

SequenceMarker absolute_marker(ItemId id, std::int64_t sample, RationalRate rate) {
    return {id, MarkerTypeId::cue(), "cue", AbsoluteSequencePoint{{sample}, rate}};
}

SequenceRegion absolute_region(ItemId id, std::int64_t sample, RationalRate rate) {
    return {id, "region", AbsoluteSequenceRange{{sample}, 100, rate}};
}

} // namespace

TEST_CASE("Sequence annotation commands publish exact dirty state and inverses",
          "[timeline][annotation]") {
    const auto initial = make_project();
    const auto inserted = marker({10}, 100, "cue");
    auto reduced = reduce_transaction(
        initial, transaction({1}, 1, 1, {}, {InsertSequenceMarker{{3}, inserted}}));
    REQUIRE(reduced);
    REQUIRE(reduced->project.find_sequence({3})->find_marker({10}));
    CHECK(reduced->project.next_item_id() == 11);
    REQUIRE(reduced->dirty.items().size() == 1);
    CHECK(reduced->dirty.items()[0] ==
          DirtyItem{
              {10}, {}, {3}, DirtyFlags::Structure | DirtyFlags::Annotation | DirtyFlags::Added});
    CHECK(std::holds_alternative<RemoveSequenceMarker>(reduced->inverses[0]));

    const auto replacement = marker({10}, 200, "moved");
    auto changed = reduce_transaction(
        reduced->project,
        transaction({1}, 2, 2, {}, {SetSequenceMarker{{3}, {10}, inserted, replacement}}));
    REQUIRE(changed);
    CHECK(changed->project.find_sequence({3})->find_marker({10})->name == "moved");
    CHECK(std::holds_alternative<SetSequenceMarker>(changed->inverses[0]));

    auto stale = reduce_transaction(
        changed->project,
        transaction({1}, 3, 3, {}, {SetSequenceMarker{{3}, {10}, inserted, replacement}}));
    REQUIRE_FALSE(stale);
    CHECK(stale.error().code == ConflictCode::ExpectedValueMismatch);
}

TEST_CASE("Sequence annotation commands round trip through undo and tombstones",
          "[timeline][annotation]") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto insert = session_transaction(
        writer, {}, {InsertSequenceRegion{{3}, {{10}, "verse", MusicalSequenceRange{{0}, {100}}}}});
    REQUIRE(session->submit(writer, std::move(insert)));
    REQUIRE(session->snapshot()->find_sequence({3})->find_region({10}));
    auto replayed = session->journal().replay(make_project(), {});
    REQUIRE(replayed);
    CHECK(replayed->find_sequence({3})->find_region({10}) != nullptr);
    REQUIRE(session->undo(writer));
    CHECK(session->snapshot()->find_sequence({3})->find_region({10}) == nullptr);
    REQUIRE(session->snapshot()->locate({10}));
    CHECK_FALSE(session->snapshot()->locate({10})->active);
    CHECK(session->snapshot()->locate({10})->kind == ItemKind::SequenceRegion);
    REQUIRE(session->redo(writer));
    CHECK(session->snapshot()->find_sequence({3})->find_region({10}) != nullptr);
}

TEST_CASE("Sequence annotation undo expects canonical absolute rates", "[timeline][annotation]") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto insert =
        session_transaction(writer, {},
                            {InsertSequenceMarker{{3}, absolute_marker({10}, 100, {48000, 1})},
                             InsertSequenceRegion{{3}, absolute_region({11}, 200, {48000, 1})}});
    REQUIRE(session->submit(writer, std::move(insert)));
    const auto before_marker = *session->snapshot()->find_sequence({3})->find_marker({10});
    const auto before_region = *session->snapshot()->find_sequence({3})->find_region({11});

    auto change = session_transaction(
        writer, session->revision(),
        {SetSequenceMarker{{3}, {10}, before_marker, absolute_marker({10}, 300, {96000, 2})},
         SetSequenceRegion{{3}, {11}, before_region, absolute_region({11}, 400, {96000, 2})}});
    REQUIRE(session->submit(writer, std::move(change)));
    REQUIRE(session->undo(writer));
    CHECK(*session->snapshot()->find_sequence({3})->find_marker({10}) == before_marker);
    CHECK(*session->snapshot()->find_sequence({3})->find_region({11}) == before_region);
}

TEST_CASE("Annotation command equivalence and retention include semantic payload",
          "[timeline][annotation]") {
    const Command left = InsertSequenceMarker{{3}, marker({10}, 100, "cue")};
    const Command same = InsertSequenceMarker{{3}, marker({10}, 100, "cue")};
    const Command renamed = InsertSequenceMarker{{3}, marker({10}, 100, "other")};
    CHECK(equivalent(left, same));
    CHECK_FALSE(equivalent(left, renamed));
    CHECK(retained_size(left) >= sizeof(InsertSequenceMarker) + 3);
}
