#include "../core/timeline/src/journal_internal.hpp"
#include "timeline_command_test_helpers.hpp"

#include <pulp/timeline/serialize.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace timeline_test;

namespace {

ContentHash hash_of(char digit) {
    const auto hash = ContentHash::from_hex(std::string(64, digit));
    REQUIRE(hash);
    return *hash;
}

Project make_durable_media_project(ContentHash source_hash = hash_of('a')) {
    auto clip =
        Clip::create({5}, {0}, {kTicksPerQuarter}, MediaRef{{2}, {7}, 11},
                     {.gain_linear = 0.5f, .fade_in_duration = 120, .fade_out_duration = 240});
    REQUIRE(clip);
    auto track = Track::create({4}, "media", {std::move(clip).value()});
    REQUIRE(track);
    auto sequence = Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter},
                                     {std::move(track).value()});
    REQUIRE(sequence);
    MediaAsset asset{{2},
                     "audio.wav",
                     128,
                     {48'000, 1},
                     source_hash,
                     AssetStoragePolicy::PreferEmbedded,
                     {{AssetLocatorKind::PackageRelative, "media/audio.wav"},
                      {AssetLocatorKind::ExternalUri, "file:///original/audio.wav"}},
                     {{"proxy",
                       hash_of('b'),
                       AssetStoragePolicy::Embedded,
                       {{AssetLocatorKind::PackageRelative, "media/proxy.wav"}}}},
                     {}};
    auto project = Project::create(
        {{1}, "durable", 6, {3}, {std::move(asset)}, {std::move(sequence).value()}});
    REQUIRE(project);
    return std::move(project).value();
}

Project make_device_chain_project(std::vector<DevicePlacement> device_chain) {
    auto track = Track::create(TrackInput{.id = {4},
                                          .name = "track",
                                          .clips = {make_note_clip({5}, {6}, 0)},
                                          .device_chain = std::move(device_chain)});
    REQUIRE(track);
    auto sequence = Sequence::create({3}, "sequence", TickDuration{8 * kTicksPerQuarter},
                                     {std::move(track).value()});
    REQUIRE(sequence);
    auto project = Project::create({{1}, "project", 9, {3}, {}, {std::move(sequence).value()}});
    REQUIRE(project);
    return std::move(project).value();
}

class ProbeJournalSink final : public JournalSink {
  public:
    pulp::runtime::Result<bool, JournalSinkError>
    append_batch(const JournalEntry& entry) noexcept override {
        ++append_calls;
        if (session)
            revision_observed_during_append = session->revision();
        if (fail_append) {
            if (append_before_failure)
                entries.push_back(entry);
            return pulp::runtime::Result<bool, JournalSinkError>(
                pulp::runtime::Err(JournalSinkError::IoError));
        }
        entries.push_back(entry);
        return pulp::runtime::Result<bool, JournalSinkError>(pulp::runtime::Ok(true));
    }

    pulp::runtime::Result<bool, JournalSinkError>
    checkpoint(const Project& snapshot, DocumentRevision durable_revision) noexcept override {
        ++checkpoint_calls;
        if (fail_checkpoint)
            return pulp::runtime::Result<bool, JournalSinkError>(
                pulp::runtime::Err(JournalSinkError::IoError));
        checkpoints.push_back(durable_revision);
        checkpoint_snapshots.push_back(snapshot);
        return pulp::runtime::Result<bool, JournalSinkError>(pulp::runtime::Ok(true));
    }

    DocumentSession* session = nullptr;
    bool fail_append = false;
    bool append_before_failure = false;
    bool fail_checkpoint = false;
    std::size_t append_calls = 0;
    std::size_t checkpoint_calls = 0;
    DocumentRevision revision_observed_during_append{};
    std::vector<JournalEntry> entries;
    std::vector<DocumentRevision> checkpoints;
    std::vector<Project> checkpoint_snapshots;
};

} // namespace

TEST_CASE("Timeline journal sink is durable before a transaction is published") {
    auto sink = std::make_shared<ProbeJournalSink>();
    auto session = std::move(DocumentSession::create(make_project(), {}, sink)).value();
    sink->session = session.get();
    auto writer = std::move(session->register_writer()).value();
    auto edit = session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});

    REQUIRE(session->submit(writer, std::move(edit)));

    REQUIRE(sink->append_calls == 1);
    REQUIRE(sink->entries.size() == 1);
    REQUIRE(sink->entries[0].before.value == 0);
    REQUIRE(sink->entries[0].after.value == 1);
    REQUIRE(sink->revision_observed_during_append.value == 0);
    REQUIRE(session->revision().value == 1);
    REQUIRE(velocity(*session->snapshot()) == 2000);
    REQUIRE(sink->checkpoint_calls == 1);
    REQUIRE(sink->checkpoints == std::vector<DocumentRevision>{{0}});
    REQUIRE(velocity(sink->checkpoint_snapshots.front()) == 1000);
}

