#include "timeline_command_test_helpers.hpp"

#include <pulp/timeline/serialize.hpp>
#include <pulp/timeline/undo.hpp>

#include <array>
#include <optional>
#include <span>
#include <vector>

#include <catch2/catch_test_macros.hpp>

using namespace timeline_test;
namespace runtime = pulp::runtime;

namespace {
runtime::Result<std::shared_ptr<const void>, PersistenceError>
decode_registered_int(const JsonValue& data, const void*) noexcept {
    const auto* value = data.find("value");
    if (!value)
        return runtime::Err(PersistenceError{PersistenceErrorCode::MissingField});
    auto parsed = parse_canonical_i64_string(*value);
    if (!parsed)
        return runtime::Err(parsed.error());
    std::shared_ptr<const void> result =
        std::make_shared<const int>(static_cast<int>(parsed.value()));
    return runtime::Ok(std::move(result));
}

runtime::Result<SchemaWriteSuccess, PersistenceError>
encode_registered_int(const std::shared_ptr<const void>& value, BoundedJsonSink& output,
                      const void*) noexcept {
    output.append("{\"value\":\"");
    output.append(std::to_string(*static_cast<const int*>(value.get())));
    output.append("\"}");
    return runtime::Ok(SchemaWriteSuccess{});
}

std::size_t retained_registered_int(const std::shared_ptr<const void>&, const void*) noexcept {
    return 4096;
}

SchemaRegistry registered_int_registry() {
    SchemaRegistryBuilder builder;
    TypeSchema schema;
    schema.type_name = "vendor.registered";
    schema.domain = SchemaDomain::Content;
    schema.current_version = 3;
    schema.fields = {{"value", SchemaValueKind::I64String}};
    schema.codec = {{}, decode_registered_int, encode_registered_int, retained_registered_int};
    REQUIRE(builder.register_type(std::move(schema)));
    auto registry = std::move(builder).build();
    REQUIRE(registry);
    return std::move(registry).value();
}
} // namespace

TEST_CASE("Registered payload retention participates in journal and undo byte limits") {
    const auto registry = registered_int_registry();
    auto content = registry.create_registered_no_owned_ids({"vendor.registered", 3},
                                                           std::make_shared<const int>(42), 1024);
    REQUIRE(content);
    auto inserted =
        Clip::create({7}, {2 * kTicksPerQuarter}, {kTicksPerQuarter}, std::move(content).value());
    REQUIRE(inserted);

    SECTION("journal preflight rejects a large inserted payload") {
        SessionLimits limits;
        limits.journal.max_retained_bytes = 1024;
        auto session = std::move(DocumentSession::create(make_project(), limits)).value();
        auto writer = std::move(session->register_writer()).value();
        auto tx = session_transaction(writer, {}, {InsertClip{{3}, {4}, inserted.value()}});
        auto rejected = session->submit(writer, std::move(tx));
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error().code == ConflictCode::JournalFull);
        REQUIRE(session->revision().value == 0);
    }

    SECTION("undo preflight counts a remove inverse that owns the payload") {
        auto track = Track::create({4}, "track", {inserted.value()});
        REQUIRE(track);
        auto sequence = Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter},
                                         {std::move(track).value()});
        REQUIRE(sequence);
        auto project =
            Project::create({{1}, "registered", 8, {3}, {}, {std::move(sequence).value()}});
        REQUIRE(project);
        SessionLimits limits;
        limits.undo.max_retained_bytes = 1024;
        auto session =
            std::move(DocumentSession::create(std::move(project).value(), limits)).value();
        auto writer = std::move(session->register_writer()).value();
        auto tx = session_transaction(writer, {}, {RemoveClip{{3}, {4}, {7}}});
        auto rejected = session->submit(writer, std::move(tx));
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error().code == ConflictCode::UndoFull);
        REQUIRE(session->snapshot()->find_sequence({3})->find_track({4})->find_clip({7}));
    }
}

