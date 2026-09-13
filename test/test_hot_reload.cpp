#include <catch2/catch_test_macros.hpp>
#include "support/thread_progress.hpp"
#include <chrono>
#include <pulp/view/script_engine.hpp>
#include <choc/platform/choc_FileWatcher.h>
#include <fstream>
#include <thread>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <atomic>
#include <unordered_map>
#include <vector>

#define private public
#include <pulp/view/hot_reload.hpp>
#undef private

using namespace pulp::view;

static void write_js_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream f(path, std::ios::trunc);
    f << content;
    f.close();
}

static std::filesystem::path make_temp_dir(const std::string& prefix) {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    auto tmp_dir = std::filesystem::temp_directory_path() /
                   (prefix + "_" + std::to_string(suffix));
    std::filesystem::remove_all(tmp_dir);
    std::filesystem::create_directories(tmp_dir);
    return tmp_dir;
}

static bool wait_for_reload_containing(HotReloader& reloader,
                                       const std::string& expected,
                                       const std::string& latest_code) {
    if (latest_code.find(expected) != std::string::npos)
        return true;

    for (int i = 0; i < 30; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        reloader.poll_reload();
        if (latest_code.find(expected) != std::string::npos)
            return true;
    }
    return false;
}

static bool wait_for_history_containing(HotReloader& reloader,
                                        const std::vector<std::string>& history,
                                        const std::string& expected) {
    auto has_expected = [&]() {
        for (const auto& code : history) {
            if (code.find(expected) != std::string::npos)
                return true;
        }
        return false;
    };

    for (int i = 0; i < 30; ++i) {
        if (has_expected())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        reloader.poll_reload();
    }
    return has_expected();
}

