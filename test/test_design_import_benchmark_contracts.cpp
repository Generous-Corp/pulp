#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#define main pulp_design_import_benchmark_main_for_test
#include "../tools/import-design/design_import_benchmark.cpp"

namespace {
Config lane_config(std::string lane) {
    Config config;
    config.lane = std::move(lane);
    return config;
}
} // namespace
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

// Allocates a fixed number of heap blocks per interactive step, so a per-frame
// allocation count can be checked against a known delta.
class AllocatingFixture final : public BenchmarkFixture {
  public:
    explicit AllocatingFixture(int per_step) : per_step_(per_step) {
        root_.set_id("allocating-root");
        root_.set_bounds({0, 0, 12, 12});
    }

    View& root() override {
        return root_;
    }

    void step(int frame) override {
        held_.clear();
        for (int i = 0; i < per_step_; ++i)
            held_.push_back(std::make_unique<int>(frame + i));
    }

    int js_evaluation_count() const override {
        return 0;
    }

  private:
    View root_;
    int per_step_ = 0;
    std::vector<std::unique_ptr<int>> held_;
};

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
          "[design-import][benchmark]") {
    REQUIRE(json_escape("plain") == "plain");
    REQUIRE(json_escape("quote\"slash\\") == "quote\\\"slash\\\\");
    REQUIRE(json_escape("line\nfeed") == "line\\nfeed");
    REQUIRE(json_escape("carriage\rreturn") == "carriage\\rreturn");
    REQUIRE(json_escape("tab\tstop") == "tab\\tstop");
    REQUIRE(json_escape(std::string("unit") + static_cast<char>(0x1f)) == "unit\\u001f");
    REQUIRE(json_escape(std::string("nul\0byte", 8)) == "nul\\u0000byte");
}

TEST_CASE("design-import benchmark parse_int accepts complete integers only",
          "[design-import][benchmark]") {
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
          "[design-import][benchmark]") {
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

    auto rebuild_lane = parse_args_vec({"bench", "--lane=baked-native-rebuild"});
    REQUIRE(rebuild_lane.has_value());
    REQUIRE(rebuild_lane->lane == "baked-native-rebuild");
}

TEST_CASE("design-import benchmark argument parser rejects invalid shapes",
          "[design-import][benchmark]") {
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
          "[design-import][benchmark]") {
    auto config = parse_args_vec({"bench", "--output="});
    REQUIRE(config.has_value());
    CHECK(config->lane == "live");
    CHECK(config->output_path == std::filesystem::path(""));
    CHECK(config->idle_ms == 60000);
    CHECK(config->interactive_ms == 60000);
    CHECK(config->target_fps == 60);
}

TEST_CASE("design-import benchmark argument parser clamps timing knobs",
          "[design-import][benchmark]") {
    auto config = parse_args_vec({"bench", "--idle-ms=-10",
                                  "--interactive-ms=-20", "--target-fps=0"});
    REQUIRE(config.has_value());
    REQUIRE(config->lane == "live");
    REQUIRE(config->idle_ms == 0);
    REQUIRE(config->interactive_ms == 0);
    REQUIRE(config->target_fps == 1);
}

