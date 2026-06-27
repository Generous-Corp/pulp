// Shell-out integration test for the zero-copy benchmark mode.
//
// Spawns `pulp-ui-preview --benchmark-seconds=2 --widget=oscilloscope
// --output=<tmp>.json`, parses the emitted JSON, and asserts the
// documented schema (required keys, non-negative values,
// memory_bandwidth_fraction ∈ [0, 1]).
//
// Only compiled when PULP_BENCHMARK AND APPLE AND NOT PULP_IOS because
// that's the same guard the ui-preview target uses. JSON parsing is
// deliberately kept to simple string scans to avoid pulling a new
// dependency into the test's link line.

#include <catch2/catch_test_macros.hpp>
#include <pulp/platform/child_process.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pulp::platform;

namespace {

fs::path ui_preview_binary() {
    if (const char* env = std::getenv("PULP_UI_PREVIEW_PATH"); env && *env) {
        return fs::path(env);
    }
    // Tests run from <build>/test; ui-preview lands in
    // <build>/examples/ui-preview/pulp-ui-preview.
    return fs::current_path() / ".." / "examples" / "ui-preview"
           / "pulp-ui-preview";
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

// Extract the first numeric value associated with "key" in a
// flat-ish JSON payload. Returns NaN on miss. The benchmark JSON is
// produced by `choc::json::toString(..., /*pretty=*/true)` — one
// member per line, with numbers in scientific or fixed notation — so
// a simple locate-colon scan is sufficient.
double extract_number(const std::string& json, const std::string& key) {
    const auto k = "\"" + key + "\"";
    auto pos = json.find(k);
    if (pos == std::string::npos) return std::nan("");
    pos = json.find(':', pos);
    if (pos == std::string::npos) return std::nan("");
    ++pos;
    // Skip whitespace.
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n')) {
        ++pos;
    }
    // Allow a leading '-' then digits/dot/exponent.
    auto end = pos;
    while (end < json.size()) {
        char c = json[end];
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' ||
            c == 'e' || c == 'E') {
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

}  // namespace

TEST_CASE("pulp-ui-preview --benchmark-seconds emits a schema-conformant JSON",
          "[bench][integration]") {
    const auto bin = ui_preview_binary();
    if (!fs::exists(bin)) {
        SUCCEED("pulp-ui-preview not built for this test run; skipping");
        return;
    }

    auto tmp_json = fs::temp_directory_path() / "pulp-bench-integration-test.json";
    fs::remove(tmp_json);

    std::vector<std::string> args = {
        "--benchmark-seconds=2",
        "--widget=oscilloscope",
        "--output=" + tmp_json.string(),
        "--target-fps=60",
    };
    auto r = exec(bin.string(), args, /*timeout_ms=*/30000);

    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(tmp_json));

    const auto json = read_all(tmp_json);
    REQUIRE_FALSE(json.empty());

    // Required top-level keys (must match bench_diff.py's docstring).
    REQUIRE(contains_key(json, "host"));
    REQUIRE(contains_key(json, "date"));
    REQUIRE(contains_key(json, "pulp_commit"));
    REQUIRE(contains_key(json, "platform"));
    REQUIRE(contains_key(json, "widget"));
    REQUIRE(contains_key(json, "seconds"));
    REQUIRE(contains_key(json, "target_fps"));
    REQUIRE(contains_key(json, "samples"));
    REQUIRE(contains_key(json, "per_frame_us"));
    REQUIRE(contains_key(json, "per_frame_bytes"));
    REQUIRE(contains_key(json, "frame_budget_us"));
    REQUIRE(contains_key(json, "memory_bandwidth_fraction"));

    // Required per_frame_us sub-keys — note the mixed _us / no-suffix
    // naming is intentional (bench_diff.py shipped first).
    REQUIRE(contains_key(json, "audio_to_triplebuffer_copy"));
    REQUIRE(contains_key(json, "triplebuffer_publish_latency"));
    REQUIRE(contains_key(json, "gpu_upload_us"));
    REQUIRE(contains_key(json, "gpu_readback_us"));
    REQUIRE(contains_key(json, "gpu_dispatch_us"));
    REQUIRE(contains_key(json, "total_frame_us"));

    REQUIRE(contains_key(json, "cpu_to_gpu_bytes"));
    REQUIRE(contains_key(json, "gpu_to_cpu_bytes"));

    // Values are non-negative.
    const double audio_copy = extract_number(json, "audio_to_triplebuffer_copy");
    const double tb_publish = extract_number(json, "triplebuffer_publish_latency");
    const double gpu_up = extract_number(json, "gpu_upload_us");
    const double gpu_rb = extract_number(json, "gpu_readback_us");
    const double gpu_dp = extract_number(json, "gpu_dispatch_us");
    const double frame_total = extract_number(json, "total_frame_us");
    const double cpu_to_gpu = extract_number(json, "cpu_to_gpu_bytes");
    const double gpu_to_cpu = extract_number(json, "gpu_to_cpu_bytes");
    const double mbf = extract_number(json, "memory_bandwidth_fraction");
    const double samples = extract_number(json, "samples");

    REQUIRE_FALSE(std::isnan(audio_copy));
    REQUIRE_FALSE(std::isnan(tb_publish));
    REQUIRE_FALSE(std::isnan(gpu_up));
    REQUIRE_FALSE(std::isnan(gpu_rb));
    REQUIRE_FALSE(std::isnan(gpu_dp));
    REQUIRE_FALSE(std::isnan(frame_total));
    REQUIRE_FALSE(std::isnan(cpu_to_gpu));
    REQUIRE_FALSE(std::isnan(gpu_to_cpu));
    REQUIRE_FALSE(std::isnan(mbf));
    REQUIRE_FALSE(std::isnan(samples));

    REQUIRE(audio_copy >= 0.0);
    REQUIRE(tb_publish >= 0.0);
    REQUIRE(gpu_up >= 0.0);
    REQUIRE(gpu_rb >= 0.0);
    REQUIRE(gpu_dp >= 0.0);
    REQUIRE(frame_total >= 0.0);
    REQUIRE(cpu_to_gpu >= 0.0);
    REQUIRE(gpu_to_cpu >= 0.0);
    REQUIRE(mbf >= 0.0);
    REQUIRE(mbf <= 1.0);
    // A 2-second @ 60 FPS run should produce at least a handful of
    // frames even on a slow sandbox. 30 is a generous floor.
    REQUIRE(samples >= 30.0);

    fs::remove(tmp_json);
}
