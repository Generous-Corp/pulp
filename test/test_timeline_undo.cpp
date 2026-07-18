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

TEST_CASE("Timeline gestures enforce phase ownership and coalesce at the group cap") {
    SessionLimits limits;
    limits.undo.max_groups = 1;
    auto session = std::move(DocumentSession::create(make_project(), limits)).value();
    auto writer = std::move(session->register_writer()).value();
    auto other = std::move(session->register_writer()).value();
    const auto group = writer.allocate_undo_group_id();

    auto invalid_phase =
        session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 1500}});
    invalid_phase.gesture_phase = static_cast<GesturePhase>(255);
    auto invalid_phase_result = session->submit(writer, std::move(invalid_phase));
    REQUIRE_FALSE(invalid_phase_result);
    REQUIRE(invalid_phase_result.error().code == ConflictCode::GestureState);

    auto malformed =
        session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 1500}});
    malformed.undo_group = group;
    malformed.gesture_phase = GesturePhase::Update;
    auto missing_begin = session->submit(writer, std::move(malformed));
    REQUIRE_FALSE(missing_begin);
    REQUIRE(missing_begin.error().code == ConflictCode::GestureState);

    auto begin = session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    begin.undo_group = group;
    begin.gesture_phase = GesturePhase::Begin;
    const auto begin_retry = begin;
    REQUIRE(session->submit(writer, std::move(begin)));
    REQUIRE(session->submit(writer, begin_retry));

    auto open_redo = session->redo(writer);
    REQUIRE_FALSE(open_redo);
    REQUIRE(open_redo.error().code == ConflictCode::GestureState);

    auto interleaved = session_transaction(other, session->revision(),
                                           {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 2500}});
    auto interleaved_result = session->submit(other, std::move(interleaved));
    REQUIRE_FALSE(interleaved_result);
    REQUIRE(interleaved_result.error().code == ConflictCode::GestureState);
    auto open_undo = session->undo(writer);
    REQUIRE_FALSE(open_undo);
    REQUIRE(open_undo.error().code == ConflictCode::GestureState);

    auto duplicate = session_transaction(writer, session->revision(),
                                         {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 2500}});
    duplicate.undo_group = group;
    duplicate.gesture_phase = GesturePhase::Begin;
    REQUIRE_FALSE(session->submit(writer, std::move(duplicate)));

    auto wrong_group = session_transaction(writer, session->revision(),
                                           {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 2500}});
    wrong_group.undo_group = writer.allocate_undo_group_id();
    wrong_group.gesture_phase = GesturePhase::End;
    REQUIRE_FALSE(session->submit(writer, std::move(wrong_group)));

    auto failed_update = session_transaction(writer, session->revision(),
                                             {SetNoteVelocity{{3}, {4}, {5}, {6}, 999, 2500}});
    failed_update.undo_group = group;
    failed_update.gesture_phase = GesturePhase::Update;
    auto failed_update_result = session->submit(writer, std::move(failed_update));
    REQUIRE_FALSE(failed_update_result);
    REQUIRE(failed_update_result.error().code == ConflictCode::ExpectedValueMismatch);

    auto update = session_transaction(writer, session->revision(),
                                      {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 2500}});
    update.undo_group = group;
    update.gesture_phase = GesturePhase::Update;
    REQUIRE(session->submit(writer, std::move(update)));
    auto end = session_transaction(writer, session->revision(),
                                   {SetNoteVelocity{{3}, {4}, {5}, {6}, 2500, 3000}});
    end.undo_group = group;
    end.gesture_phase = GesturePhase::End;
    const auto end_retry = end;
    REQUIRE(session->submit(writer, std::move(end)));
    REQUIRE(session->submit(writer, end_retry));
    REQUIRE(session->undo(writer));
    REQUIRE(velocity(*session->snapshot()) == 1000);
}
