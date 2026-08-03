#include "timeline_command_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace timeline_test;

TEST_CASE("Tempo and meter commands path-copy atomically and produce exact inverses") {
    const auto original = make_project();
    const auto tempo = make_tempo_map(90.0);
    const auto meter = make_meter_map({7, 8});
    auto edit = transaction({1}, 1, 1, {},
                            {SetTempoMap{original.tempo_map(), tempo},
                             SetMeterMap{original.meter_map(), meter}});
    auto changed = reduce_transaction(original, edit);
    REQUIRE(changed);
    REQUIRE(changed->project.tempo_map() == tempo);
    REQUIRE(changed->project.meter_map() == meter);
    REQUIRE(original.tempo_map() == TempoMap{});
    REQUIRE(original.meter_map() == MeterMap{});
    REQUIRE(changed->dirty.items().size() == 1);
    REQUIRE(changed->dirty.items()[0].item == original.id());
    REQUIRE(changed->dirty.items()[0].flags == DirtyFlags::Timing);

    auto inverse = transaction({1}, 2, 3, {}, changed->inverses);
    auto restored = reduce_transaction(changed->project, inverse);
    REQUIRE(restored);
    REQUIRE(restored->project.tempo_map() == original.tempo_map());
    REQUIRE(restored->project.meter_map() == original.meter_map());

    auto atomic_failure = transaction(
        {1}, 3, 5, {},
        {SetTempoMap{original.tempo_map(), tempo},
         SetMeterMap{meter, make_meter_map({3, 4})}});
    auto rejected = reduce_transaction(original, atomic_failure);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::ExpectedValueMismatch);
    REQUIRE(original.tempo_map() == TempoMap{});
    REQUIRE(original.meter_map() == MeterMap{});
}

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

namespace {

NoteEvent modifier_note(std::uint64_t id, std::int64_t start, std::uint8_t pitch) {
    return {{id}, {start}, {kTicksPerQuarter / 4}, 1000, pitch, 0};
}

NoteModifier chance(std::uint64_t note, std::uint16_t probability) {
    NoteModifier modifier;
    modifier.note_id = {note};
    modifier.probability = probability;
    return modifier;
}

// Two streams on different channels, so a lane is found by its address rather
// than by an index the canonical ordering happens to produce.
constexpr MidiLaneAddress kModulation{0, 0, 11, 0, 1};
constexpr MidiLaneAddress kExpression{0, 1, 11, 0, 74};

// A one-track project whose only clip carries everything a MIDI clip authors
// beside its notes: a modifier on each note, a non-zero seed, and two
// controller lanes.
Project make_authored_clip_project() {
    auto notes = MidiContent::create(
        {modifier_note(6, 0, 60), modifier_note(7, kTicksPerQuarter / 2, 64)},
        {chance(6, 1024), chance(7, 4096)}, 0xABCDEF,
        {MidiExpressionLane{{9}, kModulation, {{{10}, {0}, 0x40000000}}},
         MidiExpressionLane{{11}, kExpression, {{{12}, {kTicksPerQuarter / 2}, 0x80000000}}}});
    assert(notes);
    auto value = Clip::create({5}, {0}, {kTicksPerQuarter}, std::move(notes).value());
    assert(value);
    auto track = Track::create({4}, "track", {std::move(value).value()});
    assert(track);
    auto sequence = Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter},
                                     {std::move(track).value()});
    assert(sequence);
    auto project = Project::create({{1}, "project", 20, {3}, {}, {std::move(sequence).value()}});
    assert(project);
    return std::move(project).value();
}

const MidiContent& midi_content(const Project& project) {
    return std::get<MidiContent>(clip(project).content());
}

// Lanes address a controller stream and name no note, so every edit below
// leaves both of them whole no matter which notes it keeps.
void check_lanes_intact(const MidiContent& content) {
    REQUIRE(content.lanes().size() == 2);
    const auto* modulation = content.lane_for(kModulation);
    REQUIRE(modulation != nullptr);
    REQUIRE(modulation->points.size() == 1);
    CHECK(modulation->points[0].id == ItemId{10});
    CHECK(modulation->points[0].value == 0x40000000);
    const auto* expression = content.lane_for(kExpression);
    REQUIRE(expression != nullptr);
    REQUIRE(expression->points.size() == 1);
    CHECK(expression->points[0].id == ItemId{12});
    CHECK(expression->points[0].value == 0x80000000);
}

} // namespace

