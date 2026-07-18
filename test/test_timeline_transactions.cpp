#include "timeline_command_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <thread>

using namespace timeline_test;

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
