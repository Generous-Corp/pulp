#include "timeline_command_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <utility>
#include <variant>

using namespace timeline_test;

namespace {

class ToggleJournalSink final : public JournalSink {
  public:
    pulp::runtime::Result<bool, JournalSinkError>
    append_batch(const JournalEntry&) noexcept override {
        ++append_calls;
        if (fail_append)
            return pulp::runtime::Result<bool, JournalSinkError>(
                pulp::runtime::Err(JournalSinkError::IoError));
        return pulp::runtime::Result<bool, JournalSinkError>(pulp::runtime::Ok(true));
    }

    pulp::runtime::Result<bool, JournalSinkError>
    checkpoint(const Project&, DocumentRevision) noexcept override {
        return pulp::runtime::Result<bool, JournalSinkError>(pulp::runtime::Ok(true));
    }

    pulp::runtime::Result<bool, JournalSinkError>
    validate_restore(const Project&, DocumentRevision) noexcept override {
        return pulp::runtime::Result<bool, JournalSinkError>(pulp::runtime::Ok(true));
    }

    bool fail_append = false;
    std::size_t append_calls = 0;
};

MediaAsset make_asset(ItemId id) {
    return MediaAsset{id,
                      "recorded.wav",
                      960,
                      {48'000, 1},
                      content_hash('e'),
                      AssetStoragePolicy::PreferEmbedded,
                      {{AssetLocatorKind::PackageRelative, "media/recorded.wav"}},
                      {},
                      {}};
}

// A session holding one asset, plus the writer that created it.
struct SeededSession {
    std::unique_ptr<DocumentSession> session;
    ItemId asset_id;
};

SeededSession seed_asset() {
    auto session = std::move(DocumentSession::create(make_project())).value();
    auto author = std::move(session->register_writer()).value();
    const ItemId asset_id{session->snapshot()->next_item_id()};
    REQUIRE(session->submit(author, session_transaction(author, session->revision(),
                                                        {CreateAsset{make_asset(asset_id)}})));
    REQUIRE(session->snapshot()->assets().size() == 1);
    return {std::move(session), asset_id};
}

} // namespace

TEST_CASE("capability mask denies asset removal command class atomically") {
    auto seeded = seed_asset();
    auto& session = seeded.session;
    const auto revision_before = session->revision();
    const auto tempo_before = session->snapshot()->tempo_map();
    const auto journal_entries_before = session->journal().entries().size();

    // Everything permitted except removing an asset.
    const auto mask =
        deny(unrestricted_capabilities(), CommandClass::Asset, CommandIntent::Remove);
    auto agent = std::move(session->register_writer(mask)).value();

    // The denied command sits last behind a command the writer may issue, so a
    // per-command apply loop would have committed the tempo change before
    // reaching the refusal.
    auto tx = session_transaction(agent, revision_before,
                                  {SetTempoMap{tempo_before, make_tempo_map(88.0)},
                                   RemoveAsset{seeded.asset_id}});
    const auto denied_command = tx.commands.back().id;
    auto rejected = session->submit(agent, std::move(tx));

    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::CapabilityDenied);
    REQUIRE(rejected.error().command == denied_command);

    // Nothing applied: the asset survives, the permitted sibling did not land,
    // and no revision was published.
    REQUIRE(session->revision() == revision_before);
    REQUIRE(session->snapshot()->assets().size() == 1);
    REQUIRE(session->snapshot()->assets()[0].id == seeded.asset_id);
    REQUIRE(session->snapshot()->tempo_map() == tempo_before);
    REQUIRE(session->journal().entries().size() == journal_entries_before);

    // The same removal from a writer that holds the authority still works, so
    // the refusal is the mask and not an unrelated rejection.
    auto trusted = std::move(session->register_writer()).value();
    REQUIRE(session->submit(
        trusted, session_transaction(trusted, session->revision(), {RemoveAsset{seeded.asset_id}})));
    REQUIRE(session->snapshot()->assets().empty());
}