TEST_CASE("ReplaceNoteContent carries the modifiers, seed, and lanes of the notes it retains") {
    const auto original = make_authored_clip_project();
    // The fixture builds under NDEBUG, where its asserts vanish: state what the
    // clip carries before the edit so the checks below cannot pass vacuously.
    REQUIRE(midi_content(original).modifiers().size() == 2);
    REQUIRE(midi_content(original).modifier_seed() == 0xABCDEF);
    check_lanes_intact(midi_content(original));

    const std::vector<NoteEvent> expected{modifier_note(6, 0, 60),
                                          modifier_note(7, kTicksPerQuarter / 2, 64)};
    const std::vector<NoteEvent> replacement{modifier_note(6, 0, 67),
                                             modifier_note(7, kTicksPerQuarter / 2, 64)};

    auto edit =
        transaction({1}, 1, 1, {}, {ReplaceNoteContent{{3}, {4}, {5}, expected, replacement}});
    auto changed = reduce_transaction(original, edit);
    REQUIRE(changed);

    const auto& after = midi_content(changed->project);
    REQUIRE(after.notes().size() == 2);
    CHECK(after.notes()[0].pitch == 67);
    CHECK(after.modifier_seed() == 0xABCDEF);
    CHECK(after.modifiers().size() == 2);
    REQUIRE(after.modifier_for({6}) != nullptr);
    CHECK(after.modifier_for({6})->probability == 1024);
    REQUIRE(after.modifier_for({7}) != nullptr);
    CHECK(after.modifier_for({7})->probability == 4096);
    check_lanes_intact(after);

    auto inverse = transaction({1}, 2, 2, {}, changed->inverses);
    auto restored = reduce_transaction(changed->project, inverse);
    REQUIRE(restored);
    const auto& back = midi_content(restored->project);
    CHECK(back.notes()[0].pitch == 60);
    CHECK(back.modifier_seed() == 0xABCDEF);
    CHECK(back.modifiers().size() == 2);
    check_lanes_intact(back);
}

TEST_CASE("ReplaceNoteContent drops only the modifier whose note it removes") {
    const auto original = make_authored_clip_project();
    REQUIRE(midi_content(original).modifiers().size() == 2);
    REQUIRE(midi_content(original).modifier_for({7}) != nullptr);

    const std::vector<NoteEvent> expected{modifier_note(6, 0, 60),
                                          modifier_note(7, kTicksPerQuarter / 2, 64)};
    const std::vector<NoteEvent> replacement{modifier_note(6, 0, 60)};

    auto edit =
        transaction({1}, 1, 1, {}, {ReplaceNoteContent{{3}, {4}, {5}, expected, replacement}});
    auto changed = reduce_transaction(original, edit);
    REQUIRE(changed);

    const auto& after = midi_content(changed->project);
    REQUIRE(after.notes().size() == 1);
    CHECK(after.modifier_seed() == 0xABCDEF);
    CHECK(after.modifiers().size() == 1);
    REQUIRE(after.modifier_for({6}) != nullptr);
    CHECK(after.modifier_for({6})->probability == 1024);
    CHECK(after.modifier_for({7}) == nullptr);
    CHECK_FALSE(changed->project.locate({7})->active);
    check_lanes_intact(after);
}

namespace {

// The note set before and after an edit that deletes note seven, the note the
// fixture gives a modifier of its own.
const std::vector<NoteEvent> kBothNotes{modifier_note(6, 0, 60),
                                        modifier_note(7, kTicksPerQuarter / 2, 64)};
const std::vector<NoteEvent> kFirstNoteOnly{modifier_note(6, 0, 60)};

} // namespace

