#include "../core/timeline/src/writer_token_test_access.hpp"
#include "timeline_command_test_helpers.hpp"

#include <pulp/timeline_editor/edit_intent.hpp>
#include <pulp/timeline_editor/gesture_budget.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <optional>
#include <type_traits>
#include <vector>

using namespace pulp::timeline;
using namespace pulp::timeline_editor;
using namespace timeline_test;

namespace {

// Dense enough that one whole-content replacement is a meaningful charge, small
// enough to stay a fast unit test.
constexpr std::size_t kNoteCount = 200;
constexpr std::size_t kGrowthNotes = 100;
/// Small enough that a whole-content edit over it is cheap, putting it on the
/// far side of the crossover where the journal's flat transaction cap binds
/// before the undo budget does.
constexpr std::size_t kSparseNoteCount = 16;
/// Budget sizes the boundary is checked at, in steps. Small on purpose: the
/// boundary has to be reached exactly rather than approached. Two of them,
/// because one budget is satisfied by a prediction that ignores its inputs and
/// returns that number — the pair pins a boundary, but only a MOVING boundary
/// pins the arithmetic that places it.
constexpr std::array<std::size_t, 2> kAffordableSteps{4, 7};

constexpr ItemId kSequence{3};
constexpr ItemId kTrack{4};
constexpr ItemId kClip{5};
constexpr ItemId kFirstNote{10};
constexpr std::int64_t kNoteSpacing = kTicksPerQuarter / 4;

std::vector<NoteEvent> notes_of(std::size_t count) {
    std::vector<NoteEvent> notes;
    notes.reserve(count);
    for (std::size_t index = 0; index < count; ++index)
        notes.push_back(NoteEvent{{kFirstNote.value + index},
                                  {static_cast<std::int64_t>(index) * kNoteSpacing},
                                  {kTicksPerQuarter / 8},
                                  1000,
                                  static_cast<std::uint8_t>(36 + index % 48),
                                  0});
    return notes;
}

Project make_note_project() {
    const TickDuration span{static_cast<std::int64_t>(kNoteCount) * kNoteSpacing};
    auto content = MidiContent::create(notes_of(kNoteCount));
    REQUIRE(content);
    auto clip = Clip::create(kClip, {0}, span, std::move(content).value());
    REQUIRE(clip);
    auto track = Track::create(kTrack, "track", {std::move(clip).value()});
    REQUIRE(track);
    auto sequence = Sequence::create(kSequence, "sequence", span, {std::move(track).value()});
    REQUIRE(sequence);
    auto project = Project::create({{1},
                                    "project",
                                    kFirstNote.value + kNoteCount + 1,
                                    kSequence,
                                    {},
                                    {std::move(sequence).value()},
                                    {},
                                    {},
                                    std::nullopt});
    REQUIRE(project);
    return std::move(project).value();
}

std::vector<NoteEvent> clip_notes(const Project& project) {
    const auto& content =
        project.find_sequence(kSequence)->find_track(kTrack)->find_clip(kClip)->content();
    const auto notes = std::get<MidiContent>(content).notes();
    return {notes.begin(), notes.end()};
}

/// One drag step: raise the last note's pitch, gated on the notes the clip
/// currently holds. Chaining the gate is what makes each step a legal successor
/// of the one before it.
ReplaceNoteContent raise_last_note(std::vector<NoteEvent> expected) {
    auto replacement = expected;
    replacement.back().pitch = static_cast<std::uint8_t>(replacement.back().pitch + 1);
    return ReplaceNoteContent{kSequence, kTrack, kClip, std::move(expected),
                              std::move(replacement)};
}

/// Prices one step the way a session prices it. The inverse is the swap the note
/// reducer emits, so an editor can build the pair without committing anything.
UndoGestureBudget budget_for(const UndoLimits& limits, std::vector<NoteEvent> expected) {
    const auto forward = raise_last_note(std::move(expected));
    const ReplaceNoteContent inverse{forward.sequence_id, forward.track_id, forward.clip_id,
                                     forward.replacement, forward.expected};
    const std::array<Command, 1> forward_commands{Command{forward}};
    const std::array<Command, 1> inverse_commands{Command{inverse}};
    return undo_gesture_budget(limits, forward_commands, inverse_commands);
}

/// A session whose undo ceiling admits exactly `steps` steps over the starting
/// content, with the journal set wide enough that it never decides an outcome.
/// The journal binds independently and has no automatic eviction, so left at its
/// defaults it is what these cases would end up measuring.
SessionLimits budgeted_limits(std::size_t steps) {
    SessionLimits limits;
    // A step's charge is a property of its payload, not of the ceiling it is
    // measured against, so any limits value prices it — including the default
    // this then replaces.
    const auto step_bytes = budget_for(UndoLimits{}, notes_of(kNoteCount)).step_bytes;
    limits.undo.max_retained_bytes = steps * step_bytes;
    limits.journal.max_retained_bytes = 256 * 1024 * 1024;
    limits.journal.max_transactions = 4096;
    limits.journal.max_commands = 8192;
    return limits;
}

/// Runs `steps` coalescing steps of one open gesture, returning the conflict
/// that stopped it or nothing when every step committed.
std::optional<ConflictCode> run_gesture(DocumentSession& session, WriterToken& writer,
                                        std::size_t steps) {
    const auto group = writer.allocate_undo_group_id();
    for (std::size_t index = 0; index < steps; ++index) {
        auto tx = session_transaction(writer, session.revision(),
                                      {raise_last_note(clip_notes(*session.snapshot()))});
        tx.undo_group = group;
        tx.gesture_phase = index == 0 ? GesturePhase::Begin : GesturePhase::Update;
        INFO("gesture step " << index);
        auto committed = session.submit(writer, std::move(tx));
        if (!committed)
            return committed.error().code;
    }
    return std::nullopt;
}

std::uint8_t last_pitch(const DocumentSession& session) {
    return clip_notes(*session.snapshot()).back().pitch;
}

EditIntent move_clip_intent(GesturePhase phase, std::int64_t expected_start,
                            std::int64_t replacement_start) {
    EditIntent intent;
    intent.kind = EditIntentKind::Move;
    intent.phase = phase;
    intent.sequence_id = kSequence;
    intent.track_id = kTrack;
    intent.clip_id = kClip;
    intent.expected_range = MusicalTimeRange{{expected_start}, {kTicksPerQuarter}};
    intent.replacement_range = MusicalTimeRange{{replacement_start}, {kTicksPerQuarter}};
    return intent;
}

Transaction lower_issue(const EditGestureIdentityIssue& issue, EditIntent intent) {
    auto lowered = issue.lower(std::move(intent));
    REQUIRE(lowered);
    return std::move(lowered).value();
}

std::int64_t current_clip_start(const DocumentSession& session) {
    return clip(*session.snapshot()).start().value;
}

} // namespace