TEST_CASE("session quota exhaustion rejects transaction with no partial apply") {
    SessionLimits limits;
    limits.max_cached_results = 1;
    auto session = std::move(DocumentSession::create(make_project(), limits)).value();
    const auto tempo_before = session->snapshot()->tempo_map();

    // Budget one transaction's worth of retained size, so the first commit fits
    // and the second cannot.
    auto measure = std::move(session->register_writer()).value();
    const auto probe = session_transaction(measure, session->revision(),
                                           {SetTempoMap{tempo_before, make_tempo_map(88.0)}});
    const auto one_transaction = retained_size(probe);
    REQUIRE(one_transaction > 0);

    WriterCapabilityMask too_small;
    too_small.max_transaction_retained_bytes = one_transaction - 1;
    auto bounded = std::move(session->register_writer(too_small)).value();
    auto oversized = session_transaction(bounded, session->revision(),
                                         {SetTempoMap{tempo_before, make_tempo_map(88.0)}});
    auto transaction_rejected = session->submit(bounded, std::move(oversized));
    REQUIRE_FALSE(transaction_rejected);
    REQUIRE(transaction_rejected.error().code == ConflictCode::WriterQuotaExhausted);
    REQUIRE(session->revision() == DocumentRevision{});

    WriterCapabilityMask mask;
    mask.max_session_retained_bytes = one_transaction;
    auto agent = std::move(session->register_writer(mask)).value();

    auto first = session_transaction(agent, session->revision(),
                                     {SetTempoMap{tempo_before, make_tempo_map(88.0)}});
    auto first_result = session->submit(agent, first);
    REQUIRE(first_result);
    const auto revision_after_first = session->revision();
    const auto tempo_after_first = session->snapshot()->tempo_map();

    // An exact retry is served from the idempotency cache and is neither
    // rejected by nor charged to the already-full session quota.
    auto retry_result = session->submit(agent, first);
    REQUIRE(retry_result);
    REQUIRE(retry_result->revision == first_result->revision);
    REQUIRE(session->revision() == revision_after_first);

    auto collision = first;
    collision.commands[0].command = SetTempoMap{tempo_before, make_tempo_map(99.0)};
    auto collision_result = session->submit(agent, std::move(collision));
    REQUIRE_FALSE(collision_result);
    REQUIRE(collision_result.error().code == ConflictCode::TransactionIdCollision);

    // The writer's cumulative budget is now spent, so the second transaction is
    // refused whole rather than applying its first command.
    auto second = session_transaction(agent, revision_after_first,
                                      {SetTempoMap{tempo_after_first, make_tempo_map(132.0)},
                                       SetMeterMap{session->snapshot()->meter_map(),
                                                   make_meter_map({7, 8})}});
    auto rejected = session->submit(agent, std::move(second));

    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::WriterQuotaExhausted);
    REQUIRE(session->revision() == revision_after_first);
    REQUIRE(session->snapshot()->tempo_map() == tempo_after_first);
    REQUIRE(session->snapshot()->meter_map() == make_meter_map({4, 4}));

    // The ceiling is per writer, not per session: an unbudgeted writer still
    // commits the identical batch.
    auto trusted = std::move(session->register_writer()).value();
    REQUIRE(session->submit(trusted, session_transaction(trusted, session->revision(),
                                                         {SetTempoMap{tempo_after_first,
                                                                      make_tempo_map(132.0)}})));

    // The one-entry cache now holds the trusted commit. The original writer's
    // exact retry is expired, not a new quota-bearing transaction.
    auto expired_retry = session->submit(agent, first);
    REQUIRE_FALSE(expired_retry);
    REQUIRE(expired_retry.error().code == ConflictCode::AlreadyAppliedResultExpired);
}

