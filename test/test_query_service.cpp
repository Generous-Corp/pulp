// Tests for pulp::view::QueryService — the off-UI-thread index/search worker
// and its marshal-back contract (R7). Drives the service directly with a fake
// "deliver" sink standing in for the bridge's UI-thread result queue, so no JS
// engine is needed. Verifies: work runs off the calling thread, results are
// marshaled back through deliver, search-as-you-type supersession, release, and
// safe teardown with in-flight work.

#include <catch2/catch_test_macros.hpp>

#include <pulp/view/query_service.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using pulp::view::QueryService;

namespace {

// Thread-safe stand-in for the bridge's async_exec_results_ queue. The service
// calls deliver() from its worker thread; the test thread drains it, mirroring
// how poll_async_results() drains on the UI thread.
struct DeliverSink {
    std::mutex mutex;
    std::vector<std::pair<std::string, std::string>> results;
    std::atomic<std::thread::id> last_delivery_thread{};

    QueryService::DeliverFn fn() {
        return [this](const std::string& callback_id, std::string payload) {
            last_delivery_thread.store(std::this_thread::get_id());
            std::lock_guard<std::mutex> lock(mutex);
            results.emplace_back(callback_id, std::move(payload));
        };
    }

    std::size_t count() {
        std::lock_guard<std::mutex> lock(mutex);
        return results.size();
    }

    // Most recent payload for callback_id, or nullopt.
    std::optional<std::string> latest(const std::string& callback_id) {
        std::lock_guard<std::mutex> lock(mutex);
        std::optional<std::string> found;
        for (const auto& [id, payload] : results)
            if (id == callback_id) found = payload;
        return found;
    }
};

// Poll a predicate until true or a timeout elapses.
bool wait_until(const std::function<bool()>& done, int attempts = 400) {
    for (int i = 0; i < attempts; ++i) {
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
}

}  // namespace

TEST_CASE("QueryService delivers a build result via the deliver hook",
          "[view][query-service]") {
    DeliverSink sink;
    QueryService service(sink.fn());

    service.build("lib", {"kick", "snare", "hat"}, "onready");
    REQUIRE(wait_until([&] { return sink.latest("onready").has_value(); }));
    REQUIRE(sink.latest("onready") == "3");  // item count
    REQUIRE(service.dataset_count() == 1);
}

TEST_CASE("QueryService search returns ranked matched indices as JSON",
          "[view][query-service]") {
    DeliverSink sink;
    QueryService service(sink.fn());

    service.build("lib", {"kick", "snare", "hat", "kick_deep", "clap"});
    service.query("lib", "kick", "q1");

    REQUIRE(wait_until([&] { return sink.latest("q1").has_value(); }));
    // "kick" (exact, index 0) then "kick_deep" (prefix, index 3).
    REQUIRE(sink.latest("q1") == "[0,3]");
}

TEST_CASE("QueryService runs work off the calling thread", "[view][query-service]") {
    DeliverSink sink;
    QueryService service(sink.fn());

    service.build("lib", {"a", "b"}, "ready");
    REQUIRE(wait_until([&] { return sink.latest("ready").has_value(); }));
    REQUIRE(sink.last_delivery_thread.load() != std::this_thread::get_id());
}

TEST_CASE("QueryService search on an unknown dataset delivers an empty array",
          "[view][query-service]") {
    DeliverSink sink;
    QueryService service(sink.fn());

    service.query("missing", "kick", "q1");
    REQUIRE(wait_until([&] { return sink.latest("q1").has_value(); }));
    REQUIRE(sink.latest("q1") == "[]");
}

TEST_CASE("QueryService supersedes an in-flight query for the same dataset",
          "[view][query-service]") {
    DeliverSink sink;
    QueryService service(sink.fn());

    // A large library so a single query takes long enough to be superseded
    // while queued/running.
    std::vector<std::string> items;
    items.reserve(60000);
    for (int i = 0; i < 60000; ++i) items.push_back("sample_" + std::to_string(i));
    service.build("lib", std::move(items), "ready");
    REQUIRE(wait_until([&] { return sink.latest("ready").has_value(); }));

    // Fire a burst of queries with distinct callback ids; earlier ones should be
    // cancelled and never deliver. Only the final query is guaranteed to run to
    // completion, so at least one earlier callback must be missing.
    for (int i = 0; i < 20; ++i)
        service.query("lib", "sample_" + std::to_string(i), "cb" + std::to_string(i));

    REQUIRE(wait_until([&] { return sink.latest("cb19").has_value(); }));

    int delivered = 0;
    for (int i = 0; i < 20; ++i)
        if (sink.latest("cb" + std::to_string(i)).has_value()) ++delivered;
    // The last one delivers; supersession must have dropped at least one earlier.
    REQUIRE(sink.latest("cb19").has_value());
    REQUIRE(delivered < 20);
}

TEST_CASE("QueryService release drops the dataset", "[view][query-service]") {
    DeliverSink sink;
    QueryService service(sink.fn());

    service.build("lib", {"kick", "snare"}, "ready");
    REQUIRE(wait_until([&] { return sink.latest("ready").has_value(); }));
    REQUIRE(service.dataset_count() == 1);

    service.release("lib");
    REQUIRE(service.dataset_count() == 0);
}

TEST_CASE("QueryService release before build completes does not resurrect the dataset",
          "[view][query-service]") {
    DeliverSink sink;
    QueryService service(sink.fn());

    // Large enough that the build is still running when release() lands.
    std::vector<std::string> items;
    items.reserve(200000);
    for (int i = 0; i < 200000; ++i) items.push_back("sample_" + std::to_string(i));

    service.build("lib", std::move(items), "ready");
    service.release("lib");  // released while the build is almost certainly mid-flight

    // Drain: give the worker ample time to run the (cancelled) build past the
    // point where it would have installed the index.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    // The released dataset must stay gone — the build must not re-install it,
    // and no readiness callback must fire for a released dataset.
    REQUIRE(service.dataset_count() == 0);
    REQUIRE_FALSE(sink.latest("ready").has_value());
}

TEST_CASE("QueryService tears down cleanly with in-flight work",
          "[view][query-service]") {
    DeliverSink sink;
    {
        QueryService service(sink.fn());
        std::vector<std::string> items;
        items.reserve(80000);
        for (int i = 0; i < 80000; ++i) items.push_back("sample_" + std::to_string(i));
        service.build("lib", std::move(items), "ready");
        // Submit a query and immediately let the service destruct — must cancel,
        // join, and not crash or deliver into a destroyed sink.
        service.query("lib", "sample_1", "q1");
    }
    // If we got here without a crash/hang, teardown is safe. The sink is a
    // local that outlives the service, so any late delivery is still valid.
    SUCCEED("QueryService destroyed with in-flight work without crashing");
}