TEST_CASE("HotReloader detects file changes", "[view][hotreload]") {
    auto tmp_dir = make_temp_dir("pulp_hotreload_test");
    auto js_file = tmp_dir / "ui.js";

    // Write initial file
    write_js_file(js_file, "// initial version");

    std::string reloaded_code;
    HotReloader reloader(js_file, [&](const std::string& code) {
        reloaded_code = code;
    });

    // Initially no reload pending
    REQUIRE_FALSE(reloader.has_pending_reload());
    REQUIRE_FALSE(reloader.poll_reload());
    REQUIRE(reloader.reload_count() == 0);

    // Modify the file
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_js_file(js_file, "// modified version\nconsole.log('hello');");

    REQUIRE(wait_for_reload_containing(reloader, "modified version", reloaded_code));
    REQUIRE(reloaded_code.find("modified version") != std::string::npos);
    REQUIRE(reloader.reload_count() == 1);

    // Clean up
    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader reload_count increments", "[view][hotreload]") {
    auto tmp_dir = make_temp_dir("pulp_hotreload_test2");
    auto js_file = tmp_dir / "ui.js";

    write_js_file(js_file, "// v1");

    std::string latest_code;
    HotReloader reloader(js_file, [&](const std::string& code) {
        latest_code = code;
    });

    // Initially zero
    REQUIRE(reloader.reload_count() == 0);

    // Modify the JS file
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_js_file(js_file, "// v2");

    REQUIRE(wait_for_reload_containing(reloader, "v2", latest_code));
    REQUIRE(reloader.reload_count() >= 1);

    // Clean up
    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader multiple sequential reloads", "[view][hotreload]") {
    auto tmp_dir = make_temp_dir("pulp_hotreload_multi");
    auto js_file = tmp_dir / "ui.js";

    write_js_file(js_file, "// v1");

    std::vector<std::string> reload_history;
    HotReloader reloader(js_file, [&](const std::string& code) {
        reload_history.push_back(code);
    });

    // First reload
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_js_file(js_file, "// v2 - first change");
    REQUIRE(wait_for_history_containing(reloader, reload_history, "v2"));

    // Second reload
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_js_file(js_file, "// v3 - second change");
    REQUIRE(wait_for_history_containing(reloader, reload_history, "v3"));

    // Verify reload count matches
    REQUIRE(reloader.reload_count() >= 2);

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader only reloads on JS modification", "[view][hotreload]") {
    auto tmp_dir = make_temp_dir("pulp_hotreload_jsonly");
    auto js_file = tmp_dir / "ui.js";

    write_js_file(js_file, "// original");

    // Let the watcher settle after initial file creation
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    std::string latest_code;
    HotReloader reloader(js_file, [&](const std::string& code) {
        latest_code = code;
    });

    // Drain any initial detection
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    reloader.poll_reload();

    // Modify the JS file — SHOULD trigger reload with new content
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_js_file(js_file, "// updated version");

    REQUIRE(wait_for_reload_containing(reloader, "updated version", latest_code));
    REQUIRE(latest_code.find("updated version") != std::string::npos);

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader directory watching", "[view][hotreload]") {
    auto tmp_dir = make_temp_dir("pulp_hotreload_test3");
    auto entry = tmp_dir / "main.js";

    write_js_file(entry, "// main entry v1");

    std::string latest_code;
    HotReloader reloader(tmp_dir, "main.js", [&](const std::string& code) {
        latest_code = code;
    });

    REQUIRE(reloader.watched_path() == tmp_dir);

    // Modify the entry file
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_js_file(entry, "// main entry v2");

    REQUIRE(wait_for_reload_containing(reloader, "v2", latest_code));
    REQUIRE(latest_code.find("v2") != std::string::npos);

    // Clean up
    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader seeds observed JS file content",
          "[view][hotreload]") {
    auto tmp_dir = make_temp_dir("pulp_hotreload_seed");
    auto entry = tmp_dir / "main.js";
    auto module = tmp_dir / "module.mjs";
    auto ignored = tmp_dir / "notes.txt";

    write_js_file(entry, "// entry v1");
    write_js_file(module, "// module v1");
    write_js_file(ignored, "not javascript");

    HotReloader reloader(tmp_dir, "main.js", [](const std::string&) {});

    REQUIRE(reloader.observed_content_hashes_.count(entry.lexically_normal().string()) == 1);
    REQUIRE(reloader.observed_content_hashes_.count(module.lexically_normal().string()) == 1);
    REQUIRE(reloader.observed_content_hashes_.count(ignored.lexically_normal().string()) == 0);
    std::string reloaded_code;
    HotReloader changed_reloader(tmp_dir, "main.js", [&](const std::string& code) {
        reloaded_code = code;
    });
    REQUIRE_FALSE(changed_reloader.poll_reload());

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_js_file(entry, "// entry v2");

    REQUIRE(wait_for_reload_containing(changed_reloader, "entry v2", reloaded_code));
    REQUIRE(reloaded_code.find("entry v2") != std::string::npos);

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader file seed skips non-JS files and missing paths",
          "[view][hotreload]") {
    auto tmp_dir = make_temp_dir("pulp_hotreload_non_js");
    auto text_file = tmp_dir / "notes.txt";
    write_js_file(text_file, "not javascript");

    HotReloader reloader(text_file, [](const std::string&) {});

    REQUIRE(reloader.observed_content_hashes_.empty());
    REQUIRE_FALSE(reloader.poll_reload());

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader ignores same-content rewrites",
          "[view][hotreload]") {
    auto tmp_dir = make_temp_dir("pulp_hotreload_same_content");
    auto entry = tmp_dir / "main.js";

    write_js_file(entry, "// entry v1");

    HotReloader reloader(entry, [](const std::string&) {});
    REQUIRE(reloader.observed_content_hashes_.count(entry.lexically_normal().string()) == 1);

    // The content gate is consume-once, and the live watcher thread is a
    // second consumer of it: if the watcher observes the rewrite first, the
    // direct call below is correctly told there is nothing new. Release the
    // watcher so this thread is the only consumer and the gate's answer is
    // a property of the content rather than of who got there first.
    reloader.watcher_.reset();

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_js_file(entry, "// entry v1");
    REQUIRE_FALSE(reloader.should_reload_for_modified_file(entry));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_js_file(entry, "// entry v2");
    REQUIRE(reloader.should_reload_for_modified_file(entry));

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader content hash still reloads changed modules",
          "[view][hotreload]") {
    auto tmp_dir = make_temp_dir("pulp_hotreload_module_hash");
    auto entry = tmp_dir / "main.js";
    auto module = tmp_dir / "module.mjs";

    write_js_file(entry, "// entry v1");
    write_js_file(module, "// module v1");

    HotReloader reloader(tmp_dir, "main.js", [](const std::string&) {});
    REQUIRE(reloader.observed_content_hashes_.count(module.lexically_normal().string()) == 1);

    // The content gate is consume-once, and the live watcher thread is a
    // second consumer of it: if the watcher observes the rewrite first, the
    // direct call below is correctly told there is nothing new. Release the
    // watcher so this thread is the only consumer and the gate's answer is
    // a property of the content rather than of who got there first.
    reloader.watcher_.reset();

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_js_file(module, "// module v1");
    REQUIRE_FALSE(reloader.should_reload_for_modified_file(module));

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_js_file(module, "// module v2");
    REQUIRE(reloader.should_reload_for_modified_file(module));

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader file watcher ignores unsupported changes",
          "[view][hotreload]") {
    auto tmp_dir = make_temp_dir("pulp_hotreload_direct_filter");
    auto entry = tmp_dir / "main.js";
    auto text_file = tmp_dir / "notes.txt";

    write_js_file(entry, "// entry v1");
    write_js_file(text_file, "not javascript");

    HotReloader reloader(tmp_dir, "main.js", [](const std::string&) {});
    for (int i = 0; i < 8; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        (void)reloader.poll_reload();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_js_file(text_file, "still not javascript");

    for (int i = 0; i < 8; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        REQUIRE_FALSE(reloader.poll_reload());
    }

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader module file change reloads directory entry",
          "[view][hotreload]") {
    auto tmp_dir = make_temp_dir("pulp_hotreload_direct_module");
    auto entry = tmp_dir / "main.js";
    auto module = tmp_dir / "module.mjs";

    write_js_file(entry, "// entry v1");
    write_js_file(module, "// module v1");

    std::string latest_code;
    HotReloader reloader(tmp_dir, "main.js", [&](const std::string& code) {
        latest_code = code;
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_js_file(module, "// module v2");
    write_js_file(entry, "// entry v2 from module");

    REQUIRE(wait_for_reload_containing(reloader, "entry v2 from module", latest_code));
    REQUIRE(latest_code.find("entry v2 from module") != std::string::npos);
    REQUIRE(reloader.reload_count() == 1);

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader pending reload without callback still drains",
          "[view][hotreload]") {
    auto tmp_dir = make_temp_dir("pulp_hotreload_no_callback");
    auto entry = tmp_dir / "main.js";
    write_js_file(entry, "// entry");

    HotReloader reloader(entry, {});
    reloader.pending_code_ = "// pending";
    reloader.has_pending_ = true;
    REQUIRE(reloader.has_pending_reload());

    REQUIRE(reloader.poll_reload());
    REQUIRE_FALSE(reloader.has_pending_reload());
    REQUIRE(reloader.reload_count() == 0);
    REQUIRE_FALSE(reloader.poll_reload());

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader empty or missing entry files do not schedule reloads",
          "[view][hotreload]") {
    auto tmp_dir = make_temp_dir("pulp_hotreload_empty_entry");
    auto entry = tmp_dir / "main.js";
    auto module = tmp_dir / "module.mjs";

    write_js_file(entry, "");
    write_js_file(module, "// module v1");

    HotReloader reloader(tmp_dir, "main.js", [](const std::string&) {});

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_js_file(module, "// module v2");
    for (int i = 0; i < 8; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        REQUIRE_FALSE(reloader.poll_reload());
    }

    std::filesystem::remove(entry);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_js_file(module, "// module v3");
    for (int i = 0; i < 8; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        REQUIRE_FALSE(reloader.poll_reload());
    }

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader content gate hands a rewrite to exactly one caller",
          "[view][hotreload][rt-safety]") {
    auto tmp_dir = make_temp_dir("pulp_hotreload_gate_race");
    auto entry = tmp_dir / "main.js";

    write_js_file(entry, "// entry v0");

    HotReloader reloader(entry, [](const std::string&) {});
    reloader.watcher_.reset();

    constexpr int kThreads = 8;
    constexpr int kRounds = 512;

    for (int round = 0; round < kRounds; ++round) {
        write_js_file(entry, "// entry v" + std::to_string(round + 1));

        std::atomic<bool> go{false};
        std::atomic<int> ready{0};
        std::atomic<int> claims{0};
        std::atomic<bool> timed_out{false};
        std::vector<std::thread> threads;
        threads.reserve(kThreads);

        for (int i = 0; i < kThreads; ++i) {
            threads.emplace_back([&] {
                ready.fetch_add(1, std::memory_order_relaxed);

                // A start gate rather than a sleep: the threads must call the
                // gate close enough together to contend for the same rewrite.
                // Spinning is the point, so it carries its own deadline — a
                // release that never arrives has to fail an assertion below
                // rather than hang the suite.
                const auto deadline =
                    std::chrono::steady_clock::now() + pulp::test::kProgressDeadline;
                while (!go.load(std::memory_order_acquire) &&
                       std::chrono::steady_clock::now() < deadline) {
                }

                if (!go.load(std::memory_order_acquire)) {
                    timed_out.store(true, std::memory_order_relaxed);
                    return;
                }

                if (reloader.should_reload_for_modified_file(entry))
                    claims.fetch_add(1, std::memory_order_relaxed);
            });
        }

        const bool all_ready = pulp::test::wait_for_condition(
            [&] { return ready.load(std::memory_order_relaxed) == kThreads; });

        go.store(true, std::memory_order_release);
        for (auto& thread : threads)
            thread.join();

        REQUIRE(all_ready);
        REQUIRE_FALSE(timed_out.load());

        // The gate is a read-modify-write over the observed-hash map. Every
        // thread sees the same new content, so exactly one of them may be
        // told to reload; two claims means two reloads for one edit.
        REQUIRE(claims.load() == 1);
    }

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader teardown stops the watcher before releasing its state",
          "[view][hotreload][rt-safety]") {
    constexpr int kRounds = 8;

    for (int round = 0; round < kRounds; ++round) {
        auto tmp_dir = make_temp_dir("pulp_hotreload_teardown");
        auto entry = tmp_dir / "main.js";

        write_js_file(entry, "// entry v0");

        std::atomic<bool> stop{false};
        std::thread churn([&] {
            for (int i = 1; !stop.load(std::memory_order_relaxed); ++i) {
                write_js_file(entry, "// entry v" + std::to_string(i));
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });

        {
            // Destroyed while the watcher thread is dispatching into
            // `on_file_changed`, which writes the members declared after
            // `watcher_`. Releasing the watcher last leaves that thread
            // writing into storage the destructor has already reclaimed.
            HotReloader reloader(tmp_dir, "main.js", [](const std::string&) {});
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            REQUIRE(reloader.watched_path_ == tmp_dir);
        }

        stop.store(true, std::memory_order_relaxed);
        churn.join();
        std::filesystem::remove_all(tmp_dir);
    }
}
