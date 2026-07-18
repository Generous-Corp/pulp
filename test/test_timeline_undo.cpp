#include "timeline_command_test_helpers.hpp"

#include <pulp/timeline/serialize.hpp>

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
