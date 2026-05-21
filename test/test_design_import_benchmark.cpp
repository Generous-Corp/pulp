#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pulp::platform;

namespace {

fs::path bench_binary() {
    if (const char* env = std::getenv("PULP_DESIGN_IMPORT_BENCH_PATH"); env && *env) {
        return fs::path(env);
    }
#ifdef PULP_DESIGN_IMPORT_BENCH_PATH
    return fs::path(PULP_DESIGN_IMPORT_BENCH_PATH);
#else
    return fs::current_path() / ".." / "tools" / "import-design" / "pulp-design-import-bench";
#endif
}

std::string read_all(const fs::path& path) {
    std::ifstream in(path);
    if (!in.is_open()) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool contains_key(const std::string& json, const std::string& key) {
    return json.find("\"" + key + "\"") != std::string::npos;
}

fs::path unique_output_path(const std::string& lane) {
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    return fs::temp_directory_path() /
           ("pulp-design-import-bench-" + lane + "-" + std::to_string(tick) + ".json");
}

void set_env_var(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void set_no_launch_env() {
    set_env_var("PULP_DISABLE_PLUGIN_EDITOR", "1");
    set_env_var("PULP_HEADLESS", "1");
    set_env_var("PULP_TEST_MODE", "1");
    set_env_var("PULP_INSPECTOR_NO_LAUNCH", "1");
}

double extract_number(const std::string& json, const std::string& key) {
    const auto needle = "\"" + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return std::nan("");
    pos = json.find(':', pos);
    if (pos == std::string::npos) return std::nan("");
    ++pos;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\n' || json[pos] == '\t')) {
        ++pos;
    }
    auto end = pos;
    while (end < json.size()) {
        const char c = json[end];
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' ||
            c == '+' || c == 'e' || c == 'E') {
            ++end;
        } else {
            break;
        }
    }
    if (end == pos) return std::nan("");
    try {
        return std::stod(json.substr(pos, end - pos));
    } catch (...) {
        return std::nan("");
    }
}

void require_lane_schema(const std::string& json, const std::string& lane) {
    REQUIRE_FALSE(json.empty());
    REQUIRE(json.find("\"lane\": \"" + lane + "\"") != std::string::npos);
    REQUIRE(contains_key(json, "schema"));
    REQUIRE(contains_key(json, "fixture"));
    REQUIRE(contains_key(json, "startup"));
    REQUIRE(contains_key(json, "first_frame_ms"));
    REQUIRE(contains_key(json, "first_frame_paint_commands"));
    REQUIRE(contains_key(json, "idle"));
    REQUIRE(contains_key(json, "interactive"));
    REQUIRE(contains_key(json, "frame_ms_median"));
    REQUIRE(contains_key(json, "frame_ms_p99"));
    REQUIRE(contains_key(json, "cpu_frame_ms_median"));
    REQUIRE(contains_key(json, "cpu_frame_ms_p99"));
    REQUIRE(contains_key(json, "rss_median_bytes"));
    REQUIRE(contains_key(json, "rss_p99_bytes"));
    REQUIRE(contains_key(json, "rss_peak_bytes"));
    REQUIRE(contains_key(json, "js_evaluations_total"));

    const double first_frame = extract_number(json, "first_frame_ms");
    const double commands = extract_number(json, "first_frame_paint_commands");
    const double samples = extract_number(json, "samples");
    REQUIRE_FALSE(std::isnan(first_frame));
    REQUIRE_FALSE(std::isnan(commands));
    REQUIRE_FALSE(std::isnan(samples));
    REQUIRE(first_frame >= 0.0);
    REQUIRE(commands > 0.0);
    REQUIRE(samples > 0.0);
}

}  // namespace

TEST_CASE("design-import benchmark emits live and baked-native lane JSON",
          "[design-import][benchmark]") {
    set_no_launch_env();
    const auto bin = bench_binary();
    REQUIRE(fs::exists(bin));

    for (const auto& lane : {"live", "baked-native"}) {
        const auto out = unique_output_path(lane);
        fs::remove(out);
        std::vector<std::string> args = {
            "--lane=" + std::string(lane),
            "--idle-ms=40",
            "--interactive-ms=40",
            "--target-fps=200",
            "--output=" + out.string(),
        };
        auto r = exec(bin.string(), args, /*timeout_ms=*/30000);

        REQUIRE_FALSE(r.timed_out);
        REQUIRE(r.exit_code == 0);
        REQUIRE(fs::exists(out));
        require_lane_schema(read_all(out), lane);
        fs::remove(out);
    }
}