TEST_CASE("quota exhaustion still permits a commandless gesture close") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    const auto tempo_before = session->snapshot()->tempo_map();

    auto measure = std::move(session->register_writer()).value();
    auto probe = session_transaction(measure, session->revision(),
                                     {SetTempoMap{tempo_before, make_tempo_map(88.0)}});
    probe.undo_group = measure.allocate_undo_group_id();
    probe.gesture_phase = GesturePhase::Begin;

    const auto one_phase = retained_size(probe);
    WriterCapabilityMask mask;
    mask.max_transaction_retained_bytes = one_phase;
    mask.max_session_retained_bytes = one_phase;
    auto agent = std::move(session->register_writer(mask)).value();
    const auto group = agent.allocate_undo_group_id();

    auto begin = session_transaction(agent, session->revision(),
                                     {SetTempoMap{tempo_before, make_tempo_map(88.0)}});
    begin.undo_group = group;
    begin.gesture_phase = GesturePhase::Begin;
    REQUIRE(retained_size(begin) == one_phase);
    REQUIRE(session->submit(agent, std::move(begin)));

    const auto revision_after_begin = session->revision();
    auto close = session_transaction(agent, revision_after_begin, {});
    close.undo_group = group;
    close.gesture_phase = GesturePhase::Cancel;
    REQUIRE(retained_size(close) > 0);
    auto closed = session->submit(agent, close);
    REQUIRE(closed);
    REQUIRE(closed->revision == revision_after_begin);
    REQUIRE(closed->applied_commands.empty());
    REQUIRE(session->revision() == revision_after_begin);
    REQUIRE(session->journal().entries().back().transaction.commands.size() == 1);

    // The lifecycle-only close is idempotent and consumes no second quota
    // charge. A real follow-up edit reaches quota admission, not GestureState.
    auto close_retry = session->submit(agent, close);
    REQUIRE(close_retry);
    REQUIRE(close_retry->revision == closed->revision);
    auto empty_at_quota =
        session->submit(agent, session_transaction(agent, session->revision(), {}));
    REQUIRE_FALSE(empty_at_quota);
    REQUIRE(empty_at_quota.error().code == ConflictCode::EmptyTransaction);
    auto over_quota = session->submit(
        agent, session_transaction(agent, session->revision(),
                                   {SetTempoMap{make_tempo_map(88.0), make_tempo_map(99.0)}}));
    REQUIRE_FALSE(over_quota);
    REQUIRE(over_quota.error().code == ConflictCode::WriterQuotaExhausted);

    REQUIRE(session->can_undo());
    REQUIRE(session->undo(agent));
    REQUIRE(session->snapshot()->tempo_map() == tempo_before);

    // A lifecycle-only close neither publishes nor journals, so revision-space
    // exhaustion cannot strand the gesture it is closing.
    auto max_sink = std::make_shared<ToggleJournalSink>();
    constexpr auto before_max = std::numeric_limits<std::uint64_t>::max() - 1;
    auto max_session = std::move(DocumentSession::restore(
                                     make_project(), DocumentRevision{before_max}, {}, max_sink))
                           .value();
    auto max_writer = std::move(max_session->register_writer()).value();
    const auto max_group = max_writer.allocate_undo_group_id();
    auto max_begin = session_transaction(
        max_writer, max_session->revision(),
        {SetTempoMap{max_session->snapshot()->tempo_map(), make_tempo_map(88.0)}});
    max_begin.undo_group = max_group;
    max_begin.gesture_phase = GesturePhase::Begin;
    REQUIRE(max_session->submit(max_writer, std::move(max_begin)));
    REQUIRE(max_session->revision().value == std::numeric_limits<std::uint64_t>::max());
    auto max_close = session_transaction(max_writer, max_session->revision(), {});
    max_close.undo_group = max_group;
    max_close.gesture_phase = GesturePhase::End;
    REQUIRE(max_session->submit(max_writer, std::move(max_close)));
    REQUIRE_FALSE(max_session->is_gesture_open(max_writer.provenance(), max_group));
    REQUIRE(max_sink->append_calls == 1);
    auto empty_at_max = max_session->submit(
        max_writer, session_transaction(max_writer, max_session->revision(), {}));
    REQUIRE_FALSE(empty_at_max);
    REQUIRE(empty_at_max.error().code == ConflictCode::EmptyTransaction);
    auto max_write = max_session->submit(
        max_writer,
        session_transaction(max_writer, max_session->revision(),
                            {SetTempoMap{make_tempo_map(88.0), make_tempo_map(99.0)}}));
    REQUIRE_FALSE(max_write);
    REQUIRE(max_write.error().code == ConflictCode::SequenceExhausted);
    REQUIRE(max_session->revision().value == std::numeric_limits<std::uint64_t>::max());
    REQUIRE(max_sink->append_calls == 1);

    // A failed document append remains a permanent durability failure for
    // later writes, but cannot block an unjournaled lifecycle-only close.
    auto failing_sink = std::make_shared<ToggleJournalSink>();
    auto durable_session =
        std::move(DocumentSession::create(make_project(), {}, failing_sink)).value();
    auto durable_writer = std::move(durable_session->register_writer()).value();
    const auto durable_group = durable_writer.allocate_undo_group_id();
    auto durable_begin = session_transaction(
        durable_writer, durable_session->revision(),
        {SetTempoMap{durable_session->snapshot()->tempo_map(), make_tempo_map(88.0)}});
    durable_begin.undo_group = durable_group;
    durable_begin.gesture_phase = GesturePhase::Begin;
    REQUIRE(durable_session->submit(durable_writer, std::move(durable_begin)));
    failing_sink->fail_append = true;
    auto failed_update = session_transaction(
        durable_writer, durable_session->revision(),
        {SetTempoMap{make_tempo_map(88.0), make_tempo_map(99.0)}});
    failed_update.undo_group = durable_group;
    failed_update.gesture_phase = GesturePhase::Update;
    auto durability_failure = durable_session->submit(durable_writer, std::move(failed_update));
    REQUIRE_FALSE(durability_failure);
    REQUIRE(durability_failure.error().code == ConflictCode::JournalDurability);
    auto durability_close = session_transaction(durable_writer, durable_session->revision(), {});
    durability_close.undo_group = durable_group;
    durability_close.gesture_phase = GesturePhase::Cancel;
    REQUIRE(durable_session->submit(durable_writer, std::move(durability_close)));
    REQUIRE_FALSE(
        durable_session->is_gesture_open(durable_writer.provenance(), durable_group));
    REQUIRE(failing_sink->append_calls == 2);
    auto empty_after_failure = durable_session->submit(
        durable_writer,
        session_transaction(durable_writer, durable_session->revision(), {}));
    REQUIRE_FALSE(empty_after_failure);
    REQUIRE(empty_after_failure.error().code == ConflictCode::EmptyTransaction);
    auto later_write = durable_session->submit(
        durable_writer,
        session_transaction(durable_writer, durable_session->revision(),
                            {SetTempoMap{make_tempo_map(88.0), make_tempo_map(110.0)}}));
    REQUIRE_FALSE(later_write);
    REQUIRE(later_write.error().code == ConflictCode::JournalDurability);
}