static_assert(!std::is_copy_constructible_v<EditGestureIdentityAllocator>);
static_assert(std::is_move_constructible_v<EditGestureIdentityAllocator>);
static_assert(!std::is_move_assignable_v<EditGestureIdentityAllocator>);
static_assert(!std::is_copy_constructible_v<EditGestureIdentityIssue>);
static_assert(std::is_move_constructible_v<EditGestureIdentityIssue>);
static_assert(!std::is_move_assignable_v<EditGestureIdentityIssue>);

template <typename Allocator>
concept HasCallerDeclaredGestureAcknowledgement = requires { &Allocator::acknowledge; };

static_assert(!HasCallerDeclaredGestureAcknowledgement<EditGestureIdentityAllocator>);

TEST_CASE("Gesture identity allocation validates lifecycle before consuming writer IDs") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto other_writer = std::move(session->register_writer()).value();
    WriterToken invalid_writer;

    auto created = EditGestureIdentityAllocator::create(writer);
    REQUIRE(created);
    auto allocator = std::move(created).value();
    REQUIRE(allocator.undo_group() == UndoGroupId{writer.id(), 1});
    REQUIRE(allocator.state() == EditGestureIdentityState::AwaitingBegin);
    REQUIRE_FALSE(allocator.has_pending_identity());

    auto invalid = allocator.issue(invalid_writer, {}, GesturePhase::Begin);
    REQUIRE_FALSE(invalid);
    REQUIRE(invalid.error() == EditGestureIdentityError::InvalidWriter);

    auto wrong = allocator.issue(other_writer, {}, GesturePhase::Begin);
    REQUIRE_FALSE(wrong);
    REQUIRE(wrong.error() == EditGestureIdentityError::WriterMismatch);
    REQUIRE(other_writer.allocate_transaction_id().sequence == 1);
    REQUIRE(other_writer.allocate_command_id().sequence == 1);

    for (const auto phase :
         {GesturePhase::Single, GesturePhase::Update, GesturePhase::End, GesturePhase::Cancel}) {
        auto out_of_order = allocator.issue(writer, {}, phase);
        REQUIRE_FALSE(out_of_order);
        REQUIRE(out_of_order.error() == EditGestureIdentityError::InvalidPhase);
    }

    auto first_begin = allocator.issue(writer, {7}, GesturePhase::Begin);
    REQUIRE(first_begin);
    const auto first_begin_tx =
        lower_issue(*first_begin, move_clip_intent(GesturePhase::Begin, 0, kTicksPerQuarter));
    REQUIRE(first_begin_tx.id == TransactionId{writer.id(), 1});
    REQUIRE(first_begin_tx.commands.front().id == CommandId{writer.id(), 1});
    REQUIRE(first_begin_tx.expected_revision == DocumentRevision{7});
    REQUIRE(first_begin_tx.undo_group == allocator.undo_group());
    REQUIRE(allocator.state() == EditGestureIdentityState::AwaitingBegin);
    REQUIRE(allocator.has_pending_identity());

    auto while_pending = allocator.issue(writer, {7}, GesturePhase::Begin);
    REQUIRE_FALSE(while_pending);
    REQUIRE(while_pending.error() == EditGestureIdentityError::IdentityPending);

    auto rejected =
        allocator.submit(*session, writer, std::move(*first_begin), first_begin_tx);
    REQUIRE(rejected);
    REQUIRE_FALSE(rejected->document_result);
    REQUIRE(rejected->document_result.error().code == ConflictCode::StaleRevision);
    REQUIRE(rejected->state == EditGestureIdentityState::AwaitingBegin);
    REQUIRE_FALSE(allocator.has_pending_identity());
    auto after_submit =
        first_begin->lower(move_clip_intent(GesturePhase::Begin, 0, kTicksPerQuarter));
    REQUIRE_FALSE(after_submit);

    auto stale = allocator.submit(*session, writer, std::move(*first_begin), first_begin_tx);
    REQUIRE_FALSE(stale);
    REQUIRE(stale.error() == EditGestureIdentityError::ForeignOrStaleIssue);

    auto retried_begin = allocator.issue(writer, session->revision(), GesturePhase::Begin);
    REQUIRE(retried_begin);
    auto retried_begin_tx =
        lower_issue(*retried_begin, move_clip_intent(GesturePhase::Begin, 0, kTicksPerQuarter));
    REQUIRE(retried_begin_tx.id == TransactionId{writer.id(), 2});
    REQUIRE(retried_begin_tx.commands.front().id == CommandId{writer.id(), 2});
    REQUIRE(retried_begin_tx.undo_group == allocator.undo_group());
    auto began = allocator.submit(*session, writer, std::move(*retried_begin),
                                  std::move(retried_begin_tx));
    REQUIRE(began);
    REQUIRE(began->document_result);
    REQUIRE(began->state == EditGestureIdentityState::Open);
    REQUIRE(allocator.state() == EditGestureIdentityState::Open);

    for (const auto phase : {GesturePhase::Single, GesturePhase::Begin}) {
        auto out_of_order = allocator.issue(writer, session->revision(), phase);
        REQUIRE_FALSE(out_of_order);
        REQUIRE(out_of_order.error() == EditGestureIdentityError::InvalidPhase);
    }

    auto update = allocator.issue(writer, session->revision(), GesturePhase::Update);
    REQUIRE(update);
    auto update_tx = lower_issue(
        *update, move_clip_intent(GesturePhase::Update, kTicksPerQuarter, 2 * kTicksPerQuarter));
    REQUIRE(update_tx.id == TransactionId{writer.id(), 3});
    REQUIRE(update_tx.commands.front().id == CommandId{writer.id(), 3});
    REQUIRE(update_tx.undo_group == allocator.undo_group());
    auto updated =
        allocator.submit(*session, writer, std::move(*update), std::move(update_tx));
    REQUIRE(updated);
    REQUIRE(updated->document_result);
    REQUIRE(updated->state == EditGestureIdentityState::Open);

    auto end = allocator.issue(writer, session->revision(), GesturePhase::End);
    REQUIRE(end);
    auto end_tx = lower_issue(
        *end, move_clip_intent(GesturePhase::End, 2 * kTicksPerQuarter, 3 * kTicksPerQuarter));
    REQUIRE(end_tx.id == TransactionId{writer.id(), 4});
    REQUIRE(end_tx.commands.front().id == CommandId{writer.id(), 4});
    REQUIRE(end_tx.undo_group == allocator.undo_group());
    auto ended = allocator.submit(*session, writer, std::move(*end), std::move(end_tx));
    REQUIRE(ended);
    REQUIRE(ended->document_result);
    REQUIRE(ended->state == EditGestureIdentityState::Closed);
    REQUIRE(allocator.state() == EditGestureIdentityState::Closed);

    auto after_close = allocator.issue(writer, session->revision(), GesturePhase::Update);
    REQUIRE_FALSE(after_close);
    REQUIRE(after_close.error() == EditGestureIdentityError::GestureClosed);
    REQUIRE(writer.allocate_transaction_id() == TransactionId{writer.id(), 5});
    REQUIRE(writer.allocate_command_id() == CommandId{writer.id(), 5});
    REQUIRE(writer.allocate_undo_group_id() == UndoGroupId{writer.id(), 2});
}

