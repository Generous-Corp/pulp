#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
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

int run_benchmark_main(std::vector<std::string> args) {
    auto argv = argv_from(args);
    return pulp_design_import_benchmark_main_for_test(static_cast<int>(argv.size()),
                                                      argv.data());
}

const IRNode* find_ir_node(const IRNode& node, std::string_view id) {
    if (node.stable_anchor_id == id) return &node;
    for (const auto& child : node.children) {
        if (const auto* found = find_ir_node(child, id)) return found;
    }
    return nullptr;
}

class CountingFixture final : public BenchmarkFixture {
public:
    CountingFixture() {
        root_.set_id("counting-root");
        root_.set_bounds({0, 0, 12, 12});
    }

    View& root() override { return root_; }

    void step(int frame) override {
        ++steps;
        last_frame = frame;
    }

    int js_evaluation_count() const override { return steps + 10; }

    View root_;
    int steps = 0;
    int last_frame = -1;
};

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
    REQUIRE(parse_int(" 12", value));
    REQUIRE(value == 12);
    REQUIRE_FALSE(parse_int("12 ", value));
    REQUIRE(parse_int("+12", value));
    REQUIRE(value == 12);
    REQUIRE_FALSE(parse_int("999999999999999999999999", value));
    REQUIRE_FALSE(parse_int("0x10", value));
    REQUIRE_FALSE(parse_int("--1", value));
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
    REQUIRE_FALSE(parse_args_vec({"bench", "--lane="}).has_value());
    REQUIRE_FALSE(parse_args_vec({"bench", "--lane"}).has_value());
    REQUIRE_FALSE(parse_args_vec({"bench", "--idle-ms="}).has_value());
    REQUIRE_FALSE(parse_args_vec({"bench", "--idle-ms=1x"}).has_value());
    REQUIRE_FALSE(parse_args_vec({"bench", "--interactive-ms="}).has_value());
    REQUIRE_FALSE(parse_args_vec({"bench", "--interactive-ms"}).has_value());
    REQUIRE_FALSE(parse_args_vec({"bench", "--target-fps="}).has_value());
    REQUIRE_FALSE(parse_args_vec({"bench", "--target-fps=fast"}).has_value());
    REQUIRE_FALSE(parse_args_vec({"bench", "--output"}).has_value());
    REQUIRE_FALSE(parse_args_vec({"bench", "--unknown"}).has_value());
}

