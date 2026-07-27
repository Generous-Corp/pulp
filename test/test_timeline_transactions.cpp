#include "../core/timeline/src/session_nonce_test_access.hpp"
#include "../core/timeline/src/writer_token_test_access.hpp"
#include "timeline_command_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>
#include <mutex>
#include <thread>
#include <type_traits>
#include <unordered_set>

using namespace timeline_test;

static_assert(!std::is_copy_constructible_v<WriterToken>);
static_assert(!std::is_copy_assignable_v<WriterToken>);
static_assert(std::is_move_constructible_v<WriterToken>);

TEST_CASE("Map command failure leaves session revision journal and snapshot atomic") {
    const auto initial = make_project();
    auto session = std::move(DocumentSession::create(initial)).value();
    auto writer = std::move(session->register_writer()).value();
    auto tx = session_transaction(
        writer, {},
        {SetTempoMap{initial.tempo_map(), make_tempo_map(88.0)},
         SetMeterMap{make_meter_map({7, 8}), make_meter_map({3, 4})}});
    auto rejected = session->submit(writer, std::move(tx));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::ExpectedValueMismatch);
    REQUIRE(session->revision() == DocumentRevision{});
    REQUIRE(session->journal().entries().empty());
    REQUIRE(session->snapshot()->tempo_map() == initial.tempo_map());
    REQUIRE(session->snapshot()->meter_map() == initial.meter_map());
}

TEST_CASE("Timeline transaction rejection is atomic at every command position") {
    const auto original = make_project();
    const auto range = clip(original).time_range();
    ClipTimeRange moved = MusicalTimeRange{{2 * kTicksPerQuarter}, {kTicksPerQuarter}};
    auto tx = transaction(
        {1}, 1, 1, {},
        {MoveClip{{3}, {4}, {5}, range, moved}, SetNoteVelocity{{3}, {4}, {5}, {999}, 1000, 2000}});
    auto rejected = reduce_transaction(original, tx);
    REQUIRE_FALSE(rejected);
    REQUIRE(clip(original).start().value == 0);
    REQUIRE(original.shares_storage_with(original));
}

TEST_CASE("Document session rejects stale writers and caches exact retries") {
    auto session_result = DocumentSession::create(make_project());
    REQUIRE(session_result);
    auto session = std::move(session_result).value();
    auto first_writer = session->register_writer();
    auto second_writer = session->register_writer();
    REQUIRE(first_writer);
    REQUIRE(second_writer);
    auto first = session_transaction(first_writer.value(), {},
                                     {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    const auto retry = first;
    auto committed = session->submit(first_writer.value(), first);
    REQUIRE(committed);
    REQUIRE(committed->revision.value == 1);
    auto retried = session->submit(first_writer.value(), retry);
    REQUIRE(retried);
    REQUIRE(retried->revision.value == 1);
    REQUIRE(session->journal().entries().size() == 1);

    auto collision = retry;
    std::get<SetNoteVelocity>(collision.commands[0].command).replacement_velocity = 3000;
    auto collided = session->submit(first_writer.value(), std::move(collision));
    REQUIRE_FALSE(collided);
    REQUIRE(collided.error().code == ConflictCode::TransactionIdCollision);

    auto command_collision = session_transaction(first_writer.value(), session->revision(),
                                                 {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 3000}});
    command_collision.commands[0].id.sequence = 1;
    auto command_rejected = session->submit(first_writer.value(), std::move(command_collision));
    REQUIRE_FALSE(command_rejected);
    REQUIRE(command_rejected.error().code == ConflictCode::CommandIdCollision);

    auto stale = session_transaction(second_writer.value(), {},
                                     {SetNoteVelocity{{3}, {4}, {5}, {6}, 2000, 3000}});
    auto rejected = session->submit(second_writer.value(), std::move(stale));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::StaleRevision);
}