TEST_CASE("Gesture identity issues reject foreign tickets and survive moves while pending") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto first_created = EditGestureIdentityAllocator::create(writer);
    auto second_created = EditGestureIdentityAllocator::create(writer);
    REQUIRE(first_created);
    REQUIRE(second_created);
    auto first = std::move(first_created).value();
    auto second = std::move(second_created).value();

    auto first_issue = first.issue(writer, {}, GesturePhase::Begin);
    auto second_issue = second.issue(writer, {}, GesturePhase::Begin);
    REQUIRE(first_issue);
    REQUIRE(second_issue);
    const auto first_tx =
        lower_issue(*first_issue, move_clip_intent(GesturePhase::Begin, 0, kTicksPerQuarter));
    const auto second_tx =
        lower_issue(*second_issue, move_clip_intent(GesturePhase::Begin, 0, kTicksPerQuarter));
    REQUIRE(first_tx.undo_group != second_tx.undo_group);

    auto foreign = first.submit(*session, writer, std::move(*second_issue), second_tx);
    REQUIRE_FALSE(foreign);
    REQUIRE(foreign.error() == EditGestureIdentityError::ForeignOrStaleIssue);
    REQUIRE(first.has_pending_identity());
    REQUIRE(first.state() == EditGestureIdentityState::AwaitingBegin);

    auto moved_issue = std::move(*first_issue);
    auto moved_allocator = std::move(first);
    REQUIRE(first.state() == EditGestureIdentityState::Closed);
    auto from_moved_allocator = first.issue(writer, {}, GesturePhase::Begin);
    REQUIRE_FALSE(from_moved_allocator);
    REQUIRE(from_moved_allocator.error() == EditGestureIdentityError::GestureClosed);

    auto from_moved_issue =
        moved_allocator.submit(*session, writer, std::move(*first_issue), first_tx);
    REQUIRE_FALSE(from_moved_issue);
    REQUIRE(from_moved_issue.error() == EditGestureIdentityError::ForeignOrStaleIssue);
    REQUIRE(moved_allocator.has_pending_identity());
    auto first_submitted =
        moved_allocator.submit(*session, writer, std::move(moved_issue), first_tx);
    REQUIRE(first_submitted);
    REQUIRE(first_submitted->document_result);
    REQUIRE(moved_allocator.state() == EditGestureIdentityState::Open);
    auto second_submitted =
        second.submit(*session, writer, std::move(*second_issue), second_tx);
    REQUIRE(second_submitted);
    REQUIRE_FALSE(second_submitted->document_result);
    REQUIRE(second_submitted->state == EditGestureIdentityState::AwaitingBegin);
}

TEST_CASE("Gesture identity allocation follows transferred writer authority") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto original_writer = std::move(session->register_writer()).value();
    auto created = EditGestureIdentityAllocator::create(original_writer);
    REQUIRE(created);
    auto allocator = std::move(created).value();
    const auto provenance = original_writer.provenance();
    auto active_writer = std::move(original_writer);
    REQUIRE(active_writer.provenance() == provenance);
    REQUIRE_FALSE(original_writer.provenance().valid());

    auto from_moved_writer = allocator.issue(original_writer, {}, GesturePhase::Begin);
    REQUIRE_FALSE(from_moved_writer);
    REQUIRE(from_moved_writer.error() == EditGestureIdentityError::InvalidWriter);

    auto from_active_writer = allocator.issue(active_writer, {99}, GesturePhase::Begin);
    REQUIRE(from_active_writer);
    const auto transaction = lower_issue(
        *from_active_writer, move_clip_intent(GesturePhase::Begin, 0, kTicksPerQuarter));
    REQUIRE(transaction.id.writer == active_writer.id());
    auto submitted = allocator.submit(*session, active_writer, std::move(*from_active_writer),
                                      transaction);
    REQUIRE(submitted);
    REQUIRE_FALSE(submitted->document_result);
    REQUIRE(submitted->state == EditGestureIdentityState::AwaitingBegin);
}