TEST_CASE("Timeline journal sink failure rejects session creation") {
    auto sink = std::make_shared<ProbeJournalSink>();
    sink->fail_checkpoint = true;

    auto rejected = DocumentSession::create(make_project(), {}, sink);

    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::JournalDurability);
    REQUIRE(sink->checkpoint_calls == 1);
    REQUIRE(sink->checkpoints.empty());
}

TEST_CASE("Timeline journal sink failure poisons the session after an ambiguous append") {
    auto sink = std::make_shared<ProbeJournalSink>();
    sink->fail_append = true;
    sink->append_before_failure = true;
    auto session = std::move(DocumentSession::create(make_project(), {}, sink)).value();
    auto writer = std::move(session->register_writer()).value();
    auto edit = session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    auto retry = edit;

    auto rejected = session->submit(writer, std::move(edit));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::JournalDurability);
    REQUIRE(session->revision().value == 0);
    REQUIRE(velocity(*session->snapshot()) == 1000);
    REQUIRE(session->journal().entries().empty());
    REQUIRE(sink->entries.size() == 1);

    sink->fail_append = false;
    auto poisoned = session->submit(writer, std::move(retry));
    REQUIRE_FALSE(poisoned);
    REQUIRE(poisoned.error().code == ConflictCode::JournalDurability);
    REQUIRE(session->revision().value == 0);
    REQUIRE(sink->append_calls == 1);
    REQUIRE(sink->entries.size() == 1);
}

TEST_CASE("Timeline journal checkpoint truncates only after the sink accepts it") {
    auto sink = std::make_shared<ProbeJournalSink>();
    auto session = std::move(DocumentSession::create(make_project(), {}, sink)).value();
    auto writer = std::move(session->register_writer()).value();
    auto edit = session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    auto retry = edit;
    auto collision = edit;
    collision.expected_revision = {99};
    REQUIRE(session->submit(writer, std::move(edit)));

    REQUIRE_FALSE(session->checkpoint({2}));
    REQUIRE(sink->checkpoint_calls == 1);

    sink->fail_checkpoint = true;
    REQUIRE_FALSE(session->checkpoint({1}));
    REQUIRE(sink->checkpoint_calls == 2);
    REQUIRE(session->journal().entries().size() == 1);
    REQUIRE(session->journal().base_revision().value == 0);

    sink->fail_checkpoint = false;
    REQUIRE_FALSE(session->checkpoint({1}));
    REQUIRE(sink->checkpoint_calls == 2);
    REQUIRE(sink->checkpoints == std::vector<DocumentRevision>{{0}});
    REQUIRE(session->journal().entries().size() == 1);
    REQUIRE(session->journal().base_revision().value == 0);

    auto cached = session->submit(writer, std::move(retry));
    REQUIRE(cached);
    REQUIRE(cached->revision.value == 1);
    auto collided = session->submit(writer, std::move(collision));
    REQUIRE_FALSE(collided);
    REQUIRE(collided.error().code == ConflictCode::TransactionIdCollision);
}

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

TEST_CASE("Timeline journal checkpoint equality includes device-chain order") {
    const auto checkpoint = make_device_chain_project({{{7}}, {{8}}});
    auto session = std::move(DocumentSession::create(checkpoint)).value();
    auto writer = std::move(session->register_writer()).value();
    auto edit = session_transaction(writer, {}, {SetNoteVelocity{{3}, {4}, {5}, {6}, 1000, 2000}});
    REQUIRE(session->submit(writer, std::move(edit)));

    const auto reordered = make_device_chain_project({{{8}}, {{7}}});
    auto rejected = session->journal().replay(reordered, {});
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::ModelInvariant);
}