TEST_CASE("Document session serializes concurrent writers into one revision order") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto left = std::move(session->register_writer()).value();
    auto right = std::move(session->register_writer()).value();
    std::atomic<int> successes{0};
    std::atomic<int> unexpected_errors{0};
    auto run = [&](WriterToken& writer, std::uint16_t desired) {
        for (int attempt = 0; attempt < 64; ++attempt) {
            const auto view = session->current();
            const auto current = velocity(*view.snapshot);
            auto tx = session_transaction(writer, view.revision,
                                          {SetNoteVelocity{{3}, {4}, {5}, {6}, current, desired}});
            auto result = session->submit(writer, std::move(tx));
            if (result) {
                ++successes;
                return;
            }
            if (result.error().code != ConflictCode::StaleRevision &&
                result.error().code != ConflictCode::ExpectedValueMismatch) {
                ++unexpected_errors;
                return;
            }
        }
    };
    std::thread a(run, std::ref(left), 2000);
    std::thread b(run, std::ref(right), 3000);
    a.join();
    b.join();
    REQUIRE(unexpected_errors == 0);
    REQUIRE(successes == 2);
    REQUIRE(session->revision().value == 2);
    REQUIRE(session->journal().entries().size() == 2);
}

TEST_CASE("Writer tokens are bound to their authoritative document session") {
    auto first = std::move(DocumentSession::create(make_project())).value();
    auto second = std::move(DocumentSession::create(make_project())).value();
    auto foreign = std::move(first->register_writer()).value();
    auto tx = session_transaction(foreign, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    auto rejected = second->submit(foreign, std::move(tx));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::InvalidIdentifier);
    REQUIRE(second->revision().value == 0);
}

TEST_CASE("Writer token ID allocation is race-free and unique") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    std::mutex mutex;
    std::unordered_set<std::uint64_t> ids;
    auto allocate = [&] {
        for (int i = 0; i < 2000; ++i) {
            const auto id = writer.allocate_command_id();
            std::lock_guard lock(mutex);
            REQUIRE(id.valid());
            ids.insert(id.sequence);
        }
    };
    std::thread a(allocate);
    std::thread b(allocate);
    a.join();
    b.join();
    REQUIRE(ids.size() == 4000);
}

TEST_CASE("Writer token ID exhaustion saturates under concurrent allocation") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    constexpr auto last = std::numeric_limits<std::uint64_t>::max() - 1;
    pulp::timeline::detail::WriterTokenTestAccess::set_next_ids(writer, last, last, last);
    std::array<CommandId, 2> results;
    std::thread a([&] { results[0] = writer.allocate_command_id(); });
    std::thread b([&] { results[1] = writer.allocate_command_id(); });
    a.join();
    b.join();
    REQUIRE(results[0].valid() != results[1].valid());
    const auto valid = results[0].valid() ? results[0] : results[1];
    REQUIRE(valid.sequence == last);
    REQUIRE_FALSE(writer.allocate_command_id().valid());
    REQUIRE(writer.allocate_transaction_id().sequence == last);
    REQUIRE_FALSE(writer.allocate_transaction_id().valid());
    REQUIRE(writer.allocate_undo_group_id().sequence == last);
    REQUIRE_FALSE(writer.allocate_undo_group_id().valid());
}

TEST_CASE("Document session nonce exhaustion saturates under concurrent creation") {
    constexpr auto last = std::numeric_limits<std::uint64_t>::max() - 1;
    const auto previous = pulp::timeline::detail::SessionNonceTestAccess::exchange_next(last);
    struct RestoreNonce {
        std::uint64_t value;
        ~RestoreNonce() {
            pulp::timeline::detail::SessionNonceTestAccess::exchange_next(value);
        }
    } restore{previous};

    std::array<bool, 2> created{};
    std::thread a([&] { created[0] = static_cast<bool>(DocumentSession::create(make_project())); });
    std::thread b([&] { created[1] = static_cast<bool>(DocumentSession::create(make_project())); });
    a.join();
    b.join();
    REQUIRE(created[0] != created[1]);
    auto exhausted = DocumentSession::create(make_project());
    REQUIRE_FALSE(exhausted);
    REQUIRE(exhausted.error().code == ConflictCode::SequenceExhausted);
}

TEST_CASE("CreateAsset is undoable and redoable through its RemoveAsset inverse") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    const auto asset_id = session->snapshot()->next_item_id();
    MediaAsset asset{{asset_id}, "take.wav", 240, {48'000, 1}, content_hash('c'),
                     AssetStoragePolicy::External, {}, {}, {}};
    REQUIRE(session->submit(writer, session_transaction(writer, {}, {CreateAsset{asset}})));
    REQUIRE(session->snapshot()->assets().size() == 1);
    REQUIRE(session->can_undo());

    auto undone = session->undo(writer);
    REQUIRE(undone);
    REQUIRE(session->snapshot()->assets().empty());
    // Undo removes the asset but tombstones its identity rather than reusing it.
    const auto tombstone = session->snapshot()->locate({asset_id});
    REQUIRE(tombstone);
    REQUIRE_FALSE(tombstone->active);

    auto redone = session->redo(writer);
    REQUIRE(redone);
    REQUIRE(session->snapshot()->assets().size() == 1);
    REQUIRE(session->snapshot()->assets()[0].content_hash == content_hash('c'));
    REQUIRE(session->snapshot()->locate({asset_id})->active);
}