TEST_CASE("Gesture submission rejects mutated issued transactions before touching the session",
          "[gesture-identity][submission-authority]") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto other_writer = std::move(session->register_writer()).value();
    auto created = EditGestureIdentityAllocator::create(writer);
    REQUIRE(created);
    auto allocator = std::move(created).value();
    auto issue = allocator.issue(writer, session->revision(), GesturePhase::Begin);
    REQUIRE(issue);
    auto transaction =
        lower_issue(*issue, move_clip_intent(GesturePhase::Begin, 0, kTicksPerQuarter));

    const auto require_refused = [&](Transaction changed,
                                     EditGestureIdentityError expected_error) {
        auto refused = allocator.submit(*session, writer, std::move(*issue), std::move(changed));
        REQUIRE_FALSE(refused);
        REQUIRE(refused.error() == expected_error);
        REQUIRE(session->revision() == DocumentRevision{});
        REQUIRE(allocator.state() == EditGestureIdentityState::AwaitingBegin);
        REQUIRE(allocator.has_pending_identity());
    };

    auto wrong_transaction = transaction;
    wrong_transaction.id.sequence += 1;
    require_refused(std::move(wrong_transaction),
                    EditGestureIdentityError::IssuedTransactionMismatch);

    auto wrong_revision = transaction;
    wrong_revision.expected_revision.value += 1;
    require_refused(std::move(wrong_revision),
                    EditGestureIdentityError::IssuedTransactionMismatch);

    auto wrong_group = transaction;
    wrong_group.undo_group = UndoGroupId{writer.id(), 99};
    require_refused(std::move(wrong_group), EditGestureIdentityError::IssuedTransactionMismatch);

    auto wrong_phase = transaction;
    wrong_phase.gesture_phase = GesturePhase::Single;
    require_refused(std::move(wrong_phase), EditGestureIdentityError::IssuedTransactionMismatch);

    auto wrong_command = transaction;
    wrong_command.commands.front().id.sequence += 1;
    require_refused(std::move(wrong_command), EditGestureIdentityError::IssuedTransactionMismatch);

    auto extra_command = transaction;
    extra_command.commands.push_back(extra_command.commands.front());
    require_refused(std::move(extra_command), EditGestureIdentityError::IssuedTransactionMismatch);

    auto wrong_writer = allocator.submit(*session, other_writer, std::move(*issue), transaction);
    REQUIRE_FALSE(wrong_writer);
    REQUIRE(wrong_writer.error() == EditGestureIdentityError::WriterMismatch);
    REQUIRE(allocator.has_pending_identity());
    REQUIRE(session->revision() == DocumentRevision{});

    auto submitted =
        allocator.submit(*session, writer, std::move(*issue), std::move(transaction));
    REQUIRE(submitted);
    REQUIRE(submitted->document_result);
    REQUIRE(submitted->state == EditGestureIdentityState::Open);
    REQUIRE_FALSE(allocator.has_pending_identity());
}

TEST_CASE("Gesture submission reconciles an exact direct commit retry",
          "[gesture-identity][submission-authority][submission-committed]") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto created = EditGestureIdentityAllocator::create(writer);
    REQUIRE(created);
    auto allocator = std::move(created).value();
    auto issue = allocator.issue(writer, session->revision(), GesturePhase::Begin);
    REQUIRE(issue);
    auto transaction =
        lower_issue(*issue, move_clip_intent(GesturePhase::Begin, 0, kTicksPerQuarter));
    const auto command = transaction.commands.front().id;

    auto direct = session->submit(writer, transaction);
    REQUIRE(direct);
    REQUIRE(allocator.state() == EditGestureIdentityState::AwaitingBegin);
    REQUIRE(allocator.has_pending_identity());

    auto reconciled =
        allocator.submit(*session, writer, std::move(*issue), std::move(transaction));
    REQUIRE(reconciled);
    REQUIRE(reconciled->document_result);
    REQUIRE(reconciled->document_result->revision == direct->revision);
    REQUIRE(reconciled->document_result->applied_commands == std::vector<CommandId>{command});
    REQUIRE(reconciled->state == EditGestureIdentityState::Open);
    REQUIRE(allocator.state() == EditGestureIdentityState::Open);
}

TEST_CASE("Gesture submission survives interleaved writer ID consumption",
          "[gesture-identity][submission-authority][submission-rejected]") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto created = EditGestureIdentityAllocator::create(writer);
    REQUIRE(created);
    auto allocator = std::move(created).value();
    const auto group = allocator.undo_group();

    auto issue = allocator.issue(writer, session->revision(), GesturePhase::Begin);
    REQUIRE(issue);
    auto issued_transaction =
        lower_issue(*issue, move_clip_intent(GesturePhase::Begin, 0, kTicksPerQuarter));
    REQUIRE(issued_transaction.id == TransactionId{writer.id(), 1});
    REQUIRE(issued_transaction.commands.front().id == CommandId{writer.id(), 1});

    EditIntentIdentity independent_identity{writer.allocate_transaction_id(), session->revision(),
                                            writer.allocate_command_id(), std::nullopt};
    auto independent = lower_edit_intent(
        move_clip_intent(GesturePhase::Single, 0, kTicksPerQuarter), independent_identity);
    REQUIRE(independent);
    REQUIRE(independent->id == TransactionId{writer.id(), 2});
    REQUIRE(independent->commands.front().id == CommandId{writer.id(), 2});
    REQUIRE(session->submit(writer, std::move(*independent)));

    auto rejected = allocator.submit(*session, writer, std::move(*issue),
                                     std::move(issued_transaction));
    REQUIRE(rejected);
    REQUIRE_FALSE(rejected->document_result);
    REQUIRE(rejected->document_result.error().code ==
            ConflictCode::AlreadyAppliedResultExpired);
    REQUIRE(rejected->state == EditGestureIdentityState::AwaitingBegin);
    REQUIRE_FALSE(allocator.has_pending_identity());

    auto retry = allocator.issue(writer, session->revision(), GesturePhase::Begin);
    REQUIRE(retry);
    auto retry_transaction = lower_issue(
        *retry, move_clip_intent(GesturePhase::Begin, kTicksPerQuarter,
                                2 * kTicksPerQuarter));
    REQUIRE(retry_transaction.id == TransactionId{writer.id(), 3});
    REQUIRE(retry_transaction.commands.front().id == CommandId{writer.id(), 3});
    REQUIRE(retry_transaction.undo_group == group);
    auto retried =
        allocator.submit(*session, writer, std::move(*retry), std::move(retry_transaction));
    REQUIRE(retried);
    REQUIRE(retried->document_result);
    REQUIRE(retried->state == EditGestureIdentityState::Open);
}

