#include "detail/dawn_submission_tracker.hpp"

#include <catch2/catch_test_macros.hpp>

using pulp::gpu_audio::detail::DawnSubmissionTracker;

namespace {
using Queue = DawnSubmissionTracker::QueueResult;
using Scope = DawnSubmissionTracker::ScopeResult;
using Terminal = DawnSubmissionTracker::Terminal;

DawnSubmissionTracker::Observation clean(bool drained = false) {
    return {.uncaptured_error_generation = 7, .physically_drained = drained};
}
} // namespace

TEST_CASE("Dawn submission requires queue and scoped success") {
    DawnSubmissionTracker tracker;
    REQUIRE(tracker.begin(1, 7));
    REQUIRE(tracker.record_queue(1, Queue::Success));
    CHECK_FALSE(tracker.observe(1, clean()));
    REQUIRE(tracker.record_scope(1, Scope::Clean));
    CHECK(tracker.observe(1, clean()) == Terminal::RetiredSuccess);
    CHECK_FALSE(tracker.active());
}

TEST_CASE("Dawn submission failures remain quarantined until physical drain") {
    for (const auto queue : {Queue::Error, Queue::Cancelled}) {
        DawnSubmissionTracker tracker;
        REQUIRE(tracker.begin(1, 7));
        REQUIRE(tracker.record_queue(1, queue));
        REQUIRE(tracker.record_scope(1, Scope::Clean));
        CHECK_FALSE(tracker.observe(1, clean()));
        CHECK(tracker.observe(1, clean(true)) == Terminal::RetiredFailure);
    }

    DawnSubmissionTracker tracker;
    REQUIRE(tracker.begin(2, 7));
    REQUIRE(tracker.record_queue(2, Queue::Success));
    REQUIRE(tracker.record_scope(2, Scope::Error));
    CHECK_FALSE(tracker.observe(2, clean()));
    CHECK(tracker.observe(2, clean(true)) == Terminal::RetiredFailure);
}

TEST_CASE("loss and uncaptured errors poison synthetic success") {
    for (const auto observation : {
             DawnSubmissionTracker::Observation{7, true, false},
             DawnSubmissionTracker::Observation{8, false, false},
         }) {
        DawnSubmissionTracker tracker;
        REQUIRE(tracker.begin(3, 7));
        REQUIRE(tracker.record_queue(3, Queue::Success));
        REQUIRE(tracker.record_scope(3, Scope::Clean));
        CHECK_FALSE(tracker.observe(3, observation));
        auto drained = observation;
        drained.physically_drained = true;
        CHECK(tracker.observe(3, drained) == Terminal::RetiredFailure);
    }
}

TEST_CASE("expiry does not reinterpret late clean retirement") {
    DawnSubmissionTracker tracker;
    REQUIRE(tracker.begin(4, 7));
    REQUIRE(tracker.mark_expired(4));
    REQUIRE(tracker.record_queue(4, Queue::Success));
    REQUIRE(tracker.record_scope(4, Scope::Clean));
    CHECK(tracker.observe(4, clean()) == Terminal::RetiredSuccess);
    CHECK(tracker.expired());
}

TEST_CASE("stale and duplicate evidence cannot alter a generation") {
    DawnSubmissionTracker tracker;
    REQUIRE(tracker.begin(5, 7));
    CHECK_FALSE(tracker.record_queue(4, Queue::Success));
    REQUIRE(tracker.record_queue(5, Queue::Success));
    CHECK_FALSE(tracker.record_queue(5, Queue::Error));
    REQUIRE(tracker.record_scope(5, Scope::Clean));
    CHECK(tracker.observe(5, clean()) == Terminal::RetiredSuccess);
    CHECK_FALSE(tracker.record_scope(5, Scope::Error));
    CHECK(tracker.rejected_evidence() == 3);
}

TEST_CASE("physical drain retires missing or ambiguous callbacks as failure") {
    DawnSubmissionTracker tracker;
    REQUIRE(tracker.begin(6, 7));
    CHECK(tracker.observe(6, clean(true)) == Terminal::RetiredFailure);
    REQUIRE(tracker.begin(7, 7));
    REQUIRE(tracker.record_queue(7, Queue::Success));
    CHECK(tracker.observe(7, clean(true)) == Terminal::RetiredFailure);
}