TEST_CASE("Scene and slot inserts are undoable with stable identity ownership") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();

    Scene scene{{7}, "launch", {}};
    REQUIRE(session->submit(
        writer, session_transaction(writer, {}, {InsertScene{{3}, scene}})));
    REQUIRE(session->snapshot()->find_sequence({3})->find_scene({7}));
    REQUIRE(session->snapshot()->locate({7})->kind == ItemKind::Scene);

    Slot slot{{8}, {5}, launch_every_quarters(1), {}};
    REQUIRE(session->submit(
        writer, session_transaction(writer, session->revision(), {InsertSlot{{3}, {7}, slot}})));
    REQUIRE(session->snapshot()->find_sequence({3})->find_slot({8}));
    REQUIRE(session->snapshot()->locate({8})->kind == ItemKind::Slot);
    REQUIRE(session->snapshot()->locate({8})->parent_id == ItemId{7});

    REQUIRE(session->undo(writer));
    REQUIRE_FALSE(session->snapshot()->find_sequence({3})->find_slot({8}));
    REQUIRE_FALSE(session->snapshot()->locate({8})->active);
    REQUIRE(session->redo(writer));
    REQUIRE(session->snapshot()->find_sequence({3})->find_slot({8}));

    REQUIRE(session->undo(writer));
    REQUIRE(session->undo(writer));
    REQUIRE_FALSE(session->snapshot()->find_sequence({3})->find_scene({7}));
    REQUIRE_FALSE(session->snapshot()->locate({7})->active);
    REQUIRE(session->redo(writer));
    REQUIRE(session->snapshot()->find_sequence({3})->find_scene({7}));

    Scene populated{{9}, "nested", {Slot{{10}, {5}, launch_immediate(), {}}}};
    REQUIRE(session->submit(writer, session_transaction(writer, session->revision(),
                                                        {InsertScene{{3}, populated}})));
    REQUIRE(session->snapshot()->locate({9})->active);
    REQUIRE(session->snapshot()->locate({10})->active);
    REQUIRE(session->undo(writer));
    REQUIRE_FALSE(session->snapshot()->locate({9})->active);
    REQUIRE_FALSE(session->snapshot()->locate({10})->active);
    REQUIRE(session->redo(writer));
    REQUIRE(session->snapshot()->locate({9})->active);
    REQUIRE(session->snapshot()->locate({10})->active);
}