// Restoring a deleted note means restoring a tombstoned identity, which the
// public reducer refuses, so this drives a real DocumentSession's undo.
TEST_CASE("Undoing a note removal restores the removed note's modifier") {
    auto session = std::move(DocumentSession::create(make_authored_clip_project())).value();
    auto writer = std::move(session->register_writer()).value();
    REQUIRE(midi_content(*session->snapshot()).modifier_for({7})->probability == 4096);

    auto edit = session_transaction(
        writer, session->revision(),
        {ReplaceNoteContent{{3}, {4}, {5}, kBothNotes, kFirstNoteOnly}});
    REQUIRE(session->submit(writer, std::move(edit)));
    REQUIRE(midi_content(*session->snapshot()).notes().size() == 1);
    REQUIRE(midi_content(*session->snapshot()).modifier_for({7}) == nullptr);

    // The dropped modifier is unreachable from the edited clip, so the recorded
    // inverse has to carry it: an inverse that only re-listed the notes would
    // restore a note that has silently forgotten how it plays.
    REQUIRE(session->can_undo());
    REQUIRE(session->undo(writer));

    const auto& back = midi_content(*session->snapshot());
    REQUIRE(back.notes().size() == 2);
    CHECK(back.modifier_seed() == 0xABCDEF);
    REQUIRE(back.modifiers().size() == 2);
    REQUIRE(back.modifier_for({6}) != nullptr);
    CHECK(back.modifier_for({6})->probability == 1024);
    REQUIRE(back.modifier_for({7}) != nullptr);
    CHECK(back.modifier_for({7})->probability == 4096);
    CHECK(back.modifier_for({7})->condition == NoteConditionKind::Always);
    CHECK(back.modifier_for({7})->ratchet_count == 1);
    check_lanes_intact(back);

    // Redo drops it again and a second undo brings it back, so the payload the
    // inverse carries is recomputed rather than consumed once.
    REQUIRE(session->redo(writer));
    CHECK(midi_content(*session->snapshot()).modifier_for({7}) == nullptr);
    REQUIRE(session->undo(writer));
    REQUIRE(midi_content(*session->snapshot()).modifier_for({7}) != nullptr);
    CHECK(midi_content(*session->snapshot()).modifier_for({7})->probability == 4096);
}

TEST_CASE("A shrinking note edit drops the orphaned modifier and keeps every lane") {
    const auto original = make_authored_clip_project();
    auto edit = transaction({1}, 1, 1, {},
                            {ReplaceNoteContent{{3}, {4}, {5}, kBothNotes, kFirstNoteOnly}});
    auto changed = reduce_transaction(original, edit);
    REQUIRE(changed);

    // A modifier keys on a note id and a lane keys on a channel-voice address,
    // so the same edit has to treat them differently: the orphaned modifier
    // goes and both lanes stay whole. The inverse inherits that split — it
    // restates the modifiers because they were filtered, and says nothing about
    // lanes because nothing filtered them.
    const auto& after = midi_content(changed->project);
    CHECK(after.modifier_for({7}) == nullptr);
    check_lanes_intact(after);

    REQUIRE(changed->inverses.size() == 1);
    const auto& inverse_command = std::get<ReplaceNoteContent>(changed->inverses[0]);
    REQUIRE(inverse_command.replacement_modifiers.size() == 2);
    CHECK(inverse_command.replacement_modifiers[0] == chance(6, 1024));
    CHECK(inverse_command.replacement_modifiers[1] == chance(7, 4096));
    REQUIRE(inverse_command.expected_modifiers.size() == 1);
    CHECK(inverse_command.expected_modifiers[0] == chance(6, 1024));
}

TEST_CASE("Two note-content transactions differing only in modifiers are not equivalent") {
    const std::vector<NoteEvent> expected{modifier_note(6, 0, 60),
                                          modifier_note(7, kTicksPerQuarter / 2, 64)};
    const std::vector<NoteEvent> replacement{modifier_note(6, 0, 60)};

    ReplaceNoteContent quiet{{3}, {4}, {5}, expected, replacement, {}, {chance(6, 1024)}};
    ReplaceNoteContent loud = quiet;
    loud.replacement_modifiers = {chance(6, 4096)};

    // The idempotency cache answers a repeated transaction id with the first
    // result it saw, so calling these equivalent would apply one payload's
    // modifiers and report the other's outcome.
    auto first = transaction({1}, 1, 1, {}, {quiet});
    auto retry = transaction({1}, 1, 1, {}, {loud});
    CHECK_FALSE(equivalent(first, retry));

    auto gated = transaction({1}, 1, 1, {}, {quiet});
    gated.commands[0].command = ReplaceNoteContent{{3},  {4}, {5}, expected, replacement,
                                                   {chance(6, 1024)}, {chance(6, 1024)}};
    CHECK_FALSE(equivalent(first, gated));
    CHECK(equivalent(first, transaction({1}, 1, 1, {}, {quiet})));
}