TEST_CASE("Gesture identity provenance rejects equal numeric streams from another session",
          "[gesture-identity][session-provenance]") {
    SECTION("a foreign writer is rejected before its ID streams advance") {
        auto first_session = std::move(DocumentSession::create(make_project())).value();
        auto second_session = std::move(DocumentSession::create(make_project())).value();
        auto first_writer = std::move(first_session->register_writer()).value();
        auto second_writer = std::move(second_session->register_writer()).value();
        REQUIRE(first_writer.id() == second_writer.id());
        REQUIRE(first_writer.provenance() != second_writer.provenance());

        auto created = EditGestureIdentityAllocator::create(first_writer);
        REQUIRE(created);
        auto allocator = std::move(created).value();
        auto foreign = allocator.issue(second_writer, {}, GesturePhase::Begin);
        REQUIRE_FALSE(foreign);
        REQUIRE(foreign.error() == EditGestureIdentityError::WriterMismatch);
        REQUIRE(second_writer.allocate_transaction_id() == TransactionId{second_writer.id(), 1});
        REQUIRE(second_writer.allocate_command_id() == CommandId{second_writer.id(), 1});
        REQUIRE(second_writer.allocate_undo_group_id() == UndoGroupId{second_writer.id(), 1});
    }

    SECTION("a value-identical issue from another session cannot submit") {
        auto first_session = std::move(DocumentSession::create(make_project())).value();
        auto second_session = std::move(DocumentSession::create(make_project())).value();
        auto first_writer = std::move(first_session->register_writer()).value();
        auto second_writer = std::move(second_session->register_writer()).value();
        REQUIRE(first_writer.id() == second_writer.id());
        REQUIRE(first_writer.provenance() != second_writer.provenance());

        auto first_created = EditGestureIdentityAllocator::create(first_writer);
        auto second_created = EditGestureIdentityAllocator::create(second_writer);
        REQUIRE(first_created);
        REQUIRE(second_created);
        auto first = std::move(first_created).value();
        auto second = std::move(second_created).value();
        REQUIRE(first.undo_group() == second.undo_group());

        auto first_issue = first.issue(first_writer, {}, GesturePhase::Begin);
        auto second_issue = second.issue(second_writer, {}, GesturePhase::Begin);
        REQUIRE(first_issue);
        REQUIRE(second_issue);
        const auto first_tx =
            lower_issue(*first_issue, move_clip_intent(GesturePhase::Begin, 0, kTicksPerQuarter));
        const auto second_tx =
            lower_issue(*second_issue, move_clip_intent(GesturePhase::Begin, 0, kTicksPerQuarter));
        REQUIRE(first_tx.id == second_tx.id);
        REQUIRE(first_tx.expected_revision == second_tx.expected_revision);
        REQUIRE(first_tx.undo_group == second_tx.undo_group);
        REQUIRE(first_tx.commands.front().id == second_tx.commands.front().id);
        REQUIRE(first_tx.gesture_phase == second_tx.gesture_phase);

        auto foreign =
            first.submit(*first_session, first_writer, std::move(*second_issue), second_tx);
        REQUIRE_FALSE(foreign);
        REQUIRE(foreign.error() == EditGestureIdentityError::ForeignOrStaleIssue);
        REQUIRE(first.has_pending_identity());
        REQUIRE(second.has_pending_identity());
        auto first_submitted =
            first.submit(*first_session, first_writer, std::move(*first_issue), first_tx);
        auto second_submitted =
            second.submit(*second_session, second_writer, std::move(*second_issue), second_tx);
        REQUIRE(first_submitted);
        REQUIRE(first_submitted->document_result);
        REQUIRE(second_submitted);
        REQUIRE(second_submitted->document_result);
    }

    SECTION("a matching issue submitted to another session is authoritatively rejected") {
        auto first_session = std::move(DocumentSession::create(make_project())).value();
        auto second_session = std::move(DocumentSession::create(make_project())).value();
        auto first_writer = std::move(first_session->register_writer()).value();
        auto second_writer = std::move(second_session->register_writer()).value();
        REQUIRE(first_writer.id() == second_writer.id());

        auto created = EditGestureIdentityAllocator::create(first_writer);
        REQUIRE(created);
        auto allocator = std::move(created).value();
        auto issue = allocator.issue(first_writer, first_session->revision(), GesturePhase::Begin);
        REQUIRE(issue);
        auto transaction =
            lower_issue(*issue, move_clip_intent(GesturePhase::Begin, 0, kTicksPerQuarter));
        auto submitted = allocator.submit(*second_session, first_writer, std::move(*issue),
                                          std::move(transaction));
        REQUIRE(submitted);
        REQUIRE_FALSE(submitted->document_result);
        REQUIRE(submitted->document_result.error().code == ConflictCode::InvalidIdentifier);
        REQUIRE(submitted->state == EditGestureIdentityState::AwaitingBegin);
        REQUIRE_FALSE(allocator.has_pending_identity());
    }
}

TEST_CASE("Gesture issue lowering makes Begin phase authoritative",
          "[gesture-identity][phase-authority]") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto created = EditGestureIdentityAllocator::create(writer);
    REQUIRE(created);
    auto allocator = std::move(created).value();

    auto begin = allocator.issue(writer, session->revision(), GesturePhase::Begin);
    REQUIRE(begin);
    const auto supplied = move_clip_intent(GesturePhase::Single, 0, kTicksPerQuarter);
    auto begin_tx = lower_issue(*begin, supplied);
    REQUIRE(supplied.phase == GesturePhase::Single);
    REQUIRE(begin_tx.gesture_phase == GesturePhase::Begin);
    auto began = allocator.submit(*session, writer, std::move(*begin), std::move(begin_tx));
    REQUIRE(began);
    REQUIRE(began->document_result);
    REQUIRE(allocator.state() == EditGestureIdentityState::Open);

    auto end = allocator.issue(writer, session->revision(), GesturePhase::End);
    REQUIRE(end);
    auto end_tx = lower_issue(
        *end, move_clip_intent(GesturePhase::End, kTicksPerQuarter, 2 * kTicksPerQuarter));
    auto ended = allocator.submit(*session, writer, std::move(*end), std::move(end_tx));
    REQUIRE(ended);
    REQUIRE(ended->document_result);
    REQUIRE(session->undo(writer));
    REQUIRE(current_clip_start(*session) == 0);
}

