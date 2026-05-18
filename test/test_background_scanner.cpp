// Unit tests for the background plugin scanner (workstream 03 slice 3.2).

#include <catch2/catch_test_macros.hpp>
#include <pulp/host/background_scanner.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace pulp::host;

namespace {

PluginInfo fake_info(const std::string& id, PluginFormat fmt) {
    PluginInfo p;
    p.name = id;
    p.path = "/fake/" + id;
    p.unique_id = "com.fake." + id;
    p.format = fmt;
    return p;
}

template <typename Predicate>
bool wait_until(const Predicate& predicate,
                std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

} // namespace

TEST_CASE("BackgroundScanner: completes a quick scan", "[host][bg-scan]") {
    BackgroundScanner bs;
    std::vector<PluginInfo> final_results;
    bool final_cancelled = true;
    std::atomic<bool> done{false};

    bs.start(
        [](const CancelToken&, const ScanProgressCallback&) {
            return std::vector<PluginInfo>{
                fake_info("A", PluginFormat::VST3),
                fake_info("B", PluginFormat::CLAP),
            };
        },
        nullptr,
        [&](std::vector<PluginInfo> r, bool cancelled) {
            final_results = std::move(r);
            final_cancelled = cancelled;
            done.store(true, std::memory_order_release);
        });
    bs.join();
    REQUIRE(done.load());
    REQUIRE(final_cancelled == false);
    REQUIRE(final_results.size() == 2);
    REQUIRE(final_results[0].name == "A");
}

TEST_CASE("BackgroundScanner: empty worker still completes",
          "[host][bg-scan][coverage]") {
    BackgroundScanner bs;
    bool completion_called = false;
    bool final_cancelled = true;
    std::vector<PluginInfo> final_results{fake_info("stale", PluginFormat::VST3)};

    REQUIRE(bs.start(
        nullptr,
        [](const std::string&, int, int) {
            FAIL("progress should not fire without a worker");
        },
        [&](std::vector<PluginInfo> results, bool cancelled) {
            final_results = std::move(results);
            final_cancelled = cancelled;
            completion_called = true;
        }));

    bs.join();
    REQUIRE(completion_called);
    REQUIRE_FALSE(final_cancelled);
    REQUIRE(final_results.empty());
}

TEST_CASE("BackgroundScanner: idle cancel and join are no-ops",
          "[host][bg-scan][coverage]") {
    BackgroundScanner bs;
    REQUIRE_FALSE(bs.is_running());
    bs.cancel();
    bs.join();
    REQUIRE_FALSE(bs.is_running());
}

TEST_CASE("CancelToken request and reset toggle the shared flag",
          "[host][bg-scan][coverage]") {
    CancelToken token;
    REQUIRE_FALSE(token.requested());

    token.request();
    REQUIRE(token.requested());

    token.reset();
    REQUIRE_FALSE(token.requested());
}

TEST_CASE("BackgroundScanner: second start while running returns false",
          "[host][bg-scan]") {
    BackgroundScanner bs;
    std::atomic<bool> hold{true};
    bs.start(
        [&](const CancelToken& tok, const ScanProgressCallback&) {
            while (hold.load() && !tok.requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return std::vector<PluginInfo>{};
        },
        nullptr, nullptr);

    REQUIRE_FALSE(bs.start(
        [](const CancelToken&, const ScanProgressCallback&) {
            return std::vector<PluginInfo>{};
        },
        nullptr, nullptr));

    hold.store(false);
    bs.join();
}

TEST_CASE("BackgroundScanner: cancel marks completion as cancelled",
          "[host][bg-scan]") {
    BackgroundScanner bs;
    std::atomic<bool> started{false};
    bool final_cancelled = false;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;

    bs.start(
        [&](const CancelToken& tok, const ScanProgressCallback&) {
            started.store(true);
            while (!tok.requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            return std::vector<PluginInfo>{};
        },
        nullptr,
        [&](std::vector<PluginInfo>, bool cancelled) {
            std::lock_guard<std::mutex> lk(m);
            final_cancelled = cancelled;
            done = true;
            cv.notify_all();
        });

    while (!started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bs.cancel();
    std::unique_lock<std::mutex> lk(m);
    cv.wait_for(lk, std::chrono::seconds(2), [&] { return done; });
    REQUIRE(done);
    REQUIRE(final_cancelled == true);
    bs.join();
}

TEST_CASE("BackgroundScanner: restart after cancelled completion joins and resets token",
          "[host][bg-scan][issue-493]") {
    BackgroundScanner bs;
    std::atomic<bool> first_started{false};
    std::atomic<bool> first_worker_saw_cancel{false};
    std::atomic<bool> second_worker_saw_stale_cancel{true};
    std::mutex m;
    std::condition_variable cv;
    bool first_done = false;
    bool first_cancelled = false;
    bool second_done = false;
    bool second_cancelled = true;
    std::vector<PluginInfo> second_results;

    REQUIRE(bs.start(
        [&](const CancelToken& tok, const ScanProgressCallback&) {
            first_started.store(true, std::memory_order_release);
            while (!tok.requested()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            first_worker_saw_cancel.store(tok.requested(),
                                          std::memory_order_release);
            return std::vector<PluginInfo>{};
        },
        nullptr,
        [&](std::vector<PluginInfo>, bool cancelled) {
            std::lock_guard<std::mutex> lk(m);
            first_cancelled = cancelled;
            first_done = true;
            cv.notify_all();
        }));

    REQUIRE(wait_until([&] {
        return first_started.load(std::memory_order_acquire);
    }));
    bs.cancel();

    {
        std::unique_lock<std::mutex> lk(m);
        REQUIRE(cv.wait_for(lk, std::chrono::seconds(2), [&] {
            return first_done;
        }));
        REQUIRE(first_cancelled);
    }
    REQUIRE(first_worker_saw_cancel.load(std::memory_order_acquire));

    REQUIRE(wait_until([&] { return !bs.is_running(); }));

    // Do not call join(): this leaves the completed std::thread joinable
    // and forces start() to clean it up before assigning the replacement.
    REQUIRE(bs.start(
        [&](const CancelToken& tok, const ScanProgressCallback&) {
            second_worker_saw_stale_cancel.store(tok.requested(),
                                                std::memory_order_release);
            return std::vector<PluginInfo>{
                fake_info("Restarted", PluginFormat::CLAP),
            };
        },
        nullptr,
        [&](std::vector<PluginInfo> results, bool cancelled) {
            std::lock_guard<std::mutex> lk(m);
            second_results = std::move(results);
            second_cancelled = cancelled;
            second_done = true;
            cv.notify_all();
        }));

    {
        std::unique_lock<std::mutex> lk(m);
        REQUIRE(cv.wait_for(lk, std::chrono::seconds(2), [&] {
            return second_done;
        }));
        REQUIRE(second_cancelled == false);
        REQUIRE(second_results.size() == 1);
        REQUIRE(second_results[0].name == "Restarted");
    }

    REQUIRE(second_worker_saw_stale_cancel.load(std::memory_order_acquire) ==
            false);
    bs.join();
}

TEST_CASE("BackgroundScanner: progress callback fires from worker thread",
          "[host][bg-scan]") {
    BackgroundScanner bs;
    std::atomic<int> progress_calls{0};
    std::atomic<bool> done{false};
    bs.start(
        [](const CancelToken&, const ScanProgressCallback& prog) {
            if (prog) {
                prog("/fake/A", 1, 2);
                prog("/fake/B", 2, 2);
            }
            return std::vector<PluginInfo>{};
        },
        [&](const std::string&, int, int) { ++progress_calls; },
        [&](std::vector<PluginInfo>, bool) {
            done.store(true, std::memory_order_release);
        });
    bs.join();
    REQUIRE(done.load());
    REQUIRE(progress_calls.load() == 2);
}

TEST_CASE("CancelToken reset clears a prior cancellation request",
          "[host][bg-scan][coverage]") {
    CancelToken token;
    REQUIRE_FALSE(token.requested());
    token.request();
    REQUIRE(token.requested());
    token.reset();
    REQUIRE_FALSE(token.requested());
}

TEST_CASE("BackgroundScanner: destructor cancels + joins any running worker",
          "[host][bg-scan]") {
    std::atomic<bool> finished{false};
    {
        BackgroundScanner bs;
        bs.start(
            [&](const CancelToken& tok, const ScanProgressCallback&) {
                while (!tok.requested()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                finished.store(true, std::memory_order_release);
                return std::vector<PluginInfo>{};
            },
            nullptr, nullptr);
        // Destructor fires here — must cancel + join cleanly.
    }
    REQUIRE(finished.load());
}