TEST_CASE("Note-content retained size counts the modifiers the payload carries") {
    const std::vector<NoteEvent> expected{modifier_note(6, 0, 60),
                                          modifier_note(7, kTicksPerQuarter / 2, 64)};
    const std::vector<NoteEvent> replacement{modifier_note(6, 0, 60)};

    const Command bare{ReplaceNoteContent{{3}, {4}, {5}, expected, replacement}};
    const Command carrying{ReplaceNoteContent{
        {3}, {4}, {5}, expected, replacement, {chance(6, 1024)}, {chance(6, 1024), chance(7, 4096)}}};

    // The journal budgets a command by this number and the fallthrough answers
    // `sizeof(T)` for a payload it does not know, so a field it forgets is
    // under-counted rather than refused.
    CHECK(retained_size(carrying) == retained_size(bare) + 3 * sizeof(NoteModifier));
}

namespace {

// One note of the fixture's two, moved a quarter later and up an octave, so a
// check that reads the wrong note or the wrong field cannot pass.
const std::vector<NoteEvent> kNoteSixNow{modifier_note(6, 0, 60)};
const std::vector<NoteEvent> kNoteSixMoved{modifier_note(6, kTicksPerQuarter, 72)};

SetNoteEvents move_note_six() {
    return {{3}, {4}, {5}, kNoteSixNow, kNoteSixMoved};
}

} // namespace

TEST_CASE("SetNoteEvents rewrites only the notes it names and keeps everything else") {
    const auto original = make_authored_clip_project();
    // The fixture builds under NDEBUG, where its asserts vanish: state what the
    // clip carries before the edit so the checks below cannot pass vacuously.
    REQUIRE(midi_content(original).notes().size() == 2);
    REQUIRE(midi_content(original).modifiers().size() == 2);
    REQUIRE(midi_content(original).modifier_seed() == 0xABCDEF);
    check_lanes_intact(midi_content(original));

    auto edit = transaction({1}, 1, 1, {}, {move_note_six()});
    auto changed = reduce_transaction(original, edit);
    REQUIRE(changed);

    const auto& after = midi_content(changed->project);
    REQUIRE(after.notes().size() == 2);
    // Notes are ordered by (start, id) and the edit moves note six past note
    // seven, so the two swap places — read each by identity rather than by slot.
    const auto find = [&](ItemId id) {
        const auto found = std::find_if(after.notes().begin(), after.notes().end(),
                                        [&](const NoteEvent& note) { return note.id == id; });
        REQUIRE(found != after.notes().end());
        return *found;
    };
    const auto six = find({6});
    CHECK(six.pitch == 72);
    CHECK(six.start.value == kTicksPerQuarter);
    // The note the payload never named keeps every field it had.
    const auto seven = find({7});
    CHECK(seven.pitch == 64);
    CHECK(seven.start.value == kTicksPerQuarter / 2);
    CHECK(seven.velocity == 1000);

    // Rebuilding the content from the notes alone would take all three of these
    // with it, and none of them is reachable from the payload.
    CHECK(after.modifier_seed() == 0xABCDEF);
    REQUIRE(after.modifiers().size() == 2);
    REQUIRE(after.modifier_for({6}) != nullptr);
    CHECK(after.modifier_for({6})->probability == 1024);
    REQUIRE(after.modifier_for({7}) != nullptr);
    CHECK(after.modifier_for({7})->probability == 4096);
    check_lanes_intact(after);

    // No note entered or left, so the clip owns exactly the identities it did.
    CHECK(changed->project.next_item_id() == original.next_item_id());
    CHECK(changed->project.locate({6})->active);
    CHECK(changed->project.locate({7})->active);

    REQUIRE(changed->inverses.size() == 1);
    const auto& inverse_command = std::get<SetNoteEvents>(changed->inverses[0]);
    REQUIRE(inverse_command.expected.size() == 1);
    REQUIRE(inverse_command.replacement.size() == 1);
    CHECK(inverse_command.expected[0].pitch == 72);
    CHECK(inverse_command.replacement[0].pitch == 60);

    auto inverse = transaction({1}, 2, 2, {}, changed->inverses);
    auto restored = reduce_transaction(changed->project, inverse);
    REQUIRE(restored);
    CHECK(same_project(original, restored->project));
    check_lanes_intact(midi_content(restored->project));
}

