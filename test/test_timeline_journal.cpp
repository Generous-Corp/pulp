#include "../core/timeline/src/journal_internal.hpp"
#include "timeline_command_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace timeline_test;

TEST_CASE("Timeline journal replay reproduces the committed document") {
    const auto checkpoint = make_project();
    auto session = std::move(DocumentSession::create(checkpoint)).value();
    auto writer = std::move(session->register_writer()).value();
    auto first = session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    REQUIRE(session->submit(writer, std::move(first)));
    const auto revision_one = session->snapshot();
    const auto before = clip(*session->snapshot()).time_range();
    ClipTimeRange after = MusicalTimeRange{{2 * kTicksPerQuarter}, {kTicksPerQuarter}};
    auto second =
        session_transaction(writer, session->revision(), {MoveClip{{3}, {4}, {5}, before, after}});
    REQUIRE(session->submit(writer, std::move(second)));

    REQUIRE(session->checkpoint({1}));
    const auto journal = session->journal();
    REQUIRE(journal.base_revision().value == 1);
    auto replayed = journal.replay(*revision_one, {1});
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

    auto stale_base = journal.replay(make_project(), {});
    REQUIRE_FALSE(stale_base);
    REQUIRE(stale_base.error().code == ConflictCode::StaleRevision);

    auto wrong_snapshot = make_project();
    auto inconsistent = journal.replay(wrong_snapshot, {1});
    REQUIRE_FALSE(inconsistent);
    REQUIRE(inconsistent.error().code == ConflictCode::ModelInvariant);
}

TEST_CASE("Timeline replay enforces revision and writer ID continuity across entries") {
    const auto initial = make_project();
    auto first = transaction({1}, 1, 1, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    auto first_reduced = reduce_transaction(initial, first);
    REQUIRE(first_reduced);

    auto make_journal = [&] {
        CommandJournal journal({});
        pulp::timeline::detail::JournalAccess::append(
            journal, {{}, {1}, first, first_reduced->dirty, JournalEntryKind::Ordinary}, initial);
        return journal;
    };

    SECTION("transaction IDs cannot repeat") {
        auto journal = make_journal();
        auto second =
            transaction({1}, 1, 2, {1}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 3000}});
        auto reduced = reduce_transaction(first_reduced->project, second);
        REQUIRE(reduced);
        pulp::timeline::detail::JournalAccess::append(
            journal, {{1}, {2}, second, reduced->dirty, JournalEntryKind::Ordinary},
            first_reduced->project);
        REQUIRE_FALSE(pulp::timeline::detail::JournalAccess::checkpoint(journal, {2}));
        REQUIRE(journal.base_revision().value == 0);
        REQUIRE(journal.entries().size() == 2);
        auto replayed = journal.replay(initial, {});
        REQUIRE_FALSE(replayed);
        REQUIRE(replayed.error().code == ConflictCode::TransactionIdCollision);
    }

    SECTION("command IDs cannot repeat") {
        auto journal = make_journal();
        auto second =
            transaction({1}, 2, 1, {1}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 3000}});
        auto reduced = reduce_transaction(first_reduced->project, second);
        REQUIRE(reduced);
        pulp::timeline::detail::JournalAccess::append(
            journal, {{1}, {2}, second, reduced->dirty, JournalEntryKind::Ordinary},
            first_reduced->project);
        auto replayed = journal.replay(initial, {});
        REQUIRE_FALSE(replayed);
        REQUIRE(replayed.error().code == ConflictCode::CommandIdCollision);
    }

    SECTION("transaction expected revision must match its entry") {
        auto journal = make_journal();
        auto second = transaction({1}, 2, 2, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 3000}});
        auto reduced = reduce_transaction(first_reduced->project, second);
        REQUIRE(reduced);
        pulp::timeline::detail::JournalAccess::append(
            journal, {{1}, {2}, second, reduced->dirty, JournalEntryKind::Ordinary},
            first_reduced->project);
        auto replayed = journal.replay(initial, {});
        REQUIRE_FALSE(replayed);
        REQUIRE(replayed.error().code == ConflictCode::StaleRevision);
    }
}

TEST_CASE("Timeline replay checkpoint equality includes inactive tombstones") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto remove = session_transaction(writer, {}, {RemoveClip{{3}, {4}, {5}}});
    REQUIRE(session->submit(writer, std::move(remove)));
    REQUIRE(session->checkpoint({1}));

    auto empty_track = Track::create({4}, "track", {});
    REQUIRE(empty_track);
    auto empty_sequence = Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter},
                                           {std::move(empty_track).value()});
    REQUIRE(empty_sequence);
    auto visible_match =
        Project::create({{1}, "project", 7, {3}, {}, {std::move(empty_sequence).value()}});
    REQUIRE(visible_match);
    REQUIRE(same_project(visible_match.value(), *session->snapshot()));

    auto rejected = session->journal().replay(visible_match.value(), {1});
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::ModelInvariant);
    REQUIRE(session->journal().replay(*session->snapshot(), {1}));
}