TEST_CASE("Removed persistent scenes are conservatively charged to the undo byte limit") {
    std::vector<Slot> slots;
    slots.reserve(128);
    for (std::uint64_t index = 0; index < 128; ++index)
        slots.push_back(Slot{{100 + index}, {}, launch_immediate(), {}});
    const Scene raw_scene{{7}, "large scene", SlotList(slots)};
    auto sequence = Sequence::create(SequenceInput{
        .id = {3},
        .name = "sequence",
        .scenes = {raw_scene},
    });
    REQUIRE(sequence);
    const Scene persistent_scene = *sequence->find_scene({7});
    const auto raw_charge = retained_size(Command{InsertScene{{3}, raw_scene}});
    const auto persistent_charge = retained_size(Command{InsertScene{{3}, persistent_scene}});
    REQUIRE(persistent_charge > raw_charge);
    REQUIRE(persistent_charge >
            sizeof(InsertScene) + persistent_scene.name.size() +
                persistent_scene.slots.size() * sizeof(Slot));

    auto project = Project::create(ProjectInput{
        .id = {1},
        .name = "project",
        .next_item_id = 1'000,
        .root_sequence_id = {3},
        .sequences = {std::move(sequence).value()},
    });
    REQUIRE(project);
    const auto candidate_charge =
        retained_size(Command{RemoveScene{{3}, {7}}}) + persistent_charge;

    SECTION("one byte below the owned-storage charge rejects atomically") {
        SessionLimits limits;
        limits.undo.max_retained_bytes = candidate_charge - 1;
        auto session = std::move(DocumentSession::create(project.value(), limits)).value();
        auto writer = std::move(session->register_writer()).value();
        auto rejected =
            session->submit(writer, session_transaction(writer, {}, {RemoveScene{{3}, {7}}}));
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error().code == ConflictCode::UndoFull);
        REQUIRE(session->revision() == DocumentRevision{});
        REQUIRE(session->journal().entries().empty());
        REQUIRE(session->snapshot()->find_sequence({3})->find_scene({7}));
    }

    SECTION("the conservative owned-storage charge admits the removal") {
        SessionLimits limits;
        limits.undo.max_retained_bytes = candidate_charge;
        auto session = std::move(DocumentSession::create(project.value(), limits)).value();
        auto writer = std::move(session->register_writer()).value();
        REQUIRE(session->submit(
            writer, session_transaction(writer, {}, {RemoveScene{{3}, {7}}})));
        REQUIRE_FALSE(session->snapshot()->find_sequence({3})->find_scene({7}));
        REQUIRE(session->undo(writer));
        REQUIRE(session->snapshot()->find_sequence({3})->find_scene({7}));
    }
}

TEST_CASE("Registered payload retries compare canonical semantics across allocations") {
    const auto registry = registered_int_registry();
    const auto make_clip = [&](int value) {
        auto content = registry.create_registered_no_owned_ids(
            {"vendor.registered", 3}, std::make_shared<const int>(value), 1024);
        REQUIRE(content);
        auto created = Clip::create({7}, {2 * kTicksPerQuarter}, {kTicksPerQuarter},
                                    std::move(content).value());
        REQUIRE(created);
        return std::move(created).value();
    };

    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    auto first = session_transaction(writer, {}, {InsertClip{{3}, {4}, make_clip(42)}});
    auto exact_retry = first;
    exact_retry.commands[0].command = InsertClip{{3}, {4}, make_clip(42)};
    auto collision = first;
    collision.commands[0].command = InsertClip{{3}, {4}, make_clip(43)};

    auto committed = session->submit(writer, std::move(first));
    REQUIRE(committed);
    auto retried = session->submit(writer, std::move(exact_retry));
    REQUIRE(retried);
    REQUIRE(retried->revision == committed->revision);
    auto rejected = session->submit(writer, std::move(collision));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::TransactionIdCollision);
}

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