TEST_CASE("Undoing a SetNoteEvents restores the values the notes held before it") {
    auto session = std::move(DocumentSession::create(make_authored_clip_project())).value();
    auto writer = std::move(session->register_writer()).value();
    REQUIRE(midi_content(*session->snapshot()).notes().size() == 2);

    auto edit = session_transaction(writer, session->revision(), {move_note_six()});
    REQUIRE(session->submit(writer, std::move(edit)));
    {
        const auto& after = midi_content(*session->snapshot());
        REQUIRE(after.notes().size() == 2);
        CHECK(after.notes()[0].id == ItemId{7});
        CHECK(after.notes()[1].id == ItemId{6});
        CHECK(after.notes()[1].pitch == 72);
    }

    REQUIRE(session->can_undo());
    REQUIRE(session->undo(writer));
    {
        const auto& back = midi_content(*session->snapshot());
        REQUIRE(back.notes().size() == 2);
        CHECK(back.notes()[0].id == ItemId{6});
        CHECK(back.notes()[0].pitch == 60);
        CHECK(back.notes()[0].start.value == 0);
        CHECK(back.notes()[1].id == ItemId{7});
        CHECK(back.notes()[1].pitch == 64);
        CHECK(back.modifier_seed() == 0xABCDEF);
        CHECK(back.modifiers().size() == 2);
        check_lanes_intact(back);
    }

    // Redo re-applies it and a second undo takes it back again, so the recorded
    // inverse is recomputed on each pass rather than consumed once.
    REQUIRE(session->redo(writer));
    CHECK(midi_content(*session->snapshot()).notes()[1].pitch == 72);
    REQUIRE(session->undo(writer));
    CHECK(midi_content(*session->snapshot()).notes()[0].pitch == 60);
}

TEST_CASE("SetNoteEvents refuses a payload it cannot pair or apply") {
    const auto original = make_authored_clip_project();
    const auto reject = [&](SetNoteEvents command) {
        auto rejected = reduce_transaction(original, transaction({1}, 1, 1, {}, {command}));
        REQUIRE_FALSE(rejected);
        return rejected.error().code;
    };

    CHECK(reject({{3}, {4}, {5}, {}, {}}) == ConflictCode::ModelInvariant);
    CHECK(reject({{3}, {4}, {5}, kNoteSixNow, {}}) == ConflictCode::ModelInvariant);
    // Same length, but entry zero names a different note on each side, so the
    // inverse would describe a note the forward edit never touched.
    CHECK(reject({{3}, {4}, {5}, kNoteSixNow, {modifier_note(7, 0, 72)}}) ==
          ConflictCode::ModelInvariant);
    // The pairing that matters: the two arrays hold the same identities in
    // different orders, so every note the payload names does exist and every
    // gate it states is true. Nothing downstream refuses it — the id set stays
    // unique and the content rebuilds — and the clip quietly ends up with each
    // note wearing the other's value. Only the pairwise identity check sees it.
    CHECK(reject({{3},
                  {4},
                  {5},
                  {modifier_note(6, 0, 60), modifier_note(7, kTicksPerQuarter / 2, 64)},
                  {modifier_note(7, kTicksPerQuarter / 2, 64), modifier_note(6, 0, 60)}}) ==
          ConflictCode::ModelInvariant);
    // One note named twice: both entries would apply and the inverse could not
    // say which value to restore.
    CHECK(reject({{3},
                  {4},
                  {5},
                  {modifier_note(6, 0, 60), modifier_note(6, 0, 60)},
                  {modifier_note(6, 0, 72), modifier_note(6, 0, 65)}}) ==
          ConflictCode::ModelInvariant);
    // A well-formed pair naming a note the clip does not carry.
    CHECK(reject({{3}, {4}, {5}, {modifier_note(99, 0, 60)}, {modifier_note(99, 0, 72)}}) ==
          ConflictCode::TargetMissing);
    // The gate is every field, not just identity: this names note six correctly
    // and states a pitch it does not have.
    CHECK(reject({{3}, {4}, {5}, {modifier_note(6, 0, 61)}, kNoteSixMoved}) ==
          ConflictCode::ExpectedValueMismatch);

    // Every rejection leaves the clip exactly as it was.
    CHECK(midi_content(original).notes()[0].pitch == 60);
    CHECK(midi_content(original).modifiers().size() == 2);
    check_lanes_intact(midi_content(original));
}

