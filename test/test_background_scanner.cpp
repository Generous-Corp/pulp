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