TEST_CASE("mask is immutable after writer registration") {
    auto session = std::move(DocumentSession::create(make_project())).value();
    const auto tempo_before = session->snapshot()->tempo_map();

    auto mask =
        deny(unrestricted_capabilities(), CommandClass::Timing, CommandIntent::Modify);
    auto agent = std::move(session->register_writer(mask)).value();
    const auto provenance = agent.provenance();

    REQUIRE(session->writer_capabilities(provenance).has_value());
    REQUIRE(session->writer_capabilities(provenance)->allowed == mask.allowed);

    // Mutating the caller's copy after registration cannot reach session state.
    mask = allow(mask, CommandClass::Timing, CommandIntent::Modify);
    REQUIRE(allows(mask, {CommandClass::Timing, CommandIntent::Modify}));
    REQUIRE_FALSE(allows(*session->writer_capabilities(provenance),
                         {CommandClass::Timing, CommandIntent::Modify}));

    // Mutating the value handed back by the session cannot reach it either.
    auto observed = *session->writer_capabilities(provenance);
    observed.allowed = kAllCommandAuthorities;
    observed.max_session_retained_bytes = 1;
    REQUIRE_FALSE(allows(*session->writer_capabilities(provenance),
                         {CommandClass::Timing, CommandIntent::Modify}));

    // Enforcement still reflects the registered mask, not either copy.
    auto rejected = session->submit(
        agent, session_transaction(agent, session->revision(),
                                   {SetTempoMap{tempo_before, make_tempo_map(88.0)}}));
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::CapabilityDenied);

    // Moving the token carries the same authority; it does not reset it.
    auto moved = std::move(agent);
    REQUIRE(moved.provenance() == provenance);
    auto after_move = session->submit(
        moved, session_transaction(moved, session->revision(),
                                   {SetTempoMap{tempo_before, make_tempo_map(88.0)}}));
    REQUIRE_FALSE(after_move);
    REQUIRE(after_move.error().code == ConflictCode::CapabilityDenied);
}

