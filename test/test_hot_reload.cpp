#include <catch2/catch_test_macros.hpp>
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

TEST_CASE("HotReloader seeds observed JS files and ignores stale events",
          "[view][hotreload][codecov]") {
    auto tmp_dir = make_temp_dir("pulp_hotreload_seed");
    auto entry = tmp_dir / "main.js";
    auto module = tmp_dir / "module.mjs";
    auto ignored = tmp_dir / "notes.txt";

    write_js_file(entry, "// entry v1");
    write_js_file(module, "// module v1");
    write_js_file(ignored, "not javascript");

    HotReloader reloader(tmp_dir, "main.js", [](const std::string&) {});

    REQUIRE(reloader.observed_write_times_.count(entry.lexically_normal().string()) == 1);
    REQUIRE(reloader.observed_write_times_.count(module.lexically_normal().string()) == 1);
    REQUIRE(reloader.observed_write_times_.count(ignored.lexically_normal().string()) == 0);
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
          "[view][hotreload][codecov]") {
    auto tmp_dir = make_temp_dir("pulp_hotreload_non_js");
    auto text_file = tmp_dir / "notes.txt";
    write_js_file(text_file, "not javascript");

    HotReloader reloader(text_file, [](const std::string&) {});

    REQUIRE(reloader.observed_write_times_.empty());
    REQUIRE_FALSE(reloader.poll_reload());

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader file watcher ignores unsupported changes",
          "[view][hotreload][codecov]") {
    auto tmp_dir = make_temp_dir("pulp_hotreload_direct_filter");
    auto entry = tmp_dir / "main.js";
    auto text_file = tmp_dir / "notes.txt";

    write_js_file(entry, "// entry v1");
    write_js_file(text_file, "not javascript");

    HotReloader reloader(tmp_dir, "main.js", [](const std::string&) {});

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_js_file(text_file, "still not javascript");

    for (int i = 0; i < 8; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        REQUIRE_FALSE(reloader.poll_reload());
    }

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader module file change reloads directory entry",
          "[view][hotreload][codecov]") {
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
          "[view][hotreload][codecov]") {
    auto tmp_dir = make_temp_dir("pulp_hotreload_no_callback");
    auto entry = tmp_dir / "main.js";
    write_js_file(entry, "// entry");

    HotReloader reloader(entry, {});
    reloader.pending_code_ = "// pending";
    reloader.has_pending_ = true;

    REQUIRE(reloader.poll_reload());
    REQUIRE(reloader.reload_count() == 0);
    REQUIRE_FALSE(reloader.poll_reload());

    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader empty or missing entry files do not schedule reloads",
          "[view][hotreload][codecov]") {
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