TEST_CASE("SetNoteEvents costs the notes under the gesture rather than the clip") {
    // A clip far larger than the edit, which is the case the command exists for.
    std::vector<NoteEvent> clip_notes;
    clip_notes.reserve(200);
    for (std::uint64_t index = 0; index < 200; ++index)
        clip_notes.push_back(modifier_note(6 + index,
                                           static_cast<std::int64_t>(index) * kTicksPerQuarter / 8,
                                           60));

    const std::vector<NoteEvent> one_note{clip_notes[0]};
    const std::vector<NoteEvent> one_note_moved{modifier_note(6, 0, 72)};
    const Command narrow{SetNoteEvents{{3}, {4}, {5}, one_note, one_note_moved}};

    std::vector<NoteEvent> hundred_moved;
    for (std::size_t index = 0; index < 101; ++index)
        hundred_moved.push_back(clip_notes[index]);
    const Command wide{SetNoteEvents{{3}, {4}, {5}, hundred_moved, hundred_moved}};

    // The journal budgets a command by this number, and the dispatch answers a
    // flat `sizeof(T)` for a payload it has no arm for — so a missing arm reads
    // as a hundred-note edit costing the same as a one-note edit. Assert the
    // growth, not that the figure is merely non-zero.
    CHECK(retained_size(wide) >= retained_size(narrow) + 200 * sizeof(NoteEvent));

    // And the point of the shape: the same one-note edit expressed as
    // ReplaceNoteContent has to gate on the clip's entire note array.
    const Command whole_clip{ReplaceNoteContent{{3}, {4}, {5}, clip_notes, clip_notes}};
    CHECK(retained_size(narrow) + 398 * sizeof(NoteEvent) <= retained_size(whole_clip));
}

TEST_CASE("Two SetNoteEvents differing only in the value they start from are not equivalent") {
    const SetNoteEvents from_sixty{{3}, {4}, {5}, kNoteSixNow, kNoteSixMoved};
    SetNoteEvents from_sixty_one = from_sixty;
    from_sixty_one.expected = {modifier_note(6, 0, 61)};

    // The idempotency cache answers a repeated transaction id with the first
    // result it saw. These two agree on where the note ends up and disagree on
    // where it started, so they have different inverses: treating them as one
    // would undo to a pitch the clip never held.
    auto first = transaction({1}, 1, 1, {}, {from_sixty});
    CHECK_FALSE(equivalent(first, transaction({1}, 1, 1, {}, {from_sixty_one})));

    SetNoteEvents elsewhere = from_sixty;
    elsewhere.replacement = {modifier_note(6, kTicksPerQuarter, 71)};
    CHECK_FALSE(equivalent(first, transaction({1}, 1, 1, {}, {elsewhere})));
    CHECK(equivalent(first, transaction({1}, 1, 1, {}, {from_sixty})));
}