TEST_CASE("Gesture issue lowering makes End phase authoritative",
          "[gesture-identity][phase-authority]") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto created = EditGestureIdentityAllocator::create(writer);
    REQUIRE(created);
    auto allocator = std::move(created).value();

    auto begin = allocator.issue(writer, session->revision(), GesturePhase::Begin);
    REQUIRE(begin);
    auto begin_tx = lower_issue(*begin, move_clip_intent(GesturePhase::Begin, 0, kTicksPerQuarter));
    auto began = allocator.submit(*session, writer, std::move(*begin), std::move(begin_tx));
    REQUIRE(began);
    REQUIRE(began->document_result);

    auto end = allocator.issue(writer, session->revision(), GesturePhase::End);
    REQUIRE(end);
    const auto supplied =
        move_clip_intent(GesturePhase::Update, kTicksPerQuarter, 2 * kTicksPerQuarter);
    auto end_tx = lower_issue(*end, supplied);
    REQUIRE(supplied.phase == GesturePhase::Update);
    REQUIRE(end_tx.gesture_phase == GesturePhase::End);
    auto ended = allocator.submit(*session, writer, std::move(*end), std::move(end_tx));
    REQUIRE(ended);
    REQUIRE(ended->document_result);
    REQUIRE(allocator.state() == EditGestureIdentityState::Closed);
    REQUIRE(session->undo(writer));
    REQUIRE(current_clip_start(*session) == 0);
}

TEST_CASE("Rejected begin and end submissions retry with fresh IDs in one undo group") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto created = EditGestureIdentityAllocator::create(writer);
    REQUIRE(created);
    auto allocator = std::move(created).value();
    const auto group = allocator.undo_group();

    auto stale_begin = allocator.issue(writer, {99}, GesturePhase::Begin);
    REQUIRE(stale_begin);
    auto stale_tx =
        lower_issue(*stale_begin, move_clip_intent(GesturePhase::Begin, 0, kTicksPerQuarter));
    const auto stale_transaction_sequence = stale_tx.id.sequence;
    const auto stale_command_sequence = stale_tx.commands.front().id.sequence;
    auto stale_result =
        allocator.submit(*session, writer, std::move(*stale_begin), std::move(stale_tx));
    REQUIRE(stale_result);
    REQUIRE_FALSE(stale_result->document_result);
    REQUIRE(stale_result->document_result.error().code == ConflictCode::StaleRevision);
    REQUIRE(stale_result->state == EditGestureIdentityState::AwaitingBegin);
    REQUIRE(allocator.state() == EditGestureIdentityState::AwaitingBegin);

    auto begin = allocator.issue(writer, session->revision(), GesturePhase::Begin);
    REQUIRE(begin);
    auto begin_tx = lower_issue(*begin, move_clip_intent(GesturePhase::Begin, 0, kTicksPerQuarter));
    REQUIRE(begin_tx.id.sequence > stale_transaction_sequence);
    REQUIRE(begin_tx.commands.front().id.sequence > stale_command_sequence);
    REQUIRE(begin_tx.undo_group == group);
    auto begin_result =
        allocator.submit(*session, writer, std::move(*begin), std::move(begin_tx));
    REQUIRE(begin_result);
    REQUIRE(begin_result->document_result);
    REQUIRE(allocator.state() == EditGestureIdentityState::Open);
    REQUIRE(current_clip_start(*session) == kTicksPerQuarter);

    auto rejected_end = allocator.issue(writer, session->revision(), GesturePhase::End);
    REQUIRE(rejected_end);
    auto rejected_end_tx =
        lower_issue(*rejected_end, move_clip_intent(GesturePhase::End, 0, 2 * kTicksPerQuarter));
    const auto rejected_transaction_sequence = rejected_end_tx.id.sequence;
    const auto rejected_command_sequence = rejected_end_tx.commands.front().id.sequence;
    auto rejected_result = allocator.submit(*session, writer, std::move(*rejected_end),
                                            std::move(rejected_end_tx));
    REQUIRE(rejected_result);
    REQUIRE_FALSE(rejected_result->document_result);
    REQUIRE(rejected_result->document_result.error().code ==
            ConflictCode::ExpectedValueMismatch);
    REQUIRE(rejected_result->state == EditGestureIdentityState::Open);
    REQUIRE(allocator.state() == EditGestureIdentityState::Open);
    REQUIRE(current_clip_start(*session) == kTicksPerQuarter);

    auto end = allocator.issue(writer, session->revision(), GesturePhase::End);
    REQUIRE(end);
    auto end_tx = lower_issue(
        *end, move_clip_intent(GesturePhase::End, kTicksPerQuarter, 2 * kTicksPerQuarter));
    REQUIRE(end_tx.id.sequence > rejected_transaction_sequence);
    REQUIRE(end_tx.commands.front().id.sequence > rejected_command_sequence);
    REQUIRE(end_tx.undo_group == group);
    auto end_result = allocator.submit(*session, writer, std::move(*end), std::move(end_tx));
    REQUIRE(end_result);
    REQUIRE(end_result->document_result);
    REQUIRE(allocator.state() == EditGestureIdentityState::Closed);
    REQUIRE(current_clip_start(*session) == 2 * kTicksPerQuarter);

    REQUIRE(session->undo(writer));
    REQUIRE(current_clip_start(*session) == 0);
}

