#include "timeline_command_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace timeline_test;

TEST_CASE("Timeline journal replay reproduces the committed document") {
    const auto checkpoint = make_project();
    auto session = std::move(DocumentSession::create(checkpoint)).value();
    auto writer = std::move(session->register_writer()).value();
    auto first = session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    REQUIRE(session->submit(writer, std::move(first)));
    const auto before = clip(*session->snapshot()).time_range();
    ClipTimeRange after = MusicalTimeRange{{2 * kTicksPerQuarter}, {kTicksPerQuarter}};
    auto second =
        session_transaction(writer, session->revision(), {MoveClip{{3}, {4}, {5}, before, after}});
    REQUIRE(session->submit(writer, std::move(second)));

    const auto journal = session->journal();
    auto replayed = journal.replay(checkpoint, {});
    REQUIRE(replayed);
    REQUIRE(same_project(replayed.value(), *session->snapshot()));
}

TEST_CASE("Timeline journal is fail closed when full and truncates only checkpoints") {
    SessionLimits limits;
    limits.journal.max_transactions = 1;
    limits.journal.max_commands = 2;
    limits.journal.max_retained_bytes = 1024 * 1024;
    auto session = std::move(DocumentSession::create(make_project(), limits)).value();
    auto writer = std::move(session->register_writer()).value();
    auto first = session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    REQUIRE(session->submit(writer, std::move(first)));
    auto second = session_transaction(writer, session->revision(),
                                      {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 3000}});
    auto rejected = session->submit(writer, std::move(second));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::JournalFull);
    REQUIRE(session->revision().value == 1);
    REQUIRE(velocity(*session->snapshot()) == 2000);

    REQUIRE_FALSE(session->checkpoint({2}));
    REQUIRE(session->checkpoint({1}));
    auto journal = session->journal();
    REQUIRE(journal.entries().empty());
    REQUIRE(journal.base_revision().value == 1);
}