TEST_CASE("Scene and slot removal inverses restore exact authored order") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();

    REQUIRE(session->submit(
        writer, session_transaction(writer, {},
                                    {InsertScene{{3}, Scene{{7}, "first", {}}},
                                     InsertScene{{3}, Scene{{8}, "middle", {}}},
                                     InsertScene{{3}, Scene{{9}, "last", {}}},
                                     InsertSlot{{3}, {7},
                                                Slot{{10}, {5}, launch_immediate(), {}}},
                                     InsertSlot{{3}, {7},
                                                Slot{{11}, {5}, launch_immediate(), {}}},
                                     InsertSlot{{3}, {7},
                                                Slot{{12}, {5}, launch_immediate(), {}}}})));

    const auto revision_before_anchor_rejections = session->revision();
    auto missing_scene_anchor = session->submit(
        writer, session_transaction(writer, session->revision(),
                                    {InsertScene{{3}, Scene{{13}, "unplaced", {}}, ItemId{999}}}));
    REQUIRE_FALSE(missing_scene_anchor);
    REQUIRE(missing_scene_anchor.error().code == ConflictCode::TargetMissing);
    REQUIRE(session->revision() == revision_before_anchor_rejections);
    REQUIRE_FALSE(session->snapshot()->locate({13}));

    auto missing_slot_anchor = session->submit(
        writer, session_transaction(writer, session->revision(),
                                    {InsertSlot{{3}, {7},
                                                Slot{{14}, {5}, launch_immediate(), {}},
                                                ItemId{999}}}));
    REQUIRE_FALSE(missing_slot_anchor);
    REQUIRE(missing_slot_anchor.error().code == ConflictCode::TargetMissing);
    REQUIRE(session->revision() == revision_before_anchor_rejections);
    REQUIRE_FALSE(session->snapshot()->locate({14}));

    REQUIRE(session->submit(
        writer, session_transaction(writer, session->revision(), {RemoveScene{{3}, {8}}})));
    auto scenes = session->snapshot()->find_sequence({3})->scenes();
    REQUIRE(scenes.size() == 2);
    REQUIRE(scenes[0].id == ItemId{7});
    REQUIRE(scenes[1].id == ItemId{9});

    REQUIRE(session->undo(writer));
    scenes = session->snapshot()->find_sequence({3})->scenes();
    REQUIRE(scenes.size() == 3);
    REQUIRE(scenes[0].id == ItemId{7});
    REQUIRE(scenes[1].id == ItemId{8});
    REQUIRE(scenes[2].id == ItemId{9});
    REQUIRE(session->redo(writer));
    REQUIRE(session->undo(writer));

    REQUIRE(session->submit(
        writer, session_transaction(writer, session->revision(), {RemoveSlot{{3}, {7}, {11}}})));
    auto slots = session->snapshot()->find_sequence({3})->find_scene({7})->slots;
    REQUIRE(slots.size() == 2);
    REQUIRE(slots[0].id == ItemId{10});
    REQUIRE(slots[1].id == ItemId{12});

    REQUIRE(session->undo(writer));
    slots = session->snapshot()->find_sequence({3})->find_scene({7})->slots;
    REQUIRE(slots.size() == 3);
    REQUIRE(slots[0].id == ItemId{10});
    REQUIRE(slots[1].id == ItemId{11});
    REQUIRE(slots[2].id == ItemId{12});
    REQUIRE(session->redo(writer));
    REQUIRE(session->undo(writer));

    REQUIRE(session->submit(
        writer, session_transaction(writer, session->revision(),
                                    {RemoveSlot{{3}, {7}, {11}},
                                     RemoveSlot{{3}, {7}, {12}}})));
    REQUIRE(session->undo(writer));
    slots = session->snapshot()->find_sequence({3})->find_scene({7})->slots;
    REQUIRE(slots.size() == 3);
    REQUIRE(slots[0].id == ItemId{10});
    REQUIRE(slots[1].id == ItemId{11});
    REQUIRE(slots[2].id == ItemId{12});

    REQUIRE(session->submit(
        writer, session_transaction(writer, session->revision(),
                                    {RemoveScene{{3}, {8}}, RemoveScene{{3}, {9}}})));
    REQUIRE(session->undo(writer));
    scenes = session->snapshot()->find_sequence({3})->scenes();
    REQUIRE(scenes.size() == 3);
    REQUIRE(scenes[0].id == ItemId{7});
    REQUIRE(scenes[1].id == ItemId{8});
    REQUIRE(scenes[2].id == ItemId{9});
}