TEST_CASE("Rejected cancel submission stays retryable and undo reverts its gesture") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto created = EditGestureIdentityAllocator::create(writer);
    REQUIRE(created);
    auto allocator = std::move(created).value();
    const auto group = allocator.undo_group();

    auto begin = allocator.issue(writer, session->revision(), GesturePhase::Begin);
    REQUIRE(begin);
    auto begin_tx =
        lower_issue(*begin, move_clip_intent(GesturePhase::Begin, 0, kTicksPerQuarter));
    auto begin_result =
        allocator.submit(*session, writer, std::move(*begin), std::move(begin_tx));
    REQUIRE(begin_result);
    REQUIRE(begin_result->document_result);

    auto rejected_cancel = allocator.issue(writer, session->revision(), GesturePhase::Cancel);
    REQUIRE(rejected_cancel);
    auto rejected_cancel_tx = lower_issue(
        *rejected_cancel, move_clip_intent(GesturePhase::Cancel, 0, 2 * kTicksPerQuarter));
    const auto rejected_transaction_sequence = rejected_cancel_tx.id.sequence;
    const auto rejected_command_sequence = rejected_cancel_tx.commands.front().id.sequence;
    auto rejected_result = allocator.submit(*session, writer, std::move(*rejected_cancel),
                                            std::move(rejected_cancel_tx));
    REQUIRE(rejected_result);
    REQUIRE_FALSE(rejected_result->document_result);
    REQUIRE(rejected_result->document_result.error().code ==
            ConflictCode::ExpectedValueMismatch);
    REQUIRE(rejected_result->state == EditGestureIdentityState::Open);
    REQUIRE(allocator.state() == EditGestureIdentityState::Open);

    auto cancel = allocator.issue(writer, session->revision(), GesturePhase::Cancel);
    REQUIRE(cancel);
    auto cancel_tx = lower_issue(
        *cancel, move_clip_intent(GesturePhase::Cancel, kTicksPerQuarter, 2 * kTicksPerQuarter));
    REQUIRE(cancel_tx.id.sequence > rejected_transaction_sequence);
    REQUIRE(cancel_tx.commands.front().id.sequence > rejected_command_sequence);
    REQUIRE(cancel_tx.undo_group == group);
    auto cancel_result =
        allocator.submit(*session, writer, std::move(*cancel), std::move(cancel_tx));
    REQUIRE(cancel_result);
    REQUIRE(cancel_result->document_result);
    REQUIRE(allocator.state() == EditGestureIdentityState::Closed);
    REQUIRE(current_clip_start(*session) == 2 * kTicksPerQuarter);

    REQUIRE(session->can_undo());
    REQUIRE(session->undo(writer));
    REQUIRE(current_clip_start(*session) == 0);
}

TEST_CASE("Gesture identity exhaustion fails closed without reusing partial IDs") {
    WriterToken invalid_writer;
    auto invalid = EditGestureIdentityAllocator::create(invalid_writer);
    REQUIRE_FALSE(invalid);
    REQUIRE(invalid.error() == EditGestureIdentityError::InvalidWriter);

    constexpr auto exhausted = std::numeric_limits<std::uint64_t>::max();

    auto group_session = std::move(DocumentSession::create(make_project())).value();
    auto group_writer = std::move(group_session->register_writer()).value();
    pulp::timeline::detail::WriterTokenTestAccess::set_next_ids(group_writer, 1, 1, exhausted);
    auto no_group = EditGestureIdentityAllocator::create(group_writer);
    REQUIRE_FALSE(no_group);
    REQUIRE(no_group.error() == EditGestureIdentityError::IdentityExhausted);
    REQUIRE(group_writer.allocate_transaction_id().sequence == 1);
    REQUIRE(group_writer.allocate_command_id().sequence == 1);

    auto transaction_session = std::move(DocumentSession::create(make_project())).value();
    auto transaction_writer = std::move(transaction_session->register_writer()).value();
    pulp::timeline::detail::WriterTokenTestAccess::set_next_ids(transaction_writer, exhausted, 1,
                                                                1);
    auto transaction_created = EditGestureIdentityAllocator::create(transaction_writer);
    REQUIRE(transaction_created);
    auto transaction_allocator = std::move(transaction_created).value();
    auto no_transaction = transaction_allocator.issue(transaction_writer, {}, GesturePhase::Begin);
    REQUIRE_FALSE(no_transaction);
    REQUIRE(no_transaction.error() == EditGestureIdentityError::IdentityExhausted);
    REQUIRE(transaction_allocator.state() == EditGestureIdentityState::Exhausted);
    REQUIRE(transaction_writer.allocate_command_id().sequence == 1);
    REQUIRE(transaction_writer.allocate_undo_group_id().sequence == 2);

    auto command_session = std::move(DocumentSession::create(make_project())).value();
    auto command_writer = std::move(command_session->register_writer()).value();
    pulp::timeline::detail::WriterTokenTestAccess::set_next_ids(command_writer, 1, exhausted, 1);
    auto command_created = EditGestureIdentityAllocator::create(command_writer);
    REQUIRE(command_created);
    auto command_allocator = std::move(command_created).value();
    auto no_command = command_allocator.issue(command_writer, {}, GesturePhase::Begin);
    REQUIRE_FALSE(no_command);
    REQUIRE(no_command.error() == EditGestureIdentityError::IdentityExhausted);
    REQUIRE(command_allocator.state() == EditGestureIdentityState::Exhausted);
    REQUIRE(command_writer.allocate_transaction_id().sequence == 2);
    REQUIRE(command_writer.allocate_undo_group_id().sequence == 2);
}

// The charge must be a function of the payload rather than of the command count.
// A step carrying 100 more notes pays for them in BOTH halves the session
// accounts for — the forward command and the inverse — and each half carries the
// note array twice, once as the expected value and once as the replacement. So
// the growth is four arrays' worth, asserted as a value: `retained_size`'s
// dispatch ends in a bare `sizeof(T)`, and an arm that stopped counting the
// vectors would still return a plausible nonzero size.
TEST_CASE("A gesture step is priced by its payload rather than its command count") {
    const UndoLimits limits;
    const auto small = budget_for(limits, notes_of(kNoteCount));
    const auto large = budget_for(limits, notes_of(kNoteCount + kGrowthNotes));

    REQUIRE(large.step_bytes - small.step_bytes == 4 * kGrowthNotes * sizeof(NoteEvent));
    REQUIRE(small.step_bytes >= 4 * kNoteCount * sizeof(NoteEvent));
    // A heavier step buys fewer of them out of one budget.
    REQUIRE(large.steps < small.steps);
}