TEST_CASE("Timeline commands and replay preserve durable asset metadata") {
    const auto checkpoint = make_durable_media_project();
    auto session = std::move(DocumentSession::create(checkpoint)).value();
    auto writer = std::move(session->register_writer()).value();
    const auto before = clip(*session->snapshot()).time_range();
    ClipTimeRange after = MusicalTimeRange{{2 * kTicksPerQuarter}, {kTicksPerQuarter}};
    auto move = session_transaction(writer, {}, {MoveClip{{3}, {4}, {5}, before, after}});
    REQUIRE(session->submit(writer, std::move(move)));

    const auto journal = session->journal();
    auto replayed = journal.replay(checkpoint, {});
    REQUIRE(replayed);
    REQUIRE(replayed->assets().size() == 1);
    const auto& asset = replayed->assets()[0];
    REQUIRE(asset.content_hash == hash_of('a'));
    REQUIRE(asset.storage_policy == AssetStoragePolicy::PreferEmbedded);
    REQUIRE(asset.locators.size() == 2);
    REQUIRE(asset.representations.size() == 1);
    REQUIRE(asset.representations[0].content_hash == hash_of('b'));
    REQUIRE(equivalent(clip(*replayed), clip(*session->snapshot())));
    REQUIRE(clip(*replayed).playback_properties() == ClipPlaybackProperties{0.5f, 120, 240});

    auto registry = make_builtin_timeline_registry();
    REQUIRE(registry);
    auto serialized = serialize_project(*session->snapshot(), registry.value());
    REQUIRE(serialized);
    auto decoded = deserialize_project(serialized->json, registry.value());
    REQUIRE(decoded);
    REQUIRE(decoded->assets()[0].content_hash == asset.content_hash);
    REQUIRE(decoded->assets()[0].locators == asset.locators);
    REQUIRE(decoded->assets()[0].representations.size() == 1);
    REQUIRE(decoded->assets()[0].representations[0].content_hash ==
            asset.representations[0].content_hash);
    REQUIRE(equivalent(clip(decoded.value()), clip(*session->snapshot())));

    auto wrong_checkpoint = make_durable_media_project(hash_of('c'));
    auto rejected = journal.replay(wrong_checkpoint, {});
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::ModelInvariant);
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

TEST_CASE("Serialized checkpoints retain tombstones for replay and exact reactivation") {
    const auto initial = make_project();
    auto session = std::move(DocumentSession::create(initial)).value();
    auto writer = std::move(session->register_writer()).value();
    auto remove = session_transaction(writer, {}, {RemoveClip{{3}, {4}, {5}}});
    REQUIRE(session->submit(writer, std::move(remove)));
    const auto checkpoint = *session->snapshot();
    REQUIRE(session->checkpoint({1}));
    REQUIRE(session->undo(writer));

    auto registry = make_builtin_timeline_registry();
    REQUIRE(registry);
    auto encoded = serialize_project(checkpoint, registry.value());
    REQUIRE(encoded);
    auto decoded = deserialize_project(encoded->json, registry.value());
    REQUIRE(decoded);
    REQUIRE(decoded->locate({5}));
    REQUIRE_FALSE(decoded->locate({5})->active);
    REQUIRE(decoded->locate({6}));
    REQUIRE_FALSE(decoded->locate({6})->active);

    auto malformed = encoded->json;
    const std::string valid_tombstone =
        R"({"active":false,"clip_id":"5","id":"5","kind":"clip","parent_id":"4","sequence_id":"3","track_id":"4"})";
    const std::string malformed_tombstone =
        R"({"active":false,"clip_id":"0","id":"5","kind":"clip","parent_id":"4","sequence_id":"3","track_id":"4"})";
    const auto tombstone_position = malformed.find(valid_tombstone);
    REQUIRE(tombstone_position != std::string::npos);
    malformed.replace(tombstone_position, valid_tombstone.size(), malformed_tombstone);
    auto rejected = deserialize_project(malformed, registry.value());
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == PersistenceErrorCode::ModelRejected);
    REQUIRE(rejected.error().model_error);
    REQUIRE(rejected.error().model_error->code == ModelErrorCode::InvalidSchemaIdentity);

    malformed = encoded->json;
    const std::string orphaned_tombstone =
        R"({"active":false,"clip_id":"5","id":"5","kind":"clip","parent_id":"4","sequence_id":"2","track_id":"4"})";
    const auto orphan_position = malformed.find(valid_tombstone);
    REQUIRE(orphan_position != std::string::npos);
    malformed.replace(orphan_position, valid_tombstone.size(), orphaned_tombstone);
    rejected = deserialize_project(malformed, registry.value());
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == PersistenceErrorCode::ModelRejected);
    REQUIRE(rejected.error().model_error);
    REQUIRE(rejected.error().model_error->code == ModelErrorCode::InvalidSchemaIdentity);

    auto restored = session->journal().replay(decoded.value(), {1});
    REQUIRE(restored);
    REQUIRE(restored->find_sequence({3})->find_track({4})->find_clip({5}));
    REQUIRE(restored->locate({5})->active);
    REQUIRE(restored->locate({6})->active);
}

