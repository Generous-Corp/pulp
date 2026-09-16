#include "support/timeline_persistence_test_support.hpp"

#include <pulp/timeline/document_session.hpp>
#include <pulp/timeline/transaction.hpp>

#include <cstdint>
#include <string>

namespace {

// Two streams on different channels, so an assertion about which lane an edit
// reached cannot pass by there being only one.
constexpr MidiLaneAddress kExpression{0, 0, 11, 0, 74};
constexpr MidiLaneAddress kModulation{0, 1, 11, 0, 1};

MidiExpressionLane expression_lane() {
    return MidiExpressionLane{{11}, kExpression, {{{8}, {0}, 0}, {{10}, {48}, 0xffffffff}}};
}

MidiExpressionLane modulation_lane() {
    return MidiExpressionLane{{9}, kModulation, {{{5}, {24}, 0x80000000}}};
}

// Identities at or above the project's next_item_id, which is what a fresh
// insert requires: an identity below it was either issued already or is
// tombstoned, and the reducer refuses both outside the undo path.
MidiExpressionLane fresh_lane() {
    return MidiExpressionLane{{40}, kExpression, {{{41}, {0}, 0}, {{42}, {48}, 0xffffffff}}};
}

std::vector<NoteEvent> two_notes() {
    return {{{7}, {0}, {2}, 0xffff, 60, 0}, {{6}, {4}, {2}, 0x8000, 64, 1}};
}

Project project_with_lanes(std::vector<MidiExpressionLane> lanes) {
    auto content = take(MidiContent::create(two_notes(), {}, 0, std::move(lanes)));
    auto clip = take(Clip::create({4}, {0}, {100}, std::move(content)));
    auto track = take(Track::create({3}, "track", {clip}));
    auto sequence = take(Sequence::create({2}, "sequence", TickDuration{100}, {track}));
    return take(Project::create(ProjectInput{{1}, "project", 40, {2}, {}, {sequence}}));
}

const MidiContent& only_midi_content(const Project& project) {
    const auto* sequence = project.find_sequence({2});
    REQUIRE(sequence != nullptr);
    return std::get<MidiContent>(sequence->tracks()[0].clips()[0].content());
}

Transaction one(Command command) {
    Transaction transaction;
    transaction.id = {{1}, 1};
    transaction.commands.push_back({{{1}, 1}, std::move(command)});
    return transaction;
}

// Restoring a removed lane or a removed point means reviving a tombstoned
// identity, which the public reducer refuses by design, so those undos are
// driven through a real session rather than by re-reducing the inverse.
Transaction session_one(WriterToken& writer, DocumentRevision revision, Command command) {
    Transaction transaction;
    transaction.id = writer.allocate_transaction_id();
    transaction.expected_revision = revision;
    transaction.commands.push_back({writer.allocate_command_id(), std::move(command)});
    return transaction;
}

const MidiContent& session_content(const DocumentSession& session) {
    return only_midi_content(*session.snapshot());
}

// Every lane a clip carries, whole: identity, address, and each point with its
// own identity, position, and value. Comparing these is what "restored
// exactly" means, and it is stricter than a lane count in every way that
// matters -- a restore under a fresh identity, in another order, or with a
// point dropped all fail it.
//
// The project's whole serialization is deliberately not the comparison: an
// identity index never reissues an identity, so a lane that was inserted and
// removed leaves a tombstone and an advanced next_item_id behind. That is the
// document correctly remembering, not the restore being inexact.
std::vector<MidiExpressionLane> lanes_of(const Project& project) {
    const auto& content = only_midi_content(project);
    return {content.lanes().begin(), content.lanes().end()};
}

} // namespace