TEST_CASE("Tempo and meter edits survive undo redo journal replay and canonical persistence") {
    const auto initial = make_project();
    const auto tempo = make_tempo_map(91.0);
    const auto meter = make_meter_map({7, 8});
    auto session = std::move(DocumentSession::create(initial)).value();
    auto writer = std::move(session->register_writer()).value();
    auto tx = session_transaction(writer, {},
                                  {SetTempoMap{initial.tempo_map(), tempo},
                                   SetMeterMap{initial.meter_map(), meter}});
    REQUIRE(session->submit(writer, std::move(tx)));
    REQUIRE(session->snapshot()->tempo_map() == tempo);
    REQUIRE(session->snapshot()->meter_map() == meter);

    auto registry = make_builtin_timeline_registry();
    REQUIRE(registry);
    const auto canonical = serialize_project(*session->snapshot(), registry.value());
    REQUIRE(canonical);
    auto decoded = deserialize_project(canonical->json, registry.value());
    REQUIRE(decoded);
    REQUIRE(decoded->tempo_map() == tempo);
    REQUIRE(decoded->meter_map() == meter);
    REQUIRE(serialize_project(decoded.value(), registry.value())->json == canonical->json);

    REQUIRE(session->undo(writer));
    REQUIRE(session->snapshot()->tempo_map() == initial.tempo_map());
    REQUIRE(session->snapshot()->meter_map() == initial.meter_map());
    REQUIRE(session->redo(writer));
    REQUIRE(session->snapshot()->tempo_map() == tempo);
    REQUIRE(session->snapshot()->meter_map() == meter);

    auto replayed = session->journal().replay(initial, {});
    REQUIRE(replayed);
    REQUIRE(replayed->tempo_map() == tempo);
    REQUIRE(replayed->meter_map() == meter);
    REQUIRE(serialize_project(replayed.value(), registry.value())->json == canonical->json);
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

TEST_CASE("Timeline cancel closes the gesture so one undo reverts the whole drag") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    const auto group = writer.allocate_undo_group_id();
    auto begin = session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    begin.undo_group = group;
    begin.gesture_phase = GesturePhase::Begin;
    REQUIRE(session->submit(writer, std::move(begin)));
    auto update = session_transaction(writer, session->revision(),
                                      {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 3000}});
    update.undo_group = group;
    update.gesture_phase = GesturePhase::Update;
    REQUIRE(session->submit(writer, std::move(update)));
    auto cancel = session_transaction(writer, session->revision(),
                                      {SetNoteVelocity{{3}, {4}, {5}, {6}, 3000, 4000}});
    cancel.undo_group = group;
    cancel.gesture_phase = GesturePhase::Cancel;
    REQUIRE(session->submit(writer, std::move(cancel)));

    // A cancel arrives after its edits have applied, so closing is all the session
    // does; the revert is the caller's existing single undo over the closed group.
    REQUIRE(velocity(*session->snapshot()) == 4000);
    REQUIRE(session->can_undo());
    REQUIRE(session->undo(writer));
    REQUIRE(velocity(*session->snapshot()) == 1000);
}

TEST_CASE("Timeline cancel clears the open gesture so a later begin is admitted") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    const auto first_group = writer.allocate_undo_group_id();
    auto begin = session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    begin.undo_group = first_group;
    begin.gesture_phase = GesturePhase::Begin;
    REQUIRE(session->submit(writer, std::move(begin)));
    auto cancel = session_transaction(writer, session->revision(),
                                      {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 2500}});
    cancel.undo_group = first_group;
    cancel.gesture_phase = GesturePhase::Cancel;
    REQUIRE(session->submit(writer, std::move(cancel)));

    const auto second_group = writer.allocate_undo_group_id();
    auto next = session_transaction(writer, session->revision(),
                                    {SetNoteVelocity{{3}, {4}, {5}, {6}, 2500, 3000}});
    next.undo_group = second_group;
    next.gesture_phase = GesturePhase::Begin;
    REQUIRE(session->submit(writer, std::move(next)));
}

