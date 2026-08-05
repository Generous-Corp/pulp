#include "timeline_command_test_helpers.hpp"

#include <pulp/timeline_editor/gesture_budget.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <optional>
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

} // namespace

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