TEST_CASE("inserting an expression lane adds it and its inverse removes it exactly",
          "[timeline][midi-expression-command]") {
    const auto project = project_with_lanes({modulation_lane()});
    const auto before = lanes_of(project);

    auto reduced =
        take(reduce_transaction(project, one(InsertMidiExpressionLane{{2}, {3}, {4}, fresh_lane()})));
    const auto& content = only_midi_content(reduced.project);
    REQUIRE(content.lanes().size() == 2);
    // Canonical order is by address, so the inserted lane sorts ahead of the
    // one already there rather than landing where it was appended.
    CHECK(content.lanes()[0].address == kExpression);
    CHECK(content.lanes()[0].id == ItemId{40});
    CHECK(content.lanes()[1].address == kModulation);
    // Notes and their seed are untouched: this command carries a stream, not
    // the clip's contents.
    CHECK(content.notes().size() == 2);

    REQUIRE(reduced.dirty.items().size() == 1);
    CHECK(reduced.dirty.items()[0].item == ItemId{4});
    CHECK(reduced.dirty.items()[0].flags == (DirtyFlags::Content | DirtyFlags::Notes));

    const auto* inverse = std::get_if<RemoveMidiExpressionLane>(&reduced.inverses[0]);
    REQUIRE(inverse != nullptr);
    CHECK(inverse->lane_id == ItemId{40});
    auto undone = take(reduce_transaction(reduced.project, one(*inverse)));
    CHECK(lanes_of(undone.project) == before);
    CHECK(only_midi_content(undone.project).notes().size() == 2);
}

TEST_CASE("removing an expression lane drops it and undo restores it whole",
          "[timeline][midi-expression-command]") {
    const auto project = project_with_lanes({expression_lane(), modulation_lane()});
    const auto before = lanes_of(project);
    auto session = take(DocumentSession::create(project));
    auto writer = take(session->register_writer());

    REQUIRE(session->submit(writer, session_one(writer, session->revision(),
                                                RemoveMidiExpressionLane{{2}, {3}, {4}, {11}})));
    REQUIRE(session_content(*session).lanes().size() == 1);
    CHECK(session_content(*session).lanes()[0].id == ItemId{9});
    // Notes are untouched: removing a controller stream is not an edit to what
    // the clip plays.
    CHECK(session_content(*session).notes().size() == 2);

    REQUIRE(session->can_undo());
    REQUIRE(session->undo(writer));
    CHECK(lanes_of(*session->snapshot()) == before);

    // Redo drops it again and a second undo brings it back, so the inverse is
    // recomputed rather than consumed once.
    REQUIRE(session->redo(writer));
    CHECK(session_content(*session).lanes().size() == 1);
    REQUIRE(session->undo(writer));
    CHECK(lanes_of(*session->snapshot()) == before);
}

TEST_CASE("setting lane points replaces them under an exact gate and keeps the address",
          "[timeline][midi-expression-command]") {
    const auto project = project_with_lanes({expression_lane(), modulation_lane()});
    const auto before = lanes_of(project);
    const std::vector<MidiLanePoint> current{{{8}, {0}, 0}, {{10}, {48}, 0xffffffff}};
    // Identity 43 is above next_item_id, so it is a point this edit introduces;
    // identity 10 is dropped, which retires it.
    const std::vector<MidiLanePoint> next{{{8}, {0}, 0x40000000}, {{43}, {96}, 0x20000000}};
    auto session = take(DocumentSession::create(project));
    auto writer = take(session->register_writer());

    REQUIRE(session->submit(
        writer, session_one(writer, session->revision(),
                            SetMidiExpressionLanePoints{{2}, {3}, {4}, {11}, current, next})));
    const auto& content = session_content(*session);
    REQUIRE(content.lanes().size() == 2);
    const auto& edited = content.lanes()[0];
    CHECK(edited.id == ItemId{11});
    // The address the lane had is the address it keeps: no payload member can
    // move a stream to another controller.
    CHECK(edited.address == kExpression);
    REQUIRE(edited.points.size() == 2);
    CHECK(edited.points[0].value == 0x40000000u);
    CHECK(edited.points[1].id == ItemId{43});
    // The other stream is untouched.
    CHECK(content.lanes()[1].points.size() == 1);

    // A stale expectation is refused rather than applied over the edit that
    // landed first.
    auto stale = reduce_transaction(
        *session->snapshot(),
        one(SetMidiExpressionLanePoints{{2}, {3}, {4}, {11}, current, next}));
    REQUIRE_FALSE(stale.has_value());
    CHECK(stale.error().code == ConflictCode::ExpectedValueMismatch);

    REQUIRE(session->undo(writer));
    CHECK(lanes_of(*session->snapshot()) == before);
}