TEST_CASE("Scene and slot removals stay path-copy bounded through session transactions") {
    constexpr std::size_t scene_count = 4096;
    constexpr std::size_t slots_per_scene = 4;
    std::vector<Scene> scenes;
    scenes.reserve(scene_count);
    for (std::size_t scene_index = 0; scene_index < scene_count; ++scene_index) {
        std::vector<Slot> slots;
        slots.reserve(slots_per_scene);
        for (std::size_t slot_index = 0; slot_index < slots_per_scene; ++slot_index)
            slots.push_back(Slot{{1'000'000 + scene_index * slots_per_scene + slot_index},
                                 {},
                                 launch_immediate(),
                                 {}});
        scenes.push_back(Scene{{100'000 + scene_index}, "scene", SlotList(std::move(slots))});
    }
    auto sequence = Sequence::create(SequenceInput{
        .id = {3},
        .name = "large launcher",
        .scenes = std::move(scenes),
    });
    REQUIRE(sequence);
    auto project = Project::create(ProjectInput{
        .id = {1},
        .name = "project",
        .next_item_id = 3'000'000,
        .root_sequence_id = {3},
        .sequences = {std::move(sequence).value()},
    });
    REQUIRE(project);
    auto session = std::move(DocumentSession::create(std::move(project).value())).value();
    auto writer = std::move(session->register_writer()).value();
    const auto original = session->snapshot();

    const ItemId removed_scene{100'000 + scene_count / 2};
    const auto before_scene = Sequence::launcher_index_stats();
    REQUIRE(session->submit(
        writer, session_transaction(writer, {}, {RemoveScene{{3}, removed_scene}})));
    const auto after_scene = Sequence::launcher_index_stats();
    const auto scene_snapshot = session->snapshot();
    const auto* scene_edited = scene_snapshot->find_sequence({3});
    REQUIRE(after_scene.nodes_created - before_scene.nodes_created < 512);
    REQUIRE(scene_edited->shared_launcher_nodes_with(*original->find_sequence({3})) > 30'000);
    REQUIRE(scene_edited->find_scene(removed_scene) == nullptr);
    REQUIRE(session->journal().entries().size() == 1);

    const ItemId retained_scene{100'000 + scene_count / 2 + 1};
    const ItemId removed_slot{1'000'000 + (scene_count / 2 + 1) * slots_per_scene + 2};
    const auto before_slot = Sequence::launcher_index_stats();
    REQUIRE(session->submit(
        writer, session_transaction(writer, session->revision(),
                                    {RemoveSlot{{3}, retained_scene, removed_slot}})));
    const auto after_slot = Sequence::launcher_index_stats();
    const auto slot_snapshot = session->snapshot();
    const auto* slot_edited = slot_snapshot->find_sequence({3});
    REQUIRE(after_slot.nodes_created - before_slot.nodes_created < 256);
    REQUIRE(slot_edited->shared_launcher_nodes_with(*scene_edited) > 30'000);
    REQUIRE(slot_edited->find_slot(removed_slot) == nullptr);
    REQUIRE(session->journal().entries().size() == 2);

    REQUIRE(session->undo(writer));
    REQUIRE(session->snapshot()->find_sequence({3})->find_slot(removed_slot));
    REQUIRE(session->undo(writer));
    REQUIRE(session->snapshot()->find_sequence({3})->find_scene(removed_scene));
}

TEST_CASE("CreateAsset rejects an id that is already live") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    const auto asset_id = session->snapshot()->next_item_id();
    MediaAsset asset{{asset_id}, "first.wav", 100, {48'000, 1}, content_hash('a'),
                     AssetStoragePolicy::External, {}, {}, {}};
    REQUIRE(session->submit(writer, session_transaction(writer, {}, {CreateAsset{asset}})));

    MediaAsset again{{asset_id}, "second.wav", 100, {48'000, 1}, content_hash('b'),
                     AssetStoragePolicy::External, {}, {}, {}};
    auto rejected =
        session->submit(writer, session_transaction(writer, session->revision(),
                                                    {CreateAsset{again}}));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::IdentityNotAvailable);
    REQUIRE(session->snapshot()->assets().size() == 1);
    REQUIRE(session->snapshot()->assets()[0].content_hash == content_hash('a'));
}

TEST_CASE("RemoveAsset is rejected while a clip still references the asset") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    const auto asset_id = session->snapshot()->next_item_id();
    MediaAsset asset{{asset_id}, "ref.wav", 480, {48'000, 1}, content_hash('a'),
                     AssetStoragePolicy::External, {}, {}, {}};
    REQUIRE(session->submit(writer, session_transaction(writer, {}, {CreateAsset{asset}})));

    const auto clip_id = session->snapshot()->next_item_id();
    auto media_clip = Clip::create({clip_id}, {2 * kTicksPerQuarter}, {kTicksPerQuarter},
                                   MediaRef{{asset_id}, {0}, 100});
    REQUIRE(media_clip);
    REQUIRE(session->submit(
        writer, session_transaction(writer, session->revision(),
                                    {InsertClip{{3}, {4}, std::move(media_clip).value()}})));

    auto rejected = session->submit(
        writer, session_transaction(writer, session->revision(), {RemoveAsset{{asset_id}}}));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::ModelInvariant);
    REQUIRE(rejected.error().model_error);
    REQUIRE(rejected.error().model_error->code == ModelErrorCode::MissingAsset);
    // Fail closed: the referenced asset stays in the document.
    REQUIRE(session->snapshot()->assets().size() == 1);
    REQUIRE(session->snapshot()->find_asset({asset_id}) != nullptr);
}