TEST_CASE("design-import benchmark argument parser preserves explicit empty output",
          "[design-import][benchmark][coverage][requested]") {
    auto config = parse_args_vec({"bench", "--output="});
    REQUIRE(config.has_value());
    CHECK(config->lane == "live");
    CHECK(config->output_path == std::filesystem::path(""));
    CHECK(config->idle_ms == 60000);
    CHECK(config->interactive_ms == 60000);
    CHECK(config->target_fps == 60);
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

TEST_CASE("design-import benchmark argument parser uses last explicit option",
          "[design-import][benchmark][coverage][requested]") {
    auto config = parse_args_vec({"bench",
                                  "--lane", "live",
                                  "--lane=baked-cpp",
                                  "--idle-ms=10",
                                  "--idle-ms", "20",
                                  "--interactive-ms=30",
                                  "--interactive-ms", "40",
                                  "--target-fps=60",
                                  "--target-fps", "120",
                                  "--output", "first.json",
                                  "--output=second.json"});
    REQUIRE(config.has_value());
    REQUIRE(config->lane == "baked-cpp");
    REQUIRE(config->idle_ms == 20);
    REQUIRE(config->interactive_ms == 40);
    REQUIRE(config->target_fps == 120);
    REQUIRE(config->output_path == std::filesystem::path("second.json"));
}

TEST_CASE("design-import benchmark fixture and phase helpers fail closed",
          "[design-import][benchmark][coverage][requested]") {
    REQUIRE(make_fixture("not-a-lane") == nullptr);

    CountingFixture fixture;
    auto idle = run_phase(fixture, 0, 0, false);
    REQUIRE(idle.duration_ms == 0);
    REQUIRE(idle.samples == 0);
    REQUIRE(idle.paint_commands_last == 0);
    REQUIRE(fixture.steps == 0);
    REQUIRE(fixture.last_frame == -1);
    REQUIRE(fixture.js_evaluation_count() == 10);
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

TEST_CASE("design-import benchmark JSON report includes complete phase fields",
          "[design-import][benchmark][coverage]") {
    Config config;
    config.lane = "baked-native";
    config.target_fps = 90;

    StartupMetrics startup;
    startup.build_ms = 11.5;
    startup.first_frame_ms = 22.25;
    startup.first_frame_render_ms = 3.75;
    startup.first_frame_paint_commands = 123;
    startup.rss_after_first_frame_bytes = 456789;

    PhaseMetrics idle;
    idle.duration_ms = 100;
    idle.samples = 4;
    idle.cpu_ms = 5.5;
    idle.cpu_frame_ms_median = 1.25;
    idle.cpu_frame_ms_p99 = 2.5;
    idle.frame_ms_median = 16.0;
    idle.frame_ms_p99 = 17.5;
    idle.frame_ms_max = 20.0;
    idle.rss_median_bytes = 1000;
    idle.rss_p99_bytes = 2000;
    idle.rss_peak_bytes = 3000;
    idle.paint_commands_last = 44;
    idle.js_evaluations = 7;

    PhaseMetrics interactive = idle;
    interactive.duration_ms = 200;
    interactive.samples = 8;
    interactive.paint_commands_last = 88;
    interactive.js_evaluations = 14;

    auto json = make_json(config, startup, idle, interactive);
    REQUIRE(json.find("\"lane\": \"baked-native\"") != std::string::npos);
    REQUIRE(json.find("\"fixture\": \"phase5-imported-plugin-panel\"") != std::string::npos);
    REQUIRE(json.find("\"host\": \"") != std::string::npos);
    REQUIRE(json.find("\"platform\": \"") != std::string::npos);
    REQUIRE(json.find("\"target_fps\": 90") != std::string::npos);
    REQUIRE(json.find("\"build_ms\": 11.5000") != std::string::npos);
    REQUIRE(json.find("\"first_frame_ms\": 22.2500") != std::string::npos);
    REQUIRE(json.find("\"first_frame_render_ms\": 3.7500") != std::string::npos);
    REQUIRE(json.find("\"first_frame_paint_commands\": 123") != std::string::npos);
    REQUIRE(json.find("\"rss_after_first_frame_bytes\": 456789") != std::string::npos);
    REQUIRE(json.find("\"cpu_frame_ms_median\": 1.2500") != std::string::npos);
    REQUIRE(json.find("\"cpu_frame_ms_p99\": 2.5000") != std::string::npos);
    REQUIRE(json.find("\"frame_ms_median\": 16.0000") != std::string::npos);
    REQUIRE(json.find("\"frame_ms_p99\": 17.5000") != std::string::npos);
    REQUIRE(json.find("\"frame_ms_max\": 20.0000") != std::string::npos);
    REQUIRE(json.find("\"rss_median_bytes\": 1000") != std::string::npos);
    REQUIRE(json.find("\"rss_p99_bytes\": 2000") != std::string::npos);
    REQUIRE(json.find("\"rss_peak_bytes\": 3000") != std::string::npos);
    REQUIRE(json.find("\"paint_commands_last\": 88") != std::string::npos);
    REQUIRE(json.find("\"js_evaluations_total\": 14") != std::string::npos);
    REQUIRE(json.find("\"interaction_model\": \"value drag plus overflow-panel scroll offset;") != std::string::npos);
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

TEST_CASE("design-import benchmark metric helpers handle edge inputs",
          "[design-import][benchmark][coverage]") {
    REQUIRE(percentile({}, 50.0) == 0.0);
    REQUIRE(percentile({5.0}, 99.0) == 5.0);
    REQUIRE(percentile({30.0, 10.0, 20.0}, 50.0) == 20.0);
    REQUIRE(percentile({0.0, 100.0}, 25.0) == 25.0);

    REQUIRE(cpu_clock_delta_ms(static_cast<std::clock_t>(-1), 10) == 0.0);
    REQUIRE(cpu_clock_delta_ms(20, static_cast<std::clock_t>(-1)) == 0.0);
    REQUIRE(cpu_clock_delta_ms(20, 10) == 0.0);
    REQUIRE(cpu_clock_delta_ms(0, CLOCKS_PER_SEC) == Catch::Approx(1000.0));
}

TEST_CASE("design-import benchmark baked IR fixture carries expected controls",
          "[design-import][benchmark][coverage]") {
    const auto ir = build_baked_ir();

    REQUIRE(ir.source == DesignSource::jsx);
    REQUIRE(ir.source_adapter == "phase5-benchmark-fixture");
    REQUIRE(ir.root.stable_anchor_id == "bench-root");
    REQUIRE(ir.root.children.size() == 1);

    const auto* panel = find_ir_node(ir.root, "bench-panel");
    REQUIRE(panel != nullptr);
    REQUIRE(panel->children.size() == 8);
    REQUIRE(panel->style.background_color == "#1f2030");
    REQUIRE(panel->style.overflow == "hidden");

    const auto* drive = find_ir_node(ir.root, "drive");
    REQUIRE(drive != nullptr);
    REQUIRE(drive->audio_widget == AudioWidgetType::knob);
    REQUIRE(drive->audio_label == "Drive");
    REQUIRE(drive->audio_default == Catch::Approx(0.64f));

    const auto* shape = find_ir_node(ir.root, "shape");
    REQUIRE(shape != nullptr);
    REQUIRE(shape->audio_widget == AudioWidgetType::xy_pad);
    REQUIRE(shape->attributes.at("x") == "0.35");

    const auto* viewport = find_ir_node(ir.root, "preset-viewport");
    REQUIRE(viewport != nullptr);
    REQUIRE(viewport->style.overflow == "scroll");

    const auto* content = find_ir_node(ir.root, "preset-content");
    REQUIRE(content != nullptr);
    REQUIRE(content->children.size() == 14);
    REQUIRE(content->children.front().text_content.find("Preset 01") != std::string::npos);
    REQUIRE(content->children.back().text_content.find("Preset 14") != std::string::npos);
}

TEST_CASE("design-import benchmark fixture factory and render path are deterministic",
          "[design-import][benchmark][coverage]") {
    REQUIRE(make_fixture("missing") == nullptr);

    auto fixture = make_fixture("baked-cpp");
    REQUIRE(fixture != nullptr);
    REQUIRE(fixture->root().id() == "bench-root");

    const auto initial_commands = render_frame(fixture->root());
    REQUIRE(initial_commands > 0);

    fixture->step(8);
    REQUIRE(fixture->js_evaluation_count() == 0);
    REQUIRE(render_frame(fixture->root()) == initial_commands);
}

TEST_CASE("design-import benchmark fixture factory creates the baked-native lane",
          "[design-import][benchmark][coverage]") {
    auto baked_native = make_fixture("baked-native");
    REQUIRE(baked_native != nullptr);
    REQUIRE(baked_native->root().id() == "bench-root");
    REQUIRE(render_frame(baked_native->root()) > 0);
    baked_native->step(12);
    REQUIRE(baked_native->js_evaluation_count() == 0);
}

TEST_CASE("design-import benchmark run_phase records idle and interactive contracts",
          "[design-import][benchmark][coverage]") {
    CountingFixture fixture;

    const auto zero = run_phase(fixture, 0, 60, true);
    REQUIRE(zero.duration_ms == 0);
    REQUIRE(zero.samples == 0);
    REQUIRE(zero.cpu_ms == 0.0);
    REQUIRE(zero.paint_commands_last == 0);
    REQUIRE(fixture.steps == 0);

    const auto idle = run_phase(fixture, 2, 10000, false);
    REQUIRE(idle.duration_ms == 2);
    REQUIRE(idle.samples > 0);
    REQUIRE(idle.js_evaluations == 10);
    REQUIRE(fixture.steps == 0);

    const auto interactive = run_phase(fixture, 2, 10000, true);
    REQUIRE(interactive.duration_ms == 2);
    REQUIRE(interactive.samples > 0);
    REQUIRE(fixture.steps == interactive.samples);
    REQUIRE(fixture.last_frame == interactive.samples - 1);
}

TEST_CASE("design-import benchmark main writes reports and rejects invalid args",
          "[design-import][benchmark][coverage]") {
    auto output = temp_path("main-report");
    std::filesystem::remove(output);

    REQUIRE(run_benchmark_main({"bench", "--lane", "baked-cpp",
                                "--idle-ms", "0",
                                "--interactive-ms", "0",
                                "--target-fps", "30",
                                "--output", output.string()}) == 0);
    REQUIRE(std::filesystem::is_regular_file(output));
    auto report = read_text(output);
    REQUIRE(report.find("\"schema\": \"pulp-design-import-benchmark-v1\"") != std::string::npos);
    REQUIRE(report.find("\"lane\": \"baked-cpp\"") != std::string::npos);
    REQUIRE(report.find("\"samples\": 0") != std::string::npos);

    REQUIRE(run_benchmark_main({"bench", "--lane", "invalid"}) == 2);

    std::filesystem::remove(output);
}
