#include <catch2/catch_test_macros.hpp>
#include <pulp/view/hot_reload.hpp>
#include <fstream>
#include <thread>
#include <filesystem>

using namespace pulp::view;

static void write_js_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream f(path);
    f << content;
    f.close();
}

TEST_CASE("HotReloader detects file changes", "[view][hotreload]") {
    auto tmp_dir = std::filesystem::temp_directory_path() / "pulp_hotreload_test";
    std::filesystem::create_directories(tmp_dir);
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

    // Wait for the watcher to detect the change
    bool detected = false;
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (reloader.poll_reload()) {
            detected = true;
            break;
        }
    }

    REQUIRE(detected);
    REQUIRE(reloaded_code.find("modified version") != std::string::npos);
    REQUIRE(reloader.reload_count() == 1);

    // Clean up
    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader reload_count increments", "[view][hotreload]") {
    auto tmp_dir = std::filesystem::temp_directory_path() / "pulp_hotreload_test2";
    std::filesystem::create_directories(tmp_dir);
    auto js_file = tmp_dir / "ui.js";

    write_js_file(js_file, "// v1");

    uint32_t reload_count = 0;
    HotReloader reloader(js_file, [&](const std::string&) {
        ++reload_count;
    });

    // Initially zero
    REQUIRE(reloader.reload_count() == 0);

    // Modify the JS file
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    write_js_file(js_file, "// v2");

    bool detected = false;
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (reloader.poll_reload()) {
            detected = true;
            break;
        }
    }

    REQUIRE(detected);
    REQUIRE(reloader.reload_count() >= 1);

    // Clean up
    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("HotReloader directory watching", "[view][hotreload]") {
    auto tmp_dir = std::filesystem::temp_directory_path() / "pulp_hotreload_test3";
    std::filesystem::create_directories(tmp_dir);
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

    bool detected = false;
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (reloader.poll_reload()) {
            detected = true;
            break;
        }
    }

    REQUIRE(detected);
    REQUIRE(latest_code.find("v2") != std::string::npos);

    // Clean up
    std::filesystem::remove_all(tmp_dir);
}