TEST_CASE("undo cannot launder a denied command class") {
    auto seeded = seed_asset();
    auto& session = seeded.session;
    const auto revision_before = session->revision();

    // Undo applies the recorded inverse, and the inverse of a creation is a
    // removal, so a writer denied asset removal must not reach that effect by
    // undoing another writer's work.
    const auto mask =
        deny(unrestricted_capabilities(), CommandClass::Asset, CommandIntent::Remove);
    auto agent = std::move(session->register_writer(mask)).value();
    REQUIRE(session->can_undo());

    auto rejected = session->undo(agent);
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().code == ConflictCode::CapabilityDenied);
    REQUIRE(session->revision() == revision_before);
    REQUIRE(session->snapshot()->assets().size() == 1);

    // The refusal left the history stacks untouched, so the undo is still there
    // for a writer that holds the authority.
    REQUIRE(session->can_undo());
    REQUIRE_FALSE(session->can_redo());
    auto trusted = std::move(session->register_writer()).value();
    REQUIRE(session->undo(trusted));
    REQUIRE(session->snapshot()->assets().empty());

    // Redo re-applies the creation, which the denied writer may perform: the
    // gate is the authority each direction actually needs, not a blanket ban.
    REQUIRE(session->can_redo());
    REQUIRE(session->redo(agent));
    REQUIRE(session->snapshot()->assets().size() == 1);
}

