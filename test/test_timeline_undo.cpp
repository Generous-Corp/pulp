#include "timeline_command_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace timeline_test;

TEST_CASE("Timeline undo and redo are ordinary journaled inverse transactions") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto tx = session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    REQUIRE(session->submit(writer, std::move(tx)));
    REQUIRE(session->can_undo());
    REQUIRE(session->undo(writer));
    REQUIRE(velocity(*session->snapshot()) == 1000);
    REQUIRE(session->can_redo());
    REQUIRE(session->redo(writer));
    REQUIRE(velocity(*session->snapshot()) == 2000);
    REQUIRE(session->journal().entries().size() == 3);
}

TEST_CASE("Timeline gesture grouping undoes the full change and writers never coalesce") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    const auto group = writer.allocate_undo_group_id();
    auto begin = session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    begin.undo_group = group;
    begin.gesture_phase = GesturePhase::Begin;
    REQUIRE(session->submit(writer, std::move(begin)));
    auto end = session_transaction(writer, session->revision(),
                                   {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 3000}});
    end.undo_group = group;
    end.gesture_phase = GesturePhase::End;
    REQUIRE(session->submit(writer, std::move(end)));
    REQUIRE(velocity(*session->snapshot()) == 3000);
    REQUIRE(session->undo(writer));
    REQUIRE(velocity(*session->snapshot()) == 1000);
    REQUIRE(session->redo(writer));
    REQUIRE(velocity(*session->snapshot()) == 3000);
}

TEST_CASE("Timeline redo reactivates identities created by an insert") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto inserted = make_note_clip({7}, {8}, 2 * kTicksPerQuarter);
    auto tx = session_transaction(writer, {}, {InsertClip{{3}, {4}, inserted}});
    REQUIRE(session->submit(writer, std::move(tx)));
    REQUIRE(session->undo(writer));
    REQUIRE(session->snapshot()->find_sequence({3})->find_track({4})->find_clip({7}) == nullptr);

    auto exploit =
        session_transaction(writer, session->revision(), {InsertClip{{3}, {4}, inserted}});
    auto rejected = session->submit(writer, std::move(exploit));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::IdentityNotAvailable);

    REQUIRE(session->redo(writer));
    const auto* restored = session->snapshot()->find_sequence({3})->find_track({4})->find_clip({7});
    REQUIRE(restored);
    REQUIRE(equivalent(*restored, inserted));

    const auto journal = session->journal();
    REQUIRE(journal.entries().back().kind == JournalEntryKind::History);
    auto replayed = journal.replay(make_project(), {});
    REQUIRE(replayed);
    REQUIRE(same_project(replayed.value(), *session->snapshot()));
}

TEST_CASE("Timeline undo capacity rejects an open gesture without partial publication") {
    SessionLimits limits;
    limits.undo.max_groups = 1;
    limits.undo.max_retained_bytes = 1;
    auto session = std::move(DocumentSession::create(make_project(), limits)).value();
    auto writer = std::move(session->register_writer()).value();
    auto tx = session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    tx.undo_group = writer.allocate_undo_group_id();
    tx.gesture_phase = GesturePhase::Begin;
    auto rejected = session->submit(writer, std::move(tx));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::UndoFull);
    REQUIRE(session->revision().value == 0);
    REQUIRE(session->journal().entries().empty());
    REQUIRE(velocity(*session->snapshot()) == 1000);
}