TEST_CASE("Timeline cancel without a matching open gesture is rejected") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    const auto group = writer.allocate_undo_group_id();
    auto stray = session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    stray.undo_group = group;
    stray.gesture_phase = GesturePhase::Cancel;
    auto no_open_gesture = session->submit(writer, std::move(stray));
    REQUIRE_FALSE(no_open_gesture);
    REQUIRE(no_open_gesture.error().code == ConflictCode::GestureState);

    auto begin = session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    begin.undo_group = group;
    begin.gesture_phase = GesturePhase::Begin;
    REQUIRE(session->submit(writer, std::move(begin)));
    auto wrong_group = session_transaction(writer, session->revision(),
                                           {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 2500}});
    wrong_group.undo_group = writer.allocate_undo_group_id();
    wrong_group.gesture_phase = GesturePhase::Cancel;
    auto mismatched = session->submit(writer, std::move(wrong_group));
    REQUIRE_FALSE(mismatched);
    REQUIRE(mismatched.error().code == ConflictCode::GestureState);
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

TEST_CASE("Timeline undo and replay preserve extension content exactly") {
    auto exercise = [](Clip inserted) {
        auto session = std::move(DocumentSession::create(make_project())).value();
        auto writer = std::move(session->register_writer()).value();
        auto tx = session_transaction(writer, {}, {InsertClip{{3}, {4}, inserted}});
        REQUIRE(session->submit(writer, std::move(tx)));
        REQUIRE(session->undo(writer));
        REQUIRE(session->redo(writer));
        const auto* restored =
            session->snapshot()->find_sequence({3})->find_track({4})->find_clip({7});
        REQUIRE(restored);
        REQUIRE(equivalent(*restored, inserted));
        auto replayed = session->journal().replay(make_project(), {});
        REQUIRE(replayed);
        const auto* replayed_clip = replayed->find_sequence({3})->find_track({4})->find_clip({7});
        REQUIRE(replayed_clip);
        REQUIRE(equivalent(*replayed_clip, inserted));
        return std::move(replayed).value();
    };

    SECTION("registered content retains schema and payload identity") {
        const auto registry = registered_int_registry();
        auto payload = std::make_shared<const int>(42);
        auto content = registry.create_registered_no_owned_ids(
            {"vendor.registered", 3}, std::static_pointer_cast<const void>(payload), 1024);
        REQUIRE(content);
        auto inserted = Clip::create({7}, {2 * kTicksPerQuarter}, {kTicksPerQuarter},
                                     std::move(content).value());
        REQUIRE(inserted);
        const auto replayed = exercise(std::move(inserted).value());
        const auto& value = std::get<RegisteredContent>(clip(replayed, {7}).content());
        REQUIRE(value.schema() == SchemaIdentity{"vendor.registered", 3});
        REQUIRE(value.value_as<int>() == payload.get());
    }

    SECTION("opaque content retains exact admitted envelope") {
        const std::string raw =
            R"({"data":{"answer":"42"},"type_name":"vendor.opaque","version":9})";
        auto content = OpaqueContent::create({"vendor.opaque", 9}, raw);
        REQUIRE(content);
        auto inserted = Clip::create({7}, {2 * kTicksPerQuarter}, {kTicksPerQuarter},
                                     std::move(content).value());
        REQUIRE(inserted);
        const auto replayed = exercise(std::move(inserted).value());
        const auto& value = std::get<OpaqueContent>(clip(replayed, {7}).content());
        REQUIRE(value.schema() == SchemaIdentity{"vendor.opaque", 9});
        REQUIRE(value.raw_json() == raw);

        auto registry = make_builtin_timeline_registry();
        REQUIRE(registry);
        auto serialized = serialize_project(replayed, registry.value());
        REQUIRE(serialized);
        REQUIRE(serialized->has_opaque_objects);
        auto decoded = deserialize_project(serialized->json, registry.value());
        REQUIRE(decoded);
        const auto& decoded_value = std::get<OpaqueContent>(clip(decoded.value(), {7}).content());
        REQUIRE(decoded_value.schema() == value.schema());
        REQUIRE(decoded_value.raw_json() == raw);
    }
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


namespace {

// A note clip dense enough that one whole-content replacement is a meaningful
// fraction of an undo budget, and small enough to stay a fast unit test.
constexpr std::size_t kDenseNoteCount = 200;
constexpr std::size_t kDenseEditCount = 40;
constexpr ItemId kDenseSequence{3};
constexpr ItemId kDenseTrack{4};
constexpr ItemId kDenseClip{5};
constexpr ItemId kFirstDenseNote{10};
constexpr std::int64_t kDenseNoteSpacing = kTicksPerQuarter / 4;

/// Pitch of the last note before any edit, and after kDenseEditCount of them.
constexpr std::uint8_t kInitialLastPitch =
    static_cast<std::uint8_t>(36 + (kDenseNoteCount - 1) % 48);
constexpr std::uint8_t kEditedLastPitch =
    static_cast<std::uint8_t>(kInitialLastPitch + kDenseEditCount);
static_assert(kEditedLastPitch < 128, "the edit sweep must stay inside the MIDI pitch domain");

std::vector<NoteEvent> dense_notes() {
    std::vector<NoteEvent> notes;
    notes.reserve(kDenseNoteCount);
    for (std::size_t index = 0; index < kDenseNoteCount; ++index)
        notes.push_back(NoteEvent{{kFirstDenseNote.value + index},
                                  {static_cast<std::int64_t>(index) * kDenseNoteSpacing},
                                  {kTicksPerQuarter / 8},
                                  1000,
                                  static_cast<std::uint8_t>(36 + index % 48),
                                  0});
    return notes;
}

Project make_dense_note_project() {
    const TickDuration span{static_cast<std::int64_t>(kDenseNoteCount) * kDenseNoteSpacing};
    auto content = MidiContent::create(dense_notes());
    REQUIRE(content);
    auto clip = Clip::create(kDenseClip, {0}, span, std::move(content).value());
    REQUIRE(clip);
    auto track = Track::create(kDenseTrack, "track", {std::move(clip).value()});
    REQUIRE(track);
    auto sequence =
        Sequence::create(kDenseSequence, "sequence", span, {std::move(track).value()});
    REQUIRE(sequence);
    auto project = Project::create({{1},
                                    "project",
                                    kFirstDenseNote.value + kDenseNoteCount + 1,
                                    kDenseSequence,
                                    {},
                                    {std::move(sequence).value()}});
    REQUIRE(project);
    return std::move(project).value();
}

std::span<const NoteEvent> dense_clip_notes(const Project& project) {
    const auto& content = project.find_sequence(kDenseSequence)
                              ->find_track(kDenseTrack)
                              ->find_clip(kDenseClip)
                              ->content();
    return std::get<MidiContent>(content).notes();
}

/// One whole-content replacement that raises the pitch of the clip's last note,
/// chaining the optimistic gate: each call expects what the previous one wrote.
ReplaceNoteContent raise_last_note(std::vector<NoteEvent>& current) {
    const std::vector<NoteEvent> expected = current;
    current.back().pitch = static_cast<std::uint8_t>(current.back().pitch + 1);
    return ReplaceNoteContent{kDenseSequence, kDenseTrack, kDenseClip, expected, current};
}

/// What one such edit charges the undo budget: the forward command and the
/// inverse the reducer derives, exactly as DocumentSession accounts for it.
std::size_t dense_edit_undo_charge() {
    auto notes = dense_notes();
    const std::array<Command, 1> forward{Command{raise_last_note(notes)}};
    return 2 * retained_size(std::span<const Command>{forward});
}

/// Limits that make the budget arithmetic explicit rather than inherited: the
/// journal is set wide enough that it never decides the outcome, so the undo
/// budget is the only ceiling either case below can reach.
SessionLimits dense_note_limits(std::size_t undo_bytes) {
    SessionLimits limits;
    limits.undo.max_retained_bytes = undo_bytes;
    limits.journal.max_retained_bytes = 256 * 1024 * 1024;
    limits.journal.max_transactions = 4096;
    limits.journal.max_commands = 8192;
    return limits;
}

} // namespace

// A whole-content note replacement is charged heavily, which is why a per-frame
// drag over a large clip cannot be expressed as a stream of them. That ceiling
// is a property of an OPEN gesture group rather than of the command: only closed
// groups are evictable, and a Single-phase edit closes on admission. So the same
// command that cannot carry a drag can carry any number of commit-on-release
// edits, and the document they produce survives a save and reopen.
//
// This case and the one after it run the same command at the same size against
// the same budget and must reach OPPOSITE outcomes. Either asserted alone would
// pass against a session with no budget at all.
TEST_CASE("Commit-on-release note edits evict closed groups and outlive the undo budget") {
    // Four edits of headroom, so eviction is the only way past the fourth.
    auto session = std::move(DocumentSession::create(make_dense_note_project(),
                                                     dense_note_limits(4 * dense_edit_undo_charge())))
                       .value();
    auto writer = std::move(session->register_writer()).value();

    auto edited = dense_notes();
    for (std::size_t index = 0; index < kDenseEditCount; ++index) {
        auto tx = session_transaction(writer, session->revision(), {raise_last_note(edited)});
        tx.gesture_phase = GesturePhase::Single;
        INFO("edit index " << index);
        REQUIRE(session->submit(writer, std::move(tx)));
    }

    const auto snapshot = session->snapshot();
    const auto notes = dense_clip_notes(*snapshot);
    REQUIRE(notes.size() == kDenseNoteCount);
    REQUIRE(notes.back().pitch == kEditedLastPitch);

    // The edits must survive the boundary a user's file crosses, with the values
    // themselves checked: a count survives a document that lost every edit.
    auto registry = make_builtin_timeline_registry();
    REQUIRE(registry);
    auto written = serialize_project(*snapshot, registry.value());
    REQUIRE(written);
    auto reopened = deserialize_project(written->json, registry.value());
    REQUIRE(reopened);

    const auto restored = dense_clip_notes(reopened.value());
    REQUIRE(restored.size() == kDenseNoteCount);
    CHECK(restored.back().id == notes.back().id);
    CHECK(restored.back().pitch == kEditedLastPitch);
    CHECK(restored.back().start == notes.back().start);
    CHECK(restored.back().duration == notes.back().duration);
    CHECK(restored.back().velocity == notes.back().velocity);
    // The edit changed one note; every other note must have crossed unchanged,
    // which a check on the edited note alone cannot see.
    const auto original = dense_notes();
    for (std::size_t index = 0; index + 1 < kDenseNoteCount; ++index) {
        INFO("note index " << index);
        CHECK(restored[index].id == original[index].id);
        CHECK(restored[index].pitch == original[index].pitch);
        CHECK(restored[index].start == original[index].start);
        CHECK(restored[index].duration == original[index].duration);
    }
}

// The control for the case above: the same command, the same size, the same
// budget, kept inside one open gesture group. Nothing is evictable while the
// group stays open, so the budget is reached and the session refuses. If this
// case ever stopped rejecting, the case above would no longer be evidence.
TEST_CASE("The same note edits inside one open gesture exhaust the undo budget") {
    auto session = std::move(DocumentSession::create(make_dense_note_project(),
                                                     dense_note_limits(4 * dense_edit_undo_charge())))
                       .value();
    auto writer = std::move(session->register_writer()).value();
    const auto group = writer.allocate_undo_group_id();

    auto edited = dense_notes();
    auto begin = session_transaction(writer, session->revision(), {raise_last_note(edited)});
    begin.undo_group = group;
    begin.gesture_phase = GesturePhase::Begin;
    REQUIRE(session->submit(writer, std::move(begin)));

    std::optional<std::size_t> rejected_at;
    for (std::size_t index = 0; index < kDenseEditCount && !rejected_at; ++index) {
        auto tx = session_transaction(writer, session->revision(), {raise_last_note(edited)});
        tx.undo_group = group;
        tx.gesture_phase = GesturePhase::Update;
        const auto committed = session->submit(writer, std::move(tx));
        if (!committed) {
            CHECK(committed.error().code == ConflictCode::UndoFull);
            rejected_at = index;
        }
    }
    REQUIRE(rejected_at);
    // Reached on budget, not on the loop running out: a rejection at the final
    // index would be indistinguishable from the loop simply ending.
    CHECK(*rejected_at < kDenseEditCount - 1);
    // The refusal is atomic — the open gesture's earlier edits are still there.
    const auto snapshot = session->snapshot();
    CHECK(dense_clip_notes(*snapshot).back().pitch > kInitialLastPitch);
}