TEST_CASE("a lane point edit gates on the document's order, not the payload's",
          "[timeline][midi-expression-command]") {
    const auto project = project_with_lanes({expression_lane()});
    // The same two points the lane holds, listed in the other order. The
    // document stores one canonical order, so an expectation that states
    // another is a statement about a document that does not exist.
    const std::vector<MidiLanePoint> reordered{{{10}, {48}, 0xffffffff}, {{8}, {0}, 0}};
    auto refused = reduce_transaction(
        project,
        one(SetMidiExpressionLanePoints{{2}, {3}, {4}, {11}, reordered, reordered}));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().code == ConflictCode::ExpectedValueMismatch);

    // Control: the identical points in canonical order are admitted, so the
    // refusal is the order rather than the points.
    const std::vector<MidiLanePoint> canonical{{{8}, {0}, 0}, {{10}, {48}, 0xffffffff}};
    CHECK(reduce_transaction(
              project,
              one(SetMidiExpressionLanePoints{{2}, {3}, {4}, {11}, canonical, canonical}))
              .has_value());

    // A lane the clip does not own is a missing target rather than a silent
    // no-op that reports success.
    auto absent = reduce_transaction(
        project, one(SetMidiExpressionLanePoints{{2}, {3}, {4}, {99}, canonical, canonical}));
    REQUIRE_FALSE(absent.has_value());
}

TEST_CASE("expression lane commands refuse a forged chase receipt",
          "[timeline][midi-expression-command]") {
    const auto project = project_with_lanes({modulation_lane()});
    MidiExpressionLane forged = fresh_lane();
    forged.points[0].chased = true;

    auto inserted =
        reduce_transaction(project, one(InsertMidiExpressionLane{{2}, {3}, {4}, forged}));
    REQUIRE_FALSE(inserted.has_value());
    CHECK(inserted.error().code == ConflictCode::ModelInvariant);

    // The same refusal on the point-edit path, since a caller denied the flag
    // on insert would otherwise reach it one command later.
    const auto with_lane = project_with_lanes({expression_lane()});
    std::vector<MidiLanePoint> replacement{{{8}, {0}, 0}, {{10}, {48}, 0xffffffff}};
    replacement[1].chased = true;
    auto edited = reduce_transaction(
        with_lane, one(SetMidiExpressionLanePoints{
                       {2}, {3}, {4}, {11}, {{{8}, {0}, 0}, {{10}, {48}, 0xffffffff}},
                       replacement}));
    REQUIRE_FALSE(edited.has_value());
    CHECK(edited.error().code == ConflictCode::ModelInvariant);

    // Control: the identical payloads without the flag are admitted, so the
    // refusals above are the flag and not a malformed lane.
    CHECK(reduce_transaction(project, one(InsertMidiExpressionLane{{2}, {3}, {4}, fresh_lane()}))
              .has_value());
}