TEST_CASE("design-import benchmark argument parser uses last explicit option",
          "[design-import][benchmark]") {
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

TEST_CASE("design-import benchmark argument parser rejects orphaned values",
          "[design-import][benchmark]") {
    REQUIRE_FALSE(parse_args_vec({"bench", "stray"}).has_value());
    REQUIRE_FALSE(parse_args_vec({"bench", "--lane", "live", "extra"}).has_value());
    REQUIRE_FALSE(parse_args_vec({"bench", "--idle-ms", "1", "--", "--target-fps", "60"}).has_value());

    auto valid = parse_args_vec({"bench", "--lane", "live",
                                 "--idle-ms", "+0",
                                 "--interactive-ms", "+2",
                                 "--target-fps", "+30"});
    REQUIRE(valid.has_value());
    REQUIRE(valid->idle_ms == 0);
    REQUIRE(valid->interactive_ms == 2);
    REQUIRE(valid->target_fps == 30);
}

TEST_CASE("design-import benchmark fixture and phase helpers fail closed",
          "[design-import][benchmark]") {
    REQUIRE(make_fixture(lane_config("not-a-lane")) == nullptr);

    CountingFixture fixture;
    auto idle = run_phase(fixture, 0, std::nullopt, 0, false);
    REQUIRE(idle.duration_ms == 0);
    REQUIRE(idle.samples == 0);
    REQUIRE(idle.paint_commands_last == 0);
    REQUIRE(fixture.steps == 0);
    REQUIRE(fixture.last_frame == -1);
    REQUIRE(fixture.js_evaluation_count() == 10);
}

TEST_CASE("design-import benchmark JSON report contains escaped config and metrics",
          "[design-import][benchmark]") {
    Config config;
    config.lane = "lane\"x";
    config.target_fps = 144;

    StartupMetrics startup;
    startup.build_ms = 1.25;
    startup.first_frame_ms = 2.5;
    startup.first_frame_render_ms = 0.75;
    startup.first_frame_paint_commands = 33;
    startup.rss_after_first_frame = {4096, "test", {}};

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

    auto json = make_json(config, std::nullopt, startup, idle, interactive, TeardownMetrics{});
    REQUIRE(json.find("\"schema\": \"pulp-design-import-benchmark-v2\"") != std::string::npos);
    REQUIRE(json.find("\"lane\": \"lane\\\"x\"") != std::string::npos);
    REQUIRE(json.find("\"target_fps\": 144") != std::string::npos);
    REQUIRE(json.find("\"first_frame_paint_commands\": 33") != std::string::npos);
    REQUIRE(json.find("\"idle\": {") != std::string::npos);
    REQUIRE(json.find("\"interactive\": {") != std::string::npos);
}

TEST_CASE("design-import benchmark JSON report includes complete phase fields",
          "[design-import][benchmark]") {
    Config config;
    config.lane = "baked-native";
    config.target_fps = 90;

    StartupMetrics startup;
    startup.build_ms = 11.5;
    startup.first_frame_ms = 22.25;
    startup.first_frame_render_ms = 3.75;
    startup.first_frame_paint_commands = 123;
    startup.rss_after_first_frame = {456789, "test", {}};

    PhaseMetrics idle;
    idle.duration_ms = 100;
    idle.samples = 4;
    idle.cpu_ms = 5.5;
    idle.cpu_frame_ms_median = 1.25;
    idle.cpu_frame_ms_p99 = 2.5;
    idle.frame_ms_median = 16.0;
    idle.frame_ms_p99 = 17.5;
    idle.frame_ms_max = 20.0;
    idle.rss.median_bytes = 1000;
    idle.rss.p99_bytes = 2000;
    idle.rss.peak_bytes = 3000;
    idle.rss.source = "test";
    idle.phys_footprint.source = "none";
    idle.phys_footprint.reason = "unavailable here";
    idle.paint_commands_last = 44;
    idle.js_evaluations = 7;

    PhaseMetrics interactive = idle;
    interactive.duration_ms = 200;
    interactive.samples = 8;
    interactive.paint_commands_last = 88;
    interactive.js_evaluations = 14;

    auto json = make_json(config, std::nullopt, startup, idle, interactive, TeardownMetrics{});
    REQUIRE(json.find("\"lane\": \"baked-native\"") != std::string::npos);
    REQUIRE(json.find("\"fixture\": \"phase5-imported-plugin-panel\"") != std::string::npos);
    REQUIRE(json.find("\"host\": \"") != std::string::npos);
    REQUIRE(json.find("\"platform\": \"") != std::string::npos);
    REQUIRE(json.find("\"target_fps\": 90") != std::string::npos);
    REQUIRE(json.find("\"build_ms\": 11.5000") != std::string::npos);
    REQUIRE(json.find("\"first_frame_ms\": 22.2500") != std::string::npos);
    REQUIRE(json.find("\"first_frame_render_ms\": 3.7500") != std::string::npos);
    REQUIRE(json.find("\"first_frame_paint_commands\": 123") != std::string::npos);
    REQUIRE(json.find("\"rss_after_first_frame\": {\"value\": 456789") != std::string::npos);
    REQUIRE(json.find("\"cpu_frame_ms_median\": 1.2500") != std::string::npos);
    REQUIRE(json.find("\"cpu_frame_ms_p99\": 2.5000") != std::string::npos);
    REQUIRE(json.find("\"frame_ms_median\": 16.0000") != std::string::npos);
    REQUIRE(json.find("\"frame_ms_p99\": 17.5000") != std::string::npos);
    REQUIRE(json.find("\"frame_ms_max\": 20.0000") != std::string::npos);
    REQUIRE(json.find("\"rss\": {\"median_bytes\": 1000, \"p99_bytes\": 2000, "
                      "\"peak_bytes\": 3000") != std::string::npos);
    // An unavailable reading is null with its reason, never a stand-in 0.
    REQUIRE(json.find("\"phys_footprint\": {\"median_bytes\": null, \"p99_bytes\": null, "
                      "\"peak_bytes\": null, \"source\": \"none\", "
                      "\"reason\": \"unavailable here\"}") != std::string::npos);
    REQUIRE(json.find("\"paint_commands_last\": 88") != std::string::npos);
    REQUIRE(json.find("\"js_evaluations_total\": 14") != std::string::npos);
    REQUIRE(json.find("\"counters\": {") != std::string::npos);
    REQUIRE(json.find("\"bridge_calls\": 0") != std::string::npos);
    REQUIRE(json.find("\"layout_passes\": 0") != std::string::npos);
    REQUIRE(json.find("\"paint_calls\": 0") != std::string::npos);
    REQUIRE(json.find("\"interaction_model\": \"value drag plus overflow-panel scroll offset;") != std::string::npos);
}

TEST_CASE("design-import benchmark write_file handles stdout sentinel and nested paths",
          "[design-import][benchmark]") {
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
          "[design-import][benchmark]") {
    REQUIRE(percentile({}, 50.0) == 0.0);
    REQUIRE(percentile({5.0}, 99.0) == 5.0);
    REQUIRE(percentile({30.0, 10.0, 20.0}, 50.0) == 20.0);
    REQUIRE(percentile({0.0, 100.0}, 25.0) == 25.0);

    REQUIRE(cpu_clock_delta_ms(static_cast<std::clock_t>(-1), 10) == 0.0);
    REQUIRE(cpu_clock_delta_ms(20, static_cast<std::clock_t>(-1)) == 0.0);
    REQUIRE(cpu_clock_delta_ms(20, 10) == 0.0);
    REQUIRE(cpu_clock_delta_ms(0, CLOCKS_PER_SEC) == Catch::Approx(1000.0));
}

TEST_CASE("design-import benchmark baked IR fixture states every widget colour",
          "[design-import][benchmark][tokens]") {
    // The fixture stands in for a captured design, and materializing it must
    // leave no widget key on Pulp's built-in colour — otherwise the baked-native
    // lane measures a tree painted by the framework rather than by the design,
    // and the benchmark is itself an instance of the gap the
    // `design-token-unmapped` diagnostic exists to report.
    //
    // Asking the mapper which keys it could not fill, rather than listing them,
    // keeps this true as the rule table grows.
    const auto ir = build_baked_ir();
    std::vector<std::string> unresolved;
    const auto theme = ir_tokens_to_theme(ir.tokens, &unresolved);
    std::string gaps;
    for (const auto& key : unresolved) gaps += (gaps.empty() ? "" : ", ") + key;
    INFO("unfilled widget keys: " << gaps);
    CHECK(unresolved.empty());
    CHECK_FALSE(unmapped_widget_token_diagnostic(unresolved).has_value());

    // The palette must actually reach the primitives, and must not be Pulp's
    // own: a fixture that happened to state the built-in colours would satisfy
    // the count above while proving nothing.
    REQUIRE(theme.colors.count("knob.arc") == 1);
    CHECK(theme.colors.at("knob.arc") == theme.colors.at("accent"));
    CHECK(theme.colors.at("meter.green") == theme.colors.at("signal-low"));
    CHECK(theme.colors.at("control.track") == theme.colors.at("line-strong"));
}

TEST_CASE("design-import benchmark baked IR fixture carries expected controls",
          "[design-import][benchmark]") {
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
          "[design-import][benchmark]") {
    REQUIRE(make_fixture(lane_config("missing")) == nullptr);

    auto fixture = make_fixture(lane_config("baked-cpp"));
    REQUIRE(fixture != nullptr);
    REQUIRE(fixture->root().id() == "bench-root");

    const auto initial_commands = render_frame(fixture->root());
    REQUIRE(initial_commands > 0);

    fixture->step(8);
    REQUIRE(fixture->js_evaluation_count() == 0);
    REQUIRE(render_frame(fixture->root()) == initial_commands);
}

TEST_CASE("design-import benchmark fixture factory creates the baked-native lane",
          "[design-import][benchmark]") {
    auto baked_native = make_fixture(lane_config("baked-native"));
    REQUIRE(baked_native != nullptr);
    REQUIRE(baked_native->root().id() == "bench-root");
    REQUIRE(render_frame(baked_native->root()) > 0);
    baked_native->step(12);
    REQUIRE(baked_native->js_evaluation_count() == 0);
}

TEST_CASE("design-import benchmark fixture factory creates the rebuild control lane",
          "[design-import][benchmark]") {
    auto rebuild = make_fixture(lane_config("baked-native-rebuild"));
    REQUIRE(rebuild != nullptr);
    REQUIRE(rebuild->root().id() == "bench-root");
    REQUIRE(render_frame(rebuild->root()) > 0);
    rebuild->step(12);
    REQUIRE(rebuild->js_evaluation_count() == 0);
}

TEST_CASE("design-import benchmark run_phase records idle and interactive contracts",
          "[design-import][benchmark]") {
    CountingFixture fixture;

    const auto zero = run_phase(fixture, 0, std::nullopt, 60, true);
    REQUIRE(zero.duration_ms == 0);
    REQUIRE(zero.samples == 0);
    REQUIRE(zero.cpu_ms == 0.0);
    REQUIRE(zero.paint_commands_last == 0);
    REQUIRE(fixture.steps == 0);

    const auto idle = run_phase(fixture, 2, std::nullopt, 10000, false);
    REQUIRE(idle.duration_ms == 2);
    REQUIRE(idle.samples > 0);
    REQUIRE(idle.js_evaluations == 10);
    REQUIRE(fixture.steps == 0);

    const auto interactive = run_phase(fixture, 2, std::nullopt, 10000, true);
    REQUIRE(interactive.duration_ms == 2);
    REQUIRE(interactive.samples > 0);
    REQUIRE(fixture.steps == interactive.samples);
    REQUIRE(fixture.last_frame == interactive.samples - 1);
}

TEST_CASE("design-import benchmark main writes reports and rejects invalid args",
          "[design-import][benchmark]") {
    auto output = temp_path("main-report");
    std::filesystem::remove(output);

    REQUIRE(run_benchmark_main({"bench", "--lane", "baked-cpp",
                                "--idle-ms", "0",
                                "--interactive-ms", "0",
                                "--target-fps", "30",
                                "--output", output.string()}) == 0);
    REQUIRE(std::filesystem::is_regular_file(output));
    auto report = read_text(output);
    REQUIRE(report.find("\"schema\": \"pulp-design-import-benchmark-v2\"") != std::string::npos);
    REQUIRE(report.find("\"lane\": \"baked-cpp\"") != std::string::npos);
    REQUIRE(report.find("\"samples\": 0") != std::string::npos);

    REQUIRE(run_benchmark_main({"bench", "--lane", "invalid"}) == 2);

    std::filesystem::remove(output);
}

TEST_CASE("design-import benchmark argument parser accepts and clamps warmup frames",
          "[design-import][benchmark]") {
    auto equals = parse_args_vec({"bench", "--warmup-frames=12"});
    REQUIRE(equals.has_value());
    CHECK(equals->warmup_frames == 12);

    auto split = parse_args_vec({"bench", "--warmup-frames", "7"});
    REQUIRE(split.has_value());
    CHECK(split->warmup_frames == 7);

    auto negative = parse_args_vec({"bench", "--warmup-frames=-3"});
    REQUIRE(negative.has_value());
    CHECK(negative->warmup_frames == 0);

    CHECK_FALSE(parse_args_vec({"bench", "--warmup-frames=many"}).has_value());
    CHECK(parse_args_vec({"bench"})->warmup_frames == 0);
}

TEST_CASE("design-import benchmark JSON report carries tail, allocation, and RSS fields",
          "[design-import][benchmark]") {
    Config config;
    config.lane = "baked-native";

    PhaseMetrics interactive;
    interactive.warmup_frames = 60;
    interactive.frame_ms_p95 = 1.25;
    interactive.update_ms_p95 = 0.5;
    interactive.allocations_total = 2690;
    interactive.allocated_bytes_total = 65536;
    interactive.allocations_per_frame_median = 269.0;
    interactive.allocations_per_frame_p95 = 271.0;

    RunContext run;
    run.rss_high_water = {22511616, "getrusage.ru_maxrss", {}};
    const auto json = make_json(config, std::nullopt, StartupMetrics{}, PhaseMetrics{}, interactive,
                                TeardownMetrics{}, run);
    CHECK(json.find("\"rss_high_water\": {\"value\": 22511616") != std::string::npos);
    CHECK(json.find("\"warmup_frames\": 60") != std::string::npos);
    CHECK(json.find("\"frame_ms_p95\": 1.2500") != std::string::npos);
    CHECK(json.find("\"update_ms_p95\": 0.5000") != std::string::npos);
    CHECK(json.find("\"allocations_total\": 2690") != std::string::npos);
    CHECK(json.find("\"allocated_bytes_total\": 65536") != std::string::npos);
    CHECK(json.find("\"allocations_per_frame_median\": 269.0000") != std::string::npos);
    CHECK(json.find("\"allocations_per_frame_p95\": 271.0000") != std::string::npos);
}

TEST_CASE("design-import benchmark run_phase excludes warmup frames from samples",
          "[design-import][benchmark]") {
    CountingFixture fixture;
    const auto interactive = run_phase(fixture, 5, std::nullopt, 10000, true, 3);
    REQUIRE(fixture.steps > 3);
    CHECK(interactive.warmup_frames == 3);
    CHECK(interactive.samples == fixture.steps - 3);
}

TEST_CASE("design-import benchmark run_phase counts per-frame allocations",
          "[design-import][benchmark]") {
    AllocatingFixture quiet(0);
    AllocatingFixture busy(8);
    const auto quiet_metrics = run_phase(quiet, 5, std::nullopt, 10000, true);
    const auto busy_metrics = run_phase(busy, 5, std::nullopt, 10000, true);
    REQUIRE(quiet_metrics.samples > 1);
    REQUIRE(busy_metrics.samples > 1);
    // The empty root's render allocates the same amount in both runs, so the
    // per-frame difference is exactly the fixture's own allocations.
    CHECK(busy_metrics.allocations_per_frame_median - quiet_metrics.allocations_per_frame_median ==
          Catch::Approx(8.0));
    CHECK(busy_metrics.allocations_total >= 8u * static_cast<std::uint64_t>(busy_metrics.samples));
}

TEST_CASE("design-import benchmark excludes warmup frames from cumulative counters",
          "[design-import][benchmark]") {
    auto fixture = make_fixture(lane_config("baked-cpp"));
    REQUIRE(fixture != nullptr);
    // Per-frame cost of the idle panel, measured on one frame: its paint
    // commands and how many layout passes one render asks for.
    const auto layouts_before = View::layout_pass_count();
    const auto commands = render_frame(fixture->root());
    const auto layouts_per_frame = View::layout_pass_count() - layouts_before;
    REQUIRE(commands > 0);
    REQUIRE(layouts_per_frame > 0);

    constexpr int kWarmup = 3;
    const auto idle = run_phase(*fixture, 5, std::nullopt, 10000, false, kWarmup);
    REQUIRE(idle.samples > 0);
    const auto sampled = static_cast<std::uint64_t>(idle.samples);
    // Every frame repeats the same work, so a counter that included the
    // warmup frames would read kWarmup frames high.
    CHECK(idle.paint_calls == sampled);
    CHECK(idle.paint_commands_total == sampled * commands);
    CHECK(idle.layout_passes == sampled * layouts_per_frame);

    // Control: without warmup every rendered frame is counted.
    const auto all = run_phase(*fixture, 5, std::nullopt, 10000, false, 0);
    REQUIRE(all.samples > 0);
    CHECK(all.paint_calls == static_cast<std::uint64_t>(all.samples));
    CHECK(all.layout_passes == static_cast<std::uint64_t>(all.samples) * layouts_per_frame);

    // A phase that never leaves warmup counts nothing.
    const auto warmup_only = run_phase(*fixture, 1, std::nullopt, 10000, false, 1000000);
    CHECK(warmup_only.samples == 0);
    CHECK(warmup_only.paint_calls == 0);
    CHECK(warmup_only.paint_commands_total == 0);
    CHECK(warmup_only.layout_passes == 0);
}

TEST_CASE("design-import benchmark reports a nonzero RSS high-water mark",
          "[design-import][benchmark]") {
#if defined(__APPLE__) || defined(__linux__)
    // The high-water mark is a peak, so it can never sit below the resident
    // set measured before it. A units slip (ru_maxrss is bytes on Apple and
    // KiB on Linux) reads three orders of magnitude off and fails here.
    const auto current = rss_reading();
    REQUIRE(current.value.has_value());
    REQUIRE(*current.value > 0);
    const auto high_water = rss_high_water_reading();
    REQUIRE(high_water.value.has_value());
    CHECK(*high_water.value >= *current.value);
#else
    SKIP("RSS high-water mark is not implemented on this platform");
#endif
}

TEST_CASE("design-import benchmark rebuild lane preserves the meter peak",
          "[design-import][benchmark]") {
    constexpr int frame = 30;
    const float t = static_cast<float>(frame) / 60.0f;
    const float output = 0.55f + 0.35f * std::sin(t * 1.3f);
    // The rebuild lane carries the peak as a decimal IR attribute.
    const float expected_peak =
        std::strtof(std::to_string(std::max(0.0f, output - 0.18f)).c_str(), nullptr);

    auto rebuild = make_fixture(lane_config("baked-native-rebuild"));
    REQUIRE(rebuild != nullptr);
    rebuild->step(frame);
    auto* meter = dynamic_cast<Meter*>(find_by_id(rebuild->root(), "output"));
    REQUIRE(meter != nullptr);
    CHECK(meter->display_rms() == Catch::Approx(output));
    CHECK(meter->display_peak() == Catch::Approx(expected_peak).margin(1e-6));
    // Without the peak attribute the materializer falls back to the level.
    CHECK(meter->display_peak() != Catch::Approx(meter->display_rms()));

    auto baked = make_fixture(lane_config("baked-native"));
    REQUIRE(baked != nullptr);
    baked->step(frame);
    auto* baked_meter = dynamic_cast<Meter*>(find_by_id(baked->root(), "output"));
    REQUIRE(baked_meter != nullptr);
    CHECK(baked_meter->display_peak() == Catch::Approx(meter->display_peak()).margin(1e-6));
}

namespace {
// A two-slot importer-shaped DesignIR written to a temp file, so the IR-bound
// lanes run against real serialized bytes rather than an in-memory tree.
std::filesystem::path write_slot_ir(const std::string& name) {
    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "root";
    ir.root.style.width = 200.0f;
    ir.root.style.height = 80.0f;
    for (const char* anchor : {"title", "subtitle"}) {
        IRNode text;
        text.type = "text";
        text.stable_anchor_id = anchor;
        text.text_content = anchor;
        text.style.font_size = 14.0f;
        ir.root.children.push_back(text);
    }
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream(path) << serialize_design_ir(ir);
    return path;
}
} // namespace

TEST_CASE("design-import benchmark IR-bound retained lane keeps keyed slots",
          "[design-import][benchmark][retained]") {
    const auto path = write_slot_ir("pulp-bench-retained.ir.json");
    Config config = lane_config("baked-native");
    config.ir_path = path;
    auto fixture = make_fixture(config);
    REQUIRE(fixture != nullptr);
    const auto phase = run_phase(*fixture, 0, 5, 60, true);
    CHECK(phase.samples == 5);
    CHECK(phase.views_constructed == 0);
    CHECK(phase.views_destroyed == 0);
    CHECK(phase.slot_identity_changes == 0);
    CHECK(phase.slot_text_mismatches == 0);
    CHECK(phase.unmarked_paint_mutations == 0);
    // Every keyed write marks its own damage.
    CHECK(phase.invalidation_requests >= 2 * 5);
    std::filesystem::remove(path);
}

TEST_CASE("design-import benchmark IR-bound rebuild lane is the identity control",
          "[design-import][benchmark][retained]") {
    const auto path = write_slot_ir("pulp-bench-rebuild.ir.json");
    Config config = lane_config("baked-native-rebuild");
    config.ir_path = path;
    auto fixture = make_fixture(config);
    REQUIRE(fixture != nullptr);
    const auto phase = run_phase(*fixture, 0, 5, 60, true);
    // Rematerializing rebinds every slot on every step, which is exactly what
    // the identity counter has to be able to see.
    CHECK(phase.slot_identity_changes == 2 * 5);
    CHECK(phase.slot_text_mismatches == 0);
    CHECK(phase.views_constructed > 0);
    CHECK(phase.views_constructed == phase.views_destroyed);
    std::filesystem::remove(path);
}

TEST_CASE("design-import benchmark IR binding and teardown fail closed",
          "[design-import][benchmark][retained]") {
    const auto path = write_slot_ir("pulp-bench-binding.ir.json");
    // The lane that cannot be built from a DesignIR refuses one.
    CHECK_FALSE(parse_args_vec({"bench", "--lane=baked-cpp", "--ir=" + path.string()}).has_value());
    CHECK_FALSE(parse_args_vec({"bench", "--lane=baked-native", "--frames=-1"}).has_value());

    const auto output = std::filesystem::temp_directory_path() / "pulp-bench-binding.json";
    REQUIRE(run_benchmark_main({"bench", "--lane=baked-native", "--ir=" + path.string(),
                                "--frames=3", "--output=" + output.string()}) == 0);
    std::ifstream in(output);
    const std::string json{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    CHECK(json.find("\"pacing\": \"fixed-frames\"") != std::string::npos);
    CHECK(json.find("\"text_slots\": 2") != std::string::npos);
    CHECK(json.find("\"live_views_after_teardown\": 0") != std::string::npos);
    // A design with no widget palette materializes with a warning, never
    // silently: the receipt names it.
    CHECK(json.find("\"materialization_warnings\": [\"design-token-unmapped\"]") !=
          std::string::npos);

    // Planted negative: a view leaked past teardown fails the run.
    REQUIRE(run_benchmark_main({"bench", "--lane=baked-native", "--ir=" + path.string(),
                                "--frames=1", "--plant-leak", "--output=" + output.string()}) == 3);
    // A plant that cannot be placed fails rather than reporting a clean run.
    DesignIR lone;
    lone.root.type = "text";
    lone.root.stable_anchor_id = "only";
    lone.root.text_content = "only";
    const auto lone_path = std::filesystem::temp_directory_path() / "pulp-bench-lone.ir.json";
    std::ofstream(lone_path) << serialize_design_ir(lone);
    CHECK(run_benchmark_main({"bench", "--lane=baked-native", "--ir=" + lone_path.string(),
                              "--frames=1", "--plant-leak", "--output=" + output.string()}) == 2);
    std::filesystem::remove(lone_path);
    std::filesystem::remove(output);
    std::filesystem::remove(path);
}

TEST_CASE("design-import benchmark retained lane sees a rebound slot",
          "[design-import][benchmark][retained]") {
    // Planted negative: swap one slot's Label for a new View under the same
    // anchor. A check that only re-read the Label it resolved at build would
    // keep reporting zero.
    const auto path = write_slot_ir("pulp-bench-rebind.ir.json");
    Config config = lane_config("baked-native");
    config.ir_path = path;
    auto fixture = make_fixture(config);
    auto* bound = dynamic_cast<IrBoundFixture*>(fixture.get());
    REQUIRE(bound != nullptr);
    View* original = nullptr;
    const auto find = [&](auto& self, View& view) -> void {
        if (view.anchor_id() == "title")
            original = &view;
        for (std::size_t i = 0; i < view.child_count(); ++i)
            self(self, *view.child_at(i));
    };
    find(find, bound->tree());
    REQUIRE(original != nullptr);
    View* parent = original->parent();
    REQUIRE(parent != nullptr);
    // Hold the original so the retained lane's write through it stays valid.
    auto held = parent->remove_child(original);
    auto replacement = std::make_unique<Label>("title");
    replacement->set_anchor_id("title");
    parent->add_child(std::move(replacement));

    const auto phase = run_phase(*fixture, 0, 1, 60, true);
    CHECK(phase.slot_identity_changes == 1);
    CHECK(phase.slot_text_mismatches == 1);
    std::filesystem::remove(path);
}

namespace {

// Sleeps a fixed time in every update and does nothing else, so a lap clock
// that attributes it anywhere but the update stage is visibly wrong.
class SleepingFixture final : public BenchmarkFixture {
  public:
    explicit SleepingFixture(std::chrono::microseconds sleep) : sleep_(sleep) {
        root_.set_id("sleeping-root");
        root_.set_bounds({0, 0, 12, 12});
    }
    View& root() override {
        return root_;
    }
    void step(int) override {
        std::this_thread::sleep_for(sleep_);
    }
    int js_evaluation_count() const override {
        return 0;
    }

  private:
    View root_;
    std::chrono::microseconds sleep_;
};

// The number after `"key": ` at its first occurrence after `from`.
std::optional<double> json_number_after(const std::string& json, const std::string& key,
                                        std::size_t from = 0) {
    const auto at = json.find("\"" + key + "\": ", from);
    if (at == std::string::npos)
        return std::nullopt;
    const auto start = at + key.size() + 4;
    if (json.compare(start, 4, "null") == 0)
        return std::nullopt;
    try {
        return std::stod(json.substr(start, 32));
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

TEST_CASE("design-import benchmark lap clock attributes a planted update sleep to update only",
          "[design-import][benchmark]") {
    constexpr double kSleepMs = 3.0;
    SleepingFixture fixture(std::chrono::microseconds(static_cast<int>(kSleepMs * 1000)));
    const auto phase = run_phase(fixture, 0, 20, 60, true);
    REQUIRE(phase.samples == 20);
    CHECK(phase.stages.update.median_ms >= kSleepMs);
    // An empty root lays out and records in microseconds; the sleep must not
    // leak into either stage.
    CHECK(phase.stages.layout.median_ms < kSleepMs / 3.0);
    CHECK(phase.stages.record.median_ms < kSleepMs / 3.0);
    CHECK(phase.stages.submit.median_ms < kSleepMs / 3.0);
    // The laps cover the frame: what they leave out is the instrument itself.
    CHECK(phase.stages_consistent);
    CHECK(phase.frame_ms_median >= phase.stages.update.median_ms);

    // Control: without the sleep the update stage is near zero, so the
    // assertion above measured the plant, not a slow host.
    SleepingFixture quiet(std::chrono::microseconds(0));
    const auto control = run_phase(quiet, 0, 8, 60, true);
    CHECK(control.stages.update.median_ms < kSleepMs / 3.0);

    // Idle frames run no update at all.
    const auto idle = run_phase(fixture, 0, 4, 60, false);
    CHECK(idle.stages.update.median_ms < kSleepMs / 3.0);
}

TEST_CASE("design-import benchmark memory readings are a value or an explained null",
          "[design-import][benchmark]") {
#if defined(__APPLE__) || defined(__linux__)
    const auto rss = rss_reading();
    REQUIRE(rss.value.has_value());
    CHECK(*rss.value > 0);
    CHECK_FALSE(rss.source.empty());
    CHECK(rss.reason.empty());
#endif
    const auto footprint = phys_footprint_reading();
#if defined(__APPLE__)
    REQUIRE(footprint.value.has_value());
    CHECK(*footprint.value > 0);
#else
    CHECK_FALSE(footprint.value.has_value());
    CHECK_FALSE(footprint.reason.empty());
#endif

    // One absent sample makes the whole summary absent with its reason; a
    // median over the present samples would quietly report a smaller set.
    MemorySeries series;
    series.add({100, "probe", {}});
    series.add({std::nullopt, "probe", "probe failed"});
    series.add({300, "probe", {}});
    const auto summary = series.summary();
    CHECK_FALSE(summary.median_bytes.has_value());
    CHECK_FALSE(summary.peak_bytes.has_value());
    CHECK(summary.reason == "probe failed");

    MemorySeries present;
    present.add({100, "probe", {}});
    present.add({300, "probe", {}});
    const auto ok = present.summary();
    REQUIRE(ok.median_bytes.has_value());
    CHECK(*ok.median_bytes == 200);
    CHECK(*ok.peak_bytes == 300);
    CHECK(ok.source == "probe");
    CHECK_FALSE(MemorySeries{}.summary().median_bytes.has_value());
}

TEST_CASE("design-import benchmark classifies load and thermal confounders",
          "[design-import][benchmark]") {
    HostSample quiet;
    quiet.load_1m = 1.0;
    quiet.thermal = "nominal";
    const auto clean = classify_host({quiet, quiet}, 16);
    CHECK(clean.load_threshold == Catch::Approx(4.0));
    CHECK_FALSE(clean.confounded);
    CHECK(clean.confounders.empty());

    HostSample loaded = quiet;
    loaded.load_1m = 4.5;
    const auto busy = classify_host({quiet, loaded}, 16);
    CHECK(busy.confounded);
    REQUIRE(busy.confounders.size() == 1);
    CHECK(busy.confounders.front() == "load_1m 4.50 > cores/4 (4.00)");

    HostSample hot = quiet;
    hot.thermal = "fair";
    CHECK(classify_host({hot}, 16).confounders == std::vector<std::string>{"thermal state fair"});

    // An unreadable condition cannot be shown to be quiet, so it confounds.
    HostSample blind;
    blind.load_reason = "getloadavg failed";
    blind.thermal_reason = "no thermal-state source on this platform";
    const auto unknown = classify_host({blind}, 16);
    CHECK(unknown.confounded);
    CHECK(unknown.confounders.size() == 2);
    CHECK(classify_host({}, 16).confounded);

    // The planted load replaces the measured one and nothing else.
    const auto planted = sample_host(123.0);
    REQUIRE(planted.load_1m.has_value());
    CHECK(*planted.load_1m == 123.0);
}

TEST_CASE("design-import benchmark marks a planted contended run confounded",
          "[design-import][benchmark]") {
    const auto output = temp_path("confounded");
    std::filesystem::remove(output);
    REQUIRE(run_benchmark_main({"bench", "--lane=baked-cpp", "--frames=2", "--plant-load=100000",
                                "--output=" + output.string()}) == 0);
    const auto json = read_text(output);
    CHECK(json.find("\"planted_load\": 100000.0000") != std::string::npos);
    CHECK(json.find("\"confounded\": true") != std::string::npos);
    CHECK(json.find("\"load_1m 100000.00 > cores/4") != std::string::npos);
    // Control: a zero load leaves no load confounder (thermal state is the
    // host's own and may still confound).
    REQUIRE(run_benchmark_main({"bench", "--lane=baked-cpp", "--frames=2", "--plant-load=0",
                                "--output=" + output.string()}) == 0);
    CHECK(read_text(output).find("\"load_1m ") == std::string::npos);
    CHECK_FALSE(parse_args_vec({"bench", "--plant-load=-1"}).has_value());
    CHECK_FALSE(parse_args_vec({"bench", "--plant-load=busy"}).has_value());
    std::filesystem::remove(output);
}

TEST_CASE("design-import benchmark least-squares slope", "[design-import][benchmark]") {
    CHECK_FALSE(least_squares_slope({}).has_value());
    CHECK_FALSE(least_squares_slope({5.0}).has_value());
    CHECK(*least_squares_slope({1.0, 3.0, 5.0}) == Catch::Approx(2.0));
    CHECK(*least_squares_slope({7.0, 7.0, 7.0, 7.0}) == Catch::Approx(0.0));
    CHECK(*least_squares_slope({4.0, 2.0}) == Catch::Approx(-2.0));
}

TEST_CASE("design-import benchmark cycle slope sees a planted per-cycle leak",
          "[design-import][benchmark]") {
    CHECK_FALSE(parse_args_vec({"bench", "--cycles=2"}).has_value());
    CHECK(parse_args_vec({"bench", "--cycles=3"})->cycles == 3);

    const auto output = temp_path("cycles");
    const auto slope_of = [&](bool plant) {
        std::vector<std::string> args{"bench", "--lane=baked-cpp", "--frames=2", "--cycles=5",
                                      "--output=" + output.string()};
        if (plant)
            args.push_back("--plant-cycle-leak");
        REQUIRE(run_benchmark_main(args) == 0);
        const auto json = read_text(output);
        const auto block = json.find("\"cycles\": {");
        REQUIRE(block != std::string::npos);
        CHECK(json.find("\"count\": 5", block) != std::string::npos);
#if defined(__APPLE__)
        return json_number_after(json, "phys_footprint_slope_bytes_per_cycle", block);
#else
        return json_number_after(json, "rss_slope_bytes_per_cycle", block);
#endif
    };
#if defined(__APPLE__) || defined(__linux__)
    const auto leaking = slope_of(true);
    const auto clean = slope_of(false);
    REQUIRE(leaking.has_value());
    REQUIRE(clean.has_value());
    const double leak = static_cast<double>(kPlantedCycleLeakBytes);
    // The plant adds a touched 4 MiB per cycle; the clean run returns to its
    // footprint after every teardown.
    CHECK(*leaking > 0.75 * leak);
    CHECK(*leaking - *clean > 0.75 * leak);
#else
    SKIP("no memory probe on this platform");
#endif
    std::filesystem::remove(output);
}

TEST_CASE("design-import benchmark live lane writes IR slots through the bridge",
          "[design-import][benchmark][retained]") {
    const auto path = write_slot_ir("pulp-bench-live.ir.json");
    Config config = lane_config("live");
    config.ir_path = path;
    auto fixture = make_fixture(config);
    auto* live = dynamic_cast<IrLiveFixture*>(fixture.get());
    REQUIRE(live != nullptr);
    CHECK(live->bundle_sha256().empty());
    const auto phase = run_phase(*fixture, 0, 5, 60, true);
    CHECK(phase.samples == 5);
    CHECK(phase.slot_identity_changes == 0);
    CHECK(phase.slot_text_mismatches == 0);
    CHECK(phase.views_constructed == 0);
    CHECK(phase.js_evaluations == 5);
    // Two setText calls per frame at least: the writes go through the bridge.
    CHECK(phase.bridge_calls >= 2 * 5);
    // The layout stage, not the update, carries the frame's layout pass.
    CHECK(phase.layout_passes == 5);
    std::filesystem::remove(path);
}

TEST_CASE("design-import benchmark live lane mounts a materialized bundle and fails closed",
          "[design-import][benchmark][retained]") {
    const auto path = write_slot_ir("pulp-bench-live-bundle.ir.json");
    const auto bundle = std::filesystem::temp_directory_path() / "pulp-bench-bundle.js";
    // A behavior realm in the shape the materialized runtime mounts: it runs
    // before the DesignIR and owns its own hidden root.
    std::ofstream(bundle) << "createCol('bench-behavior-root', '');\n"
                             "globalThis.__benchBundleMounted = true;\n";
    const auto output = temp_path("live-bundle");
    REQUIRE(run_benchmark_main({"bench", "--lane=live", "--ir=" + path.string(),
                                "--bundle=" + bundle.string(), "--frames=3",
                                "--output=" + output.string()}) == 0);
    const auto json = read_text(output);
    CHECK(json.find("\"bundle\": {\"file\": \"pulp-bench-bundle.js\", \"sha256\": \"") !=
          std::string::npos);
    CHECK(json.find("\"slot_text_mismatches\": 0") != std::string::npos);
    CHECK(json.find("\"live_views_after_teardown\": 0") != std::string::npos);

    // A bundle that claims a slot's anchor for a second Label makes the slot
    // ambiguous; the lane refuses rather than writing to either.
    std::ofstream(bundle) << "createLabel('impostor', 'x', '');\n"
                             "setAnchor('impostor', 'title');\n";
    Config config = lane_config("live");
    config.ir_path = path;
    config.bundle_path = bundle;
    CHECK_THROWS_WITH(make_fixture(config),
                      Catch::Matchers::ContainsSubstring("resolved to 2 Labels"));
    // A bundle that throws fails the mount.
    std::ofstream(bundle) << "throw new Error('bundle failed');\n";
    CHECK_THROWS(make_fixture(config));

    CHECK_FALSE(
        parse_args_vec({"bench", "--lane=live", "--bundle=" + bundle.string()}).has_value());
    CHECK_FALSE(parse_args_vec({"bench", "--lane=baked-native", "--ir=" + path.string(),
                                "--bundle=" + bundle.string()})
                    .has_value());
    CHECK(parse_args_vec({"bench", "--lane=live", "--ir=" + path.string()}).has_value());
    std::filesystem::remove(bundle);
    std::filesystem::remove(output);
    std::filesystem::remove(path);
}

namespace {

// A root holding only the planted per-paint decode, for counting its work.
class PlantedDecodeFixture final : public BenchmarkFixture {
  public:
    PlantedDecodeFixture() {
        root_.set_id("planted-root");
        root_.set_bounds({0, 0, 16, 16});
        root_.add_child(std::make_unique<PlantedDecodeView>());
    }
    View& root() override {
        return root_;
    }
    void step(int) override {}
    int js_evaluation_count() const override {
        return 0;
    }

  private:
    View root_;
};

// A cached subtree: its first paint records a scene and later paints replay it.
class CachedSubtreeFixture final : public BenchmarkFixture {
  public:
    CachedSubtreeFixture() {
        root_.set_id("cached-root");
        root_.set_bounds({0, 0, 64, 32});
        auto label = std::make_unique<Label>("cached");
        configure_absolute(*label, "cached-label", 0, 0, 64, 32);
        auto panel = std::make_unique<View>();
        configure_absolute(*panel, "cached-panel", 0, 0, 64, 32);
        panel->add_child(std::move(label));
        panel->set_subtree_cached(true);
        root_.add_child(std::move(panel));
    }
    View& root() override {
        return root_;
    }
    void step(int) override {}
    int js_evaluation_count() const override {
        return 0;
    }

  private:
    View root_;
};

} // namespace

TEST_CASE("design-import benchmark Skia lane counts per-frame resource work",
          "[design-import][benchmark]") {
    std::string error;
    auto target = make_frame_target("skia-raster", 16, 16, error);
    if (!target)
        SKIP("skia-raster target unavailable: " + error);
    CHECK(target->observes_resources());
    PlantedDecodeFixture planted;
    PhaseOptions options;
    options.target = target.get();
    const auto phase = run_phase(planted, 0, 6, 60, true, 0, options);
    REQUIRE(phase.samples == 6);
    // One decode per paint, every frame, and the frames after the first are
    // what the steady-state gate reads.
    CHECK(phase.resources.image_decodes.total == 6);
    CHECK(phase.resources.image_decodes.per_frame_max == 1);
    CHECK(phase.resources.image_decodes.frames_after_first_with_work == 5);
    // A raster canvas never uploads.
    CHECK(phase.resources.gpu_uploads.total == 0);

    // The same fixture painted into a RecordingCanvas decodes nothing, which
    // is why that target is blind to the gate rather than passing it.
    const auto recorded = run_phase(planted, 0, 6, 60, true);
    CHECK(recorded.resources.image_decodes.total == 0);
    CHECK(steady_state_gate(recorded, recorded, false).status == "blind");
    CHECK(steady_state_gate(phase, phase, true).status == "fail");

    // Text shapes a paragraph on every Skia paint.
    auto bench = make_fixture(lane_config("baked-cpp"));
    const auto text = run_phase(*bench, 0, 3, 60, false, 0, options);
    CHECK(text.resources.paragraph_builds.per_frame_max > 0);
    CHECK(text.resources.paragraph_builds.frames_after_first_with_work == 2);
    CHECK(text.resources.image_decodes.total == 0);
}

TEST_CASE("design-import benchmark Skia lane counts scene-cache records and replays",
          "[design-import][benchmark]") {
    std::string error;
    auto target = make_frame_target("skia-raster", 64, 32, error);
    if (!target)
        SKIP("skia-raster target unavailable: " + error);
    CachedSubtreeFixture fixture;
    PhaseOptions options;
    options.target = target.get();
    const auto phase = run_phase(fixture, 0, 4, 60, false, 0, options);
    CHECK(phase.resources.scene_records.total == 1);
    CHECK(phase.resources.scene_replays.total == 4);
    // A replayed subtree does not repaint its Label, so it shapes no text
    // after the recording frame.
    CHECK(phase.resources.paragraph_builds.frames_after_first_with_work == 0);
}

TEST_CASE("design-import benchmark IR-bound lane draws manifest images away from their directory",
          "[design-import][benchmark]") {
    std::string error;
    auto target = make_frame_target("skia-raster", 32, 32, error);
    if (!target)
        SKIP("skia-raster target unavailable: " + error);
    // An importer writes asset paths relative to the DesignIR's own directory.
    const auto dir = std::filesystem::temp_directory_path() / "pulp-bench-ir-assets";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "assets");
    static constexpr std::uint8_t kPng[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
        0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78,
        0x9c, 0x63, 0x78, 0xd0, 0x66, 0xf3, 0x1f, 0x00, 0x06, 0x8e, 0x02, 0xa2, 0x86, 0xc6,
        0xaa, 0xc4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82};
    std::ofstream(dir / "assets" / "pixel.png", std::ios::binary)
        .write(reinterpret_cast<const char*>(kPng), sizeof(kPng));

    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "root";
    ir.root.style.width = 32.0f;
    ir.root.style.height = 32.0f;
    IRNode text;
    text.type = "text";
    text.stable_anchor_id = "caption";
    text.text_content = "caption";
    ir.root.children.push_back(text);
    IRNode image;
    image.type = "image";
    image.stable_anchor_id = "pixel";
    image.attributes["srcAssetId"] = "image:pixel";
    image.style.position = "absolute";
    image.style.left = 0.0f;
    image.style.top = 0.0f;
    image.style.width = 16.0f;
    image.style.height = 16.0f;
    ir.root.children.push_back(image);
    IRAssetRef asset;
    asset.asset_id = "image:pixel";
    asset.local_path = "assets/pixel.png";
    ir.asset_manifest.assets.push_back(asset);
    const auto path = dir / "design.ir.json";
    std::ofstream(path) << serialize_design_ir(ir);

    // Run from a directory that holds no assets/ folder, as the bench does
    // when it is not started beside the DesignIR.
    const auto previous = std::filesystem::current_path();
    std::filesystem::current_path(std::filesystem::temp_directory_path());
    Config config = lane_config("baked-native");
    config.ir_path = path;
    auto fixture = make_fixture(config);
    std::filesystem::current_path(previous);
    REQUIRE(fixture != nullptr);
    PhaseOptions options;
    options.target = target.get();
    const auto phase = run_phase(*fixture, 0, 3, 60, false, 0, options);
    // The image is decoded and cached on the first frame and drawn from the
    // cache after it; an unresolved path would read zero on every count.
    CHECK(phase.resources.image_cache_builds.total == 1);
    CHECK(phase.resources.image_cache_hits.total >= 2);
    CHECK(phase.resources.image_cache_builds.frames_after_first_with_work == 0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("design-import benchmark Graphite lane counts GPU uploads",
          "[design-import][benchmark][gpu]") {
    std::string error;
    auto target = make_frame_target("skia-graphite", 16, 16, error);
    if (!target)
        SKIP("skia-graphite target unavailable: " + error);
    PlantedDecodeFixture planted;
    PhaseOptions options;
    options.target = target.get();
    const auto phase = run_phase(planted, 0, 4, 60, true, 0, options);
    // Each uncached decode is uploaded before Graphite can draw it.
    CHECK(phase.resources.image_decodes.total == 4);
    CHECK(phase.resources.gpu_uploads.total == 4);
    CHECK(phase.resources.gpu_uploads.frames_after_first_with_work == 3);
}

TEST_CASE("design-import benchmark steady-state gate fails a planted per-frame decode",
          "[design-import][benchmark]") {
    std::string error;
    if (!make_frame_target("skia-raster", 4, 4, error))
        SKIP("skia-raster target unavailable: " + error);
    const auto output = temp_path("steady-state");
    // Planted negative: one view that decodes on every paint.
    CHECK(run_benchmark_main({"bench", "--lane=baked-native", "--canvas=skia-raster", "--frames=5",
                              "--plant-frame-decode", "--require-steady-state",
                              "--output=" + output.string()}) == 4);
    auto json = read_text(output);
    CHECK(json.find("\"canvas\": \"skia-raster\"") != std::string::npos);
    CHECK(json.find("\"status\": \"fail\"") != std::string::npos);
    CHECK(json.find("\"planted_frame_decode\": true") != std::string::npos);
    CHECK(json.find("idle: 4 frames after the first did image decodes") != std::string::npos);
    CHECK(json.find("interactive: 4 frames after the first did image decodes") !=
          std::string::npos);
    CHECK(json.find("pulp-design-import-benchmark-counters-v2") != std::string::npos);

    // Control: the same lane without the plant does no steady-state resource
    // work.
    CHECK(run_benchmark_main({"bench", "--lane=baked-native", "--canvas=skia-raster", "--frames=5",
                              "--require-steady-state", "--output=" + output.string()}) == 0);
    json = read_text(output);
    CHECK(json.find("\"status\": \"pass\"") != std::string::npos);
    // The Skia target counts no paint commands, so the receipt carries the
    // final tree's recorded count for cross-lane agreement.
    CHECK(json.find("\"paint_commands_last\": 0,") != std::string::npos);
    const auto recorded = json_number_after(json, "paint_commands_recorded");
    REQUIRE(recorded.has_value());
    CHECK(*recorded > 0);

    // A recording canvas cannot see the work, so it cannot satisfy the gate.
    CHECK(run_benchmark_main({"bench", "--lane=baked-native", "--frames=2",
                              "--require-steady-state", "--output=" + output.string()}) == 4);
    CHECK(read_text(output).find("\"status\": \"blind\"") != std::string::npos);
    CHECK_FALSE(parse_args_vec({"bench", "--canvas=metal"}).has_value());
    CHECK_FALSE(parse_args_vec({"bench", "--lane=baked-native-rebuild", "--plant-frame-decode"})
                    .has_value());
    std::filesystem::remove(output);
}
