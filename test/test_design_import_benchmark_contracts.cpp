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

    auto json = make_json(config, std::nullopt, startup, idle, interactive, TeardownMetrics{});
    REQUIRE(json.find("\"schema\": \"pulp-design-import-benchmark-v1\"") != std::string::npos);
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
    REQUIRE(report.find("\"schema\": \"pulp-design-import-benchmark-v1\"") != std::string::npos);
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

    const auto json = make_json(config, std::nullopt, StartupMetrics{}, PhaseMetrics{}, interactive,
                                TeardownMetrics{}, 22511616);
    CHECK(json.find("\"rss_high_water_bytes\": 22511616") != std::string::npos);
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
    const auto current = current_rss_bytes();
    REQUIRE(current > 0);
    CHECK(rss_high_water_bytes() >= current);
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
    // Lanes that cannot be built from a DesignIR refuse one.
    CHECK_FALSE(parse_args_vec({"bench", "--lane=live", "--ir=" + path.string()}).has_value());
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