namespace {

MediaAsset make_recorded_asset(ItemId id, ContentHash source_hash) {
    return MediaAsset{id,
                      "recorded.wav",
                      960,
                      {48'000, 1},
                      source_hash,
                      AssetStoragePolicy::PreferEmbedded,
                      {{AssetLocatorKind::PackageRelative, "media/recorded.wav"}},
                      {{"proxy",
                        hash_of('b'),
                        AssetStoragePolicy::Embedded,
                        {{AssetLocatorKind::PackageRelative, "media/recorded.proxy.wav"}}}},
                      {}};
}

} // namespace

TEST_CASE("CreateAsset appends a sealed asset preserving its content hash") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    REQUIRE(session->snapshot()->assets().empty());
    const auto next_before = session->snapshot()->next_item_id();

    auto created = make_recorded_asset({next_before}, hash_of('e'));
    auto tx = session_transaction(writer, {}, {CreateAsset{created}});
    REQUIRE(session->submit(writer, std::move(tx)));

    const auto snapshot = session->snapshot();
    REQUIRE(snapshot->assets().size() == 1);
    const auto& asset = snapshot->assets()[0];
    REQUIRE(asset.id == ItemId{next_before});
    REQUIRE(asset.content_hash == hash_of('e'));
    REQUIRE(asset.representations.size() == 1);
    REQUIRE(asset.representations[0].content_hash == hash_of('b'));
    // The asset is now an active, project-owned identity and consumed one id.
    const auto located = snapshot->locate({next_before});
    REQUIRE(located);
    REQUIRE(located->kind == ItemKind::Asset);
    REQUIRE(located->active);
    REQUIRE(snapshot->next_item_id() == next_before + 1);
}

TEST_CASE("CreateAsset with an invalid content hash is rejected fail closed") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();
    const auto next_before = session->snapshot()->next_item_id();

    MediaAsset unsealed{{next_before},
                        "unsealed.wav",
                        480,
                        {48'000, 1},
                        ContentHash{},
                        AssetStoragePolicy::External,
                        {},
                        {},
                        {}};
    REQUIRE_FALSE(unsealed.content_hash.valid());
    auto tx = session_transaction(writer, {}, {CreateAsset{unsealed}});
    auto rejected = session->submit(writer, std::move(tx));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::ModelInvariant);
    REQUIRE(rejected.error().model_error);
    REQUIRE(rejected.error().model_error->code == ModelErrorCode::InvalidContentHash);
    // Fail closed: no asset, revision, or journal entry survives the rejection.
    REQUIRE(session->snapshot()->assets().empty());
    REQUIRE(session->snapshot()->next_item_id() == next_before);
    REQUIRE(session->revision() == DocumentRevision{});
    REQUIRE(session->journal().entries().empty());
}

TEST_CASE("Journal replay reproduces a created asset byte-identically") {
    const auto checkpoint = make_project();
    auto session = std::move(DocumentSession::create(checkpoint)).value();
    auto writer = std::move(session->register_writer()).value();

    auto created = make_recorded_asset({checkpoint.next_item_id()}, hash_of('e'));
    REQUIRE(session->submit(writer, session_transaction(writer, {}, {CreateAsset{created}})));
    const auto committed = session->snapshot();

    auto replayed = session->journal().replay(checkpoint, {});
    REQUIRE(replayed);
    REQUIRE(replayed->assets().size() == 1);
    REQUIRE(replayed->assets()[0].content_hash == hash_of('e'));
    REQUIRE(replayed->next_item_id() == committed->next_item_id());

    // Determinism proof: the committed and replayed documents serialize to
    // byte-identical canonical JSON. Replay references the sealed asset by its
    // content_hash and never re-derives it.
    auto registry = make_builtin_timeline_registry();
    REQUIRE(registry);
    auto committed_json = serialize_project(*committed, registry.value());
    auto replayed_json = serialize_project(*replayed, registry.value());
    REQUIRE(committed_json);
    REQUIRE(replayed_json);
    REQUIRE(committed_json->json == replayed_json->json);
}

TEST_CASE("A created asset round-trips through the project schema") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto writer = std::move(session->register_writer()).value();

    auto created = make_recorded_asset({session->snapshot()->next_item_id()}, hash_of('f'));
    REQUIRE(session->submit(writer, session_transaction(writer, {}, {CreateAsset{created}})));

    auto registry = make_builtin_timeline_registry();
    REQUIRE(registry);
    auto serialized = serialize_project(*session->snapshot(), registry.value());
    REQUIRE(serialized);
    auto decoded = deserialize_project(serialized->json, registry.value());
    REQUIRE(decoded);
    REQUIRE(decoded->assets().size() == 1);
    const auto& asset = decoded->assets()[0];
    REQUIRE(asset.content_hash == hash_of('f'));
    REQUIRE(asset.locators.size() == 1);
    REQUIRE(asset.representations.size() == 1);
    REQUIRE(asset.representations[0].content_hash == hash_of('b'));
    REQUIRE(decoded->next_item_id() == session->snapshot()->next_item_id());
}