TEST_CASE("the content model's own refusals surface as command errors",
          "[timeline][midi-expression-command]") {
    const auto project = project_with_lanes({expression_lane()});

    // A second lane claiming the address the clip already carries.
    const MidiExpressionLane collision{{44}, kExpression, {{{45}, {0}, 0}}};
    auto duplicate_address =
        reduce_transaction(project, one(InsertMidiExpressionLane{{2}, {3}, {4}, collision}));
    REQUIRE_FALSE(duplicate_address.has_value());
    REQUIRE(duplicate_address.error().model_error.has_value());
    CHECK(duplicate_address.error().model_error->code == ModelErrorCode::DuplicateMidiLaneAddress);

    // A lane whose point reuses a note's identity.
    const MidiExpressionLane clash{{46}, kModulation, {{{7}, {0}, 0}}};
    auto identity_clash =
        reduce_transaction(project, one(InsertMidiExpressionLane{{2}, {3}, {4}, clash}));
    REQUIRE_FALSE(identity_clash.has_value());
    REQUIRE(identity_clash.error().model_error.has_value());
    CHECK(identity_clash.error().model_error->code == ModelErrorCode::DuplicateItemId);

    // Control: a lane that collides with neither is admitted, so the two
    // refusals above are the collisions rather than a reducer that refuses
    // every insert into this clip.
    const MidiExpressionLane clean{{47}, kModulation, {{{48}, {0}, 0}}};
    CHECK(reduce_transaction(project, one(InsertMidiExpressionLane{{2}, {3}, {4}, clean}))
              .has_value());

    // An address outside the width the wire gives it names no stream. The
    // refusal comes from the content model rather than from a second check in
    // the reducer, so the error carries the model's own reason.
    MidiExpressionLane malformed = clean;
    malformed.address.channel = 16;
    auto bad_address =
        reduce_transaction(project, one(InsertMidiExpressionLane{{2}, {3}, {4}, malformed}));
    REQUIRE_FALSE(bad_address.has_value());
    CHECK(bad_address.error().code == ConflictCode::ModelInvariant);
    REQUIRE(bad_address.error().model_error.has_value());
    CHECK(bad_address.error().model_error->code == ModelErrorCode::InvalidMidiLane);
}

TEST_CASE("expression lane commands round trip through their schema envelopes",
          "[timeline][midi-expression-command][persistence]") {
    const auto registry = builtins();
    const std::string insert =
        R"({"data":{"clip_id":"4","lane":{"bank":0,"channel":0,"group":0,"id":"11","index":74,)"
        R"("points":[{"id":"8","position_ticks":"0","value":0}],"status":11},)"
        R"("sequence_id":"2","track_id":"3"},)"
        R"("type_name":"pulp.timeline.command.insert_midi_expression_lane","version":1})";
    auto decoded = deserialize_commands("[" + insert + "]", registry);
    REQUIRE(decoded.has_value());
    const auto* inserted = std::get_if<InsertMidiExpressionLane>(&decoded.value()[0]);
    REQUIRE(inserted != nullptr);
    CHECK(inserted->clip_id == ItemId{4});
    CHECK(inserted->lane.address == kExpression);
    REQUIRE(inserted->lane.points.size() == 1);
    CHECK(inserted->lane.points[0].value == 0u);
    CHECK_FALSE(inserted->lane.points[0].chased);

    const std::string remove =
        R"({"data":{"clip_id":"4","lane_id":"11","sequence_id":"2","track_id":"3"},)"
        R"("type_name":"pulp.timeline.command.remove_midi_expression_lane","version":1})";
    auto removed = deserialize_commands("[" + remove + "]", registry);
    REQUIRE(removed.has_value());
    const auto* removal = std::get_if<RemoveMidiExpressionLane>(&removed.value()[0]);
    REQUIRE(removal != nullptr);
    CHECK(removal->lane_id == ItemId{11});

    const std::string points =
        R"({"data":{"clip_id":"4","expected":[{"id":"8","position_ticks":"0","value":0}],)"
        R"("lane_id":"11","replacement":[{"id":"8","position_ticks":"0","value":127}],)"
        R"("sequence_id":"2","track_id":"3"},)"
        R"("type_name":"pulp.timeline.command.set_midi_expression_lane_points","version":1})";
    auto edited = deserialize_commands("[" + points + "]", registry);
    REQUIRE(edited.has_value());
    const auto* edit = std::get_if<SetMidiExpressionLanePoints>(&edited.value()[0]);
    REQUIRE(edit != nullptr);
    REQUIRE(edit->expected.size() == 1);
    REQUIRE(edit->replacement.size() == 1);
    CHECK(edit->replacement[0].value == 127u);
}