// The two cases below are the pair. They run the SAME command at the SAME size
// against the SAME budget and differ only in how many steps the gesture takes,
// and they must reach OPPOSITE outcomes at exactly the step the budget predicts.
//
// Either alone proves nothing. "Every step committed" passes against a session
// with no budget at all; "the last step was refused" passes against a budget so
// small the gesture never starts.
//
// The pair alone is not enough either, which is why both run at TWO budget
// sizes. Each budget is built from its own step count, so a prediction that
// ignored its inputs and returned that literal would sit exactly on the boundary
// and satisfy both halves. Two sizes leave a constant failing at whichever one
// it is not: a pair pins a boundary, and only a boundary that moves pins the
// arithmetic that places it.

TEST_CASE("An open gesture commits exactly as many steps as the undo budget predicts") {
    const auto initial = notes_of(kNoteCount);
    for (const auto sized : kAffordableSteps) {
        INFO("budget sized for " << sized << " steps");
        const auto limits = budgeted_limits(sized);
        const auto budget = budget_for(limits.undo, initial);
        REQUIRE(budget.steps == sized);

        auto session = std::move(DocumentSession::create(make_note_project(), limits)).value();
        auto writer = std::move(session->register_writer()).value();

        REQUIRE(run_gesture(*session, writer, budget.steps) == std::nullopt);
        // Asserted as values: every step raised the same note one further
        // semitone, which a run that accepted submissions and applied none would
        // not show.
        REQUIRE(clip_notes(*session->snapshot()).size() == kNoteCount);
        REQUIRE(last_pitch(*session) == initial.back().pitch + budget.steps);
    }
}

TEST_CASE("An open gesture is refused the step past its undo budget") {
    const auto initial = notes_of(kNoteCount);
    for (const auto sized : kAffordableSteps) {
        INFO("budget sized for " << sized << " steps");
        const auto limits = budgeted_limits(sized);
        const auto budget = budget_for(limits.undo, initial);
        REQUIRE(budget.steps == sized);

        auto session = std::move(DocumentSession::create(make_note_project(), limits)).value();
        auto writer = std::move(session->register_writer()).value();

        // Refused for the budget, and named as such: a gesture that died of a
        // stale revision or a malformed bracket would stop at this step too.
        REQUIRE(run_gesture(*session, writer, budget.steps + 1) == ConflictCode::UndoFull);
        // One step is refused rather than the gesture discarded, which is what
        // makes the boundary a property of the budget rather than of the run.
        REQUIRE(last_pitch(*session) == initial.back().pitch + budget.steps);
    }
}

// Zero steps is a distinct answer from a small number of them, and it is the one
// an editor most needs before it starts: a gesture whose first step alone
// exceeds the budget cannot be opened, because an open group has nothing to
// evict to make room for itself. The prediction and the session must agree at
// this end of the range too — the Begin itself is what gets refused.
TEST_CASE("A step larger than the whole budget predicts no gesture at all") {
    const auto initial = notes_of(kNoteCount);
    auto limits = budgeted_limits(1);
    limits.undo.max_retained_bytes -= 1;

    REQUIRE(budget_for(limits.undo, initial).steps == 0);

    auto session = std::move(DocumentSession::create(make_note_project(), limits)).value();
    auto writer = std::move(session->register_writer()).value();
    REQUIRE(run_gesture(*session, writer, 1) == ConflictCode::UndoFull);
    // Refused rather than partially applied: the document is untouched.
    REQUIRE(last_pitch(*session) == initial.back().pitch);
}

// The other end of the range. A step carrying no commands charges nothing, so
// the undo budget never refuses one — reported as the saturating value rather
// than as a division by zero. No real command reaches this, since every
// retained_size arm includes its own sizeof, but an empty step does.
TEST_CASE("A step that charges nothing is never refused by the undo budget") {
    const UndoLimits limits;
    const auto budget = undo_gesture_budget(limits, {}, {});
    REQUIRE(budget.step_bytes == 0);
    REQUIRE(budget.steps == std::numeric_limits<std::size_t>::max());
}

// Which ceiling binds is a property of the payload, not a fixed answer, and this
// function reports only one of the two. The journal's flat `max_transactions`
// cap is the axis that can bind before undo, since a step is one transaction
// however small it is; its byte ceiling cannot, being twice the budget against
// roughly half the per-step charge.
//
// A pair again, and it has to be: "undo binds" asserted alone passes for a
// function that always returns a small number, and "the journal binds" alone
// passes for one that always returns a large one. Two payload sizes, opposite
// sides of the crossover.
TEST_CASE("Which ceiling binds a gesture depends on the size of its payload") {
    const UndoLimits limits;
    const JournalLimits journal;

    // A clip dense enough that a whole-content edit is expensive: the undo
    // budget runs out first, so the predicted count is the real ceiling.
    REQUIRE(budget_for(limits, notes_of(kNoteCount)).steps < journal.max_transactions);

    // A sparse clip: the same command is cheap enough that undo would allow far
    // more steps than the journal will retain transactions, so the flat cap
    // binds and this prediction is optimistic. That is the boundary the header
    // documents rather than a defect.
    REQUIRE(budget_for(limits, notes_of(kSparseNoteCount)).steps > journal.max_transactions);
}

// The ceiling belongs to the OPEN group and not to the command. The same payload
// against the same budget, submitted as closed single-phase edits, runs well
// past the step the gesture above died at, because a closed group is evictable
// and an open one is not. This is the fallback an editor takes when the budget
// is too small to stream, so it has to be shown to work rather than assumed.
TEST_CASE("Closed single-phase edits outrun the budget an open gesture dies at") {
    const auto initial = notes_of(kNoteCount);
    const auto limits = budgeted_limits(kAffordableSteps.front());
    const auto budget = budget_for(limits.undo, initial);

    auto session = std::move(DocumentSession::create(make_note_project(), limits)).value();
    auto writer = std::move(session->register_writer()).value();

    const std::size_t edits = budget.steps * 10;
    for (std::size_t index = 0; index < edits; ++index) {
        auto tx = session_transaction(writer, session->revision(),
                                      {raise_last_note(clip_notes(*session->snapshot()))});
        tx.gesture_phase = GesturePhase::Single;
        INFO("single edit " << index);
        REQUIRE(session->submit(writer, std::move(tx)));
    }

    REQUIRE(last_pitch(*session) == initial.back().pitch + edits);
}