TEST_CASE("Timeline edits and inverses preserve clip playback properties") {
    const ClipPlaybackProperties playback{0.375f, 120, 240};
    const ClipPlaybackProperties replacement{0.75f, 60, 90};
    auto track = Track::create({4}, "track", {make_note_clip({5}, {6}, 0, 1000, playback)});
    REQUIRE(track);
    auto sequence = Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter},
                                     {std::move(track).value()});
    REQUIRE(sequence);
    auto original = Project::create({{1}, "project", 7, {3}, {}, {std::move(sequence).value()}});
    REQUIRE(original);

    const auto old_range = clip(original.value()).time_range();
    ClipTimeRange new_range = MusicalTimeRange{{2 * kTicksPerQuarter}, {kTicksPerQuarter}};
    auto edit = transaction({1}, 1, 1, {},
                            {MoveClip{{3}, {4}, {5}, old_range, new_range},
                             SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 4096},
                             SetClipPlaybackProperties{{3}, {4}, {5}, playback, replacement}});
    auto changed = reduce_transaction(original.value(), edit);
    REQUIRE(changed);
    REQUIRE(clip(changed->project).playback_properties() == replacement);

    auto inverse = transaction({1}, 2, 4, {}, changed->inverses);
    auto restored = reduce_transaction(changed->project, inverse);
    REQUIRE(restored);
    REQUIRE(clip(restored->project).playback_properties() == playback);
    REQUIRE(same_project(original.value(), restored->project));
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
    MediaAsset asset{{9}, "asset", 10, {48'000, 1}, content_hash(), AssetStoragePolicy::External,
                     {},  {}, {}};
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

TEST_CASE("Timeline reduction support preserves full ownership and atomic identity failures") {
    auto first = Track::create({4}, "first", {make_note_clip({5}, {6}, 0)});
    auto second = Track::create({7}, "second", {});
    REQUIRE(first);
    REQUIRE(second);
    auto sequence = Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter},
                                     {std::move(first).value(), std::move(second).value()});
    REQUIRE(sequence);
    auto project = Project::create({{1}, "project", 8, {3}, {}, {std::move(sequence).value()}});
    REQUIRE(project);

    auto wrong_parent = transaction(
        {1}, 1, 1, {}, {SetNoteVelocity{{3}, {7}, {5}, {6}, 1000, 2000}});
    auto parent_rejected = reduce_transaction(project.value(), wrong_parent);
    REQUIRE_FALSE(parent_rejected);
    REQUIRE(parent_rejected.error().code == ConflictCode::ParentMismatch);

    auto notes = MidiContent::create({{{8}, {0}, {kTicksPerQuarter / 4}, 1000, 60, 0}});
    REQUIRE(notes);
    auto colliding = Clip::create({8}, {2 * kTicksPerQuarter}, {kTicksPerQuarter},
                                  std::move(notes).value());
    REQUIRE(colliding);
    auto duplicate_identity = transaction(
        {1}, 2, 2, {}, {InsertClip{{3}, {4}, std::move(colliding).value()}});
    auto identity_rejected = reduce_transaction(project.value(), duplicate_identity);
    REQUIRE_FALSE(identity_rejected);
    REQUIRE(identity_rejected.error().code == ConflictCode::ModelInvariant);
    REQUIRE(identity_rejected.error().model_error);
    REQUIRE(identity_rejected.error().model_error->code == ModelErrorCode::IdentityConflict);
    REQUIRE_FALSE(project->locate({8}));
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
    const auto before_track_nodes = Track::index_stats().nodes_created;
    auto track = Track::create({4}, "large", std::move(clips));
    REQUIRE(track);
    REQUIRE(Track::index_stats().nodes_created - before_track_nodes == 20000);
    auto sequence =
        Sequence::create({3}, "sequence", TickDuration{200000}, {std::move(track).value()});
    REQUIRE(sequence);
    const auto before_project_nodes = Project::identity_stats().nodes_created;
    auto project = Project::create({{1}, "large", 10005, {3}, {}, {std::move(sequence).value()}});
    REQUIRE(project);
    REQUIRE(Project::identity_stats().nodes_created - before_project_nodes == 10003);
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

TEST_CASE("Opaque semantic equality ignores admission limits") {
    const std::string raw = R"({"data":{"value":"same"},"type_name":"vendor.opaque","version":1})";
    auto strict = OpaqueContent::create({"vendor.opaque", 1}, raw,
                                        {.max_input_bytes = 1024, .max_opaque_bytes = 1024});
    auto broad = OpaqueContent::create({"vendor.opaque", 1}, raw);
    REQUIRE(strict);
    REQUIRE(broad);
    auto left = Clip::create({7}, {0}, {10}, std::move(strict).value());
    auto right = Clip::create({7}, {0}, {10}, std::move(broad).value());
    REQUIRE(left);
    REQUIRE(right);
    REQUIRE(equivalent(left.value(), right.value()));
}

