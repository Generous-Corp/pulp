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