TEST_CASE("value-dependent and aggregate commands cannot launder child authority") {
    SECTION("replacing complete note content requires remove authority") {
        auto session = std::move(DocumentSession::create(make_project())).value();
        auto mask =
            deny(unrestricted_capabilities(), CommandClass::Note, CommandIntent::Remove);
        auto agent = std::move(session->register_writer(mask)).value();
        const auto& midi = std::get<MidiContent>(clip(*session->snapshot()).content());
        std::vector<NoteEvent> expected(midi.notes().begin(), midi.notes().end());

        auto rejected = session->submit(
            agent, session_transaction(agent, session->revision(),
                                       {ReplaceNoteContent{{3}, {4}, {5}, expected, {}, {}, {}}}));
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error().code == ConflictCode::CapabilityDenied);
        REQUIRE(std::get<MidiContent>(clip(*session->snapshot()).content()).notes().size() == 1);

        auto trusted = std::move(session->register_writer()).value();
        REQUIRE(session->submit(
            trusted, session_transaction(trusted, session->revision(),
                                         {ReplaceNoteContent{{3}, {4}, {5}, expected, {}, {}, {}}})));
        REQUIRE(std::get<MidiContent>(clip(*session->snapshot()).content()).notes().empty());
    }

    SECTION("replacing complete note content requires create authority") {
        auto session = std::move(DocumentSession::create(make_project())).value();
        auto mask =
            deny(unrestricted_capabilities(), CommandClass::Note, CommandIntent::Create);
        auto agent = std::move(session->register_writer(mask)).value();
        const auto& midi = std::get<MidiContent>(clip(*session->snapshot()).content());
        std::vector<NoteEvent> expected(midi.notes().begin(), midi.notes().end());
        auto replacement = expected;
        auto added = expected.front();
        added.id = {7};
        added.start = {kTicksPerQuarter / 2};
        replacement.push_back(added);

        auto rejected = session->submit(
            agent, session_transaction(
                       agent, session->revision(),
                       {ReplaceNoteContent{{3}, {4}, {5}, expected, replacement, {}, {}}}));
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error().code == ConflictCode::CapabilityDenied);
        REQUIRE(std::get<MidiContent>(clip(*session->snapshot()).content()).notes().size() == 1);
    }

    SECTION("clip track and sequence aggregates require contained note authority") {
        auto assert_denied = [](Command command) {
            auto session = std::move(DocumentSession::create(make_project())).value();
            auto mask =
                deny(unrestricted_capabilities(), CommandClass::Note, CommandIntent::Create);
            auto agent = std::move(session->register_writer(mask)).value();
            const auto revision_before = session->revision();
            auto rejected = session->submit(
                agent, session_transaction(agent, revision_before, {std::move(command)}));
            REQUIRE_FALSE(rejected);
            REQUIRE(rejected.error().code == ConflictCode::CapabilityDenied);
            REQUIRE(session->revision() == revision_before);
        };

        assert_denied(InsertClip{{3}, {4}, make_note_clip({7}, {8}, 2 * kTicksPerQuarter)});

        auto track = std::move(
                         Track::create({7}, "nested", {make_note_clip({8}, {9}, 0)}))
                         .value();
        assert_denied(InsertTrack{{3}, std::move(track)});

        auto sequence_track =
            std::move(Track::create({8}, "nested", {make_note_clip({9}, {10}, 0)})).value();
        auto sequence = std::move(Sequence::create({7}, "nested", TickDuration{kTicksPerQuarter},
                                                   {std::move(sequence_track)}))
                            .value();
        assert_denied(InsertSequence{std::move(sequence)});

        assert_denied(CloneSequence{
            {3}, {7}, {{{3}, {7}}, {{4}, {8}}, {{5}, {9}}, {{6}, {10}}}});
    }

    SECTION("removing a track requires contained note removal authority") {
        auto session = std::move(DocumentSession::create(make_project())).value();
        auto mask =
            deny(unrestricted_capabilities(), CommandClass::Note, CommandIntent::Remove);
        auto agent = std::move(session->register_writer(mask)).value();
        auto rejected = session->submit(
            agent, session_transaction(agent, session->revision(), {RemoveTrack{{3}, {4}}}));
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error().code == ConflictCode::CapabilityDenied);
        REQUIRE(session->snapshot()->find_sequence({3})->find_track({4}) != nullptr);
    }

    SECTION("direct aggregate removal requires contained note removal authority") {
        auto assert_denied = [](Command command) {
            auto session = std::move(DocumentSession::create(make_project())).value();
            auto mask =
                deny(unrestricted_capabilities(), CommandClass::Note, CommandIntent::Remove);
            auto agent = std::move(session->register_writer(mask)).value();
            auto rejected = session->submit(
                agent, session_transaction(agent, session->revision(), {std::move(command)}));
            REQUIRE_FALSE(rejected);
            REQUIRE(rejected.error().code == ConflictCode::CapabilityDenied);
            REQUIRE(session->revision() == DocumentRevision{});
        };

        assert_denied(RemoveClip{{3}, {4}, {5}});
        assert_denied(RemoveSequence{{3}});
    }

    SECTION("an earlier command cannot hide a child removed by an aggregate") {
        auto empty_track = std::move(Track::create({4}, "empty", {})).value();
        auto sequence = std::move(Sequence::create({3}, "sequence",
                                                   TickDuration{8 * kTicksPerQuarter},
                                                   {std::move(empty_track)}))
                            .value();
        auto project =
            std::move(Project::create(ProjectInput{.id = {1},
                                                   .name = "project",
                                                   .next_item_id = 5,
                                                   .root_sequence_id = {3},
                                                   .sequences = {std::move(sequence)}}))
                .value();
        auto session = std::move(DocumentSession::create(std::move(project))).value();
        auto mask =
            deny(unrestricted_capabilities(), CommandClass::Note, CommandIntent::Remove);
        auto agent = std::move(session->register_writer(mask)).value();
        auto transaction = session_transaction(
            agent, session->revision(),
            {InsertClip{{3}, {4}, make_note_clip({5}, {6}, 0)}, RemoveTrack{{3}, {4}}});
        const auto removed_track_command = transaction.commands.back().id;

        auto rejected = session->submit(agent, std::move(transaction));
        REQUIRE_FALSE(rejected);
        REQUIRE(rejected.error().code == ConflictCode::CapabilityDenied);
        REQUIRE(rejected.error().command == removed_track_command);
        REQUIRE(session->snapshot()->find_sequence({3})->find_track({4}) != nullptr);
        REQUIRE(session->snapshot()->find_sequence({3})->find_track({4})->clips().empty());
    }

    SECTION("aggregate undo and redo require child authority") {
        auto session = std::move(DocumentSession::create(make_project())).value();
        auto trusted = std::move(session->register_writer()).value();
        auto track =
            std::move(Track::create({7}, "nested", {make_note_clip({8}, {9}, 0)})).value();
        REQUIRE(session->submit(
            trusted, session_transaction(trusted, session->revision(),
                                         {InsertTrack{{3}, std::move(track)}})));

        auto remove_mask =
            deny(unrestricted_capabilities(), CommandClass::Note, CommandIntent::Remove);
        auto remove_denied = std::move(session->register_writer(remove_mask)).value();
        auto rejected_undo = session->undo(remove_denied);
        REQUIRE_FALSE(rejected_undo);
        REQUIRE(rejected_undo.error().code == ConflictCode::CapabilityDenied);
        REQUIRE(session->snapshot()->find_sequence({3})->find_track({7}) != nullptr);

        REQUIRE(session->undo(trusted));
        REQUIRE(session->snapshot()->find_sequence({3})->find_track({7}) == nullptr);

        auto create_mask =
            deny(unrestricted_capabilities(), CommandClass::Note, CommandIntent::Create);
        auto create_denied = std::move(session->register_writer(create_mask)).value();
        auto rejected_redo = session->redo(create_denied);
        REQUIRE_FALSE(rejected_redo);
        REQUIRE(rejected_redo.error().code == ConflictCode::CapabilityDenied);
        REQUIRE(session->snapshot()->find_sequence({3})->find_track({7}) == nullptr);
    }
}