TEST_CASE("Asset commands include audio loop metadata in equality and retained size") {
    AudioLoopInfo loop{
        .musical_length = TickDuration{4 * kTicksPerQuarter},
        .points = {{240, AudioLoopPointKind::Manual}, {480, AudioLoopPointKind::Automatic}},
        .tags = {"drums", "warm"},
    };
    MediaAsset asset{{9}, "loop.wav", 960, {48'000, 1}, content_hash(),
                     AssetStoragePolicy::External, {}, {}, loop};
    const Command with_loop = CreateAsset{asset};
    REQUIRE(equivalent(with_loop, Command{CreateAsset{asset}}));
    REQUIRE(retained_size(with_loop) >=
            2 * sizeof(AudioLoopPoint) + 2 * sizeof(std::string) + loop.tags[0].size() +
                loop.tags[1].size());

    asset.loop_info.reset();
    REQUIRE_FALSE(equivalent(with_loop, Command{CreateAsset{asset}}));
}

TEST_CASE("Sequence commands include groove state in equality and retained size") {
    auto first_groove = GrooveTemplate::create(
        {.name = "first", .step = TickDuration{120}, .steps = {{TickDuration{10}, 1100}}});
    auto second_groove = GrooveTemplate::create(
        {.name = "second", .step = TickDuration{120}, .steps = {{TickDuration{-10}, 900}}});
    REQUIRE(first_groove);
    REQUIRE(second_groove);

    auto first = Sequence::create(SequenceInput{.id = {50},
                                                .name = "sequence",
                                                .musical_duration = TickDuration{480},
                                                .groove = std::move(first_groove).value()});
    auto second = Sequence::create(SequenceInput{.id = {50},
                                                 .name = "sequence",
                                                 .musical_duration = TickDuration{480},
                                                 .groove = std::move(second_groove).value()});
    auto plain = Sequence::create(SequenceInput{
        .id = {50}, .name = "sequence", .musical_duration = TickDuration{480}});
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(plain);

    const Command first_insert = InsertSequence{first.value()};
    REQUIRE_FALSE(equivalent(first_insert, Command{InsertSequence{second.value()}}));
    const auto dynamic_groove_size =
        first->groove().name().size() + first->groove().steps().size() * sizeof(GrooveStep);
    REQUIRE(retained_size(first_insert) >=
            retained_size(Command{InsertSequence{plain.value()}}) + dynamic_groove_size);
}

TEST_CASE("Sequence command retained size includes derived outgoing references") {
    auto reference_clips = [](bool distinct_targets) {
        std::vector<Clip> clips;
        for (std::uint64_t index = 0; index < 4; ++index) {
            auto clip = Clip::create(
                {100 + index},
                TickPosition{static_cast<std::int64_t>(index * 120)},
                TickDuration{120},
                SequenceRef{{200 + (distinct_targets ? index : 0)}, TickPosition{0}});
            REQUIRE(clip);
            clips.push_back(std::move(clip).value());
        }
        return clips;
    };

    auto unique_track =
        Track::create({60}, "references", reference_clips(true));
    auto repeated_track =
        Track::create({60}, "references", reference_clips(false));
    REQUIRE(unique_track);
    REQUIRE(repeated_track);
    auto unique = Sequence::create(
        {50}, "sequence", TickDuration{480}, {std::move(unique_track).value()});
    auto repeated = Sequence::create(
        {50}, "sequence", TickDuration{480}, {std::move(repeated_track).value()});
    REQUIRE(unique);
    REQUIRE(repeated);
    REQUIRE(unique->outgoing_sequence_refs().size() == 4);
    REQUIRE(repeated->outgoing_sequence_refs().size() == 1);
    REQUIRE(retained_size(Command{InsertSequence{unique.value()}}) ==
            retained_size(Command{InsertSequence{repeated.value()}}) +
                3 * sizeof(ItemId));
}