TEST_CASE("decode refuses a forged chase receipt and a malformed address",
          "[timeline][midi-expression-command][persistence]") {
    const auto registry = builtins();
    const auto envelope = [](std::string_view lane) {
        return std::string(R"([{"data":{"clip_id":"4","lane":)") + std::string(lane) +
               R"(,"sequence_id":"2","track_id":"3"},)"
               R"("type_name":"pulp.timeline.command.insert_midi_expression_lane",)"
               R"("version":1}])";
    };
    constexpr std::string_view well_formed =
        R"({"bank":0,"channel":0,"group":0,"id":"11","index":74,)"
        R"("points":[{"id":"8","position_ticks":"0","value":0}],"status":11})";
    // Control first: the same envelope without the defect decodes, so each
    // refusal below is the defect and not the surrounding payload.
    REQUIRE(deserialize_commands(envelope(well_formed), registry).has_value());

    constexpr std::string_view chased =
        R"({"bank":0,"channel":0,"group":0,"id":"11","index":74,)"
        R"("points":[{"chased":true,"id":"8","position_ticks":"0","value":0}],"status":11})";
    REQUIRE_FALSE(deserialize_commands(envelope(chased), registry));

    // An explicit false is the flag's authored value, so it is admitted; only
    // claiming a derivation is refused.
    constexpr std::string_view unchased =
        R"({"bank":0,"channel":0,"group":0,"id":"11","index":74,)"
        R"("points":[{"chased":false,"id":"8","position_ticks":"0","value":0}],"status":11})";
    REQUIRE(deserialize_commands(envelope(unchased), registry).has_value());

    constexpr std::string_view wide_channel =
        R"({"bank":0,"channel":16,"group":0,"id":"11","index":74,)"
        R"("points":[{"id":"8","position_ticks":"0","value":0}],"status":11})";
    REQUIRE_FALSE(deserialize_commands(envelope(wide_channel), registry));

    constexpr std::string_view wide_status =
        R"({"bank":0,"channel":0,"group":0,"id":"11","index":74,)"
        R"("points":[{"id":"8","position_ticks":"0","value":0}],"status":16})";
    REQUIRE_FALSE(deserialize_commands(envelope(wide_status), registry));
}

