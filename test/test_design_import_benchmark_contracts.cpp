#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#define main pulp_design_import_benchmark_main_for_test
#include "../tools/import-design/design_import_benchmark.cpp"
#undef main

namespace {

std::filesystem::path temp_path(const char* name) {
    return std::filesystem::temp_directory_path() /
           ("pulp-design-bench-contract-" + std::string(name) + ".json");
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

std::vector<char*> argv_from(std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args)
        argv.push_back(arg.data());
    return argv;
}

std::optional<Config> parse_args_vec(std::vector<std::string> args) {
    auto argv = argv_from(args);
    return parse_args(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

TEST_CASE("design-import benchmark escapes JSON strings used in reports",
          "[design-import][benchmark][coverage]") {
    REQUIRE(json_escape("plain") == "plain");
    REQUIRE(json_escape("quote\"slash\\") == "quote\\\"slash\\\\");
    REQUIRE(json_escape("line\nfeed") == "line\\nfeed");
    REQUIRE(json_escape("carriage\rreturn") == "carriage\\rreturn");
    REQUIRE(json_escape("tab\tstop") == "tab\\tstop");
    REQUIRE(json_escape(std::string("unit") + static_cast<char>(0x1f)) == "unit\\u001f");
    REQUIRE(json_escape(std::string("nul\0byte", 8)) == "nul\\u0000byte");
}

TEST_CASE("design-import benchmark parse_int accepts complete integers only",
          "[design-import][benchmark][coverage]") {
    int value = 0;
    REQUIRE(parse_int("42", value));
    REQUIRE(value == 42);
    REQUIRE(parse_int("-7", value));
    REQUIRE(value == -7);

    REQUIRE_FALSE(parse_int("", value));
    REQUIRE_FALSE(parse_int("12ms", value));
    REQUIRE_FALSE(parse_int("1.5", value));
    REQUIRE_FALSE(parse_int("abc", value));
}

TEST_CASE("design-import benchmark argument parser supports split and equals forms",
          "[design-import][benchmark][coverage]") {
    auto config = parse_args_vec({"bench", "--lane=baked-cpp", "--idle-ms=25",
                                  "--interactive-ms", "50", "--target-fps=120",
                                  "--output", "out.json"});
    REQUIRE(config.has_value());
    REQUIRE(config->lane == "baked-cpp");
    REQUIRE(config->idle_ms == 25);
    REQUIRE(config->interactive_ms == 50);
    REQUIRE(config->target_fps == 120);
    REQUIRE(config->output_path == std::filesystem::path("out.json"));

    auto split_lane = parse_args_vec({"bench", "--lane", "baked-native",
                                      "--idle-ms", "0", "--interactive-ms=1",
                                      "--target-fps", "2"});
    REQUIRE(split_lane.has_value());
    REQUIRE(split_lane->lane == "baked-native");
    REQUIRE(split_lane->idle_ms == 0);
    REQUIRE(split_lane->interactive_ms == 1);
    REQUIRE(split_lane->target_fps == 2);
}

TEST_CASE("design-import benchmark argument parser rejects invalid shapes",
          "[design-import][benchmark][coverage]") {
    REQUIRE_FALSE(parse_args_vec({"bench", "--lane=nope"}).has_value());
    REQUIRE_FALSE(parse_args_vec({"bench", "--lane"}).has_value());
    REQUIRE_FALSE(parse_args_vec({"bench", "--idle-ms=1x"}).has_value());
    REQUIRE_FALSE(parse_args_vec({"bench", "--interactive-ms"}).has_value());
    REQUIRE_FALSE(parse_args_vec({"bench", "--target-fps=fast"}).has_value());
    REQUIRE_FALSE(parse_args_vec({"bench", "--output"}).has_value());
    REQUIRE_FALSE(parse_args_vec({"bench", "--unknown"}).has_value());
}

TEST_CASE("design-import benchmark argument parser clamps timing knobs",
          "[design-import][benchmark][coverage]") {
    auto config = parse_args_vec({"bench", "--idle-ms=-10",
                                  "--interactive-ms=-20", "--target-fps=0"});
    REQUIRE(config.has_value());
    REQUIRE(config->lane == "live");
    REQUIRE(config->idle_ms == 0);
    REQUIRE(config->interactive_ms == 0);
    REQUIRE(config->target_fps == 1);
}

TEST_CASE("design-import benchmark JSON report contains escaped config and metrics",
          "[design-import][benchmark][coverage]") {
    Config config;
    config.lane = "lane\"x";
    config.target_fps = 144;

    StartupMetrics startup;
    startup.build_ms = 1.25;
    startup.first_frame_ms = 2.5;
    startup.first_frame_render_ms = 0.75;
    startup.first_frame_paint_commands = 33;
    startup.rss_after_first_frame_bytes = 4096;

    PhaseMetrics idle;
    idle.duration_ms = 10;
    idle.samples = 2;
    idle.frame_ms_median = 3.5;
    idle.paint_commands_last = 44;

    PhaseMetrics interactive;
    interactive.duration_ms = 20;
    interactive.samples = 3;
    interactive.cpu_ms = 4.5;
    interactive.js_evaluations = 7;

    auto json = make_json(config, startup, idle, interactive);
    REQUIRE(json.find("\"schema\": \"pulp-design-import-benchmark-v1\"") != std::string::npos);
    REQUIRE(json.find("\"lane\": \"lane\\\"x\"") != std::string::npos);
    REQUIRE(json.find("\"target_fps\": 144") != std::string::npos);
    REQUIRE(json.find("\"first_frame_paint_commands\": 33") != std::string::npos);
    REQUIRE(json.find("\"idle\": {") != std::string::npos);
    REQUIRE(json.find("\"interactive\": {") != std::string::npos);
}

TEST_CASE("design-import benchmark write_file handles stdout sentinel and nested paths",
          "[design-import][benchmark][coverage]") {
    REQUIRE(write_file({}, "stdout sentinel"));

    auto path = temp_path("nested");
    std::filesystem::remove_all(path.parent_path() / "pulp-design-bench-contract-dir");
    auto nested = path.parent_path() / "pulp-design-bench-contract-dir" / "report.json";

    REQUIRE(write_file(nested, "payload"));
    REQUIRE(std::filesystem::is_regular_file(nested));
    REQUIRE(read_text(nested) == "payload");

    std::filesystem::remove_all(nested.parent_path());
}