TEST_CASE("every command class can be individually denied") {
    // Walks the Command variant itself, so an alternative added later is
    // covered here the moment it compiles rather than when someone remembers to
    // extend a hand-written list.
    constexpr std::size_t alternatives = std::variant_size_v<Command>;
    STATIC_REQUIRE(alternatives == 47);
    using Class = CommandClass;
    using Intent = CommandIntent;
    constexpr std::array<CommandAuthority, alternatives> expected{{
        {Class::Clip, Intent::Create},       {Class::Clip, Intent::Remove},
        {Class::Automation, Intent::Create}, {Class::Automation, Intent::Remove},
        {Class::Clip, Intent::Modify},       {Class::Note, Intent::Modify},
        {Class::Note, Intent::Modify},       {Class::Clip, Intent::Modify},
        {Class::Timing, Intent::Modify},     {Class::Timing, Intent::Modify},
        {Class::Asset, Intent::Create},      {Class::Asset, Intent::Remove},
        {Class::Take, Intent::Create},       {Class::Take, Intent::Remove},
        {Class::Track, Intent::Modify},      {Class::Take, Intent::Create},
        {Class::Take, Intent::Remove},       {Class::Take, Intent::Modify},
        {Class::Take, Intent::Modify},       {Class::Track, Intent::Modify},
        {Class::Annotation, Intent::Create}, {Class::Annotation, Intent::Remove},
        {Class::Annotation, Intent::Create}, {Class::Annotation, Intent::Remove},
        {Class::Annotation, Intent::Modify}, {Class::Annotation, Intent::Modify},
        {Class::Scene, Intent::Create},      {Class::Scene, Intent::Remove},
        {Class::Scene, Intent::Create},      {Class::Scene, Intent::Remove},
        {Class::Sequence, Intent::Create},   {Class::Sequence, Intent::Create},
        {Class::Sequence, Intent::Remove},   {Class::Clip, Intent::Modify},
        {Class::Track, Intent::Modify},      {Class::Track, Intent::Create},
        {Class::Track, Intent::Remove},      {Class::Track, Intent::Modify},
        {Class::Track, Intent::Modify},      {Class::Note, Intent::Modify},
        {Class::Note, Intent::Create},       {Class::Note, Intent::Remove},
        {Class::Device, Intent::Create},     {Class::Device, Intent::Remove},
        {Class::Device, Intent::Modify},     {Class::Device, Intent::Modify},
        {Class::Device, Intent::Modify},
    }};

    std::size_t checked = 0;
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (
            [&] {
                constexpr auto authority =
                    command_authority_of<std::variant_alternative_t<I, Command>>();
                STATIC_REQUIRE(authority == expected[I]);

                // A mask denying exactly this authority refuses it and nothing else.
                const auto narrowed =
                    deny(unrestricted_capabilities(), authority.command_class, authority.intent);
                REQUIRE_FALSE(allows(narrowed, authority));

                std::size_t still_allowed = 0;
                for (std::size_t c = 0; c < kCommandClassCount; ++c) {
                    for (std::size_t i = 0; i < kCommandIntentCount; ++i) {
                        const CommandAuthority other{static_cast<CommandClass>(c),
                                                     static_cast<CommandIntent>(i)};
                        if (other == authority)
                            continue;
                        if (allows(narrowed, other))
                            ++still_allowed;
                    }
                }
                REQUIRE(still_allowed == kCommandClassCount * kCommandIntentCount - 1);

                // The default mask permits it, so denial is the deliberate act.
                REQUIRE(allows(unrestricted_capabilities(), authority));
                ++checked;
            }(),
            ...);
    }(std::make_index_sequence<alternatives>{});

    REQUIRE(checked == alternatives);
}

TEST_CASE("a destructive-denying mask refuses every remove and permits the rest") {
    constexpr auto mask = non_destructive_capabilities();
    STATIC_REQUIRE(mask.allowed == WriterCapabilityMask{}.allowed);
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (
            [&] {
                constexpr auto authority =
                    command_authority_of<std::variant_alternative_t<I, Command>>();
                REQUIRE(allows(mask, authority) == (authority.intent != CommandIntent::Remove));
            }(),
            ...);
    }(std::make_index_sequence<std::variant_size_v<Command>>{});
}