TEST_CASE("a non-destructive writer is refused a lane edit that drops points",
          "[timeline][midi-expression-command][capabilities]") {
    // The proposal profile an MCP caller gets by default: every class, no
    // destructive intent. A point edit declares Note/Modify, so nothing in the
    // command's declared authority alone would stop this writer from clearing
    // a lane that took real work to author.
    const auto proposal = non_destructive_capabilities();
    const std::vector<MidiLanePoint> current{{{8}, {0}, 0}, {{10}, {48}, 0xffffffff}};

    auto session = take(DocumentSession::create(project_with_lanes({expression_lane()})));
    auto writer = take(session->register_writer(proposal));
    const auto before = lanes_of(*session->snapshot());
    const auto revision_before = session->revision();

    // Emptying the lane retires both point identities, so admission must refuse
    // it on the destructive axis even though the command is a Modify.
    auto cleared = session->submit(
        writer, session_one(writer, session->revision(),
                            SetMidiExpressionLanePoints{{2}, {3}, {4}, {11}, current, {}}));
    REQUIRE_FALSE(cleared);
    CHECK(cleared.error().code == ConflictCode::CapabilityDenied);

    // Dropping one of the two is the same refusal: the axis is whether any
    // identity is retired, not whether the lane ends up empty.
    auto trimmed = session->submit(
        writer,
        session_one(writer, session->revision(),
                    SetMidiExpressionLanePoints{{2}, {3}, {4}, {11}, current, {current[0]}}));
    REQUIRE_FALSE(trimmed);
    CHECK(trimmed.error().code == ConflictCode::CapabilityDenied);

    // Nothing landed: the refusal is admission, not a partial edit rolled back.
    CHECK(lanes_of(*session->snapshot()) == before);
    CHECK(session->revision() == revision_before);

    // Control -- the same writer, the same command, the same lane, changing a
    // value in place. It must be admitted, or the refusals above would prove
    // only that this writer cannot edit points at all.
    const std::vector<MidiLanePoint> revalued{{{8}, {0}, 0x40000000}, {{10}, {48}, 0xffffffff}};
    REQUIRE(session->submit(
        writer, session_one(writer, session->revision(),
                            SetMidiExpressionLanePoints{{2}, {3}, {4}, {11}, current, revalued})));
    CHECK(session_content(*session).lanes()[0].points[0].value == 0x40000000u);

    // Control -- adding a point keeps every identity the lane had, so it is
    // admitted too. Authoring is open to a proposal writer; abandonment is not.
    const std::vector<MidiLanePoint> grown{
        {{8}, {0}, 0x40000000}, {{10}, {48}, 0xffffffff}, {{43}, {96}, 0x20000000}};
    REQUIRE(session->submit(
        writer, session_one(writer, session->revision(),
                            SetMidiExpressionLanePoints{{2}, {3}, {4}, {11}, revalued, grown})));
    CHECK(session_content(*session).lanes()[0].points.size() == 3);

    // Control -- a writer holding the destructive intent clears the same lane,
    // so the refusals above are the mask rather than the command being invalid.
    auto editor = take(session->register_writer(unrestricted_capabilities()));
    REQUIRE(session->submit(editor, session_one(editor, session->revision(),
                                                SetMidiExpressionLanePoints{
                                                    {2}, {3}, {4}, {11}, grown, {}})));
    CHECK(session_content(*session).lanes()[0].points.empty());
}

TEST_CASE("a writer holding note modify may author and edit a lane but not abandon one",
          "[timeline][midi-expression-command][capabilities]") {
    constexpr auto insert = command_authority_of<InsertMidiExpressionLane>();
    constexpr auto remove = command_authority_of<RemoveMidiExpressionLane>();
    constexpr auto edit = command_authority_of<SetMidiExpressionLanePoints>();
    STATIC_REQUIRE(insert.command_class == CommandClass::Note);
    STATIC_REQUIRE(insert.intent == CommandIntent::Create);
    STATIC_REQUIRE(remove.intent == CommandIntent::Remove);
    STATIC_REQUIRE(edit.intent == CommandIntent::Modify);

    // This is the reason the family is three commands rather than one. A
    // proposal-profile writer holds every class and no destructive intent, so
    // under a single Modify-declared command that could also delete a lane, it
    // would hold lane deletion; under an Insert/Remove pair with no Modify, it
    // could create a lane and never change a value in it again. Only this
    // split admits authoring and editing while still denying abandonment.
    const auto proposal = non_destructive_capabilities();
    CHECK(allows(proposal, insert));
    CHECK(allows(proposal, edit));
    CHECK_FALSE(allows(proposal, remove));

    // Control: a mask denying nothing admits all three, so the refusal above
    // is the destructive axis rather than a mask that refuses everything.
    CHECK(allows(unrestricted_capabilities(), remove));

    // Denying note modification takes the point edit and leaves the insert, so
    // the gate is the class-and-intent pair rather than a blanket note ban.
    const auto no_note_modify =
        deny(unrestricted_capabilities(), CommandClass::Note, CommandIntent::Modify);
    CHECK_FALSE(allows(no_note_modify, edit));
    CHECK(allows(no_note_modify, insert));
}
